"""The ear's audit (plan B1, step 1, 20.11.2026): today's vetoes and the new tension, side by side, decision by decision.

The conductor strikes candidates with four vetoes that grew one by one -- againstOther (the other conductor's pitch,
and from Consonance 0.5 a semitone, major seventh or tritone to it), rubsOwn (from 0.5, a minor second or ninth from
its own voices), stepRubs (from 0.3, the overlap of a step by a minor second or ninth is cut) and the ban on a unison
(holdsPitch). Plan B1 wants one measure in their place, the tension of a note against what sounds (noteTension() in
ClusterBrain.h), and one threshold on it that Consonance moves. Step 1 changes nothing: `ambient_brain_audit` with
AMBIENT_BRAIN_EAR set prints, for every decision the conductors take, every candidate with the rules that struck it and
its tension (ClusterBrain::M_Ear); this script runs it over a list of presets and asks how far one threshold would
reproduce the four rules, where it differs, and what the conductor would have chosen under the tension alone.

Usage:

  ear_audit.py --make-sample 500 --seed 2011 --out FILE    a stratified sample of the library, one name a line
  ear_audit.py --run FILE --cache DIR [--hours 1] [--jobs 2] [--chunk 5]
                                                           audit the presets, one summary JSON per preset in DIR;
                                                           presets with a summary are skipped (resumable)
  ear_audit.py --time FILE [--hours 1]                     audit the first five presets and print the time each took
  ear_audit.py --analyse DIR --out REPORT.json             aggregate the summaries into one JSON
  ear_audit.py --tables REPORT.json                        the report's tables: (a) the rules against one threshold,
                                                           (b) tension by interval and register, (c) what would change

The sample is drawn per stratum of scale, Autoplay mode (Free, Chords), Consonance (under 0.5, 0.5 .. 0.7, 0.7 .. 0.9,
0.9 and up) and whether Brain 2 plays: at least three from every stratum that has them (all of a small one), the rest in
proportion, and then at least three from every pack. Workers run at below-normal priority. Nothing here changes a preset
or a golden file. Standard library only.
"""
import argparse
import collections
import glob
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
EXE = os.path.join(ROOT, "build", "msvc", "Tools", "render", "Release", "ambient_brain_audit.exe")

# ClusterBrain::Ear* bits
SOUNDING, RESTING, OTHER_UNISON, OTHER_RUB, OWN_RUB, OWN_PITCH = 1, 2, 4, 8, 16, 32
RULES, HEARD, FROM_PITCH, LEAD, PERFECT, CONSONANT = 64, 128, 256, 512, 1024, 2048
# rules that are not about how a candidate sounds against what sounds: a candidate they strike is out either way
STRUCTURAL = SOUNDING | RESTING | RULES | HEARD | FROM_PITCH | LEAD
KINDS = ["free", "choose", "trial", "pivot", "dejavu"]
THETAS = [round(3.0 + 0.1 * k, 1) for k in range(51)]      # 3.0 .. 8.0
TBIN = 0.05                                                 # histogram bin of the tension
TBINS = 260                                                 # 0 .. 13


def load_library(packs):
    """Every preset with a conductor: name -> (pack file, key=value pairs)."""
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


def cons_bin(c):
    """Consonance in four bins: what the vetoes switch at (0.5) and the library's spread above it."""
    return "<0.5" if c < 0.5 else ("0.5-0.7" if c < 0.7 else ("0.7-0.9" if c < 0.9 else ">=0.9"))


def stratum(kv):
    """The stratum a preset is drawn from."""
    return (kv.get("scale", "JI 7-limit"), kv.get("auto_mode", "Free"),
            cons_bin(float(kv.get("brain_consonance", "0.7") or 0.7)), kv.get("brain2_on", "off"))


