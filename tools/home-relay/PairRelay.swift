// Creates an owner pairing file without putting its secret in arguments or logs.
import Foundation
import Security
import Darwin

struct Pairing: Encodable {
    let version = 1
    let name: String
    let lanHost: String
    let relayURL: String
    let token: String
}
struct Failure: Error { let message: String }
func value(_ flag: String) throws -> String {
    guard let i = CommandLine.arguments.firstIndex(of: flag), i+1 < CommandLine.arguments.count else {
        throw Failure(message: "Required: --endpoint https://YOUR-HOST/v1/u1 --printer U1-IP --output /private/folder/U1-relay.json")
    }
    return CommandLine.arguments[i+1]
}
do {
    let endpoint = try value("--endpoint"), printer = try value("--printer"), output = try value("--output")
    guard let url = URLComponents(string: endpoint), url.scheme == "https", let host = url.host, !host.isEmpty,
          url.path == "/v1/u1", url.user == nil, url.password == nil, url.query == nil, url.fragment == nil else {
        throw Failure(message: "Use your HTTPS relay address ending /v1/u1, without credentials or query parameters.")
    }
    guard let lan = URLComponents(string: printer.contains("://") ? printer : "http://"+printer),
          ["http", "https"].contains(lan.scheme ?? ""), lan.host != nil, lan.user == nil, lan.password == nil,
          lan.query == nil, lan.fragment == nil, ["", "/"].contains(lan.path) else { throw Failure(message: "Use a printer IP/hostname without credentials.") }
    let query: [String:Any] = [kSecClass as String:kSecClassGenericPassword,kSecAttrService as String:"3DPrint.HomeRelay",kSecAttrAccount as String:"owner"]
    var read = query; read[kSecReturnData as String] = true; read[kSecMatchLimit as String] = kSecMatchLimitOne
    var item: CFTypeRef?
    let status = SecItemCopyMatching(read as CFDictionary, &item)
    let token: String
    if status == errSecSuccess, let data = item as? Data, let saved = String(data:data, encoding:.utf8) { token = saved }
    else if status == errSecItemNotFound {
        var bytes = [UInt8](repeating:0,count:32)
        guard SecRandomCopyBytes(kSecRandomDefault,bytes.count,&bytes) == errSecSuccess else { throw Failure(message:"Could not create pairing secret.") }
        token = Data(bytes).base64EncodedString().replacingOccurrences(of:"+",with:"-").replacingOccurrences(of:"/",with:"_").replacingOccurrences(of:"=",with:"")
        var create = query;create[kSecValueData as String] = Data(token.utf8);create[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        guard SecItemAdd(create as CFDictionary,nil) == errSecSuccess else { throw Failure(message:"Could not store pairing in Keychain.") }
    } else { throw Failure(message:"Unlock your Mac Keychain and try again. Existing pairing was preserved.") }
    guard token.count >= 32 else { throw Failure(message:"Existing relay token is invalid; replace it in Keychain before pairing.") }
    let data = try JSONEncoder().encode(Pairing(name:"Snapmaker U1",lanHost:printer,relayURL:endpoint,token:token))
    let descriptor = open(output,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,S_IRUSR|S_IWUSR)
    guard descriptor >= 0 else { throw Failure(message:"Choose a new pairing file path in an existing private folder. Existing files are never overwritten.") }
    let file = FileHandle(fileDescriptor:descriptor,closeOnDealloc:true)
    try file.write(contentsOf:data);try file.close()
    print("Pairing saved securely. Transfer the file privately to your iPhone, import it in U1 remote access, then delete the transfer copies. Do not share it in chat.")
} catch {
    let message = (error as? Failure)?.message ?? "Could not create pairing file. No secret was displayed."
    FileHandle.standardError.write(Data((message+"\n").utf8));exit(1)
}
