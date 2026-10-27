# Handoff: Bambu Studio + Full Spectrum + Snapmaker U1 (moving to a Mac)

Branch `feature/full-spectrum` in `ryryreynoso/BambuStudio`, based on upstream Bambu Studio
commit `79df7ae`. This file says what is on the branch, what is verified, how to build and test it
on an Apple Silicon Mac, and what is left.

## 1. What you need

| Item | Where |
|---|---|
| The code | `bambu-studio-fs-u1-<N>commits.bundle` (sent in chat), or the pushed branch once it is on GitHub |
| macOS on Apple Silicon, ~40 GB free disk | Dependencies alone are large |
| Xcode 16 (with command-line tools) | App Store; then `xcode-select --install` |
| Homebrew | https://brew.sh |
| Build tools | `brew install cmake ninja git gettext libtool automake autoconf texinfo pkgconf` |
| XcodeGen (iPhone app only) | `brew install xcodegen` |
| Snapmaker Orca (for Snapmaker sign-in) | Installs the `snapmaker_control_macos_arm64` helper this branch uses |
| Your printers on the same Wi-Fi | U1 IP address; P2S IP, serial number and access code (LAN mode + developer mode) |
| Claude Code on the Mac (optional) | Desktop app or `claude` CLI, opened in the repository folder |

## 2. Get the code onto the Mac

Bundle in `~/Downloads`:

```sh
git clone https://github.com/ryryreynoso/BambuStudio.git ~/BambuStudio
cd ~/BambuStudio
git fetch https://github.com/bambulab/BambuStudio.git master        # supplies base commit 79df7ae
git bundle verify ~/Downloads/bambu-studio-fs-u1-*commits.bundle
git fetch ~/Downloads/bambu-studio-fs-u1-*commits.bundle feature/full-spectrum:feature/full-spectrum
git checkout feature/full-spectrum
git log -1 --oneline                                                  # matches the tip given in chat
git push -u origin feature/full-spectrum                              # your own GitHub login; never to master
```

Pushing starts two GitHub Actions workflows on the fork: **Build Full Spectrum** (macOS arm64 and
Windows) and **iPhone companion app**.

## 3. Build the desktop app (Apple Silicon)

```sh
cd ~/BambuStudio
./BuildMac.sh -d -a arm64 -x      # dependencies, once (long)
./BuildMac.sh -s -a arm64 -x      # the app
```

The app lands under `build/arm64/BambuStudio/BambuStudio.app` (the script prints the path). For
the unit tests, configure with `-DSLIC3R_BUILD_TESTS=ON` and build the `full_spectrum_tests`
target. The Linux session also patched `deps/` locally to mirror blocked GitHub downloads; that
patch is not on the branch and is not needed on a Mac.

## 4. Build the iPhone app

```sh
cd ~/BambuStudio/ios/PrinterCompanion
xcodegen generate
open PrinterCompanion.xcodeproj    # pick your team under Signing & Capabilities, run on the iPhone
```

It has never been compiled: Swift was not available in the cloud session. Expect a few compile
errors on the first build; fix them there.

## 5. What the branch adds

**Full Spectrum color mixing**, built on Bambu's own mixed filaments (`docs/full-spectrum/PORT_PLAN.md`):
* Cycle mode (`filament_mixed_pattern`) and the minimum sub-layer height.
* Multi-filament gradients and the sub-layer infill toggle (`mixed_color_sublayer_infill`).
* Full Spectrum 3MF project import, plus the "add 50/50 pairs" helper.
* Dialog extras: Full Spectrum's spectral color estimate, filament-change, time and purge
  estimates, and a cost warning.

**Snapmaker U1** (`docs/snapmaker-u1/README.md`):
* U1 profiles, and the OrcaSlicer macro language selected per printer
  (`gcode_macro_language = "orca"`).
* Support for more than two extruders, with a test that each tool prints its own object.
* Idle-tool standby with preheat before the next use.
* Network printing through a Moonraker print host.
* **File → Snapmaker Account and Devices…** through Snapmaker's own helper.

**iPhone companion app** (`ios/PrinterCompanion/README.md`): read-only status for the U1
(Moonraker) and Bambu printers over LAN.

Bambu Lab printers, accounts, MakerWorld, AMS, painting and 3MF are left as they are. The
regression check (X1 Carbon) gives the same G-code as unmodified Bambu Studio.

## 6. Verified so far (Linux build, 2026-10-27)

* `full_spectrum_tests`: 87 test cases, 26,339 assertions pass.
* `scripts/full_spectrum/slice_check.py` checks:
  * `cycle`, `sublayer`: pass.
  * `regress`: matches the unmodified build.
  * `u1`: pass. 226 tool changes, each cube on its own tool; 225 standby commands and 225
    correctly ordered preheats.
* The Moonraker host passes against `scripts/snapmaker/mock_moonraker.py`.
* The Snapmaker sign-in code passes against `scripts/snapmaker/fake_snapmaker_helper.py`.

## 7. Not verified yet — do these on the Mac

1. The Mac app builds and opens. Click through:
   * the mixed-filament dialog (cycle row, spectral estimate, cost warning);
   * the Quality tab sub-layer options.
2. **Snapmaker sign-in** with the real helper: File → Snapmaker Account and Devices…, sign in,
   check that the Devices page lists the U1.
3. **U1 printing**: connection button → host type Moonraker → U1 IP → Test, then Print → Send →
   Upload. Watch the first print from start to finish: start sequence, tool changes, standby and
   preheat temperatures.
4. **P2S**: slice and print a normal job through Bambu's own flow, to confirm nothing changed.
5. **Color mixing**: a physical test print. No color accuracy is claimed until then.
6. iPhone app: build, add the U1 and the P2S, check that status and temperatures update.

## 8. Not done

* Full Spectrum per-wall color groups and surface bias are not ported; imported projects
  approximate them and say so (`PORT_PLAN.md` §4).
* U1 prime tower is off: Bambu's tower is not verified on a toolchanger.

## 9. Ground rules carried over

* Never push to `master` or to `bambulab/BambuStudio`, and never force-push.
* Respect AGPL-3.0: code adapted from Snapmaker Orca and Full Spectrum keeps its attribution.
* Snapmaker's helper is used from the user's own Snapmaker Orca install, never redistributed.
* No hard-coded tokens and no plaintext credentials. TLS stays verified.
* No new motion or heater commands without testing them.

## 10. Prompt for a new Claude Code session on the Mac

> I'm continuing work on branch feature/full-spectrum of ryryreynoso/BambuStudio in
> ~/BambuStudio. Read HANDOFF.md, docs/full-spectrum/PORT_PLAN.md and
> docs/snapmaker-u1/README.md first.
>
> Then:
> 1. Build the Apple Silicon app with BuildMac.sh and the unit tests (full_spectrum_tests).
> 2. Build the iPhone app in ios/PrinterCompanion with XcodeGen and fix any compile errors.
> 3. Help me run the "Not verified yet" checklist in HANDOFF.md §7 against my Snapmaker U1, my
>    Bambu P2S and my Snapmaker account.
>
> Follow the ground rules in §9. Commit fixes to feature/full-spectrum and push only that branch.
