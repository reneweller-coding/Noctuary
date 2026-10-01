"""One map for the whole library.

Until now two tools laid out two maps that shared a plane: Tools/preset_map.py embedded the
built-ins and Tools/library/measure_packs.py embedded the pack presets, each standardising
its own descriptors and running its own projection. The browser then drew both on one square, so
a built-in and a pack preset at the same spot had nothing to do with each other.

This tool does the layout once, over everything: it measures the built-ins, reads the packs'
cached measurements, ranks the nine descriptors across the union, lays the whole library out as
one cloud (Tools/library/mapembed.py), groups it, and writes

    Core/src/PresetMeta.cpp      the built-ins' table
    Core/src/PresetClusters.inc  the groups: a name and a centroid each
    Core/src/PresetPhrases.inc   the vocabulary CLAP chose the "sounds like" line from
    Library/Packs/*.ambientpack  the packs' meta field, rewritten in place

  python Tools/library/map_all.py --pack-cache mcache.json [--builtin-cache bcache.json]

Nothing about the sound is touched: only positions, descriptors, tags and groups.
"""
import argparse
import concurrent.futures
import json
import math
import os
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "Tools"))

import mapembed                                        # noqa: E402
from measure_packs import read_pack, write_pack, LOW_PRIORITY, MEASURE   # noqa: E402
import preset_map as PM                                # noqa: E402

RENDER = os.path.join(ROOT, "bin", "msvc", "ambient_render.exe")
PACKS = os.path.join(ROOT, "Library", "Packs")
OUT_INC = os.path.join(ROOT, "Core", "src", "PresetClusters.inc")
OUT_PHRASES = os.path.join(ROOT, "Core", "src", "PresetPhrases.inc")

# The nine descriptors, in the order the C++ table reads them.
COLS = mapembed.COLS
SECONDS = 60.0


# The engine loads its default pack folders unless AMBIENT_PACKS says otherwise, and a stale copy
# of an older library in ProgramData then wins every name collision -- which is how a measurement
# pass came to measure last week's presets under this week's names. Every render here is told
# exactly which folder to read.
def _packs_env(packs):
    env = dict(os.environ)
    env["AMBIENT_PACKS"] = os.path.abspath(packs)
    return env

def slug(name):
    """The name a preset's excerpt is filed under -- the same rule measure_packs.py uses, so a
    built-in and a pack preset are looked up in the CLAP file the same way."""
    return re.sub(r"[^A-Za-z0-9]+", "_", name)[:80]


def measure_builtin(name, tapdir=None):
    """The same instrument, the same conditions as the packs: three keys held, the conductor at
    six seconds, a minute of it. --dump comes along so the parameter flags can be read too."""
    # --hour pins the arc clock to the same hour measure_packs uses. Without it the built-ins are
    # measured at whatever time the pass happens to run and the packs at 21:00, and the ranks --
    # which are what the map's axes are made of -- would mix two different conditions.
    cmd = [RENDER, "--preset", name, "--seconds", str(SECONDS), "--notes", "45,52,59",
           "--set", "brain_rate=6", "--hour", "21", "--measure", "--dump"]
    if tapdir:
        cmd += ["--tap", os.path.join(tapdir, slug(name) + ".wav")]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=_packs_env(PACKS), **LOW_PRIORITY)
    if res.returncode != 0:
        return None
    m = MEASURE.search(res.stdout or "")
    if not m:
        return None
    d = {}
    for tok in m.group(1).split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                d[k] = float(v)
            except ValueError:
                continue
    tm = re.search(r"^timbre:\s*(.*)$", res.stdout or "", re.M)
    if tm:
        try:
            d["timbre"] = [float(x) for x in tm.group(1).split()]
        except ValueError:
            pass
    params = {}
    for line in res.stdout.splitlines():
        mm = re.match(r"^param\s+(\S+)\s*=\s*(.+)$", line)
        if mm:
            params[mm.group(1)] = mm.group(2).strip()
    d["params"] = params
    return d


