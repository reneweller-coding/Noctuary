"""The long loop into the library where it belongs (19.11.2026).

The long loop (loop_*, Effects.h LongLoop, in the engine and on the Foreground page since 20.10.2026) is Robert Rich's
sleep-concert loop: the near bus with its echoes circling on a tape of many seconds, each round darker and softer. No
preset of the library used it. Rene: "Zudem sollte er auch in Presets eingebaut werden, wo er sinnvoll ist."

Where: the packs whose music is held, slow and returns -- the sleep concert and the loop and tape packs most, the slow
chords, strings, hymns and night pieces often, the dark and far ones now and then; not the processions, drums,
machines, rituals, signals, weather, glass or field recordings. Not in a preset whose conductor hurries -- a chord
sooner than every twenty seconds or an event sooner than every six (Rich's own pace, events every eight to twenty
seconds since 13.11., keeps its loop) -- nor where the delays already carry much (Delay or Delay 2 above 0.35).
Drawn per preset from its name, so a rerun draws the same; a preset that has a loop already is left as it is.

Then the level. measure_packs.py measures twelve seconds on a held chord, which a loop of eight to sixteen seconds
does not reach. So every chosen preset is rendered twice for a minute as it plays (the conductor on, the night's clock
pinned at 21 h), once as it now is and once with the loop's Mix at nought. Where the loop adds more than 3 dB -- a
feedback near 0.9 sums a held sound over many rounds -- its Mix comes down by the excess and it is measured again, so
the loop plays with the sound and does not take it over. Then master_gain comes down by what the loop adds to the
integrated loudness -- the rest of the preset's calibration stays exactly as it was.

    python Tools/library/retrofit_loop.py select [--dry-run]   # write the loop into the packs, list the names
    python Tools/library/retrofit_loop.py measure [--jobs 4]    # the two renders of every chosen preset (resumable)
    python Tools/library/retrofit_loop.py trim                  # less Mix where the loop adds more than 3 dB
    python Tools/library/retrofit_loop.py measure               # the trimmed ones with their loop again
    python Tools/library/retrofit_loop.py gain                  # master_gain down by the loop's loudness
"""
import argparse
import concurrent.futures
import json
import os
import random
import re
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
WORK = os.path.join(ROOT, "work", "library")
NAMES = os.path.join(WORK, "loop_retrofit_names.txt")
CACHE = os.path.join(WORK, "loop_retrofit_measure.json")
RENDER = os.environ.get("AMBIENT_RENDER", os.path.join(ROOT, "bin", "msvc", "ambient_render.exe"))
LOW_PRIORITY = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}

#: pack -> the share of its presets that get a loop
CHANCE = {}
for _p in ("Sleep-Concert", "Loop-Studio", "River-Loops", "Arctic-Loop", "Tape-Saturation", "Dream-Depth",
           "Slow-Unfolding"):
    CHANCE[_p] = 0.6
for _p in ("Strings-at-Rest", "Sustain", "Vast-Chord", "Still-Meadow", "Long-Transit", "Water-Hymn", "Temple-of-Air",
           "Hybrid-Melancholy", "Guitar-Twilight", "Modular-Nocturne", "Fog-City", "Chamber-Grey", "Gentle-Systems",
           "Low-Brass-Swell", "Bowl-Temple"):
    CHANCE[_p] = 0.4
for _p in ("Northern-Dark", "Deep-Earth", "Void-Station", "Ghost-Signal", "Far-Relay", "Field-Absence",
           "Painted-Field"):
    CHANCE[_p] = 0.25
LOOP_KEYS = ("loop_mix", "loop_time", "loop_feedback", "loop_tone", "loop_drive", "loop_to_far")
GAIN_MIN, GAIN_MAX = -40.0, 12.0


def settings_of(fields):
    return dict(p.split("=", 1) for p in fields[1].split(";") if "=" in p)


def fits(kv):
    """The conductor does not hurry and the delays do not already carry much."""
    def num(k, d):
        try:
            return float(kv.get(k, d))
        except ValueError:
            return d
    if "loop_mix" in kv:
        return False
    chords = kv.get("auto_mode", "Free") not in ("Free", "0")
    if chords and num("auto_rate", 45) < 20:
        return False
    if not chords and num("brain_rate", 25) < 6:
        return False
    return num("dly_mix", 0.25) <= 0.35 and num("dly2_mix", 0.0) <= 0.35


