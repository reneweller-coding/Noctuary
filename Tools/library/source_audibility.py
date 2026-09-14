"""Whether every source of a preset can be heard -- measured, and set right where it cannot.

Rene, 14.09.2026: "Wenn wir mehrere Sources haben, ob man diese dann auch wirklich alle hoert. Wenn
dem nicht so ist, dann passe die Lautstaerken der Sources bitte an." 13722 of the library's 14336
pack presets play two sources or more, and their levels were drawn from ranges one at a time: nothing
ever asked whether a texture at 0.3 is heard under a wavetable at 1.0.

How it is asked. Each active source of a preset is rendered on its own -- the preset as it is, the
conductor, the filter, the rooms and the master untouched, every other source's level at zero -- and
the layers that sound whatever the sources do, the Foundation, the Strike and the Air, are rendered
once apart, all four sources at zero. In the solo renders those layers are silenced: a first version
left them in, and every source measured the Foundation's bass as its own -- the three sources of Amber
Vigil each carried 22 dB at 63 Hz, where without the bass the quietest of them carries -15.

A render plays through the preset's entrance -- the slowest source's delay and rise, and SETTLE seconds
for the rooms to fill -- and then measures WINDOW seconds in ten octave bands, the powers of the two
channels averaged (ambient_render --skip --measure --bands, many presets to a process with --batch).
The voice sums its sources before its filter and everything after it, so what a source contributes to
a band of the whole is its own energy there against the sum of the parts; `check` renders whole presets
and says how far the sum of the parts lies from them.

A source is BURIED when it carries under BURIED_SHARE of the preset's loudness (the bands K-weighted)
and in none of the bands that matter -- within MATTER_DB of the loudest -- as much as OWN_SHARE of the
energy. That is a masking proxy, not a psychoacoustic model: a soft source with a band of its own is
heard, one that shares every band with louder parts is not. plan_fix says how one is brought forward.

    python Tools/library/source_audibility.py measure [--jobs 20] [--limit N]
    python Tools/library/source_audibility.py check [--limit 24]
    python Tools/library/source_audibility.py report
    python Tools/library/source_audibility.py fix [--dry-run]
    python Tools/library/source_audibility.py verify [--limit 60]
    python Tools/library/source_audibility.py carry      then measure, fix and verify once more

measure appends every render to build/library-work/source_audibility.parts.jsonl as it finishes, and
started again renders only what is missing. A part belongs to the preset line it was rendered from: a
preset whose line has changed since -- fix changes lines -- counts as unmeasured, so report and fix only
ever see presets as they are now, and a second fix cannot move a preset twice. fix rewrites the source
levels of the presets that need it, and their master gain so the loudness stays, in
Library/Packs/*.ambientpack and logs every change to build/library-work/source_audibility_fix.json;
verify renders a sample of the changed presets again and compares what they measure with what the fix
expected. On 14.09.2026 the first fix brought 108 of 120 sampled presets over the bar, the rest fell up to
6 dB short of the plan (noise, late entrances, sources at full level under the Vector); so a second pass:
carry moves what the fix left alone into the new lines, measure renders the sources that moved, and fix
runs again with no source giving way more than 3 dB from where it began. The library's descriptors and
loudness are NOT measured again afterwards (Rene: "da
verzichten wir erst mal drauf"). AMBIENT_RENDER names a copy of the renderer, so a rebuild during a run
of hours does not pull it away.
"""
import argparse
import concurrent.futures
import glob
import json
import math
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RENDER = os.environ.get("AMBIENT_RENDER") or os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
PACKS = os.path.join(ROOT, "Library", "Packs")
WORK = os.path.join(ROOT, "build", "library-work")
PARTS = os.path.join(WORK, "source_audibility.parts.jsonl")
OUT = os.path.join(WORK, "source_audibility.json")
FIXLOG = os.path.join(WORK, "source_audibility_fix.json")
PASS1 = os.path.join(WORK, "source_audibility_fix.pass1.json")   # the first fix's log, once `carry` has run
LOW = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}

