import Foundation
import Network
import Security
import AVFoundation

/// Shared by the macOS slicer and iOS companion. No cloud tokens or credentials in URLs/logs.
final class CameraStream {
    let onSample: (CMSampleBuffer) -> Void
    let onStatus: (String) -> Void
    private let host: String, serial: String, code: String
    private let port: NWEndpoint.Port
    private let verifier: (SecTrust, String) -> Bool
    private let queue = DispatchQueue(label: "3DPrint.camera")
    private var connection: NWConnection?, timer: DispatchSourceTimer?
    private var generation = 0, retries = 0, cseq = 0
    private var wire = CameraWire(), rtp = CameraRTP(), digest: CameraDigest?
    private var pending: (method: String, uri: String, headers: String, retried: Bool)?
    private var session = "", track = "", base = ""
    private var lastVideo = Date(), lastControl = Date(), requestTime = Date()
    private var playing = false, firstFrame = false, waitingForIDR = true
    private var sps = Data(), pps = Data(), format: CMVideoFormatDescription?

    init(host: String, serial: String, code: String, onSample: @escaping (CMSampleBuffer) -> Void, onStatus: @escaping (String) -> Void, port: NWEndpoint.Port = 322, verifier: @escaping (SecTrust, String) -> Bool = CameraTrust.verify) {
        self.host = host; self.serial = serial; self.code = code; self.port = port; self.verifier = verifier
        self.onSample = onSample; self.onStatus = onStatus
    }
    func start() { queue.async { self.retries = 0; self.connect() } }
    func stop() { queue.async { self.generation += 1; self.close(); self.onStatus("Stopped") } }
    private func close() { timer?.cancel(); timer = nil; connection?.cancel(); connection = nil }
    private func connect() {
        close(); generation += 1
        let g = generation
        guard host.range(of: #"^[A-Za-z0-9._-]+$"#, options: .regularExpression) != nil,
              serial.range(of: #"^[A-Za-z0-9]{6,40}$"#, options: .regularExpression) != nil, !code.isEmpty else {
            onStatus("Set the printer address, serial number and LAN access code."); return
        }
        wire = CameraWire(); rtp = CameraRTP(); digest = nil; session = ""; pending = nil
        sps = Data(); pps = Data(); format = nil; waitingForIDR = true
        playing = false; firstFrame = false; cseq = 0
        base = "rtsps://\(host):322/streaming/live/1"
        lastVideo = Date(); lastControl = Date(); requestTime = Date()
        onStatus("Connecting to secure camera…")
        let tls = NWProtocolTLS.Options()
        sec_protocol_options_set_min_tls_protocol_version(tls.securityProtocolOptions, .TLSv12)
        let identity = serial, verify = verifier
        sec_protocol_options_set_verify_block(tls.securityProtocolOptions, { [weak self] _, ref, complete in
            let valid = verify(sec_trust_copy_ref(ref).takeRetainedValue(), identity)
            complete(valid)
            if !valid, let self, self.generation == g {
                self.fail("Camera certificate verification failed. Check the serial number and printer firmware.", retry: false)
            }
        }, queue)
        let c = NWConnection(host: NWEndpoint.Host(host), port: port, using: NWParameters(tls: tls))
        connection = c
        c.stateUpdateHandler = { [weak self] state in
            guard let self, g == self.generation else { return }
            switch state {
            case .ready: self.request("DESCRIBE", uri: self.base, headers: "Accept: application/sdp\r\n"); self.receive(g)
            case .failed(let e):
                if case .tls = e { self.fail("Camera certificate verification failed. Check the serial number and printer firmware.", retry: false) }
                else { self.fail("Camera unavailable on port 322. Enable LAN Mode Liveview on the printer and check the network.") }
            case .waiting: self.fail("Camera network unavailable. Check Wi-Fi or your private VPN.")
            default: break
            }
        }
        c.start(queue: queue)
        let t = DispatchSource.makeTimerSource(queue: queue)
        t.schedule(deadline: .now() + 1, repeating: 1)
        t.setEventHandler { [weak self] in
            guard let self, g == self.generation else { return }
            if Date().timeIntervalSince(self.playing ? self.lastVideo : self.requestTime) > 15 {
                self.fail(self.playing ? "Camera stopped delivering video." : "Camera connection timed out. Check LAN Mode Liveview."); return
            }
            if self.pending != nil && Date().timeIntervalSince(self.requestTime) > 10 { self.fail("Camera control response timed out."); return }
            if self.playing && Date().timeIntervalSince(self.lastControl) >= 5 && self.pending == nil {
                self.request("GET_PARAMETER", uri: self.base)
                // Empty RTCP receiver report, with a stable session-local SSRC.
                self.send(Data([36, 1, 0, 8, 128, 201, 0, 1, 0x43, 0x41, 0x4d, 1]))
                self.lastControl = Date()
            }
        }
        timer = t; t.resume()
    }
    private func fail(_ text: String, retry: Bool = true) {
        generation += 1; let g = generation; close()
        guard retry && retries < 3 else { onStatus(text + " Tap Retry when ready."); return }
        retries += 1
        onStatus(text + " Reconnecting (\(retries)/3)…")
        queue.asyncAfter(deadline: .now() + Double(1 << retries)) { [weak self] in
            guard let self, self.generation == g else { return }; self.connect()
        }
    }
    private func send(_ data: Data) {
        let g = generation
        connection?.send(content: data, completion: .contentProcessed { [weak self] error in
            guard let self, self.generation == g, error != nil else { return }; self.fail("Camera connection interrupted.")
        })
    }
    private func request(_ method: String, uri: String, headers: String = "", retried: Bool = false) {
        cseq += 1; pending = (method, uri, headers, retried); requestTime = Date()
        var text = "\(method) \(uri) RTSP/1.0\r\nCSeq: \(cseq)\r\nUser-Agent: 3DPrintCamera/1\r\n"
        if var auth = digest { text += "Authorization: \(auth.header(method: method, uri: uri, code: code))\r\n"; digest = auth }
        if !session.isEmpty { text += "Session: \(session)\r\n" }
        send(Data((text + headers + "\r\n").utf8))
    }
    private func receive(_ g: Int) {
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, error in
            guard let self, self.generation == g else { return }
            do {
                if let data {
                    for message in try self.wire.append(data) {
                        switch message {
                        case .response(let code, let fields, let body): try self.response(code, fields, body)
                        case .packet(let channel, let payload):
                            if channel == 0, self.playing, let nals = try self.rtp.accept(payload) { try self.frame(nals) }
                        }
                        if self.generation != g { return }
                    }
                }
                if done || error != nil { self.fail("Camera stream disconnected.") } else { self.receive(g) }
            } catch { self.fail((error as? CameraFailure)?.message ?? "Camera stream could not be decoded.", retry: (error as? CameraFailure)?.message == "Camera lost a video packet.") }
        }
    }
    private func response(_ status: Int, _ fields: [String: String], _ body: Data) throws {
        guard let p = pending, fields["cseq"].flatMap(Int.init) == cseq else { throw CameraFailure("Camera response sequence mismatch.") }
        pending = nil
        if status == 401 {
            guard !p.retried, let challenge = fields["www-authenticate"] else { throw CameraFailure("The camera rejected the LAN access code. Pair the printer again.") }
            digest = try CameraDigest(challenge: challenge)
            request(p.method, uri: p.uri, headers: p.headers, retried: true); return
        }
        guard status == 200 else { throw CameraFailure("Camera rejected \(p.method) (RTSP \(status)).") }
        switch p.method {
        case "DESCRIBE":
            let lines = String(decoding: body, as: UTF8.self).components(separatedBy: .newlines).map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            var video = false, h264 = false, control = ""
            for l in lines {
                if l.hasPrefix("m=") { if video { break }; video = l.hasPrefix("m=video ") }
                guard video else { continue }
                if l.hasPrefix("a=rtpmap:"), l.uppercased().contains("H264/90000") { h264 = true }
                if l.hasPrefix("a=control:") { control = String(l.dropFirst(10)) }
                if let r = l.range(of: "sprop-parameter-sets=") {
                    let sets = l[r.upperBound...].split(separator: ";")[0].split(separator: ",")
                    if sets.count >= 2 { sps = Data(base64Encoded: String(sets[0])) ?? Data(); pps = Data(base64Encoded: String(sets[1])) ?? Data() }
                }
            }
            guard h264, !control.isEmpty, control != "*" else { throw CameraFailure("Camera did not offer a supported H.264 video track.") }
            // Never send credentials to a host supplied by an SDP redirect.
            let contentBase = fields["content-base"] ?? base + "/"
            guard let u = URL(string: control, relativeTo: URL(string: contentBase))?.absoluteURL,
                  u.host == host, ["rtsp", "rtsps"].contains(u.scheme ?? ""), u.user == nil, u.password == nil,
                  !u.absoluteString.contains("\r"), !u.absoluteString.contains("\n") else { throw CameraFailure("Camera advertised an unsafe track address.") }
            track = u.absoluteString
            request("SETUP", uri: track, headers: "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
        case "SETUP":
            guard let value = fields["session"]?.split(separator: ";").first, !value.isEmpty,
                  fields["transport"]?.lowercased().contains("interleaved=0-1") == true else { throw CameraFailure("Camera did not accept encrypted interleaved video.") }
            session = String(value); request("PLAY", uri: base, headers: "Range: npt=0.000-\r\n")
        case "PLAY": playing = true; lastVideo = Date(); lastControl = Date(); onStatus("Waiting for camera video…")
        default: break
        }
    }
    private func frame(_ nals: [Data]) throws {
        var slices = [Data](), idr = false
        for nal in nals {
            guard let first = nal.first else { continue }
            switch first & 31 {
            case 7: if sps != nal { sps = nal; format = nil; waitingForIDR = true }
            case 8: if pps != nal { pps = nal; format = nil; waitingForIDR = true }
            case 5: idr = true; slices.append(nal)
            case 1: slices.append(nal)
            default: break
            }
        }
        guard !sps.isEmpty, !pps.isEmpty, !slices.isEmpty else { return }
        if waitingForIDR && !idr { return }; waitingForIDR = false
        if format == nil {
            let result = sps.withUnsafeBytes { a in pps.withUnsafeBytes { b -> OSStatus in
                let ptrs = [a.bindMemory(to: UInt8.self).baseAddress!, b.bindMemory(to: UInt8.self).baseAddress!]
                let sizes = [sps.count, pps.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(allocator: kCFAllocatorDefault, parameterSetCount: 2, parameterSetPointers: ptrs, parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &format)
            } }
            guard result == noErr else { throw CameraFailure("Camera H.264 configuration is invalid.") }
        }
        var avcc = Data()
        for nal in slices { var length = UInt32(nal.count).bigEndian; withUnsafeBytes(of: &length) { avcc.append(contentsOf: $0) }; avcc.append(nal) }
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count, blockAllocator: kCFAllocatorDefault, customBlockSource: nil, offsetToData: 0, dataLength: avcc.count, flags: 0, blockBufferOut: &block) == noErr, let block else { throw CameraFailure("Camera frame allocation failed.") }
        let copied = avcc.withUnsafeBytes { CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: avcc.count) }
        var sample: CMSampleBuffer?, size = avcc.count
        var timing = CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: CMClockGetTime(CMClockGetHostTimeClock()), decodeTimeStamp: .invalid)
        guard copied == noErr, CMSampleBufferCreateReady(allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format!, sampleCount: 1, sampleTimingEntryCount: 1, sampleTimingArray: &timing, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample) == noErr, let sample else { throw CameraFailure("Camera frame creation failed.") }
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: true), CFArrayGetCount(attachments) > 0 {
            let dictionary = unsafeBitCast(CFArrayGetValueAtIndex(attachments, 0), to: CFMutableDictionary.self)
            CFDictionarySetValue(dictionary, Unmanaged.passUnretained(kCMSampleAttachmentKey_DisplayImmediately).toOpaque(), Unmanaged.passUnretained(kCFBooleanTrue).toOpaque())
        }
        lastVideo = Date()
        if !firstFrame { firstFrame = true; onStatus("Live") }
        onSample(sample)
    }
}