def make_sample(packs, n, seed):
    """A stratified sample of n preset names (see the module's description), sorted by pack and name."""
    lib = load_library(packs)
    rng = random.Random(seed)
    by = collections.defaultdict(list)
    for name in sorted(lib):
        by[stratum(lib[name][1])].append(name)
    for v in by.values():
        rng.shuffle(v)
    picked = []
    for k in sorted(by):                       # three from every stratum (all of a small one)
        picked += by[k][:3]
    rest = [k for k in sorted(by) if len(by[k]) > 3]
    total = sum(len(by[k]) - 3 for k in rest)
    room = max(0, n - len(picked) - 40)        # keep forty for the packs below
    for k in rest:                             # the rest in proportion to the stratum's size
        share = int(round(room * (len(by[k]) - 3) / total)) if total else 0
        picked += by[k][3:3 + share]
    chosen = set(picked)
    packs_seen = collections.Counter(lib[x][0] for x in chosen)
    names = sorted(lib)
    rng.shuffle(names)
    for name in names:                         # at least three from every pack
        p = lib[name][0]
        if packs_seen[p] < 3 and name not in chosen:
            chosen.add(name)
            packs_seen[p] += 1
    extra = [x for x in names if x not in chosen]
    while len(chosen) < n and extra:
        chosen.add(extra.pop())
    while len(chosen) > n:                     # trim from the largest strata, never below three
        sizes = collections.Counter(stratum(lib[x][1]) for x in chosen)
        big = max(sizes, key=lambda k: sizes[k])
        victim = sorted(x for x in chosen if stratum(lib[x][1]) == big)[-1]
        chosen.discard(victim)
    return sorted(chosen, key=lambda x: (lib[x][0], x))


def low_priority():
    """Creation flags that start a child at below-normal priority on Windows (0 elsewhere)."""
    return 0x00004000 if os.name == "nt" else 0


def safe(name):
    """A file name for a preset's summary."""
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", name)[:80] + "_%08x" % (hash_name(name))


def hash_name(name):
    """A stable 32-bit hash of a name (FNV-1a), so that two names that differ only in odd characters do not collide."""
    h = 0x811C9DC5
    for b in name.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def cents(a, b):
    """The interval from b up to a in cents (negative when a is lower)."""
    return 1200.0 * math.log2(a / b)


def octave_of(hz):
    """The octave a frequency lies in, C1 = 1 (32.7 Hz) .. C7."""
    return int(math.floor(math.log2(hz / 32.703195662574829))) + 1


def ic_bucket(c):
    """An interval in cents, as a bucket of 25 cents up to two octaves; '>2400' above."""
    c = abs(c)
    return ">2400" if c >= 2400.0 else str(int(round(c / 25.0)) * 25)


