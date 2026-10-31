// SPDX-License-Identifier: AGPL-3.0-only
import SwiftUI

struct AboutView: View {
    var body: some View {
        NavigationStack {
            List {
                Section("Printer Companion") {
                    Text("On-device slicing for Bambu Lab P2S and Snapmaker U1. Independent software; not affiliated with either manufacturer.")
                    Text("Version \(Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?") (\(Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "?"))")
                    Text("Native engine: OrcaPad 0.19.0 / OrcaSlicer 2.4.2, C ABI 42. Derived from Slic3r, PrusaSlicer, Bambu Studio and OrcaSlicer. © their contributors.")
                }
                Section("Open source") {
                    if let source = Bundle.main.url(forResource: "CorrespondingSource", withExtension: "tar.gz") {
                        ShareLink("Complete source for this build", item: source)
                    } else {
                        Text("Complete source package is not bundled in this development build.")
                    }
                    Link("Application source", destination: URL(string: "https://github.com/ryryreynoso/BambuStudio")!)
                    Link("Exact engine source and build instructions", destination: URL(string: "https://github.com/iSteeb/OrcaPad/releases/tag/release-0.19.0")!)
                    NavigationLink("GNU AGPL 3.0") { license("AGPL-3.0", directory: "Licenses") }
                    NavigationLink("Third-party notices") { license("THIRD_PARTY", directory: "Licenses", extension: "md") }
                    NavigationLink("curl license") { license("curl-8.23.0", directory: "Licenses") }
                    NavigationLink("OpenSSL license") { license("OpenSSL-3.5.9", directory: "Licenses") }
                }
                Section("Privacy and connection") {
                    Text("Models and slicing stay on this device. Printer credentials stay in the iOS Keychain. The app sends files and commands only to the printer you choose. It has no analytics service or cloud slicing account.")
                    Text("On-device slicing and local printing work without a computer. Internet access must be configured separately. Direct Snapmaker cloud sign-in is not available in this build. Bambu printing uses LAN authentication, independently of Bambu cloud sign-in.")
                }
            }.navigationTitle("About")
        }
    }
    private func license(_ name: String, directory: String, extension ext: String = "txt") -> some View {
        ScrollView {
            Text(load(name, directory: directory, extension: ext))
                .font(.system(.caption, design: .monospaced)).textSelection(.enabled).padding()
        }.navigationTitle(name)
    }
    private func load(_ name: String, directory: String, extension ext: String) -> String {
        guard let url = Bundle.main.url(forResource: name, withExtension: ext, subdirectory: directory),
              let text = try? String(contentsOf: url, encoding: .utf8) else { return "License resource missing. Please report this build." }
        return text
    }
}
