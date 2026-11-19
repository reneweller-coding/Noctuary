"""Noctuary -- the journey templates: presets in a row, per pack and across the packs.

A journey (Core/include/ambient/Journey.h) is a plain text file of presets, each held for a
while drawn from a range and crossfaded into the next; cyclic, so an evening plays itself. This
writes the templates the instrument ships in Library/Journeys, from the library's own
measurement (work/library/packs.json: the descriptors of every preset, fresh from the
rebuild) and the packs:

    python Tools/make_journeys.py                                   # every template
    python Tools/make_journeys.py --cache G:/.../work/library/packs.json

For every pack, two journeys of twelve:

  <Pack> - Journey    twelve presets spread across the pack's own space -- farthest-point
                      sampling on the standardised descriptors, so the twelve are the pack's
                      different faces and not its centre twelve times -- ordered as a chain of
                      nearest neighbours from the pack's medoid, so every crossfade is a step
                      to something close. A minute and a half to ten minutes each, by how much
                      the preset moves by itself (timings()), fades of twenty seconds to a
                      minute and a half.
  <Pack> - Night      the twelve stillest (the least flux, the least evolution, the lowest
                      centroid), chained the same way, three to fifteen minutes each, fades of
                      under a minute to two and a half: the version for falling asleep to.

    python Tools/make_journeys.py --retime    # only the dwells and fades of the templates there are

And for every family of artists (the sleep records, the deep ones, the ritual ones, the cold,
the British, the luminous, the space ones -- Tools/library/near_by_artist.json says which pack
is which), a crossing: twelve presets, every step the nearest neighbour in a different pack from
the last two, so the evening walks from artist to artist without a seam.

Nothing here listens. The descriptors are what the map is built from, and a nearest neighbour
in that space is a preset that measures alike -- which is the promise of a gentle crossfade,
not of a good one. Rene's own journeys, written in the plugin, go beside these.
"""
import argparse
import importlib.util
import json
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
PACKS = os.path.join(ROOT, "Library", "Packs")
OUT = os.path.join(ROOT, "Library", "Journeys")
FAMILY_TITLES = {"sleep": "Sleep", "deep": "Deep", "ritual": "Ritual", "cold": "Cold", "brit": "British",
                 "luminous": "Luminous", "space": "Space"}


def pack_presets():
    """pack title -> [preset names], in the packs' own order."""
    out = {}
    for f in sorted(os.listdir(PACKS)):
        if not f.endswith(".ambientpack"):
            continue
        title, names = None, []
        with open(os.path.join(PACKS, f), encoding="utf-8", errors="replace") as fh:
            for line in fh:
                t = line.strip()
                if not t or t.startswith("#"):
                    continue
                if t.startswith("pack "):
                    title = t[5:].strip()
                    continue
                if t.startswith("format "):
                    continue
                names.append(t.split("|", 1)[0].strip())
        if title and names:
            out[title] = names
    return out


