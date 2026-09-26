"""Move the presets towards the production guide's MEASURED targets, from a measurement (25.09.2026).

guide.py holds the guide's windows for parameters; two of the guide's targets are not parameters but
what comes out, and a knob can only be moved towards them once a preset has been heard:

    the sub balance   the sub (the 31.5 and 63 Hz octaves) three to six decibels over 250 and 500 Hz
                      in the long-term mean -- "more is pressure, less is thin" (section 8). Moved by
                      Sub Level, for presets that have a sub at all; a preset without one is left
                      without one, because a sub is an instrument and not a correction.
    the correlation   left against right between 0.3 and 0.7 on the master (section 6): under it the
                      image folds away in mono, over it there is no width left. Moved by the master's
                      Width, inside the guide's cap of 1.3.

Fuzzy, as Rene asked: a preset outside a window is moved into it, to a place fifteen to fifty per
cent of the window's width inside the nearer edge that is drawn from its name, so the library ends
up spread through the windows rather than stacked on their edges -- and a preset inside a window is
not touched. (A first pass moved them sixty per cent of the way to the edge; that left the median
correlation at 0.24 and the median sub balance at +0.6 dB, still outside, and was undone before it
was measured.) Where a knob runs out -- Width at its cap of 1.3 or its floor of 0.3, Sub Level at 1
or twelve decibels in one pass -- the preset goes as far as it can. The
arithmetic of each move is the measured one: the sub's octaves scale with the square of Sub Level,
and the side of an M/S picture with the square of Width, so the correlation after a move is
((1 + c) - (1 - c) k^2) / ((1 + c) + (1 - c) k^2) for a width scaled by k.

The two moves are made together, and the sub is mono: what Sub Level adds goes to the middle alone
and raises the correlation, so the width is solved from the correlation the sub's move leaves. The
first pass of 26.09.2026 did not do that, and 3162 presets it had brought into the correlation's
window came out over 0.7 after their subs had been lifted; a second fit from that measurement put
it right.

Run between the passes of the measurement (measure_packs.py --no-gain, then this, then
measure_packs.py --resume): the next pass renders exactly the presets this moved and puts every
preset's gain into the loudness window once. A second fit is followed by
measure_packs.py --resume --gain-only-rendered, so that nothing is lifted twice.

    python Tools/library/guide_fit.py --cache build/library-work/packs.json            # report
    python Tools/library/guide_fit.py --cache build/library-work/packs.json --write    # do it
"""
import argparse
import hashlib
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import measure_packs as mp  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
SUB_WINDOW = (3.0, 6.0)
CORR_WINDOW = (0.3, 0.7)
INSIDE = (0.15, 0.5)   # where inside the window a preset lands, as a share of its width from the nearer edge
WIDTH_CAP = 1.3        # the guide's cap on the master's width (guide.py)
WIDTH_DEFAULT = 1.2
SUB_STEP_MAX = 12.0    # dB, the most one pass moves a sub


def draw(name, key):
    h = hashlib.sha1(f"{name}|{key}".encode("utf-8")).digest()
    return int.from_bytes(h[:8], "big") / float(1 << 64)


def target(value, window, name, key):
    """Where a value outside the window is moved to: inside it, fifteen to fifty per cent of its
    width from the nearer edge, the place drawn from the preset's name; None when it is inside."""
    lo, hi = window
    if lo <= value <= hi:
        return None
    inward = (INSIDE[0] + (INSIDE[1] - INSIDE[0]) * draw(name, key)) * (hi - lo)
    return lo + inward if value < lo else hi - inward