class Summary:
    """One preset's aggregated ear log."""

    def __init__(self, name, pack, kv, scale):
        self.d = {"name": name, "pack": pack, "scale": scale, "mode": kv.get("auto_mode", "Free"),
                  "consonance": float(kv.get("brain_consonance", "0.7") or 0.7),
                  "layers": float(kv.get("brain_layers", "0") or 0), "brain2": kv.get("brain2_on", "off"),
                  "decisions": collections.Counter(), "hist": {}, "groups": collections.Counter(),
                  "rank": {}, "cf": {}, "step_groups": collections.Counter(), "cons_seen": collections.Counter(),
                  "pool": 0, "evaluations": 0}
        self.trials = []   # Smooth's trials of one exchange, until the exchange is over

    def flush_trials(self):
        """Smooth tried every voice for one exchange: (c) for the exchange as a whole. The winner is the trial whose
        score, divided by 1 + Smooth x travel in octaves, is highest -- as ClusterBrain::updateChords decides -- taken
        on the scores before their noise, for today's rules and for the tension alone alike."""
        trials, self.trials = self.trials, []
        if not trials or trials[0][0]["cons"] < 0.5:
            return
        row = self.d["cf"].setdefault("exchange", [[0, 0.0, 0, 0] for _ in THETAS])
        allowed = STRUCTURAL | OTHER_UNISON | OWN_PITCH | (1 << 20)

        def winner(pools):
            best, bq = None, -1.0
            for head, pool in pools:
                for note, v in pool:
                    q = v / (1.0 + head["smooth"] * abs(note - head["from"]) / 12.0)
                    if v > 0.0 and q > bq:
                        best, bq = (head["from"], note), q
            return best

        def argmax(pool):
            b = max(pool, key=lambda x: x[1], default=None)
            return [b] if b is not None and b[1] > 0.0 else []

        old = winner([(h, argmax([(c["note"], c["weight"]) for c in cs if c["weight"] > 0.0])) for h, cs in trials])
        for i, th in enumerate(THETAS):
            new = winner([(h, argmax([(c["note"], c["would"]) for c in cs
                                      if c["would"] > 0.0 and not (new_struck(c, th) & allowed)])) for h, cs in trials])
            r = row[i]
            r[0] += 1
            r[1] += 1.0 if new != old else 0.0
            r[3] += 1 if old is not None and new is None else 0

    def hist_add(self, key, t):
        """One tension into the histogram `key`."""
        h = self.d["hist"].setdefault(key, [0] * TBINS)
        h[min(TBINS - 1, max(0, int(t / TBIN)))] += 1

    def decision(self, head, cands):
        """One D line and its C lines."""
        kind = KINDS[head["kind"]]
        cons = head["cons"]
        cb = "hi" if cons >= 0.5 else "lo"
        d = self.d
        d["decisions"][kind] += 1
        d["cons_seen"][cons_bin(cons)] += 1
        # (a) and (b): candidates of the draws, outside the structural rules
        if kind in ("free", "choose", "trial"):
            for c in cands:
                d["evaluations"] += 1
                if c["struck"] & STRUCTURAL:
                    continue
                d["pool"] += 1
                s = c["struck"]
                tc = max(c["own"], c["other"])
                if head["otherCount"]:
                    self.hist_add("other|%d|%s" % (1 if s & OTHER_RUB else 0, cb), c["other"])
                if head["ownCount"]:
                    self.hist_add("own|%d|%s" % (1 if s & OWN_RUB else 0, cb), c["own"])
                if head["otherCount"] or head["ownCount"]:
                    self.hist_add("unison|%d|%s" % (1 if s & (OTHER_UNISON | OWN_PITCH) else 0, cb), tc)
                    self.hist_add("any|%d|%s" % (1 if s & (OTHER_RUB | OWN_RUB) else 0, cb), tc)
                if cb == "hi":
                    for rule, bit, t, at in (("other", OTHER_RUB, c["other"], c["otherAt"]),
                                             ("own", OWN_RUB, c["own"], c["ownAt"])):
                        if at <= 0.0:
                            continue
                        lo = min(c["hz"], at)
                        key = "%s|%d|%s|%d|%d" % (rule, 1 if s & bit else 0, ic_bucket(cents(c["hz"], at)),
                                                  octave_of(lo), int(t * 10))
                        d["groups"][key] += 1
                # (b): the tension of each worst pair by scale, interval and register
                for t, at in ((c["own"], c["ownAt"]), (c["other"], c["otherAt"])):
                    if at <= 0.0:
                        continue
                    key = "%s|%d" % (ic_bucket(cents(c["hz"], at)), octave_of(min(c["hz"], at)))
                    r = d["rank"].setdefault(key, [0.0, 0])
                    r[0] += t
                    r[1] += 1
        if kind == "trial":
            self.trials.append((head, cands))
        # (c): what one threshold alone would have chosen, Consonance from 0.5 (below it the vetoes are off)
        if cb == "hi" and kind != "trial":
            row = d["cf"].setdefault(kind, [[0, 0.0, 0, 0] for _ in THETAS])
            for i, th in enumerate(THETAS):
                changed, certain, empty = counterfactual(kind, head, cands, th)
                r = row[i]
                r[0] += 1
                r[1] += changed
                r[2] += certain
                r[3] += empty

    def step(self, rubs, total, cons, hz_a, hz_l):
        """One S line."""
        cb = "hi" if cons >= 0.3 else "lo"
        self.hist_add("step|%d|%s" % (1 if rubs else 0, cb), total)
        if cb == "hi" and hz_a > 0 and hz_l > 0:
            key = "step|%d|%s|%d|%d" % (1 if rubs else 0, ic_bucket(cents(hz_a, hz_l)), octave_of(min(hz_a, hz_l)),
                                        int(total * 10))
            self.d["step_groups"][key] += 1


def new_struck(c, th):
    """The candidate's strikes with againstOther's intervals and rubsOwn replaced by one threshold on the tension; the
    unison rules and the structural ones stay."""
    s = c["struck"] & ~(OTHER_RUB | OWN_RUB)
    if max(c["own"], c["other"]) > th:
        s |= 1 << 20
    return s