def pack_families():
    """pack title -> family, through the artists (near_by_artist.json) and their packs (artists.py)."""
    sys.path.insert(0, os.path.join(HERE, "library"))
    spec = importlib.util.spec_from_file_location("artists", os.path.join(HERE, "library", "artists.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    by_artist = {a["inspiration"]: a["name"] for a in m.ARTISTS}
    with open(os.path.join(HERE, "library", "near_by_artist.json"), encoding="utf-8") as f:
        table = json.load(f)
    fam = {}
    for artist, a in table["artists"].items():
        if artist in by_artist:
            fam[by_artist[artist]] = a.get("family", "sleep")
    return fam


def features(row):
    """The descriptors that matter for 'sounds alike', on scales where a unit is a real step."""
    t = row.get("timbre") or []
    t = (t + [0.0] * 8)[:8]
    return [math.log(max(row.get("centroid", 500.0), 30.0)),
            math.log(max(row.get("flatness", 0.001), 1e-5)) * 0.4,
            row.get("flux", 0.5) * 2.0,
            row.get("bass", 0.3) * 2.0,
            row.get("width", 0.5),
            row.get("evo_tone", 0.3) * 2.0,
            math.log1p(max(row.get("evo_level", 1.0), 0.0)) * 0.7,
            row.get("rms_db", -28.0) / 12.0,
            row.get("rough", 0.01) * 20.0,
            row.get("wet", 0.2) * 1.5] + [x * 0.5 for x in t]


def standardise(vecs):
    n, d = len(vecs), len(vecs[0])
    mean = [sum(v[k] for v in vecs) / n for k in range(d)]
    sd = [math.sqrt(sum((v[k] - mean[k]) ** 2 for v in vecs) / n) or 1.0 for k in range(d)]
    return [[(v[k] - mean[k]) / sd[k] for k in range(d)] for v in vecs]


def dist(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def medoid(idx, vecs):
    best, bestd = idx[0], float("inf")
    for i in idx:
        s = sum(dist(vecs[i], vecs[j]) for j in idx)
        if s < bestd:
            best, bestd = i, s
    return best


def farthest_points(idx, vecs, k, start):
    chosen = [start]
    while len(chosen) < min(k, len(idx)):
        far, fard = None, -1.0
        for i in idx:
            if i in chosen:
                continue
            d = min(dist(vecs[i], vecs[c]) for c in chosen)
            if d > fard:
                far, fard = i, d
        chosen.append(far)
    return chosen


def chain(chosen, vecs, start):
    """Nearest-neighbour order from `start`: every crossfade a step to something close."""
    left = [c for c in chosen if c != start]
    order = [start]
    while left:
        nxt = min(left, key=lambda i: dist(vecs[i], vecs[order[-1]]))
        order.append(nxt)
        left.remove(nxt)
    return order


# How long each step stays (19.11.2026). Rene: "In den Journeys scheinen die Presets auch alle genau gleich
# lang aktiv zu sein (und das auch noch sehr, sehr lange). Waere da nicht etwas mehr Abwechslung
# spannender?" Every step had the journey's one range, five to ten minutes (eight to fifteen at night),
# drawn evenly: twelve stays of seven and a half minutes on average, none short. Now a step's length is
# its preset's: one that moves by itself (its level and its tone evolving over the minute, the map's
# evo_level and evo_tone) can carry a long stay, a still one says what it has to say sooner. The four
# that move least of the twelve get a short range, the four that move most a long one, the rest a middle
# one; two long ones never follow each other, nor two short ones, and the fade grows with the stay.
TIMINGS = {   # short, middle, long: (dwell, fade)
    "journey": [("1:30-3:00", "0:20-0:45"), ("3:00-6:00", "0:30-1:15"), ("6:00-10:00", "0:45-1:30")],
    "night": [("3:00-6:00", "0:45-1:30"), ("6:00-10:00", "1:00-2:00"), ("10:00-15:00", "1:30-2:30")],
}


def timings(presets, cache, kind):
    """(dwell, fade) for each of `presets`, in their order, by how much each moves by itself."""
    def motion(p):
        r = cache.get(p, {})
        return math.log1p(max(r.get("evo_level", 1.6), 0.0)) + 2.0 * r.get("evo_tone", 0.42)
    n = len(presets)
    rank = sorted(range(n), key=lambda i: motion(presets[i]))
    third = max(1, n // 3)
    cls = [1] * n
    for i in rank[:third]:
        cls[i] = 0
    for i in rank[n - third:]:
        cls[i] = 2
    # No two long and no two short ones in a row (the journeys are cyclic: the last meets the first).
    for i in range(n):
        j = (i + 1) % n
        if j != i and cls[i] == cls[j] and cls[j] != 1:
            cls[j] = 1
    return [TIMINGS[kind][c] for c in cls]


def retime(cache):
    """Rewrites the dwell and fade of every template in Library/Journeys by timings(), keeping its presets, their
    order and anything after the fade (a near= field). Only the generated templates are touched."""
    changed = 0
    for fn in sorted(os.listdir(OUT)):
        if not fn.endswith(".journey"):
            continue
        path = os.path.join(OUT, fn)
        lines = open(path, encoding="utf-8").read().split("\n")
        if not any(l.startswith("# Made by Tools/make_journeys.py") for l in lines):
            continue
        steps = [i for i, l in enumerate(lines) if l.strip() and not l.startswith("#") and "|" in l]
        kind = "night" if fn.endswith(" - Night.journey") else "journey"
        times = timings([lines[i].split("|")[0].strip() for i in steps], cache, kind)
        for i, (dwell, fade) in zip(steps, times):
            parts = [x.strip() for x in lines[i].split("|")]
            parts[1:3] = [dwell, fade]
            lines[i] = " | ".join(parts)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(lines))
        changed += 1
    print("retimed %d journeys in %s" % (changed, OUT))


def write(path, name, steps, cyclic=True):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("# Noctuary journey: <preset> | <dwell m:ss or a range> | <fade> [| near=<near preset, auto or keep>]\n")
        f.write("# Made by Tools/make_journeys.py from the library's measurement; edit freely, or write your own beside it.\n")
        f.write("journey %s\n" % name)
        f.write("cyclic %s\n" % ("on" if cyclic else "off"))
        for preset, dwell, fade in steps:
            f.write("%s | %s | %s\n" % (preset, dwell, fade))


def safe(s):
    return re.sub(r"[^A-Za-z0-9 \-_.']+", " ", s).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", default=os.path.join(ROOT, "work", "library", "packs.json"))
    ap.add_argument("--per-journey", type=int, default=12)
    ap.add_argument("--retime", action="store_true",
                    help="only rewrite the dwell and fade of the templates there are (timings())")
    a = ap.parse_args()
    with open(a.cache, encoding="utf-8") as f:
        cache = json.load(f)
    if a.retime:
        retime(cache)
        return
    packs = pack_presets()
    fams = pack_families()
    names, vecs, owner = [], [], []
    for title, plist in packs.items():
        for p in plist:
            row = cache.get(p)
            if row is None:
                continue
            names.append(p); vecs.append(features(row)); owner.append(title)
    print("%d presets of %d packs measured" % (len(names), len(packs)))
    z = standardise(vecs)
    written = 0
    for title in packs:
        idx = [i for i in range(len(names)) if owner[i] == title]
        if len(idx) < a.per_journey:
            continue
        zt = standardise([vecs[i] for i in idx])          # the pack's own scale: its faces, not the library's
        local = {i: zt[k] for k, i in enumerate(idx)}
        med = medoid(idx, local)
        chosen = farthest_points(idx, local, a.per_journey, med)
        order = chain(chosen, local, med)
        write(os.path.join(OUT, "%s - Journey.journey" % safe(title)), "%s: Journey" % title,
              [(names[i],) + t for i, t in zip(order, timings([names[i] for i in order], cache, "journey"))])
        # The stillest: low flux, little evolution, a low centroid, a low roughness.
        def stillness(i):
            r = cache[names[i]]
            return r.get("flux", 0.5) * 2.0 + math.log1p(max(r.get("evo_level", 1.0), 0.0)) + math.log(max(r.get("centroid", 500.0), 30.0)) * 0.5 + r.get("rough", 0.01) * 20.0
        still = sorted(idx, key=stillness)[:a.per_journey]
        smed = medoid(still, local)
        write(os.path.join(OUT, "%s - Night.journey" % safe(title)), "%s: Night" % title,
              [(names[i],) + t for i, t in zip(chain(still, local, smed),
                                               timings([names[i] for i in chain(still, local, smed)], cache, "night"))])
        written += 2
    # The crossings: per family, a walk from pack to pack through the library's space.
    for fam, ftitle in FAMILY_TITLES.items():
        idx = [i for i in range(len(names)) if fams.get(owner[i]) == fam]
        if len(idx) < a.per_journey:
            continue
        med = medoid(idx[::max(1, len(idx) // 400)], z)   # a medoid of a thinning, the full one is quadratic
        # Every pack of the family once before any comes again: the nearest neighbour in a pack
        # not yet visited, and when all have been, the round begins anew (keeping the last two
        # out, so a family of three still alternates).
        order, visited = [med], [owner[med]]
        while len(order) < a.per_journey:
            cands = [i for i in idx if i not in order and owner[i] not in visited]
            if not cands:
                visited = visited[-2:]
                cands = [i for i in idx if i not in order and owner[i] not in visited]
                if not cands:
                    break
            nxt = min(cands, key=lambda i: dist(z[i], z[order[-1]]))
            order.append(nxt); visited.append(owner[nxt])
        write(os.path.join(OUT, "%s - Crossing.journey" % ftitle), "%s: Crossing" % ftitle,
              [(names[i],) + t for i, t in zip(order, timings([names[i] for i in order], cache, "journey"))])
        print("  %s crossing: %s" % (ftitle, " > ".join(owner[i] for i in order)))
        written += 1
    print("wrote %d journeys into %s" % (written, OUT))


if __name__ == "__main__":
    main()
