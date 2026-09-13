"""Noctuary -- give the library packs the four things that came after they were generated.

Tools/enrich_presets.py does this for the 191 compiled-in presets. This does it for the 5000 in
Library/Packs, and it has a different job, because the library is not short of features the way
the built-ins were. Counted over the twenty-five older packs:

    lfo 100 %, matrix 100 %, env shapes 67 %, source 2 51 %, z-plane 40 %, source 3 27 %

and then, at nought per cent each: the BEAT source, the z-plane's third axis, the delay's duck,
and LFO rates that are decoupled rather than merely random. Those four are what the instrument
grew after the packs were written, and they are all this tool adds.

  * The golden ladder. A preset's LFO rates are snapped to rungs of a phi ladder anchored on its
    own slowest one, so no two of them ever come back into step. Snapping rather than replacing:
    the rungs are a factor of 1.618 apart, so a rate moves by at most 27 %, and the preset keeps
    the timescale it was written with. Where two LFOs land on the same rung the later one is
    pushed up, which is the whole point -- two modulators at the same rate are one modulator.
  * z_z, the third axis of the filter cube, small, where the preset has a z-plane and no z_z.
    Nought is the old flat square; a little of it bends the shape as it moves.
  * dly_duck, where the delay is audible: the echoes step out of the way of the dry sound and
    come back as it decays.
  * One BEAT route, where there is a free slot and none: the difference tone between the two
    lowest voices, at a depth low enough to be the sound noticing its own beating.

Everything is added into empty slots only, and every preset is rendered before and after: if the
change moved its level by more than 1.5 dB, its centroid by a quarter, or its bass or width by
0.12, that preset is put back exactly as it was and counted as rejected.

    python Tools/enrich_packs.py --dry-run          what it would do, per pack
    python Tools/enrich_packs.py --jobs 6           do it, measured
"""
import argparse
import concurrent.futures
import hashlib
import math
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
PACKS = os.path.join(ROOT, "Library", "Packs")
# A sibling of Library/Packs, not a temp folder: every pack names its samples relatively, as
# ../Textures/x.wav, and a copy anywhere else is a pack whose media has vanished.
WORK = os.path.join(ROOT, "Library", "Packs.enriched")

PHI = (1.0 + math.sqrt(5.0)) / 2.0

# Where a BEAT route may go, dark presets first. Deliberately shallow targets: the difference
# tone is a slow, wandering thing, and pointed at a filter it is a hand on the sound rather than
# a gesture in it.
BEAT_DARK = [("far_size", 0.13), ("depth", 0.11), ("cosmos_smear", 0.10), ("sub_level", 0.09)]
BEAT_BRIGHT = [("cutoff", 0.12), ("brightness", 0.12), ("z_y", 0.16), ("air", 0.10)]


def measure(name, seconds, settings, packs):
    cmd = [RENDER, "--preset", name, "--seconds", str(seconds), "--measure"]
    if "brain_on=off" in settings.replace(" ", ""):
        cmd += ["--notes", "45,52,57,64"]
    env = dict(os.environ, AMBIENT_PACKS=packs)
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, env=env, timeout=180)
    except subprocess.TimeoutExpired:
        return None
    lines = (r.stdout + r.stderr).strip().splitlines()
    out = {}
    for line in lines[-1:]:
        for k, v in re.findall(r"(\w+)=(-?[\d.]+)", line):
            out[k] = float(v)
    return out if "rms" in out else None


def value(settings, key, default=None):
    m = re.search(r"(?:^|;)" + re.escape(key) + r"=([^;]*)", settings)
    return m.group(1) if m else default


def ladder(settings):
    """LFO rates snapped to a golden ladder on the preset's own slowest one. Returns the new
    settings and how many rates actually moved."""
    rates = {}
    for m in re.finditer(r"(?:^|;)lfo(\d)_rate=([\d.eE+-]+)", settings):
        try:
            rates[int(m.group(1))] = float(m.group(2))
        except ValueError:
            return settings, 0
    rates = {k: v for k, v in rates.items() if v > 0.0}
    if len(rates) < 2:
        return settings, 0
    base = min(rates.values())
    used, new = set(), {}
    for idx in sorted(rates, key=lambda k: rates[k]):        # slowest first, so it keeps rung 0
        rung = int(round(math.log(rates[idx] / base) / math.log(PHI)))
        while rung in used:
            rung += 1
        used.add(rung)
        new[idx] = base * PHI ** rung
    moved = sum(1 for k in rates if abs(new[k] - rates[k]) > 1e-9)
    out = settings
    for idx, v in new.items():
        out = re.sub(r"((?:^|;)lfo%d_rate=)[\d.eE+-]+" % idx, lambda m: m.group(1) + "%.5g" % v, out)
    return out, moved


