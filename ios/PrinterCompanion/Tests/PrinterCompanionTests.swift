import Security
import XCTest
@testable import PrinterCompanion

final class MQTTPacketTests: XCTestCase {
    func testRemainingLengthEncoding() {
        XCTAssertEqual(MQTTPacket.encodeRemainingLength(0), Data([0]))
        XCTAssertEqual(MQTTPacket.encodeRemainingLength(127), Data([0x7f]))
        XCTAssertEqual(MQTTPacket.encodeRemainingLength(128), Data([0x80, 0x01]))
        XCTAssertEqual(MQTTPacket.encodeRemainingLength(16_383), Data([0xff, 0x7f]))
        XCTAssertEqual(MQTTPacket.encodeRemainingLength(16_384), Data([0x80, 0x80, 0x01]))
    }

    func testConnectPacket() {
        let packet = MQTTPacket.connect(clientID: "c", username: "bblp", password: "12345678", keepAlive: 60)
        XCTAssertEqual(packet.first, 0x10)
        // protocol name "MQTT", level 4, flags username|password|clean, keep alive 60
        XCTAssertEqual(Array(packet[2..<12]), [0, 4, 0x4d, 0x51, 0x54, 0x54, 4, 0xc2, 0, 60])
        XCTAssertEqual(Int(packet[1]), packet.count - 2)
    }

    func testDecodePublishAndPartialPackets() throws {
        let payload = Data(#"{"print":{"mc_percent":42}}"#.utf8)
        let publish = MQTTPacket.publish(topic: "device/SN/report", payload: payload)
        var buffer = Data([0x20, 0x02, 0x00, 0x00]) + publish.prefix(5)   // CONNACK + half a PUBLISH
        var packets = try MQTTPacket.decode(&buffer)
        XCTAssertEqual(packets, [.connack(returnCode: 0)])
        buffer.append(publish.dropFirst(5))
        packets = try MQTTPacket.decode(&buffer)
        XCTAssertEqual(packets, [.publish(topic: "device/SN/report", payload: payload)])
        XCTAssertTrue(buffer.isEmpty)
    }

    func testSubscribeHasReservedFlags() {
        let packet = MQTTPacket.subscribe(packetID: 1, topic: "device/SN/report")
        XCTAssertEqual(packet.first, 0x82)
        XCTAssertEqual(packet.last, 0)  // QoS 0
    }
}

final class PrinterStatusTests: XCTestCase {
    func testBambuReportMergesPartialUpdates() {
        var status = PrinterStatus()
        status.mergeBambuReport(Data(#"{"print":{"gcode_state":"RUNNING","mc_percent":25,"mc_remaining_time":90,"layer_num":10,"total_layer_num":200,"nozzle_temper":219.6,"nozzle_target_temper":220,"bed_temper":55,"bed_target_temper":55,"subtask_name":"cube"}}"#.utf8))
        XCTAssertEqual(status.state, "Printing")
        XCTAssertEqual(status.progress, 0.25)
        XCTAssertEqual(status.remainingMinutes, 90)
        XCTAssertEqual(status.layer, 10)
        XCTAssertEqual(status.jobName, "cube")
        status.mergeBambuReport(Data(#"{"print":{"mc_percent":26}}"#.utf8))
        XCTAssertEqual(status.progress, 0.26)
        XCTAssertEqual(status.state, "Printing")
        XCTAssertEqual(status.heaters.first(where: { $0.name == "Nozzle" })?.target, 220)
    }

    func testMoonrakerQueryResult() throws {
        let reply = #"""
        {"result":{"eventtime":1.0,"status":{
          "print_stats":{"state":"printing","filename":"cube.gcode","print_duration":600,"message":"","info":{"current_layer":5,"total_layer":50}},
          "display_status":{"progress":0.5},
          "heater_bed":{"temperature":59.8,"target":60},
          "extruder":{"temperature":220.1,"target":220},
          "extruder1":{"temperature":70,"target":70}}}}
        """#
        var status = PrinterStatus()
        try status.applyMoonraker(Data(reply.utf8))
        XCTAssertEqual(status.state, "Printing")
        XCTAssertEqual(status.jobName, "cube.gcode")
        XCTAssertEqual(status.progress, 0.5)
        XCTAssertEqual(status.remainingMinutes, 10)
        XCTAssertEqual(status.layer, 5)
        XCTAssertEqual(status.heaters.map(\.name), ["Tool 1", "Tool 2", "Bed"])
    }

    func testMoonrakerRejectsOtherReplies() {
        var status = PrinterStatus()
        XCTAssertThrowsError(try status.applyMoonraker(Data(#"{"api":"0.1"}"#.utf8)))
    }

    func testMoonrakerURL() {
        XCTAssertEqual(MoonrakerClient(host: "192.168.1.20").statusURL?.absoluteString.hasPrefix("http://192.168.1.20/printer/objects/query?print_stats"), true)
        XCTAssertEqual(MoonrakerClient(host: "http://u1.local:7125/").statusURL?.host, "u1.local")
        XCTAssertEqual(MoonrakerClient(host: "10.0.1.4").cameraURL?.absoluteString, "http://10.0.1.4/server/files/camera/monitor.jpg")
    }

    func testSnapmakerErrors() throws {
        var status = PrinterStatus()
        try status.applyMoonraker(Data(#"{"result":{"status":{"print_stats":{"state":"standby"}}}}"#.utf8))
        XCTAssertNil(status.errors, "a printer without exception_manager has no error list")

        try status.applyMoonraker(Data(#"{"result":{"status":{"exception_manager":{"exceptions":[]}}}}"#.utf8))
        XCTAssertEqual(status.errors, [])

        let reply = #"{"result":{"status":{"exception_manager":{"exceptions":[{"message":"Filament runout","code":2101,"level":2},{"code":523},"raw"]}}}}"#
        try status.applyMoonraker(Data(reply.utf8))
        XCTAssertEqual(status.errors, ["Filament runout (code 2101, level 2)", "Error code 523", "raw"])
    }

    func testBambuCertificatesDecode() {
        XCTAssertEqual(BambuCertificates.authoritiesDER.count, 5)
        for der in BambuCertificates.authoritiesDER {
            XCTAssertNotNil(SecCertificateCreateWithData(nil, der as CFData))
        }
    }
}
