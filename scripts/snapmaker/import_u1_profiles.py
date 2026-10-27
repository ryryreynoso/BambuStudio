#!/usr/bin/env python3
"""Import the Snapmaker U1 system profiles from Snapmaker Orca into resources/profiles/Snapmaker.

Copies only what the U1 needs: the "Snapmaker U1" printer model, every machine, process and
filament profile that is a U1 variant or compatible with one, the base profiles they inherit
from or include, and the referenced bed model, texture and cover images. Writes a Snapmaker.json
vendor index listing exactly those files. Profiles are AGPL-3.0 (Snapmaker Orca).

usage: import_u1_profiles.py <Snapmaker Orca repo> <Bambu Studio repo>
"""
import json
import os
import shutil
import sys

KINDS = ("machine", "process", "filament")
# Snapmaker's G-code templates use the OrcaSlicer macro language (local variables, writable
# position[] and so on); this printer setting selects that engine for every Snapmaker machine.
MACRO_LANGUAGE_BASE = "fdm_klipper"
# Settings changed from Snapmaker's defaults, per profile. Bambu Studio's prime tower is built for
# single-nozzle AMS printers and is not verified on a toolchanger, and Bambu rejects it together
# with ooze prevention (which every U1 process enables), so the U1 profiles default to no tower;
# the U1 primes each tool with its own SM_PRINT_PREEXTRUDE_FILAMENT macro on tool change.
OVERRIDES = {
    MACRO_LANGUAGE_BASE: {"gcode_macro_language": "orca"},
    "fdm_process_U1_common": {"enable_prime_tower": "0"},
}
# Snapmaker Orca lists the U1 as "octoprint" but talks to it through its own Moonraker client;
# Bambu Studio's Moonraker host does the same, without relying on Moonraker's OctoPrint emulation.
for _nozzle in ("0.2", "0.4", "0.6", "0.8"):
    OVERRIDES["Snapmaker U1 (%s nozzle)" % _nozzle] = {"host_type": "moonraker"}


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


def copy_without_bom(src_path, dst_path):
    # Some Snapmaker profiles start with a UTF-8 byte-order mark, which Bambu Studio's profile
    # loader rejects ("Failed loading configuration file").
    with open(src_path, "rb") as f:
        data = f.read()
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    with open(dst_path, "wb") as f:
        f.write(data)


def is_u1(data):
    if "U1" in data.get("name", ""):
        return True
    return any("Snapmaker U1" in p for p in data.get("compatible_printers", []))


def add_missing_filament_ids(out_dir, filament_list):
    """Give every instantiated filament a filament_id, which Bambu Studio requires.

    Snapmaker Orca leaves many U1 filaments without one; Bambu Studio then refuses to load them
    ("Can not find filament_id"). The id goes on the filament's own base profile (the highest
    ancestor that is not a shared fdm_* base), so its nozzle variants share it, as Bambu's do.
    """
    paths = {item["name"]: os.path.join(out_dir, item["sub_path"]) for item in filament_list}
    data = {name: load(path) for name, path in paths.items()}

    def chain(name):
        names = []
        while name in data:
            names.append(name)
            name = data[name].get("inherits")
        return names

    owners = set()
    for name, profile in data.items():
        ancestors = chain(name)
        if profile.get("instantiation") != "true" or any(data[a].get("filament_id") for a in ancestors):
            continue
        owners.add([a for a in ancestors if not a.startswith("fdm_")][-1])

    taken = {p["filament_id"] for p in data.values() if p.get("filament_id")}
    for owner in sorted(owners):
        base = owner.replace("@U1 base", "").strip()
        filament_id = "U1_" + "".join(c if c.isalnum() else "_" for c in base.upper()).strip("_")
        assert filament_id not in taken, filament_id
        taken.add(filament_id)
        data[owner]["filament_id"] = filament_id
        with open(paths[owner], "w", encoding="utf-8") as f:
            json.dump(data[owner], f, indent=4)
            f.write("\n")
    return len(owners)


