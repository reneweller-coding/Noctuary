"""build_classic.py: die klassische Wavetable-Bibliothek aus freien Quellen (CC0).

Quellen
  AKWF -- Adventure Kid Waveforms, Kristoffer Ekstrand, CC0 1.0: 4358 Einzelzyklen zu 600 Samples in
      65 Ordnern. https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE
  WaveEdit Online -- Baenke von WaveEdit-Nutzern im Format der Synthesis-Technology-Module E352/E370,
      CC0 1.0: 705 Baenke zu 64 Zyklen x 256 Samples. https://github.com/smpldsnds/wavedit-online
  (Die gestapelten AKWF-Tabellen fuer Surge, DUNE 3 und Harmor sind dieselben Ordner in anderer Form,
  auf 512 Samples heruntergerechnet; gebaut wird aus den 600-Sample-Originalen.)

Was daraus wird
  Jede Tabelle ist eine WAV mit Frames zu 2048 Samples -- das Layout von Serum, Vital und Hive --,
  16 Bit, mit dem "clm "-Chunk, aus dem Serum und Vital die Frame-Laenge lesen. So spielt jede Tabelle
  auch dort, und die Engine muss nichts schaetzen.

  Geerdet oder hohl. Die alte Bibliothek klang duenn, weil in 60 % ihrer Tabellen irgendwo der Grundton
  fehlte: gespielt wird dann eine Oktave und mehr ueber der Note, ohne Boden. Hier traegt der Grundton
  in jedem Frame einer geerdeten Tabelle wenigstens 8 % der Energie (dieselbe Grenze, mit der die alte
  Bibliothek gemessen wurde). Ein "Zyklus", der in Wahrheit zwei, drei, vier Perioden haelt, hat seine
  Energie nur auf jeder m-ten Harmonischen und wird zurueck auf die eine Periode gefaltet -- das rettet
  die wenigsten, aber die richtig. Was dann noch ohne Grundton ist, ist so gebaut (Formantzyklen,
  Obertonformen) und kommt in eigene Tabellen unter akwf_hollow/ und wavedit_hollow/: Material fuer
  hohe Schichten, nicht fuer das Fundament.

  AKWF: die Zyklen eines Ordners werden Morph-Tabellen. Ein Zyklus wird exakt ueber seine
  Fourier-Koeffizienten auf 2048 Samples gebracht -- keine Interpolation, kein Filter --, auf gleiche
  Lautheit gesetzt, und Doppel fliegen raus. Mehr als 64 Zyklen werden nach Klang in Gruppen zerlegt.
  Innerhalb einer Tabelle stehen die Zyklen auf einem kurzen Weg durch den Klangraum (naechster Nachbar
  vom dunkelsten aus, dann 2-opt), und jeder ist in Zeit und Polaritaet auf seinen Vorgaenger
  ausgerichtet: zwei aehnliche Wellen, die gegeneinander verschoben stehen, loeschen sich in einer Blende
  halb aus, und genau das hoert man zwischen zwei Frames als Loch.

  WaveEdit: nur Baenke, deren Zyklen wirklich Einzelzyklen sind (die Stetigkeit am Zyklusende, wie in
  detectCycleLength der Engine), die nicht still, nicht halb leer und nicht doppelt sind. Reihenfolge,
  Phasen und Lautheit bleiben, wie der Autor sie gesetzt hat; gefaltet wird nur eine ganze Bank, deren
  klingende Frames alle dieselbe Periodenzahl teilen. Geerdet heisst hier: der mittlere Frame.

Neben jeder Tabelle liegt ein JSON mit Quelle(n), Lizenz, Frames und Messwerten, im Ordner CREDITS.txt.

    python build_classic.py akwf    [--out DIR]
    python build_classic.py wavedit [--out DIR]
    python build_classic.py all     [--out DIR]
    python build_classic.py selftest
"""
import argparse
import collections
import glob
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import warnings

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SOURCES = "G:/Tools/VRAudio/WavetableSources"
AKWF_REPO = SOURCES + "/AKWF-FREE"
WAVEDIT_REPO = SOURCES + "/wavedit-online"
AKWF_URL = "https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE"
WAVEDIT_URL = "https://github.com/smpldsnds/wavedit-online"
DEFAULT_OUT = os.path.join(ROOT, "Library", "WavetablesNew", "classic")
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
LICENSE = "CC0 1.0 Universal"

