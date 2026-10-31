import Foundation
import Security

/// Bambu access codes are kept in the iOS Keychain, readable only on this device after first unlock.
enum Keychain {
    private static let service = "PrinterCompanion.bambu-access-code"

    static func setAccessCode(_ code: String, for id: UUID) throws {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id.uuidString,
            kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
            kSecValueData as String: Data(code.utf8),
        ]
        let match: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service, kSecAttrAccount as String: id.uuidString]
        var status = SecItemUpdate(match as CFDictionary, [kSecValueData as String: Data(code.utf8)] as CFDictionary)
        if status == errSecItemNotFound { status = SecItemAdd(query as CFDictionary, nil) }
        guard status == errSecSuccess else { throw SlicerFailure("The LAN access code could not be saved securely (Keychain \(status)). Unlock the device and try again.") }
    }

    static func accessCode(for id: UUID) -> String? {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id.uuidString,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess, let data = item as? Data else { return nil }
        return String(data: data, encoding: .utf8)
    }

    static func deleteAccessCode(for id: UUID) {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: id.uuidString,
        ]
        SecItemDelete(query as CFDictionary)
    }
}
