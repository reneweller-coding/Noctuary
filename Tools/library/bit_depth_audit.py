"""How many of the library's recordings actually need twenty-four bits. Answer, measured: all of
them -- but the reason is not the one this tool measures, so read the whole note before acting.

The textures and the field recordings are 24-bit, and 89 of the library's 94 GB are those two
folders. The bottom eight bits hold 8.00 bits of entropy -- pure noise, which FLAC has to store
verbatim, and which is why it only reaches 56 to 68 % there against 36 % on the 16-bit archive.
Rewriting them at sixteen bits would save 55 %.

Whether that is free is two questions, and they have different answers.

AUDIBILITY, which is what this tool measures. Across the whole band the quantisation error sits
38 to 83 dB under the material's own noise floor, but a chain that isolates a narrow, quiet band
and lifts it works inside one band, where the margin is smaller. So the rule is measured per file:
find the quietest stretch, take the margin between the content and the error a 16-bit rounding
would add, band by band, and keep 24 bits where the smallest of those margins is under the
threshold. Run over the whole library on 13.09.2026 -- 9617 recordings, 735 of them already 16 bit
-- the rule does discriminate: worst margin -50 dB, tenth percentile -3, median +11, ninetieth
+50 dB. At a threshold of 12 dB, 4370 files could go to sixteen bits and 33 GB of the 95 would be
saved. The tightest are the public-domain space recordings in Archive (Webb, InSight, the
marsquakes, STS-135), whose quiet bands sit fifty decibels below where the error would land.

REPRODUCIBILITY, which it does not. The spectral source analyses a recording into thirty-two bands
and resynthesises it, and that amplifies any disturbance of its input enormously. Measured on
gamelan-04533_A5 at src1_spec_rate=1, src1_spec_breath=-1, the render was deterministic (the same
file twice differs by 376 dB) and the difference tracked the perturbation rather than standing
still, so this is not chaos -- but the gain is 76 to 91 dB: rounding the texture to 23 bits, a
perturbation BELOW the 24-bit floor, already moved the render to 35 dB under its own level, and
16 bits moved it to 2 dB under. Four other chains were safe (plain grains, a high-Q z-plane at
12 kHz, a stretch of 300, the whole Cosmos: 72 to 96 dB down). 5330 presets -- 37 % of the library
-- point a slot at the spectral type, and between them they read 3639 of the 8186 recordings the
packs name. Intersecting the two rules leaves 2435 files, 33 GB, saving about 18 GB: 19 % of the
library, bought with a standing constraint that no future preset may point a spectral slot at any
of those 2435 files, which the generator does not honour and cannot be made to cheaply.

So: the recordings keep 24 bits. The saving that IS free is elsewhere -- Impulses (3.8 GB) and
Wavetables (0.94 GB) still ship as uncompressed WAV, and on a sample of 40 each FLAC keeps only
20 % of them, so 3.8 GB comes off for nothing. The core already reads FLAC (dr_flac in
Core/src/WavFile.cpp).

This tool remains the way to answer the audibility question for a new folder:

    python Tools/library/bit_depth_audit.py [--jobs 8] [--margin 12] [--out audit.json]

Reads a spread of windows rather than whole files: a hundred gigabytes read end to end is an hour
of disk for an answer that a few seconds out of each recording gives just as well.
"""
import argparse
import concurrent.futures
import glob
import json
import os
import sys

import numpy as np
import soundfile as sf

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BANDS = [(20, 300), (300, 1000), (1000, 3000), (3000, 6000), (6000, 10000), (10000, 14000), (14000, 22050)]
WIN = 1 << 14          # 0.37 s at 44.1 kHz: long enough for the low bands to be a band at all
PROBES = 24            # windows spread across the file