def descriptors(m):
    """The raw numbers a preset is placed by. Brightness is heard logarithmically; the two
    evolution figures (how far the tone colour and the level wander over the minute) are one
    axis, so they are averaged after ranking."""
    return {
        "bright": np.log(max(m.get("centroid", 1.0), 1.0)),
        "motion": m.get("flux", 0.0),
        "width": m.get("width", 0.0),
        "noisy": m.get("flatness", 0.0),
        "bass": m.get("bass", 0.0),
        "density": m.get("voices", 0.0),
        "evo_tone": m.get("evo_tone", 0.0),
        "evo_level": m.get("evo_level", 0.0),
        "rough": m.get("rough", 0.0),
        "wet": m.get("wet", 0.0),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--pack-cache", required=True, help="the JSON written by measure_packs.py --cache")
    ap.add_argument("--builtin-cache", default="", help="reuse the built-ins' measurements from here")
    ap.add_argument("--clusters", type=int, default=14)
    ap.add_argument("--clap", default="", help="the JSON written by clap_embed.py: what it sounds like")
    ap.add_argument("--taps", default="", help="where the built-ins' 12 s excerpts go when measuring")
    # Both default to zero, and that is a measurement rather than a preference. Laying the whole
    # library out five ways and judging each against the SAME yardsticks (not against its own
    # space, which flatters the poorer one):
    #
    #     space                      neighbours kept   ... by CLAP   x~bright   y~evolve
    #     descriptors only                     0.160         0.011       0.86       0.26
    #     + fingerprint                        0.068         0.013       0.88       0.22
    #     + CLAP                               0.050         0.031       0.86       0.12
    #     all three                            0.039         0.032       0.87       0.18
    #
    # The two learned blocks cost three quarters of the neighbourhoods a person can name, and a
    # third of what the up-axis promises, to raise the CLAP neighbourhoods from 1 % to 3 % -- which
    # is no use to anybody either way. So they are measured, they name the presets, and they stay
    # out of the plane. Set them if you want to see it for yourself.
    ap.add_argument("--w-timbre", type=float, default=0.0, help="pull of the mel-cepstral fingerprint on the layout")
    ap.add_argument("--w-clap", type=float, default=0.0, help="pull of the learned embedding on the layout")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--dry-run", action="store_true", help="report, write nothing")
    a = ap.parse_args()

    # ---- the built-ins ---------------------------------------------------------------------
    # --list-presets answers with everything the engine can load, and it loads the pack library
    # from the user's data folder without being asked: 6996 names, of which 196 are built in.
    # Measuring the packs again here would take three hours and would be measuring what the
    # cache already holds, so the pack names are taken out by name.
    packNames = set()
    for f in sorted(os.listdir(PACKS)):
        if not f.endswith(".ambientpack"):
            continue
        for line in open(os.path.join(PACKS, f), encoding="utf-8", errors="replace"):
            if line.startswith("#") or line.startswith("pack ") or "|" not in line:
                continue
            packNames.add(line.split("|", 1)[0].strip())
    names = [n.strip() for n in subprocess.run([RENDER, "--list-presets"], capture_output=True, text=True,
                                               encoding="utf-8", env=_packs_env(PACKS)).stdout.splitlines() if n.strip()]
    names = [n for n in names if n not in packNames]
    built = {}
    if a.builtin_cache and os.path.exists(a.builtin_cache):
        built = json.load(open(a.builtin_cache, encoding="utf-8"))
        print(f"built-ins: {len(built)} measurements read from {a.builtin_cache}")
    else:
        print(f"built-ins: measuring {len(names)} presets at {SECONDS:.0f} s, {a.jobs} at a time", flush=True)
        if a.taps:
            os.makedirs(a.taps, exist_ok=True)
        done = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            for name, m in zip(names, ex.map(lambda n: measure_builtin(n, a.taps or None), names)):
                if m:
                    built[name] = m
                done += 1
                if done % 25 == 0:
                    print(f"  {done}/{len(names)}", flush=True)
        if a.builtin_cache:
            json.dump(built, open(a.builtin_cache, "w", encoding="utf-8"))
    missing = [n for n in names if n not in built]
    if missing:
        print(f"  {len(missing)} built-ins did not render: {', '.join(missing[:5])}")

    # ---- the packs -------------------------------------------------------------------------
    cached = json.load(open(a.pack_cache, encoding="utf-8"))
    files = sorted(f for f in os.listdir(PACKS) if f.endswith(".ambientpack"))
    packs = [(f, *read_pack(os.path.join(PACKS, f))) for f in files]
    packRows = [r for _, _, rr in packs for r in rr]
    for r in packRows:
        r["m"] = cached.get(r["name"])
    packGood = [r for r in packRows if r["m"]]
    print(f"packs: {len(packGood)} of {len(packRows)} presets measured")

    # ---- one table, one ranking, one cloud --------------------------------------------------
    rows = []
    for i, n in enumerate(names):
        if n in built:
            rows.append({"kind": "builtin", "index": i, "name": n, "m": built[n], "params": built[n].get("params", {})})
    for r in packGood:
        rows.append({"kind": "pack", "name": r["name"], "m": r["m"], "row": r})
    if not rows:
        raise SystemExit("nothing measured")
    raw = np.array([[descriptors(r["m"])[k] for k in
                     ("bright", "motion", "width", "noisy", "bass", "density", "evo_tone", "evo_level", "rough", "wet")]
                    for r in rows], dtype=np.float64)
    rk = np.column_stack([PM.rank(raw[:, c]) for c in range(raw.shape[1])])
    desc = np.column_stack([rk[:, 0], rk[:, 1], rk[:, 2], rk[:, 3], rk[:, 4], rk[:, 5],
                            PM.rank(0.5 * rk[:, 6] + 0.5 * rk[:, 7]),   # evolve: tone and level together
                            rk[:, 8], rk[:, 9]])
    print(f"union: {len(rows)} presets, {desc.shape[1]} descriptors")

    # ---- what it is like, what it is, what it sounds like ------------------------------------
    # Three ways of describing the same minute of sound, and the layout uses all three. The nine
    # descriptors measure properties a person can name. The mel-cepstral fingerprint measures the
    # shape of the spectrum itself, which is how two presets with identical descriptors can still
    # be a bell and a beehive. CLAP measures what a model that has heard a lot of the world thinks
    # it is. Each block is reduced and scaled so it contributes the weight it is given and no more.
    def block(M, keep, weight):
        M = np.asarray(M, dtype=np.float64)
        M = (M - M.mean(axis=0)) / (M.std(axis=0) + 1e-9)
        keep = min(keep, M.shape[1])
        U, S, _ = np.linalg.svd(M - M.mean(axis=0), full_matrices=False)
        P = U[:, :keep] * S[:keep]
        P = P / (P.std(axis=0) + 1e-9)
        return P * np.sqrt(weight / max(keep, 1))

    parts = [(desc - desc.mean(axis=0)) / (desc.std(axis=0) + 1e-9)]
    tim = [r["m"].get("timbre") for r in rows]
    width = max((len(t) for t in tim if t), default=0)
    haveT = sum(1 for t in tim if t and len(t) == width)
    if width and haveT >= 0.9 * len(rows) and a.w_timbre > 0:
        # A preset without a fingerprint sits at the centre of the block rather than being dropped.
        T = np.array([t if (t and len(t) == width) else [0.0] * width for t in tim], dtype=np.float64)
        parts.append(block(T, 6, a.w_timbre))
        print(f"fingerprint: {haveT} of {len(rows)} presets, {width} numbers each")
    clap = {}
    scores = None
    if a.clap and os.path.exists(a.clap):
        blob = json.load(open(a.clap, encoding="utf-8"))
        vocab, groups, clap = blob["phrases"], blob["groups"], blob["presets"]
        # Two presets whose names differ only in punctuation share one excerpt file. Neither is
        # described rather than both being described by whichever render landed last.
        seen = {}
        for r in rows:
            seen.setdefault(slug(r["name"]), []).append(r["name"])
        dup = {s for s, v in seen.items() if len(v) > 1}
        hit = [clap.get(slug(r["name"])) if slug(r["name"]) not in dup else None for r in rows]
        haveC = sum(1 for h in hit if h)
        print(f"CLAP: {haveC} of {len(rows)} presets heard, {len(vocab)} phrases"
              + (f", {len(dup)} name collisions skipped" if dup else ""))
        if haveC >= 0.5 * len(rows):
            dim = len(next(x for x in hit if x)["embedding"])
            E = np.array([h["embedding"] if h else [0.0] * dim for h in hit], dtype=np.float64)
            if a.w_clap > 0:
                parts.append(block(E, 6, a.w_clap))
            scores = np.array([h["scores"] if h else [np.nan] * len(vocab) for h in hit], dtype=np.float64)
    space = np.column_stack(parts)
    print(f"layout space: {space.shape[1]} axes from {len(parts)} block(s)"
          + ("" if len(parts) > 1 else " -- the nine descriptors alone; see --w-timbre/--w-clap"))

    xy = mapembed.embed(desc, axes=(0, 6), graph=space)
    labels, clusterNames = mapembed.cluster(desc, k=a.clusters, space=space)
    tw_new = mapembed.trustworthiness(space, xy, standardize=False)
    print(f"cloud: neighbourhoods kept {tw_new:.3f}")
    for j, nm in enumerate(clusterNames):
        print(f"   {nm:22s} {int((labels == j).sum()):5d}")

    # ---- tags ------------------------------------------------------------------------------
    def tags_of(r, d):
        if r["kind"] == "builtin":
            t = set(PM.flags_from_params(r["params"]))
        else:
            t = set()
            p = {}
            for kv in r["row"]["settings"].split(";"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    p[k] = v
            num = lambda k: float(p.get(k, "0") or 0)
            t.add("Keys" if p.get("brain_on") == "off" else "Generative")
            if num("cosmos_send") > 0.05 or num("cosmos_shimmer") > 0.05: t.add("Cosmos")
            if num("fb_bus") > 0.02 or num("fb_fm") > 0.02: t.add("Feedback")
            if p.get("src2_type", "Off") != "Off" or p.get("src3_type", "Off") != "Off": t.add("Sources")
            if any(k in p.get("scale", "") for k in ("JI", "Harmonic", "Subharmonic", "Pythag", "Bohlen", "Otonal", "Slendro")):
                t.add("JustIntonation")
            if num("sub_level") > 0.05: t.add("Sub")
            if p.get("stack", "Detune") != "Detune": t.add("Stack")
            if num("air") >= 0.3: t.add("Air")
        if d[0] < 0.3: t.add("Dark")
        if d[0] > 0.7: t.add("Bright")
        if d[1] < 0.3: t.add("Calm")
        if d[1] > 0.7: t.add("Moving")
        if d[3] < 0.4: t.add("Tonal")
        if d[3] > 0.7: t.add("Noisy")
        if d[2] > 0.7: t.add("Wide")
        if d[4] > 0.7: t.add("Bass")
        if d[5] > 0.7: t.add("Dense")
        if d[5] < 0.3: t.add("Sparse")
        if d[6] < 0.3: t.add("Still")
        if d[6] > 0.7: t.add("Evolving")
        if d[7] < 0.3: t.add("Smooth")
        if d[7] > 0.7: t.add("Rough")
        if d[8] < 0.3: t.add("Near")
        if d[8] > 0.7: t.add("Far")
        return sum(1 << PM.TAGS.index(x) for x in t if x in PM.TAGS)

    # ---- what it sounds like ------------------------------------------------------------------
    # A phrase is chosen by how much better this preset scores for it than the library does, not
    # by its raw score. Raw scores would hand the same two sentences to four thousand drones,
    # because CLAP is confident that a drone is a drone; the interesting question is which of them
    # is unusually a bell. The second phrase must come from another group, so the line says two
    # things.
    phraseOf = [(-1, -1)] * len(rows)
    phraseNames = []
    if scores is not None:
        phraseNames = vocab
        ok = ~np.isnan(scores[:, 0])
        mu = scores[ok].mean(axis=0)
        sd = scores[ok].std(axis=0) + 1e-9
        z = (scores - mu) / sd
        grp = np.array(groups)
        # Two guards against the two ways this goes wrong. Ranking by raw score alone hands the
        # same sentence to four thousand drones; ranking by the lift alone calls a soft pad a
        # bronze gong because it is a hair more gong-like than the average drone. So the phrase
        # must first be plausible for this preset at all (its raw score in the top quarter of the
        # vocabulary) and is then chosen from those by how far above the library it stands.
        keep = max(15, len(vocab) // 4)
        for i in range(len(rows)):
            if not ok[i]:
                continue
            plausible = set(np.argsort(-scores[i])[:keep].tolist())
            order = [j for j in np.argsort(-z[i]) if int(j) in plausible]
            if not order:
                continue
            first = int(order[0])
            second = -1
            for j in order[1:]:
                if grp[int(j)] != grp[first] and z[i, int(j)] >= 0.25:
                    second = int(j)
                    break
            phraseOf[i] = (first, second)
        used = {}
        for f, _ in phraseOf:
            if f >= 0:
                used[f] = used.get(f, 0) + 1
        top = sorted(used.items(), key=lambda kv: -kv[1])[:5]
        print(f"phrases: {sum(1 for f, _ in phraseOf if f >= 0)} presets named, "
              f"{len(used)} of {len(vocab)} phrases used as the first line")
        for j, c in top:
            print(f"   {c:5d}  {vocab[j]}")

    # ---- write ------------------------------------------------------------------------------
    # Two presets on the same spot of the map are two dots drawn on top of each other, and one of
    # them can never be clicked. make_presets nudges its own output apart, but the layout here
    # rewrites every position, and 146 of them landed on a shared cell. The same deterministic
    # golden-angle spiral, so the same input gives the same map.
    taken = set()
    for i in range(len(rows)):
        x, y = float(xy[i, 0]), float(xy[i, 1])
        step = 0
        while (round(x, 3), round(y, 3)) in taken and step < 64:
            step += 1
            ang = 2.39996 * step
            rad = 0.004 * math.sqrt(step)
            x = min(1.0, max(0.0, float(xy[i, 0]) + rad * math.cos(ang)))
            y = min(1.0, max(0.0, float(xy[i, 1]) + rad * math.sin(ang)))
        taken.add((round(x, 3), round(y, 3)))
        xy[i, 0], xy[i, 1] = x, y
    fams = PM.families()
    metaRows = []
    for i, r in enumerate(rows):
        d = desc[i]
        bits = tags_of(r, d)
        if r["kind"] == "builtin":
            metaRows.append({"name": r["name"], "x": float(xy[i, 0]), "y": float(xy[i, 1]),
                             "bright": float(d[0]), "motion": float(d[1]), "width": float(d[2]),
                             "noisy": float(d[3]), "bass": float(d[4]), "density": float(d[5]),
                             "evolve": float(d[6]), "rough": float(d[7]), "wet": float(d[8]),
                             "family": PM.family_of(r["index"], fams), "tags": bits,
                             "rms": float(r["m"].get("rms", 0.0)), "cluster": int(labels[i]),
                             "phrase": list(phraseOf[i])})
        else:
            row = r["row"]
            old = row["meta"].split()
            loud = old[9] if len(old) > 9 else f"{r['m'].get('rms', 0.0):.1f}"
            row["meta"] = (" ".join(f"{v:.3f}" for v in (xy[i, 0], xy[i, 1], d[0], d[1], d[2], d[3], d[4], d[5]))
                           + f" {bits} {loud}"
                           + " " + " ".join(f"{v:.3f}" for v in (d[6], d[7], d[8]))
                           + f" {phraseOf[i][0]} {phraseOf[i][1]} {int(labels[i])}")
    if a.dry_run:
        print("dry run: nothing written")
        return 0

    PM.write_cpp(metaRows, fams)
    centres = np.zeros((a.clusters, desc.shape[1]))
    for j in range(a.clusters):
        m = labels == j
        centres[j] = desc[m].mean(axis=0) if m.any() else 0.5
    with open(OUT_INC, "w", encoding="utf-8") as f:
        f.write("// Generated by Tools/library/map_all.py -- do not edit.\n")
        f.write("/// The measured groups of the library: k-means over the nine map descriptors.\n")
        f.write("const char* const kClusterNames[] = {\n")
        for nm in clusterNames:
            f.write(f'    "{nm}",\n')
        f.write("};\n")
        f.write("/// The centre of each group: its mean of the nine descriptors.\n")
        f.write("const float kClusterCentres[][kNumMapDescriptors] = {\n")
        for j in range(a.clusters):
            f.write("    { " + ", ".join(f"{v:.4f}f" for v in centres[j]) + " },\n")
        f.write("};\n")
        f.write(f"constexpr int kClusterCount = {a.clusters};   ///< how many groups there are\n")
    with open(OUT_PHRASES, "w", encoding="utf-8") as f:
        f.write("// Generated by Tools/library/map_all.py -- do not edit.\n")
        f.write("/// The vocabulary CLAP chose from (Tools/library/clap_embed.py). A preset's two\n")
        f.write("/// indices into this table are in its PresetMeta; nothing here was written by a model.\n")
        f.write("const char* const kPhrases[] = {\n")
        for ph in phraseNames:
            f.write('    "' + ph.replace('"', '\\"') + '",\n')
        f.write('    "",\n' if not phraseNames else "")
        f.write("};\n")
        f.write(f"constexpr int kPhraseCount = {len(phraseNames)};   ///< how many phrases there are\n")
    for name, head, rr in packs:
        head = [h for h in head if "estimated" not in h]
        head = [h for h in head if "map positions measured" not in h]
        head.insert(1, f"# Descriptors, map positions and groups measured by Tools/library/map_all.py "
                       f"from a {SECONDS:.0f} s render of every preset, laid out with the built-ins in one cloud.")
        write_pack(os.path.join(PACKS, name), head, rr)
    print(f"wrote {PM.OUT_CPP}, {OUT_INC}, {OUT_PHRASES} and {len(packs)} packs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
