// SPDX-License-Identifier: AGPL-3.0-only
import Foundation
import OrcaStudio
import OrcaBridgeCore

enum SlicePrinter: String, CaseIterable, Identifiable {
    case p2s = "Bambu Lab P2S 0.4 nozzle"
    case u1 = "Snapmaker U1 (0.4 nozzle)"
    var id: String { rawValue }
    var fileExtension: String { self == .p2s ? "3mf" : "gcode" }

    /// Any nozzle variant of the printer; nil for a printer this app cannot send to.
    init?(machinePreset: String) {
        if machinePreset.hasPrefix("Bambu Lab P2S") { self = .p2s }
        else if machinePreset.hasPrefix("Snapmaker U1") { self = .u1 }
        else { return nil }
    }
}

struct SliceRequest {
    let model: URL
    let printer: SlicePrinter
    let layerHeight: Double
    let infill: Int
    let supports: Bool

    func validate() throws {
        guard ["stl", "obj", "3mf", "amf"].contains(model.pathExtension.lowercased()) else {
            throw SlicerFailure("Import an STL, OBJ, AMF or model 3MF file.")
        }
        guard layerHeight.isFinite, (0.08...0.28).contains(layerHeight), (0...100).contains(infill) else {
            throw SlicerFailure("Layer height must be 0.08–0.28 mm and infill 0–100% for the 0.4 mm nozzle.")
        }
    }
}

struct SlicedFile: Identifiable {
    let url: URL
    /// nil when the slice is for a printer this app has no print flow for: export only.
    let printer: SlicePrinter?
    let machine: String
    let filamentCount: Int
    var plateNumber = 1
    let estimatedSeconds: Double
    let warnings: [String]
    var id: URL { url }

    init(url: URL, printer: SlicePrinter, estimatedSeconds: Double, warnings: [String]) {
        self.url = url; self.printer = printer; machine = printer.rawValue
        filamentCount = 1; self.estimatedSeconds = estimatedSeconds; self.warnings = warnings
    }

    init(_ job: StudioPrintJob) {
        url = job.file; printer = SlicePrinter(machinePreset: job.machine); machine = job.machine
        filamentCount = job.filamentCount; plateNumber = job.plateNumber; estimatedSeconds = job.estimatedSeconds; warnings = job.warnings
    }
}

struct SlicerFailure: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

/// All native engine work is serialized and kept off the UI thread. No cloud service or Mac is used.
final class MobileSlicer {
    private static let queue = DispatchQueue(label: "io.printercompanion.slicer", qos: .userInitiated)

    func slice(_ request: SliceRequest, progress: @escaping @Sendable (Double, String) -> Void) async throws -> SlicedFile {
        try request.validate()
        return try await withCheckedThrowingContinuation { continuation in
            Self.queue.async {
                do { continuation.resume(returning: try Self.run(request, progress: progress)) }
                catch { continuation.resume(throwing: error) }
            }
        }
    }

