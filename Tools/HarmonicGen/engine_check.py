"""Hoert die Engine eine Harmonic-Tabelle so, wie sie entworfen wurde?

harmonicgen.py prueft den Rundlauf gegen einen NACHBAU von Wavetable::analyse. Das beweist, dass die
Datei stimmt -- nicht, dass der Klang stimmt. Dazwischen liegen der Lader, spectrumAt, die Phasor-Bank
und alles, was die Stimme danach mit dem Signal anstellt.

Also: eine Tabelle erzeugen, mit ambient_render an festen Positionen spielen, alles Faerbende und
Verschmierende aus (Filter, Z-Plane, Luft, Raum, Hall, Chor, Delay, Strang-Verstimmung, ITD, Tiefe),
aus dem Ausgang die Teiltonbetraege messen und gegen das Spektrum stellen, das an dieser Position
liegt: Frame 0, die Mitte (lineare Mischung der Frames 31 und 32, wie spectrumAt sie bildet) und Frame 63.

Gemessen wird ueber die Leistung in einem Band um jeden Teilton k*f0; f0 ist der staerkste Gipfel
nahe der gespielten Note, damit eine Stimmung abseits von 440 Hz das Ergebnis nicht verfaelscht.

    python engine_check.py [--families choir,glass,organ] [--seed 7] [--note 45]
"""
import argparse
import math
import os
import subprocess
import sys
import tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import harmonicgen as hg  # noqa: E402

RENDER = os.path.join(ROOT, "bin", "msvc", "ambient_render.exe")

# Jeder Name hier steht in `ambient_render --list`; ein unbekannter bricht den Render mit Fehler ab.
NEUTRAL = {
    # Quelle 1: die Tabelle, fest an ihrer Position
    "src1_type": "Harmonic", "src1_table": "User", "src1_pos_drift": "0", "src1_transport": "0",
    "src1_root": "0", "src1_unison": "1", "src1_drift": "0", "src1_octave": "0", "src1_pan": "0",
    "src2_type": "Off", "src3_type": "Off", "src4_type": "Off",
    # Straenge: einer, unverstimmt, ohne Streuung
    "strands": "1", "detune": "0", "drift": "0", "spread": "0", "bloom": "0", "partial_spread": "0",
    "shimmer": "0", "inharmonic": "0",
    # Huellkurve: schnell oben und dort bleiben
    "attack": "0.2", "decay": "0.2", "sustain": "1",
    # alles, was das Spektrum faerbt oder verschmiert
    "filter_on": "off", "z_mix": "0", "air": "0", "presence": "0", "breath": "0", "phase_width": "0",
    "doppler": "0", "externalise": "0", "itd": "0", "depth": "0", "pan_drift": "0", "arc": "0",
    "ens_mix": "0", "dly_mix": "0", "dly2_mix": "0", "near_mix": "0", "blur_mix": "0", "far_level": "0",
    "room_level": "0", "early_level": "0", "cloud_level": "0", "cosmos_send": "0", "body_level": "0",
    "patina": "0", "fb_bus": "0", "sub_level": "0", "strike_level": "0", "haas": "0",
    "master_tilt": "0", "side_air": "0", "width": "1",
    # niemand spielt ausser der einen Note
    "brain_on": "off", "brain2_on": "off", "auto_mode": "0", "coherence": "0", "sympathy": "0", "tide": "0",
}


def render(table_path, pos, note, seconds, out):
    cmd = [RENDER, "--wavetable", table_path, "--notes", str(note), "--seconds", str(seconds),
           "--hour", "12", "--out", out]
    for k, v in list(NEUTRAL.items()) + [("src1_pos", f"{pos}")]:
        cmd += ["--set", f"{k}={v}"]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0 or not os.path.exists(out):
        raise RuntimeError(f"Render fehlgeschlagen ({r.returncode}): {(r.stdout or '')[-400:]} {(r.stderr or '')[-400:]}")
    return r.stdout


