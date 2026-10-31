import Foundation
import CryptoKit

struct CameraFailure: LocalizedError {
    let message: String
    var errorDescription: String? { message }
    init(_ message: String) { self.message = message }
}

/// Bounded incremental demultiplexer: TLS reads need not align with RTSP or RTP packets.
struct CameraWire {
    enum Message { case response(Int, [String: String], Data); case packet(UInt8, Data) }
    var bytes = Data()
    mutating func append(_ data: Data) throws -> [Message] {
        bytes.append(data)
        guard bytes.count <= 2_097_152 else { throw CameraFailure("Camera response exceeds the size limit.") }
        var result = [Message]()
        while !bytes.isEmpty {
            if bytes.first == 36 {
                guard bytes.count >= 4 else { break }
                let b = Array(bytes.prefix(4)), count = Int(b[2]) * 256 + Int(b[3])
                guard bytes.count >= 4 + count else { break }
                result.append(.packet(b[1], Data(bytes.dropFirst(4).prefix(count))))
                bytes = Data(bytes.dropFirst(4 + count))
            } else {
                guard let range = bytes.range(of: Data("\r\n\r\n".utf8)) else {
                    guard bytes.count < 65536 else { throw CameraFailure("Invalid camera response header.") }; break
                }
                let header = String(decoding: bytes[..<range.lowerBound], as: UTF8.self).components(separatedBy: "\r\n")
                let status = header[0].split(separator: " ")
                guard status.count >= 2, status[0] == "RTSP/1.0", let code = Int(status[1]) else { throw CameraFailure("Invalid camera response.") }
                var fields = [String: String]()
                for line in header.dropFirst() {
                    if let colon = line.firstIndex(of: ":") {
                        fields[String(line[..<colon]).lowercased()] = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
                    }
                }
                guard fields["content-length"] == nil || Int(fields["content-length"]!) != nil else { throw CameraFailure("Invalid camera response size.") }
                let length = fields["content-length"].flatMap(Int.init) ?? 0
                guard length >= 0, length <= 1_048_576 else { throw CameraFailure("Invalid camera response size.") }
                let end = range.upperBound + length
                guard bytes.count >= end else { break }
                result.append(.response(code, fields, Data(bytes[range.upperBound..<end])))
                bytes = Data(bytes.dropFirst(end))
            }
        }
        return result
    }
}

struct CameraDigest {
    var realm: String, nonce: String, qop: Bool, opaque: String?
    var count = 0
    init(challenge: String) throws {
        guard challenge.lowercased().hasPrefix("digest ") else { throw CameraFailure("Unsupported camera authentication method.") }
        let pattern = #"([A-Za-z]+)\s*=\s*(?:"([^"]*)"|([^,\s]+))"#
        let regex = try NSRegularExpression(pattern: pattern)
        let ns = challenge as NSString
        var f = [String: String]()
        for m in regex.matches(in: challenge, range: NSRange(location: 0, length: ns.length)) {
            f[ns.substring(with: m.range(at: 1)).lowercased()] = ns.substring(with: m.range(at: m.range(at: 2).location == NSNotFound ? 3 : 2))
        }
        guard let r = f["realm"], let n = f["nonce"], !n.isEmpty,
              f["algorithm"] == nil || f["algorithm"]?.uppercased() == "MD5",
              f["qop"] == nil || f["qop"]!.split(separator: ",").contains(where: { $0.trimmingCharacters(in: .whitespaces) == "auth" }),
              !r.contains("\r"), !r.contains("\n"), !n.contains("\r"), !n.contains("\n") else { throw CameraFailure("Unsupported camera authentication challenge.") }
        realm = r; nonce = n; qop = f["qop"] != nil; opaque = f["opaque"]
    }
    mutating func header(method: String, uri: String, code: String, cnonce: String = UUID().uuidString) -> String {
        func md5(_ text: String) -> String { Insecure.MD5.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined() }
        func quote(_ s: String) -> String { s.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"").replacingOccurrences(of: "\r", with: "").replacingOccurrences(of: "\n", with: "") }
        count += 1
        let nc = String(format: "%08x", count)
        let a1 = md5("bblp:\(realm):\(code)"), a2 = md5("\(method):\(uri)")
        let response = md5(qop ? "\(a1):\(nonce):\(nc):\(cnonce):auth:\(a2)" : "\(a1):\(nonce):\(a2)")
        var value = "Digest username=\"bblp\", realm=\"\(quote(realm))\", nonce=\"\(quote(nonce))\", uri=\"\(quote(uri))\", response=\"\(response)\""
        if qop { value += ", qop=auth, nc=\(nc), cnonce=\"\(quote(cnonce))\"" }
        if let opaque { value += ", opaque=\"\(quote(opaque))\"" }
        return value
    }
}