LEVEL_KEY = {1: "osc_level", 2: "src2_level", 3: "src3_level", 4: "src4_level"}
LEVEL_DEFAULT = {1: 1.0, 2: 0.5, 3: 0.5, 4: 0.5}
RISE_DEFAULT = 1.0
# The layers that sound whatever the sources do, with the value a line without the key plays.
REST_KEYS = {"sub_level": 0.0, "strike_level": 0.0, "air": 0.15}
METHOD = "sources alone without foundation/strike/air, those apart; stereo octave bands; 16 s from entrance + 8 s"
WINDOW = 16.0         # seconds measured
SETTLE = 8.0          # after the slowest source's entrance, before the window opens
SKIP_STEP = 4.0       # the time played before the render proper, rounded up to this so presets share a process
RANGE = 200           # presets in name order per round of processes: a run cut short leaves whole presets
# The K curve's gain at the ten octave centres (BS.1770: the RLB high pass, the head's shelf).
K_GAIN = [-13.0, -4.0, -1.0, 0.0, 0.0, 0.0, 0.5, 3.0, 4.0, 4.0]

BURIED_SHARE = 0.05   # under this share of the preset's loudness...
OWN_SHARE = 0.25      # ...and no band that matters where it carries this much: the source is buried
TARGET_SHARE = 0.08   # where a buried source is brought -- plainly there, and still the smaller voice
MATTER_DB = 15.0      # the bands that matter: K-weighted within this of the loudest, in the mix as it was
SILENT_DB = 60.0      # a source this far under the loudest band in every band does not sound at all
YIELD = 0.7071        # the sources that are heard give way at most to this of their level (-3 dB)


def num(text, default):
    try:
        return float(text)
    except (TypeError, ValueError):
        return default


def pack_presets():
    """{name: {pack, active [(slot, type, level)], entry seconds, rest (bool), hash of the line}}."""
    out = {}
    for path in sorted(glob.glob(os.path.join(PACKS, "*.ambientpack"))):
        for line in open(path, encoding="utf-8", errors="replace"):
            if "|" not in line or line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("|")
            kv = dict(x.split("=", 1) for x in parts[1].split(";") if "=" in x)
            active, entry = [], 0.0
            for k in (1, 2, 3, 4):
                t = kv.get("src%d_type" % k, "Off")
                lv = num(kv.get(LEVEL_KEY[k]), LEVEL_DEFAULT[k])
                if t != "Off" and lv > 0.0:
                    active.append((k, t, lv))
                    entry = max(entry, num(kv.get("src%d_delay" % k), 0.0) + num(kv.get("src%d_rise" % k), RISE_DEFAULT))
            out[parts[0]] = {"pack": os.path.basename(path), "active": active, "entry": entry,
                             "rest": any(num(kv.get(key), d) > 0.0 for key, d in REST_KEYS.items()),
                             "hash": "%08x" % zlib.crc32(parts[1].encode("utf-8"))}
    return out


def skip_for(entry):
    s = entry + SETTLE - WINDOW   # the window is the second half of a 2 * WINDOW render
    return 0.0 if s <= 0.0 else math.ceil(s / SKIP_STEP) * SKIP_STEP


def render_batch(names, what, skip):
    """Renders `names` in one process: `what` is a source slot ("1".."4") alone, "rest" for the layers
    without any source, or "whole" for the preset as it is. Returns ({name: bands}, {name: peak}, return code)."""
    cmd = [RENDER, "--packs", PACKS, "--batch", "", "--seconds", "%g" % (2 * WINDOW), "--notes", "45,52,59",
           "--set", "brain_rate=6", "--hour", "21", "--measure", "--bands"]
    if skip > 0.0:
        cmd += ["--skip", "%g" % skip]
    if what != "whole":
        for k in (1, 2, 3, 4):
            if what == "rest" or k != int(what):
                cmd += ["--set", "%s=0" % LEVEL_KEY[k]]
        if what != "rest":
            for key in REST_KEYS:
                cmd += ["--set", "%s=0" % key]
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        cmd[4] = f.name
        f.write("\n".join(names))
    try:
        env = dict(os.environ)
        env["AMBIENT_PACKS"] = PACKS
        env.pop("AMBIENT_MUTE", None)
        res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, **LOW)
    finally:
        os.remove(cmd[4])
    out, peaks = {}, {}
    chunks = re.split(r"^batch: (.*)$", res.stdout or "", flags=re.MULTILINE)
    for i in range(1, len(chunks) - 1, 2):
        m = re.search(r"^bands:((?: -?[\d.]+){10})\s*$", chunks[i + 1], flags=re.MULTILINE)
        if m:
            out[chunks[i].strip()] = [float(x) for x in m.group(1).split()]
        m = re.search(r"^measure:.* peak=([\d.]+)", chunks[i + 1], flags=re.MULTILINE)
        if m:
            peaks[chunks[i].strip()] = float(m.group(1))
    return out, peaks, res.returncode


