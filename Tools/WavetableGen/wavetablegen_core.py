"""WavetableGen core: wavetables (single-cycle frames, 2048 samples) from audio, from a
text prompt (through TextureGen's models) or from a procedural spectral walk.

A wavetable here is an array of shape (frames, 2048), each row one cycle, peak-normalised,
phase-aligned to its neighbour so morphing between frames does not click. Export writes the
Serum/Vital layout (frames back to back) as a 32-bit float or 16-bit WAV; Noctuary's
User table analyses that back into spectra (Tools/render --wavetable, the plugin's
"Wavetable..." button, wavetable.wav on the Quest).
"""
import json
import math
import os
import re

import numpy as np

FRAME = 2048
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


# ---------------------------------------------------------------- helpers

def note_name(hz):
    midi = 69 + 12 * math.log2(max(hz, 1e-3) / 440.0)
    n = int(round(midi))
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def slugify(text, limit=40):
    s = re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")
    return (s[:limit] or "table").rstrip("_")


def read_wav_mono(path):
    import soundfile as sf
    a, sr = sf.read(path, dtype="float32", always_2d=True)
    return a.mean(axis=1), sr


def write_table_wav(path, table, sr=44100, float32=True):
    """table: (frames, FRAME). Frames back to back, the common wavetable layout."""
    import soundfile as sf
    data = np.asarray(table, dtype=np.float32).reshape(-1)
    sf.write(path, data, sr, subtype="FLOAT" if float32 else "PCM_16")


def spectrum(frame, partials=64):
    """Magnitudes of partials 1..partials of one cycle (normalised to the strongest)."""
    f = np.abs(np.fft.rfft(frame))[1:partials + 1]
    m = f.max()
    return f / m if m > 0 else f


def frame_from_spectrum(mags, phases=None):
    """One cycle from partial magnitudes (index 0 = fundamental); zero phase unless given."""
    n = len(mags)
    spec = np.zeros(FRAME // 2 + 1, dtype=np.complex128)
    ph = np.zeros(n) if phases is None else phases
    spec[1:n + 1] = mags * np.exp(1j * ph)
    x = np.fft.irfft(spec, FRAME)
    m = np.max(np.abs(x))
    return (x / m).astype(np.float32) if m > 0 else x.astype(np.float32)


def normalise(table):
    out = np.asarray(table, dtype=np.float32).copy()
    for i in range(out.shape[0]):
        out[i] -= out[i].mean()
        m = np.max(np.abs(out[i]))
        if m > 0:
            out[i] /= m
    return out


def phase_align(table):
    """Rotate every frame so it correlates best with the previous one (circular shift), and
    start the first frame at its rising zero crossing. Keeps morphs click-free."""
    out = np.asarray(table, dtype=np.float32).copy()
    if out.shape[0] == 0:
        return out
    f0 = out[0]
    zc = np.flatnonzero((f0[:-1] <= 0) & (f0[1:] > 0))
    if len(zc):
        out[0] = np.roll(f0, -int(zc[0]))
    for i in range(1, out.shape[0]):
        a, b = out[i - 1], out[i]
        # circular cross-correlation via FFT
        corr = np.fft.irfft(np.fft.rfft(a) * np.conj(np.fft.rfft(b)), FRAME)
        shift = int(np.argmax(corr))
        out[i] = np.roll(b, shift)
    return out


# ---------------------------------------------------------------- from audio

def track_pitch(mono, sr, lo_hz=40.0, hi_hz=2000.0, frame_s=0.05):
    """Per-frame fundamental by autocorrelation (same rules as TextureGen: skip the lag-0
    lobe, shortest peak >= 0.97 of the best, interior maxima only). Returns arrays
    (times, hz) with hz = nan where nothing periodic was found."""
    frame = int(sr * frame_s)
    hop = frame // 2
    lo, hi = max(2, int(sr / hi_hz)), min(int(sr / lo_hz), frame - 3)
    times, hzs = [], []
    for start in range(0, max(1, len(mono) - frame), hop):
        x = mono[start:start + frame].astype(np.float64)
        x = x - x.mean()
        times.append((start + frame / 2) / sr)
        if float(np.dot(x, x)) < 1e-7:
            hzs.append(np.nan); continue
        ac = np.correlate(x, x, mode="full")[frame - 1:]
        ac = ac / (ac[0] + 1e-12)
        neg = np.flatnonzero(ac[:hi] <= 0.0)
        first = max(lo, int(neg[0])) if len(neg) else lo
        if first >= hi - 1:
            hzs.append(np.nan); continue
        seg = ac[first:hi]
        best = float(seg.max())
        if best <= 0.5:
            hzs.append(np.nan); continue
        k = int(np.flatnonzero(seg >= 0.97 * best)[0]) + first
        while k + 1 < hi and ac[k + 1] > ac[k]:
            k += 1
        if k <= first or k >= hi - 2 or ac[k] < ac[k - 1] or ac[k] < ac[k + 1]:
            hzs.append(np.nan); continue
        # parabolic interpolation of the peak for a sub-sample period
        y0, y1, y2 = ac[k - 1], ac[k], ac[k + 1]
        d = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) != 0 else 0.0
        hzs.append(sr / (k + d))
    return np.array(times), np.array(hzs)