def loop_settings(name, pack):
    rng = random.Random(zlib.crc32(("loop " + name).encode("utf-8")))
    sleep = CHANCE[pack] >= 0.6
    u = rng.uniform
    lg = lambda a, b: a * (b / a) ** rng.random()   # noqa: E731
    return {"loop_mix": "%.2f" % (u(0.2, 0.42) if sleep else u(0.15, 0.34)),
            "loop_time": "%.1f" % lg(8.0, 15.5),
            "loop_feedback": "%.2f" % (u(0.84, 0.93) if sleep else u(0.8, 0.9)),
            "loop_tone": "%.0f" % lg(1500.0, 4200.0),
            "loop_drive": "%.2f" % u(0.15, 0.45),
            "loop_to_far": "%.2f" % u(0.2, 0.5)}


def read_pack(path):
    raw = open(path, "rb").read()
    nl = b"\r\n" if b"\r\n" in raw else b"\n"
    return raw.decode("utf-8").split(nl.decode()), nl.decode()


def write_pack(path, lines, nl):
    open(path, "wb").write(nl.join(lines).encode("utf-8"))


def select(dry):
    chosen = []
    for fn in sorted(os.listdir(PACKS)):
        pack = fn[:-len(".ambientpack")] if fn.endswith(".ambientpack") else None
        if pack not in CHANCE:
            continue
        path = os.path.join(PACKS, fn)
        lines, nl = read_pack(path)
        changed = 0
        for i, line in enumerate(lines):
            if not line or line.startswith("#") or "|" not in line:
                continue
            fields = line.split("|")
            kv = settings_of(fields)
            if "master_gain" not in kv:   # a header line, not a preset
                continue
            name = fields[0]
            if random.Random(zlib.crc32(("pick " + name).encode("utf-8"))).random() >= CHANCE[pack] or not fits(kv):
                continue
            add = loop_settings(name, pack)
            fields[1] = fields[1].rstrip(";") + ";" + ";".join("%s=%s" % (k, add[k]) for k in LOOP_KEYS)
            lines[i] = "|".join(fields)
            chosen.append(name)
            changed += 1
        print("%-20s %3d presets get the loop" % (pack, changed))
        if changed and not dry:
            write_pack(path, lines, nl)
    print("%d presets in all" % len(chosen))
    if not dry:
        os.makedirs(WORK, exist_ok=True)
        open(NAMES, "w", encoding="utf-8").write("\n".join(chosen) + "\n")


LOUD = re.compile(r"^loudness: .*?lufs_i=(-?[\d.]+)", re.M)


def batch(names, extra):
    """Integrated loudness of each preset over a minute as it plays: {name: LUFS}."""
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
        f.write("\n".join(names))
        listing = f.name
    env = dict(os.environ, AMBIENT_PACKS=PACKS)
    env.pop("AMBIENT_MUTE", None)
    try:
        r = subprocess.run([RENDER, "--packs", PACKS, "--batch", listing, "--seconds", "60", "--hour", "21",
                            "--loudness"] + extra, capture_output=True, text=True, encoding="utf-8", errors="replace",
                           env=env, **LOW_PRIORITY)
    finally:
        os.remove(listing)
    out = {}
    chunks = re.split(r"^batch: (.*)$", r.stdout or "", flags=re.M)
    for i in range(1, len(chunks) - 1, 2):
        m = LOUD.search(chunks[i + 1])
        if m:
            out[chunks[i].strip()] = float(m.group(1))
    return out