def enrich(name, settings, mod):
    """(settings, mod, [what changed]) -- every rule fires into an empty slot only."""
    why = []
    settings, moved = ladder(settings)
    if moved:
        why.append("%d LFO rates onto the golden ladder" % moved)

    seed = int(hashlib.sha1(name.encode("utf-8")).hexdigest()[:8], 16)

    if "z_mode=" in settings and not re.search(r"(^|;)z_z=", settings):
        settings += ";z_z=%.3f" % (0.10 + 0.35 * ((seed >> 3) % 100) / 99.0)
        why.append("the filter's third axis")

    try:
        dmix = float(value(settings, "dly_mix", "0") or 0)
    except ValueError:
        dmix = 0.0
    if dmix > 0.12 and not re.search(r"(^|;)dly_duck=", settings):
        settings += ";dly_duck=%.3f" % (0.25 + 0.40 * ((seed >> 9) % 100) / 99.0)
        why.append("the delay ducking under the dry sound")

    routes = [r for r in mod.split(";") if r.strip()]
    if len(routes) < 10 and "beat>" not in mod:
        try:
            bright = float(value(settings, "brightness", "0.6") or 0.6)
        except ValueError:
            bright = 0.6
        table = BEAT_DARK if bright < 0.5 else BEAT_BRIGHT
        taken = set(re.findall(r">([a-z0-9_]+):", mod))
        for target, depth in table:
            if target not in taken:
                routes.append("beat>%s:%.3f" % (target, depth * (0.7 + 0.6 * ((seed >> 15) % 100) / 99.0)))
                why.append("a BEAT route on " + target)
                break
    return settings, ";".join(routes), why


def read_pack(path):
    """(header lines, [(fields, raw line)]) -- rows kept as fields so a column can be replaced."""
    head, rows = [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            t = line.rstrip("\n")
            if not t or t.startswith("#") or t.startswith("pack ") or (t.startswith("format ") and "|" not in t):
                head.append(t)
            else:
                rows.append(t.split("|"))
    return head, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--seconds", type=float, default=16.0)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--only", default="", help="one pack, by file name stem")
    a = ap.parse_args()
    if not os.path.isfile(RENDER):
        sys.exit("build the render tool first")

    packs = [f for f in sorted(os.listdir(PACKS)) if f.endswith(".ambientpack")]
    if a.only:
        packs = [f for f in packs if a.only.lower() in f.lower()]

    plan = []           # (pack file, row index, fields, new settings, new mod, why)
    per_pack = {}
    for fn in packs:
        head, rows = read_pack(os.path.join(PACKS, fn))
        per_pack[fn] = (head, rows)
        for i, r in enumerate(rows):
            while len(r) < 7:
                r.append("")
            s2, m2, why = enrich(r[0], r[1], r[6])
            if why:
                plan.append((fn, i, s2, m2, why))
    counts = {}
    for _, _, _, _, why in plan:
        for w in why:
            key = re.sub(r"^\d+ ", "", w).split(" on ")[0]
            counts[key] = counts.get(key, 0) + 1
    print("%d presets in %d packs, %d would change" % (
        sum(len(v[1]) for v in per_pack.values()), len(packs), len(plan)))
    for k, v in sorted(counts.items(), key=lambda kv: -kv[1]):
        print("  %-44s %5d" % (k, v))
    if a.dry_run:
        return 0

    # ---------------------------------------------------------------- the enriched copy
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    changed = {}
    for fn, i, s2, m2, why in plan:
        changed.setdefault(fn, {})[i] = (s2, m2)
    for fn, (head, rows) in per_pack.items():
        out = list(head)
        for i, r in enumerate(rows):
            r = list(r)
            if i in changed.get(fn, {}):
                r[1], r[6] = changed[fn][i]
            out.append("|".join(r))
        with open(os.path.join(WORK, fn), "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")

    # ---------------------------------------------------------------- before and after
    jobs = [(fn, i, per_pack[fn][1][i][0], per_pack[fn][1][i][1], s2, why) for fn, i, s2, m2, why in plan]
    print("measuring %d presets before and after, %d at a time" % (len(jobs), a.jobs), flush=True)

    def one(j):
        fn, i, name, old, new, why = j
        b = measure(name, a.seconds, old, PACKS)
        af = measure(name, a.seconds, new, WORK)
        return fn, i, name, b, af, why

    kept, rejected, reasons = 0, [], {}
    revert = {}
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
        for fn, i, name, b, af, why in pool.map(one, jobs):
            done += 1
            if done % 250 == 0:
                print("  %d/%d  kept %d, put back %d" % (done, len(jobs), kept, len(rejected)), flush=True)
            if b is None or af is None:
                rejected.append((name, "did not render"))
                revert.setdefault(fn, set()).add(i)
                reasons["did not render"] = reasons.get("did not render", 0) + 1
                continue
            bad = []
            if abs(af["rms"] - b["rms"]) > 1.5:
                bad.append("level")
            if abs(af["centroid"] - b["centroid"]) / max(b["centroid"], 1.0) > 0.25:
                bad.append("centroid")
            if abs(af["bass"] - b["bass"]) > 0.12:
                bad.append("bass")
            if abs(af["width"] - b["width"]) > 0.12:
                bad.append("width")
            if af.get("peak", 0.0) > 0.99:
                bad.append("clips")
            if bad:
                r = ", ".join(bad)
                rejected.append((name, r))
                revert.setdefault(fn, set()).add(i)
                reasons[r] = reasons.get(r, 0) + 1
            else:
                kept += 1

    # ---------------------------------------------------------------- put the rejects back
    for fn, (head, rows) in per_pack.items():
        out = list(head)
        back = revert.get(fn, set())
        for i, r in enumerate(rows):
            r = list(r)
            if i in changed.get(fn, {}) and i not in back:
                r[1], r[6] = changed[fn][i]
            out.append("|".join(r))
        with open(os.path.join(PACKS, fn), "w", encoding="utf-8") as f:
            f.write("\n".join(out) + "\n")
    shutil.rmtree(WORK)

    print()
    print("kept %d, put back %d" % (kept, len(rejected)))
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print("  put back for %-24s %5d" % (k, v))
    for n, r in rejected[:15]:
        print("    %-40s %s" % (n[:40], r))
    return 0


if __name__ == "__main__":
    sys.exit(main())