def table_from_audio(mono, sr, frames=32, pitch_hz=None, start=0.0, end=None, cycles_per_frame=4):
    """Slice `frames` single cycles evenly between start and end (seconds). The period comes
    from the pitch track (median, or `pitch_hz` to force it); each frame averages
    `cycles_per_frame` consecutive cycles to tame noise. Returns (table, info)."""
    n = len(mono)
    end = n / sr if end is None else min(end, n / sr)
    if pitch_hz is None:
        t, hz = track_pitch(mono, sr)
        sel = (t >= start) & (t <= end) & np.isfinite(hz)
        if sel.sum() < 3:
            raise ValueError("no stable pitch found in the selection; set the pitch by hand")
        pitch_hz = float(np.median(hz[sel]))
        voiced = float(sel.sum()) / max(1, ((t >= start) & (t <= end)).sum())
    else:
        voiced = 1.0
    period = sr / pitch_hz
    s0, s1 = int(start * sr), int(end * sr) - int(period * (cycles_per_frame + 1)) - 2
    if s1 <= s0:
        raise ValueError("selection too short for the period")
    table = np.zeros((frames, FRAME), dtype=np.float32)
    grid = np.arange(FRAME) / FRAME
    for i in range(frames):
        pos = s0 + (s1 - s0) * (i / max(frames - 1, 1))
        # start each cycle at the nearest rising zero crossing after pos (steadier phase)
        p = int(pos)
        for q in range(p, min(p + int(period), n - 2)):
            if mono[q] <= 0 < mono[q + 1]:
                p = q; break
        acc = np.zeros(FRAME)
        for c in range(cycles_per_frame):
            idx = p + (c + grid) * period
            i0 = np.floor(idx).astype(int)
            fr = idx - i0
            i0 = np.clip(i0, 0, n - 2)
            acc += mono[i0] * (1 - fr) + mono[i0 + 1] * fr
        table[i] = acc / cycles_per_frame
    table = phase_align(normalise(table))
    return table, {"pitch_hz": pitch_hz, "note": note_name(pitch_hz), "voiced": voiced, "frames": frames}


# ---------------------------------------------------------------- procedural

def _scatter(h, salt=0.0):
    """A fixed pseudo-random phase per partial. Deterministic in h rather than drawn from the
    rng, because the rng advances on every frame: a recipe that draws its phases lands a
    different set in each frame, consecutive cycles stop being related, and scanning the table
    becomes a noise burst instead of a movement. Everything here has to be continuous."""
    x = np.sin(h * 12.9898 + salt) * 43758.5453
    return (x - np.floor(x)) * 2.0 * np.pi - np.pi


