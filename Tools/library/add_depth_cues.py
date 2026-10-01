"""Give the generated presets their foreground (far_unmask, presence, the far tail under the
presence band) without re-generating them -- the same values the generator now writes, drawn
from the same name-seeded stream, so a regeneration would produce these packs to the byte.

Unlike the release gap, this CHANGES THE SOUND -- the far reverb ducks under the voice -- so the
balance caches are removed and the whole library is balanced and measured again.

  add_depth_cues.py [--work work/library] [--dry-run]
"""
import argparse
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import conductor as cond      # noqa: E402
import rebalance_voice as rb  # noqa: E402  (guarded: importing it runs nothing)


def artist_of(header_lines):
    """The artist a pack stands for, or the built-in family a staging pack holds."""
    for line in header_lines:
        m = re.match(r"# In the spirit of (.+?)\. Not affiliated", line)
        if m:
            return m.group(1)
        m = re.match(r"# Built-in family '(.+?)'", line)
        if m:
            return m.group(1)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=os.path.join(ROOT, "work", "library"))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    folders = [os.path.join(ROOT, "Library", "Packs"), os.path.join(a.work, "builtin_packs")]
    grand = 0
    for folder in folders:
        if not os.path.isdir(folder):
            print(f"  {folder}: not there")
            continue
        total = changed = unknown = 0
        keys = {}
        for f in sorted(glob.glob(os.path.join(folder, "*.ambientpack"))):
            lines = open(f, encoding="utf-8").read().split("\n")
            artist = artist_of(lines[:6])
            if artist is None or cond.family_of(artist) is None:
                unknown += 1
                print(f"  {os.path.basename(f)}: no family for {artist!r}, left alone")
                continue
            touched = False
            for i, line in enumerate(lines):
                if not line.strip() or line.startswith(("#", "pack ", "format ")):
                    continue
                fields = line.split("|")
                if len(fields) < 7:
                    continue
                total += 1
                pairs = rb.parse_settings(fields[1])
                params = {k: v for k, v in pairs if v is not None}
                cues = cond.depth_cues(artist, fields[0], params)
                if not cues:
                    continue
                for k, v in cues.items():
                    rb.put(pairs, k, v)
                    keys[k] = keys.get(k, 0) + 1
                fields[1] = rb.write_settings(pairs)
                lines[i] = "|".join(fields)
                touched = True
                changed += 1
            if touched and not a.dry_run:
                open(f, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
        grand += changed
        print(f"  {os.path.basename(folder)}: {changed} of {total} presets given their foreground "
              f"({unknown} packs without a family); keys written: {keys}" + (" (dry run)" if a.dry_run else ""))
    if not a.dry_run:
        for name in ("balance-Packs.json", "balance-builtin_packs.json", "packs.json", "packs.json.fp", "packs.json.done",
                     "staging.json", "staging.json.fp", "builtins.json"):
            path = os.path.join(a.work, name)
            if os.path.exists(path):
                os.remove(path)
                print(f"  removed {name}: the sound changed, the library is balanced and measured again")
    print(f"{grand} presets changed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
