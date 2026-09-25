"""Fit the production guide's depth model to the presets that were written before it (25.09.2026).

The engine's new parameters (depth_range, depth_predelay, depth_width, send_lowcut, sub_harmonics)
reach every preset through their defaults, because no preset names them. Two keys the generator DID
write stand in the guide's way, and this pass rewrites them, in the packs and in the built-ins:

    far_predelay    20 .. 200 ms in every preset. The gap between a sound and its room now belongs
                    to the source and follows its distance (Space: Gap, 40 ms at the ear, none on the
                    horizon); the hall's own pre-delay is set to the new default of 3 ms, so a source
                    on the horizon has no gap and one at the ear has the source's.
    sub_octave      -1 where a preset says so. The guide's sub lives at 25 .. 70 Hz; under a C3-B3
                    root that is two octaves down (33 .. 62 Hz), which is the new default. Presets
                    that never wrote the key take the default by themselves.

Everything else the guide asks for is a default or a measurement (Tools/library/measure_packs.py
now holds the loudness window in LUFS and reports the crest, mono-loss and true-peak gates).
Re-measure after this: the far reverb hears no fundamentals any more and the background stands
fourteen decibels lower, so every preset's loudness has moved.

    python Tools/library/retrofit_guide.py            # what it would do
    python Tools/library/retrofit_guide.py --write    # do it
"""
import argparse
import glob
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
BUILTINS = os.path.join(ROOT, "Core", "src", "Presets.cpp")

FAR_PREDELAY_MS = 3.0
PREDELAY = re.compile(r"far_predelay=[-+0-9.eE]+")
SUB_OCTAVE = re.compile(r"sub_octave=-1(?=[;|\"])")


def retrofit_text(text):
    """The two rewrites over one file's text; returns the new text and the two counts."""
    n_pre = len(PREDELAY.findall(text))
    text = PREDELAY.sub(f"far_predelay={FAR_PREDELAY_MS:g}", text)
    n_sub = len(SUB_OCTAVE.findall(text))
    text = SUB_OCTAVE.sub("sub_octave=-2", text)
    return text, n_pre, n_sub


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="rewrite the files (default: report only)")
    ap.add_argument("--packs", default=PACKS)
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.packs, "*.ambientpack"))) + [BUILTINS]
    tot_pre = tot_sub = 0
    for path in files:
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
        new, n_pre, n_sub = retrofit_text(text)
        tot_pre += n_pre; tot_sub += n_sub
        if new != text:
            print(f"{os.path.relpath(path, ROOT)}: far_predelay {n_pre}, sub_octave -1 -> -2 {n_sub}")
            if a.write:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(new)
    print(f"{'rewrote' if a.write else 'would rewrite'} far_predelay in {tot_pre} presets and sub_octave in {tot_sub}"
          + ("" if a.write else " (add --write)"))


if __name__ == "__main__":
    main()