    private static func run(_ request: SliceRequest, progress: @escaping @Sendable (Double, String) -> Void) throws -> SlicedFile {
        guard orca_bridge_abi_version() == 42 else { throw SlicerFailure("Slicing engine version mismatch. Reinstall this build.") }
        let fm = FileManager.default
        let data = try fm.url(for: .applicationSupportDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("OrcaEngine-0.19.0", isDirectory: true)
        try fm.createDirectory(at: data, withIntermediateDirectories: true)
        let system = data.appendingPathComponent("system", isDirectory: true)
        if !fm.fileExists(atPath: system.path) {
            guard let bundled = Bundle.main.url(forResource: "system", withExtension: nil) else { throw SlicerFailure("Printer profiles are missing from this build.") }
            let staging = data.appendingPathComponent("system-staging-\(UUID().uuidString)")
            try fm.copyItem(at: bundled, to: staging)
            try fm.moveItem(at: staging, to: system)
        }
        progress(0, "Loading profiles")
        var presets: OpaquePointer?
        try check(orca_presets_new(data.path, &presets))
        defer { orca_presets_free(presets) }
        progress(0, "Selecting printer profile")
        try check(orca_presets_select_machine(presets, request.printer.rawValue))
        try check(orca_presets_set_value(presets, 2, "layer_height", String(request.layerHeight)))
        try check(orca_presets_set_value(presets, 2, "sparse_infill_density", "\(request.infill)%"))
        try check(orca_presets_set_value(presets, 2, "enable_support", request.supports ? "1" : "0"))
        var minX = 0.0, minY = 0.0, maxX = 0.0, maxY = 0.0
        try check(orca_presets_print_volume(presets, &minX, &minY, &maxX, &maxY))
        progress(0, "Creating plate")
        var plater: OpaquePointer?
        try check(orca_plater_new(&plater))
        defer { orca_plater_free(plater) }
        var snapshot = orca_scene_snapshot()
        defer { orca_scene_snapshot_dispose(&snapshot) }
        try check(orca_plater_set_bed(plater, minX, minY, maxX - minX, maxY - minY, request.printer == .p2s ? 256 : 270, &snapshot))
        var object: UInt64 = 0, count: UInt32 = 0, revision: UInt64 = 0
        progress(0, "Importing model")
        try check(orca_plater_import_model(plater, request.model.path, (minX + maxX) / 2, (minY + maxY) / 2, &object, &count, &revision))
        // Import already centers the group and drops it onto the bed. The optional
        // upstream auto-packer crashes on iOS for this profile/model combination.
        // Preserve imported relative placements and validate the complete model instead.
        progress(0, "Checking model fits the plate")
        var imported = orca_scene_snapshot()
        defer { orca_scene_snapshot_dispose(&imported) }
        try check(orca_plater_snapshot(plater, &imported))
        var printable = 0
        if let objects = imported.objects {
            for objectIndex in 0..<Int(imported.object_count) {
                let object = objects[objectIndex]
                guard let instances = object.instances else { continue }
                for instanceIndex in 0..<Int(object.instance_count) {
                    let instance = instances[instanceIndex]
                    guard instance.printable != 0 else { continue }
                    var matrixTuple = instance.matrix
                    let matrix = withUnsafeBytes(of: &matrixTuple) { Array($0.bindMemory(to: Double.self)) }
                    guard PrintVolume.contains(
                        minimum: [Double(object.bbox_min.0), Double(object.bbox_min.1), Double(object.bbox_min.2)],
                        maximum: [Double(object.bbox_max.0), Double(object.bbox_max.1), Double(object.bbox_max.2)],
                        matrix: matrix, minX: minX, minY: minY, maxX: maxX, maxY: maxY,
                        height: request.printer == .p2s ? 256 : 270) else {
                        throw SlicerFailure("The model extends outside the printable volume. Resize or split it before slicing.")
                    }
                    printable += 1
                }
            }
        }
        guard printable > 0 else { throw SlicerFailure("The imported model has no printable instances.") }
        progress(0, "Preparing output")
        let directory = try fm.url(for: .documentDirectory, in: .userDomainMask, appropriateFor: nil, create: true)
            .appendingPathComponent("Slices", isDirectory: true)
        try fm.createDirectory(at: directory, withIntermediateDirectories: true)
        let output = directory.appendingPathComponent("\(UUID().uuidString).\(request.printer.fileExtension)")
        let context = ProgressContext(progress)
        let pointer = Unmanaged.passRetained(context).toOpaque()
        defer { Unmanaged<ProgressContext>.fromOpaque(pointer).release() }
        var report = orca_slice_report()
        defer { orca_slice_report_dispose(&report) }
        let callback: orca_progress_fn = { percent, message, raw in
            guard let raw else { return }
            Unmanaged<ProgressContext>.fromOpaque(raw).takeUnretainedValue().callback(Double(percent) / 100, message.map { String(cString: $0) } ?? "Slicing")
        }
        do {
        progress(0, "Slicing model")
            if request.printer == .p2s {
                try check(orca_plater_export_gcode_3mf_plate(plater, presets, 0, output.path, callback, pointer, nil, &report))
            } else {
                try check(orca_plater_slice_to_gcode_with_presets(plater, presets, output.path, callback, pointer, nil, &report))
            }
            guard (try fm.attributesOfItem(atPath: output.path)[.size] as? NSNumber)?.intValue ?? 0 > 0 else {
                throw SlicerFailure("The engine did not produce a printable file.")
            }
            var warnings: [String] = []
            if let items = report.warnings {
                for index in 0..<Int(report.warning_count) {
                    let warning = items[index]
                    warnings.append([string(warning.code), string(warning.message), string(warning.detail)].filter { !$0.isEmpty }.joined(separator: ": "))
                }
            }
            return SlicedFile(url: output, printer: request.printer, estimatedSeconds: report.time_estimate_s, warnings: warnings)
        } catch {
            try? fm.removeItem(at: output)
            throw error
        }
    }
    private final class ProgressContext {
        let callback: @Sendable (Double, String) -> Void
        init(_ callback: @escaping @Sendable (Double, String) -> Void) { self.callback = callback }
    }
    private static func string(_ value: orca_owned_string) -> String {
        guard let data = value.data else { return "" }
        return String(decoding: UnsafeBufferPointer(start: UnsafeRawPointer(data).assumingMemoryBound(to: UInt8.self), count: Int(value.length)), as: UTF8.self)
    }
    private static func check(_ value: orca_result) throws {
        var result = value
        defer { orca_result_dispose(&result) }
        guard result.code == 0 else { throw SlicerFailure("Engine error \(result.code): \(string(result.diagnostic))") }
    }
}