def counterfactual(kind, head, cands, th):
    """(changed, certain, empty) for one decision under threshold th: changed is 0..1 (Free: the total variation
    between the two draws, the share of draws that would land elsewhere; else 0 or 1), certain is 1 when the note
    chosen would itself be struck, empty is 1 when the tension leaves no candidate where the vetoes left one."""
    chosen = head["chosen"]
    if kind == "free":
        old = {c["note"]: c["weight"] for c in cands if c["weight"] > 0.0}
        new = {c["note"]: c["would"] for c in cands
               if c["would"] > 0.0 and not (new_struck(c, th) & (STRUCTURAL | OTHER_UNISON | OWN_PITCH | (1 << 20)))}
        so, sn = sum(old.values()), sum(new.values())
        if so <= 0.0 and sn <= 0.0:
            return 0.0, 0, 0
        if so <= 0.0 or sn <= 0.0:
            return 1.0, 1 if so > 0 else 0, 1 if sn <= 0.0 else 0
        tv = 0.5 * sum(abs(old.get(k, 0.0) / so - new.get(k, 0.0) / sn) for k in set(old) | set(new))
        cert = 1 if chosen >= 0 and chosen not in new else 0
        return tv, cert, 0
    if kind == "choose":
        def best(pool):
            b, bs = -1, -1.0
            for n_, v in pool:
                if v > bs:
                    b, bs = n_, v
            return b
        old = best((c["note"], c["weight"]) for c in cands if c["weight"] > 0.0)
        newp = [(c["note"], c["would"]) for c in cands
                if c["would"] > 0.0 and not (new_struck(c, th) & (STRUCTURAL | OTHER_UNISON | OWN_PITCH | (1 << 20)))]
        new = best(newp)
        cert = 1 if chosen >= 0 and chosen not in {n_ for n_, _ in newp} else 0
        return (1.0 if old != new else 0.0), cert, (1 if old >= 0 and new < 0 else 0)
    if kind == "pivot":
        order = sorted(cands, key=lambda c: c["order"])
        new = -1
        for tier in (PERFECT, CONSONANT):
            for c in order:
                s = new_struck(c, th)
                if c["struck"] & tier and not (s & (SOUNDING | OWN_PITCH | RULES | OTHER_UNISON | (1 << 20))):
                    new = c["note"]
                    break
            if new >= 0:
                break
        return (1.0 if new != chosen else 0.0), (1 if chosen >= 0 and chosen != new else 0), \
            (1 if chosen >= 0 and new < 0 else 0)
    if kind == "dejavu":
        c = cands[0]
        taken_old = chosen == c["note"]
        s = new_struck(c, th)
        taken_new = not (s & (SOUNDING | OWN_PITCH | FROM_PITCH | RULES | OTHER_UNISON | (1 << 20)))
        return (1.0 if taken_old != taken_new else 0.0), (1 if taken_old and not taken_new else 0), 0
    return 0.0, 0, 0


def floats(field):
    """A ';'-separated list of numbers."""
    return [float(x) for x in field.split(";") if x]


def parse_stream(stream, library, on_preset):
    """Reads ambient_brain_audit's output line by line and hands each finished preset's Summary to on_preset."""
    cur = None
    head = None
    cands = []
    scale = "?"
    last_cons = {1: 0.7, 2: 0.7}
    step_seen = {}   # conductor -> (arriving note, the leaving notes already counted beside it)

    def flush():
        nonlocal head, cands
        if cur is not None and head is not None:
            cur.decision(head, cands)
        head, cands = None, []

    for line in stream:
        line = line.rstrip("\r\n")
        if not line:
            continue
        tag = line[0]
        if tag == "C" and head is not None:
            f = line.split(",")
            cands.append({"note": int(f[1]), "hz": float(f[2]), "struck": int(f[3]), "weight": float(f[4]),
                          "would": float(f[5]), "own": float(f[6]), "left": float(f[7]), "other": float(f[8]),
                          "r": float(f[9]), "dis": float(f[10]), "ent": float(f[11]), "order": int(f[12]),
                          "ownAt": float(f[13]), "otherAt": float(f[14])})
            continue
        flush()
        if cur is not None and not (tag == "D" and line.split(",", 5)[4] == "2"):
            cur.flush_trials()
        if line.startswith("# preset "):
            if cur is not None:
                on_preset(cur)
            name = line[len("# preset "):]
            pack, kv = library.get(name, ("?", {}))
            cur = Summary(name, pack, kv, "?")
        elif line.startswith("# ear scale "):
            if cur is not None:
                cur.d["scale"] = line[len("# ear scale "):]
        elif tag == "D":
            f = line.split(",")
            head = {"who": int(f[3]), "kind": int(f[4]), "from": int(f[5]), "chosen": int(f[6]), "best": float(f[7]),
                    "root": int(f[8]), "cons": float(f[9]), "mode": int(f[10]), "layers": float(f[11]),
                    "smooth": float(f[12]), "ownCount": len(floats(f[13])), "leftCount": len(floats(f[14])),
                    "otherCount": len(floats(f[15]))}
            last_cons[head["who"]] = head["cons"]
        elif tag == "S" and cur is not None:
            f = line.split(",")
            who = int(f[2])
            # Free mode asks stepRubs() every tick for every voice that waits; count each pair once per arriving note
            arr, pair = int(f[3]), (int(f[3]), int(f[4]))
            if step_seen.get(who, (None, set()))[0] != arr:
                step_seen[who] = (arr, set())
            if pair in step_seen[who][1]:
                continue
            step_seen[who][1].add(pair)
            cur.step(f[5] == "1", float(f[9]), last_cons.get(who, 0.7), float(f[10]), float(f[11]))
    flush()
    if cur is not None:
        cur.flush_trials()
        on_preset(cur)


