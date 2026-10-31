import Foundation
import Security
import Network
import AVFoundation
import VideoToolbox
let args = CommandLine.arguments
let authority = SecCertificateCreateWithData(nil, try Data(contentsOf: URL(fileURLWithPath: args[1])) as CFData)!
let expected = args[2]
var samples = 0
var statuses = [String]()
let camera = CameraStream(host: "127.0.0.1", serial: expected, code: "fixture-code", onSample: { sample in
    samples += 1
    print("SAMPLE \(CMSampleBufferGetTotalSampleSize(sample))")
    var decoder: VTDecompressionSession?
    var callback = VTDecompressionOutputCallbackRecord(decompressionOutputCallback: { _, _, status, _, image, _, _ in
        if status == 0, let image { print("DECODED \(CVPixelBufferGetWidth(image))x\(CVPixelBufferGetHeight(image))") }
        else { print("DECODE_FAILED") }
        fflush(stdout)
    }, decompressionOutputRefCon: nil)
    if let format = CMSampleBufferGetFormatDescription(sample),
       VTDecompressionSessionCreate(allocator: kCFAllocatorDefault, formatDescription: format, decoderSpecification: nil, imageBufferAttributes: nil, outputCallback: &callback, decompressionSessionOut: &decoder) == 0, let decoder {
        VTDecompressionSessionDecodeFrame(decoder, sampleBuffer: sample, flags: [], frameRefcon: nil, infoFlagsOut: nil)
        VTDecompressionSessionWaitForAsynchronousFrames(decoder)
        VTDecompressionSessionInvalidate(decoder)
    }
    fflush(stdout)
}, onStatus: { status in
    statuses.append(status); print(status); fflush(stdout)
}, port: NWEndpoint.Port(rawValue: UInt16(args[3])!)!, verifier: { trust, serial in
    SecTrustSetPolicies(trust, SecPolicyCreateBasicX509())
    SecTrustSetAnchorCertificates(trust, [authority] as CFArray)
    SecTrustSetAnchorCertificatesOnly(trust, true)
    guard SecTrustEvaluateWithError(trust, nil), let leaf = (SecTrustCopyCertificateChain(trust) as? [SecCertificate])?.first else { return false }
    var cn: CFString?; SecCertificateCopyCommonName(leaf, &cn)
    return cn as String? == serial
})
camera.start()
RunLoop.current.run(until: Date().addingTimeInterval(Double(args[4])!))
camera.stop()
RunLoop.current.run(until: Date().addingTimeInterval(2.5))