L = 2048                  # samples per frame in the written tables
H_MAX = 512               # harmonics the engine's CycleTable keeps
SR = 44100
SIG_H = 64                # harmonics compared for order, groups and doubles
FLOOR_DB = -60.0
DUP_DB = 1.0              # two AKWF cycles closer than this are one cycle
DUP_BANK_DB = 2.0         # two WaveEdit banks closer than this are one bank
MAX_GROUP = 64
GROUP_TARGET = 48
MIN_GROUP = 4
CONTINUITY_MAX = 3.0
GROUNDED = 0.08           # the fundamental's least share of a frame's energy in a grounded table
PEAK = 10.0 ** (-1.0 / 20.0)
CLM = b"<!>2048 00000000 wavetable (Noctuary)"


# ---------------------------------------------------------------------------- one cycle

def coefficients(cycle):
    """Complex amplitudes of cosines for harmonics 1 .. min(512, (n-1)/2) of one cycle of any length:
    harmonic h is |c| cos(2 pi h t + arg c). The DC is dropped."""
    x = np.asarray(cycle, dtype=np.float64)
    n = len(x)
    top = min(H_MAX, (n - 1) // 2)
    return np.fft.rfft(x)[1:top + 1] * (2.0 / n)


def synthesise(c, n=L):
    """One cycle of n samples from coefficients (element h-1 is harmonic h)."""
    X = np.zeros(n // 2 + 1, dtype=np.complex128)
    top = min(len(c), n // 2 - 1)
    X[1:top + 1] = np.asarray(c)[:top] * (n / 2.0)
    return np.fft.irfft(X, n)


def rms_of(c):
    return float(np.sqrt(0.5 * np.sum(np.abs(c) ** 2)))


def fundamental_share(c):
    p = np.abs(np.asarray(c)) ** 2
    return float(p[0] / max(float(p.sum()), 1.0e-20)) if len(p) else 0.0


def fold_periods(c, need=0.98):
    """A "cycle" holding m whole periods has its energy on every m-th harmonic only, and is heard m
    times above the note, with nothing at the note itself. Folded, it is the one period it was cut
    from. Returns (coefficients, m)."""
    c = np.asarray(c)
    p = np.abs(c) ** 2
    tot = float(p.sum())
    for m in (8, 6, 5, 4, 3, 2):
        if tot > 0.0 and len(p) >= m and float(p[m - 1::m].sum()) >= need * tot:
            return c[m - 1::m], m
    return c, 1


def signature(c):
    mags = np.zeros(SIG_H)
    m = np.abs(np.asarray(c)[:SIG_H])
    mags[:len(m)] = m
    db = 20.0 * np.log10(np.maximum(mags, 1.0e-12))
    db -= db.max()
    return np.maximum(db, FLOOR_DB)


def dist(a, b):
    """HarmonicGen's distance: dB apart, each harmonic weighted by how far it stands above the floor."""
    w = np.maximum(a, b) - FLOOR_DB
    d = a - b
    return float(np.sqrt((w * d * d).sum() / max(float(w.sum()), 1.0e-12)))


def dist_row(a, B):
    W = np.maximum(B, a[None]) - FLOOR_DB
    D = B - a[None]
    return np.sqrt((W * D * D).sum(axis=1) / np.maximum(W.sum(axis=1), 1.0e-12))


def dist_matrix(S):
    A, B = S[:, None, :], S[None, :, :]
    W = np.maximum(A, B) - FLOOR_DB
    D = A - B
    return np.sqrt((W * D * D).sum(axis=2) / np.maximum(W.sum(axis=2), 1.0e-12))


def centroid(c):
    p = np.abs(c) ** 2
    return float((p * np.arange(1, len(p) + 1)).sum() / max(float(p.sum()), 1.0e-20))


def canonical(c):
    """Turn the cycle in time so its fundamental starts as a sine (rising through zero at sample 0).
    Left alone when there is next to no fundamental to set."""
    c = np.asarray(c)
    if len(c) == 0 or abs(c[0]) < 0.01 * float(np.abs(c).max()):
        return c
    theta = float(np.angle(c[0])) + np.pi / 2.0
    return c * np.exp(-1j * np.arange(1, len(c) + 1) * theta)


def align(prev_wave, c):
    """Shift the cycle in time, and turn it over if that fits better, so that it correlates best with
    the frame before it. Returns (coefficients, shift in samples of 2048, sign)."""
    w = synthesise(c)
    corr = np.fft.irfft(np.fft.rfft(prev_wave) * np.conj(np.fft.rfft(w)), L)   # corr[k] = sum prev[t] w[t-k]
    k = int(np.argmax(np.abs(corr)))
    sign = 1.0 if corr[k] >= 0.0 else -1.0
    h = np.arange(1, len(c) + 1)
    return np.asarray(c) * np.exp(-2j * np.pi * h * k / L) * sign, k, sign


def continuity(x, c=256):
    """Jump from a cycle's last sample to its own first, against the jump on into the next cycle. About
    1 for a bank of single cycles, far above it for frames that only happen to be cut every c samples."""
    k = np.arange(len(x) // c)
    wrap = np.abs(x[k * c + c - 1] - x[k * c]).sum()
    kk = k[:-1]
    onward = np.abs(x[kk * c + c - 1] - x[kk * c + c]).sum() * len(k) / max(len(kk), 1)
    return float(wrap / max(onward, 1.0e-12))


# ---------------------------------------------------------------------------- order and groups

def two_opt(order, D, passes=30):
    order = list(order)
    n = len(order)
    for _ in range(passes):
        improved = False
        for i in range(1, n - 1):
            for j in range(i + 1, n):
                a, b, c = order[i - 1], order[i], order[j]
                d = order[j + 1] if j + 1 < n else None
                before = D[a, b] + (D[c, d] if d is not None else 0.0)
                after = D[a, c] + (D[b, d] if d is not None else 0.0)
                if after < before - 1.0e-9:
                    order[i:j + 1] = order[i:j + 1][::-1]
                    improved = True
        if not improved:
            break
    return order


def chain(D, start):
    """A short open path through all points from `start`: nearest neighbour, then 2-opt."""
    n = len(D)
    order = [start]
    left = set(range(n)) - {start}
    while left:
        last = order[-1]
        nxt = min(left, key=lambda j: (D[last, j], j))
        order.append(nxt)
        left.remove(nxt)
    return two_opt(order, D)


def split_large(idx, S):
    if len(idx) <= MAX_GROUP:
        return [idx]
    from sklearn.cluster import KMeans
    labels = KMeans(n_clusters=2, n_init=10, random_state=0).fit_predict(S[idx])
    a = [idx[i] for i in range(len(idx)) if labels[i] == 0]
    b = [idx[i] for i in range(len(idx)) if labels[i] == 1]
    if not a or not b:
        a, b = idx[:len(idx) // 2], idx[len(idx) // 2:]
    return split_large(a, S) + split_large(b, S)


def groups(S):
    """Indices of S in groups of similar sound, none larger than MAX_GROUP, none smaller than
    MIN_GROUP unless the whole set is."""
    n = len(S)
    if n <= MAX_GROUP:
        return [list(range(n))]
    from sklearn.cluster import KMeans
    k = int(math.ceil(n / GROUP_TARGET))
    labels = KMeans(n_clusters=k, n_init=10, random_state=0).fit_predict(S)
    out = []
    for g in range(k):
        out += split_large([int(i) for i in np.where(labels == g)[0]], S)
    big = [g for g in out if len(g) >= MIN_GROUP]
    small = [g for g in out if len(g) < MIN_GROUP]
    if not big:
        return [list(range(i, min(i + GROUP_TARGET, n))) for i in range(0, n, GROUP_TARGET)]
    for g in small:
        for i in g:
            cents = [S[b].mean(axis=0) for b in big]
            big[int(np.argmin([np.linalg.norm(S[i] - ce) for ce in cents]))].append(i)
    final = []
    for g in big:
        final += split_large(g, S)
    return final


# ---------------------------------------------------------------------------- writing and measuring

def write_table(path, frames, meta):
    data = np.concatenate([np.asarray(f, dtype=np.float64) for f in frames])
    peak = float(np.abs(data).max())
    if peak > 0.0:
        data = data * (PEAK / peak)
    pcm = np.clip(np.round(data * 32767.0), -32768, 32767).astype("<i2").tobytes()
    chunks = (b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SR, SR * 2, 2, 16)
              + b"clm " + struct.pack("<I", len(CLM)) + CLM + (b"\0" if len(CLM) % 2 else b"")
              + b"data" + struct.pack("<I", len(pcm)) + pcm + (b"\0" if len(pcm) % 2 else b""))
    body = b"WAVE" + chunks
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", len(body)) + body)
    os.replace(tmp, path)
    with open(os.path.splitext(path)[0] + ".json", "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=1, ensure_ascii=False)


def read_clm(path):
    with open(path, "rb") as f:
        b = f.read()
    pos = 12
    while pos + 8 <= len(b):
        tag, size = b[pos:pos + 4], struct.unpack("<I", b[pos + 4:pos + 8])[0]
        if tag == b"clm ":
            m = re.match(rb"<!>(\d+)", b[pos + 8:pos + 8 + size])
            return int(m.group(1)) if m else 0
        pos += 8 + size + (size & 1)
    return 0


def table_metrics(coeff_frames):
    rms = np.array([rms_of(c) for c in coeff_frames])
    live = [c for c, r in zip(coeff_frames, rms) if r >= 0.01 * rms.max()] or list(coeff_frames)
    f1 = [fundamental_share(c) for c in live]
    cen = [centroid(c) for c in live]
    sigs = [signature(c) for c in coeff_frames]
    steps = [dist(a, b) for a, b in zip(sigs[:-1], sigs[1:])]
    return dict(frames=len(coeff_frames), f1_min=round(min(f1), 4), f1_median=round(float(np.median(f1)), 4),
                centroid_median=round(float(np.median(cen)), 3),
                step_max_db=round(max(steps), 2) if steps else 0.0, step_median_db=round(float(np.median(steps)), 2) if steps else 0.0,
                level_range_db=round(float(20.0 * np.log10(rms.max() / max(float(rms.min()), 1.0e-9))), 2))


def slug(text):
    s = re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")
    return s or "bank"


def git_rev(repo):
    try:
        return subprocess.run(["git", "-C", repo, "log", "-1", "--format=%h %cs"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


# ---------------------------------------------------------------------------- AKWF

def read_cycle(path):
    x, _ = sf.read(path, dtype="float64", always_2d=True)
    if x.shape[1] == 1:
        return x[:, 0]
    mono = x.mean(axis=1)
    # A stereo cycle whose sides cancel in the sum is kept as its left side.
    chan = float(np.sqrt(np.mean(x ** 2, axis=0)).max())
    return mono if float(np.sqrt(np.mean(mono ** 2))) >= 0.5 * chan else x[:, 0]


def make_tables(out, prefix, cycles, sources, base_meta):
    """Morph tables from equal-loudness cycles: doubles out, groups of like sound, each group ordered along
    a short path and aligned frame to frame. Returns ([(file, metrics, grounded)], doubles)."""
    S_all = np.array([signature(c) for c in cycles])
    keep, doubles = [], 0
    for i in range(len(cycles)):
        if keep and float(dist_row(S_all[i], S_all[keep]).min()) < DUP_DB:
            doubles += 1
            continue
        keep.append(i)
    parts = groups(S_all[keep])
    parts.sort(key=lambda g: float(np.median([centroid(cycles[keep[i]]) for i in g])))
    written = []
    for gi, g in enumerate(parts, start=1):
        idx = [keep[i] for i in g]
        order = chain(dist_matrix(S_all[idx]), int(np.argmin([centroid(cycles[i]) for i in idx])))
        coeff_frames, placed, prev = [], [], None
        for o in order:
            c = cycles[idx[o]]
            if prev is None:
                c, shift, sign = canonical(c), 0, 1.0
            else:
                c, shift, sign = align(prev, c)
            prev = synthesise(c)
            coeff_frames.append(c)
            placed.append(dict(sources[idx[o]], shift=int(shift), sign=int(sign)))
        if len(coeff_frames) == 8:   # 16384 samples is also a WaveEdit bank's size: never write it
            coeff_frames.append(coeff_frames[-1])
            placed.append(dict(placed[-1], repeated=True))
        fname = f"{prefix}_{gi:02d}.wav"
        meta = dict(base_meta, url=AKWF_URL, revision=git_rev(AKWF_REPO), license=LICENSE,
                    author="Kristoffer Ekstrand (Adventure Kid)", frame_length=L, frames=placed,
                    metrics=table_metrics(coeff_frames))
        write_table(os.path.join(out, fname), [synthesise(c) for c in coeff_frames], meta)
        written.append((fname, meta["metrics"], base_meta["grounded"]))
    return written, doubles


def build_akwf(out_dir):
    out, out_hollow = os.path.join(out_dir, "akwf"), os.path.join(out_dir, "akwf_hollow")
    os.makedirs(out, exist_ok=True)
    os.makedirs(out_hollow, exist_ok=True)
    folders = sorted(d for d in glob.glob(AKWF_REPO + "/AKWF/AKWF_*") if os.path.isdir(d))
    report = collections.Counter()
    written, pooled = [], ([], [])
    for folder in folders:
        name = os.path.basename(folder)[len("AKWF_"):]
        files = sorted(glob.glob(folder + "/*.wav"))
        grounded, hollow = ([], []), ([], [])
        for f in files:
            c = coefficients(read_cycle(f))
            if rms_of(c) < 1.0e-4:
                report["still"] += 1
                continue
            c, m = fold_periods(c)
            report["gefaltet"] += m > 1
            c = c / rms_of(c)
            src = dict(file=os.path.basename(f), folder=os.path.basename(folder), periods=m)
            target = grounded if fundamental_share(c) >= GROUNDED else hollow
            target[0].append(c)
            target[1].append(src)
        report["Zyklen"] += len(files)
        line = []
        if grounded[0]:
            w, d = make_tables(out, f"akwf_{slug(name)}", grounded[0], grounded[1],
                               dict(source="AKWF", folder=os.path.basename(folder), grounded=True))
            written += w
            report["doppelt"] += d
            line.append("geerdet " + str([m["frames"] for _, m, _ in w]))
        if len(hollow[0]) >= MIN_GROUP:
            w, d = make_tables(out_hollow, f"akwf_{slug(name)}_hollow", hollow[0], hollow[1],
                               dict(source="AKWF", folder=os.path.basename(folder), grounded=False))
            written += w
            report["doppelt"] += d
            line.append("hohl " + str([m["frames"] for _, m, _ in w]))
        elif hollow[0]:
            pooled[0].extend(hollow[0])
            pooled[1].extend(hollow[1])
            line.append(f"{len(hollow[0])} hohl in den Sammeltopf")
        print(f"  {name:16s} {len(files):3d} Zyklen: " + ", ".join(line), flush=True)
    if pooled[0]:
        w, d = make_tables(out_hollow, "akwf_hollow_mixed", pooled[0], pooled[1],
                           dict(source="AKWF", folder="(several)", grounded=False))
        written += w
        report["doppelt"] += d
        print(f"  Sammeltopf: {len(pooled[0])} hohle Zyklen -> {[m['frames'] for _, m, _ in w]}")
    return written, report


# ---------------------------------------------------------------------------- WaveEdit

def build_wavedit(out_dir):
    out, out_hollow = os.path.join(out_dir, "wavedit"), os.path.join(out_dir, "wavedit_hollow")
    os.makedirs(out, exist_ok=True)
    os.makedirs(out_hollow, exist_ok=True)
    files = sorted({p.lower(): p for p in glob.glob(WAVEDIT_REPO + "/samples/*") if p.lower().endswith(".wav")}.values())
    report = collections.Counter()
    written, bank_sigs, used = [], [], set()
    for f in files:
        x, _ = sf.read(f, dtype="float64", always_2d=True)
        mono = x.mean(axis=1)
        if len(mono) != 64 * 256:
            report["andere Groesse"] += 1
            continue
        if float(np.abs(mono).max()) < 0.01:
            report["still"] += 1
            continue
        r = continuity(mono)
        if r >= CONTINUITY_MAX:
            report["keine Einzelzyklen"] += 1
            continue
        cs = [coefficients(mono[k * 256:(k + 1) * 256]) for k in range(64)]
        rms = np.array([rms_of(c) for c in cs])
        if float((rms < 0.01 * rms.max()).mean()) > 0.25:
            report["halb leer"] += 1
            continue
        live = [k for k in range(64) if rms[k] >= 0.01 * rms.max()]
        g = 0
        for k in live:
            g = math.gcd(g, fold_periods(cs[k])[1])
        if g > 1:
            cs = [np.asarray(c)[g - 1::g] for c in cs]
            report["gefaltet"] += 1
        sig = np.concatenate([signature(cs[k]) for k in (0, 16, 32, 48, 63)])
        if bank_sigs and min(dist(sig, s) for s in bank_sigs) < DUP_BANK_DB:
            report["doppelt"] += 1
            continue
        bank_sigs.append(sig)
        grounded = float(np.median([fundamental_share(cs[k]) for k in live])) >= GROUNDED
        base = "wavedit_" + slug(os.path.splitext(os.path.basename(f))[0])
        fname, n = base + ".wav", 2
        while fname in used:
            fname, n = f"{base}_{n}.wav", n + 1
        used.add(fname)
        meta = dict(source="WaveEdit Online", file=os.path.basename(f), url=WAVEDIT_URL, revision=git_rev(WAVEDIT_REPO),
                    license=LICENSE, author="WaveEdit Online contributors", frame_length=L, source_cycle_length=256,
                    periods_folded=g, grounded=grounded, continuity=round(r, 3), metrics=table_metrics(cs))
        write_table(os.path.join(out if grounded else out_hollow, fname), [synthesise(c) for c in cs], meta)
        written.append((fname, meta["metrics"], grounded))
        report["geerdet" if grounded else "hohl"] += 1
    print(f"  {len(files)} Baenke -> {len(written)} Tabellen; " + ", ".join(f"{k} {v}" for k, v in report.most_common()))
    return written, report


def credits(out_dir):
    text = (
        "Klassische Wavetables fuer Noctuary, gebaut von Tools/WavetableLib/build_classic.py.\n"
        "Jede Tabelle: Frames zu 2048 Samples, 16 Bit, 44,1 kHz, mit clm-Chunk (Serum/Vital-Layout).\n"
        "akwf/ und wavedit/ sind geerdet (der Grundton traegt in jedem Frame, bei WaveEdit im mittleren Frame,\n"
        "wenigstens 8 % der Energie); akwf_hollow/ und wavedit_hollow/ sind ohne Grundton gebaut.\n\n"
        f"AKWF -- Adventure Kid Waveforms, Kristoffer Ekstrand\n  {AKWF_URL} (Stand {git_rev(AKWF_REPO)})\n"
        "  CC0 1.0 Universal. Die 600-Sample-Einzelzyklen der Ordner AKWF_*, zu Morph-Tabellen geordnet und ausgerichtet.\n\n"
        f"WaveEdit Online -- Baenke der WaveEdit-Nutzer (Synthesis Technology E352/E370)\n  {WAVEDIT_URL} (Stand {git_rev(WAVEDIT_REPO)})\n"
        "  CC0 1.0 Universal. Reihenfolge und Phasen wie im Original, Zyklen von 256 auf 2048 Samples gebracht.\n\n"
        "CC0 verlangt keine Namensnennung; sie steht hier trotzdem.\n")
    with open(os.path.join(out_dir, "CREDITS.txt"), "w", encoding="utf-8") as f:
        f.write(text)


def summary(written):
    for label, want in (("geerdet", True), ("hohl", False)):
        rows = [m for _, m, g in written if g == want]
        if not rows:
            continue
        def pct(key):
            v = np.array([m[key] for m in rows], dtype=float)
            return " / ".join(f"{np.percentile(v, q):.2f}" for q in (10, 50, 90))
        print(f"  {label}: {len(rows)} Tabellen (10. / 50. / 90. Perzentil)")
        print(f"    Frames:                          {pct('frames')}")
        print(f"    Grundton-Anteil, schwaechster:   {pct('f1_min')}")
        print(f"    Grundton-Anteil, mittlerer:      {pct('f1_median')}")
        print(f"    Schwerpunkt (Teiltonnummer):     {pct('centroid_median')}")
        print(f"    groesster Schritt (dB):          {pct('step_max_db')}")
        print(f"    Lautheitsspanne (dB):            {pct('level_range_db')}")


# ---------------------------------------------------------------------------- selftest

def selftest():
    fails = 0

    def ok(label, cond):
        nonlocal fails
        fails += not cond
        print(f"  {'ok  ' if cond else 'FEHL'} {label}")

    rng = np.random.default_rng(5)
    c_true = (rng.normal(size=40) + 1j * rng.normal(size=40)) / np.arange(1, 41)
    x600 = synthesise(c_true, 600) + 0.3
    c = coefficients(x600)
    ok("die Koeffizienten einer 600er-Welle kommen zurueck, der Gleichanteil nicht",
       float(np.abs(c[:40] - c_true).max()) < 1.0e-9 and float(np.abs(c[40:]).max()) < 1.0e-9)
    t = np.arange(L) / L
    ref = sum((c_true[h - 1] * np.exp(2j * np.pi * h * t)).real for h in range(1, 41))
    ok("600 -> 2048 Samples exakt, ohne Interpolation", float(np.abs(synthesise(c, L) - ref).max()) < 1.0e-9)

    w = synthesise(c_true, L)
    moved = -np.roll(w, 311)
    aligned, k, sign = align(w, coefficients(moved))
    ok(f"die Ausrichtung findet Versatz und Polaritaet (Versatz {k}, Vorzeichen {sign:+.0f})",
       float(np.abs(synthesise(aligned) - w).max()) < 1.0e-6)

    cc = np.zeros(10, dtype=complex)
    cc[0], cc[2] = 1.0, 0.3 * np.exp(0.4j)
    can = canonical(cc)
    ok("kanonische Phase: der Grundton beginnt als Sinus", abs(float(np.angle(can[0])) + np.pi / 2) < 1.0e-9
       and abs(abs(can[2]) - 0.3) < 1.0e-12)

    two = np.concatenate([synthesise(c_true[:20], 300)] * 2)          # two periods in 600 samples
    folded, m = fold_periods(coefficients(two))
    hollow = np.zeros(12, dtype=complex)
    hollow[1], hollow[2], hollow[4] = 1.0, 0.7, 0.4                    # formant-like: 2, 3, 5 -- no common period
    ok(f"zwei Perioden werden auf eine gefaltet (m = {m}), der Grundton ist wieder da",
       m == 2 and float(np.abs(folded[:20] - c_true[:20]).max()) < 1.0e-9 and fundamental_share(folded) > 0.3)
    ok("ein hohler Zyklus ohne gemeinsame Periode bleibt, wie er ist", fold_periods(hollow)[1] == 1)

    P = rng.permutation(20).astype(float)
    D = np.abs(P[:, None] - P[None, :])
    order = chain(D, int(np.argmin(P)))
    ok("der Weg durch den Klangraum ordnet Punkte auf einer Linie", all(P[order[i]] < P[order[i + 1]] for i in range(19)))

    S = np.vstack([rng.normal(loc=m, scale=1.0, size=(40, 8)) for m in (-20.0, 0.0, 20.0)])
    gs = groups(S)
    ok(f"Gruppen: 120 Punkte in {len(gs)} Gruppen, keine ueber {MAX_GROUP}, jede aus einem Haufen",
       all(len(g) <= MAX_GROUP for g in gs) and sum(len(g) for g in gs) == 120
       and all(len({int(i) // 40 for i in g}) == 1 for g in gs))

    tmp = tempfile.mkdtemp(prefix="wtlib_")
    path = os.path.join(tmp, "t.wav")
    write_table(path, [w, -w, 0.5 * w], {"t": 1})
    data, sr = sf.read(path, dtype="float64")
    ok("der clm-Chunk sagt 2048", read_clm(path) == 2048)
    ok("drei Frames zu 2048 Samples, Spitze bei -1 dBFS", len(data) == 3 * L and abs(float(np.abs(data).max()) - PEAK) < 1.0e-3)

    bank = np.concatenate([synthesise(c_true * (1.0 + 0.02 * k), 256) for k in range(64)])
    frames8 = np.concatenate([synthesise(c_true * np.exp(1j * k), L) for k in range(8)])
    rb, rf = continuity(bank), continuity(frames8)
    ok(f"Stetigkeit: Bank aus Einzelzyklen {rb:.2f}, echte 2048er-Frames {rf:.1f}", rb < CONTINUITY_MAX <= rf)

    if os.path.exists(RENDER):
        path8 = os.path.join(tmp, "eight.wav")
        write_table(path8, [synthesise(c_true * np.exp(1j * k), L) for k in range(8)], {})
        r = subprocess.run([RENDER, "--wavetable", path8, "--seconds", "0.1", "--out", os.path.join(tmp, "o.wav")],
                           capture_output=True, text=True, cwd=ROOT)
        ok("die Engine liest acht 2048er-Frames aus dem Chunk statt zu schaetzen", "8 frames of 2048 samples" in (r.stdout or ""))
    print(f"\n  {'alle bestanden' if not fails else f'{fails} fehlgeschlagen'}")
    return fails


def main():
    warnings.filterwarnings("ignore")
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("akwf", "wavedit", "all"):
        p = sub.add_parser(name)
        p.add_argument("--out", default=DEFAULT_OUT)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        sys.exit(1 if selftest() else 0)
    os.makedirs(a.out, exist_ok=True)
    credits(a.out)
    if a.cmd in ("akwf", "all"):
        print("AKWF:")
        written, report = build_akwf(a.out)
        print("  " + ", ".join(f"{k} {v}" for k, v in report.most_common()))
        summary(written)
    if a.cmd in ("wavedit", "all"):
        print("WaveEdit:")
        written, report = build_wavedit(a.out)
        summary(written)


if __name__ == "__main__":
    main()
