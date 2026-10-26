import Foundation

/// Reads a Klipper printer's status from Moonraker over the local network. Only GET requests to
/// `printer/objects/query` are made.
struct MoonrakerClient {
    let host: String
    var session: URLSession = .shared

    static let queriedObjects = ["print_stats", "display_status", "heater_bed", "extruder", "extruder1", "extruder2", "extruder3"]

    var statusURL: URL? {
        var base = host.trimmingCharacters(in: .whitespaces)
        if !base.hasPrefix("http://") && !base.hasPrefix("https://") { base = "http://" + base }
        while base.hasSuffix("/") { base.removeLast() }
        return URL(string: base + "/printer/objects/query?" + Self.queriedObjects.joined(separator: "&"))
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
