import SwiftUI

struct PrinterDetailView: View {
    @StateObject var monitor: PrinterMonitor

    var body: some View {
        List {
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
        .onAppear { monitor.start() }
        .onDisappear { monitor.stop() }
    }

    static func format(minutes: Int) -> String {
        minutes >= 60 ? "\(minutes / 60) h \(minutes % 60) min" : "\(minutes) min"
    }
}