def measure(jobs):
    """Every chosen preset with its loop and without (its Mix at nought); a preset whose loop was trimmed since is
    measured with it again (its "with" was dropped), the "without" stays."""
    names = [l.strip() for l in open(NAMES, encoding="utf-8") if l.strip()]
    cache = json.load(open(CACHE, encoding="utf-8")) if os.path.exists(CACHE) else {}
    todo = [n for n in names if "with" not in cache.get(n, {}) or "without" not in cache.get(n, {})]
    print("%d to measure, %d complete in the cache" % (len(todo), len(names) - len(todo)), flush=True)
    groups = [todo[i:i + 12] for i in range(0, len(todo), 12)]

    def one(g):
        on = batch(g, [])
        rest = [n for n in g if "without" not in cache.get(n, {})]
        return g, on, batch(rest, ["--set", "loop_mix=0"]) if rest else {}

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for k, (g, on, off) in enumerate(ex.map(one, groups)):
            for n in g:
                c = cache.setdefault(n, {})
                if n in on:
                    c["with"] = on[n]
                if n in off:
                    c["without"] = off[n]
                if "with" in c and "without" in c:
                    c["added_db"] = round(c["with"] - c["without"], 2)
            json.dump(cache, open(CACHE, "w", encoding="utf-8"), indent=0)
            if k % 10 == 0:
                print("%d of %d" % (sum(1 for n in names if "added_db" in cache.get(n, {})), len(names)), flush=True)
    print("measured %d" % sum(1 for n in names if "added_db" in cache.get(n, {})), flush=True)


def trim():
    """Where the loop adds more than 3 dB -- a feedback near 0.9 sums a held sound over many rounds, and a gain that
    took all of that back would leave the dry sound under its own loop -- its Mix comes down by the excess (at most to
    a quarter), and the preset is to be measured with its loop again (measure)."""
    cache = json.load(open(CACHE, encoding="utf-8"))
    cut = {}
    for fn in sorted(os.listdir(PACKS)):
        if not fn.endswith(".ambientpack"):
            continue
        path = os.path.join(PACKS, fn)
        lines, nl = read_pack(path)
        changed = 0
        for i, line in enumerate(lines):
            if not line or line.startswith("#") or "|" not in line:
                continue
            fields = line.split("|")
            c = cache.get(fields[0])
            if not c or c.get("added_db", 0.0) <= 3.0 or "trimmed" in c or "gain_done" in c:
                continue
            factor = max(0.25, 10.0 ** (-(c["added_db"] - 3.0) / 20.0))
            parts = fields[1].split(";")
            for j, p in enumerate(parts):
                if p.startswith("loop_mix="):
                    parts[j] = "loop_mix=%.2f" % (float(p.split("=", 1)[1]) * factor)
            fields[1] = ";".join(parts)
            lines[i] = "|".join(fields)
            c["trimmed"] = round(factor, 3)
            c["added_before_trim"] = c.pop("added_db")
            c.pop("with", None)
            cut[fields[0]] = factor
            changed += 1
        if changed:
            write_pack(path, lines, nl)
    json.dump(cache, open(CACHE, "w", encoding="utf-8"), indent=0)
    print("%d loops trimmed (the loop added more than 3 dB); measure them again" % len(cut))


def gain():
    cache = json.load(open(CACHE, encoding="utf-8"))
    moved = []
    for fn in sorted(os.listdir(PACKS)):
        if not fn.endswith(".ambientpack"):
            continue
        path = os.path.join(PACKS, fn)
        lines, nl = read_pack(path)
        changed = 0
        for i, line in enumerate(lines):
            if not line or line.startswith("#") or "|" not in line:
                continue
            fields = line.split("|")
            c = cache.get(fields[0])
            if not c or "gain_done" in c:
                continue
            if "added_db" not in c:   # trimmed and not measured again yet
                continue
            down = max(0.0, min(6.0, c["added_db"]))   # what the loop added, at most six decibels
            parts = fields[1].split(";")
            for j, p in enumerate(parts):
                if p.startswith("master_gain="):
                    g = float(p.split("=", 1)[1]) - down
                    parts[j] = "master_gain=%.4g" % min(max(g, GAIN_MIN), GAIN_MAX)
            fields[1] = ";".join(parts)
            lines[i] = "|".join(fields)
            c["gain_done"] = round(-down, 2)
            moved.append(down)
            changed += 1
        if changed:
            write_pack(path, lines, nl)
    json.dump(cache, open(CACHE, "w", encoding="utf-8"), indent=0)
    if moved:
        moved.sort()
        print("%d gains lowered: median %.2f dB, the largest %.2f dB" % (len(moved), moved[len(moved) // 2], moved[-1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("step", choices=("select", "measure", "trim", "gain"))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
    a = ap.parse_args()
    if a.step == "select":
        select(a.dry_run)
    elif a.step == "measure":
        measure(a.jobs)
    elif a.step == "trim":
        trim()
    else:
        gain()


if __name__ == "__main__":
    sys.exit(main())