def fit(p, m, name):
    """Move one preset's Sub Level and Width towards the guide's measured targets. `p` is its
    settings as an ordered dict of text, `m` its measurement (bands, corr). Returns the list of
    what was moved, for the report."""
    moved = []
    c = m.get("corr") if m else None
    # The sub balance, by Sub Level -- only where there is a sub.
    try:
        sub = float(p.get("sub_level", "0") or 0)
    except ValueError:
        sub = 0.0
    sb = mp.sub_balance(m)
    if sub > 0.0 and sb is not None:
        t = target(sb, SUB_WINDOW, name, "sub")
        if t is not None:
            # The sub's own share of the two low octaves: the rest (the pads above their low cut,
            # a texture's rumble) does not move with Sub Level. Taken as the whole when the sub
            # dominates, which is where the knob can reach the window at all.
            step = max(-SUB_STEP_MAX, min(SUB_STEP_MAX, t - sb))
            new = min(1.0, max(0.01, sub * 10.0 ** (step / 20.0)))
            if abs(new - sub) > 1e-3:
                p["sub_level"] = f"{new:.4g}"
                moved.append(("sub", sb, sb + 20.0 * math.log10(new / sub)))
                # The sub is mono: what it gains is added to the middle alone, and the correlation
                # rises with it. The first pass left this out and 3162 presets it had brought into
                # the correlation's window came out over 0.7 once their sub had been lifted.
                if c is not None and -1.0 < c < 1.0 and m.get("bands"):
                    pw = [10.0 ** (v / 10.0) for v in m["bands"]]
                    total, psub = sum(pw), pw[0] + pw[1]
                    mid = 0.5 * (1.0 + c) * total + psub * ((new / sub) ** 2 - 1.0)
                    side = 0.5 * (1.0 - c) * total
                    if mid + side > 1e-30:
                        c = (mid - side) / (mid + side)
    # The correlation, by the master's Width -- from what the sub's move leaves of it.
    if c is not None and -1.0 < c < 1.0:
        t = target(c, CORR_WINDOW, name, "corr")
        if t is not None:
            try:
                w0 = float(p.get("width", WIDTH_DEFAULT))
            except ValueError:
                w0 = WIDTH_DEFAULT
            if w0 > 1e-3:
                # c = (M - S') / (M + S') with S' = k^2 S: solve for k^2.
                k2 = ((1.0 + c) * (1.0 - t)) / ((1.0 - c) * (1.0 + t))
                w1 = min(WIDTH_CAP, max(0.3, w0 * math.sqrt(max(k2, 0.0))))
                if abs(w1 - w0) > 1e-3:
                    k2a = (w1 / w0) ** 2
                    ca = ((1.0 + c) - (1.0 - c) * k2a) / ((1.0 + c) + (1.0 - c) * k2a)
                    p["width"] = f"{w1:.4g}"
                    moved.append(("corr", c, ca))
    return moved


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--packs", default=os.path.join(ROOT, "Library", "Packs"))
    ap.add_argument("--cache", required=True, help="the measurement, as measure_packs.py --cache wrote it")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    cache = json.load(open(a.cache, encoding="utf-8"))
    files = sorted(f for f in os.listdir(a.packs) if f.endswith(".ambientpack"))
    n = nsub = ncorr = missing = 0
    before = {"sub": [], "corr": []}
    after = {"sub": [], "corr": []}
    for f in files:
        path = os.path.join(a.packs, f)
        head, rows = mp.read_pack(path)
        changed = False
        for r in rows:
            m = cache.get(r["name"])
            if not m:
                missing += 1
                continue
            p = {}
            for kv in r["settings"].split(";"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    p[k] = v
            moved = fit(p, m, r["name"])
            for kind, b, af in moved:
                before[kind].append(b); after[kind].append(af)
                if kind == "sub": nsub += 1
                else: ncorr += 1
            if moved:
                r["settings"] = ";".join(f"{k}={v}" for k, v in p.items())
                n += 1
                changed = True
        if changed and a.write:
            mp.write_pack(path, head, rows)

    def stat(xs):
        return f"median {sorted(xs)[len(xs) // 2]:.2f}" if xs else "none"
    print(f"{n} presets moved: {nsub} sub balances ({stat(before['sub'])} dB -> {stat(after['sub'])} dB), "
          f"{ncorr} correlations ({stat(before['corr'])} -> {stat(after['corr'])}); {missing} not in the cache")
    print("written" if a.write else "report only (add --write)")


if __name__ == "__main__":
    main()
