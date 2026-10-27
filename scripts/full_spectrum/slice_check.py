#!/usr/bin/env python3
"""End-to-end slicing checks for the Full Spectrum port, using real Bambu Lab presets.

Slices with the command-line slicer and inspects the G-code:
  * which physical filament extrudes on each layer (from T commands and extrusion moves),
  * the AMS filament-change blocks (M620/M621) around every tool change,
  * byte-for-byte regression against a baseline binary (header timestamps ignored).

usage:
  slice_check.py cycle    <bambu-studio> <workdir>          # cycle pattern on X1C + AMS
  slice_check.py sublayer <bambu-studio> <workdir>          # ratio mix with sub-layers + min height
  slice_check.py regress  <baseline-bin> <feature-bin> <workdir>
  slice_check.py u1       <bambu-studio> <workdir>          # 4-tool Snapmaker U1 job, one cube per tool
"""
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
BBL = os.path.join(REPO, "resources", "profiles", "BBL")
SNAPMAKER = os.path.join(REPO, "resources", "profiles", "Snapmaker")

PRINTER = "Bambu Lab X1 Carbon 0.4 nozzle"
PROCESS = "0.20mm Standard @BBL X1C"
FILAMENT = "Bambu PLA Basic @BBL X1C"


def flatten(kind, name, out, root=BBL):
    subprocess.check_call([sys.executable, os.path.join(HERE, "flatten_bbl_preset.py"), root, kind, name, out])


