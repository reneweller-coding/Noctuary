"""The rule book's own check, measured: section 11, the anti-rules of section 12 and the roles
of table 2, read off an hour of the conductor's notes.

`ambient_brain_audit` steps the engine's conductors for an hour with the parameters exactly as a
render would read them and prints the events; this turns them into the numbers the rule book
asks for. Nothing here listens -- the rule book says why: the faults show up after half an hour.

  brain_audit.py --rulebook                     every rule parameter at its full value, on the defaults
  brain_audit.py --preset "Glacier Bloom"       one library preset, as the render would play it
  brain_audit.py --sweep 100 --jobs 6           every 100th preset of the library, aggregated
"""
import argparse
import collections
import glob
import json
import math
import os
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOL = os.path.join(ROOT, "bin", "msvc", "ambient_brain_audit.exe")
PACKS = os.path.join(ROOT, "Library", "Packs")

# Section 14 at the values the rule book itself names, on top of the defaults.
RULEBOOK = [("brain_on", "1"), ("brain_density", "5"), ("brain_rate", "45"),
            ("brain_hold_min", "40"), ("brain_hold_max", "240"), ("brain_low", "24"), ("brain_high", "103"),
            ("brain_layers", "1"), ("brain_bass_hold", "6"), ("brain_top_soft", "1"),
            ("brain_low_spacing", "1"), ("brain_third_floor", "48"), ("brain_leading", "1"),
            ("brain_seventh", "0.5"), ("brain_rate_breath", "1"), ("brain_breath_period", "10"),
            ("brain_overlap", "10"), ("brain_onset_guard", "1"), ("brain_release_gap", "2"),
            ("brain_retrigger", "30"), ("brain_density_slew", "2"), ("brain_root_steps", "Diatonic"),
            ("brain_root_down", "0.5"), ("brain_pivot", "30"), ("brain_home", "1"), ("brain_home_time", "60"),
            ("brain_memory", "10"), ("brain_wander", "0.3"), ("brain_spacing", "0.6"), ("brain_harmonic", "0.5"),
            ("brain_key", "0.5"), ("brain_even", "0.3"), ("brain_blend", "0.9"), ("brain_spread", "0.7"),
            # Table 4.2 gives the minor second 0.03 and the major seventh -- its inversion -- no
            # line at all, which is what "seconds avoided" is the parameter for.
            ("brain_seconds", "-0.5"),
            ("brain_dejavu", "0.3"), ("brain_loop", "8"), ("brain2_on", "1"), ("brain2_golden", "1"),
            ("brain2_interval", "7"), ("auto_root_move", "0.1")]   # section 9: Root Move low, 0.05..0.2

# Section 4.2: the interval weights a new note should form against what sounds.
WEIGHTS = {0: 0.18, 7: 0.20, 5: 0.12, 2: 0.11, 3: 0.09, 4: 0.08, 10: 0.09, 8: 0.05, 9: 0.04, 1: 0.03, 6: 0.01, 11: 0.0}
IC_NAME = {0: "8ve/1", 1: "m2", 2: "M2", 3: "m3", 4: "M3", 5: "P4", 6: "TT", 7: "P5", 8: "m6", 9: "M6", 10: "m7", 11: "M7"}

# Section 3: the least interval by the register of the lower note.
def least_interval(lower):
    return 12 if lower < 36 else 7 if lower < 48 else 3 if lower < 60 else 2 if lower < 72 else 1

# Table 2: the roles by register.
ROLES = [("Fundament", 24, 43), ("Koerper", 43, 60), ("Farbe", 60, 84), ("Luft", 84, 104)]

# The pitch class of a MIDI note in the instrument's own tuning, as the tool reports it; a note
# the tool never played falls back to the MIDI number's. Constellations and the pitch-class share
# are counted in these, since that is what the conductor's memory keys and what is heard; the
# spacing rules stay in MIDI numbers, which is what the rule book writes them in.
PC = {}


def pc_of(note):
    return PC.get(note, note % 12)


