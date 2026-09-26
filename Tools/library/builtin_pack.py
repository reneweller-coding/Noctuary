"""The built-in presets as a pack, and back (25.09.2026).

Everything that measures a preset measures packs (measure_packs.py, guide_fit.py). The built-ins
used to reach it as the staging packs make_library.py wrote before compiling them in -- which is
also why rebuild_all.py's step 5 compiles Core/src/Presets.cpp FROM those staging packs, and would
have undone every change made to the file since (the production guide's retrofit among them). This
goes the other way: it reads the rows of Core/src/Presets.cpp as they stand, writes them as one pack
under names of their own ("BI " and the name, so a render asks for the copy and not for the
compiled-in original), and after the measurement writes back exactly the settings the measurement
moved -- the master gain, Sub Level, Width -- and nothing else.

    python Tools/library/builtin_pack.py export --out build/library-work/builtin_guide
    python Tools/library/builtin_pack.py import --pack build/library-work/builtin_guide/Builtins.ambientpack
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import guide  # noqa: E402  (the literal wrapping)

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
BUILTINS = os.path.join(ROOT, "Core", "src", "Presets.cpp")
PREFIX = "BI "
MOVED = ("master_gain", "sub_level", "width")
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')


def rows(text):
    """Every row of kPresets: (start, end, fields) with fields a list of strings or None (nullptr),
    the name first. Offsets are into `text`."""
    start = text.index("const Preset kPresets[] = {")
    end = text.index("\n};", start)
    i = text.index("{", start) + 1
    out = []
    while True:
        j = text.find("{", i, end)
        if j < 0:
            break
        # the row runs to the brace that closes it; braces never appear inside the literals
        k = j + 1
        fields, cur, in_str = [], [], False
        while True:
            ch = text[k]
            if in_str:
                if ch == "\\":
                    cur.append(text[k:k + 2]); k += 2; continue
                if ch == '"':
                    in_str = False
                else:
                    cur.append(ch)
                k += 1
                continue
            if ch == '"':
                in_str = True   # adjacent literals of one field join, as the compiler joins them
                k += 1
                continue
            if text.startswith("nullptr", k):
                fields.append(None); k += 7; continue
            if ch == ",":
                if cur:
                    fields.append("".join(cur)); cur = []
                k += 1
                continue
            if ch == "}":
                if cur:
                    fields.append("".join(cur))
                break
            k += 1
        out.append((j, k + 1, fields))
        i = k + 1
    return out


def export(out_dir):
    with open(BUILTINS, encoding="utf-8") as f:
        text = f.read()
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "Builtins.ambientpack")
    n = 0
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# The built-in presets of Core/src/Presets.cpp, written by Tools/library/builtin_pack.py to be measured.\n")
        f.write("pack Builtins (measured)\nformat 2\n")
        for _, _, fields in rows(text):
            name, settings = fields[0], fields[1] if len(fields) > 1 else ""
            if not settings:
                continue   # Init
            rest = [(fields[i] or "") if i < len(fields) else "" for i in range(2, 8)]
            tex, tab, imp, mod, envs, impb = rest
            f.write(f"{PREFIX}{name}|{settings}||{tex}|{tab}|{imp}|{mod}|{envs}|{impb}\n")
            n += 1
    print(f"exported {n} built-ins to {os.path.relpath(path, ROOT)}")


def parse(settings):
    out = {}
    for kv in settings.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def import_(pack):
    moved = {}
    for line in open(pack, encoding="utf-8"):
        line = line.rstrip("\n")
        if line.startswith(("#", "pack ", "format ")) or "|" not in line:
            continue
        name, settings = line.split("|", 2)[:2]
        if name.startswith(PREFIX):
            p = parse(settings)
            moved[name[len(PREFIX):]] = {k: p[k] for k in MOVED if k in p}
    with open(BUILTINS, encoding="utf-8", newline="") as f:
        text = f.read()
    crlf = "\r\n" in text
    text = text.replace("\r\n", "\n")
    head_end = text.index("const Preset kPresets[] = {")
    body_end = text.index("\n};", head_end)
    head, body, tail = text[:head_end], text[head_end:body_end], text[body_end:]
    changed = 0

    def row(m):
        nonlocal changed
        name = m.group(2)
        if name not in moved:
            return m.group(0)
        settings = "".join(LIT.findall(m.group(3)))
        p = parse(settings)
        new = dict(p)
        for k, v in moved[name].items():
            new[k] = v
        if new == p:
            return m.group(0)
        # master_gain stays where the file has it (first); a key the row did not have is appended
        text_new = ";".join(f"{k}={v}" for k, v in new.items())
        changed += 1
        return m.group(1) + guide.wrap_literal(text_new)
    body = guide.ROW.sub(row, body)
    text = head + body + tail
    with open(BUILTINS, "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("\n", "\r\n") if crlf else text)
    print(f"wrote the measured settings back into {changed} built-ins")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export"); e.add_argument("--out", required=True)
    i = sub.add_parser("import"); i.add_argument("--pack", required=True)
    a = ap.parse_args()
    if a.cmd == "export":
        export(a.out)
    else:
        import_(a.pack)


if __name__ == "__main__":
    main()