def _peaks(h, centres, width=0.75, tilt=0.5):
    """Energy at a set of positions that need not be whole numbers. A partial can only sit on an
    integer, so a set computed from a ratio has to be rounded somewhere -- and rounding INSIDE a
    table is a step: as t moves, a partial jumps from 7 to 8 and the frame jumps with it. Placing
    a narrow bump instead lets the energy cross from one partial to the next, which is a slide.
    Everything in this instrument is a rate or an amplitude, never a step."""
    c = np.asarray(centres, dtype=np.float64)
    return (np.exp(-((h[:, None] - c[None, :]) / width) ** 2) * c[None, :] ** -tilt).sum(axis=1)


RECIPES = {
    # name: function(partial index array 1..N, t in 0..1, rng) -> magnitudes, or (magnitudes,
    # phases) for a recipe that moves its partials against each other across the table
    "Saw to square": lambda h, t, r: (1.0 / h) * np.where(h % 2 == 1, 1.0, 1.0 - t),
    "Tilt walk":     lambda h, t, r: h ** -(0.5 + 1.5 * t),
    "Formant sweep": lambda h, t, r: (1.0 / np.sqrt(h)) * (0.05 + np.exp(-((h - (2 + 14 * t)) / 2.5) ** 2) + 0.4 * np.exp(-((h - (8 + 20 * t)) / 4.0) ** 2)),
    "Comb":          lambda h, t, r: (1.0 / np.sqrt(h)) * np.abs(np.cos(h * (0.3 + 1.2 * t))),
    "Glass thinning": lambda h, t, r: np.where(np.isin(h, [1, 3, 7, 12, 19, 27, 36, 47]), 1.0 / h ** (0.3 + 0.7 * t), 0.02 / h),
    "Odd breathing": lambda h, t, r: (1.0 / h) * np.where(h % 2 == 1, 1.0, 0.2 + 0.8 * (0.5 + 0.5 * np.sin(2 * np.pi * t))),
    "Random walk":   None,   # handled below: smooth random spectra

    # --- drone shapes added for the preset library -------------------------------------------
    # Drawbar organ: the classic 1 2 3 4 6 8 12 16 set, upper drawbars pulled out across t.
    "Organ drawbars": lambda h, t, r: np.where(np.isin(h, [1, 2, 3, 4, 6, 8, 12, 16]),
                                               np.where(h <= 4, 1.0, 0.15 + 0.85 * t) / np.sqrt(h), 0.01 / h),
    # Two formants walking apart: a -> i, the vowel most drones sit in.
    "Vowel choir":   lambda h, t, r: (1.0 / np.sqrt(h)) * (0.06
                                     + 0.9 * np.exp(-((h - (3 + 2 * t)) / 1.6) ** 2)
                                     + 0.7 * np.exp(-((h - (9 + 16 * t)) / 3.5) ** 2)
                                     + 0.3 * np.exp(-((h - (22 + 10 * t)) / 5.0) ** 2)),
    # Bell-ish sparse partials: struck metal keeps only a handful of widely spaced ones.
    "Bell partials": lambda h, t, r: np.where(np.isin(h, [1, 2, 5, 9, 14, 20, 27, 35]),
                                              h ** -(0.4 + 0.9 * t), 0.015 / h),
    # Square-law spacing: the stiff bar, gongs and plates.
    "Metal bar":     lambda h, t, r: np.where(np.isin(h, [1, 4, 9, 16, 25, 36]),
                                              h ** -(0.2 + 0.8 * t), 0.02 / h),
    # Primes fading in: an inharmonic-sounding but perfectly periodic shimmer.
    "Prime sieve":   lambda h, t, r: (1.0 / h) * np.where(
                                              np.isin(h, [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47]),
                                              0.1 + 0.9 * t, np.where(h == 1, 1.0, 0.05)),
    # A brick wall in the spectrum opening upwards: a filter sweep baked into the table.
    "Harmonic gate": lambda h, t, r: (1.0 / h ** 0.8) / (1.0 + (h / (1.5 + 44.0 * t ** 2)) ** 6),
    # Reed instruments: 1/h with a resonant bump that walks up the series.
    "Reed":          lambda h, t, r: (1.0 / h) * (1.0 + 2.5 * np.exp(-((h - (4 + 12 * t)) / 3.0) ** 2))
                                     * np.where(h % 2 == 1, 1.0, 0.55),
    # Plucked string: 1/h^2 with the plucking point (a comb notch) moving along the string.
    "Pluck point":   lambda h, t, r: (1.0 / h ** 1.6) * np.abs(np.sin(np.pi * h * (0.08 + 0.34 * t))),
    # Stacked fifths (1 3 9 27) melting into stacked octaves (1 2 4 8 16).
    "Fifth stack":   lambda h, t, r: (np.where(np.isin(h, [1, 3, 9, 27]), 1.0 - t, 0.0)
                                      + np.where(np.isin(h, [1, 2, 4, 8, 16]), t, 0.0) + 0.02) / np.sqrt(h),
    # Two combs multiplied: ring-modulation-like clusters that never repeat over t.
    "Ring cluster":  lambda h, t, r: (1.0 / np.sqrt(h)) * np.abs(np.cos(h * 0.55) * np.cos(h * (0.11 + 0.9 * t))),
    # Breath: everything present, a broad band walking upwards, high partials dominant.
    "Breath band":   lambda h, t, r: (h ** -0.25) * (0.08 + np.exp(-((np.log(h) - np.log(2 + 30 * t)) / 0.55) ** 2)),
    # Sub fold: an almost pure fundamental that grows a second and a third.
    "Sub fold":      lambda h, t, r: np.where(h == 1, 1.0, np.where(h <= 3, 0.05 + 0.55 * t, 0.02 / h)),

    # --- for drones specifically ---------------------------------------------------------------
    # A single cycle is periodic, so its spectrum is harmonic whether we like it or not: a
    # wavetable cannot hold a truly inharmonic partial. What it can hold is a harmonic set chosen
    # so the EAR hears inharmonicity -- the sparse, widely spaced patterns struck metal actually
    # has -- and partials that move against each other as the table is scanned. The second half is
    # what these recipes add: a recipe may return (magnitudes, phases), and a phase that turns with
    # t makes two neighbouring partials beat while the table is swept, which is the shimmer a drone
    # lives on and which no fixed spectrum can give.

    # Singing bowl: the bowl's own sparse set, with two twin partials beside it whose phase turns
    # once over the table -- the beat you hear when a bowl is struck twice.
    "Singing bowl":  lambda h, t, r: (np.where(np.isin(h, [1, 3, 5, 8, 12, 17, 23]), h ** -(0.3 + 0.6 * t), 0.01 / h)
                                      + 0.5 * np.where(np.isin(h, [4, 9, 18]), h ** -0.5, 0.0),
                                      np.where(np.isin(h, [4, 9, 18]), 2.0 * np.pi * t, 0.0)),
    # Gong: square-law bar partials and primes together, the wash filling in as t rises.
    "Gong wash":     lambda h, t, r: (np.where(np.isin(h, [1, 4, 9, 16, 25, 36, 49]), h ** -0.35, 0.0)
                                      + t * np.where(np.isin(h, [2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37, 41, 43]),
                                                     h ** -0.8, 0.0) + 0.008 / h),
    # Bowed string: 1/h^1.2 with the bow's noise sitting as a broad band in the top, rising with
    # pressure, and the even partials a little down the way a bowed string really is.
    "Bowed string":  lambda h, t, r: ((1.0 / h ** 1.2) * np.where(h % 2 == 0, 0.7, 1.0)
                                      + (0.25 + 0.5 * t) * np.exp(-((np.log(h) - np.log(9 + 26 * t)) / 0.7) ** 2) / np.sqrt(h),
                                      _scatter(h, 1.0) * np.clip((h - 6) / 20.0, 0.0, 1.0)),
    # Bowed cymbal: almost nothing at the bottom, a dense cluster high up that climbs.
    "Bowed cymbal":  lambda h, t, r: (np.where(h <= 2, 0.12, 1.0) * (h ** -0.3)
                                      * np.exp(-((np.log(h) - np.log(12 + 30 * t)) / 0.9) ** 2)
                                      + 0.01 / h,
                                      _scatter(h, 2.0)),
    # Prepared piano: the string's own series, and a screw's metallic buzz arriving on the high
    # primes as t rises.
    "Prepared piano": lambda h, t, r: ((1.0 / h ** 1.5)
                                       + t * 0.6 * np.where(np.isin(h, [13, 17, 19, 23, 29, 31, 37, 41, 43, 47]),
                                                            h ** -0.4, 0.0),
                                       np.where(h > 12, np.pi * t, 0.0)),
    # A real piano's partials are stretched by the stiffness of the string (Railsback): the nth
    # sits at n*sqrt(1 + B n^2), rounded to where a periodic cycle can put it.
    "Stretched string": lambda h, t, r: _peaks(
        h, np.arange(1, 17) * np.sqrt(1.0 + (0.0004 + 0.0016 * t) * np.arange(1, 17) ** 2), 0.7, 0.9) + 0.01 / h,
    # Two formants a hair apart, one of them turning in phase: vowel interference rather than a
    # vowel. Two of these detuned against each other is the choir nobody sang.
    "Formant beat":  lambda h, t, r: ((1.0 / np.sqrt(h)) * (0.05
                                      + 0.9 * np.exp(-((h - 4.0) / 1.5) ** 2)
                                      + 0.9 * np.exp(-((h - (4.6 + 0.8 * t)) / 1.5) ** 2)
                                      + 0.5 * np.exp(-((h - (11 + 9 * t)) / 3.0) ** 2)),
                                      np.where(np.abs(h - (4.6 + 0.8 * t)) < 2.0, 2.0 * np.pi * t, 0.0)),
    # PPG / Microwave: the magnitudes quantised to a handful of levels, which is what those tables
    # were -- eight bits of spectrum and no apology for it.
    "PPG digital":   lambda h, t, r: np.round((1.0 / h ** (0.6 + 0.5 * t)) * 7.0) / 7.0 + 0.01,
    # The same idea taken further: three bits, and a hard cluster up top standing in for the
    # aliasing those machines never hid.
    "Lo-fi bits":    lambda h, t, r: (np.round((1.0 / h ** 0.8) * 3.0) / 3.0
                                      + 0.25 * np.where(h > 30, np.abs(np.cos(h * (0.4 + 2.0 * t))), 0.0) + 0.01),
    # A bank of resonators tuned to a sparse set, sharpening as t rises: rings rather than shapes.
    "Resonator bank": lambda h, t, r: sum(np.exp(-((h - c) / (2.2 - 1.7 * t)) ** 2) * c ** -0.5
                                          for c in (1, 3, 7, 13, 22, 34)) + 0.008 / h,
    # Two saws at a phase offset that opens across the table: hollow at one end, solid at the
    # other, and everything between is comb filtering that never sits still.
    "Bi-phase":      lambda h, t, r: ((1.0 / h) * np.abs(np.cos(np.pi * h * t * 0.5)) + 0.01 / h,
                                      np.pi * h * t * 0.5),
    # Pink noise through one resonance, walking slowly. Random phases: this one is a colour, not
    # a waveform.
    "Pink resonance": lambda h, t, r: ((1.0 / h) * (0.15 + np.exp(-((np.log(h) - np.log(2 + 24 * t)) / 0.45) ** 2)),
                                       _scatter(h, 3.0)),
    # Bohlen-Pierce: the tritave divided in thirteen, so nothing lands on an octave and the ear
    # never finds the series it is looking for.
    "Bohlen-Pierce": lambda h, t, r: _peaks(
        h, 3.0 ** (np.arange(0, 14) / 13.0) * (3 + 9 * t), 0.7, 0.7) + 0.01 / h,
    # A sub with one odd partial at a time coming up out of it.
    "Sub bloom":     lambda h, t, r: np.where(h == 1, 1.0,
                                              np.where(h % 2 == 1,
                                                       0.35 * np.exp(-((h - (3 + 20 * t)) / 2.5) ** 2), 0.005 / h)),

    # Octaves under a window that climbs: scanned slowly, the table rises for ever without ever
    # leaving. Shepard's illusion, baked into a wavetable instead of played as one.
    "Shepard stack": lambda h, t, r: np.where(np.isin(h, [1, 2, 4, 8, 16, 32]),
                                              np.exp(-((np.log2(h) - 5.0 * t) / 1.6) ** 2), 0.006 / h),
    # 1 2 3 5 8 13 21 34: the ear keeps almost finding a series and never quite does.
    "Fibonacci":     lambda h, t, r: np.where(np.isin(h, [1, 2, 3, 5, 8, 13, 21, 34]),
                                              h ** -(0.35 + 0.75 * t), 0.008 / h),
    # A cathedral plenum rather than a Hammond: principals, then the mutation ranks -- quint,
    # tierce, larigot, septime -- drawn one after another as t rises.
    "Organ mixture": lambda h, t, r: (np.where(np.isin(h, [1, 2, 4, 8, 16]), 1.0, 0.0)
                                      + np.where(np.isin(h, [3, 6, 12]), np.clip(3 * t, 0, 1), 0.0)
                                      + np.where(np.isin(h, [5, 10]), np.clip(3 * t - 1, 0, 1), 0.0)
                                      + np.where(np.isin(h, [7, 9, 14]), np.clip(3 * t - 2, 0, 1), 0.0)
                                      + 0.006) / np.sqrt(h),
    # A vowel that actually travels: ah -> oh -> ee, both formants on their real path rather than
    # walking apart in a straight line.
    "Three vowels":  lambda h, t, r: (1.0 / np.sqrt(h)) * (0.05
                                     + 0.9 * np.exp(-((h - np.interp(t, [0, 0.5, 1], [7.0, 4.5, 3.0])) / 1.5) ** 2)
                                     + 0.8 * np.exp(-((h - np.interp(t, [0, 0.5, 1], [12.0, 8.0, 24.0])) / 3.0) ** 2)
                                     + 0.3 * np.exp(-((h - np.interp(t, [0, 0.5, 1], [26.0, 22.0, 30.0])) / 5.0) ** 2)),
    # A pipe stopped at one end has odd partials only. It opens as t rises, which is the moment a
    # clarinet turns into a flute.
    "Stopped pipe":  lambda h, t, r: (1.0 / h ** 1.1) * np.where(h % 2 == 1, 1.0, t ** 2)
                                     * (1.0 + 1.5 * np.exp(-((h - 3) / 1.2) ** 2)),
    # Every partial gets a neighbour, and which pair beats walks up the series: the whole table is
    # one slow interference pattern.
    "Beating pairs": lambda h, t, r: ((1.0 / h ** 0.9)
                                      + 0.8 * np.exp(-((h - (2 + 30 * t)) / 1.2) ** 2),
                                      np.where(np.abs(h - (2 + 30 * t)) < 3.0, 2.0 * np.pi * t * 2.0, 0.0)),
    # The bell's sparse set through a comb that shifts: clangour that changes colour rather than
    # decaying, which is what a bell cannot do and a drone can.
    "Ring bell":     lambda h, t, r: np.where(np.isin(h, [1, 2, 5, 9, 14, 20, 27, 35, 44]),
                                              h ** -0.5 * (0.15 + np.abs(np.cos(h * (0.2 + 1.1 * t)))), 0.008 / h),
    # Partials at 2.02^n instead of 2^n -- the stretched octave the instrument tunes with -- so a
    # table and the tuning agree about what an octave is.
    "Stretched octave": lambda h, t, r: _peaks(
        h, 2.02 ** np.arange(0, 6) * (1 + 2 * t), 0.8, 0.55) + 0.008 / h,
    # Rubbed metal: a dense band in the middle, slowly moving, with twin partials turning inside
    # it. A waterphone, or a sheet of steel with a bow on its edge.
    "Waterphone":    lambda h, t, r: ((h ** -0.4) * np.exp(-((np.log(h) - np.log(5 + 14 * t)) / 0.5) ** 2)
                                      + 0.01 / h,
                                      _scatter(h, 4.0) * 0.6 + np.pi * t * (h % 3 == 0)),
    # The opposite of building up: a full series with holes opening in it, one partial at a time.
    "Spectral erosion": lambda h, t, r: (1.0 / h ** 0.85) * np.clip(
        1.0 - np.exp(-((h - (1 + 46 * t)) / 3.0) ** 2) * 1.4, 0.02, 1.0),
}


