import Foundation
import VideoToolbox
import CoreVideo
var session: VTCompressionSession?
let result = VTCompressionSessionCreate(allocator: kCFAllocatorDefault, width: 32, height: 32, codecType: kCMVideoCodecType_H264, encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil, outputCallback: { _, _, status, _, sample in
    guard status == 0, let sample, let format = CMSampleBufferGetFormatDescription(sample), let block = CMSampleBufferGetDataBuffer(sample) else { exit(2) }
    var sets = [String]()
    for index in 0..<2 {
        var ptr: UnsafePointer<UInt8>?, size = 0, count = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, parameterSetIndex: index, parameterSetPointerOut: &ptr, parameterSetSizeOut: &size, parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil) == 0, let ptr else { exit(3) }
        sets.append(Data(bytes: ptr, count: size).base64EncodedString())
    }
    var bytes = Data(count: CMBlockBufferGetDataLength(block))
    let size = bytes.count
    bytes.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(block, atOffset: 0, dataLength: size, destination: $0.baseAddress!) }
    var nals = [String](), i = 0
    while i+4 <= bytes.count {
        let n = bytes[i..<i+4].reduce(0) { $0 * 256 + Int($1) }; i += 4
        guard i+n <= bytes.count else { exit(4) }
        nals.append(bytes[i..<i+n].base64EncodedString()); i += n
    }
    let json = try! JSONSerialization.data(withJSONObject: ["sets": sets, "nals": nals])
    try! json.write(to: URL(fileURLWithPath: CommandLine.arguments[1]))
}, refcon: nil, compressionSessionOut: &session)
guard result == 0, let session else { exit(1) }
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
VTSessionSetProperty(session, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
var pixel: CVPixelBuffer?
CVPixelBufferCreate(kCFAllocatorDefault, 32, 32, kCVPixelFormatType_32ARGB, nil, &pixel)
guard let pixel else { exit(5) }
CVPixelBufferLockBaseAddress(pixel, [])
memset(CVPixelBufferGetBaseAddress(pixel), 128, CVPixelBufferGetDataSize(pixel))
CVPixelBufferUnlockBaseAddress(pixel, [])
VTCompressionSessionEncodeFrame(session, imageBuffer: pixel, presentationTimeStamp: .zero, duration: CMTime(value: 1, timescale: 30), frameProperties: nil, sourceFrameRefcon: nil, infoFlagsOut: nil)
VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
VTCompressionSessionInvalidate(session)
