import Foundation
import AVFoundation
#if os(macOS)
import AppKit
#endif

/// Main-thread display adapter. A generation token prevents old sessions drawing into a new view.
@objc(VerifiedCameraPlayer)
public final class CameraPlayer: NSObject {
    @objc public let layer = AVSampleBufferDisplayLayer()
    @objc public var statusHandler: ((String) -> Void)?
    private var stream: CameraStream?
    @objc public private(set) var videoWidth = 0
    @objc public private(set) var videoHeight = 0
    private var announcedFrame = false
    private var generation = 0
    private let pendingLock = NSLock()
    private var pendingFrames = 0
    @objc public override init() { super.init(); layer.videoGravity = .resizeAspect }
    @objc public func start(host: String, serial: String, code: String) {
        precondition(Thread.isMainThread)
        stop(); let g = generation
        stream = CameraStream(host: host, serial: serial, code: code, onSample: { [weak self] sample in
            guard let self else { return }

            self.pendingLock.lock()
            self.pendingFrames += 1
            let overloaded = self.pendingFrames > 30
            self.pendingLock.unlock()
            if overloaded {
                self.pendingLock.lock(); self.pendingFrames -= 1; self.pendingLock.unlock()
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.generation == g else { return }
                    self.stop(); self.statusHandler?("Camera decoder is overloaded. Tap Retry to reconnect.")
                }
                return
            }
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                defer { self.pendingLock.lock(); self.pendingFrames -= 1; self.pendingLock.unlock() }
                guard self.generation == g else { return }
                if self.layer.status == .failed {
                    self.stop(); self.statusHandler?("Camera decoder failed. Tap Retry to reconnect."); return
                }
                // Dropping encoded P frames corrupts prediction: reset and reconnect on backpressure.
                guard self.layer.isReadyForMoreMediaData else {
                    self.stop(); self.statusHandler?("Camera decoder is overloaded. Tap Retry to reconnect."); return
                }
                self.layer.enqueue(sample)
                if let format = CMSampleBufferGetFormatDescription(sample) {
                    let dims = CMVideoFormatDescriptionGetDimensions(format)
                    self.videoWidth = Int(dims.width); self.videoHeight = Int(dims.height)
                }
                if !self.announcedFrame && self.layer.status == .rendering { self.announcedFrame = true; self.statusHandler?("Live") }
            }
        }, onStatus: { [weak self] status in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.generation == g else { return }
                if status != "Live" {
                    self.announcedFrame = false; self.layer.flushAndRemoveImage()
                    self.statusHandler?(status)
                }
            }
        })
        stream?.start()
    }
    @objc public func stop() {
        precondition(Thread.isMainThread)
        generation += 1; announcedFrame = false; stream?.stop(); stream = nil; layer.flushAndRemoveImage()
    }
    deinit { stream?.stop() }
}
