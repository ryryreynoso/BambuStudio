# Full Spectrum color mixing for Bambu Studio: audit and port plan

Status: **in progress**. This file is the source of truth for what has been ported, what is
pending, and what is out of scope. Nothing is marked done unless it compiles and is covered by
a test in this repository.

## 1. Goal

Bambu Studio with all of its existing functionality intact (Bambu account, MakerWorld, cloud
and LAN device control, AMS/AMS 2, painting, profiles, 3MF), **plus** the capabilities of the
Full Spectrum color-mixing feature from OrcaSlicer-FullSpectrum / Snapmaker Orca.

Out of scope: Snapmaker account sign-in, Snapmaker device UI, and every other Snapmaker or
OrcaSlicer feature. The Bambu networking plugin, account flows and device pages are untouched.

## 2. Sources and licenses (verified 2026-10-24)

| Repository | Revision | License | Role |
| --- | --- | --- | --- |
| bambulab/BambuStudio | `79df7ae` (2.8.3 beta fixes, 2026-10-13) | AGPL-3.0 | Base application |
| ratdoux/OrcaSlicer-FullSpectrum | `cc152a84dd` (FullSpectrum 0.9.14, 2026-09-27) | AGPL-3.0 | Primary reference: original developer, newest code |
| Snapmaker/OrcaSlicer-FullSpectrum-test | `b3c41fda4` (2026-04-28) | AGPL-3.0 | Cross-check of the Snapmaker-shipped behavior |

All inputs are AGPL-3.0, the same license as Bambu Studio. Code adapted from Full Spectrum is
marked at its definition with a "ported from OrcaSlicer-FullSpectrum" comment naming the source.

Full Spectrum forked upstream OrcaSlicer at `737948be1f` (2025-10-03) and adds 760 commits; most
of its 589 changed `src/` files are unrelated Snapmaker merges. The color-mixing files are listed
in §5. The Snapmaker wiki (`wiki.snapmaker.com`) is blocked by the build environment's network
policy, so behavior is taken from the source and the design notes in the Full Spectrum `doc/`.

## 3. Key finding: Bambu Studio already has native mixed filaments

Bambu Studio 2.8.x ships its own mixed-color feature, enabled whenever a project has two or more
physical filaments:

* Data model: a mixed filament is an extra **filament slot** after the physical ones, described
  by per-slot project keys `filament_is_mixed`, `filament_mixed_components`,
  `filament_mixed_sublayer_ratios`, `filament_mixed_gradient`, `filament_mixed_gradient_range`,
  `filament_mixed_gradient_curve`, `filament_mixed_gradient_per_part`, plus the process option
  `enable_mixed_color_sublayer`.
* Engine: `ToolOrdering::resolve_mixed_filaments()` resolves each slot per layer, either by
  deficit round-robin over the ratios (layer alternation) or by sub-layer groups (sublayer
  splitting, gradients) that `GCode.cpp` prints at fractional Z with scaled flow.
  `enforce_mixed_component_order()` keeps component order valid across groups, and H2D
  unprintable areas and filament grouping are expanded to the physical components.
* UI: sidebar mixed-filament section, `MixedFilamentDialog`, `GradientCurveEditor`,
  `ColorDecompose*` (target color to recipe, backed by a measured recipe table), texture import.
* Library: `libslic3r/FilamentMixer.*`, `FilamentMixerModel.hpp`, `ColorDecomposeRecipe.*`.

**Decision (confirmed with the project owner):** extend Bambu's native system with Full
Spectrum's algorithms and modes instead of adding a second, parallel virtual-filament system.
Two systems would compete for filament slots, painting states, tool ordering and 3MF data. One
system stays mergeable with future Bambu releases and keeps MakerWorld projects compatible.

## 4. Gap analysis and status