def run_chunk(exe, packs, names, library, hours, cache):
    """Audits a list of presets in one process, streaming, and writes a summary per preset."""
    fd, path = tempfile.mkstemp(suffix=".txt", prefix="b1_ear_")
    with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(names) + "\n")
    done = []

    def store(s):
        out = os.path.join(cache, safe(s.d["name"]) + ".json")
        tmp = out + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(s.d, fh)
        os.replace(tmp, out)
        done.append(s.d["name"])

    try:
        env = dict(os.environ, AMBIENT_PACKS=packs, AMBIENT_BRAIN_EAR="1")
        env.pop("AMBIENT_BRAIN_TRACE", None)
        env.pop("AMBIENT_BRAIN_OFFWHY", None)
        p = subprocess.Popen([exe, "--presets", path, "--hours", str(hours)], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace", env=env,
                             creationflags=low_priority(), bufsize=1 << 20)
        parse_stream(p.stdout, library, store)
        err = p.stderr.read()
        if p.wait() != 0:
            raise RuntimeError("ambient_brain_audit failed: %s" % err.strip()[:400])
    finally:
        os.remove(path)
    return done


def lower_own_priority():
    """This script itself at below-normal priority too (Windows)."""
    if os.name == "nt":
        import ctypes
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x00004000)


def run(listfile, cache, hours, jobs, chunk, exe, packs):
    """Audits every preset of the list that has no summary yet, `jobs` processes at a time."""
    import concurrent.futures as cf
    os.makedirs(cache, exist_ok=True)
    library = load_library(packs)
    names = [x.strip() for x in open(listfile, encoding="utf-8") if x.strip()]
    todo = [x for x in names if not os.path.exists(os.path.join(cache, safe(x) + ".json"))]
    print("ear_audit: %d presets, %d to do, %d jobs" % (len(names), len(todo), jobs), flush=True)
    chunks = [todo[i:i + chunk] for i in range(0, len(todo), chunk)]
    t0 = time.time()
    n = 0
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = [ex.submit(run_chunk, exe, packs, c, library, hours, cache) for c in chunks]
        for f in cf.as_completed(futs):
            n += len(f.result())
            el = time.time() - t0
            print("  %d/%d presets, %.0f s, about %.0f s left" % (n, len(todo), el, el / n * (len(todo) - n)),
                  flush=True)


def analyse(cache, out):
    """Aggregates the summaries of a cache into one JSON (the report's numbers) and prints the main tables."""
    agg = {"presets": 0, "decisions": collections.Counter(), "hist": {}, "groups": collections.Counter(),
           "step_groups": collections.Counter(), "rank": {}, "cf": {}, "cf_by": {}, "pool": 0, "evaluations": 0,
           "cons_seen": collections.Counter(), "meta": []}
    for f in sorted(glob.glob(os.path.join(cache, "*.json"))):
        d = json.load(open(f, encoding="utf-8"))
        agg["presets"] += 1
        agg["meta"].append({k: d[k] for k in ("name", "pack", "scale", "mode", "consonance", "layers", "brain2")})
        agg["decisions"].update(d["decisions"])
        agg["cons_seen"].update(d["cons_seen"])
        agg["pool"] += d["pool"]
        agg["evaluations"] += d["evaluations"]
        for k, h in d["hist"].items():
            a = agg["hist"].setdefault(k, [0] * TBINS)
            for i, v in enumerate(h):
                a[i] += v
        agg["groups"].update(d["groups"])
        agg["step_groups"].update(d["step_groups"])
        for k, (s, n) in d["rank"].items():
            r = agg["rank"].setdefault(d["scale"] + "|" + k, [0.0, 0])
            r[0] += s
            r[1] += n
        lay = "Layers>=0.9" if d["layers"] >= 0.9 else "Layers<0.9"
        for kind, rows in d["cf"].items():
            for key in (kind, "%s|%s" % (kind, d["mode"]), "%s|%s" % (kind, d["scale"]), "%s|%s" % (kind, lay)):
                a = agg["cf"].setdefault(key, [[0, 0.0, 0, 0] for _ in THETAS])
                for i, r in enumerate(rows):
                    for j in range(4):
                        a[i][j] += r[j]
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(agg, fh)
    print("ear_audit: %d presets, decisions %s" % (agg["presets"], dict(agg["decisions"])))


