// SPDX-License-Identifier: AGPL-3.0-only
import Foundation
import OSLog
import PrinterFTPS

struct PrintOptions {
    var bedLeveling = true
    var flowCalibration = false
    var timelapse = false
    /// nil uses the external spool; otherwise the firmware's 0-based AMS tray identifier.
    var amsTray: Int?
    var toolhead = 0
    /// Multi-color P2S jobs: the AMS tray for each filament, in filament order.
    var amsMapping: [Int]?
    var filamentCount = 1
    /// A sliced 3MF holds its G-code under the plate it came from.
    var plateNumber = 1
}

enum JobAcceptance: Equatable { case pending, accepted, rejected }

enum PrintContract {
    static func matches(_ printer: PrinterConfig, _ file: SlicedFile) -> Bool {
        (printer.kind == .bambuLAN && file.printer == .p2s) || (printer.kind == .moonraker && file.printer == .u1)
    }

    static func bambuAcceptance(_ report: [String: Any], sequence: String, remote: String) -> JobAcceptance {
        guard let print = report["print"] as? [String: Any] else { return .pending }
        let id = (print["sequence_id"] as? String) ?? (print["sequence_id"] as? NSNumber)?.stringValue
        if print["command"] as? String == "project_file", id == sequence {
            let result = print["result"] as? String
            let reason = (print["reason"] as? String) ?? (print["reason"] as? NSNumber)?.stringValue ?? ""
            if ["fail", "failed", "error"].contains(result ?? "") || !["", "0", "success"].contains(reason) { return .rejected }
            if result == "success" { return .accepted }
        }
        if [remote, "/" + remote].contains(print["gcode_file"] as? String ?? "") {
            if ["PREPARE", "RUNNING", "PAUSE"].contains(print["gcode_state"] as? String ?? "") { return .accepted }
            if print["gcode_state"] as? String == "FAILED" { return .rejected }
        }
        return .pending
    }

    static func bambuCommand(remote: String, sequence: String, options: PrintOptions) -> [String: Any] {
        ["print": ["sequence_id": sequence, "command": "project_file", "param": "Metadata/plate_\(options.plateNumber).gcode",
                   "project_id": "0", "profile_id": "0", "task_id": "0", "subtask_id": "0",
                   "subtask_name": remote, "file": remote, "url": "ftp:///" + remote, "md5": "from_sd_card",
                   "bed_type": "auto", "bed_leveling": options.bedLeveling, "flow_cali": options.flowCalibration,
                   "vibration_cali": true, "layer_inspect": false, "timelapse": options.timelapse,
                   "use_ams": options.amsTray != nil, "ams_mapping": options.amsMapping ?? [options.amsTray ?? -1], "ams_mapping2": [],
                   "auto_bed_leveling": options.bedLeveling ? 1 : 0, "extrude_cali_flag": options.flowCalibration ? 1 : 0]]
    }
}

private func withDeadline<T>(_ seconds: UInt64, operation: @escaping () async throws -> T) async throws -> T {
    try await withThrowingTaskGroup(of: T.self) { group in
        group.addTask { try await operation() }
        group.addTask {
            try await Task.sleep(nanoseconds: seconds * 1_000_000_000)
            throw SlicerFailure("The printer did not answer in time.")
        }
        defer { group.cancelAll() }
        return try await group.next()!
    }
}

private final class BambuJobConnection {
    private var client: BambuLANClient!
    private let events: AsyncStream<BambuLANClient.Event>
    private let continuation: AsyncStream<BambuLANClient.Event>.Continuation

    init(printer: PrinterConfig, code: String) {
        var continuation: AsyncStream<BambuLANClient.Event>.Continuation!
        events = AsyncStream(bufferingPolicy: .bufferingNewest(128)) { continuation = $0 }
        self.continuation = continuation
        client = BambuLANClient(host: printer.host, serial: printer.serial, accessCode: code) { continuation.yield($0) }
    }
    func start() { client.start() }
    func stop() { client.stop(); continuation.finish() }
    deinit { stop() }
    func send(_ value: [String: Any]) throws { try client.publishRequest(value) }

