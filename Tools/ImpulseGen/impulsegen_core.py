"""ImpulseGen core: impulse responses for Noctuary's convolution Room.

Two ways to make one, and a way to turn any recording into one:
  procedural(...)   a designed room: per-band decay (RT60 low / mid / high), size (pre-delay and
                    early-reflection spacing), diffusion onset, stereo width, optional modulation
                    shimmer. Decorrelated noise shaped by envelopes - the classic synthetic hall,
                    tuned for dark, long, Rich-style spaces.
  from_audio(...)   an impulse from a recording or an AI render (e.g. Stable Audio Open asked for
                    "a single clap in a huge stone cathedral"): onset detection, trim, fade,
                    optional tail extension when the recording ends before the room does.
  hybrid(...)       the procedural envelopes applied to the spectral colour of an audio file:
                    the room's character from the recording, the decay from the design.
Output: stereo float32 (2, n) at the given rate, energy-normalised (the synth normalises again).
"""
import json
import math
import os
import re

import numpy as np

NOTE = "ImpulseGen"


def slugify(text, limit=48):
    s = re.sub(r"[^A-Za-z0-9]+", "_", text).strip("_")
    return (s[:limit] or "room").rstrip("_")


def read_wav(path):
    import soundfile as sf
    a, sr = sf.read(path, dtype="float32", always_2d=True)
    return a.T, sr   # (channels, n)


def write_wav(path, stereo, sr, float32=True):
    import soundfile as sf
    sf.write(path, np.asarray(stereo, dtype=np.float32).T, sr, subtype="FLOAT" if float32 else "PCM_24")


def normalise(ir):
    e = float(np.sum(ir ** 2)) / ir.shape[0]
    return ir / math.sqrt(e) if e > 1e-12 else ir


# ---------------------------------------------------------------- band tools

def band_split(x, sr, lo=300.0, hi=3000.0):
    """Three bands with simple one-pole crossovers (matches the synth's built-in hall)."""
    def onepole(sig, fc):
        a = 1.0 - math.exp(-2 * math.pi * fc / sr)
        y = np.empty_like(sig); acc = 0.0
        for i in range(len(sig)):
            acc += a * (sig[i] - acc); y[i] = acc
        return y
    try:
        from scipy.signal import butter, sosfilt
        low = sosfilt(butter(2, lo / (sr / 2), "low", output="sos"), x)
        high = sosfilt(butter(2, hi / (sr / 2), "high", output="sos"), x)
        mid = x - low - high
    except ImportError:
        low = onepole(x, lo); lm = onepole(x, hi); mid = lm - low; high = x - lm
    return low, mid, high


def decay_env(n, sr, rt60, onset_s=0.0):
    t = np.arange(n) / sr
    env = 10.0 ** (-3.0 * t / max(rt60, 0.05))
    if onset_s > 0:
        env *= np.minimum(1.0, t / onset_s)
    return env


# ---------------------------------------------------------------- procedural

