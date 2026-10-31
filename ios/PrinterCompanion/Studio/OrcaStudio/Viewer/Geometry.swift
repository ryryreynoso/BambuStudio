// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (c) 2026 OrcaPad contributors

import OrcaBridge
import simd

// MARK: - Vertex formats (must match the vertex descriptors in Renderer)

/// Lit triangle vertices: position + normal.
struct VertexPNC: Sendable {
    var position: SIMD3<Float>
    var normal: SIMD3<Float>
}

/// Unlit line vertices: position + color.
struct VertexPC: Sendable {
    var position: SIMD3<Float>
    var color: SIMD4<Float>
}

// MARK: - Geometry builders

enum Geometry {
    /// Machine bed grid from the selected printer's print volume, rendered in
    /// the bed-centered viewer frame. `bed` is in engine (z-up) millimetres;
    /// minor lines every 16 mm, bright border.
    static func bedGrid(for bed: EngineBed, minor: Float = 16) -> [VertexPC] {
        let center = bed.center
        var verts: [VertexPC] = []
        let dim = SIMD4<Float>(0.22, 0.24, 0.29, 1)   // minor grid lines
        let bright = SIMD4<Float>(0.55, 0.60, 0.70, 1) // border

        func viewer(_ x: Float, _ y: Float) -> SIMD3<Float> {
            CoordinateMapper.viewerPoint(
                fromEngine: SIMD3(x, y, 0),
                bedCenter: center
            )
        }

        let step = minor > 0 ? minor : 16
        // Lines of constant engine x
        var x = bed.minX
        while x <= bed.maxX {
            let isEdge = x == bed.minX || x == bed.maxX
            let c = isEdge ? bright : dim
            verts.append(VertexPC(position: viewer(x, bed.minY), color: c))
            verts.append(VertexPC(position: viewer(x, bed.maxY), color: c))
            x += step
        }
        // Lines of constant engine y
        var y = bed.minY
        while y <= bed.maxY {
            let isEdge = y == bed.minY || y == bed.maxY
            let c = isEdge ? bright : dim
            verts.append(VertexPC(position: viewer(bed.minX, y), color: c))
            verts.append(VertexPC(position: viewer(bed.maxX, y), color: c))
            y += step
        }
        // Transparent build-volume frame: the top rectangle and four rising edges.
        let frame = SIMD4<Float>(0.30, 0.34, 0.42, 1)
        let corners = [(bed.minX, bed.minY), (bed.maxX, bed.minY), (bed.maxX, bed.maxY), (bed.minX, bed.maxY)]
        for index in corners.indices {
            let (x0, y0) = corners[index]
            let (x1, y1) = corners[(index + 1) % corners.count]
            let top0 = CoordinateMapper.viewerPoint(fromEngine: SIMD3(x0, y0, bed.height), bedCenter: center)
            let top1 = CoordinateMapper.viewerPoint(fromEngine: SIMD3(x1, y1, bed.height), bedCenter: center)
            verts.append(VertexPC(position: viewer(x0, y0), color: frame))
            verts.append(VertexPC(position: top0, color: frame))
            verts.append(VertexPC(position: top0, color: frame))
            verts.append(VertexPC(position: top1, color: frame))
        }
        return verts
    }

    /// The build plate as a solid surface, lighter at the centre, with a thin darker rim.
    static func bedSurface(for bed: EngineBed) -> [VertexPC] {
        let center = bed.center
        let rim: Float = 5
        let inner = SIMD4<Float>(0.135, 0.145, 0.175, 1)
        let edge = SIMD4<Float>(0.085, 0.09, 0.11, 1)
        let border = SIMD4<Float>(0.05, 0.053, 0.065, 1)

        func point(_ x: Float, _ y: Float) -> SIMD3<Float> {
            var p = CoordinateMapper.viewerPoint(fromEngine: SIMD3(x, y, 0), bedCenter: center)
            p.y = -0.08
            return p
        }
        func quad(_ a: VertexPC, _ b: VertexPC, _ c: VertexPC, _ d: VertexPC) -> [VertexPC] { [a, b, c, a, c, d] }
        let mid = VertexPC(position: point(center.x, center.y), color: inner)
        let ring = [(bed.minX, bed.minY), (bed.maxX, bed.minY), (bed.maxX, bed.maxY), (bed.minX, bed.maxY)]
        let outer = [(bed.minX - rim, bed.minY - rim), (bed.maxX + rim, bed.minY - rim),
                     (bed.maxX + rim, bed.maxY + rim), (bed.minX - rim, bed.maxY + rim)]
        var verts: [VertexPC] = []
        for index in ring.indices {
            let next = (index + 1) % ring.count
            let a = VertexPC(position: point(ring[index].0, ring[index].1), color: edge)
            let b = VertexPC(position: point(ring[next].0, ring[next].1), color: edge)
            verts += [mid, a, b]
            let c = VertexPC(position: point(outer[next].0, outer[next].1), color: border)
            let d = VertexPC(position: point(outer[index].0, outer[index].1), color: border)
            verts += quad(VertexPC(position: a.position, color: border), VertexPC(position: b.position, color: border), c, d)
        }
        return verts
    }