def write_cube(path, size=20.0):
    s = size
    v = [(0, 0, 0), (s, 0, 0), (s, s, 0), (0, s, 0), (0, 0, s), (s, 0, s), (s, s, s), (0, s, s)]
    faces = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
             (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    with open(path, "w") as f:
        f.write("solid cube\n")
        for a, b, c in faces:
            f.write(" facet normal 0 0 0\n  outer loop\n")
            for i in (a, b, c):
                f.write("   vertex %f %f %f\n" % v[i])
            f.write("  endloop\n endfacet\n")
        f.write("endsolid cube\n")


def prepare(workdir, colours):
    os.makedirs(workdir, exist_ok=True)
    machine = os.path.join(workdir, "machine.json")
    process = os.path.join(workdir, "process.json")
    flatten("machine", PRINTER, machine)
    flatten("process", PROCESS, process)
    base = os.path.join(workdir, "filament_base.json")
    flatten("filament", FILAMENT, base)
    filaments = []
    for i, colour in enumerate(colours):
        data = json.load(open(base))
        data["filament_colour"] = [colour]
        path = os.path.join(workdir, "filament_%d.json" % (i + 1))
        json.dump(data, open(path, "w"), indent=2)
        filaments.append(path)
    cube = os.path.join(workdir, "cube.stl")
    write_cube(cube)
    return machine, process, filaments, cube


def slice_(binary, workdir, machine, process, filaments, model, object_filament, extra, models=None):
    out = os.path.join(workdir, "out")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    cmd = [binary, "--slice", "0", "--load-settings", machine + ";" + process,
           "--load-filaments", ";".join(filaments),
           "--load-filament-ids", str(object_filament), "--outputdir", out] + extra + (models or [model])
    print("$", " ".join(cmd))
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log = os.path.join(workdir, "slice.log")
    open(log, "w").write(res.stdout)
    gcodes = [os.path.join(out, f) for f in os.listdir(out) if f.endswith(".gcode")]
    if res.returncode != 0 or not gcodes:
        raise SystemExit("slicing failed (exit %d), see %s" % (res.returncode, log))
    return gcodes[0]


def layers_and_tools(gcode_path):
    """[(z, [tools that extruded on the layer, in order])], tool changes, AMS blocks."""
    layers, current, tool = [], None, None
    tool_changes, m620 = 0, 0
    for line in open(gcode_path, errors="replace"):
        line = line.strip()
        m = re.match(r"; Z_HEIGHT: ([0-9.]+)", line)
        if m:
            current = (float(m.group(1)), [])
            layers.append(current)
            continue
        m = re.match(r"^T(\d+)\b", line)
        if m and int(m.group(1)) < 255:
            if tool is not None and int(m.group(1)) != tool:
                tool_changes += 1
            tool = int(m.group(1))
            continue
        if line.startswith("M620 S") and not line.startswith("M620 S255"):
            m620 += 1
        if current is not None and tool is not None and re.match(r"^G1 .*E[0-9.]+", line) and " E-" not in line:
            if not current[1] or current[1][-1] != tool:
                current[1].append(tool)
    return layers, tool_changes, m620


def mixed_settings(n_physical, components, ratios="", pattern="", gradient=False, gradient_range=""):
    """Project keys declaring one mixed slot after n_physical physical filaments. The CLI only
    accepts its own options, so these are merged into the process settings file."""
    def strings(last):
        return [""] * n_physical + [last]
    def bools(last):
        return ["0"] * n_physical + ["1" if last else "0"]
    return {"filament_is_mixed": bools(True),
            "filament_mixed_components": strings(components),
            "filament_mixed_sublayer_ratios": strings(ratios),
            "filament_mixed_gradient": bools(gradient),
            "filament_mixed_gradient_range": strings(gradient_range),
            "filament_mixed_gradient_curve": strings(""),
            "filament_mixed_gradient_per_part": bools(False),
            "filament_mixed_pattern": strings(pattern)}


def with_settings(process, extra):
    data = json.load(open(process))
    # The CLI does not place the prime tower for a bare STL; its default position ends up off
    # the bed and the G-code check rejects the plate. Purge into the printer's chute instead.
    data["enable_prime_tower"] = "0"
    data.update(extra)
    json.dump(data, open(process, "w"), indent=2)


def check_cycle(binary, workdir):
    machine, process, filaments, cube = prepare(workdir, ["#FF0000", "#0000FF", "#800080"])
    with_settings(process, mixed_settings(2, "1,2", "0.6667,0.3333", pattern="1/1/2"))
    gcode = slice_(binary, workdir, machine, process, filaments, cube, 3, [])
    layers, changes, m620 = layers_and_tools(gcode)
    expected = [0, 0, 1]  # T indices are 0-based: filament 1 -> T0
    bad = []
    for i, (z, tools) in enumerate(layers):
        want = expected[i % 3]
        if tools != [want]:
            bad.append((i, z, tools, want))
    print("layers: %d, tool changes: %d, AMS change blocks: %d" % (len(layers), changes, m620))
    print("first layers:", [t for _, t in layers[:9]])
    if bad:
        print("MISMATCHES (layer, z, tools, expected):", bad[:10])
        raise SystemExit(1)
    print("OK: every layer printed with the filament the 1/1/2 cycle prescribes")


def check_sublayer(binary, workdir):
    machine, process, filaments, cube = prepare(workdir, ["#FF0000", "#0000FF", "#800080"])
    extra = mixed_settings(2, "1,2", "0.7500,0.2500")
    extra.update({"enable_mixed_color_sublayer": "1", "mixed_color_sublayer_min_height": "0.06"})
    with_settings(process, extra)
    gcode = slice_(binary, workdir, machine, process, filaments, cube, 3, [])
    layers, changes, m620 = layers_and_tools(gcode)
    print("layers: %d, tool changes: %d, AMS change blocks: %d" % (len(layers), changes, m620))
    print("first layers:", [(z, t) for z, t in layers[:6]])


def check_u1(binary, workdir):
    """Four cubes on a Snapmaker U1, cube i assigned to filament i. Every cube must be printed by its
    own tool on every layer, each tool change must come from the U1 change_filament_gcode (heat the
    next tool, then select it), and no custom G-code template may fail."""
    os.makedirs(workdir, exist_ok=True)
    machine = os.path.join(workdir, "machine.json")
    process = os.path.join(workdir, "process.json")
    flatten("machine", "Snapmaker U1 (0.4 nozzle)", machine, SNAPMAKER)
    flatten("process", "0.20mm Standard @Snapmaker U1 (0.4 nozzle)", process, SNAPMAKER)
    if json.load(open(machine)).get("gcode_macro_language") != "orca":
        raise SystemExit("the U1 machine profile does not select the OrcaSlicer macro language")
    with_settings(process, {})
    base = os.path.join(workdir, "filament_base.json")
    flatten("filament", "Generic PLA", base, SNAPMAKER)
    filaments = []
    for i, colour in enumerate(["#FF0000", "#00FF00", "#0000FF", "#FFFF00"]):
        data = json.load(open(base))
        data["filament_colour"] = [colour]
        path = os.path.join(workdir, "filament_%d.json" % (i + 1))
        json.dump(data, open(path, "w"), indent=2)
        filaments.append(path)
    cube = os.path.join(workdir, "cube.stl")
    write_cube(cube, 15.0)
    gcode = slice_(binary, workdir, machine, process, filaments, cube, "1,2,3,4", [], models=[cube] * 4)

    tool, heated, problems = None, set(), []
    # Ooze prevention (standby + preheat): per tool, a standby M104 must be followed by a preheat
    # before the tool is selected again; a preheat must never come before the standby it undoes.
    standby_temp = None
    state = {}  # tool -> "standby" | "preheated"
    standby_count = preheat_count = 0
    proc = json.load(open(process))
    filament_temp = int(json.load(open(filaments[0]))["nozzle_temperature"][0])
    delta = int(proc.get("standby_temperature_delta", 0))
    if str(proc.get("ooze_prevention", "0")) == "1" and delta != 0:
        standby_temp = filament_temp + delta
    objects = {}  # object index -> set of tools that printed it
    changes = 0
    for n, line in enumerate(open(gcode, errors="replace"), 1):
        line = line.strip()
        if line.startswith("!!!!!"):
            problems.append("line %d: %s" % (n, line))
        m = re.match(r"^M10[49] (?:S\d+ T(\d+)|T(\d+) S\d+)", line)
        if m:
            heated.add(int(m.group(1) or m.group(2)))
        m = re.match(r"^M104 S(\d+) T(\d+) ; set nozzle temperature$", line)
        if m and tool is not None and int(m.group(2)) == tool and standby_temp is not None and int(m.group(1)) == standby_temp:
            state[tool] = "standby"
            standby_count += 1
        m = re.match(r"^M104 T(\d+) S(\d+) ; preheat next tool$", line)
        if m:
            t = int(m.group(1))
            # Allowed before a tool's first use (the start G-code leaves it off) or after its standby.
            if state.get(t) not in (None, "standby"):
                problems.append("line %d: T%d preheated while it was not on standby" % (n, t))
            state[t] = "preheated"
            preheat_count += 1
        m = re.match(r"^T(\d+)$", line)
        if m:
            t = int(m.group(1))
            if t > 3:
                problems.append("line %d: tool T%d does not exist on the U1" % (n, t))
            if t != tool:
                changes += 1
                if t not in heated:
                    problems.append("line %d: T%d selected before it was heated" % (n, t))
                if state.get(t) == "standby":
                    problems.append("line %d: T%d selected without a preheat after standby" % (n, t))
                state[t] = "active"
            tool = t
        m = re.match(r"^EXCLUDE_OBJECT_START NAME=cube\.stl_id_(\d+)_", line)
        if m:
            objects.setdefault(int(m.group(1)), set()).add(tool)
    print("tool changes: %d, tools per object: %s" % (changes, {k: sorted(v) for k, v in sorted(objects.items())}))
    if standby_temp is not None:
        print("ooze prevention: %d standby commands at %d C, %d preheats" % (standby_count, standby_temp, preheat_count))
        if standby_count == 0 or preheat_count == 0:
            problems.append("ooze prevention is on but no standby/preheat commands were found")
    for obj, tools in sorted(objects.items()):
        if tools != {obj}:
            problems.append("cube %d printed by tools %s, expected only T%d" % (obj, sorted(tools), obj))
    if len(objects) != 4:
        problems.append("expected 4 cubes, found %d" % len(objects))
    if problems:
        print("PROBLEMS:", *problems[:20], sep="\n  ")
        raise SystemExit(1)
    print("OK: every cube printed by its own tool, every tool heated before selection, no template errors")


def strip_volatile(path):
    keep = []
    for line in open(path, errors="replace"):
        if re.match(r"; (generated by|HEADER_BLOCK|model printing time|total estimated time)", line):
            continue
        if re.search(r"(\d{4}-\d\d-\d\d|\d\d:\d\d:\d\d)", line) and line.startswith(";"):
            continue
        keep.append(line)
    return keep


def regress(baseline, feature, workdir):
    machine, process, filaments, cube = prepare(workdir, ["#FF0000", "#0000FF"])
    results = {}
    for label, binary in (("baseline", baseline), ("feature", feature)):
        for obj in (1, 2):
            gcode = slice_(binary, os.path.join(workdir, "%s_%d" % (label, obj)), machine, process, filaments, cube, obj, [])
            results[(label, obj)] = strip_volatile(gcode)
    new_keys = ("filament_mixed_pattern", "mixed_color_sublayer_min_height")
    ok = True
    for obj in (1, 2):
        a = [l for l in results[("baseline", obj)]]
        b = [l for l in results[("feature", obj)] if not any(k in l for k in new_keys)]
        if a != b:
            ok = False
            import difflib
            diff = list(difflib.unified_diff(a, b, "baseline", "feature", n=1))
            print("object filament %d: %d differing lines" % (obj, len(diff)))
            sys.stdout.writelines(diff[:60])
        else:
            print("object filament %d: G-code identical (%d lines)" % (obj, len(a)))
    if not ok:
        raise SystemExit(1)


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "cycle":
        check_cycle(sys.argv[2], sys.argv[3])
    elif mode == "sublayer":
        check_sublayer(sys.argv[2], sys.argv[3])
    elif mode == "u1":
        check_u1(sys.argv[2], sys.argv[3])
    elif mode == "regress":
        regress(sys.argv[2], sys.argv[3], sys.argv[4])
    else:
        raise SystemExit(__doc__)
