# Printer Companion (iPhone)

A read-only iPhone and iPad app that shows the status of your printers on the same Wi-Fi network:

* **Snapmaker U1 and other Klipper printers**, through Moonraker (`printer/objects/query`): state,
  job, progress, remaining time, layer, and the temperature of each tool and the bed.
* **Bambu Lab printers in LAN mode**, through the printer's local MQTT broker (port 8883, user
  `bblp`, the access code from the printer's screen): state, job, progress, remaining time, layer,
  nozzle, bed and chamber temperatures.

It only reads. It never sends commands that move, heat, pause or start anything. Towards a Bambu
printer, the only message it sends is the "pushall" status request.

## Security

* **Bambu printers (TLS):** the connection is verified, not bypassed. The app trusts only Bambu's
  printer certificate authorities, copied from Bambu Studio's `resources/cert/printer.cer`. The
  printer's certificate must also carry the serial number you entered.
* **Moonraker (plain HTTP):** allowed for local-network addresses only (`NSAllowsLocalNetworking`).
  App Transport Security stays on for everything else.
* **Access codes:** stored in the iOS Keychain (this device only, after first unlock), never in
  plain files.

## Building

Requires a Mac with Xcode 16 and [XcodeGen](https://github.com/yonaskolb/XcodeGen):

```sh
brew install xcodegen
cd ios/PrinterCompanion
xcodegen generate
open PrinterCompanion.xcodeproj
```

Choose your Apple developer team under Signing & Capabilities to run it on an iPhone.
`.github/workflows/ios_companion.yml` builds the app and runs the unit tests on GitHub's macOS
runners (simulator, unsigned).

## Status

* The code was written without a Swift compiler, so the first CI run is the first compile.
* It has not been tried against a real printer.
* Newer Bambu firmware only allows local MQTT with LAN mode and developer mode turned on.
