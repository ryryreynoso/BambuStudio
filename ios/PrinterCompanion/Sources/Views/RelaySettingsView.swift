import SwiftUI
import UniformTypeIdentifiers

struct RelaySettingsView: View {
    let printer: PrinterConfig
    @EnvironmentObject private var store: PrinterStore
    @Environment(\.dismiss) private var dismiss
    @State private var address = ""
    @State private var token = ""
    @State private var importing = false
    @State private var testing = false
    @State private var message: String?

    var body: some View {
        NavigationStack {
            Form {
                Section("Home relay") {
                    TextField("https://your-relay.example/v1/u1", text: $address)
                        .textInputAutocapitalization(.never).autocorrectionDisabled().keyboardType(.URL)
                    SecureField("New pairing token (optional)", text: $token)
                    Button("Import pairing file") { importing = true }
                    Text("Your Mac must stay awake. On home Wi-Fi the app uses LAN; away from home it uses the HTTPS relay. No VPN is needed.").font(.caption)
                }
                Section {
                    Button(testing ? "Checking remote connection…" : "Test remote connection") {
                        testing = true; message = nil
                        Task {
                            defer { testing = false }
                            do {
                                let client = try RelayConnection.remote(address: address, token: savedOrNewToken())
                                defer { client.session.invalidateAndCancel() }
                                let status = try await client.fetchStatus()
                                let camera = try? await client.fetchCameraImage()
                                message = "Remote connection verified: \(status.state). " + (camera == nil ? "No camera snapshot available." : "Camera snapshot available.") + " No print sent."
                            } catch { message = error.localizedDescription }
                        }
                    }.disabled(testing || address.isEmpty)
                    if let message { Text(message).font(.caption) }
                }
                if printer.relayURL != nil {
                    Button("Remove remote connection", role: .destructive) {
                        do { try store.setRelay(for: printer.id, address: nil, token: nil); dismiss() }
                        catch { message = error.localizedDescription }
                    }
                }
            }
            .navigationTitle("U1 remote access")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Save") {
                        do {
                            try store.setRelay(for: printer.id, address: address, token: savedOrNewToken())
                            dismiss()
                        } catch { message = error.localizedDescription }
                    }.disabled(testing || address.isEmpty)
                }
            }
            .onAppear { address = printer.relayURL ?? "" }
            .fileImporter(isPresented: $importing, allowedContentTypes: [.json]) { result in
                do {
                    let pair = try RelayPairing.read(result.get())
                    address = pair.relayURL; token = pair.token
                    message = "Pairing imported for \(pair.name). Test the connection before saving."
                } catch { message = error.localizedDescription }
            }
        }
    }

    private func savedOrNewToken() throws -> String {
        if !token.isEmpty { return token }
        guard let saved = try RelayConnection.token(printer.id) else { throw SlicerFailure("Import your Mac’s pairing file first.") }
        return saved
    }
}
