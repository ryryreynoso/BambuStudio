# iPhone slicing workspace: status

Build 0.3.0 (8) of Printer Companion. The Slice tab is now a full workspace built on the same
on-device OrcaSlicer 2.4.2 engine the app already shipped, using OrcaPad's AGPL client code
(`ios/PrinterCompanion/Studio/`, see its `NOTICE.md`) with a new phone layout.

**Engine decision:** on-device slicing stays. No Mac or server is needed. On an iPhone 17 Pro Max,
slicing and parsing took under 2 s for small models and 37.6 s for the 541,440-triangle model.

## Verified with the real engine (47 tests, on the iPhone 17 Pro Max and in the iOS Simulator)

The simulator runs the device engine's own objects, retagged by
`Scripts/build_simulator_engine.py`. `Tests/StudioEngineTests.swift` covers:

| Check | Result |
|---|---|
| Cube imports at true size, centred, on the bed | 20 mm, centred at 128 |
| Scale to 150% | 150 layers, 30.00 mm |
| Rotate 45° on Z | Footprint 41.9 mm across wall centre lines |
| Standing on an edge | Engine refuses; the app explains why |
| Layer height 0.20 → 0.12 mm | 100 → 166 layers, 16,985 → 28,035 extrusions |
| Supports off → on, T shape | 2.99 → 4.52 g, support paths only when on |
| Infill 10% → 40% | 3.10 → 5.30 g |
| Path types in preview data | Inner/outer wall, infill, top, bottom, brim, travel, seam |
| Large model | 541,440 triangles, 750 layers, 2.7 M extrusions, 37.6 s on the iPhone |
| Arrange, undo, redo, duplicate, split | Change the document |
| Two plates | Each slices to its own file (`plate_2.gcode`) |
| Saved preset and 3MF project | Survive reopening the engine session |
| P2S output | Sliced 3MF with `Metadata/plate_1.gcode`; 256 mm bed |
| U1 output | Plain G-code, no Bambu commands; 270 mm bed |
| U1 toolheads | Cubes on toolheads 1 and 3 use T0 and T2 with 252 tool changes |

## Compared with desktop

Same STL and same named profiles (0.20mm Standard, Bambu PLA Basic, grid infill) against
Bambu Studio 02.08.02.61's command line. The two are different engines with different profile
versions (02.01.00.19 vs 02.08.00.05), so this is close agreement, not identity.

| | Phone | Desktop |
|---|---|---|
| Layers | 100 | 100 |
| Weight | 3.6 g | 3.75 g |
| Motion time from the toolpath moves | 430 s | 436 s |
| Estimated print time shown | 20m 5s | 9m 0s |

**The phone's time estimate reads about twice the desktop's on the P2S.** The moves are nearly
the same length and speed, so treat the estimate as high. The cause is not found yet; about 5
minutes of it is the start routine, which the desktop counts as about 20 seconds.

Snapmaker Orca's command line exited without output, so the U1 has no desktop comparison.

## Not verified

* The interface on a physical iPhone: frame rate, memory, touch feel. Only the engine tests ran there.
* Any print on a real printer from this build. Nothing was sent to a printer.
* Multi-color AMS mapping and the U1 identity `map_table`: built, never tried on hardware.
* Multi-plate P2S start: the command now names the right plate, but it is untested on a printer.

## Mac app comparison

| Feature | State |
|---|---|
| Plate view, XYZ gizmos, camera presets, orthographic view | Done |
| Lighting, plate surface, projected shadow | Done; the shadow is hard-edged |
| Import STL, OBJ, AMF, 3MF; open and save projects | Done |
| Move, rotate, scale, lay flat, orient, arrange, duplicate, delete | Done |
| Split, cut, modifier meshes, several plates | Done |
| Support painting, seam painting, blockers, enforcers | Done through Paint and Objects |
| Toolpath preview, layer slider, playback, feature colors and toggles | Done |
| Time, weight, cost, layer and tool-change totals | Done; time reads high (above) |
| Basic / Advanced / Expert, search, presets, per-object settings | Done |
| Undo and redo | Done |
| Per-slot filament values | **Partial:** every slot uses the first filament's temperatures and speeds. The app warns. Needs an engine rebuild. |
| Full Spectrum color mixing | Not ported |
| OrcaPad's own print-host and device screens | Compiled but not shown; this app's P2S and U1 flows are used |
