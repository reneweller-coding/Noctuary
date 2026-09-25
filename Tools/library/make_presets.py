"""Generate the preset library: thousands of presets as runtime packs.

Superseded by Tools/library/make_library.py, which generates the 2.0 library (56 packs and the
built-ins) from Tools/library/artists.py and the catalogue in Tools/library/clip_catalog.py. What
lives on here is the machinery both generations use: the parameter table read from the synth, the
eight shades, the modulation matrix, the entrance contours, the optional blocks in open_up, the
descriptors, the tags, the map layout and the names. This module still generates the library it
was written for, from styles.py.

Each style in styles.py becomes one .ambientpack file (see Core/include/ambient/Presets.h for
the format). A preset is drawn from the style's parameter ranges, then a handful of optional
blocks -- z-plane filter, Cosmos, granular cloud, feedback, the two source slots, the sub, the
convolution room -- are switched on with the style's own probabilities.

    python Tools/library/make_presets.py --per-style 200

Descriptors and map positions are derived from the settings, not measured: measuring five
thousand presets means five thousand renders. The built-in presets keep their measured table
(Tools/preset_map.py); the generated ones use this estimate, which is good enough to cluster
the map and drive the browser filters. The whole run is deterministic -- same seed, same
library -- so texture and wavetable references can be written before those files exist.

Texture and wavetable references are taken from Library/Textures and Library/Wavetables (with its
shelves Harmonic, Classic and Ambient, Tools/library/wavetable_folders.py) when they are there;
presets whose sample is missing simply load nothing into that slot.
"""
import argparse
import io
import glob
import json
import math
import os
import posixpath
import random
import re
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import guide  # noqa: E402  -- the production guide's windows, applied to every preset (25.09.2026)
from styles import STYLES  # noqa: E402
from wavetable_folders import tables as shelf_tables  # noqa: E402

RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")

# Choice names, exactly as Core/src/Params.cpp, Sources.cpp and ZPlane.cpp spell them.
STACKS = ["Octaves", "Fifths", "Major", "Minor", "Seventh", "Harmonics", "Subharmonics"]
# Noise once, not twice: a noise slot measured as the flattest thing a preset can carry (median
# flatness x1.33 against presets without one, p90 x2.2), and harsh was the word for the library.
SOURCE_TYPES = ["Wavetable", "FM", "Texture", "Noise", "Additive"]
# Weighted: the models that keep the body (low-pass, ladder) twice, the ones that take it away or
# make it metallic (high-pass, notch, comb: flatness x1.3-1.4 in the census) once.
FILTER_MODELS = ["LP 6", "LP 6", "LP 24", "LP 24", "Ladder", "Ladder", "BP 12", "BP 12", "Peak", "Peak",
                 "Formant", "Formant", "HP 12", "Notch", "Comb"]
STRIKE_TYPES = ["String", "String", "Wood", "Metal"]
TABLES = ["Classic", "Organ", "Vocal", "Glass", "Metal", "User"]
SLOT_RATIOS = ["1/1", "9/8", "6/5", "5/4", "4/3", "3/2", "8/5", "5/3", "7/4", "2/1"]
def _z_shapes():
    """Every shape the filter bank has, read from the bank itself. This used to be a hand-written
    list of the original sixteen, which meant the whole five-thousand-preset library reached for
    sixteen filters while the instrument had a hundred and fifty-five. Reading the generated
    header means the list can never fall behind again."""
    inc = os.path.join(ROOT, "Core", "src", "ZPlaneBank.inc")
    text = open(inc, encoding="utf-8").read()
    body = text[text.index("kZShapeNames[kZShapes] = {"):]
    body = body[:body.index("};")]
    return re.findall(r'"([^"]+)"', body)


Z_SHAPES = _z_shapes()
# The families whose shapes are ringing objects rather than filter curves: these are the ones
# worth putting into Modal mode (see Core/include/ambient/ZPlane.h).
Z_MODAL_SHAPES = [s for s in Z_SHAPES if s in (
    "Marimba", "Vibraphone", "Glockenspiel", "Tubular Bell", "Church Bell", "Gong", "Tam Tam",
    "Steel Plate", "Handpan", "Kalimba", "Timpani", "Frame Drum", "Tabla", "Violin Body",
    "Cello Body", "Guitar Box", "Harp Body", "Piano Board", "Sympathetic", "Open Pipe",
    "Closed Pipe", "Clarinet", "Flute", "Bottle", "Organ Pipe", "Small Room", "Cave",
    "Concrete Pipe", "Plate Reverb", "Tunnel", "Metal Bars", "Glass", "Strings", "Wood")]
SHIMMER_PITCH = ["+12", "+7", "+5", "+19", "-12", "+24"]
TAGS = ["Dark", "Bright", "Calm", "Moving", "Tonal", "Noisy", "Wide", "Bass", "Dense", "Sparse",
        "Keys", "Generative", "Cosmos", "Feedback", "Sources", "JustIntonation", "Sub", "Stack", "Air"]


# ---------------------------------------------------------------- the parameter table

def param_table():
    """key -> (section, min, max, default), read from the synth itself so the generator can
    never drift away from Core/src/Params.cpp."""
    out = subprocess.run([RENDER, "--list"], capture_output=True, text=True, encoding="utf-8").stdout
    table = {}
    for line in out.splitlines():
        m = re.match(r"^(\S+)\s+(\S+(?: \S+)*?)\s+\[(-?[\d.e+-]+) \.\. (-?[\d.e+-]+)\] default (-?[\d.e+-]+)", line)
        if m:
            table[m.group(1)] = (m.group(2).strip(), float(m.group(3)), float(m.group(4)), float(m.group(5)))
    if not table:
        raise SystemExit(f"could not read the parameter table -- build ambient_render first ({RENDER})")
    return table


PARAMS = {}


def fmt(key, value):
    """Clamp to the parameter's range and print it the way a preset string wants it."""
    section, lo, hi, _ = PARAMS[key]
    v = min(max(float(value), lo), hi)
    if abs(v - round(v)) < 1e-6:
        return str(int(round(v)))
    return f"{v:.4g}"


def draw(rng, spec):
    """One value from a range spec (see styles.py)."""
    if isinstance(spec, list):
        return spec[rng.randrange(len(spec))]
    if isinstance(spec, tuple):
        if spec[0] == "log":
            return math.exp(rng.uniform(math.log(spec[1]), math.log(spec[2])))
        if spec[0] == "int":
            return rng.randint(int(spec[1]), int(spec[2]))
        return rng.uniform(spec[0], spec[1])
    return spec


def u(rng, lo, hi):
    return rng.uniform(lo, hi)


def logu(rng, lo, hi):
    return math.exp(rng.uniform(math.log(lo), math.log(hi)))


# ---------------------------------------------------------------- shades
#
# Two hundred presets drawn from one set of ranges would be two hundred variations of the same
# preset. Each one is therefore pushed into one of eight shades: a nudge on a few parameters
# and on the chance that an optional block is on. The style stays recognisable, the presets
# inside it do not collapse into each other.

SHADES = [
    ("deep",   {"brightness": ("add", -0.18), "cutoff": ("mul", 0.45), "tilt": ("add", 0.45),
                "far_highcut": ("mul", 0.55), "sub_level": ("add", 0.12), "brain_low": ("add", -5)},
               {"sub": 0.2}),
    ("lit",    {"brightness": ("add", 0.18), "cutoff": ("mul", 2.0), "tilt": ("add", -0.3),
                "far_highcut": ("mul", 1.7), "air": ("add", 0.1), "shimmer": ("add", 0.12)},
               {"cosmos": 0.1}),
    ("still",  {"shimmer_rate": ("mul", 0.4), "drift_rate": ("mul", 0.45), "brain_rate": ("mul", 2.2),
                "ens_depth": ("add", -0.2), "filter_drift": ("add", -0.2), "attack": ("mul", 1.6),
                "release": ("mul", 1.5), "src2_spread": ("mul", 0.35), "src3_spread": ("mul", 0.35)},
               {"zplane": -0.15, "cloud": -0.1, "coherence": -0.1}),
    ("astir",  {"shimmer_rate": ("mul", 2.2), "brain_rate": ("mul", 0.5), "ens_depth": ("add", 0.2),
                "filter_drift": ("add", 0.2), "pan_drift": ("add", 0.15), "rate_wander": ("add", 0.2),
                "src2_spread": ("mul", 2.2), "src3_spread": ("mul", 2.2)},
               {"zplane": 0.2, "coherence": 0.2, "delay2": 0.1}),
    ("sparse", {"brain_density": ("add", -2), "partials": ("add", -6), "strands": ("add", -1),
                "ens_mix": ("add", -0.15), "brain_rate": ("mul", 1.6),
                "src2_grains": ("mul", 0.7), "src3_grains": ("mul", 0.7)},
               {"src2": -0.25, "src3": -0.2, "cloud": -0.12, "stack": -0.15}),
    ("massed", {"brain_density": ("add", 2), "partials": ("add", 6), "strands": ("add", 1),
                "ens_mix": ("add", 0.15), "detune": ("mul", 1.4),
                "src2_grains": ("mul", 1.6), "src3_grains": ("mul", 1.6)},
               {"src2": 0.25, "src3": 0.15, "stack": 0.25}),
    ("rough",  {"inharmonic": ("add", 0.2), "fb_drive": ("add", 0.2), "resonance": ("add", 0.12),
                "air": ("add", 0.08), "src2_spread": ("mul", 1.8), "src3_spread": ("mul", 1.8)},
               {"feedback": 0.3, "cloud": 0.2, "texture": 0.2}),
    ("clean",  {"inharmonic": ("add", -0.15), "purity": ("add", 0.06), "detune": ("mul", 0.6),
                "far_damp": ("add", -0.1)},
               {"feedback": -0.35, "cloud": -0.15, "texture": -0.1}),
]


def apply_shade_granular(p, shade):
    """The granular parameters are set when a slot is filled, which happens after the shade pass,
    so the shade's nudges to them are applied here instead."""
    _, nudges, _ = shade
    for key in ("src2_spread", "src3_spread", "src2_grains", "src3_grains"):
        if key not in p or key not in nudges:
            continue
        how, amount = nudges[key]
        v = float(p[key]) * amount if how == "mul" else float(p[key]) + amount
        _, lo, hi, _d = PARAMS[key]
        v = min(max(v, lo), hi)
        p[key] = int(round(v)) if key.endswith("grains") else v


def apply_shade(p, mod, shade):
    _, nudges, mods = shade
    for key, (how, amount) in nudges.items():
        if key not in p or isinstance(p[key], str):
            continue
        v = float(p[key])
        p[key] = v * amount if how == "mul" else v + amount
        _, lo, hi, _d = PARAMS[key]
        p[key] = min(max(p[key], lo), hi)
        if key in ("partials", "strands", "brain_density", "brain_low", "brain_high",
                   "src2_grains", "src3_grains"):
            p[key] = int(round(p[key]))
    out = dict(mod)
    for key, delta in mods.items():
        out[key] = min(1.0, max(0.0, out.get(key, 0.0) + delta))
    return out



# ---------------------------------------------------------------- modulation
#
# Every preset gets a small matrix. The depths are fractions of the target's own range, so they
# have to be read per target: 0.3 on a mix is a third of it, 0.3 on the cutoff is 5.4 kHz. The
# rates are drone rates -- a cycle between eight seconds and forty minutes, not the tenths of a
# second an LFO usually lives at.