/// RFC 6184 single NAL, STAP-A and FU-A. A lost fragment discards the complete access unit.
struct CameraRTP {
    var lastSequence: UInt16?, timestamp: UInt32?, ssrc: UInt32?
    var fragment = Data(), unit = [Data](), invalid = false
    mutating func accept(_ data: Data) throws -> [Data]? {
        let b = Array(data)
        guard b.count >= 12, b[0] >> 6 == 2 else { throw CameraFailure("Invalid camera RTP packet.") }
        let seq = UInt16(b[2]) << 8 | UInt16(b[3])
        let ts = b[4...7].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
        let source = b[8...11].reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
        let lost = lastSequence.map { $0 &+ 1 != seq } ?? false
        if timestamp != ts || ssrc != source { unit = []; fragment = Data(); invalid = false }
        if lost { throw CameraFailure("Camera lost a video packet.") }
        timestamp = ts; ssrc = source; lastSequence = seq
        var start = 12 + Int(b[0] & 15) * 4, end = b.count
        guard start <= end else { throw CameraFailure("Invalid RTP header.") }
        if b[0] & 16 != 0 {
            guard start + 4 <= end else { throw CameraFailure("Invalid RTP extension.") }
            start += 4 + (Int(b[start + 2]) * 256 + Int(b[start + 3])) * 4
        }
        if b[0] & 32 != 0 { let padding = Int(b.last!); guard padding > 0, padding <= end - start else { throw CameraFailure("Invalid RTP padding.") }; end -= padding }
        guard start < end else { throw CameraFailure("Empty RTP payload.") }
        let p = Array(b[start..<end]), type = p[0] & 31
        if (1...23).contains(type) { unit.append(Data(p)) }
        else if type == 24 {
            var i = 1
            while i < p.count {
                guard i + 2 <= p.count else { throw CameraFailure("Invalid camera NAL aggregation.") }
                let n = Int(p[i]) * 256 + Int(p[i+1]); i += 2
                guard n > 0, i + n <= p.count else { throw CameraFailure("Invalid camera NAL length.") }
                unit.append(Data(p[i..<i+n])); i += n
            }
        } else if type == 28 {
            guard p.count >= 3 else { throw CameraFailure("Invalid camera fragment.") }
            if p[1] & 128 != 0 { fragment = Data([(p[0] & 224) | (p[1] & 31)]) }
            guard !fragment.isEmpty else { invalid = true; return nil }
            fragment.append(contentsOf: p.dropFirst(2))
            if p[1] & 64 != 0 { unit.append(fragment); fragment = Data() }
        } else { throw CameraFailure("Unsupported camera H.264 packet format.") }
        guard fragment.count + unit.reduce(0, { $0 + $1.count }) < 4_194_304 else { throw CameraFailure("Camera frame exceeds the size limit.") }
        if b[1] & 128 != 0 {
            defer { unit = []; fragment = Data() }
            return invalid || !fragment.isEmpty ? nil : unit
        }
        return nil
    }
}
