"""Move the presets that exist to the circuit filters (26.09.2026), by circuit.py's rule.

Rene, after the A/B renders of the old Ladder against the Moog: "Ja, bitte umziehen. Bitte auch
noch andere Presets auf die anderen neuen Filter umstellen, auch aus den Packs (aber nur die, die
bislang nicht ausschliesslich den z-Filter benutzen). Ich denke, das Neuvermessen koennen wir uns
trotzdem sparen." The packs by their artist's family, the built-ins in Core/src/Presets.cpp by the
family their section stands in for (conductor.BUILTIN_FAMILY); the near bank is make_layer_presets.py's
and moves when that is run. Idempotent: a preset on a circuit model is not moved again.

Not measured again, as Rene asked; what a four-pole circuit's pass band gives up is written into
master_gain by the rule. --check renders a sample of every kind of move before and after (the
moved settings passed with --set, nothing written) and reports how far the loudness moved, which is
what says whether that is good enough. It was, for every kind but one: a preset moved off the old
Ladder once came out 19 dB quieter ("Envelope Hollow"). The old ladder's sample of delay in its
loop makes it oscillate at Nyquist once its corner is pushed near the top -- by the envelope, key
tracking, the drift -- and the measurement of the library counted that inaudible 24 kHz tone as
loudness, so the preset had been turned down for it. The Moog does not do that. And the diode
ladder's pass band, being the most nonlinear of the five, came out anywhere from 8 dB under to 4 dB
over what circuit.py reckoned for it, and one SEM high pass 5.5 dB under (its damping saturates, and
the high pass is what is left after it). So every move but LP 12 to the SEM -- every Ladder, LP 24,
HP 12 and Notch -- is rendered on both filters (--measure, 30 seconds each, written to
circuit_measured.json), and corrected by what it measured instead of by the reckoning, as far as
its true peak allows. LP 12 to the SEM keeps the reckoning: the same two poles and the SEM's low
pass, and every one of the sample stayed within 1.2 dB of the library's measurement.

    python Tools/library/retrofit_circuit.py               # what it would do
    python Tools/library/retrofit_circuit.py --check 12    # and how loud the moved presets come out
    python Tools/library/retrofit_circuit.py --measure     # the measured kinds of move on both filters
    python Tools/library/retrofit_circuit.py --write       # do it
"""
import json
import argparse
import collections
import concurrent.futures
import glob
import os
import re
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import circuit            # noqa: E402
import conductor          # noqa: E402
import guide              # noqa: E402  (the built-in rows' pattern and the literal wrapping)
import retrofit_review    # noqa: E402  (a pack's family)

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
BUILTINS = os.path.join(ROOT, "Core", "src", "Presets.cpp")
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
SECTION = retrofit_review.SECTION
MOVED_KEYS = ("filter_model", "cutoff", "resonance", "filter_morph", "master_gain")
MEASURED_JSON = os.path.join(HERE, "circuit_measured.json")
MEASURED_FROM = ("Ladder->", "LP 24->", "HP 12->", "Notch->")   # the kinds of move rendered on both filters
TRUE_PEAK_MAX = -1.0   # dBTP, the library's ceiling (measure_packs.py)


def retrofit_pack(text, family, counts, moves, measured=None):
    out, changed = [], 0
    for line in text.split("\n"):
        if line.startswith(("#", "pack ", "format ")) or "|" not in line:
            out.append(line); continue
        parts = line.split("|")
        new, what = circuit.apply_to_settings(parts[1], family, parts[0], measured=measured)
        if what:
            moves.append((parts[0], what, parts[1], new))
            parts[1] = new; changed += 1; counts[what] += 1
        out.append("|".join(parts))
    return "\n".join(out), changed


def retrofit_builtins(text, counts, moves, measured=None):
    start = text.index("const Preset kPresets[] = {")
    end = text.index("\n};", start)
    head, body, tail = text[:start], text[start:end], text[end:]
    sections = [(m.start(), m.group(1).strip()) for m in SECTION.finditer(body)]
    changed = 0

    def family_at(pos):
        fam = None
        for at, name in sections:
            if at > pos:
                break
            fam = conductor.BUILTIN_FAMILY.get(name)
        return fam

    def row(m):
        nonlocal changed
        settings = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(3)))
        new, what = circuit.apply_to_settings(settings, family_at(m.start()), m.group(2), measured=measured)
        if not what:
            return m.group(0)
        moves.append((m.group(2), "built-in " + what, settings, new))
        changed += 1
        counts["built-in " + what] += 1
        return m.group(1) + guide.wrap_literal(new)
    body = guide.ROW.sub(row, body)
    return head + body + tail, changed


def loudness(name, sets):
    """(integrated LUFS, true peak) of 30 seconds of a preset, the settings in `sets` put over it."""
    cmd = [RENDER, "--packs", PACKS, "--preset", name, "--seconds", "30", "--notes", "45,52,59",
           "--set", "brain_rate=6", "--hour", "21", "--loudness", "--measure"]
    for k, v in sets:
        cmd += ["--set", f"{k}={v}"]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       env=dict(os.environ, AMBIENT_PACKS=PACKS))
    m = re.search(r"lufs_i=(-?[\d.]+).*?truepeak=(-?[\d.]+)", r.stdout)
    return (float(m.group(1)), float(m.group(2))) if m else (None, None)


def lufs(name, sets):
    return loudness(name, sets)[0]


