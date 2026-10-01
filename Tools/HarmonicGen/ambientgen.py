"""AmbientGen: Wavetables fuer Drones und Ambient, ueber HarmonicGen hinaus.

HarmonicGen entwirft Spektren fuer den Harmonic-Oszillator: 32 Teiltoene, Phase egal. AmbientGen schreibt
Tabellen, die in BEIDEN Tabellen-Typen tragen: im Harmonic-Typ (er liest die Teiltoene 1..32 jedes Frames)
und im klassischen Wavetable-Typ, der die Zyklen als Samples spielt. Darum:

  128 Teiltoene je Zyklus    der Wavetable-Typ hoert bis zum 128., der Harmonic-Typ die ersten 32 exakt
  feste Phasen je Teilton     Schroeder-Phasen, in allen Frames gleich: niedriger Scheitelfaktor, und das
                              Durchfahren der Position ueberblendet Betraege, nie Phasen (kein Phasing)
  gleiche Energie je Frame    die Position aendert die Farbe, nicht den Pegel
  Leitplanken                 die von HarmonicGen (Grundton, Schwerpunkt, Glaette, Fuelle, Doppel gegen
                              die Bibliothek), dazu Glaette des vollen Spektrums und eine Obergrenze fuer
                              die Energie ueber Teilton 32

Die Familien (die Ideen vom 11.09.2026):

  consonant      Spektren, deren Teiltoene in einer Stimmung moeglichst wenig rau klingen: die Beträge werden
                 gegen Setharess Dissonanzkurve (nach Plomp und Levelt) fuer einen Akkord der Stimmung
                 optimiert -- reine Terzen, Septimen, Pythagoras, gleichstufig, Slendro, Pelog, Bohlen-Pierce
  overtone       Obertongesang: eine schmale Resonanz singt ueber einem weichen Grundklang eine Melodie auf
                 der Obertonreihe, Frame fuer Frame gleitend; mit der Position gespielt, singt die Tabelle
  vowel_*        Chor nach Stimmlage (bass, tenor, alto, soprano): Glottisquelle mal fuenf Formanten je Vokal,
                 fuer eine Note der Stimmlage ausgewertet -- Formanten liegen in Hz fest, darum ein Satz je
                 Lage, und das JSON sagt, fuer welchen Tonumfang
  bowed          gestrichene Saite: Strichstelle als Kerbenkamm, Bogendruck als Neigung, Resonanzkoerper
                 (Geige, Bratsche, Cello, Gambe) als Formanten in Hz
  tube           Rohr: Klarinette (ungerade Teiltoene unter der Grenzfrequenz), Floete, Blech (hoehere
                 Teiltoene wachsen schneller mit der Lautstaerke, nach Risset), Didgeridoo (Zungenformanten)
  sampled        gemessen statt ausgeschnitten: tonhoehengepruefte SA3-Texturen (Einzeltoene aus tonalen,
                 gehaltenen Kategorien). f0 wird je Analysefenster um die benannte Note verfolgt (fein auf
                 0,2 Cent), die Teiltoene an k*f0 gemessen, unharmonische und verstimmte Fenster verworfen,
                 der Verlauf ueber die Aufnahme geglaettet -- eine Tabelle aus den Spektren eines echten Tons
  otmorph        optimaler Transport zwischen den Spektren zweier oder dreier Tabellen anderer Familien:
                 Verschiebungs-Interpolation der Quantilfunktionen im Log-Frequenzraum, die Masse gleitet
                 zwischen den Teiltoenen, statt ueberzublenden

Auswahl: je Familie werden dreimal so viele Kandidaten gebaut wie gebraucht und nach MAP-Elites auf ein
Gitter aus Schwerpunkt, Rauigkeit und Bewegung verteilt; aus jeder Zelle kommen die besten (wenig rau im
Akkord, lebendig, reich), reihum, bis die Stueckzahl steht -- Vielfalt vor Menge.

    python ambientgen.py generate --out DIR [--count 1000] [--seed 1] [--families consonant,...] [--avoid DIR] [--jobs 8]
    python ambientgen.py selftest
    python ambientgen.py check DIR [--sample 8] [--note 45]
"""
import argparse
import collections
import concurrent.futures as cf
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import harmonicgen as hg  # noqa: E402

FRAMES, FRAME_LEN, SR, VIEW = hg.FRAMES, hg.FRAME_LEN, hg.SR, hg.PARTIALS
NFULL = 128
HF = np.arange(1, NFULL + 1, dtype=np.float64)
PHASES = np.pi * HF * HF / NFULL
VERSION = 1
PREFIX = "ambient"
STEP_MAX = hg.STEP_MAX
OVERSAMPLE = 3
TEXTURES = os.path.join(ROOT, "Library", "Textures")
LEDGER = os.path.normpath(os.path.join(ROOT, "..", "_Sources", "StableAudio3", "build", "ledger.jsonl"))


# ---------------------------------------------------------------------------- Bausteine (Frames x 128)

def through2(pts):
    """Wie hg.through, fuer Stuetzstellen mit mehreren Spalten: (Stuetzstellen, n) -> (Frames, n)."""
    pts = np.asarray(pts, dtype=np.float64)
    if pts.shape[0] == 1:
        return np.repeat(pts, FRAMES, axis=0)
    pos = np.linspace(0.0, 1.0, FRAMES) * (pts.shape[0] - 1)
    i = np.minimum(pos.astype(int), pts.shape[0] - 2)
    w = (0.5 - 0.5 * np.cos(np.pi * (pos - i)))[:, None]
    return pts[i] * (1.0 - w) + pts[i + 1] * w


def col(x):
    return np.broadcast_to(np.asarray(x, dtype=np.float64).reshape(-1, 1), (FRAMES, 1))


def ftilt(alpha):
    return HF[None, :] ** (-col(alpha))


def flowpass(hc, order=2.0):
    return 1.0 / np.sqrt(1.0 + (HF[None, :] / col(hc)) ** (2.0 * order))


def fwalk(rng, depth_db, keys=5):
    return 10.0 ** (through2(rng.uniform(-1.0, 1.0, size=(keys, NFULL)) * depth_db) / 20.0)


def bumps(f_ref, modes):
    """Glocken im Log-Frequenzraum auf dem Spektrum eines Tons der Hoehe f_ref; modes = (Mitte Hz, dB, Breite Oktaven),
    Mitte und dB duerfen Verlaeufe (64 Werte) sein."""
    freq = HF[None, :] * f_ref
    out = np.zeros((FRAMES, NFULL))
    for c, g, bw in modes:
        d = np.log2(freq / col(c)) / bw
        out += col(g) * np.exp(-0.5 * d * d)
    return 10.0 ** (out / 20.0)


def unit(s):
    s = np.maximum(np.asarray(s, dtype=np.float64), 0.0)
    return s / np.maximum(np.sqrt((s * s).sum(axis=1, keepdims=True)), 1.0e-12)