    func report() async throws -> [String: Any] {
        let events = self.events
        return try await withDeadline(15) {
            for await event in events {
                switch event {
                case .failed(let error): throw SlicerFailure(error)
                case .report(let data):
                    if let root = try JSONSerialization.jsonObject(with: data) as? [String: Any] { return root }
                case .connected: break
                }
            }
            throw SlicerFailure("The printer connection closed.")
        }
    }
    func requireIdle() async throws {
        let deadline = Date().addingTimeInterval(15)
        while Date() < deadline {
            let root = try await report()
            guard let print = root["print"] as? [String: Any], let state = print["gcode_state"] as? String else { continue }
            if ["IDLE", "FINISH", "FAILED"].contains(state) { return }
            if ["RUNNING", "PREPARE", "PAUSE"].contains(state) { throw SlicerFailure("The printer is busy. No print command was sent.") }
        }
        throw SlicerFailure("Cannot confirm that the printer is idle. No print command was sent.")
    }
}

struct PrintDelivery {
    func start(file: SlicedFile, printer: PrinterConfig, options: PrintOptions,
               update: @escaping @Sendable (String) -> Void) async throws {
        try printer.validate()
        guard PrintContract.matches(printer, file) else { throw SlicerFailure("This file was sliced for a different printer. Slice it again with the selected printer profile.") }
        try Task.checkCancellation()
        let log = Logger(subsystem: Bundle.main.bundleIdentifier ?? "PrinterCompanion", category: "PrintDelivery")
        let trackedUpdate: @Sendable (String) -> Void = { phase in
            log.info("\(phase, privacy: .public)") // Constant phase labels only; no credentials or job paths.
            update(phase)
        }
        do {
            if printer.kind == .bambuLAN { try await bambu(file: file, printer: printer, options: options, update: trackedUpdate) }
            else {
                let (client, route) = try await RelayConnection.select(printer)
                defer { if route == "Home relay" { client.session.finishTasksAndInvalidate() } }
                trackedUpdate("Connected through " + route)
                // Select once before upload; never switch transport after an ambiguous start.
                try await MoonrakerPrintDelivery(host: client.host, session: client.session).start(file: file.url, options: options, update: trackedUpdate)
            }
        } catch {
            log.error("Delivery failed; detailed cause is shown in the app. Error code: \((error as NSError).code)")
            throw error
        }
    }

    private func bambu(file: SlicedFile, printer: PrinterConfig, options: PrintOptions,
                       update: @escaping @Sendable (String) -> Void) async throws {
        guard let code = Keychain.accessCode(for: printer.id), !code.isEmpty else { throw SlicerFailure("Pair this printer with its LAN access code first.") }
        guard printer.serial.count >= 6 else { throw SlicerFailure("Enter the printer serial number before sending a job.") }
        guard options.amsTray.map({ (0...15).contains($0) }) ?? true else { throw SlicerFailure("Choose a valid AMS tray.") }
        update("Checking printer connection and idle state")
        let before = BambuJobConnection(printer: printer, code: code)
        before.start()
        do { try await before.requireIdle() } catch { before.stop(); throw error }
        before.stop()
        let remote = "phone-\(UUID().uuidString).gcode.3mf"
        guard let ca = Bundle.main.url(forResource: "printer", withExtension: "cer") else { throw SlicerFailure("Printer certificate authorities are missing from this build.") }
        update("Uploading over verified FTPS — no print command sent yet")
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            DispatchQueue.global(qos: .userInitiated).async {
                var error = [CChar](repeating: 0, count: 512)
                let result = printer_ftps_upload(file.url.path, printer.host, printer.serial, code, remote, ca.path, &error, UInt(error.count))
                if result == 0 { continuation.resume() }
                else { continuation.resume(throwing: SlicerFailure("Upload failed: \(String(cString: error)). No print command was sent.")) }
            }
        }
        try Task.checkCancellation()
        let after = BambuJobConnection(printer: printer, code: code)
        after.start()
        defer { after.stop() }
        try await after.requireIdle()
        update("Uploaded; sending one start command and waiting for printer acceptance")
        let sequence = String(UInt32.random(in: 1...UInt32.max))
        // No automatic retry after this point: a lost response can mean the printer already started.
        do {
            try after.send(PrintContract.bambuCommand(remote: remote, sequence: sequence, options: options))
            let deadline = Date().addingTimeInterval(30)
            while Date() < deadline {
                switch PrintContract.bambuAcceptance(try await after.report(), sequence: sequence, remote: remote) {
                case .accepted: update("Printer accepted the job"); return
                case .rejected: throw SlicerFailure("The printer rejected the start command. Check printer errors and LAN Only / Developer Mode.")
                case .pending: break
                }
            }
        } catch {
            throw SlicerFailure("Uploaded; start could not be confirmed: \(error.localizedDescription) Check the printer before retrying to avoid a duplicate print.")
        }
        throw SlicerFailure("Uploaded; printer acceptance is unknown. Check the printer before retrying to avoid a duplicate print.")
    }
}
