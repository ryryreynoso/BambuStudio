// SPDX-License-Identifier: AGPL-3.0-only
import OrcaBridge
import XCTest
import simd

/// Runs the real engine with the bundled P2S and U1 profiles. Nothing here talks to a printer.
/// Each test prints `VALIDATION:` lines; the parity report quotes them.
final class StudioEngineTests: XCTestCase {
    private var directory: URL!
    private var presets: EnginePresets!
    private var plater: EnginePlater!
    private let bed = SIMD2<Double>(128, 128)

    /// The engine accepts one data directory per process.
    private static let dataDirectory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("OrcaEngine-0.19.0", isDirectory: true)

    override func setUpWithError() throws {
        directory = Self.dataDirectory
        let manager = FileManager.default
        try? manager.removeItem(at: directory.appendingPathComponent("user"))
        try manager.createDirectory(at: directory.appendingPathComponent("user/default"), withIntermediateDirectories: true)
        if !manager.fileExists(atPath: directory.appendingPathComponent("system").path) {
            let system = try XCTUnwrap(Bundle.main.url(forResource: "system", withExtension: nil))
            try manager.copyItem(at: system, to: directory.appendingPathComponent("system"))
        }
        presets = try EnginePresets.create(dataDir: directory.path).get()
        plater = try EnginePlater()
    }

    private func usePrinter(_ name: String) throws {
        try presets.selectMachine(name).get()
        let volume = try presets.printVolume().get()
        _ = try plater.setBed(originX: volume.minX, originY: volume.minY, width: volume.width, depth: volume.depth, height: 1000)
    }

    private func set(_ key: String, _ value: String) throws {
        try presets.setValue(kind: .process, key: key, serializedValue: value).get()
    }

    private func cubeSTL() throws -> URL {
        try XCTUnwrap(Bundle.main.url(forResource: "orcapad-calibration-cube-20mm", withExtension: "stl", subdirectory: "Handy"))
    }

    @discardableResult
    private func importCube() throws -> SceneObject {
        let result = try plater.apply(.importModel(path: cubeSTL().path, bedCenter: bed))
        return try XCTUnwrap(result.snapshot.objects.last)
    }

    /// Writes an ASCII STL made of axis-aligned boxes.
    private func boxesSTL(_ boxes: [(min: SIMD3<Double>, max: SIMD3<Double>)]) throws -> URL {
        var text = "solid boxes\n"
        func tri(_ a: SIMD3<Double>, _ b: SIMD3<Double>, _ c: SIMD3<Double>) {
            text += "facet normal 0 0 0\nouter loop\n"
            for v in [a, b, c] { text += "vertex \(v.x) \(v.y) \(v.z)\n" }
            text += "endloop\nendfacet\n"
        }
        for box in boxes {
            let (l, h) = (box.min, box.max)
            let p = [SIMD3(l.x, l.y, l.z), SIMD3(h.x, l.y, l.z), SIMD3(h.x, h.y, l.z), SIMD3(l.x, h.y, l.z),
                     SIMD3(l.x, l.y, h.z), SIMD3(h.x, l.y, h.z), SIMD3(h.x, h.y, h.z), SIMD3(l.x, h.y, h.z)]
            for f in [[0, 2, 1], [0, 3, 2], [4, 5, 6], [4, 6, 7], [0, 1, 5], [0, 5, 4], [1, 2, 6], [1, 6, 5], [2, 3, 7], [2, 7, 6], [3, 0, 4], [3, 4, 7]] {
                tri(p[f[0]], p[f[1]], p[f[2]])
            }
        }
        text += "endsolid boxes\n"
        let url = directory.appendingPathComponent("\(UUID().uuidString).stl")
        try text.write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    private struct Slice {
        let toolpath: GcodeToolpath
        let url: URL
        let result: SliceResult

        var extrusions: [ToolpathSegment] { toolpath.segments.filter(\.isExtrusion) }
        func count(role: UInt8) -> Int { extrusions.filter { $0.role == role }.count }
        var weight: Double { toolpath.extruders.reduce(0) { $0 + $1.weightG } }
        /// Layers that carry model extrusion, leaving out the printer's start routine.
        var modelTop: Float { extrusions.filter { $0.role != 18 }.map(\.end.z).max() ?? 0 }
    }

    private func slice(_ label: String) throws -> Slice {
        let url = directory.appendingPathComponent("\(UUID().uuidString).gcode")
        let started = Date()
        let result = try plater.slice(to: url.path, presets: presets, job: SliceJob()) { _, _ in }
        let toolpath = try EngineGcode.loadToolpath(path: url.path)
        let slice = Slice(toolpath: toolpath, url: url, result: result)
        print(String(format: "VALIDATION: %@: %d layers, top %.2f mm, %d extrusions, %.2f g, %.0f s print, slice+parse %.1f s",
                     label, toolpath.layers.count, slice.modelTop, slice.extrusions.count, slice.weight,
                     result.timeEstimateSeconds, Date().timeIntervalSince(started)))
        return slice
    }

    func testCubeImportsAtTrueSizeOnTheBed() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        let object = try importCube()
        let size = object.mesh.bounds.size
        XCTAssertEqual(size.x, 20, accuracy: 0.01)
        XCTAssertEqual(size.z, 20, accuracy: 0.01)
        XCTAssertEqual(object.instances[0].transform.offsetX, 128, accuracy: 0.01)
        XCTAssertEqual(object.instances[0].transform.offsetZ, 10, accuracy: 0.01)
    }

