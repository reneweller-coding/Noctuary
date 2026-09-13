"""Noctuary -- score every preset, so "make the presets better" has something to aim at.

Tools/preset_check.py already asks whether a preset is broken: silent, clipping, clicking,
collapsing in mono. This asks the harder question -- whether it is any good -- and it asks it in
four ways that can be measured rather than argued about:

  ALIVE      does anything change over the render? A drone that measures the same in its first
             half as in its second is a chord, not a piece. Scored from how much the descriptors
             move between two windows of the same render.
  MOVING     is there motion inside the sound itself, as opposed to the slow arc? Spectral flux.
  REACH      how much of the instrument does it actually use? The count of sections whose
             parameters have been moved off their defaults -- an eleven-year-old instrument with
             three hundred parameters and a preset that touches nine of them is a preset from an
             earlier version of it.
  APART      is it different from the others? Distance to its nearest neighbour in descriptor
             space, so a bank of near-duplicates scores badly even when each one is fine.

    python Tools/rate_presets.py                    # the built-in presets
    python Tools/rate_presets.py --packs            # the library as well (slow: 5000 renders)
    python Tools/rate_presets.py --json rating.json

The score is a blunt instrument and is meant to be: it finds the twenty worst presets to look at,
it does not decide what is beautiful. Every number it prints is a measurement, and the weakest
column tells you what is missing rather than that something is.
"""
import argparse
import concurrent.futures
import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")

FIELDS = ("rms", "centroid", "flatness", "flux", "bass", "width")

# A comment between two of a preset's string literals breaks any pattern that expects the run of
# literals to be unbroken -- and it does not break it loudly, it simply stops finding that preset.
# Two of the built-in presets carry one, and this file's parsers reported 194 of 196 for a while
# without a word. Comments that begin a line are removed before matching; a "//" inside a literal
# is left alone, because none of them start a line.
def strip_line_comments(text):
    out = []
    for line in text.split(chr(10)):
        stripped = line.lstrip()
        out.append("" if stripped.startswith("//") else line)
    return chr(10).join(out)



def measure(name, seconds, settings="", extra=None):
    """One render. A preset with the conductor switched off is a playable patch and makes no
    sound at all on its own -- the first version of this tool rated the thirteen Keys presets at
    zero for being silent, which they were, because nothing had played a note. They get a held
    chord; the others are left to the conductor, which is how they are meant to be heard."""
    cmd = [RENDER, "--preset", name, "--seconds", str(seconds), "--measure"]
    if "brain_on=off" in settings.replace(" ", ""):
        cmd += ["--notes", "45,52,57,64"]
    if extra:
        cmd += extra
    # The library lives in Library/Packs here, not in the user's Documents, so the render tool is
    # pointed at it explicitly; otherwise a pack preset simply is not found and rates as missing.
    env = dict(os.environ, AMBIENT_PACKS=os.path.join(ROOT, "Library", "Packs"))
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, env=env)
    # The descriptor line begins with "measure:". It used to be the last line of the output, until
    # the render tool learned to print the timbre vector after it -- and then every preset of the
    # library rated as "no render" (13.09.2026, the same trap check_layer_presets.py had fallen into).
    lines = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("measure:")]
    if not lines:
        return None
    out = {}
    for k, v in re.findall(r"(\w+)=(-?[\d.]+)", lines[-1]):
        out[k] = float(v)
    return out if "rms" in out else None


def preset_names(with_packs):
    """(name, settings) for everything the instrument can load."""
    out = []
    p = os.path.join(ROOT, "Core", "src", "Presets.cpp")
    with open(p, encoding="utf-8") as f:
        text = strip_line_comments(f.read())
    body = text[text.index("const Preset kPresets[]"):text.index("int numCosmosPresets")]
    # A preset is { "name", "settings" } with the settings possibly split over several lines,
    # and since Tools/enrich_presets.py ran, most of them carry four more fields:
    # { "name", "settings", nullptr, nullptr, nullptr, "matrix" }. Without the tail this pattern
    # matched exactly one preset -- Init, the only one left in the short form -- and the tool
    # cheerfully reported a rating of the whole bank based on it.
    for m in re.finditer(r'\{\s*"([^"]+)",\s*((?:"[^"]*"\s*)+)(?:,[^}]*)?\}', body):
        out.append((m.group(1), "".join(re.findall(r'"([^"]*)"', m.group(2)))))
    if with_packs:
        packs = os.path.join(ROOT, "Library", "Packs")
        for fn in sorted(os.listdir(packs)):
            if not fn.endswith(".ambientpack"):
                continue
            with open(os.path.join(packs, fn), encoding="utf-8", errors="replace") as f:
                for line in f:
                    t = line.strip()
                    if not t or t.startswith("#") or t.startswith("pack ") or (t.startswith("format ") and "|" not in t):
                        continue
                    parts = t.split("|")
                    if len(parts) >= 2:
                        out.append((parts[0].strip(), parts[1].strip()))
    return out


