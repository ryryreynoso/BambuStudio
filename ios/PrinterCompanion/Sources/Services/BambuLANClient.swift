import Foundation
import Network
import Security

/// Connects to a Bambu Lab printer over its LAN MQTT broker (port 8883, user "bblp", password =
/// the printer's access code). TLS is verified against Bambu's printer certificate authorities only,
/// and the printer's certificate must name the serial number the user entered. PrintDelivery can explicitly publish a print request; monitoring sends only pushall.
final class BambuLANClient {
    enum Event {
        case connected
        case report(Data)
        case failed(String)
    }

    private let host: String
    private let serial: String
    private let accessCode: String
    private let queue = DispatchQueue(label: "PrinterCompanion.bambu-lan")
    private let onEvent: (Event) -> Void
    private var connection: NWConnection?
    private var buffer = Data()
    private var pingTimer: DispatchSourceTimer?

    init(host: String, serial: String, accessCode: String, onEvent: @escaping (Event) -> Void) {
        self.host = host
        self.serial = serial
        self.accessCode = accessCode
        self.onEvent = onEvent
    }

    func start() {
        let tls = NWProtocolTLS.Options()
        let serial = self.serial
        sec_protocol_options_set_verify_block(tls.securityProtocolOptions, { _, trustRef, complete in
            let trust = sec_trust_copy_ref(trustRef).takeRetainedValue()
            complete(BambuLANClient.isTrusted(trust, serial: serial))
        }, queue)
        let connection = NWConnection(host: NWEndpoint.Host(host), port: 8883, using: NWParameters(tls: tls))
        connection.stateUpdateHandler = { [weak self] state in self?.handle(state) }
        self.connection = connection
        connection.start(queue: queue)
    }

    func stop() {
        queue.async { [weak self] in
            guard let self else { return }
            self.pingTimer?.cancel()
            self.pingTimer = nil
            if let connection = self.connection {
                connection.send(content: MQTTPacket.disconnect, completion: .contentProcessed { _ in connection.cancel() })
            }
            self.connection = nil
        }
    }

    /// Chain must lead to one of Bambu's printer CAs, and the leaf certificate's common name must be
    /// the serial number. Host names are not checked because printers are reached by IP address.
    static func isTrusted(_ trust: SecTrust, serial: String) -> Bool {
        CameraTrust.verify(trust, serial: serial)
    }

    private func handle(_ state: NWConnection.State) {
        switch state {
        case .ready:
            let clientID = "companion-" + UUID().uuidString.prefix(8)
            send(MQTTPacket.connect(clientID: String(clientID), username: "bblp", password: accessCode, keepAlive: 60))
            receive()
        case .failed(let error):
            onEvent(.failed(error.localizedDescription))
        case .waiting(let error):
            onEvent(.failed(error.localizedDescription))
        default:
            break
        }
    }

    private func send(_ data: Data) {
        connection?.send(content: data, completion: .contentProcessed { [weak self] error in
            if let error { self?.onEvent(.failed(error.localizedDescription)) }
        })
    }

    private func receive() {
        connection?.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            if let data { self.buffer.append(data) }
            do {
                for packet in try MQTTPacket.decode(&self.buffer) { self.handle(packet) }
            } catch {
                self.onEvent(.failed("Unexpected data from the printer."))
                self.connection?.cancel()
                return
            }
            if let error {
                self.onEvent(.failed(error.localizedDescription))
            } else if isComplete {
                self.onEvent(.failed("The printer closed the connection."))
            } else {
                self.receive()
            }
        }
    }

    private func handle(_ packet: MQTTPacket.Incoming) {
        switch packet {
        case .connack(let code):
            guard code == 0 else {
                onEvent(.failed(code == 4 || code == 5 ? "The printer rejected the access code." : "The printer refused the connection (\(code))."))
                return
            }
            send(MQTTPacket.subscribe(packetID: 1, topic: "device/\(serial)/report"))

        case .suback(let id, let codes):
            guard id == 1, codes == [0] else {
                onEvent(.failed("The printer refused the status subscription."))
                connection?.cancel()
                return
            }
            onEvent(.connected)
            requestFullStatus()
            startPing()
        case .publish(let topic, let payload, let retained):
            guard topic == "device/\(serial)/report", !retained else { return }
            onEvent(.report(payload))
        default:
            break
        }
    }

    func publishRequest(_ request: [String: Any]) throws {
        let payload = try JSONSerialization.data(withJSONObject: request)
        queue.async { [weak self] in
            guard let self else { return }
            self.send(MQTTPacket.publish(topic: "device/\(self.serial)/request", payload: payload))
        }
    }

    private func requestFullStatus() {
        let request = #"{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}"#
        send(MQTTPacket.publish(topic: "device/\(serial)/request", payload: Data(request.utf8)))
    }

    private func startPing() {
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 30, repeating: 30)
        timer.setEventHandler { [weak self] in self?.send(MQTTPacket.pingreq) }
        timer.resume()
        pingTimer = timer
    }
}
