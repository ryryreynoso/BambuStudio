#!/usr/bin/env python3
"""Flatten a Bambu Lab system preset into the self-contained JSON the CLI loads.

Resolves the "inherits" chain and "include" templates of a preset under
resources/profiles/BBL/{machine,process,filament}. Used by the Full Spectrum
slicing checks (scripts/full_spectrum/slice_check.py); not shipped.

usage: flatten_bbl_preset.py <resources/profiles/BBL> <machine|process|filament> <preset name> <out.json>
"""
import json
import os
import sys


def load_index(folder):
    index = {}
    for name in os.listdir(folder):
        if name.endswith(".json"):
            with open(os.path.join(folder, name), encoding="utf-8-sig") as f:
                data = json.load(f)
            index[data.get("name", name[:-5])] = data
    return index


def resolve(index, name, seen=()):
    if name in seen:
        raise ValueError("inheritance cycle at " + name)
    data = index[name]
    out = {}
    parent = data.get("inherits")
    if parent:
        out.update(resolve(index, parent, seen + (name,)))
    for inc in data.get("include", []):
        out.update({k: v for k, v in resolve(index, inc, seen + (name,)).items()})
    out.update(data)
    return out


def main():
    root, kind, name, out_path = sys.argv[1:5]
    index = load_index(os.path.join(root, kind))
    flat = resolve(index, name)
    for key in ("inherits", "include", "instantiation"):
        flat.pop(key, None)
    flat["name"] = name
    flat["from"] = "system"
    flat["type"] = kind
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(flat, f, indent=2)


if __name__ == "__main__":
    main()
