import OrcaStudio
import SwiftUI

@main
struct PrinterCompanionApp: App {
    @StateObject private var store = PrinterStore()
    @State private var printJob: SlicedFile?

    var body: some Scene {
        WindowGroup {
            TabView {
                PrinterListView()
                    .tabItem { Label("Printers", systemImage: "printer") }
                StudioRootView { job in printJob = SlicedFile(job) }
                    .tabItem { Label("Slice", systemImage: "square.3.layers.3d") }
                AboutView()
                    .tabItem { Label("About", systemImage: "info.circle") }
            }
            .environmentObject(store)
            .sheet(item: $printJob) { PrintFileView(file: $0).environmentObject(store) }
        }
    }
}
