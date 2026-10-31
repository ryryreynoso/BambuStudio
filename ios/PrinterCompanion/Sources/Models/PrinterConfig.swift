import Foundation

/// A printer the user added. The Bambu access code is not part of this struct: it lives in the
/// Keychain (see `Keychain`), keyed by `id`.
struct PrinterConfig: Codable, Identifiable, Equatable {
    enum Kind: String, Codable, CaseIterable, Identifiable {
        case moonraker   // Snapmaker U1 and other Klipper printers
        case bambuLAN    // Bambu Lab printers in LAN mode (developer mode on newer firmware)

        var id: String { rawValue }
        var label: String {
            switch self {
            case .moonraker: return "Snapmaker U1 / Klipper (Moonraker)"
            case .bambuLAN: return "Bambu Lab (LAN)"
            }
        }
    }

    var id = UUID()
    var name: String
    var kind: Kind
    /// IP address or host name, optionally with ":port" for Moonraker.
    var host: String
    /// Bambu serial number; the printer's certificate must name it.
    var serial: String = ""
    var relayURL: String? = nil

    func validate() throws {
        if let relayURL {
            guard kind == .moonraker else { throw SlicerFailure("This relay currently supports Snapmaker U1 only.") }
            try RelayConnection.validate(relayURL)
        }
        guard !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { throw SlicerFailure("Enter a printer name.") }
        if kind == .bambuLAN {
            guard host.range(of: #"^[A-Za-z0-9._-]+$"#, options: .regularExpression) != nil,
                  serial.range(of: #"^[A-Za-z0-9]{6,40}$"#, options: .regularExpression) != nil else {
                throw SlicerFailure("Enter a printer IPv4 address or VPN DNS name and the serial number. Do not include a URL or credentials.")
            }
        } else {
            let base = host.contains("://") ? host : "http://" + host
            guard let url = URLComponents(string: base), ["http", "https"].contains(url.scheme ?? ""),
                  let hostname = url.host, !hostname.isEmpty, url.user == nil, url.password == nil,
                  url.query == nil, url.fragment == nil else {
                throw SlicerFailure("Enter an HTTP(S) printer address without passwords, query parameters or fragments. For remote U1 access, configure an HTTPS home relay.")
            }
        }
    }
}

/// The list of printers, kept in UserDefaults (no secrets).
final class PrinterStore: ObservableObject {
    @Published private(set) var printers: [PrinterConfig] = []
    private let defaults: UserDefaults
    private let key = "printers.v1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        if let data = defaults.data(forKey: key),
           let decoded = try? JSONDecoder().decode([PrinterConfig].self, from: data) {
            printers = decoded
        }
    }

    func add(_ printer: PrinterConfig, accessCode: String?) throws {
        try printer.validate()
        if let accessCode, !accessCode.isEmpty { try Keychain.setAccessCode(accessCode, for: printer.id) }
        printers.append(printer)
        save()
    }

    func setRelay(for id: UUID, address: String?, token: String?) throws {
        guard let index = printers.firstIndex(where: { $0.id == id }), printers[index].kind == .moonraker else {
            throw SlicerFailure("Select a saved U1 printer first.")
        }
        var updated = printers[index]
        updated.relayURL = address?.trimmingCharacters(in: .whitespacesAndNewlines)
        try updated.validate()
        if updated.relayURL != nil {
            guard let token else { throw SlicerFailure("Import a relay pairing first.") }
            _ = try RelayConnection.token(id, save: token)
        } else { RelayConnection.removeToken(id) }
        printers[index] = updated
        save()
    }

    func remove(at offsets: IndexSet) {
        for index in offsets { Keychain.deleteAccessCode(for: printers[index].id); RelayConnection.removeToken(printers[index].id) }
        printers.remove(atOffsets: offsets)
        save()
    }

    private func save() {
        if let data = try? JSONEncoder().encode(printers) { defaults.set(data, forKey: key) }
    }
}
