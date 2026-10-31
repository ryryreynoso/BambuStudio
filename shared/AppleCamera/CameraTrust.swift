import Foundation
import Security
enum CameraTrust {
    static func verify(_ trust: SecTrust, serial: String) -> Bool {
        let anchors = BambuCertificates.authoritiesDER.compactMap { SecCertificateCreateWithData(nil, $0 as CFData) }
        SecTrustSetPolicies(trust, SecPolicyCreateBasicX509())
        SecTrustSetAnchorCertificates(trust, anchors as CFArray)
        SecTrustSetAnchorCertificatesOnly(trust, true)
        var error: CFError?
        guard SecTrustEvaluateWithError(trust, &error),
              let chain = SecTrustCopyCertificateChain(trust) as? [SecCertificate],
              let leaf = chain.first else { return false }
        var commonName: CFString?
        SecCertificateCopyCommonName(leaf, &commonName)
        guard let name = commonName as String? else { return false }
        return name.caseInsensitiveCompare(serial.trimmingCharacters(in: .whitespaces)) == .orderedSame
    }

}
