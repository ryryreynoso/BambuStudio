import SwiftUI

struct PrinterDetailView: View {
    @StateObject var monitor: PrinterMonitor
    @EnvironmentObject private var store: PrinterStore
    @State private var editingRelay = false

    var body: some View {
        List {
            if monitor.printer.kind == .bambuLAN {
                Section("Camera") { BambuCameraView(printer: monitor.printer) }
            }
            if let errors = monitor.status.errors {
                Section("Errors") {
                    if errors.isEmpty {
                        Label("No errors", systemImage: "checkmark.circle").foregroundStyle(.green)
                    }
                    ForEach(Array(errors.enumerated()), id: \.offset) { _, error in
                        Label(error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.red)
                    }
                }
            }
            if let data = monitor.cameraImage, let image = UIImage(data: data) {
                Section("Camera") {
                    Image(uiImage: image)
                        .resizable()
                        .scaledToFit()
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                    if let date = monitor.cameraUpdate {
                        Text("Picture from \(date.formatted(date: .omitted, time: .standard))")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            if !monitor.status.filaments.isEmpty {
                Section("AMS filaments") {
                    ForEach(monitor.status.filaments) { filament in
                        LabeledContent(filament.label, value: filament.material)
                    }
                }
            }
            Section("Status") {
                LabeledContent("Connection", value: monitor.connection)
                LabeledContent("State", value: monitor.status.state)
                if let job = monitor.status.jobName { LabeledContent("Job", value: job) }
                if let progress = monitor.status.progress {
                    VStack(alignment: .leading) {
                        ProgressView(value: progress)
                        Text("\(Int((progress * 100).rounded()))%").font(.caption).foregroundStyle(.secondary)
                    }
                }
                if let minutes = monitor.status.remainingMinutes {
                    LabeledContent("Remaining", value: Self.format(minutes: minutes))
                }
                if let layer = monitor.status.layer {
                    LabeledContent("Layer", value: monitor.status.totalLayers.map { "\(layer) / \($0)" } ?? "\(layer)")
                }
                if let message = monitor.status.message { Text(message).foregroundStyle(.secondary) }
            }
            if !monitor.status.heaters.isEmpty {
                Section("Temperatures") {
                    ForEach(monitor.status.heaters) { heater in
                        LabeledContent(heater.name, value: heater.target > 0
                                       ? String(format: "%.0f / %.0f °C", heater.current, heater.target)
                                       : String(format: "%.0f °C", heater.current))
                    }
                }
            }
            if let date = monitor.lastUpdate {
                Section { Text("Updated \(date.formatted(date: .omitted, time: .standard))").font(.caption) }
            }
        }
        .navigationTitle(monitor.printer.name)
        .toolbar {
            if monitor.printer.kind == .moonraker {
                Button("Remote access") { editingRelay = true }
            }
        }
        .sheet(isPresented: $editingRelay, onDismiss: {
            if let updated = store.printers.first(where: { $0.id == monitor.printer.id }), updated != monitor.printer {
                monitor.reconfigure(updated)
            }
        }) { RelaySettingsView(printer: monitor.printer) }
        .onAppear { monitor.start() }
        .onDisappear { monitor.stop() }
    }

    static func format(minutes: Int) -> String {
        minutes >= 60 ? "\(minutes / 60) h \(minutes % 60) min" : "\(minutes) min"
    }
}