# target -> (depth lo, depth hi, needs which module or None)
MOD_TARGETS = [
    ("cutoff",        0.04, 0.14, None),
    ("brightness",    0.08, 0.28, None),
    ("shimmer",       0.10, 0.35, None),
    ("air",           0.08, 0.25, None),
    ("detune",        0.05, 0.25, None),
    ("purity",        0.04, 0.16, None),
    ("depth",         0.06, 0.22, None),
    ("pan_drift",     0.08, 0.30, None),
    ("width",         0.05, 0.18, None),
    ("ens_depth",     0.10, 0.35, None),
    ("near_mix",      0.06, 0.20, None),
    ("far_decay",     0.05, 0.20, None),
    ("far_highcut",   0.05, 0.18, None),
    ("far_size",      0.06, 0.22, None),
    ("dly_feedback",  0.04, 0.14, None),
    ("dly_mix",       0.05, 0.18, None),
    ("resonance",     0.06, 0.22, None),
    ("z_x",           0.15, 0.45, "zplane"),
    ("z_y",           0.15, 0.45, "zplane"),
    ("z_res",         0.08, 0.25, "zplane"),
    ("cosmos_shift",  0.04, 0.16, "cosmos"),
    ("cosmos_smear",  0.10, 0.30, "cosmos"),
    ("cosmos_nebula", 0.10, 0.30, "cosmos"),
    ("cloud_density", 0.10, 0.30, "cloud"),
    ("cloud_size",    0.10, 0.30, "cloud"),
    ("cloud_pitch",   0.08, 0.25, "cloud"),
    ("fb_tone",       0.06, 0.20, "feedback"),
    ("room_level",    0.05, 0.18, "room"),
    # Everything the library never modulated. The instrument has had these for rounds; not one
    # preset moved them, because the target list was written before they existed.
    ("far_rotate",    0.08, 0.25, None),
    ("far_unmask_spread", 0.08, 0.28, None),   # the key is far_unmask_spread; "far_spread" was a route to nowhere
    ("far_envelop",   0.08, 0.30, None),
    ("far_comod",     0.08, 0.30, None),
    ("early_size",    0.08, 0.30, None),
    ("elev_far",      0.08, 0.30, None),
    ("near_ild",      0.08, 0.30, None),
    ("presence",      0.06, 0.22, None),
    ("itd",           0.06, 0.22, None),   # the Time Width control; "time_width" was its label, not its key
    ("doppler",       0.06, 0.20, None),
    ("partial_spread", 0.10, 0.35, None),
    ("sub_pulse",     0.08, 0.25, None),
    ("brain_consonance", 0.06, 0.22, None),
    ("brain_spread",  0.08, 0.30, None),
    ("brain_bias",    0.08, 0.30, None),
    ("brain_dejavu",  0.08, 0.30, None),
    ("brain_cascade", 0.08, 0.28, None),
    ("brain_wander",  0.06, 0.22, None),
    ("tide",          0.06, 0.22, None),
    ("purity_drift",  0.06, 0.22, None),
    ("purity_adapt",  0.08, 0.28, None),
    ("body_tone",     0.08, 0.28, None),
    ("patina",        0.08, 0.28, None),
    ("blur_smear",    0.08, 0.30, None),
    ("cosmos_swell",  0.10, 0.35, "cosmos"),
    ("cosmos_vowel",  0.10, 0.35, "cosmos"),
    ("cosmos_shimmer", 0.08, 0.28, "cosmos"),
    ("fb_bias",       0.08, 0.28, "feedback"),
    ("fb_drive",      0.06, 0.22, "feedback"),
]
# Targets that only make sense for a slot that is actually running, keyed by the slot's type.
MOD_SLOT_TARGETS = {
    "Wavetable": [("{p}pos", 0.10, 0.40), ("{p}level", 0.08, 0.25)],
    "Harmonic":  [("{p}pos", 0.10, 0.40), ("{p}level", 0.08, 0.25), ("{p}transport", 0.10, 0.40)],
    "FM":        [("{p}fm_index", 0.08, 0.30), ("{p}level", 0.08, 0.25)],
    "Texture":   [("{p}pos", 0.10, 0.40), ("{p}density", 0.08, 0.25), ("{p}spread", 0.10, 0.35),
                  ("{p}level", 0.08, 0.25)],
    "Noise":     [("{p}pos", 0.12, 0.45), ("{p}level", 0.08, 0.25), ("{p}noise_q", 0.10, 0.35)],
    "Stretch":   [("{p}pos", 0.10, 0.40), ("{p}stretch", 0.08, 0.30), ("{p}level", 0.08, 0.25)],
    "Bow":       [("{p}bow_force", 0.10, 0.35), ("{p}bow_speed", 0.10, 0.35), ("{p}level", 0.08, 0.25)],
    "Spectral":  [("{p}spec_rate", 0.10, 0.35), ("{p}spec_breath", 0.12, 0.40), ("{p}pos", 0.10, 0.35)],
    "Additive":  [("{p}bright", 0.10, 0.30), ("{p}shimmer", 0.10, 0.30), ("{p}level", 0.08, 0.25)],
}
# The first slot's additive controls answer to their old names; the other three do not have them.
MOD_SLOT1_ADDITIVE = [("brightness", 0.08, 0.28), ("shimmer", 0.10, 0.35), ("partials", 0.10, 0.30),
                      ("odd_even", 0.10, 0.35), ("inharmonic", 0.08, 0.30), ("detune", 0.05, 0.25)]
# Sources that are not a clock: the instrument listening to itself, a field, an attractor. Drawn
# for a share of the routes, which is what turns a modulation matrix into weather.
OTHER_SOURCES = ["beat", "kura1", "kura2", "kura3", "kura4",
                 "lenia1", "lenia2", "lenia3", "lenia4",
                 "lorenz_x", "lorenz_y", "lorenz_z", "rossler_x", "rossler_y", "rossler_z",
                 "cascade", "cascade", "amp", "note", "velocity", "distance", "random"]
LFO_SHAPES = ["Sine", "Sine", "Sine", "Triangle", "Random", "Random", "Steps", "Table", "Ramp Up"]

# A route that SWITCHES instead of moving. The instrument is continuous by construction -- every
# movement a rate or an amplitude, never a step -- so these are rare and only ever aimed at
# parameters that take effect at the next note. A scale that changes while nothing new is played
# changes nothing; a filter model that changes mid-tail is a click, and is not in this list.
#
# "scale" and "brain_quantize" were in this list and are not any more. Both are decided per artist
# by the ranges document, and an LFO walking through the scales is exactly what R4.1 forbids: a
# supply stands for at least eight minutes and changes by exchanging a single degree, which is what
# brain_degree_swap does and what every preset now carries. The grid is the artist's too.
SWITCH_TARGETS = ["stack", "root", "strike_type", "keys_filter"]


def bank_presets(path, first_is_off=True):
    """The settings strings of one of the core's own preset banks (Cosmos, Strike, Z-plane),
    read straight out of the .inc file it is generated into. A pack preset that draws one gets
    exactly the layer the plugin's own bank menu would load."""
    out = []
    try:
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if not line.startswith("{ \""):
                continue
            parts = [x for x in line.split("\"") if x]
            if len(parts) >= 4 and "=" in parts[3]:
                out.append((parts[1], parts[3]))
    except OSError:
        return []
    return out[1:] if (first_is_off and out) else out


COSMOS_BANK = bank_presets(os.path.join(ROOT, "Core", "src", "CosmosPresets.inc"))
STRIKE_BANK = bank_presets(os.path.join(ROOT, "Core", "src", "StrikePresets.inc"))
Z_BANK = bank_presets(os.path.join(ROOT, "Core", "src", "ZPlanePresets.inc"))


def entrance_shape(rng):
    """A source's own entrance as a level from 0 to 1, in the envelope text form, and its mode.

    Four kinds, because an entrance is a gesture and one gesture everywhere is a mannerism: a rise
    that settles; a swell past where it settles; a breath that holds while the note is down and falls
    away when it is let go (Sustain Loop); and a slow wander between levels that never comes to rest
    (Loop). Times are seconds at Time 1."""
    kind = rng.choices(["rise", "swell", "breath", "wander"], weights=[35, 30, 20, 15])[0]
    curve = lambda: round(u(rng, -0.6, 0.2), 2)
    if kind == "rise":
        return f"0:0/{u(rng, 1.5, 8.0):.3g}:1:{curve():g}", "One Shot"
    if kind == "swell":
        t1 = u(rng, 1.0, 5.0)
        t2 = t1 + u(rng, 2.0, 8.0)
        return f"0:0/{t1:.3g}:1:{curve():g}/{t2:.3g}:{u(rng, 0.45, 0.85):.3g}:{curve():g}", "One Shot"
    if kind == "breath":
        t1 = u(rng, 1.0, 5.0)
        t2 = t1 + u(rng, 1.0, 4.0)
        t3 = t2 + u(rng, 3.0, 12.0)
        return (f"0:0/{t1:.3g}:1:{curve():g}/{t2:.3g}:{u(rng, 0.55, 0.9):.3g}:{curve():g}"
                f"/{t3:.3g}:0:{curve():g}!s2"), "Sustain Loop"
    pts, t = ["0:0"], 0.0
    n = rng.randint(4, 7)
    for _ in range(1, n):
        t += u(rng, 1.5, 6.0)
        pts.append(f"{t:.3g}:{u(rng, 0.35, 1.0):.3g}:{round(u(rng, -0.4, 0.4), 2):g}")
    return "/".join(pts) + f"!l1-{n - 1}", "Loop"


def stagger_entries(p, envs_text, rng):
    """Let the sources arrive one after another instead of all on the note.

    The Envelope section is one envelope for the whole voice, so before `src{n}_delay` existed a
    preset of a wavetable and a texture was one chord struck twice at once, however different the
    two materials were -- and that, more than anything, is why two presets built from the same
    parts sounded like the same preset. A slot that waits twenty seconds is a second instrument
    entering under a note that is already sounding.

    Not every preset: a delay everywhere would be its own mannerism. Roughly two in five of the
    later slots wait, drawn over a wide range so some are a breath and some are half a minute, and
    the first sounding slot never waits -- a note has to start somewhere. A quarter of the waiting
    slots enter on a contour of their own instead of a plain fade: the source's own sixteen-point
    envelope (Env = Own), which leaves all six of the preset's envelopes to the modulation. Its shape
    goes into the envelope field after the six, and the field is returned.
    """
    sounding = [n for n in (1, 2, 3, 4)
                if p.get(f"src{n}_type", "Additive" if n == 1 else "Off") not in ("Off", "")]
    if len(sounding) < 2:
        return envs_text
    # modulation_for hands the shapes back as ONE string, "~"-separated with the trailing empties
    # stripped -- not as a list. Enumerating it once walked over characters and wrote src2_env =
    # "Env 11", which the choice list has no name for. Split, then pad to the six and the four.
    shapes = ((envs_text.split("~") if envs_text else []) + [""] * 10)[:10]
    for n in sounding[1:]:                        # never the first: the note has to start somewhere
        if rng.random() >= 0.42:
            continue
        p[f"src{n}_delay"] = round(logu(rng, 2.5, 25.0), 2)
        if rng.random() < 0.25:
            text, mode = entrance_shape(rng)
            shapes[5 + n] = text                  # the field's seventh to tenth are Source 1 to 4
            p[f"src{n}_env"] = "Own"
            p[f"src{n}_env_mode"] = mode
            p[f"src{n}_env_time"] = round(logu(rng, 0.6, 3.0), 3)
            if rng.random() < 0.2:                # now and then the contour only colours the level
                p[f"src{n}_env_depth"] = round(u(rng, 0.5, 0.9), 3)
        else:
            p[f"src{n}_rise"] = round(logu(rng, 1.5, 12.0), 2)
    return "~".join(shapes).rstrip("~")


