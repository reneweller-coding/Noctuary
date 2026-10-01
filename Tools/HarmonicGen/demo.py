"""Hoerprobe fuer Harmonic-Tabellen: je Familie ein paar Tabellen durch die Engine, Position 0 -> 1.

engine_check.py sagt, dass die Engine spielt, was entworfen wurde. Ob es gut klingt, sagt es nicht.
Dafuer: je Tabelle ein offener Akkord (A2 E3 A3), zwei leicht verstimmte Straenge, etwas Hall, und die
Position faehrt ab der ersten Sekunde in zehn Sekunden einmal durch die ganze Tabelle. Alle Abschnitte
auf denselben Pegel, hintereinander in eine Datei, dazu eine Liste mit den Zeitmarken.

    python demo.py [--tables DIR] [--families metal,sub] [--per-family 2] [--notes 45,52,57] [--out DIR] [--name NAME]
                   [--prefix harmonic|ambient] [--type Harmonic|Wavetable]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import harmonicgen as hg  # noqa: E402
from engine_check import RENDER  # noqa: E402

SETTINGS = {
    "src1_type": "Harmonic", "src1_table": "User", "src1_pos_drift": "0", "src1_transport": "0", "src1_root": "0",
    "src2_type": "Off", "src3_type": "Off", "src4_type": "Off",
    "strands": "2", "detune": "5", "drift": "2", "spread": "0.5", "shimmer": "0", "inharmonic": "0",
    "attack": "1", "decay": "0.5", "sustain": "1", "release": "2",
    "filter_on": "off", "z_mix": "0", "air": "0", "ens_mix": "0", "dly_mix": "0", "cloud_level": "0",
    "far_level": "0.35", "near_mix": "0.15",
    "brain_on": "off", "brain2_on": "off", "auto_mode": "0",
}
SECONDS = 12.0
SWEEP = "0 src1_pos 0\n1 src1_pos 1 over 10\n"
LEVEL_DB = -20.0


def render_one(table, score, out, notes, kind="Harmonic"):
    cmd = [RENDER, "--wavetable", table, "--notes", notes, "--seconds", str(SECONDS), "--hour", "12",
           "--score", score, "--out", out]
    for k, v in dict(SETTINGS, src1_type=kind).items():
        cmd += ["--set", f"{k}={v}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"Render fehlgeschlagen ({r.returncode}): {(r.stdout or '')[-400:]} {(r.stderr or '')[-400:]}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tables", default=os.path.join(ROOT, "Library", "Wavetables"))
    ap.add_argument("--out", default=os.path.normpath(os.path.join(ROOT, "..", "_Sources", "WavetableSources", "demo-renders")))
    ap.add_argument("--name", default="harmonic_demo", help="Dateiname ohne Endung fuer .flac, .mp3 und .txt")
    ap.add_argument("--families", default=",".join(hg.FAMILIES))
    ap.add_argument("--per-family", type=int, default=2)
    ap.add_argument("--prefix", default="harmonic", help="harmonic fuer HarmonicGen, ambient fuer AmbientGen")
    ap.add_argument("--type", default="Harmonic", choices=["Harmonic", "Wavetable"], help="welcher Tabellen-Typ spielt")
    ap.add_argument("--notes", default="45,52,57")
    a = ap.parse_args()
    a.tables = os.path.abspath(a.tables)    # die Renders laufen im Projektordner: ein relativer Pfad griffe dort daneben
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="hgdemo_")
    score = os.path.join(work, "sweep.score")
    with open(score, "w", encoding="utf-8") as f:
        f.write(SWEEP)

    # Paths below --tables: the library's tables sit on shelves (Harmonic, Classic, Ambient).
    files = sorted(os.path.relpath(os.path.join(d, x), a.tables).replace(os.sep, "/")
                   for d, _, names in os.walk(a.tables) for x in names if x.endswith(".wav"))
    chosen = []
    for fam in [x.strip() for x in a.families.split(",") if x.strip()]:
        mine = [x for x in files if os.path.basename(x).startswith(f"{a.prefix}_{fam}_")]
        if mine:
            step = max(1, len(mine) // a.per_family)
            chosen += mine[::step][:a.per_family]

    parts, marks, t, rate = [], [], 0.0, None
    for name in chosen:
        out = os.path.join(work, os.path.basename(name))
        render_one(os.path.join(a.tables, name), score, out, a.notes, a.type)
        x, sr = sf.read(out, always_2d=True)
        rate = rate or sr
        rms = float(np.sqrt(np.mean(x[int(2 * sr):] ** 2)))
        x = x * (10.0 ** (LEVEL_DB / 20.0) / max(rms, 1.0e-9))
        fade = int(0.8 * sr)
        x[-fade:] *= np.linspace(1.0, 0.0, fade)[:, None]
        peak = float(np.abs(x).max())
        if peak > 0.97:
            x *= 0.97 / peak
        gap = np.zeros((int(0.6 * sr), x.shape[1]))
        marks.append((t, name))
        parts += [x, gap]
        t += (len(x) + len(gap)) / sr

    flac = os.path.join(a.out, a.name + ".flac")
    sf.write(flac, np.concatenate(parts), rate, subtype="PCM_24")
    ffmpeg = shutil.which("ffmpeg")
    mp3 = os.path.join(a.out, a.name + ".mp3")
    if ffmpeg:
        subprocess.run([ffmpeg, "-y", "-loglevel", "error", "-i", flac, "-b:a", "192k", mp3], check=True)
    with open(os.path.join(a.out, a.name + ".txt"), "w", encoding="utf-8") as f:
        f.write(f"Akkord MIDI {a.notes}, je Tabelle {SECONDS:.0f} s; die Position faehrt ab 1 s in 10 s von 0 nach 1\n\n")
        for t0, name in marks:
            f.write(f"{int(t0 // 60)}:{t0 % 60:04.1f}  {name}\n")
    print(f"{len(chosen)} Tabellen, {t / 60:.1f} min -> {flac}" + (f" und {mp3}" if ffmpeg else ""))


if __name__ == "__main__":
    main()
