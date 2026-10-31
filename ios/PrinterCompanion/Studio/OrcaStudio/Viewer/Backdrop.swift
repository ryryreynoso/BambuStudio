// SPDX-License-Identifier: AGPL-3.0-only
import MetalKit

/// The full-screen gradient behind both canvases.
final class Backdrop {
    private let pipeline: MTLRenderPipelineState
    private let depth: MTLDepthStencilState

    init(device: MTLDevice, library: MTLLibrary, view: MTKView) throws {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.label = "backdrop"
        descriptor.vertexFunction = library.makeFunction(name: "vertexBackdrop")
        descriptor.fragmentFunction = library.makeFunction(name: "fragmentBackdrop")
        descriptor.rasterSampleCount = view.sampleCount
        descriptor.colorAttachments[0].pixelFormat = view.colorPixelFormat
        descriptor.depthAttachmentPixelFormat = view.depthStencilPixelFormat
        pipeline = try device.makeRenderPipelineState(descriptor: descriptor)
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .always
        depthDescriptor.isDepthWriteEnabled = false
        guard let state = device.makeDepthStencilState(descriptor: depthDescriptor) else {
            throw NSError(domain: "Backdrop", code: 1)
        }
        depth = state
    }

    func draw(_ encoder: MTLRenderCommandEncoder) {
        encoder.setRenderPipelineState(pipeline)
        encoder.setDepthStencilState(depth)
        encoder.setCullMode(.none)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
    }
}