def modulation_for(p, style, rng, shade_name):
    """Builds the matrix rows, the envelope shapes and the LFO parameters for one preset.
    Returns (matrix text, env text). Writes the LFO and envelope parameters into `p`."""
    # Which targets are available depends on what this preset actually switched on.
    on = lambda key: {
        "zplane":  p.get("z_mode", "Off") in ("Series", "Replace"),
        "cosmos":  float(p.get("cosmos_send", 0) or 0) > 0.05,
        "cloud":   float(p.get("cloud_send", 0) or 0) > 0.05,
        "feedback": float(p.get("fb_bus", 0) or 0) > 0.02,
        "room":    float(p.get("room_level", 0) or 0) > 0.02,
    }.get(key, True)
    pool = [(t, lo, hi) for t, lo, hi, need in MOD_TARGETS if need is None or on(need)]
    for n in (1, 2, 3, 4):
        kind = p.get(f"src{n}_type", "Additive" if n == 1 else "Off")
        for tpl, lo, hi in MOD_SLOT_TARGETS.get(kind, []):
            t = tpl.format(p=f"src{n}_")
            if n == 1 and t == "src1_level":
                t = "osc_level"
            if n == 1 and t in ("src1_bright", "src1_shimmer"):
                continue                       # the first slot's own names are added below
            pool.append((t, lo, hi))
    if p.get("src1_type", "Additive") == "Additive":
        pool.extend(MOD_SLOT1_ADDITIVE)

    # A still preset gets fewer and slower routes, an astir one more and faster.
    count = {"still": (1, 3), "sparse": (1, 3), "clean": (2, 4), "deep": (2, 4),
             "lit": (2, 5), "massed": (3, 5), "rough": (3, 6), "astir": (4, 7)}.get(shade_name, (2, 5))
    n_routes = rng.randint(*count)
    rate_mul = {"still": 0.4, "sparse": 0.6, "astir": 2.6, "rough": 1.8}.get(shade_name, 1.0)

    rng.shuffle(pool)
    rows, used_lfo = [], []
    # The periods sit on a golden ladder: the first is drawn, the rest are it times phi to the
    # power of how many have gone before. Rates in a simple ratio come back into step and the
    # drone falls into a pattern a listener can hear coming; powers of phi never do.
    base_period = math.exp(u(rng, math.log(8.0), math.log(900.0))) / rate_mul
    PHI = 1.6180339887
    for i in range(min(n_routes, len(pool), 8)):
        target, lo, hi = pool[i]
        lfo = i + 1 if i < 8 else (i % 8 + 1)
        # Now and then the source is not an LFO at all: BEAT is the chord listening to how far
        # out of tune it is, and the coherence ring is four oscillators that pull on each other.
        # Both make the modulation come from the instrument rather than from a clock.
        # A wavetable's position is the one control where a plain eight-second sine is audibly
        # wrong: scanning a table is a slow walk through a spectrum, and a rigid one turns the
        # drone into an LFO with a spectrum attached. Positions get a chaotic source more often,
        # and when they do get an LFO it is a slow and smooth one (below).
        is_pos = target.endswith("_pos") or target == "pos"
        other = None
        if rng.random() < (0.55 if is_pos else 0.34):
            src = OTHER_SOURCES
            if is_pos and rng.random() < 0.6:
                src = [x for x in OTHER_SOURCES if x.startswith(("lorenz", "rossler", "lenia"))] or OTHER_SOURCES
            other = src[rng.randrange(len(src))]
        if other is not None:
            depth = u(rng, lo, hi) * (1.0 if rng.random() < 0.65 else -1.0)
            if other.startswith("lenia"):
                p.setdefault("lenia_rate", round(math.exp(u(rng, math.log(0.5), math.log(8.0))), 3))
                p.setdefault("lenia_growth", round(u(rng, 0.1, 0.28), 3))
            if other.startswith(("lorenz", "rossler")):
                p.setdefault("chaos_period", round(math.exp(u(rng, math.log(20.0), math.log(400.0))), 1))
            if other == "cascade":
                p.setdefault("brain_cascade", round(u(rng, 0.3, 0.8), 3))
            # A source that rests at zero (the hands, the fields' corners) reaches the sound only
            # through the 0..1 flag; the two-sided ones are left bipolar.
            flag = ":none:u" if other in ("amp", "velocity", "random") and rng.random() < 0.5 else ""
            rows.append(f"{other}>{target}:{depth:.3f}{flag}")
            continue
        if lfo not in used_lfo:
            used_lfo.append(lfo)
            period = base_period * (PHI ** len(used_lfo))
            if is_pos:
                # Thirty seconds to three minutes, and never a shape that jumps.
                period = max(period, math.exp(u(rng, math.log(30.0), math.log(180.0))))
            p[f"lfo{lfo}_rate"] = 1.0 / period
            p[f"lfo{lfo}_shape"] = (["Sine", "Triangle", "Random", "Random", "Table"][rng.randrange(5)]
                                    if is_pos else LFO_SHAPES[rng.randrange(len(LFO_SHAPES))])
            p[f"lfo{lfo}_phase"] = round(u(rng, 0.0, 1.0), 3)
            p[f"lfo{lfo}_depth"] = round(u(rng, 0.6, 1.0), 3)
            if p[f"lfo{lfo}_shape"] == "Table":
                p[f"lfo{lfo}_table"] = rng.randrange(0, 32)
            # Per Voice gives every note its own phase -- a cluster then breathes in parts rather
            # than as one block; Retrigger starts it at the note. Neither was ever used.
            if rng.random() < 0.30:
                # Retrigger or nothing. "Per Voice" stood in 6855 presets and was read by nobody --
                # this instrument computes its matrix once a block, not once per voice, so there is
                # no per-voice copy for an LFO to have. The name stays in the choice list so an old
                # preset still loads; the library stops asking for something it cannot get.
                p[f"lfo{lfo}_mode"] = "Retrigger"
            # And now and then a rate from the clock instead of from seconds.
            if rng.random() < 0.12:
                p[f"lfo{lfo}_sync"] = rng.choice(["64 bars", "32 bars", "16 bars", "8 bars", "4 bars"])
        depth = u(rng, lo, hi) * (1.0 if rng.random() < 0.65 else -1.0)
        row = f"lfo{lfo}>{target}:{depth:.3f}"
        # Now and then a macro decides how much of the route gets through. Sparingly: a macro is
        # performance state and starts at zero, so such a route is silent until a hand opens it.
        if rng.random() < 0.07:
            row += ":macro_" + "abcdefgh"[rng.randrange(8)]
        elif rng.random() < 0.15:
            row += ":none:u"
        rows.append(row)

    # Envelopes: slow shape generators, not attacks.
    #
    # This used to be "one or two, three to six points, never a sustain point", and measured over
    # the finished library that is exactly what came out: envelopes 1 and 2 only, at most six of
    # the sixteen breakpoints, and Sustain Loop -- one of the three modes -- used by none of six
    # thousand presets. The ceiling was the generator's, not the instrument's. The ranges below
    # are the instrument's own: up to six envelopes, up to sixteen points, and a sustain point on
    # the ones that are worth holding.
    #
    # The times sit on a golden ladder from the first envelope's, so a preset with five of them
    # running has five periods that never come back into step.
    envs = ["", "", "", "", "", ""]
    n_env = rng.choices([0, 1, 2, 3, 4, 5, 6], weights=[18, 26, 24, 14, 9, 6, 3])[0]
    base_time = math.exp(u(rng, math.log(2.0), math.log(20.0)))
    for e in range(n_env):
        pts, t = [], 0.0
        # Mostly short shapes, because a shape you can follow is worth more than a long one you
        # cannot; but a fifth of them go long, and those are the ones that use the whole editor.
        n_pts = rng.randint(3, 7) if rng.random() < 0.8 else rng.randint(8, 16)
        for k in range(n_pts):
            v = 0.0 if k in (0, n_pts - 1) else round(u(rng, -1.0, 1.0), 3)
            pts.append(f"{t:.3g}:{v:g}:{round(u(rng, -0.6, 0.6), 2):g}")
            t += u(rng, 0.6, 3.0)
        text = "/".join(pts)
        mode = "One Shot"
        if rng.random() < 0.55 and n_pts >= 4:
            text += f"!l0-{n_pts - 2}"          # loop everything but the tail
            mode = "Loop"
        elif rng.random() < 0.35 and n_pts >= 5:
            # Sustain Loop: rise, wait on the sustain point for as long as anything is sounding,
            # then play the tail from there. The point is picked in the middle of the shape so
            # there is something to rise through and something left to run out.
            text += f"!s{rng.randint(1, n_pts - 3)}"
            mode = "Sustain Loop"
        envs[e] = text
        # Golden ladder, so a preset's envelopes never line up with each other.
        p[f"env{e+1}_time"] = round(base_time * PHI ** e, 3)
        p[f"env{e+1}_mode"] = mode
        p[f"env{e+1}_depth"] = round(u(rng, 0.5, 1.0), 3)
        if rng.random() < 0.12:
            p[f"env{e+1}_sync"] = rng.choice(["32 bars", "16 bars", "8 bars", "4 bars"])
        if pool:
            target, lo, hi = pool[rng.randrange(len(pool))]
            rows.append(f"env{e+1}>{target}:{u(rng, lo, hi) * (1.0 if rng.random() < 0.7 else -1.0):.3f}")

    # And, rarely, one route that switches rather than moves -- a scale that changes every few
    # minutes, a stack that turns over. Slow and stepped on purpose, and only at targets that are
    # read when the next note starts, so nothing in the sound can click.
    if rows and rng.random() < 0.06:
        lfo = 8 if 8 not in used_lfo else (used_lfo[0] if used_lfo else 1)
        p[f"lfo{lfo}_shape"] = "Steps"
        p[f"lfo{lfo}_rate"] = round(1.0 / math.exp(u(rng, math.log(90.0), math.log(900.0))), 6)
        p[f"lfo{lfo}_depth"] = round(u(rng, 0.5, 1.0), 3)
        target = SWITCH_TARGETS[rng.randrange(len(SWITCH_TARGETS))]
        rows.append(f"lfo{lfo}>{target}:{u(rng, 0.15, 0.5):.3f}")

    return ";".join(rows), "~".join(envs).rstrip("~")

# ---------------------------------------------------------------- one preset

# Aftertouch, the wheel and the slide, as ordinary routes. All three rest at zero and are
# written with the 0..1 flag, so a preset carrying them sounds exactly as it did until a hand
# moves -- which is what makes it safe to put them in six thousand finished patches.
HAND_TARGETS = {
    "pressure": [("cutoff", 0.20, 0.45), ("brightness", 0.15, 0.35), ("z_x", 0.15, 0.4),
                 ("resonance", 0.1, 0.3), ("far_level", 0.1, 0.25), ("shimmer", 0.15, 0.4)],
    "wheel":    [("cloud_send", 0.2, 0.5), ("cosmos_send", 0.2, 0.5), ("far_level", 0.15, 0.4),
                 ("dly_mix", 0.15, 0.4), ("z_y", 0.2, 0.5), ("air", 0.15, 0.4),
                 ("blur_mix", 0.2, 0.5), ("filter_fold", 0.2, 0.6)],
    "slide":    [("z_y", 0.15, 0.4), ("inharmonic", 0.15, 0.4), ("odd_even", 0.15, 0.4),
                 ("tilt", 0.15, 0.35), ("cosmos_vowel", 0.2, 0.5)],
}


def add_hands(p, style, rng, matrix):
    if rng.random() >= style["modules"].get("hands", 0.0):
        return matrix
    rows = [r for r in matrix.split(";") if r] if matrix else []
    used = {r.split(">")[1].split(":")[0] for r in rows}
    for source in rng.sample(["pressure", "wheel", "slide"], k=rng.choice([1, 1, 2])):
        # Only targets the preset actually has: a route at the cloud of a patch with no cloud is
        # a row that does nothing, and the library is full enough of real routes already.
        choices = [(t, lo, hi) for t, lo, hi in HAND_TARGETS[source]
                   if (t in p or t in ("cutoff", "brightness", "air", "tilt", "shimmer", "resonance", "far_level"))
                   and t not in used]
        if not choices or len(rows) >= 16:
            continue
        target, lo, hi = choices[rng.randrange(len(choices))]
        rows.append("%s>%s:%.3f:u" % (source, target, u(rng, lo, hi)))
        used.add(target)
    return ";".join(rows)


