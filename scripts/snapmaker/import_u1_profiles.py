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


def load(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)


def is_u1(data):
    if "U1" in data.get("name", ""):
        return True
    return any("Snapmaker U1" in p for p in data.get("compatible_printers", []))


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
                shutil.copy(os.path.join(src, "Snapmaker", item["sub_path"]), os.path.join(out_dir, item["sub_path"]))
                index[kind + "_list"].append(item)

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
