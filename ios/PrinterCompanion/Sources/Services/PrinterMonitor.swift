import Foundation

/// Keeps one printer's status up to date while its screen is open.
@MainActor
final class PrinterMonitor: ObservableObject {
    @Published private(set) var status = PrinterStatus()
    @Published private(set) var connection = "Connecting..."
    @Published private(set) var lastUpdate: Date?
    /// Latest camera picture (JPEG) and when it was fetched; Moonraker printers only.
    @Published private(set) var cameraImage: Data?
    @Published private(set) var cameraUpdate: Date?

    private(set) var printer: PrinterConfig
    private var pollTask: Task<Void, Never>?
    private var bambu: BambuLANClient?

    init(printer: PrinterConfig) {
        self.printer = printer
    }

    func reconfigure(_ printer: PrinterConfig) {
        stop()
        self.printer = printer
        start()
    }

    func start() {
        stop()
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
        let printer = self.printer
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                do {
                    let (client, route) = try await RelayConnection.select(printer)
                    defer { if route == "Home relay" { client.session.finishTasksAndInvalidate() } }
                    let status = try await client.fetchStatus()
                    try Task.checkCancellation()
                    self?.status = status
                    self?.connection = "Connected — " + route
                    self?.lastUpdate = Date()
                    if let image = try? await client.fetchCameraImage() {
                        try Task.checkCancellation()
                        self?.cameraImage = image; self?.cameraUpdate = Date()
                    }
                } catch { if !Task.isCancelled { self?.connection = error.localizedDescription } }
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