# The instrument's own reach, as the library never used it. Everything here is drawn from the
# `extra` stream and gated on a weight, so a style that asks for none of it is untouched, and a
# preset that asks for all of it is still a preset and not a demonstration: each block sets the
# few parameters that make its feature audible and leaves the rest alone.
def open_up(p, mod, extra, rng):
    def want(key):
        return extra.random() < mod.get(key, 0.0)

    # ---- the conductor ----------------------------------------------------------------
    if p.get("brain_on", "on") != "off":
        if want("cascade"):
            # A Hawkes clock: events breed events, so the piece arrives in handfuls and then
            # leaves long holes. The branching stays under one or it would never stop.
            p["brain_cascade"] = round(u(extra, 0.25, 0.85), 3)
        if want("surprise"):
            p["brain_surprise"] = round(u(extra, 0.2, 0.8), 3)
            if extra.random() < 0.7:
                p["brain_homeostat"] = round(u(extra, 0.3, 0.9), 3)
        if want("dejavu"):
            p["brain_dejavu"] = round(u(extra, 0.35, 0.95), 3)
            if extra.random() < 0.6:
                p["brain_loop"] = round(u(extra, 0.2, 0.8), 3)
        if want("spreadbias"):
            p["brain_spread"] = round(u(extra, 0.0, 1.0), 3)
            p["brain_bias"] = round(u(extra, -0.7, 0.7), 3)
        if want("blend"):
            p["brain_blend"] = round(u(extra, 0.25, 0.9), 3)
        if want("keyfind"):
            p["brain_key"] = round(u(extra, 0.3, 0.9), 3)
        if want("evensmooth"):
            p["brain_even"] = round(u(extra, 0.2, 0.8), 3)
            p["brain_smooth"] = round(u(extra, 0.2, 0.8), 3)
        if want("timbre"):
            p["brain_timbre"] = round(u(extra, 0.3, 1.0), 3)
            if extra.random() < 0.5:
                p["brain_harmonic"] = round(u(extra, 0.2, 0.8), 3)
        if want("quantize"):
            p["brain_quantize"] = extra.choice(["8 bars", "4 bars", "2 bars", "1 bar", "1/2"])
        # Spacing keeps the voices out of each other's critical bands; NEGATIVE seeks that crowding
        # instead, which is what a cluster is -- the seconds that sit next to each other in a
        # Rich-style voicing and beat against each other on purpose. Only a style that asks for
        # clusters draws from the negative half.
        if extra.random() < 0.25:
            p["brain_spacing"] = round(u(extra, 0.2, 0.9), 3)
        elif extra.random() < mod.get("cluster", 0.0):
            p["brain_spacing"] = round(u(extra, -0.8, -0.25), 3)
    # A second conductor for the background plane: its own slower clock, its own register.
    if want("brain2"):
        p["brain2_on"] = "on"
        p["brain2_density"] = extra.randint(2, 5)
        p["brain2_rate"] = round(logu(extra, 40.0, 240.0), 2)
        p["brain2_hold_min"] = round(logu(extra, 40.0, 180.0), 1)
        p["brain2_hold_max"] = round(logu(extra, 180.0, 600.0), 1)
        p["brain2_low"] = extra.randint(24, 40)
        p["brain2_high"] = p["brain2_low"] + extra.randint(10, 30)
        p["brain2_depth"] = round(u(extra, 0.55, 1.0), 3)
        p["brain2_consonance"] = round(u(extra, 0.2, 0.8), 3)
        if extra.random() < 0.4:
            # SEMITONES, not an amount. This was drawn as 0.1 to 0.7 for a long time, as though it
            # were a 0..1 knob, which put the whole background layer ten to seventy cents beside
            # the foreground's root -- a mistuned unison rather than a degree of its own, which is
            # the one thing it was there to be. A fifth, a fourth, an octave down or the seventh.
            p["brain2_interval"] = extra.choice([7, 5, 12, -12, 10, -5])

    # ---- tuning -----------------------------------------------------------------------
    if want("adaptive"):
        p["purity_adapt"] = round(u(extra, 0.4, 1.0), 3)
    if want("guard"):
        p["purity_guard"] = round(u(extra, 0.3, 0.9), 3)
    if want("match"):
        p["match"] = round(u(extra, 0.3, 0.9), 3)
    if want("transpose"):
        p["transpose"] = extra.choice(["Fourth up", "Fifth up", "Octave up", "Fourth down", "Fifth down", "Octave down"])
    if want("keysfilter"):
        p["keys_filter"] = "One Euro"
    if extra.random() < 0.06:
        p["hold"] = "on"          # a switch: the tuning stops following the conductor

    # ---- the room ---------------------------------------------------------------------
    if want("nearfield"):
        p["near_ild"] = round(u(extra, 0.3, 0.9), 3)
    if want("comod"):
        p["far_comod"] = round(u(extra, 0.25, 0.8), 3)
    if want("envelop"):
        p["far_envelop"] = round(u(extra, 0.25, 0.9), 3)
    if want("depthlaw"):
        p["depth_law"] = round(u(extra, 0.3, 1.0), 3)
    if want("elev"):
        p["elev_near"] = round(u(extra, -0.4, 0.5), 3)
        p["elev_far"] = round(u(extra, 0.1, 0.9), 3)
    if want("binaural"):
        p["binaural"] = "Headphones"
        if extra.random() < 0.5:
            p["externalise"] = round(u(extra, 0.2, 0.7), 3)
    if want("presence"):
        p["presence"] = round(u(extra, 0.5, 3.5), 2)
    if want("farmode"):
        p["far_mode"] = extra.choice(["Scattering", "Colourless", "Rotating", "Rotating"])
        if p["far_mode"] == "Rotating":
            p["far_rotate"] = round(u(extra, 0.25, 0.8), 3)
    if want("fardiffuse"):
        p["far_diffuse"] = round(u(extra, 0.2, 0.9), 3)
    # far_freeze is NOT written any more (12.09.2026). It closes the far reverb's input and holds
    # whatever is in its tail -- which is a performance control and a good one, but a preset is
    # loaded into an EMPTY reverb, so it freezes silence and there is no far reverb for the rest of
    # the session. Measured on Init: with freeze on, the far plane's share of the output is 0.000
    # and the render is identical to far_level=0. It stood in 930 presets, and it is exactly what
    # Rene heard as "the far reverb hardly does anything". The knob stays, for hands.
    if want("farfreeze"):
        pass
    if want("earlyroom"):
        p["early_level"] = round(u(extra, 0.15, 0.6), 3)
        p["early_size"] = round(logu(extra, 3.0, 30.0), 2)
        p["early_absorb"] = round(u(extra, 0.2, 0.8), 3)
        p["early_width"] = round(u(extra, 0.4, 1.0), 3)
    if want("roommorph") and float(p.get("room_level", 0) or 0) > 0.02:
        p["room_morph"] = round(u(extra, 0.15, 0.85), 3)
    if want("archarmony") and float(p.get("arc", 0) or 0) > 0.05:
        p["arc_harmony"] = round(u(extra, 0.2, 0.8), 3)
    if want("arcclock"):
        p["arc_clock"] = "on"

    # ---- the fields, and chaos ---------------------------------------------------------
    if want("lenia"):
        p["lenia_rate"] = round(logu(extra, 0.5, 8.0), 3)
        p["lenia_growth"] = round(u(extra, 0.1, 0.28), 3)
    if want("chaos"):
        p["chaos_period"] = round(logu(extra, 20.0, 400.0), 1)
    if want("sympathy"):
        p["sympathy"] = round(u(extra, 0.15, 0.7), 3)
        p.setdefault("coherence", round(u(extra, 0.2, 0.7), 3))

    # ---- the details -------------------------------------------------------------------
    if want("pulse") and float(p.get("sub_level", 0) or 0) > 0.05:
        p["sub_pulse"] = round(logu(extra, 0.02, 0.4), 4)
    if want("fbbias") and float(p.get("fb_bus", 0) or 0) > 0.02:
        p["fb_bias"] = round(u(extra, -0.6, 0.6), 3)
    if want("partialspread"):
        p["partial_spread"] = round(u(extra, 0.3, 1.0), 3)
    if float(p.get("strike_level", 0) or 0) > 0.02 and p.get("brain_on", "on") != "off":
        # A strike that fires on every note of a conducted piece is a plucked instrument. Most of
        # these want it now and then instead, and in the clusters rather than evenly.
        if extra.random() < 0.7:
            p["strike_who"] = "Keys + Brain"
            p["strike_chance"] = round(u(extra, 0.08, 0.5), 3)
            if float(p.get("brain_cascade", 0) or 0) > 0.05 and extra.random() < 0.7:
                p["strike_cluster"] = round(u(extra, 0.4, 1.0), 3)
    if float(p.get("cosmos_send", 0) or 0) > 0.05 and want("cosmosswell"):
        p["cosmos_swell"] = round(u(extra, 0.3, 1.0), 3)
    for n in (1, 2, 3, 4):
        # Transport morphs spectra, so it belongs to the Harmonic type (the spectral table type was called
        # Wavetable in packs without a "format 2" line, which the loader translates).
        if p.get("src%d_type" % n) in ("Harmonic", "Wavetable") and want("transport"):
            p["src%d_transport" % n] = round(u(extra, 0.3, 1.0), 3)

    # ---- the body, and the patina ------------------------------------------------------
    # The Body was switched on by some styles but always with its default character: one
    # material, one pitch, one tone. Six thousand presets, one struck object.
    if float(p.get("body_level", 0) or 0) > 0.02 and want("bodychar"):
        p["body_material"] = extra.choice(["Wood", "Plate", "Bell", "String"])
        p["body_pitch"] = round(logu(extra, 0.35, 3.0), 3)
        p["body_tone"] = round(u(extra, 0.15, 0.85), 3)
        p["body_spread"] = round(u(extra, 0.1, 0.8), 3)
    if want("patina"):
        # Wow, hiss and age: the sound of the medium rather than of the instrument. Gentle by
        # default -- this is a patina, not a lo-fi effect.
        p["patina"] = round(u(extra, 0.1, 0.5), 3)
        p["patina_wow"] = round(u(extra, 0.1, 0.6), 3)
        # The hiss is the one part of the patina that measures as noise, and it is the third largest
        # source of it in the whole instrument: on a pure sine, patina 0.5 alone gives a spectral
        # flatness of 0.0008 and its hiss at 0.5 lifts that to 0.0019, against 0.000001 for the
        # untouched tone. (Air at 0.2 gives 0.0149, which is why that one is the exception now.)
        # Kept under a fifth: the medium should be heard as a medium, not as a bed of noise.
        p["patina_hiss"] = round(u(extra, 0.03, 0.2), 3)
        p["patina_age"] = round(u(extra, 0.1, 0.7), 3)

    # ---- the Vector --------------------------------------------------------------------
    # Four slots read as the corners of one square, and the point in it moved slowly. Nothing in
    # the library ever set it, so the four slots always played at their own fixed levels.
    live = sum(1 for n in (1, 2, 3, 4) if p.get("src%d_type" % n, "Additive" if n == 1 else "Off") != "Off")
    if live >= 3 and want("vector"):
        p["vec_amount"] = round(u(extra, 0.35, 1.0), 3)
        p["vec_x"] = round(u(extra, 0.15, 0.85), 3)
        p["vec_y"] = round(u(extra, 0.15, 0.85), 3)
        if extra.random() < 0.7:
            p["vec_wander"] = round(u(extra, 0.15, 0.7), 3)
            p["vec_rate"] = round(logu(extra, 0.004, 0.06), 5)

    # ---- the master, and a few corners -------------------------------------------------
    if want("mastertilt"):
        p["master_tilt"] = round(u(extra, -3.0, 3.0), 2)
        p["tilt_pivot"] = round(logu(extra, 300.0, 1600.0), 0)
    if extra.random() < 0.10:
        p["mono_guard"] = "on"          # the mono-safety net, for presets that widen hard
    if extra.random() < 0.12:
        p["near_lowcut"] = round(logu(extra, 25.0, 120.0), 1)
    if float(p.get("dly2_mix", 0) or 0) > 0.02 and extra.random() < 0.35:
        p["dly2_to_far"] = round(u(extra, 0.15, 0.7), 3)
    if p.get("z_mode", "Off") in ("Series", "Replace") and extra.random() < 0.25:
        p["z_route"] = "Parallel"
    if extra.random() < 0.06:
        p["freeze"] = "on"              # the tuning table held where it stands

    # ---- what a hand does to it --------------------------------------------------------
    # The Expression section is fixed routing: how far aftertouch pushes a voice forward, opens
    # it, lifts it; what the slide does to the two filters. The library had matrix routes from
    # the hands but never these, so every preset answered a key press the same way.
    if want("expression"):
        p["press_bright"] = round(u(extra, 0.15, 0.7), 3)
        if extra.random() < 0.6:
            p["press_distance"] = round(u(extra, 0.1, 0.6), 3)
        if extra.random() < 0.5:
            p["press_level"] = round(u(extra, 0.1, 0.5), 3)
        if extra.random() < 0.5:
            p["slide_cutoff"] = round(u(extra, 0.15, 0.7), 3)
        if extra.random() < 0.35 and p.get("z_mode", "Off") in ("Series", "Replace"):
            p["slide_z"] = round(u(extra, 0.15, 0.7), 3)
        if extra.random() < 0.3:
            p["bend_range"] = extra.choice([2, 5, 7, 12])

    # ---- on the clock ------------------------------------------------------------------
    # Tempo sync exists everywhere and was used nowhere: a drone has no beat, but a delay whose
    # time is four bars and an arc that turns over every sixty-four are the two places where a
    # host's tempo actually helps.
    if want("sync"):
        if float(p.get("dly_mix", 0) or 0) > 0.02:
            p["dly_sync_l"] = extra.choice(["4 bars", "2 bars", "1 bar", "8 bars"])
            p["dly_sync_r"] = extra.choice(["8 bars", "4 bars", "2 bars", "1 bar"])
        if float(p.get("dly2_mix", 0) or 0) > 0.02 and extra.random() < 0.6:
            p["dly2_sync_l"] = extra.choice(["8 bars", "4 bars", "2 bars"])
            p["dly2_sync_r"] = extra.choice(["16 bars", "8 bars", "4 bars"])
        if float(p.get("arc", 0) or 0) > 0.05 and extra.random() < 0.5:
            p["arc_sync"] = extra.choice(["64 bars", "32 bars", "16 bars"])
        if float(p.get("ens_mix", 0) or 0) > 0.02 and extra.random() < 0.35:
            p["ensemble_sync"] = extra.choice(["16 bars", "8 bars", "4 bars"])
        if float(p.get("cloud_send", 0) or 0) > 0.05 and extra.random() < 0.4:
            p["cloud_sync"] = extra.choice(["1/4", "1/8", "1/2"])
        if p.get("brain_on", "on") != "off" and extra.random() < 0.4:
            p["brain_sync"] = extra.choice(["8 bars", "4 bars", "16 bars"])


