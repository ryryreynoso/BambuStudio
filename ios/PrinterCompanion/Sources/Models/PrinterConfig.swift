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

    func add(_ printer: PrinterConfig, accessCode: String?) {
        printers.append(printer)
        if let accessCode, !accessCode.isEmpty { Keychain.setAccessCode(accessCode, for: printer.id) }
        save()
    }

    func remove(at offsets: IndexSet) {
        for index in offsets { Keychain.deleteAccessCode(for: printers[index].id) }
        printers.remove(atOffsets: offsets)
        save()
    }

    private func save() {
        if let data = try? JSONEncoder().encode(printers) { defaults.set(data, forKey: key) }
    }
}
