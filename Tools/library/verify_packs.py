"""Structural check of a pack library: does every line parse, and does it mean what it says.

The audio side is Tools/preset_check.py's job. This is the cheap pass that runs in a second and
catches the things a generator gets wrong in bulk: an unknown parameter key, a choice name the
synth does not know, a value outside its range, a duplicate preset name, a sample that is named
but not there, metadata outside 0..1, two presets sitting on the same spot of the map.

    python Tools/library/verify_packs.py --packs Library/Packs

Exit code 1 if anything failed.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RENDER = os.path.join(ROOT, "bin", "msvc", "ambient_render.exe")

BOOLS = {"on", "off", "true", "false", "yes", "no"}
def _sources():
    """Every modulation source, read from the core rather than copied. The hand-written set here
    did not know about BEAT the day BEAT was added, and reported six hundred perfectly good
    presets as naming an unknown source."""
    import re as _re
    path = os.path.join(ROOT, "Core", "src", "Modulation.cpp")
    text = open(path, encoding="utf-8").read()
    body = text[text.index("kSourceNames[kNumModSources] = {"):]
    return set(_re.findall(r'"([^"]+)"', body[:body.index("};")]))


SOURCES_FROM_CORE = _sources()
SOURCES = ({"none", "amp", "note", "velocity", "distance", "random"}
           | {f"lfo{i}" for i in range(1, 9)} | {f"env{i}" for i in range(1, 7)}
           | {f"macro_{c}" for c in "abcdefgh"} | {f"kura{i}" for i in range(1, 5)}
           | SOURCES_FROM_CORE)
PERFORMANCE = ("morph", "macro_", "map_", "route_", "inertia")


def choice_table():
    """key -> the value names, straight from the synth (--list-choices). A hand-kept copy here
    drifts the moment a parameter gains a value -- which is exactly what happened."""
    out = subprocess.run([RENDER, "--list-choices"], capture_output=True, text=True, encoding="utf-8").stdout
    table = {}
    for line in out.splitlines():
        if ":" in line:
            key, values = line.split(":", 1)
            table[key.strip()] = values.strip().split("|")
    return table


def param_table():
    out = subprocess.run([RENDER, "--list"], capture_output=True, text=True, encoding="utf-8").stdout
    table = {}
    for line in out.splitlines():
        m = re.match(r"^(\S+)\s+(\S+(?: \S+)*?)\s+\[(-?[\d.e+-]+) \.\. (-?[\d.e+-]+)\] default (-?[\d.e+-]+)", line)
        if m:
            table[m.group(1)] = (float(m.group(3)), float(m.group(4)))
    if not table:
        raise SystemExit(f"build ambient_render first ({RENDER})")
    return table


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", default=os.path.join(ROOT, "Library", "Packs"))
    a = ap.parse_args()
    params = param_table()
    CHOICES = choice_table()
    problems = []
    names = collections.Counter()
    cells = collections.Counter()
    total = 0
    files = sorted(f for f in os.listdir(a.packs) if f.endswith(".ambientpack"))
    if not files:
        raise SystemExit(f"no packs in {a.packs}")
    for fname in files:
        path = os.path.join(a.packs, fname)
        for ln, line in enumerate(open(path, encoding="utf-8"), 1):
            t = line.strip()
            if not t or t.startswith("#") or t.startswith("pack ") or (t.startswith("format ") and "|" not in t):
                continue
            total += 1
            where = f"{fname}:{ln}"
            f = t.split("|")
            # Nine since the impulse the Room Morph goes to travels with the preset.
            if len(f) > 9:
                problems.append(f"{where}: {len(f)} fields, at most 9 (a '|' inside a field?)")
            if len(f) < 2:
                problems.append(f"{where}: no settings field")
                continue
            name, settings = f[0].strip(), f[1].strip()
            if not name:
                problems.append(f"{where}: empty name")
            names[name] += 1
            for kv in settings.split(";"):
                if not kv:
                    continue
                if "=" not in kv:
                    problems.append(f"{where}: '{kv}' is not key=value")
                    continue
                k, v = kv.split("=", 1)
                if k not in params:
                    problems.append(f"{where}: unknown parameter '{k}'")
                    continue
                if any(k.startswith(p) for p in PERFORMANCE):
                    problems.append(f"{where}: '{k}' is performance state and must not be in a preset")
                if k in CHOICES:
                    if v not in CHOICES[k]:
                        problems.append(f"{where}: '{v}' is not a value of '{k}'")
                    continue
                if v in BOOLS:
                    continue
                try:
                    x = float(v)
                except ValueError:
                    problems.append(f"{where}: '{k}={v}' is not a number")
                    continue
                lo, hi = params[k]
                if x < lo - 1e-6 or x > hi + 1e-6:
                    problems.append(f"{where}: {k}={x} outside [{lo}, {hi}]")
            if len(f) > 2 and f[2].strip():
                m = f[2].split()
                # Nine numbers plus the tag bits; a tenth token is the measured loudness, which
                # measure_packs.py has written since the level-matching option existed; map_all
                # adds the three drone descriptors, the group and the two phrases -- sixteen.
                if len(m) not in (9, 10, 16):
                    problems.append(f"{where}: metadata has {len(m)} fields, expected 9, 10 or 16")
                else:
                    try:
                        vals = [float(x) for x in m[:8]]
                        int(m[8], 0)
                    except ValueError:
                        problems.append(f"{where}: metadata is not numeric")
                    else:
                        if any(v < -1e-6 or v > 1 + 1e-6 for v in vals):
                            problems.append(f"{where}: metadata outside 0..1")
                        cells[(round(vals[0], 3), round(vals[1], 3))] += 1
            if len(f) > 6 and f[6].strip():          # modulation matrix
                for row in f[6].split(";"):
                    row = row.strip()
                    if not row:
                        continue
                    m = re.match(r"^([a-z0-9_]+)>([a-z0-9_]+):(-?[\d.]+)((?::[a-z0-9_]+)*)$", row)
                    if not m:
                        problems.append(f"{where}: matrix row '{row}' is malformed")
                        continue
                    if m.group(1) not in SOURCES:
                        problems.append(f"{where}: unknown modulation source '{m.group(1)}'")
                    if m.group(2) not in params:
                        problems.append(f"{where}: unknown modulation target '{m.group(2)}'")
                    for extra in [x for x in m.group(4).split(":") if x]:
                        if extra != "u" and extra not in SOURCES:
                            problems.append(f"{where}: unknown via source '{extra}'")
            if len(f) > 7 and f[7].strip("~ "):      # envelope shapes
                shapes = f[7].split("~")
                if len(shapes) > 10:
                    problems.append(f"{where}: {len(shapes)} envelope shapes, at most 10 (the six, then the four sources' own)")
                for si, sh in enumerate(shapes):
                    sh = sh.strip()
                    if not sh:
                        continue
                    body = sh.split("!")[0]
                    pts = body.split("/")
                    if len(pts) > 16:
                        problems.append(f"{where}: envelope {si + 1} has {len(pts)} points, at most 16")
                    last = None
                    for pt in pts:
                        bits = pt.split(":")
                        if len(bits) not in (2, 3):
                            problems.append(f"{where}: envelope point '{pt}' is malformed")
                            break
                        try:
                            t = float(bits[0]); v = float(bits[1])
                        except ValueError:
                            problems.append(f"{where}: envelope point '{pt}' is not numeric")
                            break
                        if last is not None and t < last:
                            problems.append(f"{where}: envelope times go backwards at '{pt}'")
                        last = t
                        if v < -1.0001 or v > 1.0001:
                            problems.append(f"{where}: envelope value {v} outside -1..1")
            for which, field in (("texture", 3), ("wavetable", 4), ("impulse", 5), ("impulse B", 8)):
                if len(f) > field and f[field].strip():
                    # The texture field may name up to four clips separated by ';', one per source
                    # slot (an empty one means that slot has none); the others name one file.
                    for one in f[field].split(";"):
                        one = one.strip()
                        if not one:
                            continue
                        ref = os.path.normpath(os.path.join(a.packs, one))
                        if not os.path.isfile(ref):
                            problems.append(f"{where}: {which} not found: {one}")
    for name, n in names.items():
        if n > 1:
            problems.append(f"duplicate preset name '{name}' ({n} times)")
    stacked = sum(n - 1 for n in cells.values() if n > 1)
    if stacked:
        problems.append(f"{stacked} presets share a map position with another")
    print(f"{total} presets in {len(files)} packs")
    for p in problems[:40]:
        print(f"  {p}")
    if len(problems) > 40:
        print(f"  ... and {len(problems) - 40} more")
    print("verify: ok" if not problems else f"verify: {len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