def make_preset(style, rng, textures, wavetables, impulses, shade, extra=None):
    # `extra` is a second random stream, used only by the source types added after the library
    # was first generated. It exists so that asking "does this style want a bowed string?" costs
    # nothing from `rng`: a style that does not want one never touches `extra` either, and its
    # pack comes out exactly as it always did.
    extra = extra if extra is not None else random.Random(0)
    p = {}
    for key, spec in style["params"].items():
        if key not in PARAMS:
            continue
        p[key] = draw(rng, spec)
    mod = apply_shade(p, style["modules"], shade)
    # A floor under the later slots. Measured over the library as it stood: 13 % of presets had one
    # source, 55 % two, 29 % three, 3 % four -- and the reason was not that the styles chose
    # sparseness but that most of them never named the later slots at all (src4 appears in five
    # styles of twenty-four, src3 in twenty-three but often at 0.15). A style that asks for MORE
    # keeps what it asked for; the floor only fills in where nothing was said. With sources that
    # can now enter at their own time, a third and a fourth are variety rather than mud.
    SLOT_FLOOR = {"src2": 0.85, "src3": 0.60, "src4": 0.35}
    for key, floor in SLOT_FLOOR.items():
        mod[key] = max(mod.get(key, 0.0), floor)
    on = lambda k: rng.random() < mod.get(k, 0.0)
    field = style["name"] in FIELD_STYLES     # an environment style: swamps allowed everywhere
    # Air is a band of noise laid over the top, and in the census it was the single setting most
    # tied to a harsh preset: nineteen percent of the library had it at 0.3 or more, and those
    # measured 1.67 times as flat as the rest. Capped at 0.3 -- unless the style is about air
    # (its own range reaches past 0.45), which keeps its upper half but not its top.
    air_hi = style["params"].get("air", (0.0, 0.3))
    air_hi = float(air_hi[-1]) if isinstance(air_hi, (list, tuple)) else 0.3
    if "air" in p:
        p["air"] = min(float(p["air"]), 0.45 if air_hi > 0.45 else 0.3)
    texture_file = wavetable_file = ""
    # One clip per slot where the slots differ (the Stretch type draws its own per slot); the
    # pack's texture field then carries them ';'-separated, one per slot, empty for a slot
    # without one. A preset whose slots share one clip keeps the single-path form.
    slot_textures = {}

    # Foundation ------------------------------------------------------------------------
    if not on("sub"):
        p["sub_level"] = 0.0
    elif p.get("sub_level", 0.0) < 0.1:
        p["sub_level"] = u(rng, 0.15, 0.4)
    if p.get("sub_level", 0.0) > 0.05:
        # Under the lowest sounding voice, not on the conductor's root. On the root it moves about
        # once a quarter of an hour, so in three quarters of the library the loudest thing in the
        # sound was the one thing that did not answer the keyboard -- measured with
        # `ambient_render --tonal`, a median of 77 % of the spectrum standing still against a
        # chord played a tritone away, where the built-in presets sit at 16 %. Lowest folds by
        # octaves back into the register it was already in, so the weight stays and the pitch
        # class follows. See Tools/library/sub_follows_chord.py, which retrofitted the 5439
        # presets that were generated before this line existed.
        p.setdefault("sub_source", "Lowest")
        p.setdefault("sub_glide", logu(rng, 2.0, 20.0))
        if rng.random() < 0.4:
            p["sub_binaural"] = u(rng, 1.5, 7.0)

    # Stack -----------------------------------------------------------------------------
    if on("stack"):
        p["stack"] = STACKS[rng.randrange(len(STACKS))]
        p["strands"] = rng.randint(3, 6)

    # Envelope shape: a few presets are played rather than generated -----------------------
    if on("keys"):
        p["brain_on"] = "off"
        p["attack"] = logu(rng, 0.5, 6.0)
        p["release"] = logu(rng, 4.0, 20.0)
        p["keys_depth"] = u(rng, 0.0, 0.4)

    # Source slots ----------------------------------------------------------------------
    def fill_slot(n, force=None):
        nonlocal texture_file, wavetable_file
        pre = f"src{n}_"
        # The first slot is the instrument's original voice, and its additive controls still carry
        # the names they had before there were four slots. Everything type-specific does have a
        # src1_ name. A key that does not exist is dropped by the parser without a word -- that is
        # how five thousand presets once lost their impulses -- so the mapping is written out.
        LEGACY = {"level": "osc_level", "partials": "partials", "tilt": "tilt",
                  "bright": "brightness", "odd_even": "odd_even", "inharmonic": "inharmonic",
                  "shimmer": "shimmer", "shimmer_rate": "shimmer_rate"}
        key_of = lambda k: (LEGACY[k] if (n == 1 and k in LEGACY) else pre + k)
        put = lambda k, v: p.__setitem__(key_of(k), v)
        got = lambda k, d=None: p.get(key_of(k), d)
        want_tex = on("texture") and textures.has_tonal(field)
        want_tab = on("usertable") and wavetables
        # Stretch: the style's clips read as a continuum. A style asks for it with a "stretch"
        # module weight; the field-recording style asks for it in every slot it fills.
        want_stretch = on("stretch") and textures
        # Short-circuited on the weight, so a style with none of these draws nothing at all.
        wb = mod.get("bow", 0.0)
        ws = mod.get("spectral", 0.0)
        want_bow = wb > 0.0 and extra.random() < wb
        want_spec = ws > 0.0 and textures.has_tonal(field) and extra.random() < ws
        if want_bow:
            kind = "Bow"
        elif want_spec:
            kind = "Spectral"
        elif want_stretch:
            kind = "Stretch"
        elif want_tex and (not want_tab or rng.random() < 0.5):
            kind = "Texture"
        elif want_tab:
            kind = "Wavetable"
        else:
            # The first slot is the voice the piece is built on, and noise is a colour rather
            # than a voice: a drone whose primary source is noise is a bed with nothing in it,
            # and four hundred of those in a library is three hundred too many. Noise stays in
            # the pool for the supporting slots and is drawn only for a style that asks for it
            # by name (the noise pack) when it is the first slot.
            # Measured in the mel-cepstral fingerprint over fifty presets of each: a texture in
            # the first slot spreads 6.1 against the additive bank's 4.5, and its nearest
            # neighbour is a third further away. The bank is the instrument's signature and keeps
            # the presets that do not draw a type at all; where a type IS drawn, it is one of the
            # ones that carry their own material.
            pool = SOURCE_TYPES if n > 1 else ["Wavetable", "Wavetable", "FM", "Texture", "Texture", "Texture"]
            # A style whose sound world really is noise -- tape hiss, a bunker, a signal that
            # decayed -- may lead with it; the rest may not, and that is the difference between
            # a colour and a voice.
            if n == 1 and rng.random() < mod.get("noiseprimary", 0.0):
                pool = ["Noise"]
            kind = pool[rng.randrange(len(pool))]
            if kind == "Texture" and not textures.has_tonal(field):
                kind = "Wavetable"
        if force is not None:                  # the tonal anchor asks for a type by name
            kind = force
        put("type", kind)
        # The first slot is the voice, the others are layers under it, and they may not be drawn
        # from the same range. A preset only becomes Additive when no type is drawn at all, and it
        # then keeps the parameter's own default of 1.0 -- so for four rounds of the library the
        # additive presets stood at full level over the Air and Breath bed and every other preset
        # stood at a third of it, over the same bed. Measured, source muted against the whole
        # preset, ninety presets of each: Additive 0.0 dB, Texture -9.0 dB, Wavetable -8.5 dB. That
        # is what "only the additive presets are played tonally" was -- the others followed the
        # note exactly and could not be heard doing it. 0.6 .. 1.0 puts a drawn first slot where
        # the additive one already was; see Tools/library/rebalance_voice.py, which repaired the
        # presets that were made before this line was right.
        put("level", u(rng, 0.6, 1.0) if n == 1 else u(rng, 0.15, 0.55))
        if kind != "Noise" and rng.random() < 0.4:   # independent fine drift: sources that beat like an ensemble
            put("drift", logu(rng, 0.8, 10.0))
        put("octave", rng.choice([-2, -1, 0, 0, 0, 1]))
        put("ratio", SLOT_RATIOS[rng.randrange(len(SLOT_RATIOS))])
        put("pan", u(rng, -0.8, 0.8))
        if kind == "Wavetable":
            if want_tab and not wavetable_file:
                put("table", "User")
                wavetable_file = wavetables[rng.randrange(len(wavetables))]
            else:
                put("table", TABLES[rng.randrange(len(TABLES) - 1)])
            put("pos", u(rng, 0.0, 1.0))
            put("pos_drift", u(rng, 0.05, 0.7))
        elif kind == "Noise":
            put("noise", style["noise"][rng.randrange(len(style["noise"]))])
            put("noise_q", u(rng, 0.25, 0.85))
            put("pos", u(rng, 0.05, 0.9))          # band centre / colour
            put("pos_drift", u(rng, 0.05, 0.8))
            # Lower than it was (0.12 .. 0.45): a noise slot is a colour under the tone, and at
            # the old level it was the tone. Measured: presets with one were 1.33x as flat.
            put("level", u(rng, 0.07, 0.28))
            if got("noise") == "Crackle":
                put("density", logu(rng, 1.5, 30.0))
            put("follow", "Note" if (got("noise") in ("Band", "Wind") and rng.random() < 0.4) else "Free")
        elif kind == "FM":
            put("fm_ratio", rng.choice([0.5, 1.0, 1.5, 2.0, 2.0, 3.0, 4.0, 5.0, 7.0]))
            put("fm_index", logu(rng, 0.3, 3.5))
        elif kind == "Additive":
            # a second (or third) additive bank on its own just ratio: the classic Rich stack, but
            # with its own spectrum and its own slow pitch drift
            put("partials", rng.randint(6, 28))
            put("tilt", u(rng, 0.8, 2.2))
            put("bright", u(rng, 0.3, 0.9))
            put("odd_even", u(rng, -0.5, 0.5) if rng.random() < 0.5 else 0.0)
            if rng.random() < 0.3:
                put("inharmonic", u(rng, 0.05, 0.4))
            put("shimmer", u(rng, 0.2, 0.6))
            put("shimmer_rate", logu(rng, 0.03, 0.4))
            put("drift", logu(rng, 1.0, 8.0))
        elif kind == "Bow":
            # The gesture is Force against Speed, and the two are worth setting against each other
            # rather than both up: light and fast is breath, heavy and slow is tone. Position is
            # where the bow sits along the string, Bright the loop filter that decides how long
            # the upper partials last.
            heavy = extra.random() < 0.5
            put("bow_force", u(extra, 0.45, 0.9) if heavy else u(extra, 0.1, 0.45))
            put("bow_speed", u(extra, 0.12, 0.4) if heavy else u(extra, 0.35, 0.85))
            put("pos", u(extra, 0.1, 0.8))
            put("bright", u(extra, 0.35, 0.9))
            # In the FIRST slot this material is the voice and has to stand where the additive
            # default stands; below it, it is a layer. See the measurement at the generic draw.
            put("level", u(extra, 0.55, 1.0) if n == 1 else u(extra, 0.2, 0.5))
            if extra.random() < 0.6:
                put("drift", logu(extra, 1.0, 7.0))
        elif kind == "Spectral":
            # A recording rebuilt rather than replayed. Rate is mostly slow and sometimes stopped
            # dead -- at zero the clip becomes one held chord, which is the thing this type can do
            # and nothing else in the instrument can. Breath leans towards the noisy half more
            # often than the tonal one: a bed wants air in it.
            r = extra.random()
            put("spec_rate", 0.0 if r < 0.22 else (u(extra, 0.05, 0.6) if r < 0.75 else u(extra, 0.6, 2.5)))
            put("spec_breath", u(extra, -0.8, 0.8))
            put("pos", u(extra, 0.0, 1.0))
            put("pos_drift", u(extra, 0.05, 0.5))
            put("bright", u(extra, 0.3, 0.85))
            # In the FIRST slot this material is the voice and has to stand where the additive
            # default stands; below it, it is a layer. See the measurement at the generic draw.
            put("level", u(extra, 0.6, 1.0) if n == 1 else u(extra, 0.25, 0.6))
            own = textures.tonal_pick(extra, field)
            slot_textures[n] = own
            if not texture_file:
                texture_file = own
            pitched = bool(PITCHED.search(own))
            put("follow", "Note" if (pitched or extra.random() < 0.5) else "Free")
            if extra.random() < 0.5:
                put("drift", logu(extra, 0.5, 4.0))
        elif kind == "Stretch":
            # The clip as a continuum. The window (Grain) sits where Paulstretch is smooth, the
            # factor is log-spread from "slowed" to "geological", and each Stretch slot draws its
            # own clip so four slots are four places. Free unless the clip carries a pitch, and
            # even then mostly Free: a field recording pitched to the note is a choice, not a rule.
            put("grain", logu(rng, 150.0, 340.0))
            put("stretch", logu(rng, 6.0, 300.0))
            put("xfade", u(rng, 0.05, 0.25))
            put("pos", u(rng, 0.0, 1.0))
            put("pos_drift", u(rng, 0.1, 0.6))
            # Higher than a Texture slot: a stretched recording has no attacks to carry it, and at
            # the Texture slot's range a four-slot field preset measured -47 dBFS.
            # In the FIRST slot this material is the voice and has to stand where the additive
            # default stands; below it, it is a layer. See the measurement at the generic draw.
            put("level", u(rng, 0.6, 1.0) if n == 1 else u(rng, 0.3, 0.7))
            if rng.random() < 0.5:
                put("drift", logu(rng, 0.5, 4.0))
            # The stretched bed is where the environments belong: a recording read as a
            # continuum wants no pitch, and the field recordings are drawn here and nowhere else
            # (except by an environment style, whose Texture slots may hold them too).
            own = textures.bed_pick(rng)
            slot_textures[n] = own
            if not texture_file:
                texture_file = own
            pitched = bool(PITCHED.search(own))
            put("follow", "Note" if (pitched and rng.random() < 0.35) else "Free")
        else:                                            # Texture
            # Draw the OVERLAP, not the density. What decides whether a cloud is heard as a cloud
            # is how many grains sound at once -- Density times Grain -- and drawing the two
            # independently left that as a by-product: measured over 7858 texture slots the median
            # overlap was 2.37, 44 % of them under 2. At two the ear counts the grains, and the
            # library sounded thin for that reason and not because the engine ran out of them (45
            # slots of 7858 reached the ceiling of 64). Rene: "die Wolken klingen nicht wirklich
            # luftig und dicht, sondern eher duenn."
            #
            # Four to thirty, log-uniform: four is a texture that still shows its grain, thirty is
            # air. The density that follows is clamped to the parameter's own ceiling, which is why
            # that ceiling went from sixty a second to two hundred -- at sixty, a sixty-millisecond
            # grain could not exceed an overlap of three and a half however dense the setting.
            grain_ms = logu(rng, 60.0, 800.0)
            overlap = logu(rng, 4.0, 30.0)
            density = min(200.0, max(1.0, overlap * 1000.0 / grain_ms))
            put("grain", grain_ms)
            put("density", density)
            # Grains: enough for the overlap the density and length ask for, plus headroom and the
            # style's bias. Too few and the slot drops grains, which made more density quieter
            # instead of denser -- the ceiling used to be eight for everyone.
            gran = style["granular"]
            put("grains", int(min(128, max(4, math.ceil(density * grain_ms / 1000.0 * 1.8 * gran["grains"]) + 4))))
            # Spread: the window the start points are drawn from. Near zero the same fragment
            # repeats and the clip freezes into a drone; near one a grain may come from anywhere.
            put("spread", math.exp(u(rng, math.log(gran["spread"][0]), math.log(gran["spread"][1]))))
            # A texture slot is now level-matched to the other two, so it needs less than before.
            # In the FIRST slot this material is the voice and has to stand where the additive
            # default stands; below it, it is a layer. See the measurement at the generic draw.
            put("level", u(rng, 0.45, 1.0) if n == 1 else u(rng, 0.12, 0.42))
            if not texture_file and textures:
                texture_file = textures.tonal_pick(rng, field)
            slot_textures[n] = texture_file
            # A bed in a grain slot (an environment style only) gets longer grains: sixty
            # milliseconds of traffic is gravel, whatever the rest of the preset does.
            if textures.is_bed(texture_file) and grain_ms < 150.0:
                put("grain", logu(rng, 150.0, 340.0))
            # Only a clip with a detected pitch (TextureGen puts the note in the name) can be
            # transposed to the played note; the rest are played free, as a bed.
            # Only a clip whose pitch was detected can be transposed to the played note -- and
            # not a field recording even then: fifty-seven of them carry a note in the name
            # because something in the room happened to hum, and playing a swamp at concert
            # pitch is not tonal granular synthesis, it is a mistake with a fundamental.
            own_clip = slot_textures.get(n, texture_file)
            pitched = (bool(own_clip) and bool(PITCHED.search(own_clip))
                       and not own_clip.startswith("FieldRecordings/"))
            put("follow", "Note" if (pitched and rng.random() < 0.75) else "Free")

    # The first slot: for six thousand eight hundred presets it was the additive bank and nothing
    # else -- src1_type appears in not one of them. It is the same slot as the other three, so it
    # can be a wavetable, an FM pair, a grain texture, a noise colour, a stretched recording, a
    # bowed string or a spectral model, and now it is, in the share of presets a style asks for.
    if extra.random() < mod.get("slot1", 0.0):
        fill_slot(1)
        p.setdefault("strands", 1)      # the strand bank belongs to the additive type alone
        # A slot that is not the bank wants something beside it more often than not: two thirds
        # of the sound is then the type the style chose, and a third is what it is set against.
        if p.get("src1_type", "Additive") != "Additive" and p.get("src2_type", "Off") == "Off" and extra.random() < 0.55:
            fill_slot(2)
    # An additive bank with nothing beside it is the instrument's signature -- Rich's stack, the
    # thing it was built to do -- and it is also the least varied preset it can make. A share of
    # them keep it; the rest get something set against the bank.
    if (p.get("src1_type", "Additive") == "Additive" and p.get("src2_type", "Off") == "Off"
            and p.get("src3_type", "Off") == "Off" and extra.random() < 0.62):
        fill_slot(2)
    if on("src2"):
        fill_slot(2)
    if on("src3"):
        fill_slot(3)
    if on("src4"):
        fill_slot(4)

    # The tonal anchor: unless the style is an environment, something sits on the note. A
    # preset whose sounding slots are all samples played Free is a bed with nothing in it --
    # the key, the just scale, the conductor's chords all fall on deaf ears. First choice is
    # the slot that already holds a pitched clip, played to the note; failing that a wavetable
    # in a free slot; failing that the bank back in the first slot.
    if not field:
        def slot_type(n):
            return p.get("src1_type", "Additive") if n == 1 else p.get(f"src{n}_type", "Off")

        def on_note(n):
            t = slot_type(n)
            if t in ("Additive", "Wavetable", "FM", "Bow"):
                return True
            return t in ("Texture", "Stretch", "Spectral") and p.get(f"src{n}_follow", "Note") == "Note"

        if not any(on_note(n) for n in range(1, 5)):
            pitched_slot = next((n for n in range(1, 5)
                                 if slot_type(n) in ("Texture", "Spectral")
                                 and not textures.is_bed(slot_textures.get(n, texture_file))), None)
            free_slot = next((n for n in range(2, 5) if slot_type(n) == "Off"), None)
            if pitched_slot is not None:
                p[f"src{pitched_slot}_follow"] = "Note"
            elif free_slot is not None:
                fill_slot(free_slot, force="Wavetable")
            else:
                p["src1_type"] = "Additive"
    apply_shade_granular(p, shade)

    # Z-plane morphing filter -------------------------------------------------------------
    if on("zplane"):
        # One in five z-planes is Modal: the same shape read as a bank of resonators that rings
        # rather than a cascade that shapes. Only the shapes that are physical objects -- a
        # resonator bank on a phaser is a curiosity, on a bell it is the point.
        if rng.random() < 0.2 and Z_MODAL_SHAPES:
            p["z_mode"] = "Modal"
            p["z_shape"] = Z_MODAL_SHAPES[rng.randrange(len(Z_MODAL_SHAPES))]
            p["z_decay"] = logu(rng, 0.4, 18.0)
            p["z_damp"] = u(rng, 0.3, 0.95)
        else:
            # Replace less often than Series (was 0.45): with the voice filter gone, the z-plane
            # alone shapes the sound, and those presets measured 1.46x as flat with the widest
            # spread of all (p90 0.083 against the built-ins' 0.030).
            p["z_mode"] = "Replace" if rng.random() < 0.25 else "Series"
            p["z_shape"] = Z_SHAPES[rng.randrange(len(Z_SHAPES))]
        p["z_z"] = u(rng, 0.0, 0.7)      # the cube's third axis
        p["z_x"] = u(rng, 0.1, 0.9)
        p["z_y"] = u(rng, 0.1, 0.9)
        p["z_rate"] = logu(rng, 0.006, 0.25)
        p["z_depth"] = u(rng, 0.2, 0.9)
        p["z_res"] = u(rng, 0.2, 0.7)
        p["z_keytrack"] = u(rng, 0.0, 0.6) if rng.random() < 0.4 else 0.0
        p["z_mix"] = u(rng, 0.35, 0.8) if p["z_mode"] == "Replace" else u(rng, 0.3, 0.9)

    # The delay ducks its own loop on transients, so a fresh attack does not have to fight the
    # brightness of the last one's tail.
    if p.get("dly_mix", 0.0) and rng.random() < 0.35:
        p["dly_duck"] = u(rng, 0.25, 0.8)

    # Cosmos ------------------------------------------------------------------------------
    if on("cosmos"):
        p["cosmos_send"] = u(rng, 0.15, 0.6)
        p["cosmos_return"] = u(rng, 0.3, 0.8)
        p["cosmos_to_far"] = u(rng, 0.2, 0.8)
        which = rng.random()
        if which < 0.3:
            p["cosmos_shift"] = u(rng, -80.0, 80.0)
            p["cosmos_shift_drift"] = u(rng, 0.1, 0.7)
        elif which < 0.55:
            p["cosmos_res"] = u(rng, 0.2, 0.7)
            p["cosmos_res_pitch"] = rng.choice([0.5, 1.0, 1.5, 2.0, 3.0, 4.0])
            p["cosmos_res_fb"] = u(rng, 0.6, 0.92)
        elif which < 0.75:
            p["cosmos_nebula"] = u(rng, 0.2, 0.8)
            p["cosmos_smear"] = u(rng, 0.4, 0.95)
        else:
            p["cosmos_shimmer"] = u(rng, 0.2, 0.7)
            p["cosmos_shimmer_pitch"] = SHIMMER_PITCH[rng.randrange(len(SHIMMER_PITCH))]
        if rng.random() < 0.35:
            p["cosmos_vowel"] = u(rng, 0.2, 0.7)
            p["cosmos_vowel_rate"] = logu(rng, 0.008, 0.2)

    # Granular cloud ------------------------------------------------------------------------
    if on("cloud"):
        p["cloud_send"] = u(rng, 0.15, 0.6)
        p["cloud_density"] = logu(rng, 3.0, 40.0)
        p["cloud_size"] = logu(rng, 60.0, 600.0)
        p["cloud_pitch"] = u(rng, 0.0, 0.7)
        p["cloud_spray"] = logu(rng, 0.15, 1.8)
        p["cloud_level"] = u(rng, 0.4, 0.85)

    # Feedback loop. The ceiling stays low: past 0.25 the bus climbs and collapses to mono.
    if on("feedback"):
        p.setdefault("fb_bus", u(rng, 0.04, 0.18))
        p["fb_bus"] = min(float(p["fb_bus"]), 0.2)
        p.setdefault("fb_tone", logu(rng, 400.0, 4000.0))
        p.setdefault("fb_drive", u(rng, 0.3, 0.9))
        if rng.random() < 0.35:
            p["fb_fm"] = u(rng, 0.03, 0.18)
        if rng.random() < 0.5:
            p.setdefault("fb_tape", u(rng, 0.2, 0.8))
    else:
        p.pop("fb_bus", None)
        p.pop("fb_fm", None)

    # Second delay, room, coherence, portamento ---------------------------------------------
    if on("delay2"):
        p["dly2_mix"] = u(rng, 0.1, 0.35)
        p["dly2_time_l"] = logu(rng, 0.5, 3.5)
        p["dly2_time_r"] = logu(rng, 0.7, 4.0)
        p["dly2_feedback"] = u(rng, 0.3, 0.75)
        p["dly2_cross"] = u(rng, 0.2, 0.9)
        p["dly2_damp"] = u(rng, 0.4, 0.9)
    impulse_file = ""
    if on("room") and impulses:
        p["room_level"] = u(rng, 0.15, 0.6)
        p["room_source"] = "Far" if rng.random() < 0.6 else "Near"
        p["room_predelay"] = logu(rng, 5.0, 150.0)
        p["room_highcut"] = logu(rng, 1500.0, 9000.0)
        impulse_file = impulses[rng.randrange(len(impulses))]
    if on("coherence"):
        p["coherence"] = u(rng, 0.2, 0.8)
        p["coherence_depth"] = u(rng, 0.2, 0.8)
        p["coherence_rate"] = u(rng, 0.3, 3.0)
    if on("portamento"):
        p["portamento"] = logu(rng, 0.5, 12.0)
        p["porta_gravity"] = u(rng, 0.2, 0.9)
    if rng.random() < 0.12:
        p["air_mode"] = "Ghost"
        p["air"] = max(float(p.get("air", 0.2)), 0.2)
    if rng.random() < 0.25:
        p["purity_drift"] = u(rng, 0.1, 0.5)
        p["purity_rate"] = logu(rng, 0.003, 0.05)

    # The Rich refinements ------------------------------------------------------------------
    if on("phase"):                                  # the binaural phase field, slow
        p["phase_width"] = u(rng, 0.2, 0.8)
        p["phase_rate"] = logu(rng, 0.006, 0.08)
    if float(p.get("breath", 0.0)) > 0.05 and rng.random() < 0.4:
        p["doppler"] = u(rng, 0.2, 0.8)
    if on("blur"):                                   # attacks wiped into texture
        p["blur_mix"] = u(rng, 0.2, 0.6)
        p["blur_smear"] = u(rng, 0.3, 0.9)
    if on("filtermodel"):
        model = FILTER_MODELS[rng.randrange(len(FILTER_MODELS))]
        p["filter_model"] = model
        if model == "Comb":
            p["cutoff"] = logu(rng, 80.0, 700.0); p["resonance"] = u(rng, 0.4, 0.8); p["keytrack"] = u(rng, 0.5, 1.0)
        elif model == "Formant":
            p["cutoff"] = logu(rng, 150.0, 6000.0); p["resonance"] = u(rng, 0.3, 0.8); p["filter_drift"] = u(rng, 0.4, 1.0)
        elif model == "HP 12":
            p["cutoff"] = logu(rng, 60.0, 400.0)
        elif model in ("BP 12", "Notch", "Peak"):
            p["cutoff"] = logu(rng, 200.0, 3000.0); p["resonance"] = u(rng, 0.3, 0.8); p["filter_drift"] = u(rng, 0.3, 0.9)
        elif model == "Ladder":
            p["resonance"] = u(rng, 0.3, 0.85); p["filter_drive"] = u(rng, 0.0, 0.5)
    if on("strike") or (on("keys") and rng.random() < 0.5):   # the struck foreground against the vast background
        p["strike_level"] = u(rng, 0.2, 0.6)
        p["strike_type"] = STRIKE_TYPES[rng.randrange(len(STRIKE_TYPES))]
        p["strike_decay"] = logu(rng, 0.1, 1.8)
        p["strike_damp"] = u(rng, 0.15, 0.8)
        if rng.random() < 0.3:
            p["strike_who"] = "Keys + Brain"
    if on("absorb"):                                 # echoes that drown instead of merely fading
        p["dly_absorb"] = u(rng, 0.3, 1.0)
        if "dly2_mix" in p:
            p["dly2_absorb"] = u(rng, 0.3, 1.0)
    if on("tide"):                                   # the whole instrument leans over minutes
        p["tide"] = logu(rng, 2.0, 12.0)
        p["tide_period"] = logu(rng, 4.0, 30.0)
    if on("rotate"):                                 # the background turns
        p["far_rotate"] = u(rng, 0.2, 0.8)
    # ---- the mixing desk. Every one of these is neutral at its default, so a preset only has
    # them because its style asked for them.
    if on("narrow") and p.get("far_level", 0.8) > 0.3:
        # The funnel: the background narrower than the foreground reads as distance rather than
        # as width. Deep patches narrow further -- that is the whole point of the trick.
        deep = p.get("depth", 0.5)
        p["far_width"] = round(u(rng, 0.35, 0.85) * (1.0 - 0.25 * deep) + 0.1, 3)
    if on("haas") and p.get("near_mix", 0.0) >= 0.0:
        p["haas"] = round(u(rng, 0.15, 0.5), 3)
        p["haas_time"] = round(u(rng, 10.0, 22.0), 1)
    if on("microshift") and p.get("ens_mix", 0.0) > 0.05:
        p["ens_mode"] = "Microshift"
        p["ens_depth"] = round(u(rng, 0.3, 1.0), 3)     # the detune, up to twelve cents
        p["ens_rate"] = round(logu(rng, 0.02, 0.09), 4)  # and how slowly it wanders (0.02 is the knob's floor)
    if on("fold"):
        p["filter_fold"] = round(u(rng, 0.08, 0.45), 3)
    open_up(p, mod, extra, rng)
    # The instrument's own banks -- 257 Cosmos presets, 41 Strikes, 156 filter shapes -- were
    # written for its sections and nothing in the library ever used one. A preset that has the
    # section open takes one now and then, which is a whole character in one draw.
    if extra.random() < mod.get("banks", 0.0):
        for bank, key, gate in ((COSMOS_BANK, "cosmos_send", float(p.get("cosmos_send", 0) or 0) > 0.05),
                                (STRIKE_BANK, "strike_level", float(p.get("strike_level", 0) or 0) > 0.02),
                                (Z_BANK, "z_mode", p.get("z_mode", "Off") in ("Series", "Replace"))):
            if not (bank and gate and extra.random() < 0.6):
                continue
            name, settings = bank[extra.randrange(len(bank))]
            for kv in settings.split(";"):
                if "=" not in kv:
                    continue
                k, v = kv.split("=", 1)
                k = k.strip()
                if k not in PARAMS:
                    continue
                # The send and the level belong to the preset that is taking the layer, not to
                # the layer: a bank entry sets them for a demonstration, and here they would
                # overwrite a balance the style already chose.
                if k in ("cosmos_send", "cosmos_return", "strike_level", "z_mix"):
                    continue
                # Kept as the bank wrote it. A choice parameter accepts a name OR an index,
                # so turning "+12" into a number would silently pick the twelfth entry: that is
                # how a shimmer pitch of five semitones came back as two octaves.
                p[k] = v.strip()
    p["seed"] = rng.randrange(1, 9999)

    # brain_high must stay above brain_low, and hold_max above hold_min
    if "brain_low" in p and "brain_high" in p and p["brain_high"] <= p["brain_low"] + 6:
        p["brain_high"] = p["brain_low"] + 12
    if "brain_hold_min" in p and "brain_hold_max" in p and p["brain_hold_max"] < p["brain_hold_min"] * 1.5:
        p["brain_hold_max"] = p["brain_hold_min"] * 2.0
    matrix, envs = modulation_for(p, style, rng, shade[0])
    envs = stagger_entries(p, envs, extra)
    matrix = add_hands(p, style, rng, matrix)
    if len(set(slot_textures.values())) > 1:
        texture_file = ";".join(slot_textures.get(k, "") for k in range(1, 5))
    # Room Morph goes from the preset's impulse to a second one, and the second one travels with the
    # preset now (the pack line's ninth field). Without it the morph did nothing -- or blended into
    # whatever B a player had loaded before -- so a preset gets a partner or loses the morph.
    impulse_b = morph_partner(impulse_file, impulses, extra) if (impulse_file and "room_morph" in p) else ""
    if not impulse_b:
        p.pop("room_morph", None)
    return p, texture_file, wavetable_file, impulse_file, impulse_b, matrix, envs


