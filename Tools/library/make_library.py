"""Noctuary 2.0's preset library: fifty-six packs of 256 presets and the presets compiled into the
instrument, generated.

The styles are in artists.py -- one per artist, sixteen families for the built-ins -- and the material
is what the sample generators wrote down about every clip, table and impulse (clip_catalog.py): the
artists a clip was made for, its gesture, harmony and world, the source types it suits, the family and
note range of a table, the family and partner of a room. The machinery around it -- shades,
modulation, the instrument's optional blocks, descriptors, the layout -- is make_presets.py's, which
generated the library before this one.

What is new against make_presets:

  * a style's clips are the clips made for its artist and for the artists it borrows from, leaned by
    category, world and gesture, and never one the catalogue keeps out (an offset, a hole, a seam);
  * the source types come from the style's own weights for the voice and for the layers, and the three
    that read a clip take only clips written for them (Texture, Stretch, Spectral);
  * Harmonic and Wavetable are the two table types they have been since the classic wavetable
    arrived: a table from the Harmonic shelf plays as Harmonic, one from Classic as Wavetable, an
    ambient one as either; a table without a fundamental never carries the first slot; a sung vowel
    keeps the conductor inside the range it was sung in; a table measured from a recording gets a
    floor under its fundamental;
  * the key follows the material: a voice on a clip in F# Dorian plays in F#, on the scale that holds
    the mode when the style has that scale, and a clip that follows the note is played at the octave
    nearest the one it was recorded in, never far from its own pitch;
  * the Memory, the Cloud's feedback, resonators and swarm, detuned copies of a slot, the curved clip
    reader, and later sources that swell in on contours of their own;
  * rooms by family from the generated impulses, with a designed pair's partner as Room B;
  * the built-ins: sixteen families without a single file, written as packs to a staging folder that
    Tools/library/write_builtins.py turns into Core/src/Presets.cpp once they are balanced and measured.

    python Tools/library/make_library.py                    the 56 packs into Library/Packs
    python Tools/library/make_library.py --builtins         and the built-ins into build/builtin_packs
    python Tools/library/make_library.py --styles "Sleep Concert,Deep Earth" --per-style 24 --out-dir DIR

Deterministic: the same seed gives the same library.
"""
import argparse
import bisect
import collections
import math
import os
import random
import re
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import make_presets as mp  # noqa: E402
import clip_catalog as cc  # noqa: E402
import conductor as cond  # noqa: E402
from artists import ARTISTS, BUILTINS  # noqa: E402

u, logu = mp.u, mp.logu
ROOT_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
CLIP_TYPES = ("Texture", "Stretch", "Spectral")
ALWAYS_ON_NOTE = ("Additive", "Harmonic", "Wavetable", "FM", "Bow")
LEGACY = {"level": "osc_level", "partials": "partials", "tilt": "tilt", "bright": "brightness",
          "odd_even": "odd_even", "inharmonic": "inharmonic", "shimmer": "shimmer", "shimmer_rate": "shimmer_rate"}
# A clip's harmony, as its prompt gave it -> the scale that holds it and the steps from the clip's note to
# that scale's root. F# Dorian is E major's notes, so it is JI Major on E; A minor pentatonic is C's.
HARMONY = {
    "major": ("JI Major (Ptolemy)", 0), "maj7": ("JI Major (Ptolemy)", 0), "add9": ("JI Major (Ptolemy)", 0),
    "sus2": ("JI Major (Ptolemy)", 0), "justmaj": ("JI Major (Ptolemy)", 0),
    "major pentatonic": ("JI Pentatonic", 0), "minor pentatonic": ("JI Pentatonic", 3),
    "Lydian": ("JI Major (Ptolemy)", -5), "Mixolydian": ("JI Major (Ptolemy)", -7),
    "Dorian": ("JI Major (Ptolemy)", -2), "Phrygian": ("JI Major (Ptolemy)", -4),
    "Aeolian": ("JI Minor", 0), "minor": ("JI Minor", 0), "minor9": ("JI Minor", 0),
    "harmonic": ("Harmonic 8-16", 0), "slendro scale": ("Slendro (JI)", 0),
}
# Tables whose fundamental is weak by nature: measured from a recording, sung overtones, bowed or blown.
ROOT_FLOOR_FAMILIES = {"ambient:sampled", "ambient:overtone", "ambient:bowed", "ambient:tube", "harmonic:resonator"}
BUILTIN_TABLES = ("Classic", "Organ", "Vocal", "Glass", "Metal")


class Weighted:
    """A weighted shelf: pick() draws in proportion to the weights, with one random number."""

    def __init__(self, entries=()):
        self.items, self.cum, self.total = [], [], 0.0
        for item, w in entries:
            if w > 0.0:
                self.total += w
                self.items.append(item)
                self.cum.append(self.total)

    def __bool__(self):
        return bool(self.items)

    def __len__(self):
        return len(self.items)

    def pick(self, rng):
        i = bisect.bisect_right(self.cum, rng.random() * self.total)
        return self.items[min(i, len(self.items) - 1)]


# ---------------------------------------------------------------- the material

