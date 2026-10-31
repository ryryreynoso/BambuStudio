// SPDX-License-Identifier: AGPL-3.0-only
import XCTest
@testable import PrinterCompanion

final class MobilePrintingTests: XCTestCase {
    func testPrintableVolumeRejectsOversizeHeightAndNonfiniteCoordinates() {
        var transform = [1.0,0,0,0, 0,1,0,0, 0,0,1,0, 128,128,0,1]
        func fits(_ maximum: [Double]) -> Bool {
            PrintVolume.contains(minimum: [-10,-10,0], maximum: maximum, matrix: transform,
                                 minX: 0, minY: 0, maxX: 256, maxY: 256, height: 256)
        }
        XCTAssertTrue(fits([10,10,20]))
        XCTAssertFalse(fits([10,10,300]))
        XCTAssertFalse(fits([200,10,20]))
        transform[12] = .nan
        XCTAssertFalse(fits([10,10,20]))
    }
    func testRejectsInvalidSliceParameters() {
        let model = URL(fileURLWithPath: "/model.stl")
        for height in [Double.nan, -Double.infinity, 0, 0.5] {
            XCTAssertThrowsError(try SliceRequest(model: model, printer: .p2s, layerHeight: height, infill: 15, supports: false).validate())
        }
        XCTAssertThrowsError(try SliceRequest(model: URL(fileURLWithPath: "/file.gcode"), printer: .u1, layerHeight: 0.2, infill: 15, supports: false).validate())
        XCTAssertThrowsError(try SliceRequest(model: model, printer: .u1, layerHeight: 0.2, infill: 101, supports: false).validate())
    }
    func testUnrelatedJobAndUploadNeverCountAsAcceptance() {
        XCTAssertEqual(PrintContract.bambuAcceptance(["print": ["gcode_state": "RUNNING", "gcode_file": "other.3mf"]], sequence: "42", remote: "ours.3mf"), .pending)
        XCTAssertEqual(PrintContract.bambuAcceptance(["print": ["command": "project_file", "sequence_id": "41", "result": "success"]], sequence: "42", remote: "ours.3mf"), .pending)
        XCTAssertEqual(PrintContract.bambuAcceptance(["uploaded": true], sequence: "42", remote: "ours.3mf"), .pending)
    }
    func testCorrelatedAcceptanceAndRejection() {
        XCTAssertEqual(PrintContract.bambuAcceptance(["print": ["command": "project_file", "sequence_id": 42, "result": "success"]], sequence: "42", remote: "ours.3mf"), .accepted)
        XCTAssertEqual(PrintContract.bambuAcceptance(["print": ["command": "project_file", "sequence_id": "42", "result": "success", "reason": 1]], sequence: "42", remote: "ours.3mf"), .rejected)
        XCTAssertEqual(PrintContract.bambuAcceptance(["print": ["gcode_state": "PREPARE", "gcode_file": "/ours.3mf"]], sequence: "42", remote: "ours.3mf"), .accepted)
    }
    func testIndependentProfilesAndOptions() throws {
        let config = PrinterConfig(name: "U1", kind: .moonraker, host: "printer.test")
        let file = SlicedFile(url: URL(fileURLWithPath: "/file.3mf"), printer: .p2s, estimatedSeconds: 0, warnings: [])
        XCTAssertFalse(PrintContract.matches(config, file))
        var options = PrintOptions(); options.toolhead = 3; options.amsTray = 2
        let bambu = try XCTUnwrap(PrintContract.bambuCommand(remote: "file.3mf", sequence: "42", options: options)["print"] as? [String: Any])
        XCTAssertEqual(bambu["ams_mapping"] as? [Int], [2])
        let u1 = try XCTUnwrap(MoonrakerPrintDelivery.startParameters(remote: "file.gcode", options: options)["options"] as? [String: Any])
        XCTAssertEqual(u1["map_table"] as? [[Int]], [[0, 3]])
        XCTAssertNil(u1["ams_mapping"])
    }
    func testMQTTRejectsOversizedPacketsAndRefusedSubscription() throws {
        var oversized = Data([0x30]) + MQTTPacket.encodeRemainingLength(2 * 1024 * 1024)
        XCTAssertThrowsError(try MQTTPacket.decode(&oversized))
        var refused = Data([0x90, 3, 0, 1, 0x80])
        XCTAssertEqual(try MQTTPacket.decode(&refused), [.suback(packetID: 1, codes: [0x80])])
        var retained = Data([0x31, 4, 0, 1, 65, 66])
        XCTAssertEqual(try MQTTPacket.decode(&retained), [.publish(topic: "A", payload: Data([66]), retained: true)])
    }
    func testRejectsCredentialsInPlainPrinterConfiguration() {
        XCTAssertThrowsError(try PrinterConfig(name: "U1", kind: .moonraker, host: "http://user:password@u1.local").validate())
        XCTAssertThrowsError(try PrinterConfig(name: "U1", kind: .moonraker, host: "https://u1.local/?token=secret").validate())
        XCTAssertThrowsError(try PrinterConfig(name: "P2S", kind: .bambuLAN, host: "https://p2s.local", serial: "ABC123456").validate())
        XCTAssertNil(MoonrakerClient(host: "http://user:password@u1.local").statusURL)
    }
    func testAMSFilamentReportsMergeWithoutErasingOnPartialTelemetry() {
        var status = PrinterStatus()
        status.mergeBambuReport(Data(#"{"print":{"ams":{"ams":[{"id":"1","tray":[{"id":"2","tray_type":"PLA","tray_color":"FF0000FF"}]}]}}}"#.utf8))
        XCTAssertEqual(status.filaments.first?.id, 6)
        XCTAssertEqual(status.filaments.first?.material, "PLA")
        status.mergeBambuReport(Data(#"{"print":{"mc_percent":10}}"#.utf8))
        XCTAssertEqual(status.filaments.count, 1)
    }
    func testMultipartDoesNotStartPrintAndPreservesBinaryBytes() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let input = dir.appendingPathComponent("input.gcode"), output = dir.appendingPathComponent("multipart")
        let bytes = Data([0, 255, 10, 13, 65])
        try bytes.write(to: input)
        try MoonrakerPrintDelivery.multipart(file: input, remote: "unique.gcode", boundary: "BOUNDARY", output: output)
        let body = try Data(contentsOf: output)
        XCTAssertNotNil(body.range(of: bytes))
        XCTAssertNotNil(body.range(of: Data("name=\"print\"\r\n\r\nfalse".utf8)))
        XCTAssertThrowsError(try MoonrakerPrintDelivery.multipart(file: input, remote: "bad\r\nname.gcode", boundary: "BOUNDARY", output: output))
    }
    @MainActor func testPrinterConfigurationSurvivesRestart() throws {
        let suite = "mobile-test-\(UUID().uuidString)", defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let config = PrinterConfig(name: "U1 over VPN", kind: .moonraker, host: "u1.home.test")
        try PrinterStore(defaults: defaults).add(config, accessCode: nil)
        XCTAssertEqual(PrinterStore(defaults: defaults).printers, [config])
        let stored = try XCTUnwrap(defaults.data(forKey: "printers.v1"))
        XCTAssertFalse(String(decoding: stored, as: UTF8.self).contains("accessCode"))
    }
}

/// Real native slicing, with no network or print initiation. Run on an iPhone/iPad.
final class NativeSlicingTests: XCTestCase {
    func testP2SNativeSlice() async throws { try await slice(.p2s) }
    func testU1NativeSlice() async throws { try await slice(.u1) }
    private func slice(_ printer: SlicePrinter) async throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString).appendingPathExtension("stl")
        // Closed tetrahedron, 20 mm bounding box. ASCII STL is consumed by the real engine.
        let vertices = [[0.0,0,0],[20,0,0],[0,20,0],[0,0,20]]
        let faces = [[0,2,1],[0,1,3],[0,3,2],[1,2,3]]
        var stl = "solid tetra\n"
        for face in faces {
            stl += "facet normal 0 0 0\nouter loop\n"
            for index in face { let v = vertices[index]; stl += "vertex \(v[0]) \(v[1]) \(v[2])\n" }
            stl += "endloop\nendfacet\n"
        }
        stl += "endsolid tetra\n"
        try stl.write(to: url, atomically: true, encoding: .utf8)
        defer { try? FileManager.default.removeItem(at: url) }
        let result = try await MobileSlicer().slice(SliceRequest(model: url, printer: printer, layerHeight: 0.2, infill: 15, supports: false)) { _, stage in print("NATIVE_STAGE: \(stage)") }
        defer { try? FileManager.default.removeItem(at: result.url) }
        let output = try Data(contentsOf: result.url)
        XCTAssertGreaterThan(output.count, 1024)
        XCTAssertGreaterThan(result.estimatedSeconds, 0)
        if printer == .p2s {
            XCTAssertEqual(Array(output.prefix(2)), [80, 75])
            XCTAssertNotNil(output.range(of: Data("Metadata/plate_1.gcode".utf8)))
        } else {
            XCTAssertNotNil(output.range(of: Data("G1 ".utf8)))
        }
    }
}
