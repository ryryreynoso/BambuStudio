import Foundation

/// Keeps one printer's status up to date while its screen is open.
@MainActor
final class PrinterMonitor: ObservableObject {
    @Published private(set) var status = PrinterStatus()
    @Published private(set) var connection = "Connecting..."
    @Published private(set) var lastUpdate: Date?

    let printer: PrinterConfig
    private var pollTask: Task<Void, Never>?
    private var bambu: BambuLANClient?

    init(printer: PrinterConfig) {
        self.printer = printer
    }

    func start() {
        switch printer.kind {
        case .moonraker: startMoonraker()
        case .bambuLAN: startBambu()
        }
    }

    func stop() {
        pollTask?.cancel()
        pollTask = nil
        bambu?.stop()
        bambu = nil
    }

    private func startMoonraker() {
        let client = MoonrakerClient(host: printer.host)
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    let status = try await client.fetchStatus()
                    self?.status = status
                    self?.connection = "Connected"
                    self?.lastUpdate = Date()
                } catch {
                    self?.connection = error.localizedDescription
                }
                try? await Task.sleep(nanoseconds: 3_000_000_000)
            }
        }
    }

    private func startBambu() {
        guard let code = Keychain.accessCode(for: printer.id), !code.isEmpty else {
            connection = "No access code saved for this printer."
            return
        }
        let client = BambuLANClient(host: printer.host, serial: printer.serial, accessCode: code) { [weak self] event in
            Task { @MainActor in
                guard let self else { return }
                switch event {
                case .connected:
                    self.connection = "Connected"
                case .report(let payload):
                    self.status.mergeBambuReport(payload)
                    self.lastUpdate = Date()
                case .failed(let message):
                    self.connection = message
                }
            }
        }
        bambu = client
        client.start()
    }
}
