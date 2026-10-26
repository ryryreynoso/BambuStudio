import SwiftUI

struct AddPrinterView: View {
    @EnvironmentObject private var store: PrinterStore
    @Environment(\.dismiss) private var dismiss

    @State private var name = ""
    @State private var kind = PrinterConfig.Kind.moonraker
    @State private var host = ""
    @State private var serial = ""
    @State private var accessCode = ""

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
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        let printer = PrinterConfig(name: name.trimmingCharacters(in: .whitespaces), kind: kind,
                                                    host: host.trimmingCharacters(in: .whitespaces),
                                                    serial: serial.trimmingCharacters(in: .whitespaces))
                        store.add(printer, accessCode: kind == .bambuLAN ? accessCode : nil)
                        dismiss()
                    }
                    .disabled(!canSave)
                }
            }
        }
    }
}
