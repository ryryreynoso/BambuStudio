import Foundation

/// What the app shows for one printer. Everything is read-only: the app never sends commands that
/// move, heat or start anything.
struct PrinterStatus: Equatable {
    struct Heater: Equatable, Identifiable {
        var id: String { name }
        var name: String
        var current: Double
        var target: Double
    }

    var state: String = "Unknown"
    var jobName: String?
    /// 0...1
    var progress: Double?
    var remainingMinutes: Int?
    var layer: Int?
    var totalLayers: Int?
    var heaters: [Heater] = []
    var message: String?
    /// Active printer errors, when the printer reports them (Snapmaker's `exception_manager`).
    /// nil means the printer has no error list; an empty array means it reports none.
    var errors: [String]?
}

extension PrinterStatus {
    /// Merges one Bambu `device/<serial>/report` payload. Printers send a full report after `pushall`
    /// and partial ones afterwards, so only fields that are present are updated.
    mutating func mergeBambuReport(_ payload: Data) {
        guard let root = try? JSONSerialization.jsonObject(with: payload) as? [String: Any],
              let print = root["print"] as? [String: Any] else { return }
        if let value = print["gcode_state"] as? String { state = Self.bambuStateName(value) }
        if let value = print["subtask_name"] as? String, !value.isEmpty { jobName = value }
        if let value = Self.number(print["mc_percent"]) { progress = max(0, min(1, value / 100)) }
        if let value = Self.number(print["mc_remaining_time"]) { remainingMinutes = Int(value) }
        if let value = Self.number(print["layer_num"]) { layer = Int(value) }
        if let value = Self.number(print["total_layer_num"]) { totalLayers = Int(value) }
        updateHeater("Nozzle", current: Self.number(print["nozzle_temper"]), target: Self.number(print["nozzle_target_temper"]))
        updateHeater("Bed", current: Self.number(print["bed_temper"]), target: Self.number(print["bed_target_temper"]))
        if let value = Self.number(print["chamber_temper"]) {
            updateHeater("Chamber", current: value, target: nil)
        }
    }

    /// Replaces the status with one Moonraker `printer/objects/query` result.
    mutating func applyMoonraker(_ payload: Data) throws {
        guard let root = try JSONSerialization.jsonObject(with: payload) as? [String: Any],
              let result = root["result"] as? [String: Any],
              let objects = result["status"] as? [String: Any] else {
            throw MoonrakerError.unexpectedReply
        }
        var updated = PrinterStatus()
        if let stats = objects["print_stats"] as? [String: Any] {
            if let value = stats["state"] as? String { updated.state = value.capitalized }
            if let value = stats["filename"] as? String, !value.isEmpty { updated.jobName = value }
            if let value = stats["message"] as? String, !value.isEmpty { updated.message = value }
            if let info = stats["info"] as? [String: Any] {
                if let value = Self.number(info["current_layer"]) { updated.layer = Int(value) }
                if let value = Self.number(info["total_layer"]) { updated.totalLayers = Int(value) }
            }
            if let duration = Self.number(stats["print_duration"]),
               let display = objects["display_status"] as? [String: Any],
               let fraction = Self.number(display["progress"]), fraction > 0.001 {
                updated.remainingMinutes = Int(duration / fraction * (1 - fraction) / 60)
            }
        }
        if let display = objects["display_status"] as? [String: Any], let value = Self.number(display["progress"]) {
            updated.progress = max(0, min(1, value))
        }
        for (index, key) in ["extruder", "extruder1", "extruder2", "extruder3"].enumerated() {
            if let heater = objects[key] as? [String: Any] {
                updated.updateHeater("Tool \(index + 1)", current: Self.number(heater["temperature"]), target: Self.number(heater["target"]))
            }
        }
        if let bed = objects["heater_bed"] as? [String: Any] {
            updated.updateHeater("Bed", current: Self.number(bed["temperature"]), target: Self.number(bed["target"]))
        }
        if let manager = objects["exception_manager"] as? [String: Any] {
            updated.errors = (manager["exceptions"] as? [Any] ?? []).map(Self.describeException)
        }
        self = updated
    }

    /// Snapmaker does not document its exception entries, so this shows whatever text an entry has,
    /// with its code and level when present.
    static func describeException(_ entry: Any) -> String {
        guard let fields = entry as? [String: Any] else { return "\(entry)" }
        let text = ["message", "msg", "description", "name"].lazy.compactMap { fields[$0] as? String }.first { !$0.isEmpty }
        let details = ["code", "id", "level", "module", "index"].compactMap { key -> String? in
            guard let value = fields[key], !(value is NSNull) else { return nil }
            return "\(key) \(value)"
        }
        switch (text, details.isEmpty) {
        case let (text?, true): return text
        case let (text?, false): return "\(text) (\(details.joined(separator: ", ")))"
        case (nil, false): return "Error " + details.joined(separator: ", ")
        case (nil, true): return "\(fields)"
        }
    }

    mutating func updateHeater(_ name: String, current: Double?, target: Double?) {
        guard current != nil || target != nil else { return }
        if let index = heaters.firstIndex(where: { $0.name == name }) {
            if let current { heaters[index].current = current }
            if let target { heaters[index].target = target }
        } else {
            heaters.append(Heater(name: name, current: current ?? 0, target: target ?? 0))
        }
    }

    static func number(_ value: Any?) -> Double? {
        switch value {
        case let value as Double: return value
        case let value as Int: return Double(value)
        case let value as NSNumber: return value.doubleValue
        case let value as String: return Double(value)
        default: return nil
        }
    }

    static func bambuStateName(_ state: String) -> String {
        switch state.uppercased() {
        case "IDLE": return "Idle"
        case "PREPARE": return "Preparing"
        case "RUNNING": return "Printing"
        case "PAUSE": return "Paused"
        case "FINISH": return "Finished"
        case "FAILED": return "Failed"
        case "SLICING": return "Slicing"
        default: return state.capitalized
        }
    }
}

enum MoonrakerError: Error, LocalizedError {
    case unexpectedReply
    case http(Int)

    var errorDescription: String? {
        switch self {
        case .unexpectedReply: return "The printer did not answer like Moonraker."
        case .http(let status): return "The printer answered with HTTP \(status)."
        }
    }
}
