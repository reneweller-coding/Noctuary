"""The music audit: the faults found by ear in November 2026, counted over an hour of both conductors.

Every measure here began as something Rene heard and nobody had measured (19./20.11.2026): two
conductors that did not hear each other (a semitone, major seventh or tritone between them up to
46 % of the time), a conductor that did not check its own notes (minor seconds 3.9 % of the time),
the second conductor doubling the first one's note, notes struck again a few seconds after they
were let go, and notes released before their own attack had swelled in. Each was found by a
throw-away script; this is the one tool that keeps measuring them, and `--check` is the ctest
`music_audit` that goes red when one of them comes back.

`ambient_brain_audit` steps the engine's conductors for an hour with the parameters exactly as a
render would read them, without audio, and prints every note event with the frequency the tuning
gives it. Everything below is measured in cents of that frequency -- never in MIDI keys, because
the same fault came three times from code that compared keys where the tuning decides what sounds.

Per preset (a preset with `brain_on` off has no conductor and is skipped):

  own_rub           share of the time a conductor sounds during which two of its own notes are a minor second
                    or minor ninth apart (25..150 or 1225..1350 cents), over both conductors
  cross_rub         share of the time both sound during which a note of one and a note of the other, less than
                    two octaves apart, are a semitone, major seventh or tritone apart (interval class 25..150 or
                    550..650 cents)
  doubling          onsets within 25 cents of a pitch the other conductor holds at that moment
  restrike          onsets within 25 cents of a pitch the same conductor let go less than 30 s before
  self_unison       onsets within 25 cents of a pitch the same conductor still holds (another key the tuning plays
                    almost alike: two voices beating against each other, the invariant of plan A4)
  short             notes held for less than the preset's own attack (pack key `attack`, default 6 s): they
                    never reach their level
  changes_per_hour  onsets of both conductors per hour
  silence_s         seconds with no conductor note sounding, after the first note
  one_note          conductors that play a single pitch for the whole hour

Usage:

  music_audit.py --sample 200 --seed 2011          a deterministic sample of the library
  music_audit.py --all [--estimate]                the whole library (times the first 50, prints an estimate;
                                                   --estimate stops there)
  music_audit.py --check Tests/golden/music_audit.json    the stored sample against its bands; exit 1 outside
  music_audit.py --update [--golden FILE]          measure the golden sample afresh and write the file

The audit binary is found next to the build (`--exe` overrides it), the library in Library/Packs
(`--packs`). Workers run at below-normal priority, eight at most, each auditing a list of presets
in one process (`ambient_brain_audit --presets`), since loading the library costs more than an
hour of one preset's conductors. Writes a TSV (one row per preset, `--tsv`) and prints a summary.
Exit codes: 0 fine, 1 a measure outside its band, 2 a usage or run error.
"""
import argparse
import concurrent.futures as cf
import glob
import json
import math
import os
import random
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
GOLDEN = os.path.join(ROOT, "Tests", "golden", "music_audit.json")
EXES = [os.path.join(ROOT, "build", "msvc", "Tools", "render", "Release", "ambient_brain_audit.exe"),
        os.path.join(ROOT, "bin", "msvc", "ambient_brain_audit.exe")]
GOLDEN_SEED = 2011
GOLDEN_N = 200
REST_S = 30.0       # the rest after letting a pitch go, as the conductor's rule (ClusterBrain::resting)
SAME_C = 25.0       # two notes within this many cents are one pitch (as ClusterBrain::againstOther)
COLUMNS = ["name", "pack", "attack", "brain2", "ons", "notes", "restrike", "doubling", "short", "self_unison",
           "own_rub", "cross_rub", "changes_per_hour", "silence_s", "one_note", "sound_s", "both_s"]


def load_library(packs):
    """Every preset with a conductor: name -> (pack file, its key=value pairs)."""
    presets = {}
    for f in sorted(glob.glob(os.path.join(packs, "*.ambientpack"))):
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if line.startswith("#") or "|" not in line:
                    continue
                name, rest = line.rstrip("\r\n").split("|", 1)
                kv = dict(x.split("=", 1) for x in rest.split(";") if "=" in x)
                if kv.get("brain_on", "on").strip().lower() in ("off", "0", "0.0", "false"):
                    continue
                presets.setdefault(name, (os.path.basename(f), kv))
    return presets


