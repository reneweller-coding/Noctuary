"""Search the far reverb's eight delay-line lengths for the flattest tail.

A feedback delay network's late response is a sum of modes at the delay lines' own frequencies and
their combinations. Where those pile up, the tail rings at that pitch: the "colour" of a reverb
that should be colourless. The recent literature optimises the feedback matrix and the gains for
this (Dal Santo, Prawda, Schlecht and Valimaki, DAFx 2023); the cheapest version of the same idea,
and the one this instrument can use without changing its structure, is to choose the lengths.

    python Tools/optimise_fdn.py [--iterations 4000] [--seed 7]

The score is the spread of the simulated tail's third-octave magnitudes in decibels: the smaller,
the flatter. The classic set is measured first as the number to beat, and the winning set is
printed in the form the C++ table wants. It is a search, not a proof: the same seed gives the same
answer, and the number in the source is whatever this printed.
"""
import argparse
import numpy as np

SR = 48000
LINES = 8
CLASSIC_MS = [29.7, 37.1, 41.1, 43.7, 53.3, 61.9, 71.3, 79.9]


def simulate(lengths_ms, seconds=2.0, decay=4.0, scattering=True, rng=None):
    """The instrument's network: eight lines, a Householder reflection, per-line decay gains, and
    (in the scattering modes) a short all-pass inside every line. Returns the impulse response."""
    n = int(seconds * SR)
    lens = [max(4, int(round(ms * SR / 1000.0))) for ms in lengths_ms]
    gains = [10.0 ** (-3.0 * L / (decay * SR)) for L in lens]
    bufs = [np.zeros(L + 1) for L in lens]
    pos = [0] * LINES
    # the scattering all-passes, the same lengths the engine uses
    ap_ms = [1.9, 2.3, 2.9, 3.7, 4.3, 5.3, 6.1, 7.1]
    ap_len = [max(1, int(round(m * SR / 1000.0))) for m in ap_ms]
    ap_buf = [np.zeros(L + 1) for L in ap_len]
    ap_pos = [0] * LINES
    out = np.zeros(n)
    x = 1.0
    for i in range(n):
        o = np.empty(LINES)
        for l in range(LINES):
            v = bufs[l][pos[l]]
            if scattering:
                d = ap_buf[l][ap_pos[l]]
                y = d - 0.5 * v
                ap_buf[l][ap_pos[l]] = v + 0.5 * y
                ap_pos[l] = (ap_pos[l] + 1) % ap_len[l]
                v = y
            o[l] = v
        hh = o.sum() * (2.0 / LINES)
        for l in range(LINES):
            bufs[l][pos[l]] = gains[l] * (o[l] - hh) + (-x if (l & 1) else x)
            pos[l] = (pos[l] + 1) % lens[l]
        out[i] = 0.3 * (o[0] - o[1] + o[2] - o[3])
        x = 0.0
    return out


def flatness_db(ir, from_s=0.25):
    """The spread of the tail's third-octave magnitudes, in decibels. Smaller is flatter."""
    seg = ir[int(from_s * SR):]
    seg = seg[: 1 << int(np.floor(np.log2(len(seg))))]
    mag = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    freq = np.fft.rfftfreq(len(seg), 1.0 / SR)
    edges = 100.0 * 2.0 ** (np.arange(0, int(np.log2(8000.0 / 100.0) * 3) + 1) / 3.0)
    bands = []
    for lo, hi in zip(edges[:-1], edges[1:]):
        k = (freq >= lo) & (freq < hi)
        if k.any():
            bands.append(20.0 * np.log10(mag[k].mean() + 1e-12))
    b = np.array(bands)
    return float(b.std())


def coprime_set(rng, lo_ms=28.0, hi_ms=84.0):
    """Eight lengths spread over the same span as the classic set, in samples, mutually prime --
    a length that shares a factor with another puts their modes on top of each other."""
    from math import gcd
    targets = np.geomspace(lo_ms, hi_ms, LINES) * (1.0 + rng.uniform(-0.06, 0.06, LINES))
    out = []
    for t in targets:
        want = int(round(t * SR / 1000.0))
        for step in range(0, 400):
            for cand in (want + step, want - step):
                if cand < 1000:
                    continue
                if all(gcd(cand, o) == 1 for o in out):
                    out.append(cand)
                    break
            else:
                continue
            break
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iterations", type=int, default=300)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    rng = np.random.default_rng(a.seed)

    base = flatness_db(simulate(CLASSIC_MS))
    print("classic lengths: %.3f dB spread" % base)

    best, bestScore = None, base
    for it in range(a.iterations):
        lens = coprime_set(rng)
        ms = [L * 1000.0 / SR for L in lens]
        score = flatness_db(simulate(ms))
        if score < bestScore:
            bestScore = score
            best = ms
            print("  %4d: %.3f dB  %s" % (it, score, ", ".join("%.1f" % m for m in ms)))
    if best is None:
        print("nothing beat the classic set")
        return 0
    print("\nbest: %.3f dB against %.3f dB" % (bestScore, base))
    print("static const float FlatMs[Lines] = { %s };" % ", ".join("%.1ff" % m for m in best))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