def measure(path):
    """The smallest band margin in the quietest window, and what the file costs now."""
    try:
        info = sf.info(path)
    except Exception as e:                        # noqa: BLE001
        return {"file": path, "error": str(e)}
    bits = {"PCM_16": 16, "PCM_24": 24, "PCM_32": 32, "FLOAT": 32}.get(info.subtype, 0)
    out = {"file": os.path.relpath(path, ROOT).replace("\\", "/"),
           "bytes": os.path.getsize(path), "subtype": info.subtype,
           "rate": info.samplerate, "ch": info.channels, "seconds": round(info.duration, 2)}
    if bits <= 16:                                # nothing to take away
        out["keep24"] = False
        out["margin"] = None
        return out
    frames = info.frames
    if frames < WIN * 2:
        out["keep24"] = False
        out["margin"] = None
        return out
    # The quietest of the probes, then that one window in detail.
    starts = [int(i * (frames - WIN) / max(PROBES - 1, 1)) for i in range(PROBES)]
    quiet, at = None, starts[0]
    for s in starts:
        try:
            with sf.SoundFile(path) as fh:
                fh.seek(s)
                x = fh.read(WIN, dtype="float64", always_2d=True)
        except Exception:                          # noqa: BLE001
            continue
        if x.shape[0] < WIN:
            continue
        r = float(np.sqrt(np.mean(x ** 2)))
        if quiet is None or r < quiet:
            quiet, at = r, s
    with sf.SoundFile(path) as fh:
        fh.seek(at)
        x = fh.read(WIN, dtype="float64", always_2d=True)
    # Plain rounding: the error a conversion without dither would add, which is the worst case.
    e = x - np.round(x * 32768.0) / 32768.0
    w = np.hanning(WIN)
    fr = np.fft.rfftfreq(WIN, 1.0 / info.samplerate)
    worst, where = 1.0e9, ""
    for ch in range(x.shape[1]):
        fx = np.abs(np.fft.rfft(x[:, ch] * w))
        fe = np.abs(np.fft.rfft(e[:, ch] * w))
        for lo, hi in BANDS:
            m = (fr >= lo) & (fr < hi)
            if not m.any():
                continue
            a = float(np.sqrt(np.mean(fx[m] ** 2)))
            b = float(np.sqrt(np.mean(fe[m] ** 2)))
            d = 20.0 * np.log10(max(a, 1e-20) / max(b, 1e-20))
            if d < worst:
                worst, where = d, "%d-%d Hz" % (lo, hi)
    out["margin"] = round(worst, 1)
    out["band"] = where
    out["quiet_dbfs"] = round(20.0 * np.log10(max(quiet or 1e-20, 1e-20)), 1)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--margin", type=float, default=12.0, help="dB of band margin below which a file keeps 24 bits")
    ap.add_argument("--out", default=os.path.join(ROOT, "work", "library", "bit-depth.json"))
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()

    files = []
    for folder in ("Textures", "FieldRecordings", "Archive"):
        files += sorted(glob.glob(os.path.join(ROOT, "Library", folder, "**", "*.flac"), recursive=True))
    if a.limit:
        files = files[:a.limit]
    print("%d recordings" % len(files), flush=True)

    rows, done = [], 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for r in ex.map(measure, files):
            rows.append(r)
            done += 1
            if done % 500 == 0:
                print("  %d/%d" % (done, len(files)), flush=True)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w", encoding="utf-8") as f:
        json.dump(rows, f)

    good = [r for r in rows if "error" not in r]
    wide = [r for r in good if r.get("margin") is not None]
    keep = [r for r in wide if r["margin"] < a.margin]
    total = sum(r["bytes"] for r in good)
    convertible = sum(r["bytes"] for r in wide if r["margin"] >= a.margin)
    print("\n%d read, %d already 16 bit or shorter than a window" % (len(good), len(good) - len(wide)))
    if wide:
        m = sorted(r["margin"] for r in wide)
        print("band margin: worst %.0f dB, 10th %.0f, median %.0f, 90th %.0f dB"
              % (m[0], m[len(m) // 10], m[len(m) // 2], m[9 * len(m) // 10]))
        for t in (0, 6, 12, 18, 24):
            n = sum(1 for r in wide if r["margin"] < t)
            print("   under %2d dB of margin: %5d files (%4.1f %%)" % (t, n, 100.0 * n / len(wide)))
    print("\nat a threshold of %.0f dB: %d files keep 24 bits, %d go to 16"
          % (a.margin, len(keep), len(wide) - len(keep)))
    print("the library is %.1f GB; %.1f GB of it may be halved -> saves about %.1f GB"
          % (total / 1e9, convertible / 1e9, convertible * 0.55 / 1e9))
    if keep:
        worst = sorted(keep, key=lambda r: r["margin"])[:8]
        print("\nthe tightest, which keep their bits:")
        for r in worst:
            print("   %-46s %5.1f dB at %s" % (os.path.basename(r["file"])[:46], r["margin"], r.get("band", "")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