def cents(hz):
    """Cents above A 440: the pitch as the tuning plays it."""
    return 1200.0 * math.log2(hz / 440.0)


def own_rubs(cs):
    """True when two of one conductor's notes are a minor second or ninth apart (cents list)."""
    for i in range(len(cs)):
        for j in range(i + 1, len(cs)):
            d = abs(cs[i] - cs[j])
            if 25.0 < d < 150.0 or 1225.0 < d < 1350.0:
                return True
    return False


def cross_rubs(a, b):
    """True when a note of one and a note of the other, under two octaves apart, rub (cents lists)."""
    for x in a:
        for y in b:
            d = abs(x - y)
            if d >= 2400.0:
                continue
            c = math.fmod(d, 1200.0)
            c = min(c, 1200.0 - c)
            if 25.0 < c < 150.0 or 550.0 < c < 650.0:
                return True
    return False


def measure(name, pack, kv, lines, hours):
    """One preset's row from its E lines."""
    attack = float(kv.get("attack", "6") or 6)
    end = hours * 3600.0
    held = {1: {}, 2: {}}      # conductor -> MIDI note -> (on time, cents)
    offat = {1: [], 2: []}     # conductor -> [(off time, cents)] of the last REST_S seconds
    pitches = {1: set(), 2: set()}
    ons = notes = restrike = doubling = short = self_unison = 0
    sound = {1: 0.0, 2: 0.0}
    own = {1: 0.0, 2: 0.0}
    both = cross = silence = 0.0
    state_own = {1: False, 2: False}
    state_cross = False
    first = None
    last_t = 0.0

    def advance(t):
        nonlocal both, cross, silence
        dt = t - last_t
        if dt <= 0.0:
            return
        for w in (1, 2):
            if held[w]:
                sound[w] += dt
                if state_own[w]:
                    own[w] += dt
        if held[1] and held[2]:
            both += dt
            if state_cross:
                cross += dt
        if first is not None and not held[1] and not held[2]:
            silence += dt

    def close(w, note, t):
        nonlocal notes, short
        on_t, c = held[w].pop(note)
        notes += 1
        if t - on_t < attack:
            short += 1
        offat[w].append((t, c))

    for line in lines:
        if not line.startswith("E,"):
            continue
        _, ts, ws, on, note, _vel, _pc, hzs = line.split(",")
        t, w, note, hz = float(ts), int(ws), int(note), float(hzs)
        if w not in (1, 2) or hz <= 0.0:
            continue
        advance(t)
        last_t = t
        c = cents(hz)
        if on == "1":
            ons += 1
            if first is None:
                first = t
            o = 3 - w
            if any(abs(c - x[1]) < SAME_C for x in held[o].values()):
                doubling += 1
            offat[w] = [x for x in offat[w] if t - x[0] < REST_S]
            if any(abs(c - x[1]) < SAME_C for x in offat[w]):
                restrike += 1
            if any(abs(c - x[1]) < SAME_C for x in held[w].values()):
                self_unison += 1
            if note in held[w]:
                close(w, note, t)
            held[w][note] = (t, c)
            pitches[w].add(round(c))
        elif note in held[w]:
            close(w, note, t)
        state_own[1] = own_rubs([x[1] for x in held[1].values()])
        state_own[2] = own_rubs([x[1] for x in held[2].values()])
        state_cross = bool(held[1] and held[2]) and cross_rubs([x[1] for x in held[1].values()],
                                                               [x[1] for x in held[2].values()])
    advance(end)
    snd = sound[1] + sound[2]
    one = sum(1 for w in (1, 2) if len(pitches[w]) == 1)
    return {"name": name, "pack": pack, "attack": attack, "brain2": kv.get("brain2_on", "off"),
            "ons": ons, "notes": notes, "restrike": restrike, "doubling": doubling, "short": short,
            "self_unison": self_unison,
            "own_rub": (own[1] + own[2]) / snd if snd > 0 else 0.0,
            "cross_rub": cross / both if both > 0 else 0.0,
            "changes_per_hour": ons / hours, "silence_s": silence, "one_note": one,
            "sound_s": snd, "both_s": both, "own_s": own[1] + own[2], "cross_s": cross}