def harmonics(path, f_guess, count=hg.PARTIALS):
    x, sr = sf.read(path, always_2d=True)
    mono = x.mean(axis=1)
    seg = mono[len(mono) // 2:]
    n = 1 << int(math.floor(math.log2(len(seg))))
    seg = seg[:n] * np.hanning(n)
    p = np.abs(np.fft.rfft(seg)) ** 2
    f = np.fft.rfftfreq(n, 1.0 / sr)

    def band(hz, cents):
        sel = (f >= hz * 2 ** (-cents / 1200.0)) & (f <= hz * 2 ** (cents / 1200.0))
        return float(p[sel].sum()), sel

    _, sel = band(f_guess, 60.0)
    idx = np.where(sel)[0]
    f0 = float(f[idx[int(np.argmax(p[idx]))]])
    amps = np.array([math.sqrt(band(k * f0, 25.0)[0]) if k * f0 < 0.45 * sr else 0.0 for k in range(1, count + 1)])
    return amps / max(np.linalg.norm(amps), 1e-12), f0


def compare(want, got, floor_db=-30.0):
    """Nur Teiltoene, die in der Tabelle ueber floor_db liegen: darunter misst man den Rauschteppich."""
    w_db = 20 * np.log10(np.maximum(want, 1e-9))
    g_db = 20 * np.log10(np.maximum(got, 1e-9))
    audible = w_db >= w_db.max() + floor_db
    diff = g_db[audible] - w_db[audible]
    corr = float(np.corrcoef(w_db[audible], g_db[audible])[0, 1]) if audible.sum() > 2 else 1.0
    return corr, float(np.abs(diff).max()), float(np.median(diff)), int(audible.sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--families", default="choir,glass,organ")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--note", type=int, default=45)
    ap.add_argument("--seconds", type=float, default=10.0)
    a = ap.parse_args()
    if not os.path.exists(RENDER):
        sys.exit("ambient_render nicht gefunden: " + RENDER)
    f_note = 440.0 * 2 ** ((a.note - 69) / 12.0)
    work = tempfile.mkdtemp(prefix="hgcheck_")
    bad = 0
    for fam in [x.strip() for x in a.families.split(",") if x.strip()]:
        s, params, _ = hg.build(fam, a.seed)
        table = os.path.join(work, f"check_{fam}_{a.seed}.wav")
        hg.write_table(table, hg.render(s), dict(family=fam, seed=a.seed, recipe=params))
        print(f"{fam}, Seed {a.seed}, Note {a.note} ({f_note:.1f} Hz)")
        mid = hg.normalise_frames(0.5 * (s[31] + s[32])[None])[0]
        for pos, want, label in ((0.0, s[0], "Frame 0"), (0.5, mid, "Mitte 31/32"), (1.0, s[-1], "Frame 63")):
            out = os.path.join(work, f"out_{fam}_{pos}.wav")
            render(table, pos, a.note, a.seconds, out)
            got, f0 = harmonics(out, f_note)
            corr, worst, median, n = compare(want, got)
            ok = corr > 0.98 and worst < 3.0
            bad += not ok
            print(f"  {'ok  ' if ok else 'FEHL'} Position {pos:.1f} ({label}): f0 {f0:.2f} Hz, {n} hoerbare Teiltoene, "
                  f"Korrelation {corr:.4f}, groesste Abweichung {worst:.2f} dB, Median {median:+.2f} dB")
            print("       Tabelle " + " ".join(f"{20 * math.log10(max(v, 1e-9)):6.1f}" for v in want[:12]))
            print("       Ausgang " + " ".join(f"{20 * math.log10(max(v, 1e-9)):6.1f}" for v in got[:12]))
    print(f"\n  {'Engine spielt die Tabellen wie entworfen' if not bad else f'{bad} Abweichung(en) zwischen Tabelle und Klang'}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
