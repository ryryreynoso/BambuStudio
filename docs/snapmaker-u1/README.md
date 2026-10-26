# Snapmaker U1 support in Bambu Studio

Slicing for the Snapmaker U1 (Klipper, four toolheads) inside Bambu Studio, and sending jobs to it
over the local network through Moonraker, without changing how Bambu Lab printers slice. Snapmaker
account sign-in is a separate step that is not done yet.

## What was added

| Area | Change |
|---|---|
| Profiles | `resources/profiles/Snapmaker/` holds the U1 model, machines, processes and filaments, imported from Snapmaker Orca (AGPL-3.0) by `scripts/snapmaker/import_u1_profiles.py`. The script changes two settings: it sets `gcode_macro_language = "orca"` on `fdm_klipper`, and it sets `enable_prime_tower = 0` on `fdm_process_U1_common` (see "Known gaps"). |
| G-code macros | U1 templates use the OrcaSlicer macro language: `local` variables, writable `position[]`, `e_retracted[]` and `e_restart_extra[]`. `src/libslic3r/PlaceholderParserOrca.cpp` is a port of that parser. A printer selects it with `gcode_macro_language = "orca"`; when the setting is empty (every Bambu Lab printer), Bambu's own parser runs unchanged. `GCode::placeholder_parser_process_with_outputs` copies any changes a script makes to `position[]` and the retraction state back into the G-code writer. |
| Template variables | Exposed only when the Orca macro language is selected:<br>• `chamber_cooling_mode`, computed from the softening temperature of the plate's filaments as Snapmaker Orca does; this uses the new filament option `filament_is_high_temperature`.<br>• `temperature`, an alias of `nozzle_temperature`.<br>• `travel_speed` as a single number. Bambu stores it per extruder variant, and the U1 tool-change macro compares it with a number. |
| More than two extruders | Bambu assumes at most two physical extruders. These now size by the printer instead, with a minimum of two so Bambu printers are unchanged:<br>• filament grouping uses an identity tool map for toolchangers;<br>• the G-code writer's active-filament slots and `Extruder`'s shared E and retraction slots;<br>• `GCodeProcessor`'s per-extruder volume and filament state;<br>• the per-layer filament ordering, which dropped every filament on extruders 3 and 4;<br>• extruders without their own height limit use `printable_height`. |
| Network printing | New print host type `moonraker` (`src/slic3r/Utils/Moonraker.cpp`, adapted from Snapmaker Orca, AGPL-3.0). The connection test reads `printer/info`; the upload posts the G-code to `server/files/upload` (root `gcodes`) and sets `print=true` only when the user picks **Upload and Print**. If no port was entered and the host does not answer, port 8080 is tried, as Snapmaker Orca does. The U1 machine profiles use it. The connection dialog now keeps the printer profile's host type instead of always switching to OctoPrint. |
| Robustness | `Print::apply` no longer reads past a single-valued `filament_scarf_seam_type`. |

## Sending a job to the U1

1. Select a Snapmaker U1 printer and click the connection button next to it.
2. Enter the U1's IP address (host type Moonraker is preselected) and use **Test** to check it.
3. Slice, then use **Print → Send** and choose **Upload** or **Upload and Print**.

## Testing

`scripts/full_spectrum/slice_check.py u1 <bambu-studio> <workdir>` slices four cubes on a U1,
with cube *i* assigned to filament *i*. It fails if:
* any cube is printed by a tool other than its own;
* a tool is selected before it was heated;
* a tool other than T0–T3 appears;
* a custom G-code template fails.

`scripts/snapmaker/mock_moonraker.py <port> <log.json>` stands in for Moonraker. With it running,
`MOONRAKER_TEST_HOST=127.0.0.1:<port> full_spectrum_tests "[Moonraker]"` tests the connection
and uploads a file twice, once with each post-upload action.

Results on the Linux build (2026-10-26):

* `u1`: OK. 226 tool changes, each cube printed only by its own tool, slicing returns "Success".
* `regress`: X1 Carbon output from this build matches the unmodified build. Ignoring `M73` and
  `F` values, the unmodified build alternates between two outputs from run to run, and this
  build's output is identical to one of them.
* `cycle` and `sublayer` (Full Spectrum checks on the X1 Carbon): OK.
* Moonraker host against the mock: the connection test passes; the uploads arrive with root
  `gcodes`, path `bambu`, and `print=false` and then `print=true`; with no port given, both
  uploads reach the mock on port 8080.
* `full_spectrum_tests`: all 84 test cases (26,330 assertions) pass.

## Known gaps

* **Not compared with Snapmaker Orca output.** Snapmaker Orca could not be run in this
  environment. The U1 G-code was checked for structure (start sequence, tool-change macro,
  temperatures, tool usage), not line by line against Snapmaker's output.
* **Not printed.** Nothing has been run on a physical U1. Check the first jobs on the printer
  before relying on them.
* **Moonraker host tested only against a mock.** It has not reached a real U1. Snapmaker printers
  may require authorization that the mock does not; the host sends `printhost_apikey` as
  `X-Api-Key` when one is entered.
* **Ooze prevention does nothing.** Bambu's implementation is the older PrusaSlicer one and only
  works when a skirt is printed, which the U1 profiles do not do. Idle tools therefore stay at
  printing temperature. Snapmaker Orca drops idle tools to standby instead
  (`standby_temperature_delta`, `preheat_time`).
* **No prime tower.** Bambu's wipe tower is designed for one nozzle fed by an AMS. It is off in
  the U1 profiles, and Bambu refuses to combine it with ooze prevention.
* **`travel_speed` workaround.** Snapmaker Orca's macro engine also rejects a list where one
  number is expected, and no code that turns `travel_speed` into a single number was found in
  its source. The scalar value supplied here is what the U1 macro evidently expects, but it
  differs from Snapmaker Orca's code as found.

## Next steps

1. Snapmaker account sign-in through the official `snapmaker_control` helper installed with
   Snapmaker Orca.