def moved_sets(old, new, keys=("filter_model", "cutoff", "resonance", "filter_morph")):
    o, n = circuit.parse(old), circuit.parse(new)
    return [(k, n[k]) for k in keys if k in n and n.get(k) != o.get(k)]


def measure(moves, jobs):
    """Every preset of a kind of move in MEASURED_FROM, rendered on both filters (at its old level):
    how many decibels louder the circuit came out, capped where its true peak would pass the ceiling
    after the correction. Presets measured before are kept."""
    done = json.load(open(MEASURED_JSON, encoding="utf-8")) if os.path.isfile(MEASURED_JSON) else {"delta_db": {}, "lufs_before_after_truepeak_after": {}}
    work = [(name, moved_sets(old, new)) for name, what, old, new in moves
            if what.replace("built-in ", "").startswith(MEASURED_FROM) and name not in done["delta_db"]]
    print(f"rendering {len(work)} presets on both filters ({len(done['delta_db'])} measured before) ...", flush=True)
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        before = list(ex.map(lambda w: loudness(w[0], []), work))
        after = list(ex.map(lambda w: loudness(w[0], w[1]), work))
    delta, raw, capped = done["delta_db"], done["lufs_before_after_truepeak_after"], 0
    for (name, _), (lb, _), (la, tpa) in zip(work, before, after):
        if lb is None or la is None or lb < -70.0 or la < -70.0:
            continue
        d = la - lb
        raw[name] = [round(lb, 2), round(la, 2), round(tpa, 2)]
        # The correction lifts by -d; the true peak after it is tpa - d.
        if tpa - d > TRUE_PEAK_MAX:
            d = tpa - TRUE_PEAK_MAX
            capped += 1
        delta[name] = round(d, 2)
    json.dump({"what": "dB louder on the circuit than on the filter it replaced, at the old level, 30 s, "
                       "notes 45 52 59 (retrofit_circuit.py --measure)",
               "delta_db": delta, "lufs_before_after_truepeak_after": raw},
              open(MEASURED_JSON, "w", encoding="utf-8"), indent=0, sort_keys=True)
    d = sorted(delta[name] for name, _ in work if name in delta)
    if d:
        print(f"{len(d)} measured: median {statistics.median(d):+.1f} dB, {d[0]:+.1f} .. {d[-1]:+.1f}; "
              f"{sum(1 for x in d if abs(x) > 6)} more than 6 dB apart; {capped} held by the true peak")


def check(moves, per_kind, jobs):
    """Render a sample of every kind of move before and after; report the loudness change."""
    by_kind = collections.defaultdict(list)
    for mv in moves:
        by_kind[mv[1]].append(mv)
    work = []
    for kind, mvs in sorted(by_kind.items()):
        step = max(1, len(mvs) // per_kind)
        for name, _, old, new in mvs[::step][:per_kind]:
            o, n = circuit.parse(old), circuit.parse(new)
            sets = [(k, n[k]) for k in MOVED_KEYS if k in n and n.get(k) != o.get(k)]
            work.append((kind, name, sets))
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        before = list(ex.map(lambda w: lufs(w[1], []), work))
        after = list(ex.map(lambda w: lufs(w[1], w[2]), work))
    deltas = collections.defaultdict(list)
    for (kind, name, sets), b, a in zip(work, before, after):
        if b is not None and a is not None and b > -70.0:
            deltas[kind].append(a - b)
            if abs(a - b) > 3.0:
                print(f"  {kind}: {name} {b:.1f} -> {a:.1f} LUFS  ({'; '.join(f'{k}={v}' for k, v in sets)})")
    for kind, d in sorted(deltas.items()):
        if not d:
            continue
        print(f"  {kind:16s} {len(d):3d} rendered: loudness moved {statistics.mean(d):+5.1f} dB on average, "
              f"{min(d):+5.1f} .. {max(d):+5.1f}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--write", action="store_true", help="rewrite the files (default: report only)")
    ap.add_argument("--packs", default=PACKS)
    ap.add_argument("--check", type=int, default=0, help="render this many moved presets of each kind before and after")
    ap.add_argument("--measure", action="store_true", help="render the Ladder's and LP 24's moved presets on both filters into circuit_measured.json")
    ap.add_argument("--jobs", type=int, default=12)
    a = ap.parse_args()
    total = 0
    counts = collections.Counter()
    moves = []
    measured = None
    if os.path.isfile(MEASURED_JSON) and not a.measure:
        measured = json.load(open(MEASURED_JSON, encoding="utf-8"))["delta_db"]
        print(f"presets corrected by what they measured ({len(measured)} in {os.path.basename(MEASURED_JSON)})")
    for path in sorted(glob.glob(os.path.join(a.packs, "*.ambientpack"))) + [BUILTINS]:
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
        crlf = "\r\n" in text
        text = text.replace("\r\n", "\n")
        if path == BUILTINS:
            new, n = retrofit_builtins(text, counts, moves, measured)
        else:
            new, n = retrofit_pack(text, retrofit_review.pack_family(text), counts, moves, measured)
        total += n
        if new != text and a.write:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(new.replace("\n", "\r\n") if crlf else new)
    for k, v in sorted(counts.items()):
        print(f"  {k:28s} {v}")
    print(f"{'moved' if a.write else 'would move'} {total} presets" + ("" if a.write else " (add --write)"))
    if a.measure and not a.write:
        measure(moves, a.jobs)
    if a.check and not a.write:
        check(moves, a.check, a.jobs)


if __name__ == "__main__":
    main()
