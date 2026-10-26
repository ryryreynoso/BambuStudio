import SwiftUI

@main
struct PrinterCompanionApp: App {
    @StateObject private var store = PrinterStore()

    var body: some Scene {
        WindowGroup {
            PrinterListView()
                .environmentObject(store)
        }
    }
}