def view32(s):
    return unit(np.asarray(s)[:, :VIEW])


# ---------------------------------------------------------------------------- Rauigkeit (Sethares 1993)

def pair_rough(f1, f2):
    """Dissonanz zweier Sinustoene gleicher Staerke nach Sethares' Anpassung der Plomp-Levelt-Kurve."""
    fmin = np.minimum(f1, f2)
    s = 0.24 / (0.0207 * fmin + 18.96)
    d = np.abs(f2 - f1) * s
    return np.exp(-3.5 * d) - np.exp(-5.75 * d)


def rough_matrix(f0, ratios, n):
    """G so, dass a' G a die Rauigkeit eines Akkords aus Stimmen mit dem Spektrum a ist (Produktform)."""
    k = np.arange(1, n + 1, dtype=np.float64)
    g = np.zeros((n, n))
    for p in ratios:
        for q in ratios:
            g += pair_rough(k[:, None] * f0 * p, k[None, :] * f0 * q)
    return 0.5 * (g + g.T)


_G_DESC = None


def g_desc():
    global _G_DESC
    if _G_DESC is None:                                   # Quinte und Oktave auf A2: der Drone-Akkord
        _G_DESC = rough_matrix(110.0, (1.0, 1.5, 2.0), VIEW)
    return _G_DESC


def descriptors(s):
    v = view32(s)
    g = g_desc()
    rough = float(np.mean([v[i] @ g @ v[i] for i in (0, 21, 42, 63)]))
    p = s * s
    cen_hz = float(np.median((p * (HF[None, :] * 220.0)).sum(axis=1) / np.maximum(p.sum(axis=1), 1e-18)))
    move = float(hg.steps(s).mean())
    rich = float(np.median((s > s.max(axis=1, keepdims=True) * 0.01).sum(axis=1)))
    return dict(rough=rough, centroid_hz=cen_hz, move=move, rich=rich)


# ---------------------------------------------------------------------------- consonant

TUNINGS = {
    "just_major": (1.0, 5 / 4, 3 / 2, 2.0), "just_minor": (1.0, 6 / 5, 3 / 2, 2.0),
    "open_fifths": (1.0, 3 / 2, 2.0, 3.0), "septimal": (1.0, 7 / 6, 3 / 2, 7 / 4),
    "harmonic_seventh": (1.0, 5 / 4, 3 / 2, 7 / 4), "pythagorean": (1.0, 9 / 8, 81 / 64, 3 / 2),
    "equal_major": (1.0, 2 ** (4 / 12), 2 ** (7 / 12), 2.0), "equal_minor7": (1.0, 2 ** (3 / 12), 2 ** (7 / 12), 2 ** (10 / 12)),
    "slendro": (1.0, 2 ** (1 / 5), 2 ** (2 / 5), 2 ** (3 / 5)), "pelog": (1.0, 2 ** (120 / 1200), 2 ** (540 / 1200), 2 ** (670 / 1200)),
    "bohlen_pierce": (1.0, 5 / 3, 7 / 3, 3.0),
}


def optimise_consonance(prior, g, mu, iters=160):
    """min a'Ga + mu |a - p|^2 unter a >= 0 und |a| = 1, projizierter Gradient. mu ist relativ zum groessten
    Eigenwert von G, also unabhaengig von Stimmung und Lage; klein heisst: weit weg vom Ausgangsspektrum."""
    n = g.shape[0]
    p = prior[:n] / max(float(np.linalg.norm(prior[:n])), 1e-12)
    lam = float(np.linalg.eigvalsh(g)[-1])
    m = mu * lam
    step = 0.5 / (lam + m)
    a = p.copy()
    for _ in range(iters):
        a = np.maximum(a - step * (2.0 * (g @ a) + 2.0 * m * (a - p)), 0.0)
        nrm = float(np.linalg.norm(a))
        if nrm < 1e-12:
            return p
        a /= nrm
    return a


def fam_consonant(rng):
    names = [str(x) for x in rng.choice(list(TUNINGS), size=int(rng.integers(1, 3)), replace=False)]
    f0 = float(rng.uniform(98.0, 196.0))
    n = 48
    keys, info = [], []
    for k in range(3):
        tun = names[k % len(names)]
        prior = (HF ** -rng.uniform(0.5, 1.3)) / np.sqrt(1.0 + (HF / rng.uniform(8.0, 40.0)) ** 3.0)
        prior = prior * 10.0 ** (rng.uniform(-8.0, 0.0, size=NFULL) / 20.0)
        g = rough_matrix(f0, TUNINGS[tun], n)
        mu = float(rng.uniform(0.04, 0.5))
        a = optimise_consonance(prior, g, mu)
        before = float((prior[:n] / np.linalg.norm(prior[:n])) @ g @ (prior[:n] / np.linalg.norm(prior[:n])))
        full = prior.copy()
        full[:n] = a * np.linalg.norm(prior[:n])
        tail = float(np.mean(full[32:n]) / max(np.mean(prior[32:n]), 1e-12))
        full[n:] = prior[n:] * tail                        # der Rest folgt dem Ausgangsspektrum im selben Mass
        keys.append(full)
        info.append(dict(tuning=tun, mu=round(mu, 3), rough_before=round(before, 5), rough_after=round(float(a @ g @ a), 5)))
    order = [int(i) for i in rng.permutation(3)]
    return through2(np.stack([keys[i] for i in order])), dict(f0_hz=round(f0, 1), tunings=names, keys=[info[i] for i in order])


# ---------------------------------------------------------------------------- overtone

OVERTONE_LADDER = (4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16)


def fam_overtone(rng):
    n_notes = int(rng.integers(4, 7))
    idx = int(rng.integers(2, 7))
    path = [OVERTONE_LADDER[idx]]
    for _ in range(n_notes - 1):
        idx = int(np.clip(idx + rng.choice([-2, -1, 1, 1, 2]), 0, len(OVERTONE_LADDER) - 1))
        path.append(OVERTONE_LADDER[idx])
    glide = float(rng.uniform(0.45, 0.7))
    seg = FRAMES / n_notes
    centre = np.empty(FRAMES)
    for f in range(FRAMES):
        s = min(int(f / seg), n_notes - 1)
        u = f / seg - s
        a, b = path[s], path[min(s + 1, n_notes - 1)]
        w = 0.0 if u < 1.0 - glide else 0.5 - 0.5 * math.cos(math.pi * (u - (1.0 - glide)) / glide)
        centre[f] = a + (b - a) * w
    width = float(rng.uniform(0.3, 0.6))
    # Die gesungene Resonanz bekommt einen Pegel relativ zum Grundton, keine Verstaerkung: ueber einem steil
    # fallenden Koerper liess selbst +34 dB den 9. Teilton unter dem vierten, und die Melodie war weg.
    level = 10.0 ** (hg.curve(rng, -4.0, 2.0) / 20.0)
    peak = np.exp(-0.5 * ((HF[None, :] - centre[:, None]) / width) ** 2)
    body = ftilt(hg.curve(rng, 1.4, 2.2)) * flowpass(hg.curve(rng, 5.0, 12.0), 1.5)
    body = body / body.max(axis=1, keepdims=True)
    spec = body + level[:, None] * peak
    double = bool(rng.random() < 0.5)
    if double:                                             # die tiefere, breitere zweite Resonanz des Khoomei
        c2 = centre * rng.uniform(0.45, 0.6)
        spec = spec + 0.3 * level[:, None] * np.exp(-0.5 * ((HF[None, :] - c2[:, None]) / rng.uniform(0.8, 1.5)) ** 2)
    return spec, dict(path=path, width=round(width, 2), glide=round(glide, 2), double=double)


