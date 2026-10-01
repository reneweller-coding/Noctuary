"""The built-in presets: the staging packs make_library.py wrote, compiled into Core/src/Presets.cpp.

A built-in preset is a line in a C++ array, and it has to sound right on a machine where the sample
library was never installed -- so it names no file at all, and its room is the synthetic reverb
rather than the convolution (which would keep whatever impulse was loaded before it).

Everything that measures a preset measures packs: the voice against its own bed
(rebalance_voice.py), the loudness window (measure_packs.py), the descriptors. So the sixteen
families are generated as ordinary packs into a staging folder, measured there like any other pack,
and only then compiled in by this tool.

    python Tools/library/write_builtins.py --packs work/builtin_packs
    python Tools/library/write_builtins.py --packs work/builtin_packs --check    report, write nothing

Everything outside `const Preset kPresets[] = { ... };` stays exactly as it is: the file's head, the
section banks it includes and the functions under it. The family comments that Tools/preset_map.py
reads -- "// ---- N..M name", which become the browser's families -- are written from the pack
names, in the order artists.py lists them. "Init", the empty preset, opens the array.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from artists import BUILTINS  # noqa: E402

PRESETS_CPP = os.path.join(ROOT, "Core", "src", "Presets.cpp")
ARRAY = "const Preset kPresets[] = {"
FIELDS = ["name", "settings", "meta", "texture", "wavetable", "impulse", "mod", "envs", "impulse_b"]
WIDTH = 108


def pack_file(name):
    return re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-") + ".ambientpack"


def read_pack(path):
    rows = []
    for ln, line in enumerate(open(path, encoding="utf-8"), 1):
        t = line.strip()
        if not t or t.startswith("#") or t.startswith("pack ") or (t.startswith("format ") and "|" not in t):
            continue
        f = t.split("|")
        if len(f) < 2:
            raise SystemExit(f"{os.path.basename(path)}:{ln}: no settings field")
        while len(f) < len(FIELDS):
            f.append("")
        rows.append({k: f[i].strip() for i, k in enumerate(FIELDS)})
    return rows


def wrap(text, sep):
    """One C++ string literal per line, split after `sep` so the pieces stay readable."""
    parts, line = [], ""
    for piece in text.split(sep):
        piece = piece + sep
        if line and len(line) + len(piece) > WIDTH:
            parts.append(line)
            line = piece
        else:
            line += piece
    if line:
        parts.append(line)
    if parts:
        parts[-1] = parts[-1][:-len(sep)] if parts[-1].endswith(sep) else parts[-1]
    return [p for p in parts if p]


def literal(text, sep, indent):
    return ("\n" + indent).join('"%s"' % p for p in wrap(text, sep)) or '""'


def entry(row):
    """One initialiser. The commas matter more than they look: two string literals with nothing
    between them are ONE string to a C++ compiler, so a missing comma would silently glue the
    settings to the modulation matrix instead of failing to build."""
    envs = row["envs"].strip("~ ")
    out = ['    { "%s",' % row["name"], "      " + literal(row["settings"], ";", "      ")]
    if row["mod"] or envs:
        out[-1] += ","
        out.append("      nullptr, nullptr, nullptr,")     # texture, wavetable, impulse: never a file
        out.append("      " + literal(row["mod"], ";", "      "))
    if envs:
        out[-1] += ","
        out.append("      " + literal(row["envs"], "~", "      "))
    out[-1] += " },"
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", default=os.path.join(ROOT, "work", "builtin_packs"))
    ap.add_argument("--out", default=PRESETS_CPP)
    ap.add_argument("--check", action="store_true", help="report what would be written, write nothing")
    a = ap.parse_args()

    body, index, seen, families = [], 0, {}, []
    body.append("")
    for st in BUILTINS:
        path = os.path.join(a.packs, pack_file(st["name"]))
        if not os.path.exists(path):
            raise SystemExit(f"no staging pack for '{st['name']}' ({path}) -- run make_library.py --builtins first")
        rows = read_pack(path)
        if st is BUILTINS[0]:
            rows = [{k: "" for k in FIELDS} | {"name": "Init"}] + rows
        start = index
        lines = []
        for r in rows:
            for which in ("texture", "wavetable", "impulse", "impulse_b"):
                if r[which]:
                    raise SystemExit(f"'{r['name']}' names a {which}: a built-in preset may not use a file")
            for field in ("name", "settings", "mod", "envs"):
                if '"' in r[field] or "\\" in r[field]:
                    raise SystemExit(f"'{r['name']}': a quote or a backslash in {field}")
            if r["name"] in seen:
                raise SystemExit(f"duplicate preset name '{r['name']}' ({seen[r['name']]} and {st['name']})")
            seen[r["name"]] = st["name"]
            lines += entry(r) if r["settings"] or r["mod"] or r["envs"] else ['    { "%s", "" },' % r["name"]]
            index += 1
        body.append("    // ---------------------------------------------------------------- "
                    f"{start}..{index - 1} {st['name']}")
        body += lines
        body.append("")
        families.append((st["name"], index - start))

    text = open(a.out, encoding="utf-8").read()
    i = text.index(ARRAY) + len(ARRAY)
    j = text.index("\n};", i)
    out = text[:i] + "\n" + "\n".join(body) + text[j + 1:]
    for name, n in families:
        print(f"  {n:4d}  {name}")
    print(f"{index} built-in presets in {len(families)} families")
    if a.check:
        print("check only: nothing written")
        return 0
    with open(a.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(out)
    print(f"wrote {a.out} -- rebuild, then measure the built-ins (map_all.py --dry-run)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
