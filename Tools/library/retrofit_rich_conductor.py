"""The Sleep Concert pack's conductor set to Robert Rich's clock as measured from his recordings (13.11.2026).

The user, on Zodiacal: the conductor should come into Rich's range -- from measurements of his corpus rather than the
researched ranges of conductor_ranges.json, the weight on his arhythmic recordings -- and then the same for Noctuary.

Measured (GranularSynth/Tools/presets/rich_measure.py over his 285 recordings; the 162 with a rhythm of 0.15 or less,
1,630 minutes): the harmony stands 3.2 / 3.9 / 5.9 s (the lag at which the chroma's deviations lose half their
correlation), 7.5 / 11.4 / 15.1 moves a minute, 5 / 7 / 8 notes sounding, the lowest note 29 / 31 / 35, the highest
79 / 87 / 91 (quartiles). The pack as it was (events every 36 - 62 s, holds of 112 - 437 s, his researched row), eight
presets rendered for eight minutes and measured the same way: the harmony stood 40 s (median) and hardly moved at all
(0.4 moves a minute). Events every 12 s, chords every 18 s, holds of 20 - 60 s, three notes brought it to 9 - 36 s and
two to four moves (texture.py). The rest of the stillness is the pack's own sound (halls of half a minute, the Memory
and the Cloud bringing back what was played), which this pass leaves as it is.

So, for every preset of the pack: 3 - 5 notes, an event every 8 - 20 s (log), the Autoplay's chords every 12 - 30 s,
holds of 1.5 - 2.5 and 4 - 7 times the event gap. Nothing else changes; conductor_ranges.json (Rene's document) is not
touched -- the measured row is in conductor_measured.json beside it. The pack's loudness is measured on a held chord
(measure_packs.py: --notes 45,52,59), so the gains stand as they were.

    python Tools/library/retrofit_rich_conductor.py            # what it would do
    python Tools/library/retrofit_rich_conductor.py --write    # do it

Deterministic: each preset's dice are seeded by its name; a second run writes the same values.
"""
import argparse
import hashlib
import json
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACK = os.path.join(ROOT, "Library", "Packs", "Sleep-Concert.ambientpack")
MEASURED = os.path.join(HERE, "conductor_measured.json")


def logu(r, lo, hi):
    return math.exp(r.uniform(math.log(lo), math.log(hi)))


def retrofit(settings, name):
    r = random.Random(int(hashlib.sha1(("rich-clock/" + name).encode("utf-8")).hexdigest()[:12], 16))
    rate = logu(r, 8.0, 20.0)
    new = {
        "brain_density": str(r.choice([3, 4, 4, 5])),
        "brain_rate": "%.4g" % rate,
        "auto_rate": "%.4g" % logu(r, 12.0, 30.0),
        "brain_hold_min": "%.4g" % (rate * r.uniform(1.5, 2.5)),
        "brain_hold_max": "%.4g" % (rate * r.uniform(4.0, 7.0)),
    }
    parts, seen = [], set()
    for p in settings.split(";"):
        if "=" not in p:
            if p:
                parts.append(p)
            continue
        k = p.split("=", 1)[0]
        if k in new:
            parts.append("%s=%s" % (k, new[k]))
            seen.add(k)
        else:
            parts.append(p)
    parts += ["%s=%s" % (k, v) for k, v in new.items() if k not in seen]
    return ";".join(parts), new


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    lines = open(PACK, encoding="utf-8", newline="").read().split("\n")   # its CRLF kept as it is
    out, changed = [], 0
    for line in lines:
        if "|" not in line or line.startswith(("#", "pack ", "format ")):
            out.append(line)
            continue
        fields = line.split("|")
        settings, new = retrofit(fields[1], fields[0])
        if settings != fields[1]:
            changed += 1
        fields[1] = settings
        out.append("|".join(fields))
        if changed <= 3:
            print("%-22s %s" % (fields[0], " ".join("%s=%s" % kv for kv in new.items())))
    print("%d presets of the Sleep Concert pack %s" % (changed, "rewritten" if a.write else "would change"))
    if a.write:
        with open(PACK, "w", encoding="utf-8", newline="") as f:
            f.write("\n".join(out))
        json.dump({
            "Robert Rich": {
                "source": "GranularSynth/Tools/presets/rich_measure.py, 13.11.2026: his 285 recordings on the NAS, "
                          "the 162 with rhythm <= 0.15 (1,630 minutes); quartiles",
                "measured": {"harmony_stands_s": [3.17, 3.91, 5.93], "moves_per_min": [7.48, 11.36, 15.07],
                             "notes_sounding": [5, 7, 8], "lowest_note": [29.0, 31.3, 35.0],
                             "highest_note": [79.0, 86.7, 91.0], "thirds": 0.36, "minor_sevenths": 0.20,
                             "fifths": 0.22},
                "ranges": {"brain_density": {"type": "choice", "options": {"3": 0.25, "4": 0.5, "5": 0.25}},
                           "brain_rate": {"type": "log-uniform", "min": 8, "max": 20},
                           "auto_rate": {"type": "log-uniform", "min": 12, "max": 30},
                           "brain_hold_min": {"type": "times brain_rate", "min": 1.5, "max": 2.5},
                           "brain_hold_max": {"type": "times brain_rate", "min": 4.0, "max": 7.0}},
            }
        }, open(MEASURED, "w", encoding="utf-8"), indent=1)
        print("written:", PACK, "and", MEASURED)


if __name__ == "__main__":
    main()