def morph_partner(name, pool, rng):
    """The impulse Room Morph should go to: a designed pair's other half first (hall_012a and
    hall_012b share their seed and their noise, so the morph between them never dips), then another
    impulse of the same family, then any other."""
    stem, ext = os.path.splitext(name)
    if stem[-1:] in ("a", "b"):
        other = stem[:-1] + ("b" if stem[-1] == "a" else "a") + ext
        if other in pool:
            return other
    family = re.sub(r"_\d+[ab]?$", "", stem)
    same = [f for f in pool if f != name and f.startswith(family + "_")]
    rest = same or [f for f in pool if f != name]
    return rest[rng.randrange(len(rest))] if rest else ""


def settings_string(p):
    """Only what differs from the default: applyPreset resets everything first."""
    parts = []
    for key, value in p.items():
        section, lo, hi, default = PARAMS[key]
        if isinstance(value, str):
            parts.append(f"{key}={value}")
        elif abs(float(value) - default) > 1e-6:
            parts.append(f"{key}={fmt(key, value)}")
    return ";".join(parts)


# ---------------------------------------------------------------- descriptors

def norm(v, lo, hi):
    return min(max((v - lo) / (hi - lo), 0.0), 1.0)


def descriptors(p):
    """An estimate of what the preset will sound like, from its settings. Six numbers in 0..1,
    ranked against the whole library afterwards."""
    g = lambda k, d=None: float(p[k]) if k in p and not isinstance(p[k], str) else (
        PARAMS[k][3] if d is None else d)
    s = lambda k: p.get(k, "")

    cutoff = g("cutoff")
    bright = (0.35 * g("brightness")
              + 0.20 * norm(math.log(max(cutoff, 40.0)), math.log(200.0), math.log(9000.0))
              + 0.15 * norm(math.log(max(g("far_highcut"), 500.0)), math.log(1000.0), math.log(12000.0))
              + 0.12 * (1.0 - norm(g("tilt"), 0.4, 2.8))
              + 0.10 * g("air") + 0.08 * g("shimmer"))

    motion = (0.22 * norm(math.log(max(g("shimmer_rate"), 0.005)), math.log(0.02), math.log(1.2))
              + 0.15 * g("shimmer")
              + 0.15 * (g("z_depth") * norm(math.log(max(g("z_rate"), 0.004)), math.log(0.005), math.log(0.6))
                        if s("z_mode") in ("Series", "Replace") else 0.0)
              + 0.12 * g("ens_depth") * g("ens_mix")
              + 0.12 * (1.0 - norm(math.log(max(g("brain_rate"), 2.0)), math.log(8.0), math.log(250.0)))
              + 0.10 * g("filter_drift") + 0.08 * g("breath")
              + 0.06 * max(g("src2_pos_drift") if s("src2_type") == "Wavetable" else 0.0,
                           g("src3_pos_drift") if s("src3_type") == "Wavetable" else 0.0))

    width = (0.30 * g("spread") + 0.22 * g("itd") + 0.18 * norm(g("width"), 0.6, 1.9)
             + 0.15 * g("pan_drift") + 0.15 * g("ens_mix"))

    noisy = (0.28 * g("inharmonic") + 0.18 * g("air")
             + 0.14 * (1.0 if s("src2_type") == "Texture" or s("src3_type") == "Texture" else 0.0)
             + 0.14 * g("cloud_send") + 0.12 * g("fb_drive") * min(g("fb_bus") * 6.0, 1.0)
             + 0.08 * g("fb_tape") + 0.06 * min(g("src2_fm_index") / 6.0, 1.0))

    bass = (0.34 * g("sub_level") + 0.22 * (1.0 - norm(g("brain_low"), 24.0, 60.0))
            + 0.16 * (1.0 - norm(math.log(max(g("cutoff"), 40.0)), math.log(200.0), math.log(6000.0)))
            + 0.14 * norm(g("bass_mono"), 60.0, 300.0)
            + 0.14 * (1.0 - norm(g("pad_low_cut"), 0.0, 200.0)))
    if s("sub_octave") == "-2":
        bass = min(1.0, bass + 0.08)

    density = (0.26 * norm(g("brain_density"), 1.0, 10.0)
               + 0.16 * norm(g("strands"), 1.0, 6.0)
               + 0.14 * norm(g("partials"), 4.0, 32.0)
               + 0.14 * (1.0 - norm(math.log(max(g("brain_rate"), 2.0)), math.log(8.0), math.log(250.0)))
               + 0.12 * ((0.5 if s("src2_type") not in ("", "Off") else 0.0)
                         + (0.5 if s("src3_type") not in ("", "Off") else 0.0))
               + 0.10 * g("cloud_send") + 0.08 * norm(g("ens_mix"), 0.0, 1.0))
    if s("brain_on") == "off":
        density *= 0.55
    return [bright, motion, width, noisy, bass, density]


