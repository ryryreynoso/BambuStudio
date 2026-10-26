import SwiftUI

struct PrinterListView: View {
    @EnvironmentObject private var store: PrinterStore
    @State private var adding = false

    var body: some View {
        NavigationStack {
            List {
                ForEach(store.printers) { printer in
                    NavigationLink(value: printer.id) {
                        VStack(alignment: .leading) {
                            Text(printer.name).font(.headline)
                            Text(printer.kind.label).font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                .onDelete { store.remove(at: $0) }
            }
            .overlay {
                if store.printers.isEmpty {
                    ContentUnavailableView("No printers", systemImage: "printer",
                                           description: Text("Add your Snapmaker U1 or Bambu Lab printer on the same Wi-Fi network."))
                }
            }
            .navigationTitle("Printers")
            .navigationDestination(for: UUID.self) { id in
                if let printer = store.printers.first(where: { $0.id == id }) {
                    PrinterDetailView(monitor: PrinterMonitor(printer: printer))
                }
            }
            .toolbar {
                Button { adding = true } label: { Image(systemName: "plus") }
                    .accessibilityLabel("Add printer")
            }
            .sheet(isPresented: $adding) {
                AddPrinterView()
            }
        }
    }
}
