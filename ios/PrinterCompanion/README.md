# Printer Companion — iPhone and iPad

The Slice tab is a full 3D workspace on an on-device OrcaSlicer engine: import, edit, slice,
inspect real toolpaths and print, with no Mac or server. See `docs/mobile-studio.md` for what is
verified. Full Spectrum desktop editing is not ported.

## Printing

1. Add a printer on the Printers tab. Use a reachable private IP or VPN DNS name.
2. Import a model on Slice, select its exact printer and slice on the device.
3. Review warnings and export, or choose Print. Select destination, AMS tray/external spool
   (P2S) or U1 toolhead. Confirm material/nozzle/plate and that the bed is clear.
4. Confirm Start Print. The app uploads, sends one start request and checks acceptance.
   Upload alone never counts as acceptance. After a timeout, check the printer before retrying.

**P2S:** LAN Only + Developer Mode, serial number, current LAN access code, and accessible
storage are required. Verified implicit FTPS 990 uploads the sliced 3MF; verified MQTT 8883
sends `project_file`. No official signature is impersonated. No cloud login is required.
The app trusts Bambu printer CAs and verifies serial identity on both transports.

**U1:** Moonraker HTTP(S) upload with `print=false`, followed by the Snapmaker websocket
`server.files.start_local_print` request and actual `print_stats` filename/state checks.
Selected toolhead, leveling, flow calibration and timelapse are preserved in start options.
The backend is independent of Bambu. Do not expose Moonraker publicly; use a private VPN.

Status, progress, temperatures, AMS information and existing U1 camera snapshots remain.
P2S native LAN camera streaming is implemented in build 4; actual P2S video verification
is pending the printer liveview setting. Automatic discovery, cloud printing and multi-filament
model assignment are not implemented in this beta. Configure printer addresses manually.

## Away from home

Slicing runs offline. Printing remotely requires a secure route into the home network
(e.g. a router/NAS/Raspberry Pi providing a VPN subnet route). The Mac need not run, but
some home gateway must. No VPN is needed on home Wi-Fi. TestFlight distributes app builds;
it does not create printer connectivity or compile source changes automatically.
Manufacturer cloud printing requires a supported authorized integration, not merely a login.

## Security and storage

Printer configuration keeps the existing `printers.v1` format. Access codes stay in the
this-device-only iOS Keychain. A failed secure save prevents pairing and displays an error.
Credential-bearing URLs are rejected. TLS verification stays enabled; FTPS control and data
are encrypted. Production curl verbose logging is disabled. No secret keys or access codes
are bundled. No analytics or cloud slicing service is used.

## Build and verification

Requires Xcode and XcodeGen. For the simulator, run `python3 Scripts/build_simulator_engine.py`
once: it retags the real engine for the arm64 simulator, so simulator slices are real too.
That library is development-only and is not shipped or packaged.

```
python3 Scripts/verify_vendor.py
xcodegen generate
xcodebuild test -project PrinterCompanion.xcodeproj -scheme PrinterCompanion \
  -destination 'platform=iOS Simulator,name=<available iPhone>' CODE_SIGNING_ALLOWED=NO
python3 Tests/FTPSFixture/run.py
```

`StudioEngineTests` and `NativeSlicingTests` run the real engine with the bundled printer
profiles. They generate files only and never print. Close the app in the simulator first.
All other tests use local data or localhost FTPS/RTSPS fixtures. Physical printing requires
separate owner approval and a prepared printer. Do not equate build success with hardware verification.

## Source, licenses and rebuilding dependencies

This combined work is AGPL-3.0-only. Preserve the upstream lineage, bundled licenses and
`Vendor/*PROVENANCE.md`. Exact engine: OrcaPad release-0.19.0 / OrcaSlicer v2.4.2, ABI 42.
About includes license texts and the complete corresponding source package for the build.
The package contains this entire iOS project plus the engine's full source release archive,
OpenSSL 3.5.9 and curl 8.23.0 sources under Dependencies. It excludes user models, account
sessions, credentials and local Xcode user state. The large source bundle increases beta
size; it keeps source available alongside the distributed app without publishing private app data.

Extract the engine source archive and follow its BUILDING.md to rebuild the iOS engine
(Bridge/build-core.sh; Linux reference toolchain). Copy the generated xcframework to Vendor/.
For networking, extract the two source archives into one directory, set NETWORK_BUILD_ROOT
and run Scripts/build_network.sh on a Mac with Xcode/CMake. Copy its install-<sdk>/lib static
libraries to Vendor/FTPS/<sdk>/ and curl headers to NativeFTPS/curl. Update hash manifests
only when intentionally rebuilding verified source. `package_source.py` creates the complete
source package before distribution. Rebuild it after source changes; do not ship an old package.
Use your own Apple development team to install a modified build. No manufacturer's private
signing key or privileged networking plugin is needed.

## TestFlight

Bundle ID remains io.github.ryryreynoso.PrinterCompanion, preserving the installed app's
configuration. Version 0.2.0, build 4. Archive the Release scheme and distribute through
App Store Connect after tests pass, with the source package and privacy manifest bundled.
Apple account agreements, valid distribution signing and App Store Connect app access
must be current. Do not upload the old read-only 0.1.0 archive.

## P2S camera (build 4)

The printer detail screen now has native H.264 LAN video. Enable LAN Mode Liveview on the
P2S screen, and pair with its address, serial number and LAN access code. The shared
`../../shared/AppleCamera` Swift sources implement TLS 1.2+, Bambu CA + exact serial verification,
RTSP Digest authentication, RTP/TCP depacketization and Apple sample-buffer rendering. No
manufacturer cloud camera authorization is assumed. A private route to the home LAN is still
required away from home. Camera playback pauses when the app is backgrounded.

Run `python3 Tests/CameraFixture/run-tests.py` on macOS for local TLS authentication,
certificate rejection, H.264 decoding, disconnect/reconnect, timeout and cancellation tests.
Generated test certificates/keys remain in a temporary directory and are deleted afterward.
The fixture never connects to a printer. CameraTests runs in the normal iOS test scheme.

The source archive layout is `ios/PrinterCompanion/` plus `shared/AppleCamera/`; preserve
both directories when extracting and run XcodeGen inside `ios/PrinterCompanion`.
Hardware caveat: the owner's P2S rejects TCP port 322 with LAN liveview unavailable.
Until that setting is enabled, live video on that physical P2S cannot be verified.

## Optional U1 home relay (build 5)

U1 pairing now accepts an optional HTTPS `/v1/u1` relay URL and a device-Keychain token.
It falls back from a failed LAN status probe before upload, and then keeps the selected
transport for the complete print attempt. Existing acceptance checks are unchanged.
The relay helper and deployment prerequisites are in `../../tools/home-relay` from the
repository root layout. This is a development feature: no public endpoint is configured,
no cellular print is verified, and the home Mac must remain awake. Bambu cloud/relay access
is not enabled by this feature. U1 remote camera currently means JPEG snapshots.

### U1 relay pairing (0.2.0 build 6)

Saved U1 printers now have **Remote access** settings: import a private JSON pairing file,
test the HTTPS route explicitly, save it, or remove it without deleting the LAN printer.
Pairing tokens remain in device-only Keychain. Binary JSON WebSocket starts are now supported
by the Mac relay, matching URLSession's actual send format. No physical print has been tested.
A stable internet endpoint and an awake home Mac are still required; TestFlight distribution
alone does not create remote printer connectivity. See `tools/home-relay/README.md`.