def tag_bits(p, d):
    """d: the ranked descriptors."""
    bright, motion, width, noisy, bass, density = d
    t = set()
    t.add("Keys" if p.get("brain_on") == "off" else "Generative")
    if float(p.get("cosmos_send", 0) or 0) > 0.05: t.add("Cosmos")
    if float(p.get("fb_bus", 0) or 0) > 0.02 or float(p.get("fb_fm", 0) or 0) > 0.02: t.add("Feedback")
    if p.get("src2_type") or p.get("src3_type"): t.add("Sources")
    if any(k in str(p.get("scale", "")) for k in ("JI", "Harmonic", "Subharmonic", "Pythag", "Bohlen", "Otonal", "Slendro")):
        t.add("JustIntonation")
    if float(p.get("sub_level", 0) or 0) > 0.05: t.add("Sub")
    if p.get("stack"): t.add("Stack")
    if float(p.get("air", 0) or 0) >= 0.3: t.add("Air")
    if bright < 0.3: t.add("Dark")
    if bright > 0.7: t.add("Bright")
    if motion < 0.3: t.add("Calm")
    if motion > 0.7: t.add("Moving")
    if noisy < 0.4: t.add("Tonal")
    if noisy > 0.7: t.add("Noisy")
    if width > 0.7: t.add("Wide")
    if bass > 0.7: t.add("Bass")
    if density > 0.7: t.add("Dense")
    if density < 0.3: t.add("Sparse")
    return sum(1 << TAGS.index(x) for x in t)


def rank(v):
    order = np.argsort(np.argsort(v))
    return order / max(len(v) - 1, 1)


def layout(desc):
    """Map positions: half the rank of the two strongest directions, half a principal-component
    projection, then every preset is pushed into its own cell of a fine grid so nothing hides
    underneath anything else."""
    x = desc.copy()
    x = (x - x.mean(axis=0)) / (x.std(axis=0) + 1e-9)
    u_, s_, vt = np.linalg.svd(x - x.mean(axis=0), full_matrices=False)
    pc = u_[:, :2] * s_[:2]
    xy = np.zeros((len(desc), 2))
    xy[:, 0] = 0.5 * rank(desc[:, 0]) + 0.5 * rank(pc[:, 0])     # brightness / first component
    xy[:, 1] = 0.5 * rank(desc[:, 4]) + 0.5 * rank(pc[:, 1])     # weight / second component

    # Grid de-collision: n presets, a grid with about 3n cells, spiral search for a free one.
    n = len(xy)
    side = int(math.ceil(math.sqrt(n * 3.0)))
    taken = {}
    order = np.argsort(-desc[:, 5])          # densest first, they get the best cells
    for i in order:
        cx = min(side - 1, max(0, int(xy[i, 0] * side)))
        cy = min(side - 1, max(0, int(xy[i, 1] * side)))
        if (cx, cy) not in taken:
            taken[(cx, cy)] = i
            continue
        placed = False
        for r in range(1, side):
            for dx in range(-r, r + 1):
                for dy in (-r, r) if abs(dx) < r else range(-r, r + 1):
                    a, b = cx + dx, cy + dy
                    if 0 <= a < side and 0 <= b < side and (a, b) not in taken:
                        taken[(a, b)] = i
                        placed = True
                        break
                if placed: break
            if placed: break
    out = np.zeros((n, 2))
    for (cx, cy), i in taken.items():
        out[i] = ((cx + 0.5) / side, (cy + 0.5) / side)
    return out


# ---------------------------------------------------------------- assets and names

PITCHED = re.compile(r"_[A-G]#?-?\d+\.wav$")


def rejected_clips(dirname):
    """Names Tools/library/check_textures.py has flagged as unusable."""
    path = os.path.join(dirname, "rejected.txt")
    if not os.path.isfile(path):
        return set()
    out = set()
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if line:
            out.add(line)
    return out


# Which styles are environments first: their grain and spectral slots may carry a swamp, and a
# preset of theirs need not sit on any note. Everyone else gets tone in the tonal slots and the
# environments only as stretched beds. Measured before this rule: the packs were tagged Noisy
# twice as often and Tonal half as often as the built-ins, 58 % of all sample references were
# field recordings (the pool weighted them double for every style), and 1061 presets had no slot
# on the note at all -- the pitch played was irrelevant, the just scale with it.
FIELD_STYLES = {"Field Recordings"}
# What "tonal material" is, measured (Tools/library/clip_tonality.py): periodic enough to have
# a pitch, and with its weight below the region where periodic turns into piercing -- a
# referee's whistle at F6 is as periodic as a cello and was in the tonal slots.
TONAL_HARM_MIN = 0.7
TONAL_CENTROID_MAX = 2500.0
_TONALITY = {}


def tonality():
    """Per-clip measurements, or an empty dict when the file was never written (then the folder
    line alone decides, as it did before)."""
    if not _TONALITY:
        _TONALITY["_"] = {}
        _TONALITY["silent"] = set()
        path = os.path.join(ROOT, "Library", "tonality.json")
        if os.path.exists(path):
            try:
                blob = json.load(open(path, encoding="utf-8"))
                _TONALITY["_"] = blob.get("clips", {})
                _TONALITY["silent"] = set(blob.get("silent", []))
            except (ValueError, OSError):
                pass
    return _TONALITY["_"]


def is_tonal_clip(rel):
    """A clip that may carry a tonal slot: from the pitched folder, and measured tonal when a
    measurement exists."""
    if not rel.startswith("Textures/"):
        return False
    m = tonality().get(rel)
    if m is None:
        return True
    return m.get("harm", 1.0) >= TONAL_HARM_MIN and m.get("centroid", 0.0) <= TONAL_CENTROID_MAX


class ClipPools:
    """The clips a style may draw from, in two shelves: `tonal` for the slots that play a note
    (Texture, Spectral) and `beds` for the ones that read a recording as a continuum (Stretch).
    `everything` is both, for a style that is an environment and for the checks that only ask
    whether there are clips at all."""

    def __init__(self, tonal, beds, everything):
        self.tonal, self.beds, self.everything = tonal, beds, everything
        self._tonal_set = set(tonal)

    def __len__(self):
        return len(self.everything)

    def __getitem__(self, i):
        return self.everything[i]

    def has_tonal(self, field):
        return bool(self.everything) if field else bool(self.tonal)

    def tonal_pick(self, rng, field):
        pool = self.everything if field else (self.tonal or self.everything)
        return pool[rng.randrange(len(pool))]

    def bed_pick(self, rng):
        pool = self.beds or self.everything
        return pool[rng.randrange(len(pool))]

    def is_bed(self, rel):
        return bool(rel) and rel not in self._tonal_set


def texture_pool(dirname, style_name, rejected, fielddir=None, fieldshare=0.5):
    """The style's clips, as two shelves (see ClipPools). Duds are left out.

    Two folders: Textures holds material with a detected pitch and FieldRecordings holds
    environments. Each clip travels as "Folder/name.wav" so the pack reference and the
    note-following decision both know which it is; a field recording is never transposed to the
    played note. The tonal shelf is the pitched folder filtered by measurement; the bed shelf is
    everything else, with the environments weighted by the style's appetite for them."""
    slug = re.sub(r"[^a-z0-9]+", "_", style_name.lower()).strip("_")[:20]
    tonality()
    skip = set(rejected) | {os.path.basename(s) for s in _TONALITY.get("silent", set())}

    def listing(d, folder):
        if not d or not os.path.isdir(d):
            return []
        return sorted(folder + "/" + os.path.basename(f) for f in glob.glob(os.path.join(d, "*.wav"))
                      if os.path.basename(f) not in skip)

    tex = listing(dirname, "Textures")
    fld = listing(fielddir, "FieldRecordings")
    own = [f for f in tex + fld if os.path.basename(f).startswith(slug + "_")]
    if own:
        tonal = [f for f in own if is_tonal_clip(f)]
        return ClipPools(tonal, [f for f in own if f not in set(tonal)], own)
    tonal = [f for f in tex if is_tonal_clip(f)]
    rest = [f for f in tex if f not in set(tonal)]
    beds = rest + fld * max(1, int(round(fieldshare * 4))) if fld else rest
    everything = tex + fld * max(1, int(round(fieldshare * 4))) if fld else tex
    # Most of the library comes from prompt lists rather than per-style generation, so the file
    # name no longer says which style a clip belongs to. Tools/library/clip_affinity.py answers
    # that by listening: CLAP scores every clip against every style's own sentences. The style's
    # clips are weighted heavily and the whole shelf is left in behind them, because a library
    # where every style only ever hears its own material is a library of forty islands.
    close = affinity_for(style_name)
    if close:
        def weighted(pool):
            have = set(pool)
            picked = [c for c in close if c in have]
            return (picked * 3 + pool) if picked else pool
        tonal, beds, everything = weighted(tonal), weighted(beds), weighted(everything)
    return ClipPools(tonal, beds, everything)