class Material:
    """The clips one style draws from, weighted, on four shelves: `tonal` (a grain slot that plays the
    note), `spectral` (clips written for the Spectral type), and the beds for Stretch, textures and field
    recordings apart so the style's appetite for environments decides between them."""

    def __init__(self, style, clips):
        mat = style["material"]
        own = {cc.key(t) for t in mat["targets"]}
        kin = {cc.key(k): w for k, w in mat["kin"].items()}
        cats, worlds, gestures = mat["cats"], mat["worlds"], mat["gestures"]
        avoid = set(mat["avoid"])
        usable = [c for c in clips if not c["unusable"] and c["category"] not in avoid]
        self.has_own = any(own & set(c["targets"]) for c in usable)
        self.field = style["field"]
        self.fr = float(mat["fr"])
        tonal, spectral, beds_t, beds_f, every = [], [], [], [], []
        for c in usable:
            t = set(c["targets"])
            w = (3.0 if own & t else 0.0) + sum(wk for k, wk in kin.items() if k in t)
            if c["category"] in cats:
                # A style no clip was written for is seeded by its categories; one that has its own
                # clips only leans on them.
                w = (w + (0.0 if self.has_own else 0.6)) * (1.0 + cats[c["category"]])
            elif cats:
                w *= 0.5 if self.has_own else 0.25
            if w <= 0.0:
                continue
            w *= 1.0 + worlds.get(c["world"], 0.0)
            if c["gesture"]:
                w *= 1.0 + gestures.get(c["gesture"], 0.0)
            m = c["measure"] or {}
            eng = set(c["engines"])
            tonal_clip = (c["kind"] == "texture" and c["midi"] is not None and m.get("harm", 1.0) >= 0.6
                          and m.get("centroid", 0.0) <= 3000.0)
            if tonal_clip and "Texture" in eng:
                tonal.append((c, w))
            if "Spectral" in eng:
                spectral.append((c, w))
            if "Stretch" in eng:
                (beds_f if c["kind"] == "field" else beds_t).append((c, w))
            every.append((c, w))
        self.tonal, self.spectral = Weighted(tonal), Weighted(spectral)
        self.beds_t, self.beds_f, self.every = Weighted(beds_t), Weighted(beds_f), Weighted(every)

    def grain(self, rng):
        pool = self.every if self.field else self.tonal
        return pool.pick(rng) if pool else None

    def spectral_pick(self, rng, lead):
        if not self.spectral:
            return None
        clip = self.spectral.pick(rng)
        # The voice rebuilt from a recording wants something with a pitch in it, where the style has any.
        tries = 0
        while lead and not self.field and clip["midi"] is None and tries < 6:
            clip = self.spectral.pick(rng)
            tries += 1
        return clip

    def bed(self, rng):
        first, second = (self.beds_f, self.beds_t) if rng.random() < self.fr else (self.beds_t, self.beds_f)
        pool = first or second
        return pool.pick(rng) if pool else None


class Tables:
    """The style's tables. One file per preset: the first slot that takes a user table decides it, and a
    later slot of the other type takes it only if the table was made for that type too."""

    def __init__(self, style, tables, clip_by_id):
        weights = style["wavetables"]
        self.builtin = Weighted([(f[8:], w) for f, w in weights.items() if f.startswith("builtin:")]) \
            or Weighted([(n, 1.0) for n in BUILTIN_TABLES])
        self.builtin_weight = sum(w for f, w in weights.items() if f.startswith("builtin:")) or 0.3
        self.pools = {}
        self.user_weight = {}
        if style["builtin"]:
            return
        count = collections.Counter(t["family"] for t in tables)
        cats = style["material"]["cats"]
        own = {cc.key(x) for x in style["material"]["targets"]}
        entries = []
        for t in tables:
            w = float(weights.get(t["family"], 0.0))
            if w <= 0.0:
                continue
            w /= count[t["family"]]
            if t["category"]:
                w *= 1.0 + cats.get(t["category"], 0.0)
                src = clip_by_id.get(t["source_id"])
                if src and own & set(src["targets"]):
                    w *= 2.0
            entries.append((t, w))
        for kind in ("Harmonic", "Wavetable"):
            for lead in (True, False):
                cand = [(t, w) for t, w in entries if kind in t["types"] and (t["grounded"] or not lead)]
                self.pools[(kind, lead)] = Weighted(cand)
                self.user_weight[(kind, lead)] = sum(w for _, w in cand)

    def user(self, rng, kind, lead):
        pool = self.pools.get((kind, lead))
        if not pool:
            return None
        share = self.user_weight[(kind, lead)] / (self.user_weight[(kind, lead)] + self.builtin_weight)
        return pool.pick(rng) if rng.random() < share else None


def name_for(style, rng, used):
    """A name from the style's two word pools -- but never "Horn Horns": a name whose halves share
    a stem reads as a mistake, and both pools legitimately hold the same word."""
    name = mp.name_for(style, rng, used)
    for _ in range(3):
        w = name.split()
        if len(w) < 2 or w[0][:4].lower() != w[1][:4].lower():
            return name
        name = mp.name_for(style, rng, used)
    return name


def room_pool(style, impulses):
    weights = style["rooms"]
    count = collections.Counter(i["family"] for i in impulses)
    names = [(i["name"], weights[i["family"]] / count[i["family"]]) for i in impulses if weights.get(i["family"], 0.0) > 0.0]
    return Weighted(names), [n for n, _ in names]


# ---------------------------------------------------------------- the key and the register

def key_lock(p, style, clip):
    """The key of the clip the voice plays: its note's pitch class, and its mode's scale when the style
    lists that scale (a style's tuning is its character, so it is never replaced by one it does not use)."""
    pc = clip["midi"] % 12
    spec = style["params"].get("scale")
    choices = spec if isinstance(spec, list) else []
    mapped = HARMONY.get(clip["harmony"])
    if mapped and mapped[0] in choices:
        p["scale"] = mapped[0]
        p["root"] = ROOT_NAMES[(pc + mapped[1]) % 12]
    else:
        p["root"] = ROOT_NAMES[pc]


def fit_octave(p, n, clip):
    """The octave that keeps a note-following clip near the pitch it was recorded at: the conductor's
    register is the style's, the clip's is the clip's, and a transposition of two octaves is an artefact."""
    if p.get("brain_on") == "off":
        centre = 60.0
    else:
        centre = 0.5 * (float(p.get("brain_low", 40)) + float(p.get("brain_high", 72)))
    k = int(round((clip["midi"] - centre) / 12.0))
    p[f"src{n}_octave"] = max(-2, min(2, k))


# ---------------------------------------------------------------- what the instrument grew