# ---------------------------------------------------------------------------- vowel (Stimmlagen)

# Formanten F1..F5 (Hz), ihre Pegel (dB) und Bandbreiten (Hz) je Stimmlage und Vokal, nach der Formanttabelle des
# Csound-Handbuchs (Anhang "Formant Values").
FORMANT_SETS = {
    "soprano": {"a": ((800, 1150, 2900, 3900, 4950), (0, -6, -32, -20, -50), (80, 90, 120, 130, 140)),
                "e": ((350, 2000, 2800, 3600, 4950), (0, -20, -15, -40, -56), (60, 100, 120, 150, 200)),
                "i": ((270, 2140, 2950, 3900, 4950), (0, -12, -26, -26, -44), (60, 90, 100, 120, 120)),
                "o": ((450, 800, 2830, 3800, 4950), (0, -11, -22, -22, -50), (70, 80, 100, 130, 135)),
                "u": ((325, 700, 2700, 3800, 4950), (0, -16, -35, -40, -60), (50, 60, 170, 180, 200))},
    "alto": {"a": ((800, 1150, 2800, 3500, 4950), (0, -4, -20, -36, -60), (80, 90, 120, 130, 140)),
             "e": ((400, 1600, 2700, 3300, 4950), (0, -24, -30, -35, -60), (60, 80, 120, 150, 200)),
             "i": ((350, 1700, 2700, 3700, 4950), (0, -20, -30, -36, -60), (50, 100, 120, 150, 200)),
             "o": ((450, 800, 2830, 3500, 4950), (0, -9, -16, -28, -55), (70, 80, 100, 130, 135)),
             "u": ((325, 700, 2530, 3500, 4950), (0, -12, -30, -40, -64), (50, 60, 170, 180, 200))},
    "tenor": {"a": ((650, 1080, 2650, 2900, 3250), (0, -6, -7, -8, -22), (80, 90, 120, 130, 140)),
              "e": ((400, 1700, 2600, 3200, 3580), (0, -14, -12, -14, -20), (70, 80, 100, 120, 120)),
              "i": ((290, 1870, 2800, 3250, 3540), (0, -15, -18, -20, -30), (40, 90, 100, 120, 120)),
              "o": ((400, 800, 2600, 2800, 3000), (0, -10, -12, -12, -26), (40, 80, 100, 120, 120)),
              "u": ((350, 600, 2700, 2900, 3300), (0, -20, -17, -14, -26), (40, 60, 100, 120, 120))},
    "bass": {"a": ((600, 1040, 2250, 2450, 2750), (0, -7, -9, -9, -20), (60, 70, 110, 120, 130)),
             "e": ((400, 1620, 2400, 2800, 3100), (0, -12, -9, -12, -18), (40, 80, 100, 120, 120)),
             "i": ((250, 1750, 2600, 3050, 3340), (0, -30, -16, -22, -28), (60, 90, 100, 120, 120)),
             "o": ((400, 750, 2400, 2600, 2900), (0, -11, -21, -20, -40), (40, 80, 100, 120, 120)),
             "u": ((350, 600, 2400, 2675, 2950), (0, -20, -32, -28, -36), (40, 80, 100, 120, 120))},
}
REGISTER_MIDI = {"bass": (40, 64), "tenor": (48, 72), "alto": (53, 77), "soprano": (60, 84)}


def vowel_family(register):
    def fam(rng):
        lo, hi = REGISTER_MIDI[register]
        midi = float(rng.uniform(lo + 3, hi - 9))
        f_ref = 440.0 * 2.0 ** ((midi - 69.0) / 12.0)
        seq = [str(v) for v in rng.choice(list("aeiou"), size=int(rng.integers(2, 5)))]
        table = FORMANT_SETS[register]
        freqs = through2(np.array([table[v][0] for v in seq], dtype=np.float64))
        levels = through2(np.array([table[v][1] for v in seq], dtype=np.float64))
        widths = through2(np.array([table[v][2] for v in seq], dtype=np.float64))
        freqs = freqs * 2.0 ** (through2(rng.uniform(-30.0, 30.0, size=(4, 5))) / 1200.0)   # langsam wandernde Formanten
        chorus = float(rng.uniform(1.0, 1.5))              # mehrere Stimmen verbreitern die Formanten
        freq = HF[None, :] * f_ref
        # Die Formanten als Kaskade von Resonatoren zweiter Ordnung (Klatt 1980), jeder mit Gleichanteil 1:
        # ueber einem Formanten faellt das Spektrum mit 12 dB je Oktave, und die Pegel der Formanten
        # zueinander ergeben sich aus Lage und Breite. Die Tabellenpegel sind fuer parallele Synthese und
        # heben hier nur wenig nach. Eine Summe von Lorentz-Glocken hat Auslaeufer mit 6 dB je Oktave, die
        # sich zu einem hellen Teppich addieren -- so gemessen: Alt und Tenor mit Schwerpunkt ueber 10.
        env = np.ones((FRAMES, NFULL))
        for i in range(5):
            F, B = freqs[:, i:i + 1], chorus * widths[:, i:i + 1]
            env *= F * F / np.sqrt((F * F - freq * freq) ** 2 + (freq * B) ** 2)
            env *= 10.0 ** (0.25 * levels[:, i:i + 1] / 20.0)
        env += env.max(axis=1, keepdims=True) * 10.0 ** (-70.0 / 20.0)
        if register != "soprano":                          # der Saengerformant um 3 kHz, der einen Chor traegt
            env *= bumps(f_ref, ((rng.uniform(2600.0, 3200.0), rng.uniform(4.0, 10.0), 0.35),))
        spec = env * ftilt(hg.curve(rng, 0.4, 1.0)) * fwalk(rng, 1.2)
        return spec, dict(register=register, f_ref_hz=round(f_ref, 1), midi_ref=round(midi, 1), note_range=[lo, hi],
                          vowels=seq, chorus=round(chorus, 2))
    return fam


# ---------------------------------------------------------------------------- bowed / tube

