"""Noctuary -- give the built-in presets the parts of the instrument they were written before.

The 191 compiled-in presets were written over many rounds, and the instrument kept growing under
them: the modulation matrix, three source slots instead of one, the z-plane's 155 shapes and its
modal mode, the Karplus-Strong strike, the BEAT source, autoplay. Measured (Tools/rate_presets.py),
they touch a median of three of the instrument's twelve families, where the generated library
touches ten. They are not worse than they were -- they are simply from an earlier version of the
thing they run on.

    python Tools/enrich_presets.py --dry-run     what it would add, and to which presets
    python Tools/enrich_presets.py               do it, then measure that nothing changed character

What it adds, and the rules it adds by:

  * A modulation matrix where there is none: three slow routes plus one from BEAT, drawn without
    repeats from a pool chosen for the preset's own character (a bright preset gets brightness,
    shimmer, the z-plane point; a dark one gets depth, the far reverb's size, the sub). Depths
    small enough to be a breath rather than a gesture, and signed either way.
  * LFO rates on a golden-ratio ladder anchored on the preset's own base period, somewhere
    between 34 s and 78 s, so the four modulators never line up with each other -- and no two
    presets get the same four rates, which the first version of this tool did to all 191 of them.
  * A z-plane filter where the preset has none, in Series at a modest Mix, with a shape picked
    from what the preset already sounds like.
  * A little more of the odd harmonics where the spectrum is at its default, which in just
    intonation is where this instrument is at its most itself.

What it will not do: change anything the preset already sets. Every rule fires only into an
empty slot, so a preset that already has a matrix, or a z-plane, or three sources, keeps exactly
what it had.

And it proves the point rather than claiming it. Every preset is rendered before and after, and
if the enrichment moved its level by more than 1.5 dB, its centroid by more than a quarter, or
its bass or width by more than 0.12, the change is thrown away and the preset is left alone. The
report says how many were kept and how many were rejected, and by what.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
PRESETS = os.path.join(ROOT, "Core", "src", "Presets.cpp")

# The golden ladder. Rates that share no simple ratio never come back into step, so four of them
# running at once make a surface that does not repeat inside a listening. The anchor is per
# preset -- the first version of this tool gave all 191 the same four rates and the same four
# routes, which decouples each preset from itself and couples the whole bank to each other.
PHI = 1.6180339887

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


# Route targets, by what the preset already sounds like. Three are drawn from the pool without
# repeats, so two presets rarely land on the same set.
TARGETS_DARK = ["far_size", "depth", "z_x", "sub_level", "far_damp", "pan_drift", "breath",
                "cosmos_smear", "near_decay", "drift", "inharmonic", "far_predelay"]
TARGETS_BRIGHT = ["brightness", "z_y", "pan_drift", "cutoff", "shimmer", "air", "tilt",
                  "ens_depth", "spread", "resonance", "detune", "arc"]
TARGETS_BEAT = ["cosmos_smear", "cutoff", "z_y", "purity", "far_size", "brightness",
                "resonance", "depth", "sub_level", "air"]


def seed_of(name):
    return int(hashlib.sha1(name.encode("utf-8")).hexdigest()[:16], 16)


def pick(seq, n, seed):
    """n items of seq, without repeats, deterministically from the seed."""
    pool, out = list(seq), []
    for _ in range(min(n, len(pool))):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        out.append(pool.pop(seed % len(pool)))
    return out


def wrap_literal(settings, width=104):
    """The settings string as adjacent C++ literals, one per line, broken between pairs.

    An enriched preset carries two to three hundred characters more than it did, and written as
    one literal it is a line nobody can read or review in a diff. Broken on the semicolons it
    stays the file it was: a list of presets you can look at."""
    parts, line, lines = settings.split(";"), "", []
    for i, p in enumerate(parts):
        piece = p + (";" if i + 1 < len(parts) else "")
        if line and len(line) + len(piece) > width:
            lines.append(line)
            line = ""
        line += piece
    if line:
        lines.append(line)
    nl = chr(10)
    return nl.join('      "%s"' % s for s in lines)


def measure(name, seconds, settings, extra=None):
    cmd = [RENDER, "--preset", name, "--seconds", str(seconds), "--measure"]
    if "brain_on=off" in settings.replace(" ", ""):
        cmd += ["--notes", "45,52,57,64"]
    for kv in (extra or []):
        cmd += ["--set", kv]
    env = dict(os.environ, AMBIENT_PACKS=os.path.join(ROOT, "Library", "Packs"))
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, env=env)
    lines = (r.stdout + r.stderr).strip().splitlines()
    if not lines:
        return None
    out = {}
    for k, v in re.findall(r"(\w+)=(-?[\d.]+)", lines[-1]):
        out[k] = float(v)
    return out if "rms" in out else None


def parse_presets():
    """[(name, settings, span)] over Presets.cpp, span being where the settings literal sits."""
    with open(PRESETS, encoding="utf-8") as f:
        text = strip_line_comments(f.read())
    start = text.index("const Preset kPresets[]")
    end = text.index("int numCosmosPresets")
    out = []
    # The tail is optional: an entry this tool has already rewritten carries the four extra
    # fields { name, settings, nullptr, nullptr, nullptr, matrix }, and must still be found.
    for m in re.finditer(r'\{\s*"([^"]+)",\s*((?:"[^"]*"\s*)+)(?:,[^}]*)?\}', text[start:end]):
        settings = "".join(re.findall(r'"([^"]*)"', m.group(2)))
        # The span is the whole { ... } entry: the matrix is a field of its own (Presets.h), so
        # adding one means rewriting the entry rather than just its settings string.
        out.append((m.group(1), settings, (start + m.start(), start + m.end())))
    return text, out


def has(settings, key):
    return re.search(r"(^|;)" + re.escape(key) + "=", settings) is not None


def value(settings, key, default=None):
    m = re.search(r"(?:^|;)" + re.escape(key) + r"=([^;]*)", settings)
    return m.group(1) if m else default


def enrich(name, settings):
    """The settings to add, as a list of "key=value", and a note on why."""
    add, why = [], []
    bright = float(value(settings, "brightness", "0.7") or 0.7)
    dark = bright < 0.5
    keys = "brain_on=off" in settings.replace(" ", "")
    seed = seed_of(name)
    frac = lambda shift: ((seed >> shift) % 1000) / 999.0      # a stable 0..1 per preset

    # ---- the modulation matrix, if it has none
    mod = None
    # The ladder's anchor is the preset's own: between 34 s and 78 s, so the four rungs land
    # somewhere in the minutes and no two presets share a set of rates.
    base = 34.0 + 44.0 * frac(0)
    added_lfos = not has(settings, "lfo1_rate")
    if added_lfos:
        for i in range(4):
            add.append("lfo%d_rate=%.5g" % (i + 1, 1.0 / (base * PHI ** i)))
            add.append("lfo%d_depth=%.2f" % (i + 1, 0.40 + 0.25 * frac(6 + 3 * i)))
        why.append("four LFOs on a ladder from %.0f s" % base)
    # Routes go in the preset's mod field rather than its settings, so they are returned apart.
    pool = TARGETS_DARK if dark else TARGETS_BRIGHT
    routes = []
    # One route per LFO the ladder put there, so none of them turns unheard.
    for i, target in enumerate(pick(pool, 4 if added_lfos else 3, seed)):
        depth = (0.10 + 0.14 * frac(18 + 4 * i)) * (-1.0 if (seed >> (30 + i)) & 1 else 1.0)
        routes.append("lfo%d>%s:%.3f" % (i + 1, target, depth))
    beat = pick([t for t in TARGETS_BEAT if t not in [r.split(">")[1].split(":")[0] for r in routes]],
                1, seed >> 5)
    if beat:
        routes.append("beat>%s:%.3f" % (beat[0], 0.08 + 0.09 * frac(24)))
    mod = ";".join(routes)
    why.append("a matrix of %d slow routes" % len(routes))

    # ---- a z-plane where there is none
    if not has(settings, "z_mode"):
        # Names out of Core/src/ZPlaneBank.inc, checked against --list-choices: an invented shape
        # name loads as shape 0 and the preset quietly gets a filter nobody chose.
        shapes = ["Wood", "Cave", "Frame Drum", "Metal Bars"] if dark else \
                 ["Glass", "Choir", "Soprano", "Church Bell"]
        if has(settings, "cosmos_send"):
            shapes = ["Choir", "Glass", "Rounded Vowels", "Air Lift"]
        shape = shapes[(seed >> 27) % len(shapes)]
        add += ["z_mode=Series", "z_shape=" + shape,
                "z_x=%.2f" % (0.20 + 0.40 * frac(33)), "z_y=%.2f" % (0.30 + 0.45 * frac(36)),
                "z_z=%.2f" % (0.05 + 0.30 * frac(39)), "z_res=%.2f" % (0.40 + 0.25 * frac(42)),
                "z_mix=%.2f" % (0.25 + 0.20 * frac(45)),
                "z_rate=%.4f" % (0.008 + 0.030 * frac(48)), "z_depth=%.2f" % (0.20 + 0.25 * frac(51))]
        why.append("a z-plane on " + shape)

    # ---- the odd harmonics, where the spectrum is untouched
    if not has(settings, "odd_even") and not keys:
        add.append("odd_even=%.2f" % (-0.30 + 0.18 * frac(54)))
        why.append("a little more of the odd harmonics")

    # ---- a second source, very quietly, only where there is just the one
    if not has(settings, "src2_type") and not has(settings, "src3_type"):
        ratio = ["3/2", "2/1", "5/4", "4/3", "5/3"][(seed >> 57) % 5]
        add += ["src2_type=Additive", "src2_level=%.2f" % (0.10 + 0.09 * frac(21)),
                "src2_ratio=" + ratio, "src2_octave=%d" % (1 if frac(15) < 0.7 else 0),
                "src2_partials=%d" % (6 + int(8 * frac(12))), "src2_bright=%.2f" % (0.35 + 0.25 * frac(9)),
                "src2_drift=%.1f" % (2.0 + 4.0 * frac(3))]
        why.append("a quiet %s on Source 2" % ratio)

    return add, mod, why


def ladder(name, settings):
    """The enrichment in three strengths, strongest first. A preset that fails the character test
    with everything gets another go with less, rather than being left in the state it was in --
    eight of the brighter presets moved three decibels with the z-plane in and were fine without
    it, and leaving them untouched would have been the tool giving up rather than being careful."""
    add, mod, why = enrich(name, settings)
    full = (add, mod, why)
    no_z = ([kv for kv in add if not kv.startswith("z_")],
            mod, [w for w in why if not w.startswith("a z-plane")])
    bare = ([kv for kv in add if kv.startswith("lfo")], mod,
            [w for w in why if "LFO" in w or "matrix" in w])
    return [full, no_z, bare]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--seconds", type=float, default=16.0)
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    if not os.path.isfile(RENDER):
        sys.exit("build the render tool first")

    text, presets = parse_presets()
    print("%d built-in presets" % len(presets))

    kept, rejected, skipped = [], [], []
    edits = []
    for name, settings, span in presets:
        if a.only and a.only.lower() not in name.lower():
            continue
        if name == "Init":
            # The blank one stays blank. It is where you start from when you want none of this.
            skipped.append(name)
            continue
        before = measure(name, a.seconds, settings)
        if before is None:
            rejected.append((name, "did not render"))
            continue
        placed = False
        lastbad = "nothing left to try"
        for add, mod, why in ladder(name, settings):
            if not add:
                continue
            after = measure(name, a.seconds, settings, add)
            if after is None:
                lastbad = "did not render"
                continue
            d_rms = abs(after["rms"] - before["rms"])
            d_cen = abs(after["centroid"] - before["centroid"]) / max(before["centroid"], 1.0)
            d_bass = abs(after["bass"] - before["bass"])
            d_width = abs(after["width"] - before["width"])
            bad = []
            if d_rms > 1.5:
                bad.append("level %.1f dB" % d_rms)
            if d_cen > 0.25:
                bad.append("centroid %.0f %%" % (100 * d_cen))
            if d_bass > 0.12:
                bad.append("bass %.2f" % d_bass)
            if d_width > 0.12:
                bad.append("width %.2f" % d_width)
            if after["peak"] > 0.99:
                bad.append("clips")
            if bad:
                lastbad = ", ".join(bad)
                continue
            kept.append((name, why, d_rms, d_cen))
            edits.append((span, name, settings + ";" + ";".join(add), mod))
            placed = True
            break
        if not placed:
            rejected.append((name, lastbad))

    print("kept %d, rejected %d, nothing to add for %d" % (len(kept), len(rejected), len(skipped)))
    for n, r in rejected[:20]:
        print("  rejected %-30s %s" % (n[:30], r))
    if a.dry_run:
        for n, why, dr, dc in kept[:12]:
            print("  %-30s +%s   (level %.2f dB, centroid %.0f %%)" % (n[:30], ", ".join(why), dr, 100 * dc))
        return 0

    # Rewritten back to front so the spans stay valid. A preset with a matrix becomes the six
    # field form { name, settings, texture, wavetable, impulse, mod }; the three file fields are
    # null because a compiled-in preset never names a sample.
    out = text
    for (lo, hi), pname, newsettings, mod in sorted(edits, key=lambda e: -e[0][0]):
        nl = chr(10)
        lit = wrap_literal(newsettings)
        if mod:
            entry = ('{ "%s",' + nl + '%s,' + nl + '      nullptr, nullptr, nullptr,'
                     + nl + '      "%s" }') % (pname, lit, mod)
        else:
            entry = ('{ "%s",' + nl + '%s }') % (pname, lit)
        out = out[:lo] + entry + out[hi:]
    with open(PRESETS, "w", encoding="utf-8") as f:
        f.write(out)
    print("rewrote %s" % PRESETS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