def procedural(sr=48000, seconds=5.0, rt60_low=5.0, rt60_mid=3.0, rt60_high=1.2, size=1.0, predelay_ms=0.0,
               diffusion_ms=25.0, width=1.0, early_level=0.5, modulation=0.0, tone=0.0, seed=1):
    """A designed hall. size scales the early-reflection pattern (1 = a large hall, 0.3 = a room,
    3 = a cavern); tone tilts the noise colour (-1 dark .. +1 bright); modulation adds a slow
    pitch/phase wobble to the tail (0..1) for a shimmering, less static room."""
    rng = np.random.default_rng(seed)
    n = int(seconds * sr)
    out = np.zeros((2, n), dtype=np.float64)
    t = np.arange(n) / sr
    for c in range(2):
        noise = rng.standard_normal(n)
        if tone != 0.0:   # colour: first-order tilt
            a = 1.0 - math.exp(-2 * math.pi * (600.0 * 2 ** (2 * tone)) / sr)
            lp = np.empty(n); acc = 0.0
            for i in range(n):
                acc += a * (noise[i] - acc); lp[i] = acc
            noise = lp if tone < 0 else noise - 0.7 * lp
        low, mid, high = band_split(noise, sr)
        tail = (low * decay_env(n, sr, rt60_low, diffusion_ms / 1000) * 1.2 +
                mid * decay_env(n, sr, rt60_mid, diffusion_ms / 1000) +
                high * decay_env(n, sr, rt60_high, diffusion_ms / 1000) * 0.7)
        if modulation > 0:   # slow, per-channel phase wobble by resampling the tail along a wandering time axis
            wob = np.cumsum(1.0 + 0.002 * modulation * np.sin(2 * math.pi * (0.3 + 0.2 * c) * t + rng.uniform(0, 6.28)))
            wob = np.clip(wob, 0, n - 1)
            tail = np.interp(wob, np.arange(n), tail)
        # early reflections: a cluster whose spacing grows with size, alternating polarity
        ers = np.zeros(n)
        count = 10
        for k in range(count):
            at = int((0.004 + 0.06 * size * (k / count) ** 1.4 + 0.004 * rng.uniform()) * sr)
            if at < n:
                ers[at] += (1.0 if k % 2 == 0 else -1.0) * early_level * (1.0 - 0.08 * k) * (1.0 if c == 0 else rng.uniform(0.7, 1.0))
        ir = tail + ers
        pre = int(predelay_ms / 1000 * sr)
        if pre > 0:
            ir = np.concatenate([np.zeros(pre), ir])[:n]
        out[c] = ir
    if width < 1.0:   # narrow the stereo image toward the mid
        mid = 0.5 * (out[0] + out[1]); side = 0.5 * (out[0] - out[1]) * width
        out[0], out[1] = mid + side, mid - side
    # gentle fade-out over the last 10 %
    fade = np.ones(n); k = int(0.1 * n); fade[-k:] = np.linspace(1, 0, k) ** 2
    out *= fade
    return normalise(out.astype(np.float32))


# ---------------------------------------------------------------- from audio

def find_onset(mono, sr, threshold_db=-20.0):
    peak = float(np.max(np.abs(mono))) + 1e-12
    thr = peak * 10 ** (threshold_db / 20)
    idx = np.flatnonzero(np.abs(mono) > thr)
    if len(idx) == 0:
        return 0
    start = int(idx[0])
    # back up to the preceding zero crossing / quiet point (max 5 ms)
    back = max(0, start - int(0.005 * sr))
    seg = np.abs(mono[back:start + 1])
    return back + int(np.argmin(seg)) if len(seg) else start


def from_audio(stereo, sr, max_seconds=8.0, floor_db=-60.0, extend=False, rt60_extend=3.0, seed=1):
    """Cut a recording to an impulse: start at the onset, end where the tail falls below
    floor_db (or at max_seconds), fade the end. With extend=True a synthetic tail continues
    the recording's decay when it is cut off early (a short AI render of a long room)."""
    x = np.asarray(stereo, dtype=np.float64)
    if x.ndim == 1:
        x = x[None, :]
    if x.shape[0] == 1:
        x = np.vstack([x, x])
    mono = x.mean(axis=0)
    on = find_onset(mono, sr)
    x = x[:, on:]
    n = min(x.shape[1], int(max_seconds * sr))
    x = x[:, :n]
    # envelope in 20 ms windows to find where the tail hits the floor
    win = max(1, int(0.02 * sr))
    env = np.array([np.sqrt(np.mean(x[:, i:i + win] ** 2)) for i in range(0, n - win + 1, win)])
    peak = env.max() + 1e-12
    below = np.flatnonzero(env < peak * 10 ** (floor_db / 20))
    end = n
    if len(below):
        # first window after the peak that stays below the floor
        pk = int(np.argmax(env))
        after = below[below > pk]
        if len(after):
            end = min(n, int(after[0] + 1) * win)
    x = x[:, :end]
    if extend and end < int(max_seconds * sr):
        # continue the decay: last 100 ms of the recording as colour, faded into designed tail
        rng = np.random.default_rng(seed)
        add = int(max_seconds * sr) - end
        ext = np.zeros((2, add))
        last_rms = float(np.sqrt(np.mean(x[:, -int(0.1 * sr):] ** 2)) + 1e-9)
        for c in range(2):
            noise = rng.standard_normal(add)
            low, mid, high = band_split(noise, sr)
            tail = low * decay_env(add, sr, rt60_extend) + mid * decay_env(add, sr, rt60_extend * 0.7) + high * decay_env(add, sr, rt60_extend * 0.35)
            tail *= last_rms / (np.sqrt(np.mean(tail[:int(0.1 * sr)] ** 2)) + 1e-9)
            ext[c] = tail
        # crossfade 50 ms
        cf = min(int(0.05 * sr), x.shape[1])
        ramp = np.linspace(0, 1, cf)
        x[:, -cf:] *= (1 - ramp)
        ext[:, :cf] *= ramp
        x = np.concatenate([x[:, :-cf], x[:, -cf:] + ext[:, :cf], ext[:, cf:]], axis=1)
    n = x.shape[1]
    k = max(1, int(0.05 * n))
    fade = np.ones(n); fade[-k:] = np.linspace(1, 0, k) ** 2
    fade[:min(64, n)] *= np.linspace(0, 1, min(64, n))
    x *= fade
    return normalise(x.astype(np.float32))


