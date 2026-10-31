import SwiftUI
import UniformTypeIdentifiers

struct AddPrinterView: View {
    @EnvironmentObject private var store: PrinterStore
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var kind = PrinterConfig.Kind.moonraker
    @State private var host = ""
    @State private var serial = ""
    @State private var accessCode = ""
    @State private var relayURL = ""
    @State private var relayToken = ""
    @State private var importingPairing = false
    @State private var saveError: String?

    private var canSave: Bool {
        !name.trimmingCharacters(in: .whitespaces).isEmpty && !host.trimmingCharacters(in: .whitespaces).isEmpty &&
            (kind == .moonraker || (!serial.isEmpty && !accessCode.isEmpty))
    }

    var body: some View {
        NavigationStack {
            Form {
                Section {
                    TextField("Name", text: $name)
                    Picker("Type", selection: $kind) {
                        ForEach(PrinterConfig.Kind.allCases) { Text($0.label).tag($0) }
                    }
                    TextField(kind == .moonraker ? "IP address (optionally :port)" : "IP address", text: $host)
                        .textInputAutocapitalization(.never)
                        .keyboardType(.URL)
                        .autocorrectionDisabled()
                }
                if kind == .moonraker {
                    Section("Optional home relay") {
                        Button("Import pairing file") { importingPairing = true }
                        TextField("https://your-relay.example/v1/u1", text: $relayURL)
                            .textInputAutocapitalization(.never).autocorrectionDisabled()
                        SecureField("Relay pairing token", text: $relayToken)
                        Text("Uses LAN when reachable, otherwise your HTTPS relay. Your Mac must remain awake. Configure the relay before pairing; this does not create a cloud account.").font(.caption)
                    }
                }
                if kind == .bambuLAN {
                    Section {
                        TextField("Serial number", text: $serial)
                            .textInputAutocapitalization(.characters)
                            .autocorrectionDisabled()
                        SecureField("Access code", text: $accessCode)
                    } footer: {
                        Text("Shown on the printer's screen under network settings. On newer firmware, turn on LAN mode and developer mode. The access code is stored in the iOS Keychain.")
                    }
                }
            }
            .navigationTitle("Add printer")
            .fileImporter(isPresented: $importingPairing, allowedContentTypes: [.json]) { result in
                do {
                    let pair = try RelayPairing.read(result.get())
                    name = pair.name; host = pair.lanHost; relayURL = pair.relayURL; relayToken = pair.token
                } catch { saveError = error.localizedDescription }
            }
            .alert("Could not save printer", isPresented: Binding(get: { saveError != nil }, set: { if !$0 { saveError = nil } })) { Button("OK") { saveError = nil } } message: { Text(saveError ?? "") }
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        var printer = PrinterConfig(name: name.trimmingCharacters(in: .whitespaces), kind: kind,
                                                    host: host.trimmingCharacters(in: .whitespaces),
                                                    serial: serial.trimmingCharacters(in: .whitespaces))
                        do {
                            if kind == .moonraker && !relayURL.isEmpty {
                                printer.relayURL = relayURL.trimmingCharacters(in: .whitespacesAndNewlines)
                                try printer.validate()
                                _ = try RelayConnection.token(printer.id, save: relayToken)
                            }
                            try store.add(printer, accessCode: kind == .bambuLAN ? accessCode : nil)
                            dismiss()
                        } catch { saveError = error.localizedDescription }
                    }
                    .disabled(!canSave)
                }
            }
        }
    }
}
