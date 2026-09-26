"""Fit the review of the conductor against ambient harmony to the presets that exist (25.09.2026).

The engine's fixes (the key profile of the scale, the prime limit, the distances instead of interval
classes, harmonic entropy, the guards for other tunings) reach every preset by themselves. What is a
range -- adaptive intonation where the root moves, the deep family's background register, the root's
targets, Utonal, Series, the arc's hold on the harmony -- is set here, family by family, through
review.apply(): the packs by their artist's family (conductor.family_of), the built-ins by the family
their section of Core/src/Presets.cpp stands in for (conductor.BUILTIN_FAMILY). Idempotent: a second
pass moves nothing.

Re-measure after this, with everything else of the round.

    python Tools/library/retrofit_review.py            # what it would do
    python Tools/library/retrofit_review.py --write    # do it
"""
import argparse
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import artists     # noqa: E402
import conductor   # noqa: E402
import guide       # noqa: E402  (the built-in rows' pattern and the literal wrapping)
import review      # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
BUILTINS = os.path.join(ROOT, "Core", "src", "Presets.cpp")
SECTION = re.compile(r"//\s*-+\s*\d+\s*\.\.\s*\d+\s+(.+?)\s*$", re.M)


def pack_family(text):
    """The family of a pack: its 'pack' line names the style, the style names the artist."""
    m = re.search(r"^pack (.+)$", text, re.M)
    if not m:
        return None
    name = m.group(1).strip()
    for st in artists.ARTISTS:
        if st["name"] == name:
            return conductor.family_of(st.get("inspiration", "")) or conductor.family_of(name)
    return conductor.family_of(name)


def retrofit_pack(text, family, counts):
    out, changed = [], 0
    for line in text.split("\n"):
        if line.startswith(("#", "pack ", "format ")) or "|" not in line:
            out.append(line); continue
        parts = line.split("|")
        new, n = review.apply_to_settings(parts[1], family, parts[0])
        if n:
            parts[1] = new; changed += 1; counts[family] += 1
        out.append("|".join(parts))
    return "\n".join(out), changed


def retrofit_builtins(text, counts):
    start = text.index("const Preset kPresets[] = {")
    end = text.index("\n};", start)
    head, body, tail = text[:start], text[start:end], text[end:]
    sections = [(m.start(), m.group(1).strip()) for m in SECTION.finditer(body)]
    changed = 0

    def family_at(pos):
        fam = None
        for at, name in sections:
            if at > pos:
                break
            fam = conductor.BUILTIN_FAMILY.get(name)
        return fam

    def row(m):
        nonlocal changed
        fam = family_at(m.start())
        settings = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(3)))
        new, n = review.apply_to_settings(settings, fam, m.group(2))
        if n == 0:
            return m.group(0)
        changed += 1
        counts[fam] += 1
        return m.group(1) + guide.wrap_literal(new)
    body = guide.ROW.sub(row, body)
    return head + body + tail, changed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="rewrite the files (default: report only)")
    ap.add_argument("--packs", default=PACKS)
    a = ap.parse_args()
    total = 0
    counts = collections.Counter()
    for path in sorted(glob.glob(os.path.join(a.packs, "*.ambientpack"))) + [BUILTINS]:
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
        crlf = "\r\n" in text
        text = text.replace("\r\n", "\n")
        if path == BUILTINS:
            new, n = retrofit_builtins(text, counts)
        else:
            fam = pack_family(text)
            if fam is None:
                print(f"{os.path.relpath(path, ROOT)}: no family, left alone")
                continue
            new, n = retrofit_pack(text, fam, counts)
        total += n
        if new != text:
            print(f"{os.path.relpath(path, ROOT)}: {n} preset(s) moved into the review's windows")
            if a.write:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(new.replace("\n", "\r\n") if crlf else new)
    print("by family: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items(), key=lambda kv: str(kv[0]))))
    print(f"{'rewrote' if a.write else 'would rewrite'} {total} presets" + ("" if a.write else " (add --write)"))


if __name__ == "__main__":
    main()