    /// Unit cube centred on the origin (scaled/positioned by the model matrix).
    static func unitCube() -> (vertices: [VertexPNC], indices: [UInt32]) {
        // (normal, tangent, bitangent) per face — consistent quad corners.
        let faces: [(n: SIMD3<Float>, t: SIMD3<Float>, b: SIMD3<Float>)] = [
            (SIMD3(0, 0, 1), SIMD3(1, 0, 0), SIMD3(0, 1, 0)),
            (SIMD3(0, 0, -1), SIMD3(-1, 0, 0), SIMD3(0, 1, 0)),
            (SIMD3(1, 0, 0), SIMD3(0, 0, -1), SIMD3(0, 1, 0)),
            (SIMD3(-1, 0, 0), SIMD3(0, 0, 1), SIMD3(0, 1, 0)),
            (SIMD3(0, 1, 0), SIMD3(1, 0, 0), SIMD3(0, 0, -1)),
            (SIMD3(0, -1, 0), SIMD3(1, 0, 0), SIMD3(0, 0, 1)),
        ]
        var verts: [VertexPNC] = []
        var indices: [UInt32] = []
        let h: Float = 0.5
        for f in faces {
            let center = f.n * h
            let base = UInt32(verts.count)
            for (u, v) in [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)] {
                let p = center + f.t * (Float(u) * h) + f.b * (Float(v) * h)
                verts.append(VertexPNC(position: p, normal: f.n))
            }
            indices.append(contentsOf: [0, 1, 2, 0, 2, 3].map { $0 + base })
        }
        return (verts, indices)
    }
}

// MARK: - Engine mesh → viewer mesh

extension Geometry {
    /// Expands an owned engine-space mesh into viewer-space, per-face
    /// duplicated vertices with flat normals. The object pivot is removed
    /// here and restored by CoordinateMapper.modelMatrix per instance.
    static func expandFlat(mesh: SceneMesh) -> (verts: [VertexPNC], idx: [UInt32]) {
        guard !mesh.positions.isEmpty,
              !mesh.indices.isEmpty,
              mesh.indices.count.isMultiple(of: 3),
              mesh.indices.allSatisfy({ Int($0) < mesh.positions.count }) else {
            return ([], [])
        }
        func point(_ index: Int) -> SIMD3<Float> {
            // Object-local frame: the pivot is already subtracted and the
            // bed anchor is applied per-instance by CoordinateMapper.
            CoordinateMapper.viewerPoint(
                fromEngine: mesh.positions[index] - mesh.pivot,
                bedCenter: .zero
            )
        }
        var verts: [VertexPNC] = []
        verts.reserveCapacity(mesh.indices.count)
        for triangle in 0..<(mesh.indices.count / 3) {
            let a = point(Int(mesh.indices[triangle * 3]))
            let b = point(Int(mesh.indices[triangle * 3 + 1]))
            let c = point(Int(mesh.indices[triangle * 3 + 2]))
            var normal = simd_cross(b - a, c - a)
            let length = simd_length(normal)
            normal = length > 1e-12
                ? normal / length
                : SIMD3<Float>(repeating: 0)
            verts.append(VertexPNC(position: a, normal: normal))
            verts.append(VertexPNC(position: b, normal: normal))
            verts.append(VertexPNC(position: c, normal: normal))
        }
        // Face-duplicated vertices need sequential indices.
        return (verts, (0..<verts.count).map { UInt32($0) })
    }
}