_AFFINITY = {}


def affinity_for(style_name):
    """The clips a model put closest to this style, or nothing if the file was never written."""
    if not _AFFINITY:
        path = os.path.join(ROOT, "Library", "affinity.json")
        _AFFINITY["_"] = {}
        if os.path.exists(path):
            try:
                blob = json.load(open(path, encoding="utf-8"))
                for nm, folders in blob.get("byStyle", {}).items():
                    _AFFINITY["_"][nm] = [c for lst in folders.values() for c in lst]
            except (ValueError, OSError):
                pass
    return _AFFINITY["_"].get(style_name, [])


# The impulse families the styles still name were retired when the room generator (Tools/ImpulseGen/roomgen.py)
# replaced make_impulses.py; each maps to the new families that do its job.
RETIRED_IMPULSE_FAMILIES = {
    "room_hall": ("hall",), "room_chamber": ("chamber",), "room_bunker": ("chamber",), "room_cathedral": ("cathedral",),
    "room_cavern": ("cavern",), "room_plate": ("plate",), "diffusion": ("hall", "chamber"), "spectral": ("drift",),
    "reverse": ("swell",), "shimmer": ("vast", "bloom"), "tuned": ("cathedral", "vast"), "modal": ("plate", "chamber"),
    "comb": ("plate", "far"), "scatter": ("plate", "far"), "struck": ("chamber", "far"),
}
_LEGACY_IMPULSES = None


def legacy_impulses():
    """The impulses earlier packs named: kept on the shelf for old sessions, never taken by a new preset."""
    global _LEGACY_IMPULSES
    if _LEGACY_IMPULSES is None:
        path = os.path.join(ROOT, "Tools", "library", "legacy_impulses.json")
        _LEGACY_IMPULSES = set()
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                _LEGACY_IMPULSES = set(json.load(f).get("files", {}))
    return _LEGACY_IMPULSES


def impulse_pool(dirname, style):
    """Impulses whose name starts with one of the style's families, the retired family names translated to
    the new ones, and never one of the legacy files."""
    legacy = legacy_impulses()
    files = sorted(os.path.basename(f) for f in glob.glob(os.path.join(dirname, "*.wav"))
                   if os.path.basename(f) not in legacy)
    prefixes = []
    for pre in style["impulses"]:
        prefixes.extend(RETIRED_IMPULSE_FAMILIES.get(pre.rstrip("_"), (pre,)))
    own = [f for f in files if any(f.startswith(pre.rstrip("_") + "_") for pre in prefixes)]
    return own or files


def check_style_tables():
    """Every recipe a style names has to exist. Four styles asked for "Harmonic drawbars", which
    is not what the recipe is called ("Organ drawbars"), and the slug match simply never fired --
    a dead name costs nothing, says nothing and is invisible, which is why it sat there for the
    whole life of the library."""
    sys.path.insert(0, os.path.join(ROOT, "Tools", "WavetableGen"))
    try:
        from wavetablegen_core import RECIPES
    except ImportError:
        return []                      # no numpy here: the generator still works, just unchecked
    bad = []
    for st in STYLES:
        for t in st.get("tables", []):
            if t not in RECIPES:
                bad.append((st["name"], t))
    return bad


def wavetable_pool(dirname, style):
    """The style's own recipes first, and the rest of the shelf behind them.

    A style names two or three of the nineteen wavetable recipes, and with the pool limited to
    those, 384 of the 608 tables in the library were never once loaded -- the shelf was full and
    the generator kept reaching for the same corner of it. The style's recipes are weighted three
    to one, so a pack still sounds like itself while every table can turn up somewhere."""
    slugs = [re.sub(r"[^a-z0-9]+", "_", t.lower()).strip("_") for t in style["tables"]]
    # Paths below the folder, shelf included ("Harmonic/harmonic_organ_003.wav"): the pack names a
    # table as ../Wavetables/ and this.
    files = shelf_tables(dirname)
    own = [f for f in files if any(posixpath.basename(f).startswith(s + "_") for s in slugs)]
    # A table sliced out of a clip is named "clip_<the clip's name>", so it inherits that clip's
    # place in the library: if a model put the recording of a bowed cymbal near this style, the
    # table made from it belongs there too. Nineteen recipes cannot tell a style apart; a thousand
    # recorded sounds can.
    # The table's name is "clip_" + the same slug rule WavetableGen uses, so the two are compared
    # on their first forty characters rather than on a guess about the suffix.
    close = {re.sub(r"[^A-Za-z0-9]+", "_", os.path.splitext(os.path.basename(c))[0]).strip("_")[:40]
             for c in affinity_for(style["name"])}
    if close:
        own = own + [f for f in files
                     if posixpath.basename(f).startswith("clip_") and posixpath.basename(f)[5:45] in close]
    return (own * 3 + files) if own else files


_NUMBER = re.compile(r"^[-+]?[0-9.]+$")
ROMAN = ["", " II", " III", " IV", " V", " VI", " VII", " VIII", " IX", " X"]


def name_for(style, rng, used):
    firsts, seconds = style["words"]
    for _ in range(60):
        n = f"{firsts[rng.randrange(len(firsts))]} {seconds[rng.randrange(len(seconds))]}"
        if n not in used:
            used.add(n)
            return n
    base = f"{firsts[rng.randrange(len(firsts))]} {seconds[rng.randrange(len(seconds))]}"
    for r in ROMAN[1:] + [f" {i}" for i in range(2, 400)]:
        if base + r not in used:
            used.add(base + r)
            return base + r
    return base


# ---------------------------------------------------------------- main

def main():
    global PARAMS
    bad = check_style_tables()
    if bad:
        for name, t in bad:
            print(f"style {name}: no wavetable recipe called '{t}'")
        return 1
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--per-style", type=int, default=200)
    ap.add_argument("--styles", default="", help="comma-separated style names; default is all of them")
    ap.add_argument("--seed", type=int, default=23)
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "Library", "Packs"))
    ap.add_argument("--textures", default=os.path.join(ROOT, "Library", "Textures"))
    ap.add_argument("--fields", default=os.path.join(ROOT, "Library", "FieldRecordings"))
    ap.add_argument("--wavetables", default=os.path.join(ROOT, "Library", "Wavetables"))
    ap.add_argument("--impulses", default=os.path.join(ROOT, "Library", "Impulses"))
    a = ap.parse_args()
    PARAMS = param_table()
    os.makedirs(a.out_dir, exist_ok=True)

    rejects = rejected_clips(a.textures) | rejected_clips(a.fields)
    if rejects:
        print(f"{len(rejects)} clips skipped (see {os.path.join(a.textures, 'rejected.txt')})")
    packs = []
    taken = set()          # map cells already used, across the whole library
    # And the cells the library already occupies. The nudge below only knows about the presets
    # this run has written, so writing ONE pack into a side folder put 109 of its 200 presets on
    # a dot another pack was already sitting on -- invisible in the browser, and nothing said so
    # until verify_packs counted them.
    for existing in sorted(glob.glob(os.path.join(ROOT, "Library", "Packs", "*.ambientpack"))
                           + glob.glob(os.path.join(a.out_dir, "*.ambientpack"))):
        with io.open(existing, encoding="utf-8") as fh:
            for line in fh:
                if line.startswith("#") or "|" not in line:
                    continue
                cols = line.split("|")
                if len(cols) < 3:
                    continue
                bits = cols[2].split()
                if len(bits) >= 2:
                    try:
                        taken.add((round(float(bits[0]), 3), round(float(bits[1]), 3)))
                    except ValueError:
                        pass
    # Names stay unique across the whole synth, so "--preset <name>" and the browser search
    # always mean one preset -- the built-in ones included.
    # Pointed at the library, so the names already in it count as taken. Without this the render
    # tool only reports the built-ins, and a new pack happily reuses a name an existing pack has
    # -- which it did: forty-eight collisions with the older packs, all of them plausible pairs
    # like "Iron Reach" that two different word pools can both produce.
    # Names already taken: the packs in the output folder AND the library proper, when the output
    # is a folder of its own. Writing a new pack into a side folder and checking names only
    # against that folder produced 59 presets that collided with the library the moment the pack
    # was moved in -- "Ice Descent" twice, once in Permafrost and once in Field Recordings.
    library = os.path.join(ROOT, "Library", "Packs")
    folders = a.out_dir if os.path.normcase(os.path.abspath(a.out_dir)) == os.path.normcase(os.path.abspath(library)) \
              else a.out_dir + ";" + library
    env = dict(os.environ, AMBIENT_PACKS=folders)
    used_names = {n.strip() for n in subprocess.run([RENDER, "--list-presets"], capture_output=True,
                                                    text=True, encoding="utf-8", env=env).stdout.splitlines() if n.strip()}
    wanted = [w.strip().lower() for w in a.styles.split(",") if w.strip()]
    for si, st in enumerate(STYLES):
        # The seed is keyed on the style's index, not on how many are being written, so asking
        # for one pack gives exactly the pack that a full run would have given.
        if wanted and st["name"].lower() not in wanted:
            continue
        rng = random.Random(a.seed * 104729 + si)
        textures = texture_pool(a.textures, st["name"], rejects, a.fields,
                                st["modules"].get("environment", 0.5))
        tables = wavetable_pool(a.wavetables, st)
        impulses = impulse_pool(a.impulses, st)
        rows = []
        for k in range(a.per_style):
            p, tex, tab, imp, impb, matrix, envs = make_preset(st, rng, textures, tables, impulses,
                                                        SHADES[k % len(SHADES)],
                                                        random.Random(a.seed * 15485863 + si * 7919 + k))
            rows.append({"name": name_for(st, rng, used_names), "params": p, "shade": SHADES[k % len(SHADES)][0],
                         "settings": guide.apply_to_settings(settings_string(p))[0],   # the production guide's windows (guide.py)
                         # one path, or up to four ';'-separated (one per slot): each non-empty
                         # part gets the folder, an empty part stays empty and means "no clip"
                         # The clip already carries its folder ("Textures/x.wav"), so the
                         # reference is one level up from the pack and then that.
                         "texture": ";".join(f"../{part}" if part else "" for part in tex.split(";")) if tex else "",
                         "wavetable": f"../Wavetables/{tab}" if tab else "",
                         "impulse": f"../Impulses/{imp}" if imp else "",
                         "impulse_b": f"../Impulses/{impb}" if impb else "",
                         "mod": matrix, "envs": envs,
                         "desc": descriptors(p)})
        packs.append((st, rows))

    # Rank the descriptors across the whole library, then lay the map out over all of it.
    all_rows = [r for _, rows in packs for r in rows]
    desc = np.array([r["desc"] for r in all_rows], dtype=np.float64)
    for c in range(desc.shape[1]):
        desc[:, c] = rank(desc[:, c])
    xy = layout(desc)
    for i, r in enumerate(all_rows):
        r["desc"] = desc[i]
        r["xy"] = xy[i]
        r["tags"] = tag_bits(r["params"], desc[i])

    total = 0
    for st, rows in packs:
        path = os.path.join(a.out_dir, re.sub(r"[^A-Za-z0-9]+", "-", st["name"]) + ".ambientpack")
        with open(path, "w", encoding="utf-8") as f:
            f.write(f"# {st['name']} -- {len(rows)} presets for Noctuary, generated by Tools/library/make_presets.py\n")
            f.write(f"# In the spirit of {st['inspiration']}. Not affiliated with, sampled from or endorsed by anyone.\n")
            f.write("# Format: name|settings|x y bright motion width noisy bass density tags|"
                    "texture|wavetable|impulse|mod matrix|env shapes|impulse B\n")
            f.write(f"pack {st['name']}\n")
            for r in rows:
                d = r["desc"]
                # Two presets on the same spot of the map are two dots the browser draws on top of
                # each other, and one of them can never be clicked. Nudged apart on a deterministic
                # spiral, so the same seed still gives the same library.
                x, y = r["xy"][0], r["xy"][1]
                step = 0
                while (round(x, 3), round(y, 3)) in taken and step < 64:
                    step += 1
                    ang = 2.39996 * step                     # the golden angle: no two land alike
                    x = min(1.0, max(0.0, r["xy"][0] + 0.004 * step * math.cos(ang)))
                    y = min(1.0, max(0.0, r["xy"][1] + 0.004 * step * math.sin(ang)))
                taken.add((round(x, 3), round(y, 3)))
                meta = " ".join(f"{v:.3f}" for v in (x, y, d[0], d[1], d[2], d[3], d[4], d[5]))
                f.write(f"{r['name']}|{r['settings']}|{meta} {r['tags']}|{r['texture']}|"
                        f"{r['wavetable']}|{r['impulse']}|{r['mod']}|{r['envs']}|{r['impulse_b']}\n")
        total += len(rows)
        print(f"{len(rows):5d}  {path}")
    print(f"{total} presets in {len(packs)} packs -> {a.out_dir}")


if __name__ == "__main__":
    main()