def new_features(p, mod, extra):
    want = lambda k: extra.random() < mod.get(k, 0.0)
    # The Memory: a long drifting memory of the foreground beside the Cosmos.
    if want("memory"):
        p["mem_send"] = round(u(extra, 0.12, 0.45), 3)
        p["mem_return"] = round(u(extra, 0.3, 0.75), 3)
        p["mem_to_far"] = round(u(extra, 0.2, 0.6), 3)
        p["mem_lines"] = extra.choice(["2", "4", "4", "8"])
        p["mem_size"] = round(logu(extra, 3.0, 30.0), 2)
        p["mem_blur"] = round(u(extra, 0.1, 0.7), 3)
        p["mem_drift"] = round(u(extra, 0.1, 0.6), 3)
        p["mem_hold"] = round(u(extra, 0.55, 0.95), 3)
        p["mem_age"] = round(u(extra, 0.1, 0.7), 3)
        if extra.random() < 0.3:
            p["mem_renew"] = round(u(extra, 0.1, 0.5), 3)
        if extra.random() < (0.5 if mod.get("patina", 0.0) >= 0.5 or mod.get("feedback", 0.0) >= 0.5 else 0.2):
            p["mem_drive"] = round(u(extra, 0.1, 0.5), 3)
        if extra.random() < 0.35:
            p["mem_recall"] = round(u(extra, 0.2, 0.7), 3)
            p["mem_seek"] = round(u(extra, 0.3, 0.9), 3)
            p["mem_grain"] = round(logu(extra, 80.0, 500.0), 1)
        r = extra.random()
        if r < 0.07:
            p["mem_reverse"] = "on"
        elif r < 0.14:
            p["mem_half"] = "on"
    # The Cloud grown up: grains of grains, flocks, and resonators on the scale.
    if float(p.get("cloud_send", 0) or 0) > 0.05 and want("cloudplus"):
        for part in extra.sample(["feedback", "scatter", "swarm", "resonance", "transpose"], k=extra.randint(2, 3)):
            if part == "feedback":
                p["cloud_feedback"] = round(u(extra, 0.15, 0.7), 3)
                p["cloud_tone"] = round(logu(extra, 1200.0, 9000.0), 0)
                if extra.random() < 0.35:
                    p["cloud_shift"] = extra.choice(["+12", "+7", "+5", "-5", "-7", "-12"])
            elif part == "scatter":
                p["cloud_scatter"] = round(u(extra, 0.15, 0.6), 3)
            elif part == "swarm":
                p["cloud_swarm"] = round(u(extra, 0.2, 0.8), 3)
            elif part == "resonance":
                p["cloud_resonance"] = round(u(extra, 0.2, 0.65), 3)
                p["cloud_res_mode"] = extra.choice(["Band", "Band", "Comb"])
                p["cloud_res_notes"] = extra.choice(["Scale", "Chord", "Chord", "Fifths", "Octaves"])
                p["cloud_res_decay"] = round(logu(extra, 0.8, 10.0), 2)
            else:
                p["cloud_transpose"] = extra.choice([12, -12, 7, -5, 19])
    # Detuned copies of a pitched slot, spread across the field.
    for n in (1, 2, 3, 4):
        t = p.get(f"src{n}_type", "Additive" if n == 1 else "Off")
        if t in ("Harmonic", "Wavetable", "FM") and want("unison"):
            p[f"src{n}_unison"] = extra.randint(2, 4)
            p[f"src{n}_uni_detune"] = round(logu(extra, 4.0, 25.0), 2)
            p[f"src{n}_uni_width"] = round(u(extra, 0.3, 0.9), 3)
        # A clip transposed to the note read on a curve: no metallic fold above its pitch.
        if t == "Texture" and p.get(f"src{n}_follow") == "Note" and want("hermite"):
            p[f"src{n}_interp"] = "Hermite"
    if float(p.get("cosmos_shimmer", 0) or 0) > 0.05 and want("grainshimmer"):
        p["cosmos_shimmer_mode"] = "Grain"
    # The chord judged as a whole rather than against its root alone. It used to ride on Timbre --
    # half the presets that heard consonance through their own spectrum also got it -- which left
    # it off in nine presets out of ten. Measured, it halves the roughness of a chord.
    if p.get("brain_on", "on") != "off" and "brain_harmonic" not in p and want("chordharm"):
        p["brain_harmonic"] = round(u(extra, 0.35, 0.85), 3)
    # Two conductors on periods that never line up: the background's clock is a crooked multiple of
    # the foreground's, so a state the two of them share comes round after hours rather than after
    # a few minutes. (Drawn on its own, the two rates landed within a factor of two often enough
    # that the planes were heard as one.)
    if p.get("brain2_on") == "on" and "brain_rate" in p and p.get("brain2_golden") != "on":
        p["brain2_rate"] = round(min(300.0, float(p["brain_rate"]) * extra.choice([2.3, 3.7, 5.3, 7.1])), 2)
    if float(p.get("ens_mix", 0) or 0) > 0.05 and p.get("ens_mode") is None and extra.random() < 0.08:
        p["ens_mode"] = "Velvet"


def entrances(p, envs_text, extra, mod):
    """make_presets.stagger_entries with the style's appetite for contours: a style of swells lets more of
    its sources wait and more of them enter on a shape of their own -- and may let the voice itself
    swell in, the way a bowed or EBowed note does."""
    own = mod.get("ownenv", 0.25)
    sounding = [n for n in (1, 2, 3, 4) if p.get(f"src{n}_type", "Additive" if n == 1 else "Off") not in ("Off", "")]
    shapes = ((envs_text.split("~") if envs_text else []) + [""] * 10)[:10]
    if sounding and own >= 0.5 and extra.random() < own * 0.5:
        n = sounding[0]
        text, mode = mp.entrance_shape(extra)
        if mode in ("One Shot", "Sustain Loop"):
            shapes[5 + n] = text
            p[f"src{n}_env"] = "Own"
            p[f"src{n}_env_mode"] = mode
            p[f"src{n}_env_time"] = round(logu(extra, 0.8, 3.0), 3)
    for n in sounding[1:]:
        if extra.random() >= 0.42 + 0.3 * own:
            continue
        p[f"src{n}_delay"] = round(logu(extra, 2.5, 25.0), 2)
        if extra.random() < 0.25 + 0.5 * own:
            text, mode = mp.entrance_shape(extra)
            shapes[5 + n] = text
            p[f"src{n}_env"] = "Own"
            p[f"src{n}_env_mode"] = mode
            p[f"src{n}_env_time"] = round(logu(extra, 0.6, 3.0), 3)
            if extra.random() < 0.2:
                p[f"src{n}_env_depth"] = round(u(extra, 0.5, 0.9), 3)
        else:
            p[f"src{n}_rise"] = round(logu(extra, 1.5, 12.0), 2)
    return "~".join(shapes).rstrip("~")