def low_priority():
    """Creation flags that start a child at below-normal priority on Windows (0 elsewhere)."""
    return 0x00004000 if os.name == "nt" else 0


def run_chunk(exe, packs, names, library, hours):
    """Audits a list of presets in one ambient_brain_audit process; returns their rows."""
    fd, path = tempfile.mkstemp(suffix=".txt", prefix="music_audit_")
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(names) + "\n")
    try:
        env = dict(os.environ, AMBIENT_PACKS=packs)
        env.pop("AMBIENT_BRAIN_TRACE", None)
        r = subprocess.run([exe, "--presets", path, "--hours", str(hours)], capture_output=True, text=True,
                           encoding="utf-8", errors="replace", env=env, creationflags=low_priority())
    finally:
        os.remove(path)
    if r.returncode != 0:
        raise RuntimeError("ambient_brain_audit failed (%d): %s" % (r.returncode, r.stderr.strip()[:400]))
    rows, cur, lines = [], None, []
    for line in r.stdout.splitlines() + ["# preset <end>"]:
        if line.startswith("# preset "):
            if cur is not None:
                pack, kv = library[cur]
                rows.append(measure(cur, pack, kv, lines, hours))
            cur, lines = line[len("# preset "):], []
            if cur == "<end>":
                break
        else:
            lines.append(line)
    if len(rows) != len(names):
        raise RuntimeError("expected %d presets from ambient_brain_audit, got %d" % (len(names), len(rows)))
    return rows


def audit(exe, packs, names, library, hours, jobs, chunk, progress=False):
    """All the presets named, in chunks over at most `jobs` processes; rows in the order given."""
    chunks = [names[i:i + chunk] for i in range(0, len(names), chunk)]
    out = [None] * len(chunks)
    done = 0
    t0 = time.time()
    with cf.ThreadPoolExecutor(max(1, min(jobs, 8))) as ex:
        futs = {ex.submit(run_chunk, exe, packs, c, library, hours): i for i, c in enumerate(chunks)}
        for f in cf.as_completed(futs):
            out[futs[f]] = f.result()
            done += len(chunks[futs[f]])
            if progress:
                el = time.time() - t0
                print("  %d of %d presets, %.0f s, about %.0f s to go" % (done, len(names), el,
                      el / done * (len(names) - done)), flush=True)
    return [r for c in out for r in c]


def totals(rows):
    """The sums over a set of rows: counts summed, shares weighted by time."""
    snd = sum(r["sound_s"] for r in rows)
    both = sum(r["both_s"] for r in rows)
    hours_total = len(rows)
    return {"presets": len(rows),
            "ons": sum(r["ons"] for r in rows),
            "notes": sum(r["notes"] for r in rows),
            "restrike": sum(r["restrike"] for r in rows),
            "doubling": sum(r["doubling"] for r in rows),
            "short": sum(r["short"] for r in rows),
            "self_unison": sum(r["self_unison"] for r in rows),
            "own_rub": sum(r["own_s"] for r in rows) / snd if snd else 0.0,
            "cross_rub": sum(r["cross_s"] for r in rows) / both if both else 0.0,
            "changes_per_hour": sum(r["changes_per_hour"] for r in rows) / max(1, hours_total),
            "silence_s": sum(r["silence_s"] for r in rows),
            "one_note": sum(r["one_note"] for r in rows),
            "mute": sum(1 for r in rows if r["ons"] == 0)}


