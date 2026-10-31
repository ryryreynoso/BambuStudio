import XCTest
@testable import PrinterCompanion
final class RelayTests: XCTestCase {
    func testSecureEndpointValidation() throws {
        try RelayConnection.validate("https://relay.example/v1/u1")
        for value in ["http://relay.example/v1/u1", "https://user:secret@relay.example/v1/u1", "https://relay.example/v1/u1?token=x", "https://relay.example/", "https://relay.example/v1/u1#secret"] {
            XCTAssertThrowsError(try RelayConnection.validate(value))
        }
    }
    func testExistingPrinterConfigurationSurvives() throws {
        let printer = PrinterConfig(name: "U1", kind: .moonraker, host: "10.0.1.4")
        let restored = try JSONDecoder().decode(PrinterConfig.self, from: JSONEncoder().encode(printer))
        XCTAssertNil(restored.relayURL)
        XCTAssertEqual(restored, printer)
    }
    func testBambuCannotAccidentallyUseU1Relay() {
        let printer = PrinterConfig(name: "P2S", kind: .bambuLAN, host: "10.0.1.16", serial: "TEST1234", relayURL: "https://relay.example/v1/u1")
        XCTAssertThrowsError(try printer.validate())
    }
    func testPairingImportValidatesCredentialsAndVersion() throws {
        func document(version: Int = 1, token: String = String(repeating: "x", count: 43), address: String = "https://relay.example/v1/u1") throws -> Data {
            try JSONSerialization.data(withJSONObject: ["version":version,"name":"U1","lanHost":"10.0.1.4","relayURL":address,"token":token])
        }
        let pairing = try RelayPairing.decode(document())
        XCTAssertEqual(pairing.name, "U1")
        XCTAssertEqual(pairing.lanHost, "10.0.1.4")
        XCTAssertThrowsError(try RelayPairing.decode(document(version: 2)))
        XCTAssertThrowsError(try RelayPairing.decode(document(token: "short")))
        XCTAssertThrowsError(try RelayPairing.decode(document(token: String(repeating: "x",count:32) + "\r\nInjected: yes")))
        XCTAssertThrowsError(try RelayPairing.decode(document(address: "http://relay.example/v1/u1")))
    }
    func testPairingImportRejectsInvalidAndOversizedDocuments() {
        XCTAssertThrowsError(try RelayPairing.decode(Data("[]".utf8)))
        XCTAssertThrowsError(try RelayPairing.decode(Data(repeating: 32, count: 16_385)))
    }
    func testRelayConfigurationPersistsWithoutSecret() throws {
        let config = PrinterConfig(name:"U1",kind:.moonraker,host:"10.0.1.4",relayURL:"https://relay.example/v1/u1")
        let data = try JSONEncoder().encode(config)
        XCTAssertEqual(try JSONDecoder().decode(PrinterConfig.self,from:data),config)
        let object = try XCTUnwrap(JSONSerialization.jsonObject(with:data) as? [String:Any])
        XCTAssertNil(object["token"])
    }

}
