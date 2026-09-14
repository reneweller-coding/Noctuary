"""One calibration of the near bank from several measurements, without rendering again.

fore_gain is a gain and nothing else -- ambient_selftest measures +6.02 dB set as +6.01 dB out -- so a
measurement taken with gains in the bank tells what the same event would have measured without them:
the ratio minus the gain. Two runs of near_loudness.py over different backgrounds therefore combine into
one set of ratios at zero gain, and the median over all of their backgrounds is a better calibration
than either run alone. Written 14.09.2026: four reference backgrounds (dense, dark, bright, sparse) and
four from the journey Rene heard nothing near in.

    python Tools/library/near_gain_combine.py RUN.json[@gains_used.json] [RUN.json[@gains.json] ...] [--target 0]

A run measured with no gains in the bank takes no gains file. Writes Tools/library/near_gain.json.
"""
import argparse
import json
import os
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "Tools", "library", "near_gain.json")
RANGE = (-24.0, 36.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--target", type=float, default=0.0)
    a = ap.parse_args()
    ratios, backgrounds = {}, []
    for spec in a.runs:
        path, _, gains_path = spec.partition("@")   # not ":" -- a Windows path has one
        run = json.load(open(path, encoding="utf-8"))
        used = json.load(open(gains_path, encoding="utf-8"))["gain_db"] if gains_path else {}
        backgrounds += run["backgrounds"]
        for name, v in run["presets"].items():
            for bg, r in v.get("per_background", {}).items():
                ratios.setdefault(name, []).append(r - float(used.get(name, 0.0)))
    gains, spread = {}, []
    for name, rs in sorted(ratios.items()):
        med = statistics.median(rs)
        gains[name] = round(min(max(a.target - med, RANGE[0]), RANGE[1]), 1)
        spread.append(max(rs) - min(rs))
    doc = {"target_lu": a.target, "backgrounds": backgrounds,
           "how": "median over every background of the loudest momentary loudness (BS.1770, 400 ms) of the first "
                  "event against the background's loudness, at zero gain; Tools/library/near_loudness.py and "
                  "near_gain_combine.py",
           "gain_db": gains}
    json.dump(doc, open(OUT, "w", encoding="utf-8"), indent=1)
    g = sorted(gains.values())
    print("wrote %s: %d gains from %d backgrounds; %.1f .. %+.1f dB, median %+.1f; spread of a preset over them: median %.1f dB"
          % (OUT, len(gains), len(backgrounds), g[0], g[-1], statistics.median(g), statistics.median(spread)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
