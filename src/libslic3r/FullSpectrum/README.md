# Vendored Full Spectrum core

Source: https://github.com/ratdoux/OrcaSlicer-FullSpectrum at `cc152a84dd` (FullSpectrum
0.9.14, 2026-09-27), `src/libslic3r/MixedFilament*` and `src/libslic3r/FullSpectrum*`.
License: AGPL-3.0, the same as Bambu Studio.

Local changes, kept minimal so upstream updates stay a directory diff:

* Everything in `MixedFilament*` lives in `namespace Slic3r::FullSpectrum` so it cannot clash
  with Bambu Studio's native mixed-filament code.
* Include paths adjusted for the subdirectory. Upstream `filament_mixer.h` is replaced by Bambu's
  `FilamentMixer.hpp`; both declare the same `filament_mixer_lerp*` functions over the same
  MIT-licensed pigment model (`FilamentMixerModel.hpp` is coefficient-identical to upstream
  `filament_mixer_model.h`).
* `MAXIMUM_FILAMENT_NUMBER` (from upstream `libslic3r.h`) is defined in `MixedFilament.hpp`.

This code is used to read Full Spectrum project data and for Full Spectrum color prediction.
Slicing uses Bambu Studio's native mixed-filament engine (`FilamentMixer`, `ToolOrdering`).
Tests: `tests/libslic3r/test_full_spectrum_core.cpp` (upstream cases that only use this core).
