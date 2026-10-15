"""The twelve routes over the map, written from the library rather than by hand.

A route is a list of waypoints the synth walks by itself, and a waypoint is a preset NAME -- so
every route named presets that a regenerated library no longer has, and eleven of the twelve broke
the moment the built-ins were rebuilt. The names were the only thing holding them; the intent was
never written down anywhere a tool could read.

So it is written down here. Each route says which families it may draw from and what it should feel
like at each stop -- darker, brighter, denser, further out -- and this picks the built-in preset
that fits, preferring one that is FAR on the map from the stop before it, because a route whose
waypoints sit on top of each other is a route that does not travel. Only built-ins are eligible:
a route that named a pack preset would break for anyone without that pack.

    python Tools/library/make_routes.py            rewrite Core/src/Route.cpp
    python Tools/library/make_routes.py --check     report, write nothing
"""
import argparse
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
META = os.path.join(ROOT, "Core", "src", "PresetMeta.cpp")
ROUTE = os.path.join(ROOT, "Core", "src", "Route.cpp")

# The character of a stop, as the six ranked descriptors the map itself is drawn from. None means
# "do not care", which is most of them: a stop is described by the two or three things that make it
# what it is, not by all six.
FIELDS = ("bright", "motion", "width", "noisy", "bass", "density")


def wp(travel, hold, family, same=None, **want):
    # `same` returns to the preset an earlier stop chose. Two of the routes are journeys out and
    # back, and coming home to a DIFFERENT preset of the same family is not coming home.
    return {"travel": travel, "hold": hold, "family": family, "same": same, "want": want}


# The twelve, with their original names and timings. A whole route is a 20-40 minute set at speed 1.
ROUTES = [
    ("Night Descent", [
        wp(30, 90, "Just Drones", bright=0.5, density=0.4),
        wp(90, 120, "Just Drones", bright=0.3, density=0.6),
        wp(120, 180, "Slow Worlds", bright=0.2, motion=0.2),
        wp(150, 240, "Deep and Sub", bright=0.1, bass=0.8),
        wp(180, 300, "Deep and Sub", bright=0.05, bass=0.95, density=0.3),
    ]),
    ("Glass to Storm", [
        wp(30, 120, "Glass and Bells", bright=0.8, noisy=0.2),
        wp(90, 120, "Glass and Bells", bright=0.9, density=0.6),
        wp(90, 90, "Clouds and Memory", bright=0.6, motion=0.6),
        wp(150, 180, "Weather and Noise", noisy=0.9, motion=0.8),
        wp(120, 120, "Weather and Noise", noisy=0.8, bass=0.6),
        wp(240, 120, "Glass and Bells", same=0),
    ]),
    ("Breath and Choir", [
        wp(30, 120, "Choirs and Vowels", bright=0.4, density=0.3),
        wp(120, 150, "Choirs and Vowels", bright=0.6, density=0.7),
        wp(120, 180, "Choirs and Vowels", bright=0.8, width=0.8),
        wp(150, 180, "Organs and Reeds", bright=0.5, density=0.8),
        wp(180, 120, "Choirs and Vowels", bright=0.35, density=0.35),
    ]),
    ("Cosmos Crossing", [
        wp(30, 120, "Cosmos", bright=0.4, width=0.6),
        wp(150, 180, "Cosmos", bright=0.7, motion=0.7),
        wp(150, 180, "Space and Motion", width=0.95, bright=0.6),
        wp(120, 180, "Space and Motion", width=0.8, bass=0.6),
        wp(200, 240, "Cosmos", bright=0.25, motion=0.25),
    ]),
    ("Slow Tide Loop", [
        wp(30, 150, "Slow Worlds", motion=0.15, density=0.4),
        wp(120, 150, "Slow Worlds", motion=0.35, bright=0.6),
        wp(120, 150, "Slow Worlds", motion=0.25, bright=0.3),
        wp(120, 150, "Slow Worlds", motion=0.1, density=0.5),
    ]),
    ("Sub Journey", [
        wp(30, 150, "Deep and Sub", bass=0.6, density=0.3),
        wp(150, 180, "Deep and Sub", bass=0.8, bright=0.2),
        wp(150, 180, "Deep and Sub", bass=0.95, density=0.6),
        wp(150, 150, "Metal and Feedback", bass=0.8, noisy=0.6),
        wp(180, 180, "Deep and Sub", bass=0.85, motion=0.15),
    ]),
    ("Keys Interlude", [
        wp(20, 120, "Played Keys", bright=0.4, density=0.3),
        wp(60, 120, "Played Keys", bright=0.6, density=0.5),
        wp(90, 120, "Played Keys", bright=0.75, width=0.6),
        wp(90, 150, "Generative Chords", bright=0.5, density=0.7),
        wp(120, 120, "Played Keys", bright=0.3, density=0.25),
    ]),
    ("Metal and Feedback", [
        wp(30, 150, "Metal and Feedback", bright=0.5, noisy=0.4),
        wp(120, 150, "Metal and Feedback", bright=0.7, motion=0.7),
        wp(120, 150, "Strike and Modal", bright=0.8, motion=0.6),
        wp(150, 180, "Metal and Feedback", noisy=0.8, bass=0.7),
        wp(180, 150, "Metal and Feedback", bright=0.3, motion=0.2),
    ]),
    ("Ninety Minute Arc", [
        wp(30, 600, "Slow Worlds", motion=0.1, density=0.3),
        wp(300, 900, "Slow Worlds", motion=0.5, density=0.7),
        wp(600, 900, "Slow Worlds", motion=0.15, density=0.35),
    ]),
    ("Storm Front", [
        wp(30, 150, "Weather and Noise", noisy=0.7, motion=0.5),
        wp(120, 120, "Weather and Noise", noisy=0.9, motion=0.9),
        wp(120, 150, "Clouds and Memory", noisy=0.6, motion=0.7),
        wp(150, 180, "Weather and Noise", noisy=0.8, bass=0.7),
        wp(180, 240, "Weather and Noise", noisy=0.5, motion=0.2),
    ]),
    # Two presets as far apart as the library goes, and back -- the route that shows the map is a
    # space rather than a list. The middle stop is chosen for distance alone.
    ("Between Two Worlds", [
        wp(30, 60, "Just Drones", bright=0.3, noisy=0.05),
        wp(300, 60, "Weather and Noise", noisy=0.95, motion=0.9),
        wp(300, 60, "Just Drones", same=0),
    ]),
]
# The twelfth walks the corners of the map itself, so it needs no preset at all.
CORNERS = ("Wide Wander",
           "0.15,0.85|30|60|0.14;0.85,0.85|240|60|0.14;0.85,0.15|240|60|0.14;0.15,0.15|240|60|0.14;0.5,0.5|240|120|0.2")


