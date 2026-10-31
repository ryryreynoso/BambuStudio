// SPDX-License-Identifier: AGPL-3.0-only
import OrcaBridge
import SwiftUI

/// A finished slice on its way to the host app's print flow.
public struct StudioPrintJob: Sendable {
    public let file: URL
    /// The engine's printer preset name, for example "Bambu Lab P2S 0.4 nozzle".
    public let machine: String
    public let filamentCount: Int
    /// 1-based plate the file was sliced from.
    public let plateNumber: Int
    public let estimatedSeconds: Double
    public let warnings: [String]
}

/// The Slice tab's entry point. It owns the engine session for the app's lifetime.
public struct StudioRootView: View {
    @State private var environment = AppEnvironment()
    private let onPrint: (StudioPrintJob) -> Void

    public init(onPrint: @escaping (StudioPrintJob) -> Void) {
        self.onPrint = onPrint
    }

    public var body: some View {
        RootView()
            .environment(environment)
            .task {
                environment.onPrint = onPrint
                await environment.bootstrap()
            }
    }
}

// MARK: - Shared chrome

/// One round icon inside a capsule group.
struct StudioIcon: View {
    let symbol: String
    let label: String

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 16, weight: .medium))
            .frame(width: 42, height: 42)
            .contentShape(Rectangle())
            .accessibilityLabel(label)
    }
}

/// The printer switcher: the two owned printers first, each with its nozzle sizes.
struct StudioPrinterMenu: View {
    @Environment(AppEnvironment.self) private var environment
    var onAllPrinters: () -> Void

    private var presets: PresetStore { environment.presets }

    private func family(_ prefix: String) -> [PresetEntry] {
        presets.machines.filter { $0.name.hasPrefix(prefix) }
    }

    private func nozzle(_ name: String) -> String {
        guard let match = name.range(of: #"[0-9.+]+ nozzle"#, options: .regularExpression) else { return name }
        return String(name[match]).replacingOccurrences(of: " nozzle", with: " mm nozzle")
    }

    private var title: String {
        let name = presets.selection.machine
        if name.hasPrefix("Bambu Lab P2S") { return "Bambu P2S" }
        if name.hasPrefix("Snapmaker U1") { return "Snapmaker U1" }
        return name.isEmpty ? "Loading printers…" : name
    }

    private var subtitle: String {
        let name = presets.selection.machine
        return name.isEmpty ? "" : nozzle(name)
    }

    private func section(_ title: String, prefix: String) -> some View {
        Section(title) {
            ForEach(family(prefix), id: \.name) { entry in
                Button {
                    Task {
                        await presets.select(kind: .machine, name: entry.name)
                        await presets.preferMakerPLA()
                    }
                } label: {
                    if entry.name == presets.selection.machine {
                        Label(nozzle(entry.name), systemImage: "checkmark")
                    } else {
                        Text(nozzle(entry.name))
                    }
                }
            }
        }
    }

    var body: some View {
        Menu {
            section("Bambu Lab P2S", prefix: "Bambu Lab P2S")
            section("Snapmaker U1", prefix: "Snapmaker U1")
            Button("Other printers…", action: onAllPrinters)
        } label: {
            HStack(spacing: 8) {
                Image(systemName: "printer.fill")
                    .foregroundStyle(Color.accentColor)
                VStack(alignment: .leading, spacing: 0) {
                    Text(title).font(.subheadline.weight(.semibold)).lineLimit(1)
                    Text(subtitle).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
                }
                Image(systemName: "chevron.down")
                    .font(.caption2.weight(.bold))
                    .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 14)
            .frame(height: 42)
            .background(.ultraThinMaterial, in: Capsule())
        }
        .buttonStyle(.plain)
        .disabled(presets.isSelecting || environment.document.isBusy)
    }
}

/// The floating Slice button. While slicing it shows progress and cancels on tap.
struct StudioSliceButton: View {
    @Environment(AppEnvironment.self) private var environment

    private var canSlice: Bool {
        environment.document.scene?.objects.isEmpty == false &&
            !environment.document.isBusy && !environment.presets.isSelecting
    }

    var body: some View {
        if environment.slicing.isSlicing {
            Button { environment.slicing.cancel() } label: {
                HStack(spacing: 10) {
                    ProgressView(value: Double(environment.slicing.progress?.percent ?? 0), total: 100)
                        .tint(.white)
                        .frame(width: 70)
                    Text("\(environment.slicing.progress?.percent ?? 0)%")
                        .font(.subheadline.monospacedDigit().weight(.semibold))
                    Image(systemName: "xmark")
                        .font(.caption.weight(.bold))
                }
                .foregroundStyle(.white)
                .padding(.horizontal, 18)
                .frame(height: 48)
                .background(Color.accentColor, in: Capsule())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Cancel slicing")
        } else {
            Menu {
                Button {
                    Task { await environment.slicing.run(document: environment.document, presets: environment.presets) }
                } label: { Label("Slice this plate", systemImage: "scissors") }
                if environment.document.plates.count > 1 {
                    Button {
                        Task { await environment.slicing.runAll(document: environment.document, presets: environment.presets) }
                    } label: { Label("Slice all plates", systemImage: "square.stack.3d.up.fill") }
                }
            } label: {
                Label("Slice", systemImage: "square.3.layers.3d")
                    .font(.body.weight(.semibold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 24)
                    .frame(height: 48)
                    .background(canSlice ? Color.accentColor : Color.gray.opacity(0.5), in: Capsule())
                    .shadow(color: canSlice ? Color.accentColor.opacity(0.45) : .clear, radius: 10, y: 3)
            } primaryAction: {
                Task { await environment.slicing.run(document: environment.document, presets: environment.presets) }
            }
            .disabled(!canSlice)
        }
    }
}
