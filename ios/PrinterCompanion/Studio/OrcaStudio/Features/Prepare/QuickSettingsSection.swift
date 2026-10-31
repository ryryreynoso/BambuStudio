// SPDX-License-Identifier: AGPL-3.0-only
import SwiftUI

/// The everyday controls at the top of Print settings. Every control writes the engine key
/// the full settings list shows, so the two always agree and slicing uses the value directly.
struct QuickSettingsSection: View {
    @Environment(AppEnvironment.self) private var environment
    var onEditFlushVolumes: () -> Void

    @State private var infillDraft: Double?

    private var presets: PresetStore { environment.presets }
    private var process: [String: String] { presets.editedConfigs[.process] ?? [:] }

    private func set(_ key: String, _ value: String) {
        Task { await presets.setValue(kind: .process, key: key, serializedValue: value) }
    }

    /// Layer heights the selected nozzle can print: 20% to 75% of its diameter.
    private var layerHeights: [Double] {
        let nozzle = presets.editedConfigs[.machine]?["nozzle_diameter"]
            .flatMap { Double($0.split(separator: ",").first ?? "") } ?? 0.4
        let range = (nozzle * 0.2 - 0.001)...(nozzle * 0.75 + 0.001)
        return [0.08, 0.12, 0.16, 0.20, 0.24, 0.28, 0.32, 0.40, 0.48, 0.56].filter(range.contains)
    }

    private var layerHeight: Double { Double(process["layer_height"] ?? "") ?? 0.2 }

    private var infill: Double {
        Double((process["sparse_infill_density"] ?? "15").replacingOccurrences(of: "%", with: "")) ?? 15
    }

    private var supports: Bool { process["enable_support"] == "1" }

    var body: some View {
        Section {
            VStack(alignment: .leading, spacing: 8) {
                Text("Layer height").font(.subheadline.weight(.medium))
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: 6) {
                        ForEach(layerHeights, id: \.self) { value in
                            let active = abs(value - layerHeight) < 0.001
                            Button {
                                set("layer_height", String(format: "%.2f", value))
                            } label: {
                                Text(String(format: "%.2f", value))
                                    .font(.footnote.monospacedDigit().weight(.semibold))
                                    .foregroundStyle(active ? Color.white : Color.primary)
                                    .frame(width: 54, height: 34)
                                    .background(
                                        active ? Color.accentColor : Color.secondary.opacity(0.18),
                                        in: RoundedRectangle(cornerRadius: 10)
                                    )
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
            VStack(alignment: .leading, spacing: 2) {
                LabeledContent("Infill", value: "\(Int(infillDraft ?? infill))%")
                    .font(.subheadline)
                Slider(
                    value: Binding(get: { infillDraft ?? infill }, set: { infillDraft = $0 }),
                    in: 0...100, step: 5
                ) { editing in
                    if !editing, let value = infillDraft {
                        set("sparse_infill_density", "\(Int(value))%")
                        infillDraft = nil
                    }
                }
            }
            Toggle("Supports", isOn: Binding(
                get: { supports },
                set: { set("enable_support", $0 ? "1" : "0") }
            ))
            .font(.subheadline)
            Picker("Brim", selection: Binding(
                get: { process["brim_type"] ?? "auto_brim" },
                set: { set("brim_type", $0) }
            )) {
                Text("Auto").tag("auto_brim")
                Text("Outer").tag("outer_only")
                Text("None").tag("no_brim")
            }
            .font(.subheadline)
            VStack(alignment: .leading, spacing: 6) {
                Text(filamentTitle).font(.subheadline.weight(.medium))
                ScrollView(.horizontal, showsIndicators: false) {
                    FilamentChips(
                        slots: presets.filamentSlots,
                        isBusy: presets.isSelecting,
                        onAdd: { Task { await presets.addFilament() } },
                        onRemove: { slot in Task { await presets.removeFilament(slot: slot) } },
                        onSelectPreset: { slot, name in
                            Task { await presets.selectFilamentPreset(slot: slot, name: name) }
                        },
                        onSetColour: { slot, colour in
                            Task { await presets.setFilamentColour(slot: slot, colour: colour) }
                        },
                        onEditFlushVolumes: onEditFlushVolumes,
                        filamentPresets: presets.visibleEntries(for: .filament)
                    )
                }
                if Set(presets.filamentSlots.map(\.presetName)).count > 1 {
                    Label(
                        "Different filaments: every slot prints with the first filament's temperatures and speeds. Use one material type, or the print can fail.",
                        systemImage: "exclamationmark.triangle.fill"
                    )
                    .font(.footnote)
                    .foregroundStyle(.orange)
                }
            }
        } header: {
            Text("Quick settings")
        }
    }

    private var filamentTitle: String {
        presets.selection.machine.hasPrefix("Snapmaker U1") ? "Filament · one per toolhead" : "Filament · one per AMS slot"
    }
}