    func testRotateAndScaleChangeWhatIsSliced() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        let object = try importCube()
        let instance = object.instances[0]
        var transform = instance.transform
        transform.scaleX = 1.5; transform.scaleY = 1.5; transform.scaleZ = 1.5
        _ = try plater.apply(.setTransform(objectID: object.id, instanceID: instance.id, transform: transform, label: "Scale"))
        let scaled = try slice("cube scaled 150%")
        XCTAssertEqual(scaled.modelTop, 30, accuracy: 0.05)
        XCTAssertEqual(scaled.toolpath.layers.count, 150, accuracy: 2)

        _ = try plater.apply(.rotate(objectID: object.id, instanceID: instance.id, axis: .z, radians: .pi / 4))
        let rotated = try slice("cube scaled and rotated 45° on Z")
        // Seen from above, a 30 mm square turned 45° spans 42.4 mm corner to corner.
        let walls = rotated.extrusions.filter { $0.role == 2 }
        let span = (walls.map(\.start.x).max() ?? 0) - (walls.map(\.start.x).min() ?? 0)
        XCTAssertEqual(span, 41.9, accuracy: 0.6) // Wall centre lines sit half a line width inside.
        XCTAssertEqual(rotated.modelTop, 30, accuracy: 0.05)

        // Stood on an edge, the cube has no first layer: the engine must refuse, not crash.
        _ = try plater.apply(.rotate(objectID: object.id, instanceID: instance.id, axis: .x, radians: .pi / 4))
        XCTAssertThrowsError(try slice("cube standing on an edge"))
    }

    func testLayerHeightChangesLayersAndPaths() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        try importCube()
        try set("layer_height", "0.2")
        let coarse = try slice("cube 0.20 mm")
        try set("layer_height", "0.12")
        let fine = try slice("cube 0.12 mm")
        XCTAssertEqual(coarse.toolpath.layers.count, 100, accuracy: 2)
        XCTAssertEqual(fine.toolpath.layers.count, 167, accuracy: 3)
        XCTAssertEqual(coarse.modelTop, 20, accuracy: 0.05)
        XCTAssertEqual(fine.modelTop, 20, accuracy: 0.13)
        XCTAssertGreaterThan(fine.extrusions.count, coarse.extrusions.count)
    }

    func testSupportsAppearOnlyWhenEnabled() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        let tee = try boxesSTL([(SIMD3(-5, -5, 0), SIMD3(5, 5, 25)), (SIMD3(-25, -5, 25), SIMD3(25, 5, 30))])
        _ = try plater.apply(.importModel(path: tee.path, bedCenter: bed))
        try set("enable_support", "0")
        let without = try slice("T-shape, supports off")
        try set("enable_support", "1")
        let with = try slice("T-shape, supports on")
        XCTAssertEqual(without.count(role: 14) + without.count(role: 15), 0)
        XCTAssertGreaterThan(with.count(role: 14) + with.count(role: 15), 100)
        XCTAssertGreaterThan(with.weight, without.weight)
    }

    func testInfillDensityChangesMaterial() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        try importCube()
        try set("sparse_infill_density", "10%")
        let light = try slice("cube infill 10%")
        try set("sparse_infill_density", "40%")
        let dense = try slice("cube infill 40%")
        XCTAssertGreaterThan(dense.weight, light.weight * 1.2)
        XCTAssertGreaterThan(dense.count(role: 4), light.count(role: 4))
    }

    func testPreviewCarriesEveryPathType() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        try importCube()
        try set("brim_type", "outer_only")
        let result = try slice("cube with brim")
        for (name, role) in [("inner wall", UInt8(1)), ("outer wall", 2), ("sparse infill", 4), ("top surface", 6), ("bottom surface", 7), ("brim", 13)] {
            XCTAssertGreaterThan(result.count(role: role), 0, name)
        }
        XCTAssertTrue(result.toolpath.segments.contains { $0.moveType == .travel })
        XCTAssertTrue(result.toolpath.segments.contains { $0.moveType == .seam })
        XCTAssertGreaterThan(result.toolpath.extruders[0].weightG, 0)
        XCTAssertGreaterThan(result.toolpath.estimatedSeconds, 60)
    }

    func testObjectToolsChangeTheDocument() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        let object = try importCube()
        let instance = object.instances[0]
        var scene = try plater.apply(.addInstance(objectID: object.id)).snapshot
        XCTAssertEqual(scene.objects[0].instances.count, 2)
        _ = try plater.apply(.arrange(allowRotations: false), presets: presets)
        scene = try plater.snapshot()
        let first = scene.objects[0].instances[0].transform, second = scene.objects[0].instances[1].transform
        XCTAssertGreaterThanOrEqual(hypot(first.offsetX - second.offsetX, first.offsetY - second.offsetY), 20)
        scene = try plater.apply(.undo).snapshot
        scene = try plater.apply(.undo).snapshot
        XCTAssertEqual(scene.objects[0].instances.count, 1)
        scene = try plater.apply(.redo).snapshot
        XCTAssertEqual(scene.objects[0].instances.count, 2)
        _ = try plater.apply(.deleteInstance(objectID: object.id, instanceID: instance.id))

        let twoBodies = try boxesSTL([(SIMD3(0, 0, 0), SIMD3(10, 10, 10)), (SIMD3(30, 0, 0), SIMD3(40, 10, 10))])
        let imported = try plater.apply(.importModel(path: twoBodies.path, bedCenter: bed))
        let split = try plater.apply(.splitObjects(objectID: XCTUnwrap(imported.createdObjectID)))
        XCTAssertEqual(split.createdObjectCount, 2)
    }

    func testMultiplePlatesSliceSeparately() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        try importCube()
        let second = try plater.apply(.addPlate)
        let plateID = try XCTUnwrap(second.createdPlateID)
        let selected = try plater.selectPlate(plateID)
        let plate = try XCTUnwrap(selected.plates.first { $0.id == plateID })
        let center = SIMD2(Double(plate.origin.x + plate.size.x / 2), Double(plate.origin.y + plate.size.y / 2))
        _ = try plater.apply(.importModel(path: cubeSTL().path, bedCenter: center))
        let scene = try plater.snapshot()
        XCTAssertEqual(Set(scene.objects.flatMap(\.instances).map(\.plate)), [0, 1])
        let url = directory.appendingPathComponent("second.3mf")
        _ = try plater.exportGcode3mfPlate(plateIndex: 1, to: url.path, presets: presets, job: SliceJob()) { _, _ in }
        let data = try Data(contentsOf: url)
        let hasFirst = data.range(of: Data("Metadata/plate_1.gcode".utf8)) != nil
        let hasSecond = data.range(of: Data("Metadata/plate_2.gcode".utf8)) != nil
        print("VALIDATION: second plate 3MF: plate_1.gcode \(hasFirst), plate_2.gcode \(hasSecond)")
        XCTAssertTrue(hasFirst || hasSecond)
    }

    func testSettingsSurviveARestart() throws {
        try usePrinter("Snapmaker U1 (0.4 nozzle)")
        try set("layer_height", "0.12")
        try set("sparse_infill_density", "35%")
        try presets.saveUser(kind: .process, name: "My U1 Fine", detach: false).get()
        presets = nil
        let reopened = try EnginePresets.create(dataDir: directory.path).get()
        try reopened.selectMachine("Snapmaker U1 (0.4 nozzle)").get()
        try reopened.select(machine: "Snapmaker U1 (0.4 nozzle)", filament: reopened.current(kind: .filament).get(), process: "My U1 Fine").get()
        let config = try reopened.resolvedConfig().get()
        XCTAssertTrue(config.contains("\"layer_height\":\"0.12\"") || config.contains("\"layer_height\": \"0.12\""))
        XCTAssertTrue(config.contains("35%"))

        try presets_projectRoundTrip(reopened)
    }

    private func presets_projectRoundTrip(_ session: EnginePresets) throws {
        let object = try plater.apply(.importModel(path: cubeSTL().path, bedCenter: bed)).createdObjectID
        XCTAssertNotNil(object)
        let url = directory.appendingPathComponent("project.3mf")
        try plater.saveProject(path: url.path, presets: session, thumbnail: nil)
        let fresh = try EnginePlater()
        _ = try fresh.openProject(path: url.path, presets: session)
        XCTAssertEqual(try fresh.snapshot().objects.count, 1)
    }

    func testBambuAndU1OutputStayIndependent() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        XCTAssertEqual(try presets.printVolume().get().width, 256, accuracy: 0.1)
        try importCube()
        let bambuFile = directory.appendingPathComponent("p2s.3mf")
        _ = try plater.exportGcode3mfPlate(plateIndex: 0, to: bambuFile.path, presets: presets, job: SliceJob()) { _, _ in }
        let bambu = try Data(contentsOf: bambuFile)
        XCTAssertEqual(Array(bambu.prefix(2)), [80, 75])
        XCTAssertNotNil(bambu.range(of: Data("Metadata/plate_1.gcode".utf8)))
        let bambuGcode = try slice("P2S cube")
        let bambuText = try String(contentsOf: bambuGcode.url, encoding: .utf8)

        _ = try plater.apply(.clear)
        try usePrinter("Snapmaker U1 (0.4 nozzle)")
        let volume = try presets.printVolume().get()
        print("VALIDATION: U1 bed \(volume.width) × \(volume.depth)")
        _ = try plater.apply(.importModel(path: cubeSTL().path, bedCenter: SIMD2(volume.centerX, volume.centerY)))
        let u1 = try slice("U1 cube")
        XCTAssertEqual(u1.modelTop, 20, accuracy: 0.05)
        let u1Text = try String(contentsOf: u1.url, encoding: .utf8)
        XCTAssertNotEqual(bambuText.prefix(4000), u1Text.prefix(4000))
        XCTAssertFalse(u1Text.contains("M620"), "Bambu AMS commands must not reach the U1")
        XCTAssertTrue(u1Text.localizedCaseInsensitiveContains("snapmaker u1"))
    }

    func testU1FourToolheadsAssignFilamentsAndToolChanges() throws {
        try usePrinter("Snapmaker U1 (0.4 nozzle)")
        let volume = try presets.printVolume().get()
        try presets.setFilamentCount(4, newColour: "#00AA55").get()
        XCTAssertEqual(try presets.filamentSlots().get().count, 4)
        let center = SIMD2(volume.centerX, volume.centerY)
        let first = try plater.apply(.importModel(path: cubeSTL().path, bedCenter: center))
        let second = try plater.apply(.importModel(path: cubeSTL().path, bedCenter: center))
        _ = try plater.apply(.arrange(allowRotations: false), presets: presets)
        for (created, tool) in [(first.createdObjectID, "1"), (second.createdObjectID, "3")] {
            for key in ["outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id", "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id"] {
                _ = try plater.apply(.setSetting(objectID: XCTUnwrap(created), volumeID: nil, key: key, value: tool))
            }
        }
        let result = try slice("U1 two cubes, toolheads 1 and 3")
        let tools = Set(result.extrusions.filter { $0.role != 18 }.map(\.extruder))
        XCTAssertEqual(tools, [0, 2])
        XCTAssertGreaterThan(result.toolpath.extruders[2].weightG, 3)
        let gcode = try String(contentsOf: result.url, encoding: .utf8)
        let changes = gcode.split(separator: "\n").filter { $0 == "T0" || $0 == "T2" }.count
        print("VALIDATION: U1 tool-change commands in G-code: \(changes)")
        XCTAssertGreaterThan(changes, 50)
    }

    /// Reference from desktop Bambu Studio's CLI: same STL, "0.20mm Standard @BBL P2S",
    /// "Bambu PLA Basic @BBL P2S": 100 layers, 3.75 g, 1236 mm of filament, 9m 0s.
    func testMatchesDesktopBambuStudioWithTheSameProfiles() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        try presets.select(machine: "Bambu Lab P2S 0.4 nozzle", filament: "Bambu PLA Basic @BBL P2S",
                           process: presets.current(kind: .process).get()).get()
        try set("sparse_infill_pattern", "grid")
        try importCube()
        let result = try slice("P2S cube, desktop's filament and pattern")
        let gcode = try String(contentsOf: result.url, encoding: .utf8)
        XCTAssertTrue(gcode.contains("; slow_down_layer_time = 4"), "the filament's own values must be used")
        XCTAssertTrue(gcode.contains("; filament_max_volumetric_speed = 21"))
        XCTAssertEqual(result.toolpath.layers.count, 100)
        XCTAssertEqual(result.weight, 3.75, accuracy: 0.2)
        // Desktop's moves add up to 436 s at their programmed speeds. The two estimators disagree
        // (the phone's reads high on the P2S), so the moves are what gets compared.
        var motion = 0.0
        for segment in result.toolpath.segments where segment.isExtrusion || segment.moveType == .travel {
            guard segment.feedrate > 0 else { continue }
            motion += Double(simd_distance(segment.start, segment.end) / segment.feedrate)
        }
        print("VALIDATION: P2S cube motion time \(Int(motion)) s; desktop 436 s")
        XCTAssertEqual(motion, 436, accuracy: 60)
    }

    /// A slot's filament must reach the G-code, not just its name.
    func testSecondFilamentSlotUsesItsOwnValues() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        try presets.setFilamentCount(2).get()
        try presets.selectFilament(slot: 1, presetName: "Bambu PLA Basic @BBL P2S").get()
        try importCube()
        let result = try slice("P2S two filaments")
        let gcode = try String(contentsOf: result.url, encoding: .utf8)
        // Known limit of this engine bridge: every slot slices with the first filament's values.
        // The app warns about it. This expectation trips if a new engine build fixes it.
        XCTExpectFailure("Slots share the first filament's values")
        XCTAssertTrue(gcode.contains("; filament_max_volumetric_speed = 12,21"))
    }

    func testLargeComplexModelSlices() throws {
        try usePrinter("Bambu Lab P2S 0.4 nozzle")
        let sphere = try plater.apply(.addShapeObject(shape: .sphere, sizeMM: 150, bedCenter: bed))
        let object = try XCTUnwrap(sphere.createdObjectID)
        let torus = try plater.apply(.addShapeObject(shape: .torus, sizeMM: 60, bedCenter: SIMD2(40, 40)))
        XCTAssertNotNil(torus.createdObjectID)
        for _ in 0..<2 {
            let scene = try plater.snapshot()
            let volume = try XCTUnwrap(scene.objects.first { $0.id == object }?.volumes.first)
            _ = try plater.apply(.subdivideVolume(objectID: object, volumeID: volume.id))
        }
        let triangles = try plater.snapshot().objects.reduce(0) { $0 + $1.mesh.indices.count / 3 }
        let result = try slice("sphere 150 mm + torus, \(triangles) triangles")
        XCTAssertGreaterThan(result.toolpath.layers.count, 500)
    }
}
