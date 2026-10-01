"""Real impulse responses for the Room, from collections downloaded by hand.

Picks the recordings that are spaces fit for ambient work and brings them into one shape: 48 kHz,
24-bit WAV, the direct sound 1 ms in, the recording's own noise floor cut off with a fade, peak at
-1 dBFS. For the longest and cleanest it adds the variants the classic ambient tricks ask for:
reversed (a swell into the dry sound) and at half speed (twice as long and an octave down). A
pre-delay and a high-pass ahead of the room need no files: the Room has Pre-Delay, and its Low Cut
after the convolution is the same filter as one before it, because the convolution is linear.

Sources, under --sources:
  AIR/AIR_1_4
      Aachen Impulse Response database 1.4, (c) 2009-2011 RWTH Aachen University (IND), MIT licence
      according to the project page. Only the Aula Carolina, a former church, is long enough to
      matter. Its binaural pairs go into Library/Impulses as real_air_aula_carolina_*.wav, credited
      in Library/Impulses/CREDITS.txt.
  EchoThief/EchoThiefImpulseResponseLibrary
      Chris Warren, echothief.com: real places. Free to use to create derivative work such as
      convolving other sounds with it; any other use by agreement with the author -- and handing
      the files on in a content pack is such a use. Written to --local, never into the library.
  GrantNelson480L/Big Gee's Lexicon 480L Reverb
      Captures of a Lexicon 480L by Grant Nelson, a free download that states no licence. --local.
  Fokke/<collection>/
      Fokke van Saane's impulses (a church, a factory hall, an EMT 244), from the Internet Archive's
      copy of his page: freeware, and he asks to be told where they are used. --local.

The choice, with the measurements of ir_metrics.py: T30 >= 1.4 s (a space, not a colour), R2 >= 0.97
(one clean decay: no slapback, no flutter), envelope residual <= 3.5 dB (nothing rising again), echo
density >= 0.9 in the tail (diffuse), at least 36 dB of decay above the recording's noise, and a
power centroid no higher than 7 kHz. Variants for T30 >= 2 s with R2 >= 0.99.

  python Tools/library/import_real_impulses.py [--sources DIR] [--local DIR] [--dry-run]
"""
import argparse
import glob
import json
import math
import os
import re
import sys

import numpy as np
import scipy.io as sio
import soundfile as sf
from scipy.signal import resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
LIBRARY = os.path.join(ROOT, "Library", "Impulses")
DEFAULT_SOURCES = os.path.normpath(os.path.join(ROOT, "..", "_Sources", "IRSources"))

sys.path.insert(0, HERE)
import ir_metrics  # noqa: E402

RATE = 48000

CREDITS = """Room impulse responses in this folder that were measured rather than generated
==============================================================================

real_air_aula_carolina_*.wav

    From the Aachen Impulse Response (AIR) Database, version 1.4: binaural room impulse responses
    of the Aula Carolina in Aachen, a former church. Converted to 48 kHz 24-bit stereo (left and
    right ear), trimmed at the noise floor and normalised; the _rev files are reversed, the _half
    files play at half speed.

    Jeub, M., Schaefer, M. and Vary, P.: A binaural room impulse response database for the
    evaluation of dereverberation algorithms. Proceedings of the International Conference on
    Digital Signal Processing (DSP), Santorini, 2009.

    Copyright (c) 2009-2011 RWTH Aachen University, Institute of Communication Systems and Data
    Processing (IND), http://www.iks.rwth-aachen.de

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software
    and associated documentation files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
    BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""

LOCAL_README = """Impulse responses for local use only -- NOT for the library or a content pack
=============================================================================

echothief_*.wav   EchoThief Impulse Response Library, Chris Warren (echothief.com). Licence: free
                  to use to create derivative work, such as convolving other sounds with them;
                  any other use by agreement with the author (chris@superhoax.com). Shipping the
                  files themselves needs that agreement.
lex480_*.wav      Big Gee's Lexicon 480L Reverb, Grant Nelson (grantnelson.co), a free download
                  that states no licence: ask before shipping.
fokke_*.wav       Fokke van Saane (fokkie@xs4all.nl; retrieved from the Internet Archive's copy of
                  fokkie.home.xs4all.nl): "The IR's are freeware", and he asks to hear where they
                  are used (his page: a copy of any CD made with them). Handing the files on is
                  not addressed: ask before shipping.

