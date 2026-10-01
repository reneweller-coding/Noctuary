"""Give every generated preset the Release Gap it never had (R8.3), without re-generating or
re-balancing anything.

The conductor ranges have no column for `brain_release_gap`, so every pack of the 2.0 library
left it at nought, and a quarter of the presets had note-offs inside two seconds of each other.
The generator now draws it (conductor.py) from a stream seeded by the preset's name; this writes
the same value into the packs that already exist -- the value a regeneration would give -- and
re-stamps the balance cache for the lines it changed, since the balance measures levels and a
gap between note-offs moves none.

  add_release_gap.py [--work work/library] [--dry-run]
"""
import argparse
import glob
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import rebalance_voice as rb   # noqa: E402  (guarded: importing it runs nothing)


def method(pairs):
    """rebalance_voice's method string at its defaults, which is what rebuild_all runs it with."""
    return (f"sec=30.0|hour=12.0|target=0.0|cut=4.0|subcut=9.0|bed={'+'.join(rb.BED)}"
            f"|skip={rb.warm_up(pairs, 75.0):.1f}")


def gap_for(name):
    return random.Random(name + "|release_gap").uniform(2.0, 4.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", default=os.path.join(ROOT, "work", "library"))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    folders = [(os.path.join(ROOT, "Library", "Packs"), os.path.join(a.work, "balance-Packs.json")),
               (os.path.join(a.work, "builtin_packs"), os.path.join(a.work, "balance-builtin_packs.json"))]
    for folder, cache in folders:
        if not os.path.isdir(folder):
            print(f"  {folder}: not there")
            continue
        have = json.load(open(cache, encoding="utf-8")) if os.path.exists(cache) else {}
        matched = total = changed = restamped = already = 0
        for f in sorted(glob.glob(os.path.join(folder, "*.ambientpack"))):
            lines = open(f, encoding="utf-8").read().split("\n")
            touched = False
            for i, line in enumerate(lines):
                if not line.strip() or line.startswith(("#", "pack ", "format ")):
                    continue
                fields = line.split("|")
                if len(fields) < 7:
                    continue
                name = fields[0]
                pairs = rb.parse_settings(fields[1])
                total += 1
                old_fp = rb.fingerprint(line, method(pairs))
                known = have.get(name) == old_fp      # the cache still describes this line
                matched += known
                if rb.get_str(pairs, "brain_release_gap", "") != "":
                    already += 1
                    continue
                rb.put(pairs, "brain_release_gap", gap_for(name))
                fields[1] = rb.write_settings(pairs)
                lines[i] = "|".join(fields)
                touched = True
                changed += 1
                if known:
                    have[name] = rb.fingerprint(lines[i], method(pairs))
                    restamped += 1
            if touched and not a.dry_run:
                open(f, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
        if not a.dry_run and os.path.exists(cache):
            json.dump(have, open(cache, "w", encoding="utf-8"))
        print(f"  {os.path.basename(folder)}: {total} presets, cache described {matched} of them; "
              f"{changed} given a release gap ({already} had one), {restamped} cache entries re-stamped"
              + (" (dry run)" if a.dry_run else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