BODIES = {  # Tonumfang (Hz) und Koerpermoden (Hz, dB, Breite Oktaven)
    "violin": ((196.0, 660.0), ((275, 5, 0.3), (460, 4, 0.25), (550, 4, 0.25), (1000, 2, 0.5), (2700, 6, 0.6))),
    "viola": ((130.0, 440.0), ((230, 5, 0.3), (380, 4, 0.3), (480, 3, 0.3), (900, 2, 0.5), (2300, 5, 0.6))),
    "cello": ((65.0, 262.0), ((100, 6, 0.35), (175, 4, 0.3), (220, 4, 0.3), (600, 2, 0.5), (1800, 4, 0.6))),
    "gamba": ((73.0, 294.0), ((140, 5, 0.35), (280, 4, 0.35), (420, 3, 0.35), (900, 3, 0.5), (1600, 4, 0.6))),
}


def fam_bowed(rng):
    body = str(rng.choice(list(BODIES)))
    (lo, hi), modes = BODIES[body]
    f_ref = float(np.exp(rng.uniform(math.log(lo), math.log(hi / 1.5))))
    beta = hg.curve(rng, 0.05, 0.2)                        # Strichstelle: sul tasto <-> sul ponticello
    notch = np.abs(np.sin(np.pi * HF[None, :] * beta[:, None])) * 0.95 + 0.05
    pressure = hg.curve(rng, 0.0, 1.0)                     # mehr Bogendruck: flacheres Spektrum
    spec = (ftilt(1.25 - 0.35 * pressure) * notch * bumps(f_ref, modes)
            * flowpass(hg.curve(rng, 20.0, 60.0), 2.0) * fwalk(rng, 1.0))
    return spec, dict(body=body, f_ref_hz=round(f_ref, 1))


def fam_tube(rng):
    kind = str(rng.choice(["clarinet", "flute", "brass", "didgeridoo"]))
    if kind == "clarinet":
        f_ref = float(rng.uniform(147.0, 523.0))
        fc = hg.curve(rng, 1100.0, 2200.0)                 # Grenzfrequenz des Tonlochgitters, wandernd
        freq = HF[None, :] * f_ref
        even = (HF % 2 == 0)[None, :]
        g = np.where(even & (freq < fc[:, None]), 10.0 ** (rng.uniform(-28.0, -16.0) / 20.0), 1.0)
        spec = ftilt(hg.curve(rng, 0.5, 0.9)) * g / np.sqrt(1.0 + (freq / fc[:, None]) ** 4)
    elif kind == "flute":
        f_ref = float(rng.uniform(262.0, 880.0))
        second = np.where(HF == 2, rng.uniform(0.3, 0.9), 0.0)[None, :]
        spec = ftilt(hg.curve(rng, 1.8, 2.8)) * (1.0 + second) * fwalk(rng, 2.0)
    elif kind == "brass":
        f_ref = float(rng.uniform(98.0, 392.0))
        loud = hg.curve(rng, 0.15, 1.0, keys=4)            # an- und abschwellend: hoehere Teiltoene wachsen schneller
        beta = float(rng.uniform(0.5, 1.2))
        spec = (ftilt(np.full(FRAMES, rng.uniform(0.6, 1.0))) * loud[:, None] ** (beta * (HF[None, :] - 1.0) / 4.0)
                * bumps(f_ref, ((rng.uniform(900.0, 1500.0), 6.0, 0.5),)))
    else:
        f_ref = float(rng.uniform(55.0, 98.0))
        spec = (ftilt(np.full(FRAMES, rng.uniform(1.0, 1.5)))
                * bumps(f_ref, ((hg.curve(rng, 700.0, 1600.0, keys=5), rng.uniform(10.0, 18.0), 0.3),
                                (hg.curve(rng, 1800.0, 3200.0, keys=5), rng.uniform(6.0, 12.0), 0.3))))
    return spec, dict(kind=kind, f_ref_hz=round(f_ref, 1))


FAMILIES = {"consonant": fam_consonant, "overtone": fam_overtone, "bowed": fam_bowed, "tube": fam_tube}
for _reg in REGISTER_MIDI:
    FAMILIES["vowel_" + _reg] = vowel_family(_reg)

LIMITS = {
    "consonant":     dict(f1_min=0.10, centroid=(1.2, 10.0), active_min=4, above_max=0.30),
    "overtone":      dict(f1_min=0.08, centroid=(1.5, 14.0), active_min=4, above_max=0.30),
    # Hohe Lagen haben von Natur aus wenige hoerbare Teiltoene: ein Sopran-u auf 400 Hz sind drei oder vier.
    "vowel_bass":    dict(f1_min=0.05, centroid=(1.3, 16.0), active_min=5, above_max=0.45),
    "vowel_tenor":   dict(f1_min=0.05, centroid=(1.1, 14.0), active_min=4, above_max=0.40),
    "vowel_alto":    dict(f1_min=0.05, centroid=(1.0, 10.0), active_min=3, above_max=0.30),
    "vowel_soprano": dict(f1_min=0.05, centroid=(1.0, 8.0), active_min=2, above_max=0.25),
    "bowed":         dict(f1_min=0.06, centroid=(1.5, 14.0), active_min=8, above_max=0.45),
    "tube":          dict(f1_min=0.08, centroid=(1.1, 12.0), active_min=3, above_max=0.40),
    "sampled":       dict(f1_min=0.05, centroid=(1.0, 16.0), active_min=4, above_max=0.45),
    "otmorph":       dict(f1_min=0.08, centroid=(1.2, 14.0), active_min=4, above_max=0.40),
}
TARGETS = {"consonant": 150, "overtone": 110, "vowel_bass": 40, "vowel_tenor": 40, "vowel_alto": 40, "vowel_soprano": 40,
           "bowed": 90, "tube": 90, "sampled": 250, "otmorph": 150}


# ---------------------------------------------------------------------------- sampled: messen, nicht ausschneiden

SAMPLED_CATEGORIES = ("choir", "chant", "solo-voice", "overtone-voice", "overtone-horn", "bowed-low-string", "long-string",
                      "drone-string", "string-ensemble", "bowed-folk", "reed-organ", "pipe-organ", "flute", "blown-vessel",
                      "brass", "pure-tone", "analog-pad", "modular-drone", "drone-ensemble", "feedback-drone", "sub-drone",
                      "singing-wire", "tape-keyboard", "electrical-hum", "pipe-tank")
NOTE_PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}


def note_hz(token):
    """"A#1" -> Hz, wie baseHzFromName in der Engine (A4 = 440 Hz); None ohne Notennamen."""
    if len(token) < 2 or token[0] not in NOTE_PC:
        return None
    pc, i = NOTE_PC[token[0]], 1
    if token[i] == "#":
        pc, i = pc + 1, i + 1
    elif token[i] == "b":
        pc, i = pc - 1, i + 1
    try:
        octave = int(token[i:])
    except ValueError:
        return None
    return 440.0 * 2.0 ** (((octave + 1) * 12 + pc - 69) / 12.0)


