import SwiftUI
import AVFoundation

private final class CameraSurface: UIView {
    var cameraLayer: CALayer?
    override func layoutSubviews() { super.layoutSubviews(); cameraLayer?.frame = bounds }
}
private struct CameraDisplay: UIViewRepresentable {
    let player: CameraPlayer
    func makeUIView(context: Context) -> UIView {
        let view = CameraSurface(); view.cameraLayer = player.layer; view.backgroundColor = .black
        view.layer.addSublayer(player.layer)
        return view
    }
    func updateUIView(_ uiView: UIView, context: Context) {
        DispatchQueue.main.async { player.layer.frame = uiView.bounds }
    }
    static func dismantleUIView(_ uiView: UIView, coordinator: ()) {
        uiView.layer.sublayers?.forEach { $0.removeFromSuperlayer() }
    }
}

struct BambuCameraView: View {
    let printer: PrinterConfig
    @State private var player = CameraPlayer()
    @State private var status = "Camera stopped"
    @Environment(\.scenePhase) private var phase
    var body: some View {
        VStack(alignment: .leading) {
            CameraDisplay(player: player).aspectRatio(16 / 9, contentMode: .fit)
            Text(status).font(.caption).foregroundStyle(.secondary)
            Button("Retry camera") { start() }
            Text("Requires LAN Mode Liveview on your P2S. Away from home, connect through your private VPN.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .onAppear { start() }
        .onDisappear { player.stop() }
        .onChange(of: phase) { _, newPhase in
            if newPhase == .active { start() } else { player.stop(); status = "Camera paused" }
        }
    }
    private func start() {
        player.statusHandler = { status = $0 }
        guard let code = Keychain.accessCode(for: printer.id), !code.isEmpty else {
            status = "Pair this printer with its LAN access code first."; return
        }
        player.start(host: printer.host, serial: printer.serial, code: code)
    }
}