def load_parts():
    """{(name, what): record} of the renders done by this method -- the latest one where there are two --
    or None when the file holds another method's."""
    done = {}
    if not os.path.exists(PARTS):
        return done
    with open(PARTS, encoding="utf-8") as f:
        try:
            if json.loads(f.readline()).get("method") != METHOD:
                return None
        except ValueError:
            return None
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue   # a line cut off when a run was stopped while writing it
            done[(r["n"], r["k"])] = r
    return done


def wanted(p):
    return [str(k) for k, _, _ in p["active"]] + (["rest"] if p["rest"] else [])


def render_parts(presets, names, jobs, done, label="renders"):
    """Renders every part of `names` not yet in `done` and appends each to PARTS as it finishes."""
    pos = {n: i for i, n in enumerate(names)}
    groups = {}
    todo = 0
    for n in names:
        p = presets[n]
        skip = skip_for(p["entry"])
        for w in wanted(p):
            r = done.get((n, w))
            if r is None or r["hash"] != p["hash"] or r["skip"] != skip:
                groups.setdefault((pos[n] // RANGE, w, skip), []).append(n)
                todo += 1
    batches = [(ns, w, s) for (_, w, s), ns in sorted(groups.items())]
    print("%d presets: %d %s to do in %d processes" % (len(names), todo, label, len(batches)), flush=True)
    if not batches:
        return 0
    t0, finished, missing, note = time.time(), 0, 0, 0
    with open(PARTS, "a", encoding="utf-8") as log, concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        futs = {ex.submit(render_batch, ns, w, s): (ns, w, s) for ns, w, s in batches}
        for f in concurrent.futures.as_completed(futs):
            ns, w, s = futs[f]
            try:
                got, _, _ = f.result()
            except Exception as e:   # a process that could not be started: its presets stay missing
                print("  a process failed: %s" % e, flush=True)
                got = {}
            for n in ns:
                if n in got:
                    log.write(json.dumps({"n": n, "k": w, "hash": presets[n]["hash"], "skip": s, "bands": got[n]}) + "\n")
                else:
                    missing += 1
            log.flush()
            finished += len(ns)
            if finished >= note or finished == todo:
                el = time.time() - t0
                print("  %d/%d %s, %.0f min, about %.0f min to go%s" % (
                    finished, todo, label, el / 60.0, el / finished * (todo - finished) / 60.0,
                    (", %d without a result" % missing) if missing else ""), flush=True)
                note = finished + max(1, todo // 100)
    return missing


def assemble(presets, names, done):
    """The measured state of `names` as the packs are now: a preset is complete when every part of its
    present line has been rendered."""
    result = {"method": METHOD, "presets": {}}
    for n in names:
        p = presets[n]
        skip = skip_for(p["entry"])

        def part(w):
            r = done.get((n, w))
            return r["bands"] if r is not None and r["hash"] == p["hash"] and r["skip"] == skip else None
        slots = {str(k): {"type": t, "level": lv, "bands": part(str(k))} for k, t, lv in p["active"]}
        rest = part("rest") if p["rest"] else None
        result["presets"][n] = {"pack": p["pack"], "entry_s": p["entry"], "skip_s": skip, "hash": p["hash"],
                                "complete": all(v["bands"] is not None for v in slots.values()) and (rest is not None or not p["rest"]),
                                "rest": rest, "slots": slots}
    return result


def multi_names(presets):
    return sorted(n for n, p in presets.items() if len(p["active"]) >= 2)


def state():
    presets = pack_presets()
    done = load_parts()
    if done is None:
        sys.exit("%s holds another method's renders; measure --fresh starts over" % PARTS)
    return presets, done, assemble(presets, multi_names(presets), done)


# ---------------------------------------------------------------- the judgement

def band_weights():
    return [10 ** (g / 10.0) for g in K_GAIN]


def energies(p):
    """Band energies: {slot: [10]} of the sources, and [10] of the rest (zeros where there is none)."""
    E = {k: [10 ** (x / 10.0) for x in v["bands"]] for k, v in p["slots"].items()}
    R = [10 ** (x / 10.0) for x in p["rest"]] if p.get("rest") else [0.0] * 10
    return E, R


def totals(E, R):
    return [sum(E[k][b] for k in E) + R[b] for b in range(10)]


def matter_bands(E, R):
    kw = [10 * math.log10(t + 1e-30) + K_GAIN[b] for b, t in enumerate(totals(E, R))]
    top = max(kw)
    return [b for b in range(10) if kw[b] >= top - MATTER_DB]


def classify(E, R, bands, W):
    tot = totals(E, R)
    whole = sum(W[b] * tot[b] for b in range(10))
    loudest = max(tot)
    out = {}
    for k in E:
        silent = max(E[k]) < loudest * 10 ** (-SILENT_DB / 10.0)
        ls = sum(W[b] * E[k][b] for b in range(10)) / whole if whole > 0 else 0.0
        own = max([E[k][b] / tot[b] for b in bands if tot[b] > 0] or [0.0])
        out[k] = {"silent": silent, "loudness_share": ls, "ownership": own,
                  "buried": (not silent) and ls < BURIED_SHARE and own < OWN_SHARE}
    return out


def plan_fix(p, least=None):
    """The levels that bring the buried sources of a preset forward, and the master gain that keeps its
    loudness -- or None when nothing is to be done. `least`: {slot: the lowest factor on its present level
    it may give way to}, YIELD where not given -- a second pass passes what is left of the 3 dB.

    Judged once, on the preset as it is: the bands that matter do not move while it is being changed, or
    lowering the loud sources lowers the bar and a source "becomes heard" in a band it already owned (a
    first version did exactly that). A buried source is raised until it carries TARGET_SHARE, up to full
    level. Where full level is not enough, the sources that are heard give way together, their balance
    kept, none under YIELD of where it was; the master gain then takes the loudness back -- and carries
    the near events up with it, which is why the others give way by 3 dB at most.

    A buried source that full level and the others giving way still would not bring to BURIED_SHARE is out
    of reach of any level -- a frozen spectrum on the silent tail of a gong, a source two octaves under a
    band pass -- and is reported and left as it is: the others do not give way for nothing (on 40 presets
    the first plan lowered a source by 6 dB for a partner that stayed 40 dB under). The Foundation,
    Strike and Air are not touched, and neither is a source silent in every band."""
    if not p["complete"] or len(p["slots"]) < 2:
        return None
    W = band_weights()
    E0, R = energies(p)
    bands = matter_bands(E0, R)
    lv0 = {k: float(v["level"]) for k, v in p["slots"].items()}
    c0 = classify(E0, R, bands, W)
    silent = sorted(k for k in E0 if c0[k]["silent"])
    buried0 = sorted((c0[k]["loudness_share"], k) for k in E0 if c0[k]["buried"])
    if not silent and not buried0:
        return None
    heard = [k for k in E0 if not c0[k]["buried"] and not c0[k]["silent"]]
    rest_w = sum(W[b] * R[b] for b in range(10))
    own_w = {k: sum(W[b] * E0[k][b] for b in range(10)) for k in E0}
    floor = {k: min(1.0, max(0.0, (least or {}).get(k, YIELD))) for k in E0}
    f = {k: 1.0 for k in E0}
    out_of_reach = []

    def weighted(j):
        return own_w[j] * f[j] ** 2
    for _, k in buried0:
        a = weighted(k)
        room = 1.0 / (lv0[k] * f[k])   # as far as full level
        fixed = sum(weighted(j) for j in E0 if j != k and j not in heard) + rest_w
        loud = sum(weighted(j) for j in heard)
        lowest = sum(own_w[j] * floor[j] ** 2 for j in heard)
        best = a * room ** 2 / (a * room ** 2 + fixed + lowest) if a > 0 else 0.0
        if best < BURIED_SHARE:
            out_of_reach.append(k)
            continue
        T = min(TARGET_SHARE, best)
        g = math.sqrt(T * (fixed + loud) / ((1.0 - T) * a))
        if g <= room:
            f[k] *= g
            continue
        f[k] *= room
        a = weighted(k)
        if loud > 0:
            # a / (a + fixed + h^2 loud) = T  ->  h^2 = (a (1 - T) / T - fixed) / loud
            h = math.sqrt(max((a * (1.0 - T) / T - fixed) / loud, 0.0))
            for j in heard:
                f[j] = max(f[j] * min(h, 1.0), floor[j])
    E = {j: [E0[j][b] * f[j] ** 2 for b in range(10)] for j in E0}
    c1 = classify(E, R, bands, W)
    total0 = sum(W[b] * t for b, t in enumerate(totals(E0, R)))
    total1 = sum(W[b] * t for b, t in enumerate(totals(E, R)))
    levels = {k: round(min(1.0, lv0[k] * f[k]), 4) for k in E0}
    changed = any(abs(levels[k] - lv0[k]) > 1e-4 for k in E0)
    return {"changed": changed, "levels": levels, "was": lv0, "out_of_reach": out_of_reach,
            "loudness_share_before": {k: round(c0[k]["loudness_share"], 4) for k in E0},
            "loudness_share_after": {k: round(c1[k]["loudness_share"], 4) for k in E0},
            "rest_share": round(rest_w / total0, 4) if total0 > 0 else 0.0,
            "buried_before": [k for _, k in buried0],
            "buried_after": sorted(k for k in E0 if c1[k]["buried"]),
            "bands": bands,
            "silent": silent,
            "loudness_db": round(10.0 * math.log10(total0), 2) if total0 > 0 else -300.0,
            "master_gain_db": round(10.0 * math.log10(total0 / total1), 2) if total1 > 0 else 0.0}


def hints(kv, k, t):
    """Why a source might be out of reach of its level, from the preset's line: tags, not a diagnosis."""
    s = "src%s_" % k
    out = []
    if t == "Spectral" and num(kv.get(s + "spec_rate"), 1.0) == 0.0:
        out.append("frozen spectrum")
    if t in ("Spectral", "Texture", "Stretch") and num(kv.get(s + "pos"), 0.0) >= 0.85:
        out.append("position near the sample's end")
    if num(kv.get(s + "delay"), 0.0) >= 12.0:
        out.append("entrance after 12 s or more")
    if num(kv.get(s + "octave"), 0.0) <= -2.0:
        out.append("two octaves down or more")
    if kv.get("filter_model", "").startswith(("BP", "HP", "Comb")):
        out.append("voice filter %s" % kv["filter_model"].split(" ")[0])
    return out or ["no obvious reason"]


# ---------------------------------------------------------------- the commands

def measure(a):
    presets = pack_presets()
    names = multi_names(presets)
    if a.limit:
        step = max(1, len(names) // a.limit)   # spread over the whole library, not the first packs
        names = names[::step][:a.limit]
    os.makedirs(WORK, exist_ok=True)
    done = {} if a.fresh else load_parts()
    if done is None:
        print("%s holds another method's renders; --fresh starts over" % PARTS)
        return 2
    if a.fresh or not os.path.exists(PARTS):
        with open(PARTS, "w", encoding="utf-8") as f:
            f.write(json.dumps({"method": METHOD}) + "\n")
    missing = render_parts(presets, names, a.jobs, done)
    result = assemble(presets, names, load_parts())
    json.dump(result, open(OUT, "w", encoding="utf-8"))
    incomplete = sum(1 for p in result["presets"].values() if not p["complete"])
    print("wrote %s: %d presets, %d of them incomplete%s" % (OUT, len(names), incomplete,
          (" (%d renders gave no result; measure again renders what is missing)" % missing) if missing else ""))
    return 0


def render_whole(by_skip, jobs):
    """{skip: [names]} rendered as they are: ({name: bands}, {name: peak})."""
    whole, peaks = {}, {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for got, pk, _ in ex.map(lambda kv: render_batch(kv[1], "whole", kv[0]), sorted(by_skip.items())):
            whole.update(got)
            peaks.update(pk)
    return whole, peaks


def check(a):
    """Renders whole presets and compares them with the sum of their parts."""
    presets, done, st = state()
    complete = sorted(n for n, p in st["presets"].items() if p["complete"])
    if not complete:
        print("nothing measured yet")
        return 1
    pick = complete[::max(1, len(complete) // a.limit)][:a.limit]
    by_skip = {}
    for n in pick:
        by_skip.setdefault(st["presets"][n]["skip_s"], []).append(n)
    whole = render_whole(by_skip, a.jobs)[0]
    W = band_weights()
    band_dev, loud_dev = [], []
    for n in pick:
        if n not in whole:
            continue
        E, R = energies(st["presets"][n])
        tot = totals(E, R)
        Wb = [10 ** (x / 10.0) for x in whole[n]]
        for b in matter_bands(E, R):
            band_dev.append(10 * math.log10((tot[b] + 1e-30) / (Wb[b] + 1e-30)))
        loud_dev.append(10 * math.log10(sum(W[b] * tot[b] for b in range(10)) / sum(W[b] * Wb[b] for b in range(10))))
    if not loud_dev:
        print("no whole render came back")
        return 1
    absb = sorted(abs(x) for x in band_dev)
    print("%d presets rendered whole; the sum of the parts against the whole:" % len(loud_dev))
    print("  loudness (K-weighted): median %+.2f dB, from %+.2f to %+.2f" % (statistics.median(loud_dev), min(loud_dev), max(loud_dev)))
    print("  bands that matter: median |deviation| %.2f dB, 90 %% within %.2f dB, largest %.2f (%d bands)"
          % (statistics.median(absb), absb[int(0.9 * (len(absb) - 1))], absb[-1], len(absb)))
    return 0


def report(a):
    """What the fix would find, by the same classification it uses."""
    presets, done, st = state()
    W = band_weights()
    by_count, per_type, hidden, silent, under_rest = {}, {}, [], [], 0
    incomplete = 0
    for n, p in st["presets"].items():
        if not p["complete"]:
            incomplete += 1
            continue
        E, R = energies(p)
        c = classify(E, R, matter_bands(E, R), W)
        row = by_count.setdefault(len(E), [0, 0])
        row[0] += 1
        buried = [k for k in E if c[k]["buried"]]
        if buried:
            row[1] += 1
            tot = totals(E, R)
            if sum(W[b] * R[b] for b in range(10)) > 0.5 * sum(W[b] * tot[b] for b in range(10)):
                under_rest += 1
        for k in buried:
            t = p["slots"][k]["type"]
            per_type[t] = per_type.get(t, 0) + 1
            hidden.append((c[k]["loudness_share"], n, k, t, p["slots"][k]["level"], c[k]["ownership"]))
        silent += ["%s (source %s %s)" % (n, k, p["slots"][k]["type"]) for k in E if c[k]["silent"]]
    total = sum(v[0] for v in by_count.values())
    print("%d presets measured (%d not, or changed since); %d have a buried source (under %.0f %% of the loudness, no band of its own)"
          % (total, incomplete, sum(v[1] for v in by_count.values()), 100 * BURIED_SHARE))
    for c in sorted(by_count):
        print("   %d sources: %5d presets, %5d with one buried (%.1f %%)" % (c, by_count[c][0], by_count[c][1], 100.0 * by_count[c][1] / max(1, by_count[c][0])))
    print("types of the buried sources:", ", ".join("%s %d" % kv for kv in sorted(per_type.items(), key=lambda kv: -kv[1])))
    print("presets with a buried source where Foundation, Strike and Air carry most of the loudness: %d" % under_rest)
    print("sources that do not sound at all: %d%s" % (len(silent), (" -- " + ", ".join(silent[:8])) if silent else ""))
    hidden.sort()
    print("the most buried:")
    for ls, n, k, t, lv, own in hidden[:12]:
        print("   %-28s source %s %-10s level %.2f  loudness share %.4f  best band share %.2f" % (n, k, t, lv, ls, own))
    return 0


def fix(a):
    presets, done, st = state()
    # A second pass lets no source give way further than YIELD of the level it had before the first:
    # one that gave 3 dB then gives nothing now.
    first = json.load(open(PASS1, encoding="utf-8")) if os.path.exists(PASS1) else {}
    plans = {}
    for n, p in st["presets"].items():
        least = None
        if n in first and first[n]["changed"]:
            least = {k: max(YIELD, min(1.0, YIELD * first[n]["was"][k] / max(float(p["slots"][k]["level"]), 1e-6)))
                     for k in p["slots"] if k in first[n]["was"]}
        pl = plan_fix(p, least)
        if pl is not None:
            plans[n] = pl
    by_pack = {}
    for n, pl in plans.items():
        by_pack.setdefault(st["presets"][n]["pack"], {})[n] = pl
    changed, unresolved, silent_presets, reach, tags = 0, [], [], [], {}
    for pack, mine in sorted(by_pack.items()):
        path = os.path.join(PACKS, pack)
        # newline="" on both ends: the packs are stored with CRLF, and a text-mode round trip turned every
        # line of all 56 files into a change (the last field of a line keeps its "\r" this way)
        text = open(path, encoding="utf-8", newline="").read()
        lines = text.split("\n")
        touched = False
        for i, line in enumerate(lines):
            if "|" not in line or line.startswith("#"):
                continue
            parts = line.split("|")
            pl = mine.get(parts[0])
            if pl is None:
                continue
            if pl["silent"]:
                silent_presets.append("%s (source %s)" % (parts[0], ",".join(pl["silent"])))
            line_kv = dict(x.split("=", 1) for x in parts[1].split(";") if "=" in x)
            for k in pl["out_of_reach"]:
                reach.append("%s (source %s)" % (parts[0], k))
                for tag in hints(line_kv, k, st["presets"][parts[0]]["slots"][k]["type"]):
                    tags[tag] = tags.get(tag, 0) + 1
            if [k for k in pl["buried_after"] if k not in pl["out_of_reach"]]:
                unresolved.append(parts[0])
            if not pl["changed"]:
                continue
            keys = {LEVEL_KEY[int(k)]: k for k in pl["levels"]}
            kv = [x.split("=", 1) for x in parts[1].split(";") if "=" in x]
            seen = set()
            for pair in kv:
                seen.add(pair[0])
                if pair[0] in keys:
                    pair[1] = "%.4g" % pl["levels"][keys[pair[0]]]
                elif pair[0] == "master_gain":
                    pair[1] = "%.4g" % max(-40.0, min(12.0, float(pair[1]) + pl["master_gain_db"]))
            for key, k in keys.items():
                if key not in seen:
                    kv.append([key, "%.4g" % pl["levels"][k]])
            if "master_gain" not in seen:
                kv.append(["master_gain", "%.4g" % max(-40.0, min(12.0, -6.0 + pl["master_gain_db"]))])
            parts[1] = ";".join("%s=%s" % (x[0], x[1]) for x in kv)
            lines[i] = "|".join(parts)
            changed += 1
            touched = True
        if touched and not a.dry_run:
            open(path, "w", encoding="utf-8", newline="").write("\n".join(lines))
    if not a.dry_run:
        json.dump(plans, open(FIXLOG, "w", encoding="utf-8"), indent=1)
    moved = [pl for pl in plans.values() if pl["changed"]]
    raised = sum(1 for pl in moved for k in pl["levels"] if pl["levels"][k] > pl["was"][k] + 1e-4)
    lowered = sum(1 for pl in moved for k in pl["levels"] if pl["levels"][k] < pl["was"][k] - 1e-4)
    gains = sorted(pl["master_gain_db"] for pl in moved) or [0.0]
    yields = sorted(20 * math.log10(pl["levels"][k] / pl["was"][k]) for pl in moved for k in pl["levels"]
                    if pl["levels"][k] < pl["was"][k] - 1e-4)
    raises = sorted(20 * math.log10(pl["levels"][k] / pl["was"][k]) for pl in moved for k in pl["levels"]
                    if pl["levels"][k] > pl["was"][k] + 1e-4)
    print("%s %d presets: %d sources brought forward (median %+.1f dB, up to %+.1f), %d gave way (median %+.1f dB);"
          " master gain moved %+.1f .. %+.1f dB, median %+.1f"
          % ("would change" if a.dry_run else "changed", changed, raised, statistics.median(raises or [0.0]), max(raises or [0.0]),
             lowered, statistics.median(yields or [0.0]), gains[0], gains[-1], statistics.median(gains)))
    print("  sources out of reach of any level, left as they are: %d in %d presets%s"
          % (len(reach), len({r.rsplit(" (", 1)[0] for r in reach}), (": " + ", ".join(reach[:6])) if reach else ""))
    if tags:
        print("    what their lines show:", ", ".join("%s %d" % kv for kv in sorted(tags.items(), key=lambda kv: -kv[1])))
    print("  brought forward and still under the bar: %d%s"
          % (len(unresolved), (": " + ", ".join(unresolved[:10])) if unresolved else ""))
    print("  a source that does not sound at all, left alone: %d%s"
          % (len(silent_presets), (": " + ", ".join(silent_presets[:10])) if silent_presets else ""))
    return 0


def carry(a):
    """After a fix, before measuring again: the parts of a changed preset that the fix did not touch --
    the sources whose level stayed, and the rest -- moved only by the master gain, which comes after
    everything. They are carried into the new line as they were plus that gain, so the second measure
    renders only the sources whose level moved. The fix's log is kept as PASS1 for the second fix."""
    if not os.path.exists(FIXLOG):
        sys.exit("no fix to carry from")
    plans = json.load(open(FIXLOG, encoding="utf-8"))
    presets = pack_presets()
    done = load_parts()
    if done is None:
        sys.exit("%s holds another method's renders" % PARTS)
    carried, left = 0, 0
    with open(PARTS, "a", encoding="utf-8") as log:
        for n, pl in sorted(plans.items()):
            if not pl["changed"]:
                continue
            p = presets[n]
            skip = skip_for(p["entry"])
            for w in wanted(p):
                r = done.get((n, w))
                if r is None or r["hash"] == p["hash"] or r["skip"] != skip:
                    continue   # never rendered, or rendered from the new line already
                if w != "rest" and abs(pl["levels"].get(w, -1.0) - pl["was"].get(w, -2.0)) > 1e-4:
                    left += 1   # its level moved: measure renders it
                    continue
                bands = [round(x + pl["master_gain_db"], 2) if x > -299.0 else x for x in r["bands"]]
                log.write(json.dumps({"n": n, "k": w, "hash": p["hash"], "skip": skip, "bands": bands, "carried": True}) + "\n")
                carried += 1
    os.replace(FIXLOG, PASS1)
    print("carried %d parts into the changed lines; %d sources whose level moved are left to render; the fix's log is now %s"
          % (carried, left, PASS1))
    return 0


def verify(a):
    """Renders a sample of the presets fix changed, as they are now, and compares with what it expected."""
    plans = json.load(open(FIXLOG, encoding="utf-8"))
    names = sorted(n for n, pl in plans.items() if pl["changed"])
    pick = names[::max(1, len(names) // a.limit)][:a.limit]
    presets = pack_presets()
    done = load_parts()
    if done is None:
        sys.exit("%s holds another method's renders" % PARTS)
    render_parts(presets, pick, a.jobs, done, "renders of changed presets")
    st = assemble(presets, pick, load_parts())
    by_skip = {}
    for n in pick:
        by_skip.setdefault(skip_for(presets[n]["entry"]), []).append(n)
    whole, peaks = render_whole(by_skip, a.jobs)
    W = band_weights()
    dev, still, fine, loud = [], [], 0, []
    for n in pick:
        p = st["presets"][n]
        pl = plans[n]
        if n in whole:
            loud.append((10 * math.log10(sum(W[b] * 10 ** (x / 10.0) for b, x in enumerate(whole[n]))) - pl["loudness_db"], n))
        if not p["complete"]:
            continue
        E, R = energies(p)
        c = classify(E, R, pl["bands"], W)   # in the bands that mattered before the fix, as the fix judged it
        forward = [k for k in pl["buried_before"] if k not in pl["out_of_reach"]]
        for k in forward:
            dev.append(10 * math.log10(max(c[k]["loudness_share"], 1e-9) / max(pl["loudness_share_after"][k], 1e-9)))
        buried = [k for k in forward if c[k]["buried"]]
        if buried:
            still.append("%s (source %s)" % (n, ",".join(buried)))
        else:
            fine += 1
    if not dev:
        print("nothing came back")
        return 1
    print("%d changed presets rendered again: in %d every source brought forward is over the bar now, in %d not%s"
          % (fine + len(still), fine, len(still), (": " + ", ".join(still[:10])) if still else ""))
    print("  loudness share of those sources against the fix's expectation: median %+.2f dB, from %+.2f to %+.2f"
          % (statistics.median(dev), min(dev), max(dev)))
    if loud:
        ld = sorted(loud)
        print("  loudness of the whole render now against the sum of the parts before (which reads ~0.3 dB over a whole):"
              " median %+.2f dB, from %+.2f to %+.2f; furthest: %s" % (
                  statistics.median([x for x, _ in ld]), ld[0][0], ld[-1][0],
                  ", ".join("%s %+.1f" % (n, x) for x, n in sorted(ld, key=lambda t: -abs(t[0]))[:5])))
    if peaks:
        # against the peaks the library measurement found before any of this (a longer render, so only a
        # rough comparison: a preset that clipped there already is not the fix's doing -- Millstone Vigil)
        lib = os.path.join(WORK, "packs.json")
        before = {n: v.get("peak") for n, v in json.load(open(lib, encoding="utf-8")).items()} if os.path.exists(lib) else {}
        hot = sorted((v, n) for n, v in peaks.items() if v > 0.891)
        print("  peaks of the whole renders: highest %.3f, %d over -1 dBFS%s" % (
            max(peaks.values()), len(hot), (": " + ", ".join("%s %.2f (library measurement %s)" % (
                n, v, "%.2f" % before[n] if before.get(n) is not None else "-") for v, n in hot[-6:])) if hot else ""))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", choices=["measure", "check", "report", "fix", "carry", "verify"])
    ap.add_argument("--jobs", type=int, default=20)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    if a.what in ("check", "verify") and not a.limit:
        a.limit = 24 if a.what == "check" else 60
    return {"measure": measure, "check": check, "report": report, "fix": fix, "carry": carry, "verify": verify}[a.what](a)


if __name__ == "__main__":
    sys.exit(main())