def write_tsv(rows, path):
    """One row per preset."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\t".join(COLUMNS) + "\n")
        for r in rows:
            fh.write("\t".join(("%.4f" % r[c]) if isinstance(r[c], float) else str(r[c]) for c in COLUMNS) + "\n")


def summary(rows, worst=10):
    """The readable totals and the worst presets per measure."""
    t = totals(rows)
    ons = max(1, t["ons"])
    notes = max(1, t["notes"])
    print("presets %d, onsets %d, notes %d" % (t["presets"], t["ons"], t["notes"]))
    print("  own_rub    %6.2f %% of the time a conductor sounds" % (100 * t["own_rub"]))
    print("  cross_rub  %6.2f %% of the time both sound" % (100 * t["cross_rub"]))
    print("  doubling   %6d onsets  (%.2f %%)" % (t["doubling"], 100.0 * t["doubling"] / ons))
    print("  restrike   %6d onsets  (%.2f %%)" % (t["restrike"], 100.0 * t["restrike"] / ons))
    print("  short      %6d notes   (%.2f %%)" % (t["short"], 100.0 * t["short"] / notes))
    print("  self_unison %5d onsets  (%.2f %%) within 25 cents of a pitch the same conductor holds" % (
        t["self_unison"], 100.0 * t["self_unison"] / ons))
    print("  changes    %8.1f onsets per hour, on average" % t["changes_per_hour"])
    print("  silence    %8.0f s in all, %.1f s per preset" % (t["silence_s"], t["silence_s"] / max(1, t["presets"])))
    print("  one_note   %6d conductors on a single pitch; %d presets without a note" % (t["one_note"], t["mute"]))
    for key, label in (("own_rub", "own_rub"), ("cross_rub", "cross_rub"), ("doubling", "doubling"),
                       ("restrike", "restrike"), ("short", "short"), ("self_unison", "self_unison"), ("silence_s", "silence_s")):
        w = sorted(rows, key=lambda r: -r[key])[:worst]
        w = [r for r in w if r[key] > 0]
        if not w:
            continue
        print("worst by %s:" % label)
        for r in w:
            v = ("%.1f %%" % (100 * r[key])) if key.endswith("rub") else ("%g" % round(r[key], 1))
            print("  %-34s %-28s %10s   (onsets %d, attack %g)" % (r["name"][:34], r["pack"][:28], v, r["ons"],
                                                                 r["attack"]))
    return t


def bands_for(t):
    """The tolerance bands stored with the golden totals (see the golden file's header for the reasoning)."""
    def act(v):            # activity: two-sided, 15 %
        return [round(v * 0.85, 4), round(v * 1.15, 4)]

    def fault(v, add, mul):  # a fault: no lower bound, room above for harmless drift
        return [0, round(max(v * mul, v + add), 4)]
    return {"ons": act(t["ons"]), "notes": act(t["notes"]), "changes_per_hour": act(t["changes_per_hour"]),
            "restrike": fault(t["restrike"], 15, 2.0), "doubling": fault(t["doubling"], 15, 2.0),
            "short": fault(t["short"], 40, 1.3), "self_unison": fault(t["self_unison"], 50, 1.3), "own_rub": fault(t["own_rub"], 0.003, 2.0),
            "cross_rub": fault(t["cross_rub"], 0.01, 1.5), "silence_s": fault(t["silence_s"], 1800, 1.5),
            "one_note": fault(t["one_note"], 2, 1.0), "mute": fault(t["mute"], 1, 1.0)}


def golden_sample(library):
    """The fixed golden sample: GOLDEN_N names drawn with GOLDEN_SEED from the sorted library."""
    return random.Random(GOLDEN_SEED).sample(sorted(library), min(GOLDEN_N, len(library)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--sample", type=int, help="audit N presets drawn with --seed")
    mode.add_argument("--all", action="store_true", help="audit the whole library")
    mode.add_argument("--check", metavar="GOLDEN", help="compare the golden sample with its bands")
    mode.add_argument("--update", action="store_true", help="measure the golden sample and write --golden")
    ap.add_argument("--seed", type=int, default=1911)
    ap.add_argument("--golden", default=GOLDEN)
    ap.add_argument("--hours", type=float, default=1.0)
    ap.add_argument("--exe", default=None)
    ap.add_argument("--packs", default=PACKS)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--chunk", type=int, default=25, help="presets per audit process")
    ap.add_argument("--tsv", default=os.path.join(ROOT, "build", "msvc", "music_audit", "music_audit.tsv"))
    ap.add_argument("--worst", type=int, default=10)
    ap.add_argument("--estimate", action="store_true", help="with --all: stop after the first 50 and the estimate")
    a = ap.parse_args()
    exe = a.exe or next((e for e in EXES if os.path.exists(e)), None)
    if not exe or not os.path.exists(exe):
        print("music_audit: no ambient_brain_audit (build it, or pass --exe)")
        return 2
    exe, a.packs = os.path.abspath(exe), os.path.abspath(a.packs)
    library = load_library(a.packs)
    if not library:
        print("music_audit: no presets with a conductor in %s" % a.packs)
        return 2
    t0 = time.time()
    if a.check or a.update:
        path = a.check or a.golden
        if a.check:
            with open(path, encoding="utf-8") as fh:
                golden = json.load(fh)
            names = golden["presets"]
            hours = golden.get("hours", 1.0)
            missing = [n for n in names if n not in library]
            if missing:
                print("music_audit: %d golden presets are no longer in the library (first: %s); "
                      "run music_audit.py --update if that was meant" % (len(missing), missing[0]))
                return 1
        else:
            names, hours = golden_sample(library), a.hours
        rows = audit(exe, a.packs, names, library, hours, a.jobs, a.chunk)
        write_tsv(rows, a.tsv)
        t = summary(rows, a.worst)
        print("(%.0f s, TSV %s)" % (time.time() - t0, a.tsv))
        if a.update:
            golden = {
                "_about": ["Golden totals of Tools/library/music_audit.py, checked by the ctest music_audit.",
                           "Written by: python Tools/library/music_audit.py --update (%s). The sample is %d presets with a "
                           "conductor drawn with random.Random(%d) from the sorted library; their names are stored, so "
                           "the check runs the same presets until this file is written again." % (
                               time.strftime("%Y-%m-%d"), len(names), GOLDEN_SEED),
                           "Bands: the conductors are deterministic per preset, so the totals come back exactly on the "
                           "same code. Activity (ons, notes, changes_per_hour) may move 15 % either way: a change to "
                           "the conductor's pace is not a fault. Faults have no lower bound (fewer is better: write the "
                           "file again to tighten) and an upper one at twice the count or share plus a small floor "
                           "(restrike and doubling +15, short x1.3 or +40, self_unison x1.3 or +50, own_rub +0.3 points, cross_rub x1.5 or +1 "
                           "point, silence x1.5 or +30 min, one_note +2, mute +1). A rule switched off moves its "
                           "measure far beyond that: againstOther switched off (20.11.2026) took doubling from 7 to 501 and cross_rub from 1.05 % to 8.97 % of the time."],
                "hours": hours, "seed": GOLDEN_SEED, "presets": names, "totals": t, "bands": bands_for(t)}
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
            with open(path, "w", encoding="utf-8", newline="\n") as fh:
                json.dump(golden, fh, indent=1)
                fh.write("\n")
            print("music_audit: wrote %s" % path)
            return 0
        bad = 0
        for key, (lo, hi) in golden["bands"].items():
            v = t[key]
            g = golden["totals"].get(key)
            if v < lo or v > hi:
                print("music_audit: FAIL %s = %s outside [%s, %s] (golden %s)" % (key, v, lo, hi, g))
                bad += 1
            elif g is not None and lo == 0 and v < g:
                print("music_audit: %s = %s, better than the golden %s (--update tightens the band)" % (key, v, g))
        if bad:
            print("music_audit: %d measure(s) outside their bands" % bad)
            return 1
        print("music_audit: all measures inside their bands")
        return 0
    names = sorted(library)
    if a.sample:
        names = random.Random(a.seed).sample(names, min(a.sample, len(names)))
        rows = audit(exe, a.packs, names, library, a.hours, a.jobs, a.chunk)
    else:
        first = names[:50]
        rows = audit(exe, a.packs, first, library, a.hours, a.jobs, max(1, min(a.chunk, 50 // max(1, a.jobs) + 1)))
        el = time.time() - t0
        print("first 50 presets in %.1f s: the whole library (%d) should take about %.0f min" % (
            el, len(names), el / 50 * len(names) / 60), flush=True)
        if a.estimate:
            return 0
        rows += audit(exe, a.packs, names[50:], library, a.hours, a.jobs, a.chunk, progress=True)
    write_tsv(rows, a.tsv)
    summary(rows, a.worst)
    print("(%.0f s, TSV %s)" % (time.time() - t0, a.tsv))
    return 0


if __name__ == "__main__":
    sys.exit(main())