Converted by Tools/library/import_real_impulses.py (48 kHz, 24 bit, trimmed, normalised; _rev
reversed, _half at half speed).
"""


def slug(text):
    words = re.sub(r"[^A-Za-z0-9]+", " ", text).split()
    return "".join(w[:1].upper() + w[1:] for w in words)


def to_rate(x, sr):
    sr = int(round(sr))
    if sr == RATE:
        return x
    g = math.gcd(RATE, sr)
    return np.stack([resample_poly(c, RATE // g, sr // g, window=("kaiser", 8.0)) for c in x])


def shape(x):
    """Direct sound 1 ms in, the noise floor cut off with a fade, peak at -1 dBFS."""
    level = np.sqrt(np.mean(x ** 2, axis=0))
    onset = int(np.argmax(level > level.max() * 10.0 ** (-30.0 / 20.0)))
    x = x[:, max(0, onset - RATE // 1000):]
    hop = RATE // 100
    frames = x.shape[1] // hop
    if frames >= 20:
        e = np.mean((x[:, : frames * hop] ** 2).reshape(x.shape[0], frames, hop), axis=(0, 2)) + 1e-30
        db = 10.0 * np.log10(e / e.max())
        peak = int(np.argmax(db))
        floor = float(np.median(db[int(frames * 0.9):]))
        # The decay line, fitted between -5 dB and 10 dB above the floor (at most -25 dB), meets
        # the floor where the room has ended and only the recording's noise goes on.
        idx = np.where((db[peak:] <= -5.0) & (db[peak:] >= max(-25.0, floor + 10.0)))[0] + peak
        if idx.size >= 5 and floor > -90.0:
            slope, icpt = np.polyfit(idx, db[idx], 1)
            if slope < 0:
                cross = (floor + 3.0 - icpt) / slope
                x = x[:, : int(np.clip(cross * hop, (peak + 10) * hop, x.shape[1]))]
    x = x.copy()
    fade = min(int(min(0.3 * RATE, max(0.05 * RATE, 0.1 * x.shape[1]))), x.shape[1] // 2)
    x[:, -fade:] *= np.cos(np.linspace(0.0, np.pi / 2.0, fade)) ** 2
    return x * (10.0 ** (-1.0 / 20.0) / max(float(np.abs(x).max()), 1e-12))


def reversed_ir(x):
    y = x[:, ::-1].copy()
    n = min(24, y.shape[1])          # inside the 1 ms before the direct sound: the hit itself stays
    y[:, -n:] *= np.linspace(1.0, 0.0, n)
    return y


def half_speed(x):
    return np.stack([resample_poly(c, 2, 1, window=("kaiser", 8.0)) for c in x])


def fit(m):
    resid = m.get("env_resid_db")
    t30 = m.get("t30") or 0.0
    return (t30 >= 1.4 and (m.get("r2") or 0.0) >= 0.97
            and (resid if resid is not None else 99.0) <= 3.5 and (m.get("ned_tail") or 0.0) >= 0.9
            # at least 36 dB of the decay before the recording's noise: a tail that stops at -30 dB
            # is heard stopping under a long pad
            and (m.get("seconds") or 0.0) >= 0.6 * t30
            # the same line as for the generated impulses: a centroid above 7 kHz is hiss
            and (m.get("centroid_hz") or 0.0) <= 7000.0)


def rich(m):
    # A big space's decay is seldom one straight line (a church has two slopes), so from 4 s on
    # the ordinary R2 is enough.
    t30, r2 = m.get("t30") or 0.0, m.get("r2") or 0.0
    return bool((t30 >= 2.0 and r2 >= 0.99) or (t30 >= 4.0 and r2 >= 0.97))


def write(dest, name, y):
    os.makedirs(dest, exist_ok=True)
    peak = float(np.abs(y).max())
    if peak > 0.999:
        y = y * (0.999 / peak)
    sf.write(os.path.join(dest, name + ".wav"), y.T.astype(np.float32), RATE, subtype="PCM_24")


def take(x, sr, name, source, dest, log, dry):
    y = shape(to_rate(np.asarray(x, dtype=np.float64), sr))
    m = ir_metrics.analyse_array(y, RATE, name + ".wav")
    entry = {"file": name + ".wav", "source": source, "dest": dest, "fit": bool(fit(m)), "metrics": m, "variants": []}
    log.append(entry)
    if not entry["fit"] or dry:
        return
    write(dest, name, y)
    if rich(m):
        write(dest, name + "_rev", reversed_ir(y))
        write(dest, name + "_half", half_speed(y))
        entry["variants"] = [name + "_rev.wav", name + "_half.wav"]


def air(sources, log, dry):
    folder = os.path.join(sources, "AIR", "AIR_1_4")
    pairs = {}
    for f in sorted(glob.glob(os.path.join(folder, "air_binaural_aula_carolina_*.mat"))):
        mat = sio.loadmat(f, squeeze_me=True, struct_as_record=False)
        info = mat["air_info"]
        try:
            angle = int(info.angle)
        except (TypeError, ValueError):
            angle = 0
        key = (int(info.head), int(info.distance), angle)
        pairs.setdefault(key, {})[int(info.channel)] = (np.asarray(mat["h_air"], dtype=np.float64).ravel(), int(info.fs))
    for (head, distance, angle), chans in sorted(pairs.items()):
        if 0 not in chans or 1 not in chans:
            continue
        (right, fs), (left, _) = chans[0], chans[1]   # AIR: channel 1 is the left ear, 0 the right
        n = min(left.size, right.size)
        name = f"real_air_aula_carolina_{distance}m_{angle}deg" + ("" if head else "_nohead")
        take(np.stack([left[:n], right[:n]]), fs, name, "AIR 1.4, RWTH Aachen (MIT)", LIBRARY, log, dry)


def folder_of_files(pattern, prefix, source, local, log, dry):
    for path in sorted(glob.glob(pattern, recursive=True)):
        x, sr = sf.read(path, always_2d=True, dtype="float64")
        take(x.T, sr, prefix + slug(os.path.splitext(os.path.basename(path))[0]), source, local, log, dry)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sources", default=DEFAULT_SOURCES)
    ap.add_argument("--local", default=None, help="for what may not be shipped (default: <sources>/curated-local)")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    local = args.local or os.path.join(args.sources, "curated-local")
    log = []
    air(args.sources, log, args.dry_run)
    folder_of_files(os.path.join(args.sources, "EchoThief", "EchoThiefImpulseResponseLibrary", "*", "*.wav"),
                    "echothief_", "EchoThief, Chris Warren (convolution use only)", local, log, args.dry_run)
    folder_of_files(os.path.join(args.sources, "GrantNelson480L", "Big Gee's Lexicon 480L Reverb", "**", "*.aif"),
                    "lex480_", "Big Gee's Lexicon 480L, Grant Nelson (no licence stated)", local, log, args.dry_run)
    fokke = os.path.join(args.sources, "Fokke")
    for path in sorted(glob.glob(os.path.join(fokke, "**", "*.wav"), recursive=True)):
        rel = os.path.relpath(path, fokke).split(os.sep)
        top, stem = slug(rel[0]), slug(os.path.splitext(rel[-1])[0])
        x, sr = sf.read(path, always_2d=True, dtype="float64")
        take(x.T, sr, "fokke_" + (top if top == stem else f"{top}_{stem}"),
             "Fokke van Saane (free; handing on not granted)", local, log, args.dry_run)
    by_source = {}
    for e in log:
        s = by_source.setdefault(e["source"], [0, 0, 0])
        s[0] += 1
        s[1] += int(e["fit"])
        s[2] += len(e["variants"])
    for source, (n, k, v) in by_source.items():
        print(f"  {source}: {k} of {n} fit, {v} variants")
    for e in sorted(log, key=lambda e: -(e["metrics"].get("t30") or 0.0)):
        if e["fit"]:
            m = e["metrics"]
            print(f"    {e['file']:58s} {m['seconds']:5.2f} s  T30 {m['t30']:4.2f}  R2 {m['r2']:.3f}  "
                  f"centroid {m['centroid_hz']:5.0f} Hz  corr {m['corr']:+.2f}{'  + rev, half' if e['variants'] else ''}")
    if args.dry_run:
        return
    if any(e["fit"] and e["dest"] == LIBRARY for e in log):
        with open(os.path.join(LIBRARY, "CREDITS.txt"), "w", encoding="utf-8") as fh:
            fh.write(CREDITS)
    if any(e["fit"] and e["dest"] == local for e in log):
        with open(os.path.join(local, "README.txt"), "w", encoding="utf-8") as fh:
            fh.write(LOCAL_README)
    with open(os.path.join(args.sources, "import_manifest.json"), "w", encoding="utf-8") as fh:
        json.dump(log, fh, indent=1, default=lambda o: o.item() if hasattr(o, "item") else str(o))
    print(f"manifest: {os.path.join(args.sources, 'import_manifest.json')}")


if __name__ == "__main__":
    main()