| # | Requested feature | Bambu native | Full Spectrum source | Status here |
| --- | --- | --- | --- | --- |
| 1 | Virtual mixed filaments | Yes (slots) | `MixedFilamentManager` | Native |
| 2 | Ratio mode | Yes, N components | `WeightedBlend` | Native |
| 3 | **Cycle mode** | No | `Pattern.cpp`, `Resolver.cpp` | **Done** (`filament_mixed_pattern`, dialog row with sequence strip), single group |
| 4 | Match mode (HEX) | Yes (`ColorDecompose`) | `MixedColorMatchHelpers` + KM/KS model | Native matching; **Full Spectrum's spectral (KM/KS) estimate shown** under the dialog preview for ratio mixes |
| 5 | Gradient mode | 2 components, custom curve | N components, stops | **Done**: 3-filament gradients via Full Spectrum's sampler (engine, dialog, import) |
| 6 | Ratio recommendations | Yes (`recommend_from_physical_filaments`) | auto pairwise generation | **Done**: right-click "Add 50/50 mixes of all filament pairs" |
| 7 | Mixed-color previews | Yes | `Display.cpp` | Native |
| 8 | Layer alternation | Yes (deficit round-robin) | `Resolver.cpp` | Native |
| 9 | Painting / object assignment | Yes | gizmo | Native |
| 10 | Subdivide mix layer | Yes (`enable_mixed_color_sublayer`) | Local-Z | Native |
| 11 | Full-domain subdivision | Applies to all slot geometry | `dithering_local_z_whole_objects` | Native: Bambu always subdivides the whole slot geometry (Full Spectrum's override on); layer 0 is never split (Full Spectrum's "Keep first layer unsplit") |
| 12 | Infill subdivision | Always | `dithering_local_z_infill` | **Done**: `mixed_color_sublayer_infill` (default on = Bambu's behavior); off prints infill once per layer at full height |
| 13 | Gradient slicing + preview | Yes (2 comps) | `Gradient*.cpp` | 2 native; 3+ done (dialog preview uses the slicing sampler) |
| 14 | Minimum mix height | No | `mixed_filament_height_lower_bound` | **Engine done** (`mixed_color_sublayer_min_height`, Quality tab) |
| 15 | Save / reopen | Yes (project config) | FS 3MF JSON | Native; new keys persist via project config; **Full Spectrum projects import** (compact rows) |
| 16 | Compatibility warnings | Type consistency, integrity | | **Done**: dialog warns when a recipe needs ≥2 changes per layer or heavy purge |
| 17 | Usage / toolchange estimates | GCodeProcessor | | **Done**: changes per layer, load/unload time and purge weight per 100 layers in the dialog |
| – | Per-perimeter cycle groups | No | `resolve_perimeter` | **Not ported**: Full Spectrum assigns filaments per wall during wall generation; Bambu has no per-wall tool assignment. Imported projects approximate it and say so |
| – | Surface bias | No | `PerimeterModulation` | **Not ported**: part of Full Spectrum's image-map renderer (`ImageMap/`), a separate subsystem. Imported projects approximate it and say so |

## 5. Full Spectrum file inventory (reference)

Core: `MixedFilament.hpp`, `MixedFilament/{Manager,Resolver,Pattern,Gradient,GradientPreview,
Common,Definition,LegacyRow,Display,Preview,ColorNames,PerimeterModulation}.cpp`,
`filament_mixer.*`, `filament_mixer_model.h`, `FullSpectrum{KSPairResidual,ICCPolynomialEstimator}.*`,
`FullSpectrum*Profile.h`. Engine call sites: `PrintObjectSlice.cpp`, `MultiMaterialSegmentation.cpp`,
`PrintObject.cpp`, `GCode/ToolOrdering.cpp`, `GCode.cpp`, `PrintApply.cpp`, `LayerRegion.cpp`.
Persistence: `Format/FullSpectrum3mf/*`. GUI: `MixedFilamentDialog`, `MixedColorMatchPanel`,
`MFD*Accordion`, `MixedFilamentBatchDialog`, `Widgets/FilamentCardMixed`, `Widgets/HSLColorPicker`.

Not ported: image maps / texture perimeter modulation (`ImageMap/*`), the Snapmaker filament
color catalogue (`FilamentColorLibrary`), Snapmaker device code, U1 profiles.

## 6. Ported so far

### Cycle mode (`filament_mixed_pattern`)

* Per-slot project key, plumbed like `filament_mixed_gradient_curve` (load/export selections,
  resize, delete, AMS-sync snapshot, slot reordering).
* Syntax: steps are 1-based physical indices separated by `/` (or space, `-`, `_`); the compact
  Full Spectrum form `1121` (one character per step) and component letters `A`, `B`, ... are
  accepted. `,` separates per-perimeter groups (rejected for now, see below).
* Engine: `sequence[object_layer_index % size]`, using the object's own layer index exactly like
  Full Spectrum. A patterned slot is never split into sub-layers (Full Spectrum
  `mixed_filament_definition_uses_local_z`).
* Bambu adaptation: every step must be one of the slot's components, because filament grouping,
  H2D nozzle mapping and unprintable-area checks are derived from the component list. Full
  Spectrum allows arbitrary physical filaments in a pattern.
* Deleting a physical filament shifts pattern steps; a pattern that used the deleted filament is
  cleared (the slot is already flagged broken by Bambu's component remap).
* Tests: `tests/libslic3r/test_filament_mixer_pattern.cpp`.

### Vendored Full Spectrum core (`src/libslic3r/FullSpectrum/`)

* Upstream `MixedFilament*` and color-prediction code in `namespace Slic3r::FullSpectrum`; see
  its README for the exact local changes. Used for project import, gradient sampling and color
  prediction, never for the slicing pipeline itself.
* 64 upstream unit-test cases that exercise only this core are ported in
  `tests/libslic3r/test_full_spectrum_core.cpp`.

### Full Spectrum project import

* `Format/FullSpectrumProjectImport.*` reads `mixed_filament_definitions` and the Local-Z,
  gradient and dithering options from `Metadata/project_settings.config`, runs Full Spectrum's own
  loader exactly as Full Spectrum prepares a slice, and maps each visible row, in virtual-id order,
  to a native slot. Painted facets and object assignments therefore keep their mix.
* Layer-cadence rows (pairs, multi-color blends, manual patterns, "advanced dithering", height-
  weighted cadence) become cycle patterns sampled from Full Spectrum's resolver. Tests check that
  the native resolution equals Full Spectrum's on every layer.
* Local-Z rows map to sub-layer ratio mode, gradients to native gradients. Per-perimeter groups,
  surface bias and custom gradient stop positions are approximated and reported.
* GUI: `Sidebar::import_full_spectrum_mixed_filaments()` runs after a 3MF loads, creates the
  slots through the normal "Add mixed filament" path, enables sub-layers when needed and shows a
  notification listing any approximation. The vNext JSON (`/Metadata/fullspectrum/*.json`) is not
  read; Full Spectrum always writes the compact rows as well.

### Multi-filament gradients

* Gradient slots with 3 components follow Full Spectrum's multi-filament gradient
  (`FullSpectrum/GradientBridge`): adjacent pairs blend in sub-layers, inner filaments get a solid
  band (Full Spectrum's default 22 % middle window), stops come from the component weights.
* The two-filament gradient path, including the custom curve and per-part mode, is unchanged.
  Multi-filament gradients use one run over all objects of the slot, not per object.

### Minimum sublayer height (`mixed_color_sublayer_min_height`)

* Process option on the Quality tab next to "Mixed color sublayer", default 0 (off), so existing
  projects slice byte-identically. Full Spectrum's default is 0.06 mm.
* `clamp_mixed_sublayer_heights()` generalizes Full Spectrum's two-component
  `mixed_filament_local_z_pair_heights()` to N components. For two components it is checked
  against a verbatim copy of the Full Spectrum function over 2,772 input combinations.
* Ratio-mode sub-layers are clamped in `ToolOrdering`; components clamped to zero are removed
  from the group, so no empty pass or needless filament change is planned. Gradient heights are
  computed per object in `GCode.cpp` and clamped there; a zero-height pass is skipped (the filament
  change for it may still happen, which wastes some purge but never prints wrong geometry).
* Tests: `tests/libslic3r/test_filament_mixer_sublayer.cpp`.

## 7. Testing

* Unit tests (Catch2, `tests/libslic3r`): `test_filament_mixer_pattern`, `test_filament_mixer_sublayer`,
  `test_full_spectrum_core`, `test_full_spectrum_import`, `test_full_spectrum_gradient`.
* End-to-end: `scripts/full_spectrum/slice_check.py` slices with real Bambu Lab X1 Carbon presets
  (flattened by `flatten_bbl_preset.py`) and checks the filament printed on every layer, and runs
  the before/after regression against an unmodified build.

### Results (Linux build of this branch, 2026-10-24)

* `full_spectrum_tests` against the real libslic3r: **83 test cases, 26,327 assertions, all pass**
  (includes the 64 ported upstream Full Spectrum core cases).
* Cycle mode, X1 Carbon 0.4 + Bambu PLA Basic, 20 mm cube on a `1/1/2` slot: all 100 layers print
  the prescribed filament; 66 filament changes, each through the profile's `M620`/`M621` AMS block.
* Sub-layers with a 0.06 mm minimum, 75/25 mix at 0.20 mm: passes of 0.14 mm and 0.06 mm (layer 0
  unsplit), matching Full Spectrum's documented example.
* Regression: plain single-filament prints on filament 1 and 2 are byte-identical to the unmodified
  baseline apart from the two new keys in the config block, whenever both runs produce the same
  variant. Bambu Studio's own output varies slightly between runs (feedrate/slow-down lines), for
  the baseline as well as this branch, so single runs can differ in those lines.
* The GUI (dialog row, change estimate, project import notification) compiles but has not been
  exercised interactively; no physical prints have been made.

## 8. Bambu AMS adaptation

On the U1 a filament change is a cheap tool swap. On Bambu printers it is an AMS swap: cut,
retract, load, purge into the prime tower (flush volumes matrix). Bambu's native mixed feature
already emits these through the normal filament-change path, so no new motion or heater commands
are introduced by this port. The mixed-filament dialog estimates the cost of a recipe:
filament changes per layer, load and unload time (`machine_load_filament_time` +
`machine_unload_filament_time`), and, on single-nozzle printers, the purge weight from the project's
flush volumes (`flush_volumes_matrix` × `flush_multiplier`, PLA density). It turns orange and
suggests alternatives at 2 or more changes per layer or 300 mm³ of purge per layer. Nothing is
blocked: Bambu's existing checks still decide what a printer can print.

## 9. Validation plan

* Unit tests for every ported algorithm, with expectations taken from the Full Spectrum code.
* End-to-end slicing tests (`tests/fff_print`) checking per-layer tool sequences in G-code.
* Regression: slice fixed non-mixed and native-mixed projects with the unmodified baseline and
  with this branch; G-code must match apart from header timestamps.
* Physical color accuracy is **not** claimed until test prints are made.