def run_tool(preset=None, sets=(), hours=1.0, dt=0.005, seed=None):
    cmd = [TOOL, "--hours", str(hours), "--dt", str(dt)]
    if preset:
        cmd += ["--preset", preset]
    for k, v in sets:
        cmd += ["--set", f"{k}={v}"]
    if seed is not None:
        cmd += ["--seed", str(seed)]
    env = dict(os.environ)
    env.setdefault("AMBIENT_PACKS", PACKS)
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=ROOT, timeout=600)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or f"exit {r.returncode}")
    events, roots = [], []
    PC.clear()
    for line in r.stdout.splitlines():
        if line.startswith("E,"):
            parts = line.split(",")
            _, t, which, on, note, vel = parts[:6]
            events.append((float(t), int(which), on == "1", int(note), float(vel)))
            if len(parts) > 6:      # the pitch class as the instrument's tuning has it
                PC[int(note)] = int(parts[6])
        elif line.startswith("R,"):
            _, t, root = line.split(",")
            roots.append((float(t), int(root)))
    return events, roots


def intervals_of(events, which, end):
    """(t_on, t_off, note, vel) for one conductor; a note still sounding ends at `end`."""
    out, open_ = [], {}
    for t, w, on, note, vel in events:
        if w != which:
            continue
        if on:
            if note in open_:                      # a note-on over a note-on: close the first
                t0, v0 = open_.pop(note)
                out.append((t0, t, note, v0))
            open_[note] = (t, vel)
        elif note in open_:
            t0, v0 = open_.pop(note)
            out.append((t0, t, note, v0))
    for note, (t0, v0) in open_.items():
        out.append((t0, end, note, v0))
    out.sort()
    return out


def root_at(roots, t):
    r = roots[0][1] if roots else 48
    for tr, root in roots:
        if tr <= t:
            r = root
        else:
            break
    return r