# ---------------------------------------------------------------- one preset

def make_preset(style, rng, extra, material, tables, rooms, shade, astir=False):
    p = {}
    for key, spec in style["params"].items():
        if key in mp.PARAMS:
            p[key] = mp.draw(rng, spec)
    mod = mp.apply_shade(p, style["modules"], shade)
    # What this pack PLAYS comes from Rene's own table of ranges rather than from the style: the
    # style says how a preset sounds, the table says what the conductor does with it. It is applied
    # here, before anything reads the register -- fitting a clip's octave, clamping a wavetable to
    # the notes it was recorded over -- so those all see the register this preset will really use.
    spec_holds = cond.draw(style["inspiration"] if not style["builtin"] else style["name"],
                           extra, astir, mp.PARAMS)
    p.update(spec_holds)
    builtin, field = style["builtin"], style["field"]
    # An artist's pack plays itself. The keys module switches the conductor off and leaves a preset
    # that is silent until someone holds a note down -- which is a fine thing to have, and which is
    # what the built-in family Played Keys is for. In a pack that stands for how one artist's music
    # moves it is not: one preset in ten made the pack look broken to anyone browsing it. The draw
    # is still made, so the random stream is where it always was and nothing else in the preset
    # moves; only the branch is never taken.
    if not builtin:
        mod["keys"] = 0.0
    floor = {"src2": 0.5, "src3": 0.25, "src4": 0.1} if style["sparse"] else {"src2": 0.85, "src3": 0.60, "src4": 0.35}
    for k, f in floor.items():
        mod[k] = max(mod.get(k, 0.0), f)
    if style["sparse"]:
        for k in ("src2", "src3", "src4"):
            mod[k] = min(mod[k], style["modules"].get(k, mod[k]))
    on = lambda k: rng.random() < mod.get(k, 0.0)
    # Air is filtered noise on the note, and it is by a wide margin the instrument's loudest source
    # of noise. Measured on a pure sine through the Harmonic source with everything else off, the
    # spectral flatness goes from 0.000001 to 0.0149 at air 0.2 -- and 0.012 is exactly where the
    # generated library sat. Every other block, switched on one at a time, measured 0.000001 to
    # 0.00003: the far reverb, the near reverb, the Cloud, the Cosmos, the delay, Breath. On real
    # presets, air=0 drops the flatness by four to twenty-eight times.
    #
    # The parameter's own default is 0.15, so a preset that never mentions air still plays with it:
    # to be without it, it has to say zero. That is why every one of the first 14336 presets carried
    # a noise band. It is the exception now rather than the rule, and it stays under a tenth unless
    # the style is about air -- which its own range says, by reaching past 0.3.
    air_hi = style["params"].get("air", (0.0, 0.1))
    air_hi = float(air_hi[-1]) if isinstance(air_hi, (list, tuple)) else 0.1
    airy = air_hi >= 0.3
    if extra.random() < (0.45 if airy else 0.15):
        p["air"] = round(u(extra, 0.04, 0.22 if airy else 0.10), 3)
        if extra.random() < 0.15:
            p["air_mode"] = "Ghost"
    else:
        p["air"] = 0.0
    slot_clips = {}
    state = {"table": None}

    # Foundation, stack, played rather than conducted -- as make_presets had them.
    if not on("sub"):
        p["sub_level"] = 0.0
    elif p.get("sub_level", 0.0) < 0.1:
        p["sub_level"] = u(rng, 0.15, 0.4)
    if p.get("sub_level", 0.0) > 0.05:
        p.setdefault("sub_source", "Lowest")
        p.setdefault("sub_glide", logu(rng, 2.0, 20.0))
        if rng.random() < 0.4:
            p.setdefault("sub_binaural", u(rng, 1.5, 7.0))
    if on("stack"):
        p["stack"] = mp.STACKS[rng.randrange(len(mp.STACKS))]
        p["strands"] = rng.randint(3, 6)
    if on("keys"):
        p["brain_on"] = "off"
        p["attack"] = logu(rng, 0.5, 6.0)
        p["release"] = logu(rng, 4.0, 20.0)
        p["keys_depth"] = u(rng, 0.0, 0.4)

    def fill_slot(n, force=None):
        pre = f"src{n}_"
        key_of = lambda k: (LEGACY[k] if (n == 1 and k in LEGACY) else pre + k)
        put = lambda k, v: p.__setitem__(key_of(k), v)
        got = lambda k, d=None: p.get(key_of(k), d)
        lead = n == 1
        weights = dict(style["types"]["lead" if lead else "layer"])
        if builtin or material is None:
            for t in CLIP_TYPES:
                weights.pop(t, None)
        else:
            if not (material.tonal or (field and material.every)):
                weights.pop("Texture", None)
            if not material.spectral:
                weights.pop("Spectral", None)
            if not (material.beds_t or material.beds_f):
                weights.pop("Stretch", None)
        # A style whose sound world really is noise -- tape hiss, a bunker, a signal that decayed --
        # leads with it more often; for the rest noise stays a colour under the tone.
        if lead and "Noise" in weights:
            weights["Noise"] *= 1.0 + 2.0 * mod.get("noiseprimary", 0.0)
        kind = force or (Weighted(weights.items()).pick(rng) if weights else "Additive")
        put("type", kind)
        put("level", u(rng, 0.6, 1.0) if lead else u(rng, 0.15, 0.55))
        if kind != "Noise" and rng.random() < 0.4:
            put("drift", logu(rng, 0.8, 10.0))
        if kind in CLIP_TYPES:
            put("octave", 0)
            put("ratio", "1/1")
        else:
            put("octave", rng.choice([-2, -1, 0, 0, 0, 1]))
            put("ratio", mp.SLOT_RATIOS[rng.randrange(len(mp.SLOT_RATIOS))])
        put("pan", u(rng, -0.8, 0.8))
        if kind in ("Harmonic", "Wavetable"):
            chosen = state["table"]
            if chosen is not None and kind in chosen["types"] and (chosen["grounded"] or not lead):
                put("table", "User")
            elif chosen is None and (t := tables.user(extra, kind, lead)) is not None:
                state["table"] = t
                put("table", "User")
            else:
                put("table", tables.builtin.pick(extra))
            put("pos", u(rng, 0.0, 1.0))
            put("pos_drift", u(rng, 0.05, 0.7))
            if (kind == "Harmonic" and got("table") == "User" and state["table"]["family"] in ROOT_FLOOR_FAMILIES
                    and extra.random() < mod.get("rootfloor", 0.0)):
                put("root", "on")
        elif kind == "Noise":
            put("noise", style["noise"][rng.randrange(len(style["noise"]))])
            put("noise_q", u(rng, 0.25, 0.85))
            put("pos", u(rng, 0.05, 0.9))
            put("pos_drift", u(rng, 0.05, 0.8))
            put("level", u(rng, 0.07, 0.28))
            if got("noise") == "Crackle":
                put("density", logu(rng, 1.5, 30.0))
            put("follow", "Note" if (got("noise") in ("Band", "Wind") and rng.random() < 0.4) else "Free")
        elif kind == "FM":
            put("fm_ratio", rng.choice([0.5, 1.0, 1.5, 2.0, 2.0, 3.0, 4.0, 5.0, 7.0]))
            put("fm_index", logu(rng, 0.3, 3.5))
        elif kind == "Additive":
            if not lead:
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
            heavy = extra.random() < 0.5
            put("bow_force", u(extra, 0.45, 0.9) if heavy else u(extra, 0.1, 0.45))
            put("bow_speed", u(extra, 0.12, 0.4) if heavy else u(extra, 0.35, 0.85))
            put("pos", u(extra, 0.1, 0.8))
            put("bright", u(extra, 0.35, 0.9))
            put("level", u(extra, 0.55, 1.0) if lead else u(extra, 0.2, 0.5))
            if extra.random() < 0.6:
                put("drift", logu(extra, 1.0, 7.0))
        elif kind == "Spectral":
            clip = material.spectral_pick(extra, lead)
            r = extra.random()
            put("spec_rate", 0.0 if r < 0.22 else (u(extra, 0.05, 0.6) if r < 0.75 else u(extra, 0.6, 2.5)))
            # Breath is this type's tonal-to-noisy control, and it is the only source setting that
            # measures as noise at all: on a pure sine, +0.8 gives a spectral flatness of 0.0027
            # against 0.000001 at -0.8, with every other source parameter -- grains from 60 to 400
            # ms, a stretch of 300, a frozen read head, shimmer, inharmonicity, purity drift --
            # sitting at 0.000007 or below. Drawn evenly it put half of the spectral voices into the
            # noisy half. It leans tonal now, and only a style that is about air goes past zero.
            put("spec_breath", round(u(extra, -0.8, 0.45 if airy else 0.05), 3))
            put("pos", u(extra, 0.0, 1.0))
            put("pos_drift", u(extra, 0.05, 0.5))
            put("bright", u(extra, 0.3, 0.85))
            put("level", u(extra, 0.6, 1.0) if lead else u(extra, 0.25, 0.6))
            slot_clips[n] = clip
            put("follow", "Note" if clip["midi"] is not None and extra.random() < 0.85 else "Free")
            if extra.random() < 0.5:
                put("drift", logu(extra, 0.5, 4.0))
        elif kind == "Stretch":
            clip = material.bed(rng)
            put("grain", logu(rng, 150.0, 340.0))
            put("stretch", logu(rng, 6.0, 300.0))
            put("xfade", u(rng, 0.05, 0.25))
            put("pos", u(rng, 0.0, 1.0))
            put("pos_drift", u(rng, 0.1, 0.6))
            put("level", u(rng, 0.6, 1.0) if lead else u(rng, 0.3, 0.7))
            if rng.random() < 0.5:
                put("drift", logu(rng, 0.5, 4.0))
            slot_clips[n] = clip
            put("follow", "Note" if (clip["midi"] is not None and rng.random() < 0.4) else "Free")
            if got("follow") == "Free" and rng.random() < 0.15:
                put("octave", -1)                     # a bed at half speed, an octave down
        else:                                          # Texture
            clip = material.grain(rng)
            grain_ms = logu(rng, 60.0, 800.0)
            overlap = logu(rng, 4.0, 30.0)
            density = min(200.0, max(1.0, overlap * 1000.0 / grain_ms))
            put("grain", grain_ms)
            put("density", density)
            gran = style["granular"]
            put("grains", int(min(128, max(4, math.ceil(density * grain_ms / 1000.0 * 1.8 * gran["grains"]) + 4))))
            put("spread", math.exp(u(rng, math.log(gran["spread"][0]), math.log(gran["spread"][1]))))
            put("level", u(rng, 0.45, 1.0) if lead else u(rng, 0.12, 0.42))
            slot_clips[n] = clip
            if clip["kind"] == "field" and grain_ms < 150.0:
                put("grain", logu(rng, 150.0, 340.0))
            put("follow", "Note" if (clip["midi"] is not None and rng.random() < 0.8) else "Free")

    fill_slot(1)
    if p.get("src1_type", "Additive") != "Additive":
        p.setdefault("strands", 1)          # the strand bank belongs to the additive type alone
    if on("src2"):
        fill_slot(2)
    if on("src3"):
        fill_slot(3)
    if on("src4"):
        fill_slot(4)

    def slot_type(n):
        return p.get("src1_type", "Additive") if n == 1 else p.get(f"src{n}_type", "Off")

    # The tonal anchor: unless the style is an environment, something sits on the note.
    if not field:
        on_note = lambda n: (slot_type(n) in ALWAYS_ON_NOTE
                             or (slot_type(n) in CLIP_TYPES and p.get(f"src{n}_follow", "Free") == "Note"))
        if not any(on_note(n) for n in range(1, 5)):
            pitched = next((n for n in range(1, 5) if slot_type(n) in CLIP_TYPES and n in slot_clips
                            and slot_clips[n]["midi"] is not None), None)
            free = next((n for n in range(2, 5) if slot_type(n) == "Off"), None)
            if pitched is not None:
                p[f"src{pitched}_follow"] = "Note"
            elif free is not None:
                fill_slot(free, force="Harmonic")
            else:
                p["src1_type"] = "Additive"

    # The key and the register follow the material.
    lead_clip = next((slot_clips[n] for n in range(1, 5) if n in slot_clips and slot_clips[n]["midi"] is not None
                      and p.get(f"src{n}_follow") == "Note"), None)
    if lead_clip is not None and extra.random() < mod.get("keylock", 0.0):
        key_lock(p, style, lead_clip)
    for n, clip in slot_clips.items():
        if clip["midi"] is not None and p.get(f"src{n}_follow") == "Note":
            fit_octave(p, n, clip)
    table = state["table"]
    if table is not None and table["note_range"] and p.get("src1_table") == "User":
        lo, hi = table["note_range"]
        blo = max(int(p.get("brain_low", lo)), lo)
        bhi = min(int(p.get("brain_high", hi)), hi)
        if bhi - blo < 10:
            blo, bhi = lo, hi
        p["brain_low"], p["brain_high"] = blo, bhi
    mp.apply_shade_granular(p, shade)

    # Z-plane, with the style's own ringing objects when it has them.
    if on("zplane"):
        modal = style["zshapes"] and rng.random() < 0.45
        if modal or (rng.random() < 0.2 and mp.Z_MODAL_SHAPES):
            shapes = [z for z in style["zshapes"] if z in mp.Z_SHAPES] or mp.Z_MODAL_SHAPES
            p["z_mode"] = "Modal"
            p["z_shape"] = shapes[rng.randrange(len(shapes))]
            p["z_decay"] = logu(rng, 0.4, 18.0)
            p["z_damp"] = u(rng, 0.3, 0.95)
        else:
            p["z_mode"] = "Replace" if rng.random() < 0.25 else "Series"
            p["z_shape"] = mp.Z_SHAPES[rng.randrange(len(mp.Z_SHAPES))]
        p["z_z"] = u(rng, 0.0, 0.7)
        p["z_x"] = u(rng, 0.1, 0.9)
        p["z_y"] = u(rng, 0.1, 0.9)
        p["z_rate"] = p.get("z_rate", logu(rng, 0.006, 0.25))
        p["z_depth"] = u(rng, 0.2, 0.9)
        p["z_res"] = u(rng, 0.2, 0.7)
        p["z_keytrack"] = u(rng, 0.0, 0.6) if rng.random() < 0.4 else 0.0
        p["z_mix"] = u(rng, 0.35, 0.8) if p["z_mode"] == "Replace" else u(rng, 0.3, 0.9)
    if p.get("dly_mix", 0.0) and rng.random() < 0.35:
        p["dly_duck"] = u(rng, 0.25, 0.8)

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
            p["cosmos_shimmer_pitch"] = mp.SHIMMER_PITCH[rng.randrange(len(mp.SHIMMER_PITCH))]
        if rng.random() < 0.35:
            p["cosmos_vowel"] = u(rng, 0.2, 0.7)
            p["cosmos_vowel_rate"] = logu(rng, 0.008, 0.2)
    if on("cloud"):
        p["cloud_send"] = u(rng, 0.15, 0.6)
        p["cloud_density"] = logu(rng, 3.0, 40.0)
        p["cloud_size"] = logu(rng, 60.0, 600.0)
        p["cloud_pitch"] = u(rng, 0.0, 0.7)
        p["cloud_spray"] = logu(rng, 0.15, 1.8)
        p["cloud_level"] = u(rng, 0.4, 0.85)
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
        for k in ("fb_bus", "fb_fm", "fb_drive", "fb_tape", "fb_tone"):
            p.pop(k, None)
    if on("delay2"):
        p["dly2_mix"] = u(rng, 0.1, 0.35)
        p["dly2_time_l"] = logu(rng, 0.5, 3.5)
        p["dly2_time_r"] = logu(rng, 0.7, 4.0)
        p["dly2_feedback"] = u(rng, 0.3, 0.75)
        p["dly2_cross"] = u(rng, 0.2, 0.9)
        p["dly2_damp"] = u(rng, 0.4, 0.9)
    impulse = ""
    if not builtin and rooms is not None and rooms[0] and on("room"):
        p["room_level"] = u(rng, 0.15, 0.6)
        p["room_source"] = "Far" if rng.random() < 0.6 else "Near"
        p["room_predelay"] = logu(rng, 5.0, 150.0)
        p["room_highcut"] = logu(rng, 1500.0, 9000.0)
        impulse = rooms[0].pick(rng)
    else:
        p.pop("room_level", None)
    if on("coherence"):
        p["coherence"] = u(rng, 0.2, 0.8)
        p["coherence_depth"] = u(rng, 0.2, 0.8)
        p["coherence_rate"] = u(rng, 0.3, 3.0)
    if on("portamento"):
        p["portamento"] = logu(rng, 0.5, 12.0)
        p["porta_gravity"] = u(rng, 0.2, 0.9)
    # (Ghost is decided with the air itself, above. It used to be drawn here -- and it RAISED air to
    # 0.2 wherever it fell, which put a noise band into one preset in eight that had chosen less.)
    if rng.random() < 0.25 and "purity_drift" not in p:
        p["purity_drift"] = u(rng, 0.1, 0.5)
        p["purity_rate"] = logu(rng, 0.003, 0.05)
    if on("phase"):
        p["phase_width"] = u(rng, 0.2, 0.8)
        p["phase_rate"] = logu(rng, 0.006, 0.08)
    if float(p.get("breath", 0.0)) > 0.05 and rng.random() < 0.4:
        p["doppler"] = u(rng, 0.2, 0.8)
    if on("blur"):
        p["blur_mix"] = u(rng, 0.2, 0.6)
        p["blur_smear"] = u(rng, 0.3, 0.9)
    if on("filtermodel"):
        models = style["filters"] or mp.FILTER_MODELS
        model = models[rng.randrange(len(models))]
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
    if on("strike") or (p.get("brain_on") == "off" and rng.random() < 0.5):
        p["strike_level"] = u(rng, 0.2, 0.6)
        p["strike_type"] = style["strikes"][rng.randrange(len(style["strikes"]))]
        p["strike_decay"] = logu(rng, 0.1, 1.8)
        p["strike_damp"] = u(rng, 0.15, 0.8)
        if rng.random() < 0.3:
            p["strike_who"] = "Keys + Brain"
    if on("absorb"):
        p["dly_absorb"] = u(rng, 0.3, 1.0)
        if "dly2_mix" in p:
            p["dly2_absorb"] = u(rng, 0.3, 1.0)
    if on("tide"):
        p["tide"] = logu(rng, 2.0, 12.0)
        p["tide_period"] = logu(rng, 4.0, 30.0)
    if on("rotate"):
        p["far_rotate"] = u(rng, 0.2, 0.8)
    if on("narrow") and p.get("far_level", 0.8) > 0.3:
        deep = p.get("depth", 0.5)
        p["far_width"] = round(u(rng, 0.35, 0.85) * (1.0 - 0.25 * deep) + 0.1, 3)
    if on("haas"):
        p["haas"] = round(u(rng, 0.15, 0.5), 3)
        p["haas_time"] = round(u(rng, 10.0, 22.0), 1)
    if on("microshift") and p.get("ens_mix", 0.0) > 0.05:
        p["ens_mode"] = "Microshift"
        p["ens_depth"] = round(u(rng, 0.3, 1.0), 3)
        p["ens_rate"] = round(logu(rng, 0.02, 0.09), 4)
    if on("fold"):
        p["filter_fold"] = round(u(rng, 0.08, 0.45), 3)
    mp.open_up(p, mod, extra, rng)
    new_features(p, mod, extra)
    if extra.random() < mod.get("banks", 0.0):
        for bank, gate in ((mp.COSMOS_BANK, float(p.get("cosmos_send", 0) or 0) > 0.05),
                           (mp.STRIKE_BANK, float(p.get("strike_level", 0) or 0) > 0.02),
                           (mp.Z_BANK, p.get("z_mode", "Off") in ("Series", "Replace"))):
            if not (bank and gate and extra.random() < 0.6):
                continue
            _, settings = bank[extra.randrange(len(bank))]
            for kv in settings.split(";"):
                if "=" not in kv:
                    continue
                k, v = kv.split("=", 1)
                k = k.strip()
                if k in mp.PARAMS and k not in ("cosmos_send", "cosmos_return", "strike_level", "z_mix"):
                    p[k] = v.strip()
    # The ranges again, and this time last. Applying them once at the top is not enough: the style
    # pipeline writes twenty-six of the same keys further down -- the second conductor's whole
    # block, spacing, key, harmonic, even, smooth, timbre, blend, quantize and the rest -- so half
    # the table was being overwritten by the module that happened to run afterwards. Counted in the
    # first generated library: every one of the 14336 presets carried the new parameters, and 26 of
    # the old ones had been taken back.
    #
    # Four keys are NOT restored, because down there the MATERIAL knows better and that was a rule
    # of this library before it was a rule of the conductor: the register may be narrowed to the
    # notes a wavetable was actually recorded over, the scale may be locked to the key of the clip
    # that sings, and a preset made to be played from a keyboard has its conductor switched off.
    for key, value in spec_holds.items():
        if key not in ("brain_low", "brain_high", "scale", "brain_on"):
            p[key] = value
    p["seed"] = rng.randrange(1, 9999)
    if "brain_low" in p and "brain_high" in p and int(p["brain_high"]) <= int(p["brain_low"]) + 6:
        p["brain_high"] = int(p["brain_low"]) + 12
    # A hold whose two ends are close together is a fixed hold, and in the brit family and at Paul
    # Bradley that is the point -- so this only widens a hold the ranges document did not set.
    if ("brain_hold_min" in p and "brain_hold_max" in p and "brain_hold_max" not in spec_holds
            and float(p["brain_hold_max"]) < float(p["brain_hold_min"]) * 1.5):
        p["brain_hold_max"] = float(p["brain_hold_min"]) * 2.0
    matrix, envs = mp.modulation_for(p, style, rng, shade[0])
    envs = entrances(p, envs, extra, mod)
    matrix = mp.add_hands(p, style, rng, matrix)
    texture = ""
    if slot_clips:
        rels = {n: c["rel"] for n, c in slot_clips.items()}
        texture = rels[min(rels)] if len(set(rels.values())) == 1 else ";".join(rels.get(n, "") for n in range(1, 5))
    impulse_b = ""
    if impulse and "room_morph" in p:
        impulse_b = mp.morph_partner(impulse, rooms[1], extra)
    if not impulse_b:
        p.pop("room_morph", None)
    wavetable = state["table"]["rel"] if state["table"] is not None and any(
        p.get(f"src{n}_table") == "User" for n in range(1, 5)) else ""
    return p, texture, wavetable, impulse, impulse_b, matrix, envs