NAMES12 = {0: "P1", 100: "m2", 200: "M2", 300: "m3", 400: "M3", 500: "P4", 600: "TT", 700: "P5", 800: "m6",
           900: "M6", 1000: "m7", 1100: "M7", 1200: "P8", 1300: "m9", 1400: "M9", 1500: "m10", 1600: "M10",
           1700: "P11", 1800: "TT+8", 1900: "P12", 2000: "m13", 2100: "M13", 2200: "m14", 2300: "M14"}


def interval_name(bucket):
    """A name for a 25-cent bucket: the 12-TET interval within 25 cents, else the cents."""
    if bucket == ">2400":
        return ">2 oct"
    c = int(bucket)
    near = int(round(c / 100.0)) * 100
    return NAMES12.get(near, str(c)) + ("" if abs(c - near) == 0 else "(%+d)" % (c - near))


def agreement(agg, rule, th, cb="hi"):
    """For one rule at threshold th: (n, agree, struck but at or under th, not struck but over th, struck)."""
    hs = agg["hist"].get("%s|1|%s" % (rule, cb), [0] * TBINS)
    hn = agg["hist"].get("%s|0|%s" % (rule, cb), [0] * TBINS)
    cut = int(round(th / TBIN))
    s_lo, s_hi = sum(hs[:cut]), sum(hs[cut:])
    n_lo, n_hi = sum(hn[:cut]), sum(hn[cut:])
    n = s_lo + s_hi + n_lo + n_hi
    return n, s_hi + n_lo, s_lo, n_hi, s_lo + s_hi


def best_threshold(agg, rule, cb="hi"):
    """The threshold (0.05 steps) at which the rule's verdicts and the tension agree best (balanced accuracy)."""
    best, bt = -1.0, 0.0
    for k in range(40, 200):
        th = k * TBIN
        n, ag, s_lo, n_hi, s = agreement(agg, rule, th, cb)
        if not s or n == s:
            continue
        bal = 0.5 * ((s - s_lo) / s + (n - s - n_hi) / (n - s))
        if bal > best:
            best, bt = bal, th
    return bt, best