def read_meta():
    text = open(META, encoding="utf-8", errors="replace").read()
    fams = re.search(r"const char\* const Families\[[^\]]*\]\s*=\s*\{(.*?)\};", text, re.S)
    families = re.findall(r'"([^"]*)"', fams.group(1)) if fams else []
    out = []
    for line in text.splitlines():
        m = re.match(r"\s*\{\s*([-\d.f, ]+?)\s*,\s*(\d+)\s*,\s*0x[0-9a-f]+u\s*,.*?//\s*(.*?)\s*$", line)
        if not m:
            continue
        nums = [float(v.strip().rstrip("f")) for v in m.group(1).split(",")]
        if len(nums) < 8:
            continue
        out.append({"name": m.group(3), "family": families[int(m.group(2))] if families else "",
                    **dict(zip(("x", "y") + FIELDS, nums[:8]))})
    return out, families


def pick(presets, stop, taken, previous):
    """The preset that fits this stop best: near the wanted character, in the wanted family, and
    away from the stop before it. Distance counts for a quarter -- enough to break a tie between
    two presets that fit equally, never enough to choose one that does not fit."""
    best, bestScore = None, -1e9
    for p in presets:
        if p["name"] in taken or (stop["family"] and p["family"] != stop["family"]):
            continue
        miss = sum((p[k] - v) ** 2 for k, v in stop["want"].items())
        score = -miss
        if previous is not None:
            score += 0.25 * ((p["x"] - previous["x"]) ** 2 + (p["y"] - previous["y"]) ** 2) ** 0.5
        if score > bestScore:
            best, bestScore = p, score
    return best


def build(presets):
    routes, report = [], []
    for name, stops in ROUTES:
        taken, previous, items, chosen = set(), None, [], []
        for stop in stops:
            if stop["same"] is not None:
                p = chosen[stop["same"]]
            else:
                p = pick(presets, stop, taken, previous)
                if p is None:
                    raise SystemExit(f"{name}: nothing in family {stop['family']!r} left to choose")
                taken.add(p["name"])
            previous = p
            chosen.append(p)
            items.append(f"{p['name']}|{stop['travel']}|{stop['hold']}")
        routes.append((name, ";".join(items)))
        span = max(((a["x"] - b["x"]) ** 2 + (a["y"] - b["y"]) ** 2) ** 0.5
                   for a in presets if a["name"] in taken for b in presets if b["name"] in taken)
        report.append((name, len(items), span))
    routes.append(CORNERS)
    return routes, report


def write(routes):
    text = open(ROUTE, encoding="utf-8", errors="replace").read()
    body = ["const RoutePreset Routes[] = {"]
    for name, points in routes:
        body.append(f'    {{ "{name}",')
        body.append(f'      "{points}" }},')
    body.append("};")
    new = re.sub(r"const RoutePreset Routes\[\] = \{.*?\n\};", "\n".join(body), text, count=1, flags=re.S)
    if new == text:
        raise SystemExit("the Routes block was not found in Route.cpp")
    open(ROUTE, "w", encoding="utf-8", newline="\n").write(new)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--check", action="store_true", help="report, write nothing")
    a = ap.parse_args()
    presets, families = read_meta()
    if len(presets) < 200:
        raise SystemExit(f"only {len(presets)} presets read from {META} -- has the map been laid out?")
    print(f"{len(presets)} built-ins in {len(families)} families")
    routes, report = build(presets)
    for name, n, span in report:
        print(f"  {name:22s} {n} stops, widest step {span:.2f} of the map")
    for name, points in routes:
        print(f"\n{name}\n  {points}")
    if a.check:
        print("\ncheck only: nothing written")
        return 0
    write(routes)
    print(f"\nwrote {len(routes)} routes into {ROUTE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