def hybrid(stereo, sr, seconds=5.0, rt60_low=5.0, rt60_mid=3.0, rt60_high=1.2, seed=1):
    """Colour from the recording (its average spectrum), decay from the design: the recording's
    magnitude spectrum shapes noise, then the three-band envelopes apply."""
    x = np.asarray(stereo, dtype=np.float64)
    if x.ndim == 1:
        x = x[None, :]
    mono = x.mean(axis=0)
    n = int(seconds * sr)
    nfft = 4096
    spec = np.zeros(nfft // 2 + 1)
    hop = nfft // 2
    for s in range(0, max(1, len(mono) - nfft), hop):
        spec += np.abs(np.fft.rfft(mono[s:s + nfft] * np.hanning(nfft)))
    spec /= spec.max() + 1e-12
    spec = np.maximum(spec, 0.02)
    rng = np.random.default_rng(seed)
    out = np.zeros((2, n))
    for c in range(2):
        noise = rng.standard_normal(n)
        # colour the noise by filtering with the recording's spectrum (overlap-add of shaped frames)
        col = np.zeros(n + nfft)
        for s in range(0, n, hop):
            frame = noise[s:s + nfft]
            if len(frame) < nfft:
                frame = np.pad(frame, (0, nfft - len(frame)))
            f = np.fft.rfft(frame * np.hanning(nfft)) * spec
            col[s:s + nfft] += np.fft.irfft(f, nfft)
        col = col[:n]
        low, mid, high = band_split(col, sr)
        out[c] = (low * decay_env(n, sr, rt60_low, 0.02) * 1.2 + mid * decay_env(n, sr, rt60_mid, 0.02) + high * decay_env(n, sr, rt60_high, 0.02) * 0.7)
    k = int(0.1 * n); fade = np.ones(n); fade[-k:] = np.linspace(1, 0, k) ** 2
    out *= fade
    return normalise(out.astype(np.float32))


def describe(ir, sr):
    """RT60 estimate per band and a few numbers for the log."""
    mono = ir.mean(axis=0)
    n = len(mono)
    edc = np.cumsum(mono[::-1] ** 2)[::-1]
    edc = 10 * np.log10(edc / (edc[0] + 1e-20) + 1e-20)
    def t_at(db):
        idx = np.flatnonzero(edc <= db)
        return idx[0] / sr if len(idx) else n / sr
    t20 = t_at(-25) - t_at(-5)
    corr = float(np.corrcoef(ir[0], ir[1])[0, 1]) if ir.shape[0] > 1 and ir[0].std() > 0 and ir[1].std() > 0 else 1.0
    return {"seconds": n / sr, "rt60_estimate": 3.0 * t20, "stereo_correlation": corr}


def save(path, ir, sr, meta=None, float32=True):
    write_wav(path, ir, sr, float32)
    m = {"sample_rate": sr}
    m.update(describe(ir, sr))
    if meta:
        m.update(meta)
    with open(os.path.splitext(path)[0] + ".txt", "w", encoding="utf-8") as f:
        f.write(json.dumps(m, indent=2, default=float))
    return path