def rename_bambu_name_clashes(out_dir, filament_list, bambu_names):
    """Rename U1 filaments that share a name with a Bambu Lab filament, e.g. "Generic PLA".

    Bambu Studio refuses to load a second preset with a name already loaded ("duplicated
    defines"). Snapmaker names its 0.4 mm variants plainly and the others "<name> @U1 <d> nozzle";
    the clashing ones become "<name> @U1 0.4 nozzle" to match.
    """
    renamed = 0
    for item in filament_list:
        if item["name"] not in bambu_names:
            continue
        path = os.path.join(out_dir, item["sub_path"])
        data = load(path)
        if data.get("instantiation") != "true":
            continue  # base profiles are never compared with another bundle's presets
        printers = data.get("compatible_printers", [])
        assert len(printers) == 1 and printers[0].startswith("Snapmaker U1 ("), (item["name"], printers)
        nozzle = printers[0][len("Snapmaker U1 ("):-1]  # "0.4 nozzle"
        new_name = "%s @U1 %s" % (item["name"], nozzle)
        new_sub_path = os.path.join(os.path.dirname(item["sub_path"]), new_name + ".json")
        data["name"] = new_name
        os.remove(path)
        with open(os.path.join(out_dir, new_sub_path), "w", encoding="utf-8") as f:
            json.dump(data, f, indent=4)
            f.write("\n")
        item["name"], item["sub_path"] = new_name, new_sub_path
        renamed += 1
    return renamed


def main():
    src_repo, dst_repo = sys.argv[1:3]
    src = os.path.join(src_repo, "resources", "profiles")
    dst = os.path.join(dst_repo, "resources", "profiles")
    vendor = load(os.path.join(src, "Snapmaker.json"))

    # name -> (kind, sub_path, data) for every profile the vendor index lists
    entries = {}
    for kind in KINDS:
        for item in vendor[kind + "_list"]:
            data = load(os.path.join(src, "Snapmaker", item["sub_path"]))
            entries[item["name"]] = (kind, item["sub_path"], data)
    models = [m for m in vendor["machine_model_list"] if m["name"] == "Snapmaker U1"]

    wanted = {name for name, (_, _, data) in entries.items() if is_u1(data)}
    # Close over inherits/include so every selected profile resolves.
    stack = list(wanted)
    while stack:
        data = entries[stack.pop()][2]
        for parent in [data.get("inherits")] + list(data.get("include", [])):
            if parent and parent in entries and parent not in wanted:
                wanted.add(parent)
                stack.append(parent)

    out_dir = os.path.join(dst, "Snapmaker")
    if os.path.isdir(out_dir):
        shutil.rmtree(out_dir)
    for kind in KINDS:
        os.makedirs(os.path.join(out_dir, kind), exist_ok=True)

    index = {k: vendor[k] for k in ("name", "version", "force_update", "description")}
    index["description"] = "Snapmaker U1 configurations (from Snapmaker Orca, AGPL-3.0)"
    index["machine_model_list"] = models
    for kind in KINDS:
        index[kind + "_list"] = []
    for model in models:
        shutil.copy(os.path.join(src, "Snapmaker", model["sub_path"]), os.path.join(out_dir, model["sub_path"]))
    # Keep the vendor's own ordering so parents are listed before children.
    for kind in KINDS:
        for item in vendor[kind + "_list"]:
            if item["name"] in wanted:
                dst_path = os.path.join(out_dir, item["sub_path"])
                if item["name"] in OVERRIDES:
                    data = dict(entries[item["name"]][2])
                    data.update(OVERRIDES[item["name"]])
                    with open(dst_path, "w", encoding="utf-8") as f:
                        json.dump(data, f, indent=4)
                        f.write("\n")
                else:
                    copy_without_bom(os.path.join(src, "Snapmaker", item["sub_path"]), dst_path)
                index[kind + "_list"].append(item)
    bambu = load(os.path.join(dst, "BBL.json"))
    rename_bambu_name_clashes(out_dir, index["filament_list"], {f["name"] for f in bambu["filament_list"]})
    add_missing_filament_ids(out_dir, index["filament_list"])

    # Bed model, bed texture and cover images referenced by the U1 model and machines.
    assets = {"Snapmaker U1_cover.png"}
    for model in models:
        model_data = load(os.path.join(src, "Snapmaker", model["sub_path"]))
        for key in ("bed_model", "bed_texture", "hotend_model"):
            if model_data.get(key):
                assets.add(model_data[key])
    for name in wanted:
        kind, _, data = entries[name]
        if kind == "machine":
            for key in ("bed_model", "bed_texture"):
                if data.get(key):
                    assets.add(data[key])
    for asset in sorted(assets):
        path = os.path.join(src, "Snapmaker", asset)
        if os.path.exists(path):
            shutil.copy(path, os.path.join(out_dir, asset))

    with open(os.path.join(dst, "Snapmaker.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=4)
        f.write("\n")
    print("models:", [m["name"] for m in models])
    for kind in KINDS:
        print(kind, len(index[kind + "_list"]))
    print("assets:", sorted(a for a in assets if os.path.exists(os.path.join(src, "Snapmaker", a))))


if __name__ == "__main__":
    main()
