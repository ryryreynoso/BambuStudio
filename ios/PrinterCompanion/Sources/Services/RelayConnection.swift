import Foundation
import Security

final class RelayRedirectPolicy: NSObject, URLSessionTaskDelegate {
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse, newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) { completionHandler(nil) }
}

enum RelayConnection {
    static func validate(_ address: String) throws {
        guard let url = URLComponents(string: address), url.scheme == "https", let host = url.host, !host.isEmpty,
              url.user == nil, url.password == nil, url.query == nil, url.fragment == nil,
              url.path == "/v1/u1" else { throw SlicerFailure("Enter your HTTPS home relay address ending in /v1/u1, without credentials or query parameters.") }
    }
    static func token(_ id: UUID, save: String? = nil) throws -> String? {
        let query: [String: Any] = [kSecClass as String:kSecClassGenericPassword,kSecAttrService as String:"PrinterCompanion.home-relay",kSecAttrAccount as String:id.uuidString]
        if let save {
            try RelayPairing.validateToken(save)
            let value = Data(save.utf8)
            var status = SecItemUpdate(query as CFDictionary,[kSecValueData as String:value] as CFDictionary)
            if status == errSecItemNotFound { var item=query;item[kSecValueData as String]=value;item[kSecAttrAccessible as String]=kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;status=SecItemAdd(item as CFDictionary,nil) }
            guard status == errSecSuccess else { throw SlicerFailure("Could not save the relay pairing securely.") };return nil
        }
        var request=query;request[kSecReturnData as String]=true;request[kSecMatchLimit as String]=kSecMatchLimitOne
        var value:CFTypeRef?
        guard SecItemCopyMatching(request as CFDictionary,&value)==errSecSuccess,let data=value as? Data else { return nil }
        return String(data:data,encoding:.utf8)
    }
    static func removeToken(_ id: UUID) {
        SecItemDelete([kSecClass as String:kSecClassGenericPassword,kSecAttrService as String:"PrinterCompanion.home-relay",kSecAttrAccount as String:id.uuidString] as CFDictionary)
    }
    static func select(_ printer: PrinterConfig) async throws -> (MoonrakerClient, String) {
        let local=MoonrakerClient(host:printer.host)
        guard let address=printer.relayURL, !address.isEmpty else { return (local,"LAN") }
        do { _ = try await local.fetchStatus(); return (local,"LAN") } catch { try Task.checkCancellation() }
        guard let secret=try token(printer.id) else { throw SlicerFailure("Home relay pairing is missing. Pair it again.") }
        let remote = try remote(address: address, token: secret)
        do { _ = try await remote.fetchStatus(); return (remote, "Home relay") }
        catch { remote.session.invalidateAndCancel(); throw error }
    }

    static func remote(address: String, token: String) throws -> MoonrakerClient {
        try validate(address)
        try RelayPairing.validateToken(token)
        let config=URLSessionConfiguration.ephemeral
        config.httpAdditionalHeaders=["Authorization":"Bearer " + token]
        config.timeoutIntervalForRequest=15
        config.urlCache = nil
        let session=URLSession(configuration:config,delegate:RelayRedirectPolicy(),delegateQueue:nil)
        return MoonrakerClient(host:address,session:session)
    }
}
