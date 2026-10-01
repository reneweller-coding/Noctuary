"""The impulse files the released preset packs name, kept under their names.

Sessions and user presets store the absolute path of the room they were saved with, and restoring one whose
file has gone silently keeps whatever room was loaded before. So the impulses the released packs -- and with
them people's sessions -- name stay where they are and in every content download, under the names they have.
New presets never take one (make_presets.py reads this list), and the room generator never writes one of
these names.

    python Tools/library/legacy_impulses.py [--rev HEAD]

Writes Tools/library/legacy_impulses.json: for every name, how many presets named it and the file it is kept
as now -- in Library/Impulses, in the folder the 11.09.2026 curation moved it to, or as the FLAC the last
content build made of it.
"""
import argparse
import collections
import json
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "legacy_impulses.json")
SOURCES = (os.path.join(ROOT, "Library", "Impulses"),
           os.path.normpath(os.path.join(ROOT, "..", "_Sources", "IRSources", "rejected-generated")),
           os.path.join(ROOT, "Deploy", "content", "Impulses"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rev", default="HEAD", help="the commit whose Library/Packs were released")
    a = ap.parse_args()
    listing = subprocess.run(["git", "-C", ROOT, "ls-tree", "--name-only", a.rev, "Library/Packs/"],
                             capture_output=True, text=True, check=True).stdout.split()
    counts = collections.Counter()
    for path in listing:
        if not path.endswith(".ambientpack"):
            continue
        text = subprocess.run(["git", "-C", ROOT, "show", f"{a.rev}:{path}"], capture_output=True, check=True).stdout
        for line in text.decode("utf-8", "replace").splitlines():
            t = line.strip()
            if not t or t.startswith(("#", "pack ", "format ")):
                continue
            cols = t.split("|")
            for field in (5, 8):   # impulse A, and impulse B in packs that carry one
                if len(cols) > field and cols[field].strip():
                    counts[cols[field].strip().split("/")[-1]] += 1
    files, missing = {}, []
    for name in sorted(counts):
        stem = os.path.splitext(name)[0]
        found = None
        for folder in SOURCES:
            for candidate in (name, stem + ".flac"):
                if os.path.isfile(os.path.join(folder, candidate)):
                    found = os.path.join(folder, candidate)
                    break
            if found:
                break
        if found is None:
            missing.append(name)
            continue
        try:
            inside = os.path.commonpath([ROOT, found]) == ROOT
        except ValueError:          # another drive
            inside = False
        files[name] = {"presets": counts[name], "source": (os.path.relpath(found, ROOT) if inside else found).replace("\\", "/")}
    with open(OUT, "w", encoding="utf-8") as fh:
        json.dump({"rev": a.rev, "files": files, "missing": missing}, fh, indent=1)
    print(f"{len(files)} legacy impulses, {sum(counts.values())} preset references, {len(missing)} missing -> {OUT}")


if __name__ == "__main__":
    main()