def sampled_pool(seed):
    rows = []
    with open(LEDGER, encoding="utf-8") as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if not r.get("accepted") or r.get("harmony") != "single note" or r.get("category") not in SAMPLED_CATEGORIES:
                continue
            name = str(r.get("file", "")).replace("tape-loop", "tapeloop")
            path = os.path.join(TEXTURES, name)
            if not name or not os.path.exists(path):
                continue
            token = os.path.splitext(name)[0].rsplit("_", 1)[-1]
            hz = note_hz(token)
            if hz is None or not 65.0 <= hz <= 700.0:        # C2..F5: genug Teiltoene, Fenster nicht zu lang
                continue
            rows.append((str(r["id"]), path, str(r["category"]), token, hz))
    rows.sort()
    order = np.random.default_rng(seed).permutation(len(rows))
    return [rows[i] for i in order]


def analyse_clip(path, hz):
    """Teiltoene eines gehaltenen Tons: f0 je Fenster um die benannte Note verfolgt, Betraege an k*f0 gemessen.
    Rueckgabe (Spektren Frames x 128 oder None, Info oder Grund)."""
    import soundfile as sf
    info = sf.info(path)
    sr = info.samplerate
    start = int(info.frames * 0.08)
    stop = min(int(info.frames * 0.92), start + 75 * sr)
    x, _ = sf.read(path, start=start, stop=stop, dtype="float32", always_2d=True)
    mono = x.mean(axis=1).astype(np.float64)
    n = 1 << int(math.ceil(math.log2(max(8192.0, 10.0 * sr / hz))))   # wenigstens zehn Perioden je Fenster
    n = min(n, 65536)
    if len(mono) < 6 * n:
        return None, "zu kurz"
    hop = max(n // 2, (len(mono) - n) // 160)
    win = np.hanning(n)
    scale = 2.0 / win.sum()
    df = sr / n
    kmax = int(min(NFULL, (0.45 * sr) / (hz * 1.04), 16000.0 / hz))
    kk = np.arange(1, kmax + 1, dtype=np.float64)
    coarse = hz * 2.0 ** (np.arange(-50.0, 50.1, 2.0) / 1200.0)
    k8 = np.arange(1, min(8, kmax) + 1, dtype=np.float64)
    offs = np.arange(-2, 3)
    amps, harm, f0s = [], [], []
    for pos in range(0, len(mono) - n, hop):
        mag = np.abs(np.fft.rfft(mono[pos:pos + n] * win)) * scale
        grid = np.arange(len(mag))
        score = (np.interp(coarse[:, None] * k8[None, :] / df, grid, mag) / np.sqrt(k8)).sum(axis=1)
        c0 = coarse[int(np.argmax(score))]
        fine = c0 * 2.0 ** (np.arange(-2.0, 2.01, 0.2) / 1200.0)
        k24 = kk[:min(24, kmax)]
        score = (np.interp(fine[:, None] * k24[None, :] / df, grid, mag) / np.sqrt(k24)).sum(axis=1)
        f0 = fine[int(np.argmax(score))]
        centre = np.rint(kk * f0 / df).astype(int)
        idx = np.clip(centre[:, None] + offs[None, :], 1, len(mag) - 2)
        local = mag[idx]
        j = idx[np.arange(kmax), np.argmax(local, axis=1)]
        a, b, c = np.log(mag[j - 1] + 1e-12), np.log(mag[j] + 1e-12), np.log(mag[j + 1] + 1e-12)
        den = a - 2.0 * b + c
        shift = np.where(np.abs(den) > 1e-12, 0.5 * (a - c) / np.where(np.abs(den) > 1e-12, den, 1.0), 0.0)
        peak = np.exp(b - 0.25 * (a - c) * np.clip(shift, -1.0, 1.0))
        lo_bin, hi_bin = int(0.5 * f0 / df), min(len(mag) - 1, int((kmax + 0.5) * f0 / df))
        band = float((mag[lo_bin:hi_bin] ** 2).sum())
        near = float((local ** 2).sum())
        amps.append(peak)
        harm.append(near / max(band, 1e-18))
        f0s.append(f0)
    amps, harm, f0s = np.array(amps), np.array(harm), np.array(f0s)
    cents = 1200.0 * np.log2(f0s / hz)
    keep = (harm >= 0.5) & (np.abs(cents) <= 45.0)
    if keep.sum() < 24:
        return None, "zu wenig tonale Fenster"
    if float(np.median(harm[keep])) < 0.62:
        return None, "nicht harmonisch genug"
    db = 20.0 * np.log10(np.maximum(amps[keep], 1e-9))
    db -= db.max(axis=1, keepdims=True)
    db = np.maximum(db, -70.0)
    t = np.arange(len(db), dtype=np.float64)
    if len(db) >= 5:                                        # Medianfilter ueber die Zeit: einzelne Ausreisser weg
        pad = np.vstack([db[:1], db[:1], db, db[-1:], db[-1:]])
        db = np.median(np.stack([pad[i:i + len(db)] for i in range(5)]), axis=0)
    x = np.linspace(0.0, len(db) - 1.0, FRAMES)
    res = np.stack([np.interp(x, t, db[:, h]) for h in range(kmax)], axis=1)
    kern = np.exp(-0.5 * (np.arange(-4, 5) / 1.5) ** 2)
    kern /= kern.sum()
    padded = np.vstack([res[:1].repeat(4, axis=0), res, res[-1:].repeat(4, axis=0)])
    res = np.stack([np.convolve(padded[:, h], kern, mode="valid") for h in range(kmax)], axis=1)
    spec = np.full((FRAMES, NFULL), 10.0 ** (-70.0 / 20.0) * 1e-3)
    spec[:, :kmax] = 10.0 ** (res / 20.0)
    return spec, dict(frames_kept=int(keep.sum()), window=n, f0_median_hz=round(float(np.median(f0s[keep])), 3),
                      f0_spread_cents=round(float(np.std(cents[keep])), 2), harmonicity=round(float(np.median(harm[keep])), 3),
                      partials_measured=kmax)


# ---------------------------------------------------------------------------- otmorph

EDGES = np.log2(np.arange(0.5, NFULL + 1.0, 1.0))           # Zelle k im Log-Raum: [log2(k-1/2), log2(k+1/2)]
QUANTILES = (np.arange(4096) + 0.5) / 4096.0


def quantiles(amp):
    p = np.maximum(np.asarray(amp, dtype=np.float64) ** 2, 0.0) + 1e-12
    cdf = np.concatenate([[0.0], np.cumsum(p / p.sum())])
    return np.interp(QUANTILES, cdf, EDGES)


def ot_frames(keys):
    """Verschiebungs-Interpolation (McCann 1997; Henderson und Solomon 2019, Audio Transport): zwischen den
    Schluesselspektren werden die Quantilfunktionen der Leistung interpoliert, die Masse gleitet im
    Log-Frequenzraum zwischen den Teiltoenen."""
    q = np.stack([quantiles(k) for k in keys])
    pos = np.linspace(0.0, 1.0, FRAMES) * (len(keys) - 1)
    i = np.minimum(pos.astype(int), len(keys) - 2)
    w = 0.5 - 0.5 * np.cos(np.pi * (pos - i))
    out = np.empty((FRAMES, NFULL))
    for f in range(FRAMES):
        qt = q[i[f]] * (1.0 - w[f]) + q[i[f] + 1] * w[f]
        hist, _ = np.histogram(qt, bins=EDGES)
        out[f] = np.sqrt(hist / len(QUANTILES))
    return out


# ---------------------------------------------------------------------------- Leitplanken

def lift(s, f1_min):
    """Den ersten Teilton so weit anheben, dass er f1_min der Energie traegt, die der Harmonic-Typ hoert (1..32)."""
    s = unit(s)
    if f1_min <= 0.0:
        return s, 0
    a1 = s[:, 0] ** 2
    rest = (s[:, 1:VIEW] ** 2).sum(axis=1)
    need = a1 / np.maximum(a1 + rest, 1e-18) < f1_min
    if need.any():
        s[need, 0] = np.sqrt(f1_min * rest[need] / (1.0 - f1_min))
        s = unit(s)
    return s, int(need.sum())


def smooth(s, f1_min, passes=60):
    n = 0
    while n < passes and max(float(hg.steps(s).max()), float(hg.steps(view32(s)).max())) > STEP_MAX:
        p = np.vstack([s[:1], s, s[-1:]])
        s = unit(0.25 * p[:-2] + 0.5 * p[1:-1] + 0.25 * p[2:])
        s, _ = lift(s, f1_min)
        n += 1
    return s, n


def check(s, lim):
    m = hg.metrics(view32(s))
    why = hg.check(m, lim)
    if why:
        return why, m
    if float(hg.steps(s).max()) > STEP_MAX + 1e-6:
        return "Sprung (128 Teiltoene)", m
    if float((s[:, VIEW:] ** 2).sum(axis=1).max()) > lim["above_max"]:
        return "zu viel ueber Teilton 32", m
    return None, m


def finish(family, spec, params, seed):
    lim = LIMITS[family]
    s, lifted = lift(spec, lim["f1_min"])
    s, passes = smooth(s, lim["f1_min"])
    why, m = check(s, lim)
    if why:
        return dict(family=family, seed=seed, why=why)
    return dict(family=family, seed=seed, S=s.astype(np.float32), params=params, metrics=m, desc=descriptors(s),
                fixes=dict(lifted_frames=lifted, smoothing_passes=passes))


def build_rng(job):
    family, seed = job
    spec, params = FAMILIES[family](np.random.default_rng(seed))
    return finish(family, spec, params, seed)


def build_sampled(job):
    row, seed = job
    ident, path, category, token, hz = row
    try:
        spec, info = analyse_clip(path, hz)
    except Exception as e:                                  # eine kaputte Datei ist ein verworfener Kandidat
        return dict(family="sampled", seed=seed, why=f"lesen: {type(e).__name__}")
    if spec is None:
        return dict(family="sampled", seed=seed, why=info)
    params = dict(source=os.path.basename(path), category=category, note=token, note_hz=round(hz, 3), **info)
    return finish("sampled", spec, params, seed)


def build_otmorph(job):
    keys, meta, seed = job
    return finish("otmorph", ot_frames(keys), meta, seed)


# ---------------------------------------------------------------------------- Rendern

_BASIS = None


def basis():
    global _BASIS
    if _BASIS is None:
        n = np.arange(FRAME_LEN)
        _BASIS = np.sin(2.0 * np.pi * np.outer(HF, n) / FRAME_LEN + PHASES[:, None])
    return _BASIS


def render(s):
    frames = np.asarray(s, dtype=np.float64) @ basis()
    peak = float(np.abs(frames).max())
    return (frames * (10.0 ** (-1.0 / 20.0) / max(peak, 1e-12))).astype(np.float32)


def roundtrip(frames, s):
    s = unit(s)
    err32 = float(np.abs(hg.analyse(frames) - view32(s)).max())
    full = unit(np.abs(np.fft.rfft(np.asarray(frames, dtype=np.float64), axis=1))[:, 1:NFULL + 1])
    return err32, float(np.abs(full - s).max())


# ---------------------------------------------------------------------------- Auswahl

def ranks(values):
    v = np.asarray(values, dtype=np.float64)
    if len(v) < 2:
        return np.zeros(len(v))
    return np.argsort(np.argsort(v, kind="stable"), kind="stable") / (len(v) - 1.0)


def select(cands, want, bank):
    """MAP-Elites: Zellen aus Schwerpunkt, Rauigkeit und Bewegung (je drei Stufen), reihum die besten je Zelle,
    jede gegen alles schon Angenommene (und die Bibliothek) auf Doppel geprueft."""
    if not cands or want <= 0:
        return [], bank, 0
    r = ranks([c["desc"]["rough"] for c in cands])
    mv = ranks([c["desc"]["move"] for c in cands])
    ri = ranks([c["desc"]["rich"] for c in cands])
    ce = ranks([c["desc"]["centroid_hz"] for c in cands])
    cells = collections.defaultdict(list)
    for i, c in enumerate(cands):
        c["fitness"] = float(0.45 * (1.0 - r[i]) + 0.30 * mv[i] + 0.15 * ri[i] + 0.10 * (1.0 - 2.0 * abs(ce[i] - 0.5)))
        c["cell"] = [min(int(ce[i] * 3), 2), min(int(r[i] * 3), 2), min(int(mv[i] * 3), 2)]
        cells[tuple(c["cell"])].append(c)
    for key in cells:
        cells[key].sort(key=lambda c: -c["fitness"])
    chosen, dups = [], 0
    while len(chosen) < want and any(cells.values()):
        for key in sorted(cells, key=lambda k: -(cells[k][0]["fitness"] if cells[k] else -1.0)):
            while cells[key]:
                c = cells[key].pop(0)
                if len(bank) and float(hg.distance(c["sig"], bank).min()) < hg.DIST_MIN_DB:
                    dups += 1
                    continue
                chosen.append(c)
                bank = np.concatenate([bank, c["sig"][None]])
                break
            if len(chosen) >= want:
                break
    return chosen, bank, dups


# ---------------------------------------------------------------------------- generate

def library_bank(folders):
    bank = np.empty((0, len(hg.SIG_FRAMES), VIEW))
    for folder in folders:
        # samt den Regalen darunter (Library/Wavetables/Harmonic, Classic, Ambient)
        for path in sorted(os.path.join(d, f) for d, _, names in os.walk(folder) for f in names):
            if path.lower().endswith(".wav"):
                fr = hg.read_table(path)
                if fr is not None:
                    bank = np.concatenate([bank, hg.signature(hg.at_positions(hg.analyse(fr)))[None]])
    return bank


def run_jobs(fn, jobs, workers):
    if workers <= 1:
        return [fn(j) for j in jobs]
    with cf.ProcessPoolExecutor(max_workers=workers) as ex:
        return list(ex.map(fn, jobs, chunksize=4))


def generate(out, count, seed, families, avoid, workers):
    os.makedirs(out, exist_ok=True)
    scale = count / float(sum(TARGETS.values()))
    want = {f: max(1, int(round(TARGETS[f] * scale))) for f in families}
    master = np.random.default_rng(seed)
    bank = library_bank(avoid)
    print(f"  {len(bank)} vorhandene Tabellen als Vergleich; Ziel {sum(want.values())} Tabellen", flush=True)
    accepted, surplus = {}, []
    report = {}

    def take(fam, results):
        nonlocal bank
        good = [r for r in results if "S" in r]
        why = collections.Counter(r["why"] for r in results if "S" not in r)
        for c in good:
            c["S"] = c["S"].astype(np.float64)
            c["sig"] = hg.signature(view32(c["S"]))
        chosen, bank, dups = select(good, want[fam], bank)
        ids = {id(c) for c in chosen}
        surplus.extend(c for c in good if id(c) not in ids)
        accepted[fam] = chosen
        report[fam] = dict(built=len(results), passed=len(good), chosen=len(chosen), duplicates=dups, rejected=dict(why))
        print(f"  {fam:14s} {len(chosen):4d} von {want[fam]:4d}   Kandidaten {len(results):5d}, Leitplanken bestanden "
              f"{len(good):5d}, Doppel {dups:4d}  " + ", ".join(f"{k} {v}" for k, v in why.most_common(4)), flush=True)

    for fam in [f for f in families if f in FAMILIES]:
        seeds = master.integers(0, 2 ** 31 - 1, size=want[fam] * OVERSAMPLE)
        take(fam, run_jobs(build_rng, [(fam, int(s)) for s in seeds], workers))
    if "sampled" in families:
        pool = sampled_pool(int(master.integers(0, 2 ** 31 - 1)))
        need = min(len(pool), want["sampled"] * OVERSAMPLE)
        print(f"  sampled: {len(pool)} passende Einzeltoene in der Bibliothek, {need} werden gemessen", flush=True)
        seeds = master.integers(0, 2 ** 31 - 1, size=need)
        take("sampled", run_jobs(build_sampled, [(pool[i], int(seeds[i])) for i in range(need)], workers))
    if "otmorph" in families:
        pool = [c for fam in accepted for c in accepted[fam]] + surplus
        if len(pool) >= 3:
            rng = np.random.default_rng(int(master.integers(0, 2 ** 31 - 1)))
            jobs = []
            for _ in range(want["otmorph"] * OVERSAMPLE):
                picks = []
                while len(picks) < int(rng.integers(2, 4)):
                    c = pool[int(rng.integers(0, len(pool)))]
                    if all(c["family"] != p["family"] for p, _ in picks):
                        picks.append((c, int(rng.choice([0, 21, 42, 63]))))
                keys = [c["S"][fr] for c, fr in picks]
                meta = dict(keys=[dict(family=c["family"], seed=c["seed"], frame=fr) for c, fr in picks])
                jobs.append((keys, meta, int(rng.integers(0, 2 ** 31 - 1))))
            take("otmorph", run_jobs(build_otmorph, jobs, workers))

    written = 0
    for fam, chosen in accepted.items():
        index = 0
        for c in chosen:
            frames = render(c["S"])
            e32, efull = roundtrip(frames, c["S"])
            if e32 > 1e-4 or efull > 1e-4:
                raise RuntimeError(f"Rundlauf verletzt: {fam} Seed {c['seed']}: {e32:.2e} / {efull:.2e}")
            while os.path.exists(os.path.join(out, f"{PREFIX}_{fam}_{index:03d}.wav")):
                index += 1
            name = f"{PREFIX}_{fam}_{index:03d}.wav"
            index += 1
            rnd = lambda d: {k: (round(v, 5) if isinstance(v, float) else v) for k, v in d.items()}
            hg.write_table(os.path.join(out, name), frames, dict(
                generator="ambientgen", version=VERSION, family=fam, seed=int(c["seed"]), frames=FRAMES,
                frame_len=FRAME_LEN, partials=NFULL, grounded=True, types=["Harmonic", "Wavetable"],
                recipe=c["params"], fixes=c["fixes"], metrics=rnd(c["metrics"]), descriptors=rnd(c["desc"]),
                fitness=round(c["fitness"], 4), cell=c["cell"],
                full=dict(step_max=round(float(hg.steps(c["S"]).max()), 4),
                          above32_max=round(float((c["S"][:, VIEW:] ** 2).sum(axis=1).max()), 4)),
                roundtrip_max_error=e32, roundtrip_full_max_error=efull))
            written += 1
    with open(os.path.join(out, "ambientgen_report.json"), "w", encoding="utf-8") as f:
        json.dump(dict(version=VERSION, seed=seed, count=count, families=report), f, indent=1)
    print(f"  {written} Tabellen geschrieben nach {out}", flush=True)


# ---------------------------------------------------------------------------- check: spielt die Engine sie so?

def engine_check(folder, sample, note, seconds):
    import engine_check as ec
    folder = os.path.abspath(folder)    # die Renders laufen im Projektordner: ein relativer Pfad griffe dort daneben
    # der Ordner selbst oder ein Regal darunter (Library/Wavetables/Ambient)
    where = {f: os.path.join(d, f) for d, _, names in os.walk(folder) for f in names
             if f.startswith(PREFIX + "_") and f.endswith(".wav")}
    files = sorted(where)
    if not files:
        sys.exit("keine Tabellen in " + folder)
    fams = collections.OrderedDict()
    for f in files:
        fams.setdefault(f[len(PREFIX) + 1:].rsplit("_", 1)[0], []).append(f)
    pick = []
    for fam, names in fams.items():
        step = max(1, len(names) // sample)
        pick += names[::step][:sample]
    import tempfile
    work = tempfile.mkdtemp(prefix="agcheck_")
    f_note = 440.0 * 2 ** ((note - 69) / 12.0)
    bad = 0
    for name in pick:
        path = where[name]
        frames = hg.read_table(path)
        full = unit(np.abs(np.fft.rfft(frames.astype(np.float64), axis=1))[:, 1:NFULL + 1])
        line = []
        for kind, count in (("Harmonic", VIEW), ("Wavetable", 64)):
            ec.NEUTRAL["src1_type"] = kind
            for pos, fr in ((0.0, 0), (1.0, len(frames) - 1)):
                out = os.path.join(work, f"{name}_{kind}_{pos}.wav")
                ec.render(path, pos, note, seconds, out)
                got, _ = ec.harmonics(out, f_note, count=count)
                want = unit(full[fr:fr + 1, :count])[0]
                corr, worst, median, n = ec.compare(want, got)
                ok = corr > 0.98 and worst < 3.0
                bad += not ok
                line.append(f"{kind[:4]} {pos:.0f}: {'ok' if ok else 'FEHL'} r={corr:.3f} max {worst:.2f} dB ({n})")
        print(f"  {name:32s} " + "   ".join(line), flush=True)
    print(f"\n  {'Engine spielt die Tabellen in beiden Typen wie entworfen' if not bad else f'{bad} Abweichung(en)'}")
    return bad


# ---------------------------------------------------------------------------- selftest

def selftest():
    fails = 0

    def ok(label, cond):
        nonlocal fails
        fails += not cond
        print(f"  {'ok  ' if cond else 'FEHL'} {label}")

    rng = np.random.default_rng(5)
    s = unit(rng.uniform(0.0, 1.0, size=(FRAMES, NFULL)) * HF[None, :] ** -1.0)
    e32, efull = roundtrip(render(s), s)
    ok(f"Rundlauf 128 Teiltoene: Harmonic-Sicht {e32:.1e}, volles Spektrum {efull:.1e}", e32 < 1e-5 and efull < 1e-5)

    ok("Rauigkeit: Einklang 0, kleine Sekunde bei 220 Hz rauer als Quinte",
       abs(float(pair_rough(220.0, 220.0))) < 1e-12 and float(pair_rough(220.0, 233.1)) > float(pair_rough(220.0, 330.0)))
    g = rough_matrix(130.8, TUNINGS["equal_major"], 48)
    prior = HF ** -0.7
    a = optimise_consonance(prior, g, 0.05)
    p = prior[:48] / np.linalg.norm(prior[:48])
    ok(f"Optimierung senkt die Rauigkeit im Akkord: {float(p @ g @ p):.4f} -> {float(a @ g @ a):.4f}", float(a @ g @ a) < 0.8 * float(p @ g @ p))

    spec, params = fam_overtone(np.random.default_rng(11))
    peaks = np.argmax(spec[:, 3:], axis=1) + 4
    ok(f"overtone: der lauteste Oberton folgt der Melodie {params['path']}",
       int(peaks[0]) == params["path"][0] and int(peaks[-1]) == params["path"][-1])

    spec, params = vowel_family("bass")(np.random.default_rng(3))
    f_ref = params["f_ref_hz"]
    band = (HF * f_ref > 200) & (HF * f_ref < 1400)
    peak_hz = float(HF[band][np.argmax(spec[0, band])] * f_ref)
    first = FORMANT_SETS["bass"][params["vowels"][0]][0][:2]
    ok(f"vowel: lautester Teilton unter 1,4 kHz ({peak_hz:.0f} Hz) nahe F1/F2 {first} des ersten Vokals",
       min(abs(peak_hz - first[0]), abs(peak_hz - first[1])) < 1.6 * f_ref)

    a = np.zeros(NFULL); a[0] = 1.0
    b = np.zeros(NFULL); b[7] = 1.0
    fr = ot_frames([a, b])
    mids = [float((fr[f] ** 2 * np.log2(HF)).sum() / (fr[f] ** 2).sum()) for f in (0, 32, 63)]
    ok(f"otmorph: die Masse gleitet von Teilton 1 nach 8 (Schwerpunkt log2 {mids[0]:.2f} -> {mids[1]:.2f} -> {mids[2]:.2f})",
       mids[0] < 0.3 and 1.0 < mids[1] < 2.0 and mids[2] > 2.7)

    # Messen statt ausschneiden: ein synthetischer Ton mit Vibrato und bekannten Teiltoenen
    import soundfile as sf
    import tempfile
    sr, dur, f0 = 44100, 12.0, 146.83
    t = np.arange(int(sr * dur)) / sr
    vib = 1.0 + 0.004 * np.sin(2 * np.pi * 5.0 * t)
    phase = 2 * np.pi * np.cumsum(f0 * vib) / sr
    want = np.array([1.0, 0.5, 0.7, 0.25, 0.3, 0.12, 0.1, 0.05])
    sig = sum(w * np.sin((k + 1) * phase) for k, w in enumerate(want)) * 0.2
    tmp = os.path.join(tempfile.mkdtemp(prefix="agself_"), "test-00001_D3.flac")
    sf.write(tmp, np.stack([sig, sig], axis=1), sr, subtype="PCM_24")
    got, info = analyse_clip(tmp, note_hz("D3"))
    if got is None:
        ok(f"sampled: Testton gemessen ({info})", False)
    else:
        g8 = got[32, :8] / got[32, 0]
        err = float(np.abs(20 * np.log10(np.maximum(g8, 1e-9)) - 20 * np.log10(want / want[0])).max())
        ok(f"sampled: acht Teiltoene mit Vibrato auf {err:.2f} dB genau, f0 {info['f0_median_hz']} Hz", err < 1.0)

    for fam in FAMILIES:
        good, whys = 0, collections.Counter()
        for sd in range(12):
            r = build_rng((fam, 2000 + sd))
            good += "S" in r
            if "S" not in r:
                whys[r["why"]] += 1
        ok(f"Familie {fam:14s}: {good}/12 bestehen die Leitplanken" + (f"  ({dict(whys)})" if whys else ""), good >= 6)
    print(f"\n  {'alle bestanden' if not fails else f'{fails} fehlgeschlagen'}")
    return fails


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--out", required=True)
    g.add_argument("--count", type=int, default=1000)
    g.add_argument("--seed", type=int, default=1)
    g.add_argument("--families", default=",".join(TARGETS))
    g.add_argument("--avoid", action="append", default=[])
    g.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 4))
    sub.add_parser("selftest")
    c = sub.add_parser("check")
    c.add_argument("folder")
    c.add_argument("--sample", type=int, default=2)
    c.add_argument("--note", type=int, default=45)
    c.add_argument("--seconds", type=float, default=6.0)
    a = ap.parse_args()
    if a.cmd == "generate":
        fams = [f.strip() for f in a.families.split(",") if f.strip()]
        unknown = [f for f in fams if f not in TARGETS]
        if unknown:
            sys.exit(f"unbekannte Familien: {unknown}")
        generate(a.out, a.count, a.seed, fams, a.avoid, a.jobs)
    elif a.cmd == "selftest":
        sys.exit(1 if selftest() else 0)
    else:
        sys.exit(1 if engine_check(a.folder, a.sample, a.note, a.seconds) else 0)


if __name__ == "__main__":
    main()