def analyse(events, roots, hours, dt):
    end = hours * 3600.0
    n1 = intervals_of(events, 1, end)
    n2 = intervals_of(events, 2, end)
    ons1 = sorted(t for t, w, on, _, _ in events if w == 1 and on)
    offs1 = sorted(t for t, w, on, _, _ in events if w == 1 and not on)
    m = collections.OrderedDict()
    if not n1:
        m["events"] = (0, False, "the conductor played nothing")
        return m
    first = n1[0][0]

    # ---- sounding count per second (from the first note on) -------------------------------
    T = int(end)
    count = [0] * T
    for t0, t1, note, vel in n1:
        for s in range(max(0, int(t0)), min(T, int(math.ceil(t1)))):
            count[s] += 1
    after = [c for s, c in enumerate(count) if s >= first]
    silent = sum(1 for c in after if c == 0)
    m["11.1 time with a note"] = (100.0 * (1 - silent / max(1, len(after))), silent == 0,
                                  f"{silent} s of silence after the first note")
    mean_v = statistics.mean(after) if after else 0.0
    m["11.2 voices mean/max"] = ((round(mean_v, 2), max(after) if after else 0),
                                 2.0 <= mean_v <= 8.0 and max(after) <= 10, "2..8, none above 10")

    # ---- onsets: the thirty-millisecond rule, the exponential gaps, G1 ----------------------
    gaps = [b - a for a, b in zip(ons1, ons1[1:])]
    between = sum(1 for g in gaps if 0.031 < g < 2.999)
    fused = sum(1 for g in gaps if g <= 0.031)
    far = [g for g in gaps if g >= 2.999]
    m["11.7 onsets 30 ms..3 s"] = (between, between == 0, f"{fused} fused, {len(far)} separate")
    # Onsets inside thirty milliseconds are one decision (a Blend); the groups are what the
    # rules about decisions are counted on.
    groups = []
    for t in ons1:
        if groups and t - groups[-1][1] <= 0.031:
            groups[-1][1] = t
            groups[-1][2] += 1
        else:
            groups.append([t, t, 1])
    m["R2.3 the entrance"] = (groups[0][2] if groups else 0, bool(groups) and groups[0][2] == 1,
                             "notes in the first decision: the sound builds from below, one voice first")
    fast = sum(1 for a, b in zip(groups, groups[1:]) if b[0] - a[1] < 20.0 - 2.5 * dt)   # to the tick
    m["Anti 2 decisions < 20 s apart"] = (fast, fast == 0, f"of {len(groups)} decisions")
    if len(far) >= 8:
        mu = statistics.mean(far)
        cv = statistics.pstdev(far) / mu if mu > 0 else 0.0
        med = statistics.median(far)
        # An exponential has a coefficient of variation of one and a median of 0.69 of its mean;
        # a clock has a coefficient near nought. The breath widens the spread a little.
        m["11.3 gaps exponential (cv, median/mean)"] = ((round(cv, 2), round(med / mu, 2)),
                                                       0.6 <= cv <= 1.6, f"mean {mu:.1f} s over {len(far)} gaps")
        m["G1 mean decision gap"] = (round(mu, 1), 20.0 <= mu <= 150.0, "one decision per 20 s .. 2.5 min")
        hist = collections.Counter(int(g // 5) for g in far)
        top = hist.most_common(1)[0][1] / len(far)
        m["11.3 no peak at a fixed gap"] = (round(top, 2), top < 0.35, "share of the busiest 5 s bin")

    # ---- density autocorrelation: no period under four minutes (11.4) ----------------------
    if len(after) > 900:
        x = [c - mean_v for c in after]
        var = sum(v * v for v in x) / len(x)
        acf = []
        for lag in range(0, 600, 5):
            s = sum(x[i] * x[i + lag] for i in range(len(x) - lag)) / max(1, len(x) - lag)
            acf.append(s / var if var > 0 else 0.0)
        # the first local maximum after the decay, if any, inside four minutes. A maximum worth the
        # name: an exchange lifts the count for the ten seconds of its overlap, and those bumps,
        # never closer than the floor, put a ripple of 0.3 to 0.4 into the correlation at the
        # floor's lag whatever the clock does. A period the rule means is a swing of the density.
        peak = None
        for i in range(2, len(acf) - 1):
            if acf[i] > acf[i - 1] and acf[i] >= acf[i + 1] and acf[i] > 0.5 and i * 5 < 240:
                peak = (i * 5, round(acf[i], 2))
                break
        m["11.4 density period < 4 min"] = (peak, peak is None, "lag and height of a secondary maximum above 0.5")

    # ---- pairs of sounding notes: section 3, the anti-rules 4, 5, 6, R3.2, R4.3 ----------------
    spacing = seconds48 = thirds48 = tritone55 = both_thirds = leading = 0
    pairs = 0
    for i in range(len(n1)):
        a0, a1, na, _ = n1[i]
        for j in range(i + 1, len(n1)):
            b0, b1, nb, _ = n1[j]
            if b0 >= a1:
                break
            if min(a1, b1) - max(a0, b0) <= 0.0:
                continue
            pairs += 1
            lower, d = min(na, nb), abs(na - nb)
            if d < least_interval(lower):
                spacing += 1
            if d in (1, 2) and lower < 48:
                seconds48 += 1
            if d in (3, 4) and lower < 48:
                thirds48 += 1
            if d % 12 == 6 and lower < 55:
                tritone55 += 1
    # R4.3: a major and a minor third over the same note, together, under 60
    for i in range(len(n1)):
        a0, a1, na, _ = n1[i]
        if na >= 60:
            continue
        has = {}
        for j in range(len(n1)):
            b0, b1, nb, _ = n1[j]
            if j == i or nb - na not in (3, 4):
                continue
            lo, hi = max(a0, b0), min(a1, b1)
            if hi > lo:
                has.setdefault(nb - na, []).append((lo, hi))
        for x0, x1 in has.get(3, []):
            for y0, y1 in has.get(4, []):
                if min(x1, y1) > max(x0, y0):
                    both_thirds += 1
    # Anti 6: the leading note while a root sounds
    for t0, t1, note, _ in n1:
        root = root_at(roots, t0)
        if (pc_of(note) - pc_of(root)) % 12 != 11:
            continue
        for u0, u1, other, _ in n1:
            if pc_of(other) == pc_of(root) and min(t1, u1) > max(t0, u0):
                leading += 1
                break
    m["3 spacing by register"] = (spacing, spacing == 0, f"of {pairs} sounding pairs")
    m["Anti 4 seconds under 48"] = (seconds48, seconds48 == 0, "")
    m["Anti 5 thirds under 48"] = (thirds48, thirds48 == 0, "")
    m["R3.2 tritone under 55"] = (tritone55, tritone55 == 0, "")
    m["R4.3 both thirds under 60"] = (both_thirds, both_thirds == 0, "")
    m["Anti 6 leading note with root"] = (leading, leading == 0, "")

    # ---- R2.2: two to an octave, one below 36 --------------------------------------------------
    over = 0
    for s in range(int(first), T):
        per = collections.Counter()
        for t0, t1, note, _ in n1:
            if t0 <= s < t1:
                per[note // 12] += 1
        for octave, k in per.items():
            if k > (1 if octave * 12 < 36 else 2):
                over += 1
    m["R2.2 notes per octave"] = (over, over == 0, "seconds with a third note in an octave (a second below 36)")

    # ---- interval histogram against 4.2 (R4.2: against everything that sounds) -----------------
    hist = collections.Counter()
    for t0, t1, note, _ in n1:
        for u0, u1, other, _ in n1:
            if u0 < t0 < u1 and other != note:
                hist[abs(note - other) % 12] += 1
    total = sum(hist.values())
    if total:
        dev = {ic: hist[ic] / total - WEIGHTS[ic] for ic in range(12)}
        worst = max(dev.items(), key=lambda kv: abs(kv[1]))
        tv = 0.5 * sum(abs(v) for v in dev.values())
        m["11.5 interval histogram vs 4.2"] = (round(tv, 2), tv < 0.2,
                                               "total variation; worst " + IC_NAME[worst[0]] + f" {worst[1]:+.2f}; "
                                               + " ".join(f"{IC_NAME[ic]}={hist[ic]/total:.2f}" for ic in (0, 7, 5, 2, 3, 4, 10, 8, 9, 1, 6, 11)))

    # ---- retrigger, release gap, overlap, all-ending, density jumps ------------------------------
    last_off = {}
    retrig = 0
    for t, w, on, note, _ in events:
        if w != 1:
            continue
        if on and note in last_off and t - last_off[note] < 30.0:
            retrig += 1
        if not on:
            last_off[note] = t
    m["Anti 10 retrigger < 30 s"] = (retrig, retrig == 0, "")
    close_offs = sum(1 for a, b in zip(offs1, offs1[1:]) if b - a < 2.0 - 1e-6)
    m["R8.3 note-offs < 2 s apart"] = (close_offs, close_offs == 0, "")
    short_overlap = 0
    for t in offs1:
        before = [o for o in ons1 if o < t]
        still = sum(1 for t0, t1, _, _ in n1 if t0 < t < t1)
        if before and still > 0 and t - before[-1] < 10.0 - 2.5 * dt:   # to the tick
            short_overlap += 1
    m["R5.4 overlap under 10 s"] = (short_overlap, short_overlap == 0, "note-offs less than 10 s after the last note-on")
    all_end = sum(1 for s in range(int(first) + 1, T) if count[s] == 0 and count[s - 1] >= 2)
    m["Anti 1 all voices end together"] = (all_end, all_end == 0, "")
    # Anti 9 is about Density the parameter, which nothing here moves; what the notes can show is
    # how far the count rises inside a minute, fused chords and all. Reported, not judged.
    rise = max((max(count[s:s + 60]) - count[s] for s in range(int(first), T - 60)), default=0)
    m["Anti 9 largest rise in a minute"] = (rise, None, "voices; a Blend arrives together, Density Slew is the rule's parameter")

    # ---- constellations (R6.6 / 11.9): what a decision CREATES may not have sounded within ten
    # minutes. A constellation that comes back because a note left is not created, so only the
    # constellation after each decision is asked -- which is also what the conductor's memory keys.
    series = []
    for s in range(T):
        lows = [note for t0, t1, note, _ in n1 if t0 <= s < t1]
        series.append((frozenset(pc_of(n) for n in lows), min(lows) // 12) if lows else None)
    changes = []
    for s, key in enumerate(series):
        if not changes or changes[-1][1] != key:
            changes.append((s, key))
    repeats = 0
    for g0, g1, n in groups:
        s = int(g1) + 1
        if s >= T or series[s] is None:
            continue
        key = series[s]
        # A decision that adds no pitch class and lowers nothing -- an octave or a unison of
        # what sounds -- creates no constellation: it is the one already there, thickened.
        if int(g0) < T and series[int(g0)] == key:
            continue
        for i in range(len(changes) - 1, -1, -1):
            start, k = changes[i]
            if start > s - 1:
                continue
            endt = changes[i + 1][0] if i + 1 < len(changes) else T
            if k == key and endt <= s and s - endt < 600:
                repeats += 1
                break
            if s - endt >= 600:
                break
    m["R6.6 constellation back < 10 min"] = (repeats, repeats == 0, f"of {len(groups)} decisions, {len(changes)} constellations")

    # ---- pitch classes (11.10): the share of the sounding note-seconds, not of the clock, and the
    # root excepted at the time it IS the root -- a foundation lying on the root of the moment is
    # what the rule allows, a class that goes on dominating after the root has moved is not.
    secs = collections.Counter()
    total_secs = 0.0
    bounds = [t for t, _ in roots] + [end]
    for t0, t1, note, _ in n1:
        total_secs += t1 - t0
        for i in range(len(bounds) - 1):
            lo, hi = max(t0, bounds[i]), min(t1, bounds[i + 1])
            if hi > lo and pc_of(note) != pc_of(roots[i][1]):
                secs[pc_of(note)] += hi - lo
    total_secs = total_secs or 1.0
    heavy = [(pc, round(v / total_secs, 2)) for pc, v in secs.items() if v / total_secs > 0.25]
    m["11.10 pitch class over 25 %"] = (heavy, not heavy, "the root of the moment excepted; largest "
                                       + ", ".join(f"{pc}:{v / total_secs:.2f}" for pc, v in secs.most_common(3)))

    # ---- the root (R6.1, R6.2, R6.4, Anti 8, 11.8) ------------------------------------------
    moves = [(t, r) for t, r in roots[1:]]
    per_hour = len(moves) / hours
    m["11.8 root changes per hour"] = (round(per_hour, 1), 3.0 <= per_hour <= 10.0, "3..10")
    climbs = sum(1 for (t0, r0), (t1, r1) in zip(roots, roots[1:]) if r1 - r0 == 1)
    m["Anti 8 root climbs a semitone"] = (climbs, climbs == 0, "")
    if len(roots) >= 2:
        steps = collections.Counter()
        for (t0, r0), (t1, r1) in zip(roots, roots[1:]):
            d = r1 - r0
            steps[("down" if d < 0 else "up") + f" {abs(d) % 12}"] += 1
        m["R6.2 root steps"] = (dict(steps.most_common(6)), None, "semitones, signed")
        gaps_r = [b - a for (a, _), (b, _) in zip(roots, roots[1:])]
        m["R6.1 minutes between root changes"] = (round(statistics.mean(gaps_r) / 60.0, 1), None, "4..12 asked for")
    home = roots[0][1] if roots else None
    back = [t for t, r in roots if r == home and t > 0]
    m["R6.4 back home"] = (round(back[-1] / 60.0, 1) if back else None,
                          bool(roots) and roots[-1][1] == home, "minute of the last return; the last root must be home")

    # ---- table 2: the roles -------------------------------------------------------------------
    roles = {}
    for name, lo, hi in ROLES:
        rows = [(t1 - t0, vel) for t0, t1, note, vel in n1 if lo <= note < hi and t1 < end]
        if rows:
            holds = [h for h, _ in rows]
            vels = [round(v * 127) for _, v in rows]
            roles[name] = (len(rows), round(statistics.median(holds)), round(statistics.mean(vels)),
                           (min(vels), max(vels)))
    m["Tab.2 roles n/hold/vel/range"] = (roles, None, "count, median hold s, mean velocity, velocity range")

    # ---- Anti 7 (a figure of three repeats), Anti 12 (alternation) ----------------------------
    seq = [note for t, w, on, note, _ in events if w == 1 and on]
    trig = collections.Counter(tuple(seq[i:i + 3]) for i in range(len(seq) - 2))
    m["Anti 7 repeated three-note figures"] = (sum(1 for k, v in trig.items() if v > 1), None, "figures seen twice or more")
    alt = sum(1 for i in range(len(seq) - 3) if seq[i] == seq[i + 2] and seq[i + 1] == seq[i + 3] and seq[i] != seq[i + 1])
    m["Anti 12 a voice alternating A B A B"] = (alt, alt == 0, "")

    # ---- the second conductor: Anti 15 --------------------------------------------------------
    ons2 = sorted(t for t, w, on, _, _ in events if w == 2 and on)
    g2 = [b - a for a, b in zip(ons2, ons2[1:]) if b - a > 0.031]
    if len(far) >= 8 and len(g2) >= 8:
        ratio = statistics.mean(g2) / statistics.mean(far)
        simple = any(abs(ratio - p / q) < 0.03 * (p / q) for p in range(1, 5) for q in range(1, 5))
        # Reported, not judged: fifty gaps an hour put a fifteen per cent sampling error on the
        # mean, so a golden pair of clocks measures as 3:2 one hour in four. The clocks themselves
        # are set in the golden ratio (brain2_golden), which is what Anti 15 asks.
        m["Anti 15 the two clocks' ratio"] = (round(ratio, 3), None, "mean gap of conductor 2 over 1" + (" (near a p/q, within sampling error)" if simple else ""))

    # ---- the near layer (13.09.2026): the foreground's own rules, when it is on ------------
    # Conductor 3 in the tool's output is the near events: one note at a time for a Note or a
    # Phrase, a run of short ones for a Sequence. What is checked here is what the engine promises
    # whatever the settings: never two events at once, never one within six seconds of a root
    # change, and -- with Hold Brain on, the default -- no decision of the conductor while a Note
    # or a Phrase sounds. The gaps are reported against their own floor (the smaller of five
    # seconds and a fifth of the rate) rather than judged, since the rate is a parameter the audit
    # does not see.
    n3 = intervals_of(events, 3, end)
    if n3:
        m["Near events"] = (len(n3), None, "notes the foreground played")
        overlaps = sum(1 for (a0, a1, _, _), (b0, b1, _, _) in zip(n3, n3[1:]) if b0 < a1 - dt)
        m["Near never two at once"] = (overlaps, overlaps == 0, "notes beginning while one still sounds")
        soon = 0
        for t0, _, _, _ in n3:
            last = max((tr for tr, _ in roots if tr <= t0 and tr > 0.0), default=-1.0e9)
            if t0 - last < 6.0 - dt:
                soon += 1
        m["Near not just after a root change"] = (soon, soon == 0, "notes within six seconds of a root change")
        gaps = [b0 - a1 for (a0, a1, _, _), (b0, b1, _, _) in zip(n3, n3[1:]) if b0 >= a1]
        long_gaps = [g for g in gaps if g > 2.0]      # a sequence's steps are not gaps between events
        if long_gaps:
            m["Near gaps min/mean s"] = ((round(min(long_gaps), 1), round(statistics.mean(long_gaps), 1)), None, "between events")
        inside = 0
        for t, w, on, _, _ in events:
            if w == 1 and on and any(t0 < t < t1 for t0, t1, _, _ in n3 if t1 - t0 > 2.0):
                inside += 1
        m["Near the conductor holds"] = (inside, None, "conductor notes begun while a long near note sounded (0 with Hold Brain on)")
    return m


def load_names(every):
    names = []
    i = 0
    for f in sorted(glob.glob(os.path.join(PACKS, "*.ambientpack"))):
        for line in open(f, encoding="utf-8"):
            if not line.strip() or line.startswith(("#", "pack ", "format ")):
                continue
            p = line.split("|")
            if len(p) < 7:
                continue
            if i % every == 0:
                names.append(p[0])
            i += 1
    return names


def print_report(m, title):
    print(f"\n== {title}")
    for key, (value, ok, note) in m.items():
        mark = "  " if ok is None else ("ok" if ok else "!!")
        print(f"  {mark} {key:38s} {str(value):24s} {note}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--preset", default="")
    ap.add_argument("--rulebook", action="store_true", help="section 14 at its full values, on the defaults")
    ap.add_argument("--set", action="append", default=[], help="key=value on top")
    ap.add_argument("--hours", type=float, default=1.0)
    ap.add_argument("--dt", type=float, default=0.005)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--sweep", type=int, default=0, help="every Nth preset of the library")
    ap.add_argument("--seeds", type=int, default=0, help="run this many seeds of one setting and aggregate")
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--json", default="", help="write the per-preset results here")
    a = ap.parse_args()
    sets = list(RULEBOOK) if a.rulebook else []
    sets += [tuple(s.split("=", 1)) for s in a.set]

    if a.sweep or a.seeds:
        if a.seeds:
            names = [f"seed {k}" for k in range(a.seeds)]
            print(f"{a.seeds} seeds of {a.preset or 'the rule book'}, {a.hours:g} h each")
        else:
            names = load_names(a.sweep)
            print(f"{len(names)} presets, {a.hours:g} h each")
        results = {}

        def one(name):
            try:
                if a.seeds:
                    ev, roots = run_tool(a.preset or None, sets, a.hours, a.dt, int(name.split()[1]) + 1)
                else:
                    ev, roots = run_tool(name, sets, a.hours, a.dt, a.seed)
                return name, analyse(ev, roots, a.hours, a.dt)
            except Exception as e:      # report, keep sweeping
                return name, {"error": (str(e), False, "")}
        with ThreadPoolExecutor(max_workers=a.jobs) as ex:
            for i, (name, m) in enumerate(ex.map(one, names)):
                results[name] = m
                if (i + 1) % 50 == 0:
                    print(f"  {i + 1}/{len(names)}", flush=True)
        # Aggregate: for every rule with a verdict, how many presets keep it.
        rules = collections.OrderedDict()
        for name, m in results.items():
            for key, (value, ok, note) in m.items():
                if ok is None:
                    continue
                r = rules.setdefault(key, {"kept": 0, "broken": 0, "worst": []})
                if ok:
                    r["kept"] += 1
                else:
                    r["broken"] += 1
                    r["worst"].append((name, value))
        print("\n== library sweep: presets keeping each rule")
        for key, r in rules.items():
            n = r["kept"] + r["broken"]
            worst = ", ".join(f"{nm} ({v})" for nm, v in r["worst"][:3])
            print(f"  {100.0 * r['kept'] / max(1, n):5.1f} %  {key:38s} broken in {r['broken']:4d} of {n}  {worst}")
        if a.json:
            json.dump({k: {kk: [str(v[0]), v[1], v[2]] for kk, v in m.items()} for k, m in results.items()},
                      open(a.json, "w", encoding="utf-8"), indent=1)
        return 0

    ev, roots = run_tool(a.preset or None, sets, a.hours, a.dt, a.seed)
    m = analyse(ev, roots, a.hours, a.dt)
    print_report(m, a.preset or ("the rule book on the defaults" if a.rulebook else "the defaults"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