# ---------------------------------------------------------------- the library

def write_pack(path, style, rows, builtin):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        if builtin:
            f.write(f"# Built-in family '{style['name']}' -- {len(rows)} presets, generated by Tools/library/make_library.py.\n")
            f.write("# Staging only: Tools/library/write_builtins.py compiles these into Core/src/Presets.cpp.\n")
        else:
            f.write(f"# {style['name']} -- {len(rows)} presets for Noctuary 2.0, generated by Tools/library/make_library.py\n")
            f.write(f"# In the spirit of {style['inspiration']}. Not affiliated with, sampled from or endorsed by anyone.\n")
        f.write("# Format: name|settings|x y bright motion width noisy bass density tags|"
                "texture|wavetable|impulse|mod matrix|env shapes|impulse B\n")
        f.write(f"pack {style['name']}\n")
        f.write("format 2\n")
        for r in rows:
            d = r["desc"]
            meta = " ".join(f"{v:.3f}" for v in (r["xy"][0], r["xy"][1], d[0], d[1], d[2], d[3], d[4], d[5]))
            f.write(f"{r['name']}|{r['settings']}|{meta} {r['tags']}|{r['texture']}|{r['wavetable']}|"
                    f"{r['impulse']}|{r['mod']}|{r['envs']}|{r['impulse_b']}\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--per-style", type=int, default=256)
    ap.add_argument("--styles", default="", help="comma-separated pack names; default all 56")
    ap.add_argument("--seed", type=int, default=2026)
    ap.add_argument("--out-dir", default=os.path.join(ROOT, "Library", "Packs"))
    ap.add_argument("--builtins", action="store_true", help="also the built-ins, into --builtins-out")
    ap.add_argument("--builtins-only", action="store_true")
    ap.add_argument("--builtins-out", default=os.path.join(ROOT, "build", "builtin_packs"))
    ap.add_argument("--builtin-count", type=int, default=16, help="presets per built-in family (the first has one less: Init)")
    a = ap.parse_args()
    mp.PARAMS = mp.param_table()
    os.makedirs(a.out_dir, exist_ok=True)

    # Every name is unique across the synth. The renderer still carries the old built-ins, and every
    # measurement until write_builtins has replaced them asks it for a preset by name.
    empty = tempfile.mkdtemp(prefix="nopacks_")
    used = {n.strip() for n in subprocess.run([mp.RENDER, "--list-presets"], capture_output=True, text=True,
                                               encoding="utf-8", env=dict(os.environ, AMBIENT_PACKS=empty)).stdout.splitlines()
            if n.strip()}
    wanted = [w.strip().lower() for w in a.styles.split(",") if w.strip()]
    writing = {st["name"] for st in ARTISTS if not wanted or st["name"].lower() in wanted}
    if a.builtins or a.builtins_only:
        writing |= {st["name"] for st in BUILTINS}
    # What the packs this run does not rewrite already hold: their names, so no two presets in the
    # synth share one, and their map cells, so no preset hides underneath another.
    cells = set()
    for folder in (a.out_dir, a.builtins_out):
        for fname in (sorted(os.listdir(folder)) if os.path.isdir(folder) else []):
            if not fname.endswith(".ambientpack"):
                continue
            pack_name, rows = None, []
            for line in open(os.path.join(folder, fname), encoding="utf-8", errors="replace"):
                if line.startswith("pack "):
                    pack_name = line[5:].strip()
                elif "|" in line and not line.startswith("#"):
                    rows.append(line.split("|"))
            if pack_name in writing:
                continue
            used.update(r[0].strip() for r in rows)
            for r in rows:
                bits = r[2].split() if len(r) > 2 else []
                if len(bits) >= 2:
                    try:
                        cells.add((round(float(bits[0]), 3), round(float(bits[1]), 3)))
                    except ValueError:
                        pass

    clips = cc.clips()
    tables = cc.tables()
    impulses = cc.impulses()
    clip_by_id = {c["id"]: c for c in clips if c["kind"] == "texture"}
    jobs = []
    if not a.builtins_only:
        jobs += [(i, st, a.per_style, a.out_dir) for i, st in enumerate(ARTISTS) if st["name"] in writing]
    if a.builtins or a.builtins_only:
        os.makedirs(a.builtins_out, exist_ok=True)
        jobs += [(1000 + i, st, a.builtin_count - (1 if i == 0 else 0), a.builtins_out) for i, st in enumerate(BUILTINS)]

    packs = []
    for si, st, count, out in jobs:
        rng = random.Random(a.seed * 104729 + si)
        material = None if st["builtin"] else Material(st, clips)
        tabs = Tables(st, tables, clip_by_id)
        rooms = None if st["builtin"] else room_pool(st, impulses)
        rows = []
        for k in range(count):
            shade = mp.SHADES[k % len(mp.SHADES)]
            extra = random.Random(a.seed * 15485863 + si * 7919 + k)
            # Half of every pack is the moving half (the ranges document's "Still und bewegt"):
            # faster events, more wander, a rate that breathes harder. A pack of one temperament
            # throughout is a pack in which every preset is the same piece.
            p, tex, tab, imp, impb, matrix, envs = make_preset(st, rng, extra, material, tabs, rooms, shade,
                                                               astir=(k % 2 == 1))
            # The name is drawn AFTER the preset, as it always was, so the streams stay where they
            # were; the foreground cues come after the name, from a stream seeded by it.
            name = name_for(st, rng, used)
            p.update(cond.depth_cues(st["inspiration"] if not st["builtin"] else st["name"], name, p))
            rows.append({"name": name, "params": p, "settings": mp.settings_string(p),
                         "texture": ";".join(f"../{part}" if part else "" for part in tex.split(";")) if tex else "",
                         "wavetable": f"../Wavetables/{tab}" if tab else "",
                         "impulse": f"../Impulses/{imp}" if imp else "",
                         "impulse_b": f"../Impulses/{impb}" if impb else "",
                         "mod": matrix, "envs": envs, "desc": mp.descriptors(p)})
        packs.append((st, rows, out))
        kinds = collections.Counter(r["params"].get("src1_type", "Additive") for r in rows)
        print(f"{len(rows):4d}  {st['name']:22s} voices: " + ", ".join(f"{k} {v}" for k, v in kinds.most_common()), flush=True)

    all_rows = [r for _, rows, _ in packs for r in rows]
    desc = np.array([r["desc"] for r in all_rows], dtype=np.float64)
    for c in range(desc.shape[1]):
        desc[:, c] = mp.rank(desc[:, c])
    xy = mp.layout(desc)
    # Two presets on one spot of the map are two dots drawn on top of each other, and the lower one
    # can never be clicked. Nudged apart on a deterministic golden-angle spiral, away from each
    # other and from the cells the packs this run leaves alone already hold.
    for i, r in enumerate(all_rows):
        x, y = float(xy[i, 0]), float(xy[i, 1])
        step = 0
        while (round(x, 3), round(y, 3)) in cells and step < 64:
            step += 1
            ang, rad = 2.39996 * step, 0.004 * math.sqrt(step)
            x = min(1.0, max(0.0, float(xy[i, 0]) + rad * math.cos(ang)))
            y = min(1.0, max(0.0, float(xy[i, 1]) + rad * math.sin(ang)))
        cells.add((round(x, 3), round(y, 3)))
        r["desc"], r["xy"], r["tags"] = desc[i], (x, y), mp.tag_bits(r["params"], desc[i])
    total = 0
    for st, rows, out in packs:
        path = os.path.join(out, re.sub(r"[^A-Za-z0-9]+", "-", st["name"]).strip("-") + ".ambientpack")
        write_pack(path, st, rows, st["builtin"])
        total += len(rows)
    print(f"{total} presets in {len(packs)} packs")
    return 0


if __name__ == "__main__":
    sys.exit(main())
