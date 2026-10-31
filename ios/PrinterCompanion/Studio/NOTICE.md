# Studio sources

`OrcaBridge/` and `OrcaStudio/` come from OrcaPad release-0.19.0 (commit 20e698819e6b),
AGPL-3.0-only, © OrcaPad contributors, the same release as `Vendor/OrcaBridgeCore.xcframework`.
Its files keep their SPDX and copyright headers. `LICENSE` is OrcaPad's licence text.
`Resources/` holds OrcaPad's settings schema, test models and the Sarabun font (SIL OFL 1.1).

Changes for this app:
* Built as two static modules beside the existing app, entered through `StudioRootView`.
* A phone-first shell, tool dock, printer switcher, quick settings and floating Slice button.
* One workspace for editing and preview; settings in a half-height sheet.
* New lighting, bed surface, shadows and backdrop (`Viewer/Shaders.swift`, `Viewer/Backdrop.swift`).
* Printing goes through this app's own P2S and U1 flows, not OrcaPad's device tab.
* Filament assignment uses the engine's per-feature `*_filament_id` keys.