def table_procedural(recipe, frames=32, partials=48, seed=0, noise=0.0, phase_scatter=0.0):
    """A wavetable from a spectral recipe over t = 0..1, optionally with a random component
    (noise: random per-partial gain walk) and random phases (phase_scatter 0..1: 0 = all
    partials in phase = classic waveform look; 1 = fully random phases = smoother, wider)."""
    rng = np.random.default_rng(seed)
    h = np.arange(1, partials + 1, dtype=np.float64)
    phases = rng.uniform(-np.pi, np.pi, partials) * phase_scatter
    walk = np.ones(partials)
    table = np.zeros((frames, FRAME), dtype=np.float32)
    # smooth random targets for the walk: new target every 8 frames, smoothstep between
    targets = [np.exp(rng.normal(0, 1.0, partials)) for _ in range(frames // 8 + 2)]
    for i in range(frames):
        t = i / max(frames - 1, 1)
        ph = phases
        if recipe == "Random walk":
            base = h ** -(0.7 + 0.6 * t)
        else:
            got = RECIPES[recipe](h, t, rng)
            # A recipe may hand back its own phases as well: partials that turn against each other
            # as the table is scanned, which is how a still spectrum is made to shimmer.
            if isinstance(got, tuple):
                got, own = got
                ph = phases + np.asarray(own, dtype=np.float64)
            base = np.maximum(got, 0.0)
        if noise > 0 or recipe == "Random walk":
            seg = i / 8.0
            a, b = targets[int(seg)], targets[int(seg) + 1]
            u = seg - int(seg); u = u * u * (3 - 2 * u)
            walk = a + (b - a) * u
            amount = max(noise, 1.0 if recipe == "Random walk" else 0.0)
            base = base * (walk ** amount)
        table[i] = frame_from_spectrum(base, ph)
    return phase_align(normalise(table))


def morph_tables(a, b, frames=32):
    """Cross-fade between the first frames of a and b (spectral domain, so it stays clean)."""
    out = np.zeros((frames, FRAME), dtype=np.float32)
    sa, sb = np.fft.rfft(a[0]), np.fft.rfft(b[0])
    for i in range(frames):
        t = i / max(frames - 1, 1)
        x = np.fft.irfft(sa * (1 - t) + sb * t, FRAME)
        out[i] = x
    return phase_align(normalise(out))


# ---------------------------------------------------------------- preview

def render_preview(table, sr=48000, seconds=6.0, hz=110.0, sweep=True):
    """Plays the table at `hz`, sweeping the frame position 0 -> 1 over the clip (or holding
    frame 0), with linear interpolation between frames and a short fade."""
    n = int(seconds * sr)
    frames = table.shape[0]
    phase = (np.arange(n) * hz / sr) % 1.0
    pos = np.linspace(0, 1, n) if sweep and frames > 1 else np.zeros(n)
    fpos = pos * (frames - 1)
    i0 = np.minimum(fpos.astype(int), max(frames - 2, 0))
    fr = fpos - i0
    idx = phase * FRAME
    j0 = idx.astype(int)
    jf = idx - j0
    j1 = (j0 + 1) % FRAME
    def sample(fi):   # per-sample gather: frame index and phase index vary together
        return table[fi, j0] * (1 - jf) + table[fi, j1] * jf
    y = sample(i0) * (1 - fr) + sample(np.minimum(i0 + 1, frames - 1)) * fr
    fade = np.minimum(1.0, np.minimum(np.arange(n), n - np.arange(n)) / (0.02 * sr))
    return (0.4 * y * fade).astype(np.float32)


def save_table(path, table, info=None, float32=True):
    write_table_wav(path, table, float32=float32)
    meta = {"frames": int(table.shape[0]), "frame_len": FRAME}
    if info:
        meta.update({k: (float(v) if isinstance(v, (np.floating, float)) else v) for k, v in info.items()})
    with open(os.path.splitext(path)[0] + ".txt", "w", encoding="utf-8") as f:
        f.write(json.dumps(meta, indent=2))
    return path