def sections_of(settings):
    """Which sections of the instrument a preset's settings touch."""
    keys = set(re.findall(r"([a-z0-9_]+)=", settings))
    fams = set()
    for k in keys:
        for prefix, fam in (("src1", "source"), ("src2", "source2"), ("src3", "source3"), ("src4", "source4"),
                            ("osc_", "source"), ("z_", "zplane"), ("dly2", "delay2"),
                            ("dly_", "delay"), ("far_", "far"), ("cosmos_", "cosmos"),
                            ("cloud_", "cloud"), ("body_", "body"), ("patina", "patina"),
                            ("brain2", "brain2"), ("brain_", "brain"), ("auto_", "autoplay"),
                            ("strike", "strike"), ("sub_", "foundation"), ("air", "air"),
                            ("lfo", "lfo"), ("env1", "modenv"), ("env2", "modenv"),
                            ("env3", "modenv"), ("env4", "modenv"), ("env5", "modenv"),
                            ("env6", "modenv"), ("room_", "room"), ("near_", "nearverb"),
                            ("blur_", "blur"), ("feedback", "feedback"), ("phase_", "phase"),
                            ("press_", "expression"), ("slide_", "expression"),
                            ("coherence", "coherence"), ("purity", "tuning"), ("scale", "tuning")):
            if k.startswith(prefix):
                fams.add(fam)
                break
    return fams


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--packs", action="store_true", help="rate the library too (slow)")
    ap.add_argument("--seconds", type=float, default=24.0)
    ap.add_argument("--json", default="")
    ap.add_argument("--worst", type=int, default=25)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    # The library's own measurement (measure_packs.py, sixty seconds a preset after its warm-up,
    # and the built-ins measured again from the binary) already holds everything this rating
    # needs: the six descriptors, the flux, and how far the sound travelled over its minute
    # (evo_tone, evo_level). Rating used to render every preset twice more to learn the same --
    # two hours for a library that had just been measured for four (Rene, 13.09.2026: "Warum macht
    # man das dann nicht gleich mit der Vermessung?"). Now it reads the caches and renders only
    # what they do not have.
    ap.add_argument("--cache", action="append", default=None,
                    help="measurement caches to read (default: build/library-work/packs.json and builtins.json); --no-cache to render everything")
    ap.add_argument("--no-cache", action="store_true")
    a = ap.parse_args()
    if not os.path.isfile(RENDER):
        sys.exit("build the render tool first")

    cache = {}
    if not a.no_cache:
        paths = a.cache or [os.path.join(ROOT, "build", "library-work", "packs.json"),
                            os.path.join(ROOT, "build", "library-work", "builtins.json")]
        for p in paths:
            if os.path.isfile(p):
                with open(p, encoding="utf-8") as f:
                    for name, row in json.load(f).items():
                        if isinstance(row, dict) and "centroid" in row:
                            cache.setdefault(name, row)
        print("measurement cache: %d presets" % len(cache))

    items = preset_names(a.packs)
    if a.limit:
        items = items[:a.limit]
    print("rating %d presets, %.0f s each where not measured" % (len(items), a.seconds))

    def rate_one(item):
        name, settings = item
        row = cache.get(name)
        if row is not None:
            whole = dict(row)
            whole["rms"] = row.get("rms_db", row.get("rms", -120.0))
            # ALIVE from the minute's own travel: the tone's (0 .. about 1) and the level's (dB),
            # on scales where a drone that clearly moved scores one. The scales are chosen so the
            # library sits where the old two-render ALIVE put it (median about 0.3, the top tenth
            # above 0.6); the score weights were tuned to that.
            alive = 0.6 * min(1.0, float(row.get("evo_tone", 0.0)) / 0.8) + 0.4 * min(1.0, float(row.get("evo_level", 0.0)) / 8.0)
            return name, settings, whole, alive
        whole = measure(name, a.seconds, settings)
        early = measure(name, a.seconds * 0.4, settings)
        if whole is None or early is None:
            return name, settings, None, 0.0
        # ALIVE: how far the descriptors travel between the early window and the whole.
        alive = 0.0
        for f in FIELDS:
            lo, hi = early.get(f, 0.0), whole.get(f, 0.0)
            scale = {"rms": 6.0, "centroid": 200.0, "flatness": 0.02, "flux": 0.15,
                     "bass": 0.25, "width": 0.25}[f]
            alive += min(1.0, abs(hi - lo) / scale)
        alive /= len(FIELDS)
        return name, settings, whole, alive

    rows = []
    done = 0
    rendered = sum(1 for name, _ in items if name not in cache)
    if rendered:
        print("  %d presets are not in the cache and will be rendered" % rendered)
    # Four at a time. The renders are CPU-bound and short; four leaves the machine usable, which
    # matters when the library takes five thousand of them.
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as pool:
      for name, settings, whole, alive in pool.map(rate_one, items):
        done += 1
        if whole is None:
            print("  no render: %s" % name)
            continue
        moving = min(1.0, whole.get("flux", 0.0) / 0.6)
        reach = min(1.0, len(sections_of(settings)) / 12.0)
        rows.append(dict(name=name, alive=alive, moving=moving, reach=reach,
                         desc=[whole.get(f, 0.0) for f in FIELDS], rms=whole.get("rms", -120.0)))
        if done % 250 == 0:
            print("  %d/%d" % (done, len(items)), flush=True)

    # APART: distance to the nearest other preset, in a normalised descriptor space.
    norm = []
    for f_i, f in enumerate(FIELDS):
        vals = [r["desc"][f_i] for r in rows]
        lo, hi = min(vals), max(vals)
        norm.append((lo, max(hi - lo, 1e-9)))
    for r in rows:
        r["v"] = [(r["desc"][i] - norm[i][0]) / norm[i][1] for i in range(len(FIELDS))]
    # Nearest neighbour, on a grid. Comparing five thousand presets with each other is twenty-five
    # million distances in Python; bucketing them by a coarse cell and looking only at the
    # neighbouring cells makes it a few hundred thousand and gives the same answer.
    cell = 0.12
    buckets = {}
    for idx, r in enumerate(rows):
        key = tuple(int(v / cell) for v in r["v"])
        buckets.setdefault(key, []).append(idx)
    import itertools
    for idx, r in enumerate(rows):
        key = tuple(int(v / cell) for v in r["v"])
        best = 9e9
        for off in itertools.product((-1, 0, 1), repeat=len(FIELDS)):
            nb = buckets.get(tuple(k + o for k, o in zip(key, off)))
            if not nb:
                continue
            for j in nb:
                if j == idx:
                    continue
                o = rows[j]
                d = math.sqrt(sum((x - y) ** 2 for x, y in zip(r["v"], o["v"])))
                if d < best:
                    best = d
        if best > 8e9:
            best = 3 * cell          # nothing in the neighbouring cells: it is on its own
        r["apart"] = min(1.0, best / 0.25)

    for r in rows:
        r["score"] = 0.30 * r["alive"] + 0.25 * r["moving"] + 0.25 * r["reach"] + 0.20 * r["apart"]

    rows.sort(key=lambda r: r["score"])
    print()
    print("%-34s %6s %6s %6s %6s %6s" % ("the weakest", "score", "alive", "moving", "reach", "apart"))
    for r in rows[:a.worst]:
        print("%-34s %6.2f %6.2f %6.2f %6.2f %6.2f"
              % (r["name"][:34], r["score"], r["alive"], r["moving"], r["reach"], r["apart"]))
    print()
    for key in ("score", "alive", "moving", "reach", "apart"):
        vals = sorted(r[key] for r in rows)
        print("%-7s median %.2f   worst %.2f   best %.2f" % (key, vals[len(vals) // 2], vals[0], vals[-1]))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump([{k: r[k] for k in ("name", "score", "alive", "moving", "reach", "apart", "rms")}
                       for r in rows], f, indent=1)
        print("wrote %s" % a.json)


if __name__ == "__main__":
    main()
