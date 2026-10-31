import XCTest
@testable import PrinterCompanion

final class CameraTests: XCTestCase {
    func testFragmentedAndCoalescedResponses() throws {
        var wire = CameraWire()
        let response = Data("RTSP/1.0 200 OK\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\nsdp".utf8)
        var messages = [CameraWire.Message]()
        for byte in response + Data([36,0,0,3,1,2,3]) { messages += try wire.append(Data([byte])) }
        XCTAssertEqual(messages.count, 2)
        guard case .response(200, let headers, let body) = messages[0] else { return XCTFail() }
        XCTAssertEqual(headers["cseq"], "1"); XCTAssertEqual(body, Data("sdp".utf8))
        guard case .packet(0, let packet) = messages[1] else { return XCTFail() }
        XCTAssertEqual(packet, Data([1,2,3]))
    }
    func testHostileResponseSizes() throws {
        for length in ["-1", "99999999"] {
            var wire = CameraWire()
            XCTAssertThrowsError(try wire.append(Data("RTSP/1.0 200 OK\r\nContent-Length: \(length)\r\n\r\n".utf8)))
        }
        var wire = CameraWire()
        XCTAssertThrowsError(try wire.append(Data(repeating: 65, count: 65536)))
    }
    func testDigestAndRejectedAlgorithms() throws {
        var digest = try CameraDigest(challenge: "Digest realm=\"LIVE555 Streaming Media\", nonce=\"test\"")
        let header = digest.header(method: "DESCRIBE", uri: "rtsps://printer:322/streaming/live/1", code: "12345678")
        XCTAssertTrue(header.contains("response=\"60496c03fe47ae1aae78c80955d1a7d4\""))
        XCTAssertFalse(header.contains("12345678"))
        XCTAssertThrowsError(try CameraDigest(challenge: "Basic realm=\"camera\""))
        XCTAssertThrowsError(try CameraDigest(challenge: "Digest realm=\"camera\", nonce=\"abc\", algorithm=SHA-256"))
        var qop = try CameraDigest(challenge: "Digest realm=\"camera\", nonce=\"abc\", qop=\"auth,auth-int\"")
        XCTAssertTrue(qop.header(method: "PLAY", uri: "rtsp://printer/", code: "secret", cnonce: "test").contains("nc=00000001"))
        XCTAssertTrue(qop.header(method: "PLAY", uri: "rtsp://printer/", code: "secret", cnonce: "test").contains("nc=00000002"))
    }
    func packet(_ seq: UInt16, _ payload: [UInt8], marker: Bool = true, ts: UInt8 = 1) -> Data {
        Data([128, marker ? 224 : 96, UInt8(seq >> 8), UInt8(seq & 255), 0,0,0,ts, 0,0,0,1] + payload)
    }
    func testNALFragmentationAndSequenceWrap() throws {
        var rtp = CameraRTP()
        XCTAssertNil(try rtp.accept(packet(65535, [124,133,1,2], marker: false)))
        XCTAssertEqual(try rtp.accept(packet(0, [124,69,3,4])), [Data([101,1,2,3,4])])
        XCTAssertEqual(try rtp.accept(packet(1, [24,0,2,103,1,0,2,104,2], ts: 2)), [Data([103,1]),Data([104,2])])
    }
    func testPacketLossAndMalformedStream() throws {
        var rtp = CameraRTP()
        XCTAssertNil(try rtp.accept(packet(1, [124,133,1], marker: false)))
        XCTAssertThrowsError(try rtp.accept(packet(3, [124,69,2])))
        rtp = CameraRTP()
        XCTAssertThrowsError(try rtp.accept(packet(1, [24,0,255,1])))
        rtp = CameraRTP()
        XCTAssertThrowsError(try rtp.accept(Data([0,1,2])))
    }
}