def tables(report):
    """Prints the tables of the report from an --analyse JSON."""
    agg = json.load(open(report, encoding="utf-8"))
    print("presets %d, decisions %s, candidates %d, outside the structural rules %d" %
          (agg["presets"], agg["decisions"], agg["evaluations"], agg["pool"]))
    print("decisions by Consonance: %s" % agg["cons_seen"])
    print("\n(a) one threshold against each rule (Consonance >= 0.5; stepRubs >= 0.3)")
    th_any, bal = best_threshold(agg, "any")
    print("threshold best for againstOther's intervals and rubsOwn together: %.2f (balanced accuracy %.3f)" %
          (th_any, bal))
    print("rule | best own threshold | balanced | at the common threshold: n | agree | struck, tension fine | "
          "tension over, not struck | struck")
    for rule in ("other", "own", "any", "unison", "step"):
        th, b = best_threshold(agg, rule)
        n, ag, s_lo, n_hi, s = agreement(agg, rule, th_any)
        print("%-6s | %.2f | %.3f | %d | %.1f %% | %d | %d | %d" % (rule, th, b, n, 100.0 * ag / max(1, n), s_lo,
                                                                     n_hi, s))
    for rule in ("other", "own", "step"):
        src = agg["step_groups"] if rule == "step" else agg["groups"]
        fine, over = collections.Counter(), collections.Counter()
        for k, v in src.items():
            r, st, ic, octv, tb = k.split("|")
            if r != rule:
                continue
            t = int(tb) / 10.0
            if st == "1" and t < th_any:
                fine["%s C%s" % (interval_name(ic), octv)] += v
            if st == "0" and t >= th_any:
                over["%s C%s" % (interval_name(ic), octv)] += v
        print("\n%s: struck by the rule, tension at or under %.2f -- largest groups" % (rule, th_any))
        for k, v in fine.most_common(10):
            print("  %-20s %d" % (k, v))
        print("%s: tension over %.2f, not struck -- largest groups" % (rule, th_any))
        for k, v in over.most_common(10):
            print("  %-20s %d" % (k, v))
    print("\n(b) mean tension of the worst pair by interval and register")
    by_scale = collections.defaultdict(dict)
    for k, (s, n) in agg["rank"].items():
        sc, ic, octv = k.split("|")
        by_scale[sc][(ic, octv)] = (s, n)
    for sc in sorted(by_scale):
        rows = collections.defaultdict(dict)
        for (ic, octv), (s, n) in by_scale[sc].items():
            if ic == ">2400" or int(ic) > 1200 or n < 20:
                continue
            rows[ic][int(octv)] = s / n
        print("\n" + sc)
        octs = sorted({o for r in rows.values() for o in r})
        print("  interval   " + " ".join("   C%d" % o for o in octs))
        for ic in sorted(rows, key=int):
            print("  %-10s " % interval_name(ic) + " ".join(
                ("%5.2f" % rows[ic][o]) if o in rows[ic] else "    ." for o in octs))
    print("\n(c) decisions that would change under the tension alone (Consonance >= 0.5), at %.1f" % th_any)
    i = min(range(len(THETAS)), key=lambda k: abs(THETAS[k] - th_any))
    for key in sorted(agg["cf"]):
        n, ch, cert, emp = agg["cf"][key][i]
        print("  %-36s %6d decisions, %7.1f change (%5.1f %%), %5d certain, %4d empty" %
              (key, n, ch, 100.0 * ch / max(1, n), cert, emp))
    kinds = ("free", "exchange", "choose", "pivot", "dejavu")
    print("\n(c) by threshold: change in per cent of the decisions of each kind, and of all")
    print("  threshold " + " ".join("%9s" % k for k in kinds) + "       all")
    for i, th in enumerate(THETAS):
        if i % 5 and abs(th - th_any) > 1e-6:
            continue
        tot = [0, 0.0]
        cells = []
        for key in kinds:
            r = agg["cf"].get(key)
            if r:
                tot[0] += r[i][0]
                tot[1] += r[i][1]
                cells.append("%8.1f%%" % (100.0 * r[i][1] / max(1, r[i][0])))
            else:
                cells.append("%9s" % "-")
        print("  %9.1f " % th + " ".join(cells) + "  %7.1f%% of %d" % (100.0 * tot[1] / max(1, tot[0]), tot[0]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--make-sample", type=int)
    ap.add_argument("--seed", type=int, default=2011)
    ap.add_argument("--out")
    ap.add_argument("--run")
    ap.add_argument("--time")
    ap.add_argument("--cache")
    ap.add_argument("--analyse")
    ap.add_argument("--tables", help="print the tables of an --analyse JSON")
    ap.add_argument("--hours", type=float, default=1.0)
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--chunk", type=int, default=5)
    ap.add_argument("--exe", default=EXE)
    ap.add_argument("--packs", default=PACKS)
    a = ap.parse_args()
    lower_own_priority()
    if a.make_sample:
        names = make_sample(a.packs, a.make_sample, a.seed)
        with open(a.out, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("\n".join(names) + "\n")
        print("ear_audit: %d presets written to %s" % (len(names), a.out))
        return 0
    if a.time:
        library = load_library(a.packs)
        names = [x.strip() for x in open(a.time, encoding="utf-8") if x.strip()][:5]
        cache = tempfile.mkdtemp(prefix="b1_ear_time_")
        for n in names:
            t0 = time.time()
            run_chunk(a.exe, a.packs, [n], library, a.hours, cache)
            print("  %-40s %.1f s" % (n, time.time() - t0), flush=True)
        return 0
    if a.run:
        run(a.run, a.cache, a.hours, a.jobs, a.chunk, a.exe, a.packs)
        return 0
    if a.analyse:
        analyse(a.analyse, a.out)
        return 0
    if a.tables:
        tables(a.tables)
        return 0
    ap.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main())
