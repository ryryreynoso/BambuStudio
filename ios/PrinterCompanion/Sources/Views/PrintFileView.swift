// SPDX-License-Identifier: AGPL-3.0-only
import SwiftUI

struct PrintFileView: View {
    let file: SlicedFile
    @EnvironmentObject private var store: PrinterStore
    @Environment(\.dismiss) private var dismiss
    @State private var selected: UUID?
    @State private var options = PrintOptions()
    @State private var useAMS = false
    @State private var tray = 0
    @State private var confirming = false
    @State private var busy = false
    @State private var attempted = false
    @State private var message = ""
    @State private var error: String?
    /// One AMS slot per filament in a multi-color job.
    @State private var trays: [Int] = []
    private var isMultiFilament: Bool { file.filamentCount > 1 }
    private var printers: [PrinterConfig] { store.printers.filter { PrintContract.matches($0, file) } }

    var body: some View {
        NavigationStack {
            Form {
                Section("Destination") {
                    if printers.isEmpty { Text("Add the matching printer on the Printers tab first.") }
                    Picker("Printer", selection: $selected) {
                        Text("Choose printer").tag(UUID?.none)
                        ForEach(printers) { Text($0.name).tag(Optional($0.id)) }
                    }
                    Text(file.machine).font(.caption)
                    if file.printer == nil {
                        Text("This app has no print flow for this printer. Share the sliced file instead.").font(.caption)
                        ShareLink("Share sliced file", item: file.url)
                    } else if file.printer == .p2s {
                        Text("P2S requires LAN Only and Developer Mode for third-party print commands. Use the current LAN access code. A cloud login does not authorize this build.").font(.caption)
                        Toggle("Use AMS", isOn: $useAMS)
                        if useAMS && isMultiFilament {
                            ForEach(0..<file.filamentCount, id: \.self) { index in
                                Picker("Filament \(index + 1)", selection: Binding(
                                    get: { index < trays.count ? trays[index] : index },
                                    set: { value in
                                        if trays.count != file.filamentCount { trays = Array(0..<file.filamentCount) }
                                        trays[index] = value
                                    }
                                )) {
                                    ForEach(0..<16) { Text("AMS \($0 / 4 + 1), slot \($0 % 4 + 1)").tag($0) }
                                }
                            }
                            Text("Choose the tray loaded with each filament. Confirm the materials at the printer.").font(.caption)
                        } else if useAMS {
                            Picker("AMS tray", selection: $tray) {
                                ForEach(0..<16) { Text("AMS \($0 / 4 + 1), slot \($0 % 4 + 1)").tag($0) }
                            }
                            Text("Choose the tray loaded with the profile’s PLA. Tray mapping is manual; confirm the material at the printer.").font(.caption)
                        } else { Text("Uses the external spool.").font(.caption) }
                    } else if isMultiFilament {
                        Text("Filament 1 prints on toolhead 1, filament 2 on toolhead 2, and so on. Confirm each toolhead holds the matching filament.").font(.caption)
                    } else {
                        Picker("Toolhead", selection: $options.toolhead) {
                            ForEach(0..<4) { Text("Toolhead \($0 + 1)").tag($0) }
                        }
                    }
                }
                Section("Print preparation") {
                    Toggle("Bed leveling", isOn: $options.bedLeveling)
                    Toggle("Flow calibration", isOn: $options.flowCalibration)
                    Toggle("Timelapse", isOn: $options.timelapse)
                    Text("Confirm the plate is clear and that the nozzle and filament match this slice.").font(.caption)
                }
                Section {
                    Button("Send and start print") { confirming = true }
                        .disabled(selected == nil || busy || attempted || file.printer == nil)
                    if busy { ProgressView() }
                    if !message.isEmpty { Text(message) }
                    if let error { Text(error).foregroundStyle(.red).textSelection(.enabled) }
                    if attempted && !busy {
                        Text("Check the printer’s status on the Printers tab. To send another job, close this screen and choose Print again.").font(.caption)
                    }
                }
            }
            .disabled(busy)
            .navigationTitle("Print sliced file")
            .toolbar { Button("Close") { dismiss() }.disabled(busy) }
            .interactiveDismissDisabled(busy)
            .confirmationDialog("Start a physical print on the selected printer?", isPresented: $confirming, titleVisibility: .visible) {
                Button("Start print", role: .destructive, action: start)
            } message: { Text("This uploads the sliced file and sends a start command. Confirm the printer is ready.") }
        }
    }
    private func start() {
        guard let selected, let printer = printers.first(where: { $0.id == selected }), !busy, !attempted else { return }
        options.amsTray = useAMS ? tray : nil
        options.filamentCount = file.filamentCount
        options.plateNumber = file.plateNumber
        if useAMS && isMultiFilament {
            options.amsMapping = trays.count == file.filamentCount ? trays : Array(0..<file.filamentCount)
        }
        busy = true; attempted = true; error = nil
        UIApplication.shared.isIdleTimerDisabled = true
        Task { @MainActor in
            defer { busy = false; UIApplication.shared.isIdleTimerDisabled = false }
            do {
                try await PrintDelivery().start(file: file, printer: printer, options: options) { text in
                    Task { @MainActor in message = text }
                }
            } catch { self.error = error.localizedDescription }
        }
    }
}
