"""Fit the production guide's depth model to the presets that were written before it (25.09.2026).

The engine's new parameters (depth_range, depth_predelay, depth_width, send_lowcut, sub_harmonics)
reach every preset through their defaults, because no preset names them. What the generator DID
write and what stands in the guide's way is moved into the guide's windows by `guide.apply()` --
one table for this pass and for make_presets.py, listed in Tools/library/guide.py: the plane
(depth, presence, far_predelay), the rooms (decays, the far low-pass, far width, the room's
pre-delay and low-pass), the low end (pad low cut, bass mono, subsonic, the sub's octave and its
beat), clarity (purity, detune, beat ceiling, air, master width), the grains, and the LFOs' sync.

Re-measure after this: the far reverb hears no fundamentals any more, the background stands
fourteen decibels lower and the plane reaches the horizon, so every preset's loudness has moved.

    python Tools/library/retrofit_guide.py            # what it would do
    python Tools/library/retrofit_guide.py --write    # do it
"""
import argparse
import glob
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import guide  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
BUILTINS = os.path.join(ROOT, "Core", "src", "Presets.cpp")


def retrofit_pack(text):
    """Every preset line of one pack through guide.apply(); returns (text, presets changed)."""
    out, changed = [], 0
    for line in text.split("\n"):
        if line.startswith(("#", "pack ", "format ")) or "|" not in line:
            out.append(line); continue
        parts = line.split("|")
        new, n = guide.apply_to_settings(parts[1])
        if n:
            parts[1] = new; changed += 1
        out.append("|".join(parts))
    return "\n".join(out), changed


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="rewrite the files (default: report only)")
    ap.add_argument("--packs", default=PACKS)
    a = ap.parse_args()
    total = 0
    for path in sorted(glob.glob(os.path.join(a.packs, "*.ambientpack"))) + [BUILTINS]:
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
        crlf = "\r\n" in text
        text = text.replace("\r\n", "\n")
        if path == BUILTINS:
            new, n = guide.apply_to_builtins(text)
        else:
            new, n = retrofit_pack(text)
        total += n
        if new != text:
            print(f"{os.path.relpath(path, ROOT)}: {n} preset(s) moved into the guide's windows")
            if a.write:
                with open(path, "w", encoding="utf-8", newline="") as f:
                    f.write(new.replace("\n", "\r\n") if crlf else new)
    print(f"{'rewrote' if a.write else 'would rewrite'} {total} presets" + ("" if a.write else " (add --write)"))


if __name__ == "__main__":
    main()
