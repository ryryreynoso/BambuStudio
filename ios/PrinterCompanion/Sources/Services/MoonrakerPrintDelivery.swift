// SPDX-License-Identifier: AGPL-3.0-only
import Foundation

/// Snapmaker U1 transport. Upload and start are deliberately separate, with no implicit start on upload.
struct MoonrakerPrintDelivery {
    let host: String
    var session: URLSession = .shared
    private var client: MoonrakerClient { MoonrakerClient(host: host, session: session) }

    static func startParameters(remote: String, options: PrintOptions) -> [String: Any] {
        ["type": "gcode", "path": remote, "print_plate": 1,
         "options": ["bed_level": options.bedLeveling ? 1 : 0,
                     "flow_calibrate": options.flowCalibration ? 1 : 0,
                     "time_lapse_camera": options.timelapse ? 1 : 0,
                     "map_table": options.filamentCount > 1 ? (0..<options.filamentCount).map { [$0, $0] } : [[0, options.toolhead]]],
         "extra_info": ["print_task_id": ""]]
    }

    private func requireIdle() async throws {
        let state = try await client.fetchStatus().state.lowercased()
        guard ["standby", "complete", "cancelled", "error", "idle"].contains(state) else {
            throw SlicerFailure("The printer is busy or its idle state could not be confirmed. No print command was sent.")
        }
    }

    func start(file: URL, options: PrintOptions, update: @escaping @Sendable (String) -> Void) async throws {
        guard (0...3).contains(options.toolhead) else { throw SlicerFailure("Select toolhead 1–4 for the U1.") }
        update("Checking U1 connection and idle state")
        try await requireIdle()
        let remote = "phone-\(UUID().uuidString).gcode"
        let boundary = "PrinterCompanion-\(UUID().uuidString)"
        let body = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: body) }
        try Self.multipart(file: file, remote: remote, boundary: boundary, output: body)
        guard let url = URL(string: client.baseURL + "/server/files/upload") else { throw URLError(.badURL) }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.timeoutInterval = 600
        request.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        update("Uploading G-code — no print command sent yet")
        let (data, response) = try await session.upload(for: request, fromFile: body)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw SlicerFailure("U1 file upload failed. No print command was sent.")
        }
        guard let reply = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let item = reply["item"] as? [String: Any], item["path"] as? String == remote,
              item["root"] as? String == "gcodes" else {
            throw SlicerFailure("The U1 did not confirm the uploaded filename. No print command was sent.")
        }
        try Task.checkCancellation()
        try await requireIdle() // Recheck: another app could have started a job during upload.
        guard var components = URLComponents(string: client.baseURL + "/websocket") else { throw URLError(.badURL) }
        components.scheme = components.scheme == "https" ? "wss" : "ws"
        guard let socketURL = components.url else { throw URLError(.badURL) }
        let socket = session.webSocketTask(with: socketURL)
        let timeout = DispatchWorkItem { socket.cancel(with: .goingAway, reason: nil) }
        DispatchQueue.global().asyncAfter(deadline: .now() + 30, execute: timeout)
        defer { timeout.cancel(); socket.cancel(with: .normalClosure, reason: nil) }
        socket.resume()
        let id = UUID().uuidString
        let message: [String: Any] = ["jsonrpc": "2.0", "id": id, "method": "server.files.start_local_print",
                                      "params": Self.startParameters(remote: remote, options: options)]
        update("Uploaded; waiting for U1 start response")
        // Exactly one start request. A timeout must never cause an automatic retry.
        do {
            try await socket.send(.data(JSONSerialization.data(withJSONObject: message)))
            while true {
                let incoming = try await socket.receive()
                let data: Data
                switch incoming { case .data(let value): data = value; case .string(let value): data = Data(value.utf8); @unknown default: continue }
                guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any], root["id"] as? String == id else { continue }
                if root["error"] != nil { throw SlicerFailure("The U1 rejected the start request. Check printer errors and selected toolhead.") }
                guard root["result"] != nil else { throw SlicerFailure("The U1 sent an invalid start response.") }
                break
            }
            update("U1 received the request; checking the actual job")
            let deadline = Date().addingTimeInterval(45)
            while Date() < deadline {
                let status = try await client.fetchStatus()
                if status.jobName == remote {
                    if ["printing", "paused"].contains(status.state.lowercased()) { update("U1 accepted the job"); return }
                    if status.state.lowercased() == "error" { throw SlicerFailure("The U1 stopped the job with an error.") }
                }
                try await Task.sleep(nanoseconds: 2_000_000_000)
            }
        } catch {
            throw SlicerFailure("Uploaded; start could not be confirmed: \(error.localizedDescription) Check the U1 before retrying to avoid a duplicate print.")
        }
        throw SlicerFailure("Uploaded; the U1 has not reported this job as printing. Check the printer before retrying.")
    }

    /// Copies the file in bounded chunks, avoiding a second full-sized G-code allocation in memory.
    static func multipart(file: URL, remote: String, boundary: String, output: URL) throws {
        guard remote.range(of: #"^[A-Za-z0-9._-]+$"#, options: .regularExpression) != nil else { throw SlicerFailure("Invalid upload filename.") }
        FileManager.default.createFile(atPath: output.path, contents: nil)
        let destination = try FileHandle(forWritingTo: output)
        defer { try? destination.close() }
        func write(_ value: String) throws { try destination.write(contentsOf: Data(value.utf8)) }
        for (name, value) in [("root", "gcodes"), ("path", ""), ("print", "false")] {
            try write("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"\r\n\r\n\(value)\r\n")
        }
        try write("--\(boundary)\r\nContent-Disposition: form-data; name=\"file\"; filename=\"\(remote)\"\r\nContent-Type: application/octet-stream\r\n\r\n")
        let source = try FileHandle(forReadingFrom: file)
        defer { try? source.close() }
        while let data = try source.read(upToCount: 65536), !data.isEmpty { try destination.write(contentsOf: data) }
        try write("\r\n--\(boundary)--\r\n")
    }
}
