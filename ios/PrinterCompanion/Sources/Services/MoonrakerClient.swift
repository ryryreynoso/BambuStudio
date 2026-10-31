import Foundation

/// Read-only telemetry client. Upload and start requests live in MoonrakerPrintDelivery.
struct MoonrakerClient {
    let host: String
    var session: URLSession = .shared

    /// `exception_manager` is Snapmaker's list of active printer errors; Moonraker leaves out objects
    /// a printer does not have, so plain Klipper printers still answer.
    static let queriedObjects = ["print_stats", "display_status", "heater_bed", "extruder", "extruder1", "extruder2", "extruder3", "exception_manager"]

    var baseURL: String {
        var base = host.trimmingCharacters(in: .whitespaces)
        if !base.hasPrefix("http://") && !base.hasPrefix("https://") { base = "http://" + base }
        while base.hasSuffix("/") { base.removeLast() }
        return base
    }

    var statusURL: URL? {
        guard let components = URLComponents(string: baseURL), components.user == nil, components.password == nil,
              components.query == nil, components.fragment == nil else { return nil }
        return URL(string: baseURL + "/printer/objects/query?" + Self.queriedObjects.joined(separator: "&"))
    }

    /// The Snapmaker U1 saves a camera picture here every few seconds while its camera monitor runs
    /// (during prints, for its AI checks). Snapmaker's own app reads the same file.
    var cameraURL: URL? {
        URL(string: baseURL + "/server/files/camera/monitor.jpg")
    }

    /// The latest camera picture, or nil when the printer has none (404) or no camera root.
    func fetchCameraImage() async throws -> Data? {
        guard let url = cameraURL else { throw URLError(.badURL) }
        var request = URLRequest(url: url)
        request.timeoutInterval = 5
        request.cachePolicy = .reloadIgnoringLocalCacheData
        let (data, response) = try await session.data(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode),
              http.value(forHTTPHeaderField: "Content-Type")?.hasPrefix("image/") ?? true else { return nil }
        return data
    }

    func fetchStatus() async throws -> PrinterStatus {
        guard let url = statusURL else { throw URLError(.badURL) }
        var request = URLRequest(url: url)
        request.timeoutInterval = 5
        let (data, response) = try await session.data(for: request)
        if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
            throw MoonrakerError.http(http.statusCode)
        }
        var status = PrinterStatus()
        try status.applyMoonraker(data)
        return status
    }
}
