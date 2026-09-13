"""Noctuary -- generate the Cosmos and Strike preset banks (Core/src/*Presets.inc).

Both are layers: a bank of presets that touches only one section, so it lands on top of whatever
sound is loaded without disturbing it. The Cosmos bank is the feedback network; the Strike bank is
the Karplus-Strong pluck that fires on note-on.

    python Tools/make_layer_presets.py

Why generated. Two hundred and fifty-six presets written by hand are two hundred and fifty-six
chances to type 0.8 where 0.08 was meant, and the result of that is not a wrong note, it is a
preset that sounds broken and nobody can say why. Here each family is a small designed GRID --
two or three parameters, four or five values each, chosen so that every step is audible -- and
the arithmetic is done by a machine. What is not automated is which parameters a family varies
and over what range; that is the design, and it is written out one family at a time below.

Every generated preset is then rendered and measured by Tools/check_layer_presets.py, which is
what actually keeps a silent or a clipping one out of the bank.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))

COSMOS_OUT = os.path.join(ROOT, "Core", "src", "CosmosPresets.inc")
STRIKE_OUT = os.path.join(ROOT, "Core", "src", "StrikePresets.inc")


def fmt(**kw):
    """A settings string in the parameter table's own key order-independent form."""
    return ";".join("%s=%s" % (k, v if isinstance(v, str) else ("%g" % v)) for k, v in kw.items())


# ============================================================ Cosmos
# Sixteen families of sixteen. Each family names the two or three things that make it what it is
# and walks them over a grid; the rest of the section stays at whatever the family's base says.
COSMOS = []


def cfam(name, presets):
    assert len(presets) == 16, (name, len(presets))
    COSMOS.append((name, presets))


def grid(names, base, axes, fixed=None):
    """`axes` is a list of (key, [values]); the product must come to sixteen."""
    out, i = [], 0
    (k1, v1), (k2, v2) = axes
    for a in v1:
        for b in v2:
            s = dict(base)
            s[k1] = a
            s[k2] = b
            if fixed:
                s.update(fixed)
            out.append((names[i], fmt(**s)))
            i += 1
    return out


# ---- 1. Shift: the frequency shifter, which is what makes the network inharmonic
cfam("Shift", grid(
    ["Breath Shift", "Hair Shift", "Slow Slide", "Cold Slide",
     "Six Hertz", "Twelve", "Twenty Five", "Fifty",
     "Ninety", "Hundred Fifty", "Two Twenty", "Three Hundred",
     "Falling Six", "Falling Forty", "Falling Ninety", "Falling Two Twenty"],
    dict(cosmos_send=0.8, cosmos_shift_drift=0.4, cosmos_return=0.7, cosmos_to_far=0.5),
    [("cosmos_shift", [1.5, 6, 25, 90]), ("cosmos_shift_drift", [0.15, 0.45, 0.8, 1.0])]))

# ---- 2. Beating: shifts small enough that the ear hears interference, not pitch
cfam("Beating", grid(
    ["Half Hertz", "One Hertz", "Two Hertz", "Four Hertz",
     "Slow Wash", "Slow Roll", "Slow Turn", "Slow Fold",
     "Deep Beat", "Deep Sway", "Deep Pulse", "Deep Fold",
     "Near Still", "Barely Moving", "Almost Static", "Held Breath"],
    dict(cosmos_shift_drift=0.2, cosmos_smear=0.8, cosmos_to_far=0.6),
    [("cosmos_shift", [0.5, 1, 2, 4]), ("cosmos_send", [0.5, 0.7, 0.85, 1.0])],
    fixed=dict(cosmos_return=0.75)))

# ---- 3. Resonators: the tuned comb, at the ratios that mean something against the root
cfam("Resonators", grid(
    ["Root", "Root Deep", "Root Long", "Root Endless",
     "Fifth", "Fifth Deep", "Fifth Long", "Fifth Endless",
     "Octave", "Octave Deep", "Octave Long", "Octave Endless",
     "Twelfth", "Twelfth Deep", "Twelfth Long", "Twelfth Endless"],
    dict(cosmos_send=1.0, cosmos_res=0.7, cosmos_return=0.7, cosmos_to_far=0.4),
    [("cosmos_res_pitch", [1, 1.5, 2, 3]), ("cosmos_res_fb", [0.75, 0.86, 0.92, 0.96])]))

# ---- 4. Deep resonators: under the root, where the network becomes a room rather than a note
cfam("Deep", grid(
    ["Hull", "Hull Long", "Keel", "Keel Long",
     "Cellar", "Cellar Long", "Vault", "Vault Long",
     "Under", "Under Long", "Below", "Below Long",
     "Foundation", "Foundation Long", "Bedrock", "Bedrock Long"],
    dict(cosmos_send=1.0, cosmos_res=0.85, cosmos_return=0.8, cosmos_to_far=0.3),
    [("cosmos_res_pitch", [0.25, 0.375, 0.5, 0.75]), ("cosmos_res_fb", [0.82, 0.90, 0.94, 0.965])]))

# ---- 5. Vowels: the formant bank inside the loop, from a held vowel to a talking one
cfam("Vowels", grid(
    ["Held Vowel", "Slow Vowel", "Turning Vowel", "Talking",
     "Held Choir", "Slow Choir", "Turning Choir", "Choir Talking",
     "Held Throat", "Slow Throat", "Turning Throat", "Throat Talking",
     "Held Whisper", "Slow Whisper", "Turning Whisper", "Whisper Talking"],
    dict(cosmos_send=0.85, cosmos_return=0.8, cosmos_to_far=0.5, cosmos_smear=0.6),
    [("cosmos_vowel", [0.45, 0.65, 0.85, 1.0]), ("cosmos_vowel_rate", [0.008, 0.03, 0.09, 0.28])]))

# ---- 6. Nebula: diffusion, where individual repeats stop being repeats
cfam("Nebula", grid(
    ["Thin Cloud", "Thin Drift", "Thin Wash", "Thin Fog",
     "Cloud", "Drift", "Wash", "Fog",
     "Deep Cloud", "Deep Drift", "Deep Wash", "Deep Fog",
     "Total Cloud", "Total Drift", "Total Wash", "Total Fog"],
    dict(cosmos_send=0.9, cosmos_return=0.75, cosmos_to_far=0.7),
    [("cosmos_nebula", [0.3, 0.55, 0.8, 1.0]), ("cosmos_smear", [0.3, 0.55, 0.8, 1.0])]))

# ---- 7. Shimmer: pitch-shifted feedback, the octave-up bloom
cfam("Shimmer", grid(
    ["Shimmer Up", "Shimmer Up Deep", "Shimmer Up Full", "Shimmer Up Total",
     "Shimmer Fifth", "Shimmer Fifth Deep", "Shimmer Fifth Full", "Shimmer Fifth Total",
     "Shimmer Down", "Shimmer Down Deep", "Shimmer Down Full", "Shimmer Down Total",
     "Shimmer Two Up", "Shimmer Two Up Deep", "Shimmer Two Up Full", "Shimmer Two Up Total"],
    dict(cosmos_send=0.9, cosmos_return=0.75, cosmos_to_far=0.6, cosmos_smear=0.5),
    [("cosmos_shimmer_pitch", ["+12", "+7", "-12", "+24"]),
     ("cosmos_shimmer", [0.3, 0.5, 0.75, 1.0])]))

# ---- 8. Metallic: a big shift with the resonator behind it, which is what makes it ring like metal
cfam("Metallic", grid(
    ["Sheet", "Sheet Long", "Plate", "Plate Long",
     "Girder", "Girder Long", "Rail", "Rail Long",
     "Wire", "Wire Long", "Cable", "Cable Long",
     "Foil", "Foil Long", "Blade", "Blade Long"],
    dict(cosmos_send=1.0, cosmos_res=0.55, cosmos_return=0.7, cosmos_to_far=0.6,
         cosmos_shift_drift=0.25),
    [("cosmos_shift", [60, 110, 180, 260]), ("cosmos_res_fb", [0.80, 0.88, 0.93, 0.96])]))

# ---- 9. Glass: high resonators and the shimmer up, nothing below the middle
cfam("Glass", grid(
    ["Pane", "Pane Bright", "Pane Long", "Pane Endless",
     "Bowl", "Bowl Bright", "Bowl Long", "Bowl Endless",
     "Flute Glass", "Flute Bright", "Flute Long", "Flute Endless",
     "Ice Glass", "Ice Bright", "Ice Long", "Ice Endless"],
    dict(cosmos_send=0.9, cosmos_res=0.5, cosmos_return=0.65, cosmos_to_far=0.7,
         cosmos_shimmer=0.4, cosmos_shimmer_pitch="+12"),
    [("cosmos_res_pitch", [3, 4, 6, 8]), ("cosmos_res_fb", [0.78, 0.86, 0.92, 0.955])]))

# ---- 10. Drift: the shift wanders on its own curve, so nothing is ever quite where it was
cfam("Drift", grid(
    ["Tide", "Tide Wide", "Tide Deep", "Tide Total",
     "Current", "Current Wide", "Current Deep", "Current Total",
     "Weather", "Weather Wide", "Weather Deep", "Weather Total",
     "Season", "Season Wide", "Season Deep", "Season Total"],
    dict(cosmos_send=0.85, cosmos_shift_drift=1.0, cosmos_smear=0.7, cosmos_return=0.75),
    [("cosmos_shift", [4, 14, 40, 110]), ("cosmos_to_far", [0.3, 0.5, 0.75, 1.0])]))

# ---- 11. Wide: everything sent to the far plane, which is what puts the network behind the room
cfam("Wide", grid(
    ["Horizon", "Horizon Far", "Horizon Deep", "Horizon Total",
     "Distance", "Distance Far", "Distance Deep", "Distance Total",
     "Expanse", "Expanse Far", "Expanse Deep", "Expanse Total",
     "Beyond", "Beyond Far", "Beyond Deep", "Beyond Total"],
    dict(cosmos_send=0.8, cosmos_smear=0.85, cosmos_nebula=0.5, cosmos_shift=7,
         cosmos_shift_drift=0.6),
    [("cosmos_to_far", [0.55, 0.7, 0.85, 1.0]), ("cosmos_return", [0.25, 0.45, 0.65, 0.85])]))

# ---- 12. Ghost: barely there, which is where this network is at its best
cfam("Ghost", grid(
    ["Trace", "Trace Slow", "Trace Deep", "Trace Wide",
     "Rumour", "Rumour Slow", "Rumour Deep", "Rumour Wide",
     "Shadow", "Shadow Slow", "Shadow Deep", "Shadow Wide",
     "Whisper Back", "Whisper Slow", "Whisper Deep", "Whisper Wide"],
    dict(cosmos_shift=3, cosmos_shift_drift=0.7, cosmos_smear=0.9, cosmos_nebula=0.4),
    [("cosmos_send", [0.2, 0.32, 0.45, 0.6]), ("cosmos_return", [0.2, 0.35, 0.5, 0.65])],
    fixed=dict(cosmos_to_far=0.8)))

# ---- 13. Choir: the vowel bank and the resonator together, which is where voices come from
cfam("Choir", grid(
    ["Alien Choir", "Alien Deep", "Alien Long", "Alien Endless",
     "Far Choir", "Far Deep", "Far Long", "Far Endless",
     "Low Choir", "Low Deep", "Low Long", "Low Endless",
     "High Choir", "High Deep", "High Long", "High Endless"],
    dict(cosmos_send=0.9, cosmos_vowel=0.9, cosmos_vowel_rate=0.02, cosmos_res=0.5,
         cosmos_return=0.8, cosmos_to_far=0.55),
    [("cosmos_res_pitch", [1, 0.5, 2, 4]), ("cosmos_res_fb", [0.80, 0.88, 0.93, 0.96])]))

# ---- 14. Machine: shift, resonator and diffusion at once -- the network as an object, not a space
cfam("Machine", grid(
    ["Engine", "Engine Hot", "Engine Long", "Engine Total",
     "Turbine", "Turbine Hot", "Turbine Long", "Turbine Total",
     "Reactor", "Reactor Hot", "Reactor Long", "Reactor Total",
     "Generator", "Generator Hot", "Generator Long", "Generator Total"],
    dict(cosmos_send=1.0, cosmos_res=0.6, cosmos_nebula=0.55, cosmos_shift_drift=0.3,
         cosmos_return=0.7, cosmos_to_far=0.45),
    [("cosmos_shift", [18, 45, 95, 170]), ("cosmos_res_fb", [0.78, 0.86, 0.92, 0.955])]))

# ---- 15. Bloom: shimmer and nebula, the slow upward opening
cfam("Bloom", grid(
    ["Open", "Open Wide", "Open Deep", "Open Total",
     "Rise", "Rise Wide", "Rise Deep", "Rise Total",
     "Ascend", "Ascend Wide", "Ascend Deep", "Ascend Total",
     "Lift", "Lift Wide", "Lift Deep", "Lift Total"],
    dict(cosmos_send=0.9, cosmos_shimmer_pitch="+12", cosmos_smear=0.75,
         cosmos_return=0.7, cosmos_to_far=0.7),
    [("cosmos_shimmer", [0.35, 0.55, 0.75, 0.95]), ("cosmos_nebula", [0.2, 0.45, 0.7, 0.95])]))

# ---- 16. Edge: the top of the feedback range, where the network starts to sing on its own
cfam("Edge", grid(
    ["Held", "Held Bright", "Held Low", "Held Wide",
     "Singing", "Singing Bright", "Singing Low", "Singing Wide",
     "Ringing", "Ringing Bright", "Ringing Low", "Ringing Wide",
     "On The Edge", "Edge Bright", "Edge Low", "Edge Wide"],
    dict(cosmos_send=0.85, cosmos_res=0.9, cosmos_shift=2, cosmos_shift_drift=0.5,
         cosmos_smear=0.6, cosmos_to_far=0.5),
    [("cosmos_res_fb", [0.93, 0.95, 0.962, 0.97]), ("cosmos_res_pitch", [1, 2, 0.5, 3])],
    fixed=dict(cosmos_return=0.6)))


# ============================================================ Strike
# The Karplus-Strong pluck. Three excitations (a string, a dull wooden knock, a stretched metal
# string with an all-pass in its loop) and three controls, which is a small space -- so this bank
# is forty presets rather than two hundred, and every one of them is somewhere different in it.
STRIKE = [
    ("Strings", [
        ("Nylon", dict(strike_level=0.55, strike_type="String", strike_decay=0.35, strike_damp=0.70)),
        ("Steel", dict(strike_level=0.60, strike_type="String", strike_decay=0.60, strike_damp=0.35)),
        ("Harp", dict(strike_level=0.50, strike_type="String", strike_decay=1.10, strike_damp=0.45)),
        ("Long String", dict(strike_level=0.55, strike_type="String", strike_decay=2.20, strike_damp=0.30)),
        ("Muted", dict(strike_level=0.60, strike_type="String", strike_decay=0.12, strike_damp=0.80)),
        ("Damped Felt", dict(strike_level=0.45, strike_type="String", strike_decay=0.25, strike_damp=0.95)),
        ("Open String", dict(strike_level=0.65, strike_type="String", strike_decay=1.60, strike_damp=0.12)),
        ("Wire", dict(strike_level=0.70, strike_type="String", strike_decay=0.90, strike_damp=0.05)),
        ("Whisper Pluck", dict(strike_level=0.22, strike_type="String", strike_decay=0.80, strike_damp=0.55)),
        ("Cloud Pluck", dict(strike_level=0.35, strike_type="String", strike_decay=3.00, strike_damp=0.60)),
    ]),
    ("Wood", [
        ("Knock", dict(strike_level=0.55, strike_type="Wood", strike_decay=0.10, strike_damp=0.60)),
        ("Block", dict(strike_level=0.60, strike_type="Wood", strike_decay=0.20, strike_damp=0.45)),
        ("Marimba Hit", dict(strike_level=0.55, strike_type="Wood", strike_decay=0.45, strike_damp=0.35)),
        ("Log Drum", dict(strike_level=0.65, strike_type="Wood", strike_decay=0.80, strike_damp=0.30)),
        ("Dry Tap", dict(strike_level=0.40, strike_type="Wood", strike_decay=0.06, strike_damp=0.85)),
        ("Hollow Wood", dict(strike_level=0.60, strike_type="Wood", strike_decay=1.40, strike_damp=0.20)),
        ("Soft Mallet", dict(strike_level=0.35, strike_type="Wood", strike_decay=0.55, strike_damp=0.75)),
        ("Far Knock", dict(strike_level=0.20, strike_type="Wood", strike_decay=0.30, strike_damp=0.55)),
        ("Kalimba Tine", dict(strike_level=0.50, strike_type="Wood", strike_decay=0.90, strike_damp=0.15)),
        ("Bamboo", dict(strike_level=0.58, strike_type="Wood", strike_decay=0.14, strike_damp=0.72)),
    ]),
    ("Metal", [
        ("Bell Pluck", dict(strike_level=0.50, strike_type="Metal", strike_decay=1.20, strike_damp=0.35)),
        ("Clang", dict(strike_level=0.65, strike_type="Metal", strike_decay=0.60, strike_damp=0.20)),
        ("Gamelan", dict(strike_level=0.55, strike_type="Metal", strike_decay=2.00, strike_damp=0.40)),
        ("Anvil", dict(strike_level=0.70, strike_type="Metal", strike_decay=0.25, strike_damp=0.10)),
        ("Long Metal", dict(strike_level=0.50, strike_type="Metal", strike_decay=3.00, strike_damp=0.25)),
        ("Glass Tine", dict(strike_level=0.40, strike_type="Metal", strike_decay=1.60, strike_damp=0.55)),
        ("Struck Wire", dict(strike_level=0.60, strike_type="Metal", strike_decay=0.80, strike_damp=0.05)),
        ("Distant Bell", dict(strike_level=0.22, strike_type="Metal", strike_decay=2.40, strike_damp=0.45)),
        ("Sheet", dict(strike_level=0.62, strike_type="Metal", strike_decay=0.45, strike_damp=0.15)),
        ("Halo", dict(strike_level=0.32, strike_type="Metal", strike_decay=2.80, strike_damp=0.65)),
    ]),
    ("Conducted", [
        # The same excitations, but fired by the conductor as well as by the keys -- which turns
        # the pluck from something you play into something the piece does on its own.
        ("Rain On Strings", dict(strike_level=0.35, strike_type="String", strike_decay=1.20, strike_damp=0.55, strike_who="Keys + Brain")),
        ("Slow Harp", dict(strike_level=0.30, strike_type="String", strike_decay=2.60, strike_damp=0.40, strike_who="Keys + Brain")),
        ("Falling Wood", dict(strike_level=0.34, strike_type="Wood", strike_decay=0.50, strike_damp=0.55, strike_who="Keys + Brain")),
        ("Wind Chimes", dict(strike_level=0.28, strike_type="Metal", strike_decay=2.20, strike_damp=0.50, strike_who="Keys + Brain")),
        ("Temple", dict(strike_level=0.32, strike_type="Metal", strike_decay=3.00, strike_damp=0.30, strike_who="Keys + Brain")),
        ("Distant Work", dict(strike_level=0.20, strike_type="Wood", strike_decay=0.18, strike_damp=0.70, strike_who="Keys + Brain")),
        ("Ghost Pluck", dict(strike_level=0.16, strike_type="String", strike_decay=1.80, strike_damp=0.65, strike_who="Keys + Brain")),
        ("Gamelan Rain", dict(strike_level=0.30, strike_type="Metal", strike_decay=1.40, strike_damp=0.45, strike_who="Keys + Brain")),
        ("Loose Strings", dict(strike_level=0.40, strike_type="String", strike_decay=0.70, strike_damp=0.20, strike_who="Keys + Brain")),
        ("Slow Bells", dict(strike_level=0.26, strike_type="Metal", strike_decay=3.00, strike_damp=0.55, strike_who="Keys + Brain")),
    ]),
]


# ============================================================ Near
# The foreground (13.09.2026): the Near Source and the Near Events together, one layer. Each
# preset names a source and its shape, and how the events come -- a flute every few minutes, a
# bowl rubbed once in a while, drops for a minute, a radio voice, a Berlin-school line for four
# minutes out of every twelve. The families are the near types and the sequences.
NEAR_OUT = os.path.join(ROOT, "Core", "src", "NearPresets.inc")


def near(kind="Note", rate=180.0, length=8.0, attack=0.3, decay=1.0, sustain=1.0, release=3.0,
         cutoff=8000.0, resonance=0.1, filt="LP 12", fenv=0.0, pitch="Consonant", spread=0.5,
         approach=0.0, distance=0.0, dry=0.0, to_delay2=0.0, to_cosmos=0.0,
         proximity=0.6, hold="on", level=0.7, clip=None, **src):
    """One near preset's settings: the events, the envelope and the filter, then the source.
    `clip` names a file of the library's archive, relative to the library's root; it travels
    beside the settings as the preset's texture. `distance` is where the event sits between the
    planes (Approach arrives there; negative Approach leaves from there), `dry` the share of it
    that goes past every reverb and delay, and `to_delay2` / `to_cosmos` the two sends: a share of
    the event ADDED to what the second delay and the Cosmos already hear, so an event can answer
    itself across a minute, or be shifted and smeared into deep space, while the bed stays where
    it is (13.09.2026)."""
    d = dict(fore_level=level, fore_kind=kind, fore_rate=rate, fore_length=length, fore_pitch=pitch,
             fore_spread=spread, fore_approach=approach, fore_distance=distance, fore_dry=dry,
             fore_delay2=to_delay2, fore_cosmos=to_cosmos,
             fore_proximity=proximity, fore_hold=hold,
             fore_attack=attack, fore_decay=decay, fore_sustain=sustain, fore_release=release,
             fore_cutoff=cutoff, fore_resonance=resonance, fore_filter=filt, fore_filter_env=fenv)
    for k, v in src.items():
        d["fore_" + k] = v
    if clip:
        d["__clip__"] = clip
    return d


def archive(title, folder, clip, length, rate, **kw):
    """A near preset that plays one recording of the archive, once, straight: the Clip type on the
    near source, Pitch = Free, the event as long as the recording, and the conductor asked to
    begin nothing under it. `clip` is the file's path under Library/Archive."""
    kw.setdefault("attack", 0.05)
    kw.setdefault("release", 1.5)
    kw.setdefault("cutoff", 12000)
    kw.setdefault("proximity", 0.4)
    kw.setdefault("spread", 0.4)
    kw.setdefault("level", 0.55)
    kw.setdefault("approach", 0.0)
    return (title, near(kind="Note", rate=rate, length=length, pitch="Root", type="Clip", follow="Free", pos=0.0,
                        clip="Archive/" + folder + "/" + clip, **kw))


def seq(steps=7, step=0.42, mutation=0.12, scatter=0.3, bloom=0.6, length=240.0, rate=480.0, approach=0.25, **kw):
    kw.setdefault("hold", "off")
    kw.setdefault("attack", 0.005)
    kw.setdefault("decay", 0.35)
    kw.setdefault("sustain", 0.0)
    kw.setdefault("release", 0.35)
    kw.setdefault("fenv", 0.6)
    d = near(kind="Sequence", rate=rate, length=length, approach=approach, **kw)
    d.update(fore_steps=steps, fore_step=step, fore_mutation=mutation, fore_scatter=scatter, fore_bloom=bloom)
    return d


NEAR = [
    ("Flutes", [
        ("Bamboo Breath", near(type="Flute", force=0.55, speed=0.35, pos=0.12, bright=0.6, pos_drift=0.35, octave=0,
                               rate=150, length=9, attack=0.35, release=3.5, approach=0.15, level=0.6)),
        ("Airy Flute", near(type="Flute", force=0.45, speed=0.8, pos=0.1, bright=0.5, pos_drift=0.25,
                            rate=200, length=7, attack=0.5, release=4, level=0.55)),
        ("Overblown", near(type="Flute", force=0.75, speed=0.4, pos=0.95, bright=0.75, pos_drift=0.3, octave=-1,
                           rate=240, length=6, attack=0.25, release=3, level=0.55)),
        ("Flute Phrase", near(type="Flute", force=0.6, speed=0.4, pos=0.15, bright=0.6, pos_drift=0.4,
                              kind="Phrase", glide=5, rate=210, length=16, attack=0.4, release=4, approach=0.2, level=0.6)),
        ("Night Flute", near(type="Flute", force=0.5, speed=0.5, pos=0.1, bright=0.45, pos_drift=0.3, octave=-1,
                             rate=330, length=12, attack=0.8, release=6, approach=0.35, pitch="Highest", level=0.5)),
        ("Shakuhachi", near(type="Flute", force=0.65, speed=0.7, pos=0.2, bright=0.7, pos_drift=0.5,
                            kind="Phrase", glide=3, rate=180, length=11, attack=0.15, release=2.5, level=0.6)),
        # Two animals on the same pipe (13.09.): the owl's two hooted notes, low and breathy, and a
        # howl that glides up, holds and falls -- both far, both rare.
        ("Night Owl", near(type="Flute", force=0.4, speed=0.3, pos=0.3, bright=0.25, pos_drift=0.2, octave=-1,
                           kind="Phrase", glide=1.2, rate=240, length=4, attack=0.12, release=1.2, approach=0.3,
                           cutoff=3000, pitch="Lowest", spread=0.6, level=0.45)),
        ("Distant Howl", near(type="Flute", force=0.5, speed=0.45, pos=0.25, bright=0.35, pos_drift=0.3, octave=0,
                              kind="Phrase", glide=6, rate=420, length=9, attack=1.5, release=4, approach=0.7,
                              cutoff=4000, spread=0.8, level=0.4)),
    ]),
    ("Bowls", [
        ("Singing Bowl", near(type="Bowl", force=0.5, speed=0.4, pos=0.1, bright=0.6, pos_drift=0.4,
                              rate=240, length=22, attack=2, release=14, level=0.6)),
        ("Rim Rub", near(type="Bowl", force=0.65, speed=0.6, pos=0.0, bright=0.8, pos_drift=0.3,
                         rate=200, length=14, attack=1.5, release=10, level=0.6)),
        ("Deep Bowl", near(type="Bowl", force=0.45, speed=0.35, pos=0.3, bright=0.4, pos_drift=0.5, octave=-1,
                           rate=300, length=30, attack=3, release=20, pitch="Lowest", level=0.55)),
        ("Bowl Beating", near(type="Bowl", force=0.5, speed=0.45, pos=0.15, bright=0.6, pos_drift=0.9,
                              rate=260, length=25, attack=2.5, release=16, level=0.55)),
        ("Bowl Phrase", near(type="Bowl", force=0.55, speed=0.45, pos=0.1, bright=0.65, pos_drift=0.4,
                             kind="Phrase", glide=12, rate=280, length=36, attack=2, release=14, level=0.55)),
    ]),
    ("Water", [
        ("Cave Drops", near(type="Drops", density=1.2, pos=0.45, bright=0.5, pos_drift=0.6, follow="Free",
                            rate=200, length=40, attack=0.5, release=4, level=0.55)),
        ("Cistern", near(type="Drops", density=0.6, pos=0.05, bright=0.3, pos_drift=0.5, follow="Free",
                         rate=260, length=50, attack=0.5, release=5, approach=0.2, level=0.55)),
        ("Wet Marimba", near(type="Drops", density=2.5, pos=0.6, bright=0.6, pos_drift=0.3, follow="Note",
                             rate=220, length=30, attack=0.3, release=3, level=0.55)),
        ("Steady Drip", near(type="Drops", density=0.35, pos=0.4, bright=0.45, pos_drift=0.2, follow="Free",
                             rate=150, length=90, attack=0.5, release=4, level=0.5)),
        ("Rain In A Bowl", near(type="Drops", density=8, pos=0.7, bright=0.7, pos_drift=0.8, follow="Free",
                                rate=300, length=25, attack=1, release=6, approach=0.3, level=0.45)),
        # The sonar (Rene, 13.09.): one sine, an octave over the highest voice, struck in three
        # milliseconds, a core of a hundred and fifty, and a tail of ten seconds that the far
        # reverb of whatever sound is playing takes over. Nothing held: a ping is over before the
        # conductor could get in its way.
        ("Sonar Ping", near(type="Additive", partials=1, pitch="Highest", octave=2, rate=75, length=0.5,
                            attack=0.003, decay=0.15, sustain=0.12, release=10, cutoff=18000, resonance=0.0,
                            spread=0.15, proximity=0.3, hold="off", level=0.5, to_delay2=0.45)),
        ("Echo Sounder", near(type="Additive", partials=1, pitch="Highest", octave=2, rate=40, length=0.5,
                              attack=0.002, decay=0.08, sustain=0.05, release=5, cutoff=18000, resonance=0.0,
                              spread=0.1, proximity=0.2, hold="off", level=0.42, to_delay2=0.6)),
        ("Deep Sonar", near(type="Additive", partials=1, pitch="Highest", octave=1, rate=140, length=0.5,
                            attack=0.004, decay=0.3, sustain=0.2, release=14, cutoff=18000, resonance=0.0,
                            spread=0.25, proximity=0.5, hold="off", level=0.55)),
        # The foghorn (Rene, 13.09.): two horns a beat apart -- FM at a ratio of 1.045 puts the
        # sidebands three hertz off the carrier, which is the same beating -- under a ladder that
        # opens from 160 to 550 Hz as the horn fills and closes as it empties, deep in the far
        # field, a point on the horizon. Nothing held; the horn is not a soloist.
        ("Foghorn", near(type="FM", fm_ratio=1.045, fm_index=0.6, octave=-1, pitch="Root", rate=240, length=5,
                         attack=1.2, decay=1.0, sustain=0.8, release=1.8, cutoff=160, resonance=0.3, filt="Ladder", fenv=0.55,
                         spread=0.1, distance=0.7, approach=0.0, proximity=0.2, hold="off", level=0.8)),
    ]),
    # The signals (13.09., the second foreground round): the sounds of things, close and far.
    ("Signals", [
        ("Whistler", near(type="Whistler", bright=0.6, speed=0.3, pitch="Consonant", octave=1, kind="Note", rate=200, length=3,
                          attack=0.01, decay=0.5, sustain=0.6, release=1.5, cutoff=16000, resonance=0.0,
                          spread=0.3, distance=0.15, approach=-0.7, proximity=0.5, hold="off", level=0.45)),
        ("Seed Pod", near(type="Shaker", density=2.0, force=0.35, pos=0.55, noise_q=0.3, follow="Free", kind="Note", rate=150, length=3,
                          attack=0.005, decay=0.3, sustain=1.0, release=0.4, cutoff=16000, resonance=0.0,
                          spread=0.45, distance=0.05, dry=0.85, proximity=0.9, hold="off", level=0.5)),
        ("Geiger Counter", near(type="Geiger", density=0.6, force=0.4, pos=0.52, noise_q=0.27, kind="Note", rate=120, length=20,
                                attack=0.005, decay=0.1, sustain=1.0, release=0.1, cutoff=16000, resonance=0.0,
                                spread=0.7, distance=0.0, dry=1.0, proximity=0.3, hold="off", level=0.5)),
        ("Bunker Tube", near(type="Tube", pos=0.2, bright=0.5, pitch="Root", kind="Note", rate=300, length=25,
                             attack=0.01, decay=0.5, sustain=1.0, release=1.5, cutoff=12000, resonance=0.0,
                             spread=0.3, distance=0.35, dry=0.3, proximity=0.6, hold="off", level=0.45)),
        ("Krell Circuit", near(type="Krell", speed=0.35, pos=0.7, fm_ratio=1.5, fm_index=1.0, octave=0, kind="Note", rate=240, length=14,
                               attack=0.05, decay=0.5, sustain=1.0, release=0.8, cutoff=9000, resonance=0.1,
                               spread=0.8, distance=0.25, approach=0.0, proximity=0.4, hold="off", level=0.45, to_cosmos=0.5)),
        ("Deep Space Beacon", near(type="Beacon", density=0.5, octave=2, pitch="Root", kind="Note", rate=240, length=12,
                                   attack=0.005, decay=0.2, sustain=1.0, release=0.3, cutoff=16000, resonance=0.0,
                                   spread=0.2, distance=0.2, approach=0.35, proximity=0.5, hold="off", level=0.45, to_cosmos=0.55, to_delay2=0.3)),
        ("Number Station", near(type="Morse", speed=0.35, pos=0.2, bright=0.5, octave=2, pitch="Root", kind="Note", rate=360, length=30,
                                attack=0.01, decay=0.5, sustain=1.0, release=0.5, cutoff=6000, resonance=0.0,
                                spread=0.3, distance=0.3, dry=0.2, proximity=0.5, hold="on", level=0.4, to_delay2=0.35)),
        ("Shortwave Dial", near(type="Dial", pos=0.7, bright=0.5, pitch="Root", octave=1, kind="Note", rate=300, length=16,
                                attack=0.5, decay=1.0, sustain=1.0, release=2.0, cutoff=5000, resonance=0.0,
                                spread=0.4, distance=0.3, approach=0.2, proximity=0.4, hold="off", level=0.4)),
    ]),
    # Bronze and wood (13.09.): struck, and left to ring.
    ("Bells", [
        ("Ting-Sha", near(type="Chime", force=0.85, pos_drift=0.3, tilt=0.0, bright=0.35, pitch="Highest", octave=2, kind="Note",
                          rate=150, length=2, attack=0.001, decay=0.5, sustain=1.0, release=12, cutoff=18000, resonance=0.0,
                          spread=0.2, distance=0.1, dry=0.4, proximity=0.5, hold="off", level=0.5)),
        ("Ship's Bell", near(type="Chime", force=0.6, pos_drift=0.15, tilt=0.5, bright=0.2, pitch="Root", octave=1, kind="Note",
                             rate=240, length=2, attack=0.001, decay=0.5, sustain=1.0, release=8, cutoff=14000, resonance=0.0,
                             spread=0.3, distance=0.5, proximity=0.3, hold="off", level=0.5)),
        ("Church Bell Far", near(type="Chime", force=0.9, pos_drift=0.1, tilt=1.0, bright=0.15, pitch="Root", octave=0, kind="Note",
                                 rate=400, length=3, attack=0.002, decay=1.0, sustain=1.0, release=16, cutoff=6000, resonance=0.0,
                                 spread=0.1, distance=0.7, proximity=0.0, hold="off", level=0.7)),
        ("Wind Chimes", seq(type="Chime", force=0.5, pos_drift=0.25, tilt=0.0, bright=0.4, octave=2, cutoff=18000, resonance=0.0,
                            filt="LP 12", steps=9, step=0.5, mutation=0.2, scatter=0.7, bloom=0.0, attack=0.001, decay=0.3,
                            sustain=1.0, release=4, length=90, rate=400, approach=0.2, distance=0.2, level=0.4)),
    ]),
    # The radio (13.09.): phrases cut out of "Quiet, Please" (1947-49, Wyllis Cooper; the archive's
    # Radio folder, see Library/Archive/SOURCES.md), a folder rather than a file: every event plays
    # one of them, drawn at random, never the same twice running. Free, straight, from the start;
    # the conductor holds its onsets while the voice speaks and for ten seconds after.
    ("Radio", [
        ("Quiet, Please", near(kind="Note", rate=150, length=8, pitch="Root", type="Clip", follow="Free", pos=0.0,
                               attack=0.05, release=1.5, cutoff=12000, proximity=0.7, spread=0.3, approach=0.0,
                               level=0.5, clip="Archive/Radio/Quiet-Please/")),
        ("Night Announcer", near(kind="Note", rate=300, length=9, pitch="Root", type="Clip", follow="Free", pos=0.0,
                                 attack=0.1, release=2.0, cutoff=9000, proximity=0.4, spread=0.5, approach=0.25,
                                 level=0.45, clip="Archive/Radio/Quiet-Please/")),
        ("Whisper At The Ear", near(kind="Note", rate=90, length=8, pitch="Root", type="Clip", follow="Free", pos=0.0,
                                    attack=0.05, release=1.0, cutoff=6000, proximity=0.9, spread=0.1, approach=0.0,
                                    level=0.35, clip="Archive/Radio/Quiet-Please/")),
        ("Lost Transmission", near(kind="Note", rate=420, length=10, pitch="Root", type="Clip", follow="Free", pos=0.0,
                                   attack=0.3, release=3.0, cutoff=7000, proximity=0.2, spread=0.6, approach=0.6,
                                   level=0.4, to_delay2=0.5, to_cosmos=0.3, clip="Archive/Radio/Quiet-Please/")),
        # The shift register on the phrases: every step begins another one and the gate cuts it
        # off -- a cut-up of the broadcast, for a minute and a half out of every ten.
        ("Radio Cut-Up", seq(type="Clip", follow="Free", pos=0.0, cutoff=9000, resonance=0.1, filt="LP 12",
                             steps=5, step=0.6, mutation=0.1, scatter=0.4, bloom=0.2, attack=0.01, decay=0.3, sustain=0.3,
                             release=0.1, length=90, rate=600, approach=0.0, level=0.4, clip="Archive/Radio/Quiet-Please/")),
    ]),
    # The Library of Congress (13.09.): Citizen DJ's sample packs, 250 clips of each under
    # Archive/LoC/<collection>/ (Tools/library/fetch_loc_samples.py, the Library's own rights
    # statement in SOURCES.md), each a pool: a cylinder of 1905, the variety stage, a government
    # film's narrator, Tony Schwartz's New York, an interview, a folk performance, and the
    # gramophone -- opera, chamber music, a folk song -- coming out of the far reverb.
    ("Library", [
        ("Edison Cylinder", near(kind="Note", rate=180, length=8, pitch="Root", type="Clip", follow="Free", pos=0.0,
                                 attack=0.05, release=1.5, cutoff=8000, proximity=0.6, spread=0.3, approach=0.1,
                                 level=0.5, clip="Archive/LoC/Edison/")),
        ("Variety Stage", near(kind="Note", rate=240, length=8, pitch="Root", type="Clip", follow="Free", pos=0.0,
                               attack=0.05, release=1.5, cutoff=8000, proximity=0.5, spread=0.5, approach=0.2,
                               level=0.45, clip="Archive/LoC/Variety-Stage/")),
        ("Screening Room", near(kind="Note", rate=300, length=10, pitch="Root", type="Clip", follow="Free", pos=0.0,
                                attack=0.1, release=2.0, cutoff=9000, proximity=0.4, spread=0.4, approach=0.3,
                                level=0.45, clip="Archive/LoC/Screening-Room/")),
        ("New York, 1950s", near(kind="Note", rate=200, length=12, pitch="Root", type="Clip", follow="Free", pos=0.0,
                                 attack=0.3, release=2.5, cutoff=10000, proximity=0.3, spread=0.7, approach=0.4,
                                 level=0.45, clip="Archive/LoC/Tony-Schwartz/")),
        ("Interview", near(kind="Note", rate=240, length=8, pitch="Root", type="Clip", follow="Free", pos=0.0,
                           attack=0.05, release=1.5, cutoff=9000, proximity=0.7, spread=0.2, approach=0.0,
                           level=0.45, clip="Archive/LoC/Joe-Smith/")),
        ("Cylinder Cut-Up", seq(type="Clip", follow="Free", pos=0.0, cutoff=8000, resonance=0.1, filt="LP 12",
                                steps=7, step=0.5, mutation=0.12, scatter=0.4, bloom=0.2, attack=0.01, decay=0.3, sustain=0.3,
                                release=0.1, length=90, rate=600, approach=0.0, level=0.4, clip="Archive/LoC/Edison/")),
    ]),
    ("Voices", [
        ("Radio Murmur", near(type="Murmur", force=0.5, speed=0.5, pos=0.85, bright=0.5, pos_drift=0.4, octave=-1,
                              rate=240, length=14, attack=0.2, release=1.2, spread=0.7, level=0.5)),
        ("Ground Control", near(type="Murmur", force=0.6, speed=0.55, pos=1.0, bright=0.6, pos_drift=0.3, octave=-1,
                                rate=300, length=18, attack=0.1, release=0.8, spread=0.8, level=0.5)),
        # A voice that never says anything, close and without its radio, can sound like a broken
        # voice (Rene, 13.09.): the whisper keeps a little of the band and arrives out of the far
        # plane rather than standing at the lips.
        ("Whisper Close", near(type="Murmur", force=0.3, speed=0.4, pos=0.35, bright=0.4, pos_drift=0.6, octave=-1,
                               rate=210, length=10, attack=0.3, release=2, spread=0.4, proximity=0.5, approach=0.3, level=0.4)),
        ("Far Chatter", near(type="Murmur", force=0.45, speed=0.6, pos=0.7, bright=0.5, pos_drift=0.5, octave=-1,
                             rate=280, length=20, attack=0.5, release=3, approach=0.4, spread=0.9, level=0.45)),
        ("Low Voice", near(type="Murmur", force=0.55, speed=0.35, pos=0.3, bright=0.35, pos_drift=0.3, octave=-2,
                           rate=260, length=12, attack=0.4, release=2.5, pitch="Lowest", level=0.5)),
    ]),
    ("Ice", [
        ("Ice Shift", near(type="Ice", force=0.7, speed=0.2, pos=0.2, bright=0.4, pos_drift=0.3, octave=-1,
                           rate=180, length=10, attack=0.8, release=3, level=0.6)),
        ("Old Wood", near(type="Ice", force=0.6, speed=0.4, pos=0.5, bright=0.3, pos_drift=0.2, octave=-1,
                          rate=210, length=8, attack=0.5, release=2, level=0.55)),
        ("Glacier", near(type="Ice", force=0.8, speed=0.12, pos=0.1, bright=0.25, pos_drift=0.4, octave=-2,
                         rate=320, length=24, attack=2, release=6, approach=0.3, pitch="Lowest", level=0.6)),
        ("Hull Strain", near(type="Ice", force=0.75, speed=0.3, pos=0.3, bright=0.35, pos_drift=0.3, octave=-2,
                             rate=240, length=14, attack=1, release=4, level=0.55)),
    ]),
    ("Strings", [
        ("E-Bow Glide", near(type="Bow", force=0.6, speed=0.45, pos=0.3, bright=0.6, pos_drift=0.2,
                             kind="Phrase", glide=10, rate=240, length=26, attack=3, release=7, approach=0.2, level=0.55)),
        ("Lap Steel Cry", near(type="Bow", force=0.55, speed=0.55, pos=0.25, bright=0.75, pos_drift=0.3, octave=1,
                               kind="Phrase", glide=7, rate=260, length=20, attack=2, release=6, pitch="Highest", level=0.5)),
        ("Cello Under", near(type="Bow", force=0.7, speed=0.35, pos=0.35, bright=0.4, pos_drift=0.2, octave=-1,
                             rate=220, length=18, attack=2.5, release=6, pitch="Lowest", level=0.55)),
        ("Koto", near(type="Off", strike=0.7, strike_type="String", strike_decay=0.9, strike_damp=0.35,
                      rate=120, length=1, attack=0.005, release=0.5, cutoff=6000, level=0.6)),
        ("Harp Touch", near(type="Off", strike=0.55, strike_type="String", strike_decay=1.8, strike_damp=0.45,
                            rate=150, length=1, attack=0.005, release=1, pitch="Highest", level=0.55)),
    ]),
    ("Sequences", [
        ("Phaedra", seq(type="FM", fm_ratio=1.0, fm_index=1.4, octave=0, cutoff=900, resonance=0.35, filt="Ladder",
                        steps=7, step=0.42, mutation=0.12, bloom=0.7, length=240, rate=480, level=0.5)),
        ("Rubycon", seq(type="FM", fm_ratio=2.0, fm_index=0.9, octave=-1, cutoff=700, resonance=0.4, filt="Ladder",
                        steps=11, step=0.33, mutation=0.15, scatter=0.35, bloom=0.8, length=300, rate=600, level=0.5)),
        ("Gamelan Line", seq(type="Additive", partials=10, tilt=1.6, inharmonic=0.3, octave=1, cutoff=9000, resonance=0.1,
                             filt="LP 12", steps=9, step=0.5, mutation=0.1, scatter=0.25, bloom=0.3, decay=0.6, release=0.5,
                             length=200, rate=500, level=0.45)),
        ("Koto Line", seq(type="Off", strike=0.7, strike_type="String", strike_decay=0.8, strike_damp=0.35, cutoff=8000,
                          steps=7, step=0.48, mutation=0.1, scatter=0.3, bloom=0.2, fenv=0.0, length=180, rate=420, level=0.55)),
        ("Vocal Pulse", seq(type="Additive", partials=16, tilt=1.0, octave=-1, cutoff=1200, resonance=0.6, filt="Formant",
                            steps=5, step=0.6, mutation=0.08, scatter=0.2, bloom=0.5, decay=0.5, release=0.6, fenv=0.3,
                            length=210, rate=540, level=0.45)),
        ("Subterranean Pulse", seq(type="Additive", partials=3, tilt=2.0, octave=-2, cutoff=300, resonance=0.2, filt="LP 24",
                                   steps=5, step=0.8, mutation=0.05, scatter=0.1, bloom=0.2, decay=0.5, release=0.6,
                                   pitch="Root", length=260, rate=600, level=0.6)),
        ("Glass Balafon", seq(type="FM", fm_ratio=3.5, fm_index=0.7, octave=1, cutoff=12000, resonance=0.05, filt="LP 12",
                              steps=9, step=0.36, mutation=0.15, scatter=0.4, bloom=0.4, decay=0.4, release=0.4,
                              length=220, rate=520, level=0.4)),
        # Three more from Rene's list: a glass table pluck whose frame wanders, an FM bell on a
        # ratio that is not a whole number so every strike beats differently, and a machine pulse
        # -- a sub impulse with a wooden tick, filtered, the ship's engine of SRF and Lustmord
        # rather than an arpeggio.
        ("Glass Pluck", seq(type="Wavetable", table="Glass", pos=0.3, pos_drift=0.7, octave=1, cutoff=9000, resonance=0.1, filt="LP 12",
                            steps=7, step=0.45, mutation=0.12, scatter=0.3, bloom=0.5, decay=0.5, release=0.45,
                            length=230, rate=520, level=0.45)),
        ("Beating Bell", seq(type="FM", fm_ratio=3.53, fm_index=1.1, octave=0, cutoff=14000, resonance=0.05, filt="LP 12",
                             steps=11, step=0.55, mutation=0.1, scatter=0.25, bloom=0.3, decay=1.2, release=1.0,
                             length=240, rate=560, level=0.4)),
        ("Machine Pulse", seq(type="Additive", partials=2, tilt=2.5, octave=-2, cutoff=220, resonance=0.3, filt="LP 24",
                              strike=0.35, strike_type="Wood", strike_decay=0.08, strike_damp=0.8,
                              steps=5, step=0.9, mutation=0.04, scatter=0.05, bloom=0.15, decay=0.35, release=0.5,
                              pitch="Root", length=300, rate=640, level=0.6)),
        # More of them, on the other sources (Rene, 13.09.: "gerne mit unterschiedlichen Sounds"):
        # the same shift register driving a flute, a bowl, the ice, a bow, a harmonic bank under
        # a ladder, a wavetable, pitched drops, a tuned noise, and a slow one for a cathedral.
        ("Flute Line", seq(type="Flute", pos=0.45, bright=0.6, octave=0, cutoff=9000, resonance=0.1, filt="LP 12",
                           steps=7, step=0.5, mutation=0.1, scatter=0.25, bloom=0.4, attack=0.03, decay=0.3, sustain=0.5,
                           release=0.3, length=200, rate=480, level=0.45)),
        ("Bowl Line", seq(type="Bowl", force=0.5, speed=0.4, pos=0.3, octave=1, cutoff=12000, resonance=0.05, filt="LP 12",
                          steps=9, step=0.6, mutation=0.08, scatter=0.3, bloom=0.3, decay=0.8, sustain=0.2, release=0.8,
                          length=220, rate=520, level=0.45)),
        ("Ice Steps", seq(type="Ice", force=0.6, speed=0.3, pos=0.4, octave=-1, cutoff=3000, resonance=0.3, filt="Ladder",
                          steps=5, step=0.8, mutation=0.06, scatter=0.15, bloom=0.4, decay=0.5, release=0.6,
                          pitch="Root", length=240, rate=600, level=0.5)),
        ("Bowed Line", seq(type="Bow", force=0.6, speed=0.5, pos=0.2, octave=0, cutoff=5000, resonance=0.2, filt="LP 24",
                           steps=7, step=0.42, mutation=0.12, scatter=0.25, bloom=0.6, attack=0.02, decay=0.3, sustain=0.4,
                           release=0.3, length=210, rate=500, level=0.45)),
        ("Harmonic Pulse", seq(type="Harmonic", bright=0.4, octave=-1, cutoff=1500, resonance=0.5, filt="Ladder",
                               steps=11, step=0.3, mutation=0.15, scatter=0.35, bloom=0.8, decay=0.25, release=0.3,
                               length=260, rate=560, level=0.5)),
        ("Wavetable Run", seq(type="Wavetable", pos=0.5, pos_drift=0.5, octave=0, cutoff=2500, resonance=0.45, filt="Ladder",
                              steps=7, step=0.36, mutation=0.14, scatter=0.3, bloom=0.7, decay=0.3, release=0.3,
                              length=230, rate=540, level=0.5)),
        ("Drop Sequence", seq(type="Drops", density=3.0, pos=0.5, bright=0.55, follow="Note", octave=1, cutoff=14000,
                              resonance=0.05, filt="LP 12", steps=9, step=0.5, mutation=0.1, scatter=0.3, bloom=0.3,
                              decay=0.4, release=0.5, length=200, rate=480, level=0.45)),
        ("Tuned Noise", seq(type="Noise", noise="White", noise_q=0.85, pos=0.5, octave=0, cutoff=12000, resonance=0.1,
                            filt="LP 12", steps=7, step=0.45, mutation=0.1, scatter=0.35, bloom=0.4, decay=0.2,
                            release=0.3, length=180, rate=460, level=0.45)),
        ("Slow Cathedral", seq(type="FM", fm_ratio=2.0, fm_index=0.5, octave=-1, cutoff=3000, resonance=0.15, filt="LP 24",
                               steps=5, step=1.2, mutation=0.06, scatter=0.15, bloom=0.5, decay=1.5, release=1.5,
                               length=300, rate=700, level=0.45)),
        # A heartbeat (13.09.): two steps, the second the softer one, on a sub of two partials
        # under a closed ladder, at a walking pulse that the tempo drifter lets breathe; nothing
        # mutates, nothing scatters, the room stays away.
        ("Heartbeat", seq(type="Additive", partials=2, tilt=2.0, octave=-2, cutoff=140, resonance=0.25, filt="LP 24",
                          steps=2, step=0.42, mutation=0.0, scatter=0.0, bloom=0.0, attack=0.004, decay=0.14, release=0.2,
                          pitch="Root", length=45, rate=360, approach=0.0, proximity=0.8, level=0.55)),
    ]),
    # The archive (Library/Archive, fetched by Tools/library/fetch_archive.py): NASA's own
    # recordings, works of the United States government. Each preset plays one of them straight,
    # once, close, every few minutes; the radio loops through their own band, the Mars recordings
    # as they are. Lengths are the recordings' own, a little over.
    ("Archive", [
        archive("Houston, We've Had a Problem", "NASA/Historical/Apollo-Mercury", "Apollo 13 - Houston weve had a problem.flac", 14, 420),
        archive("The Eagle Has Landed", "NASA/Historical/Apollo-Mercury", "Apollo 11 - Eagle has landed extended.flac", 40, 600),
        archive("One Small Step", "NASA/Historical/Apollo-Mercury", "Apollo 11 - One small step.flac", 14, 480),
        archive("Merry Christmas from Apollo 8", "NASA/Historical/Apollo-Mercury", "Apollo 8 - Merry Christmas.flac", 30, 540),
        archive("Godspeed, John Glenn", "NASA/Historical/Apollo-Mercury", "Mercury 6 - Godspeed.flac", 12, 420),
        archive("Fireflies", "NASA/Historical/Apollo-Mercury", "Mercury 7 - Fireflies.flac", 20, 480),
        archive("We Choose the Moon", "NASA/Historical/Apollo-Mercury", "JFK - We choose the moon.flac", 45, 720),
        archive("Roger Roll", "NASA/Historical/Discovery", "Discovery - Roger roll.flac", 8, 300),
        archive("Go at Throttle Up", "NASA/Historical/Discovery", "Discovery - Go at throttle up 1.flac", 8, 300),
        archive("Nice to Be in Orbit", "NASA/Historical/Discovery", "Discovery - Nice to be in orbit.flac", 8, 330),
        archive("Houston, Discovery", "NASA/Historical/Discovery", "Discovery - Houston Discovery.flac", 8, 300),
        archive("Dust It Off First", "NASA/Historical/Shuttle", "STS-1 - Were going to dust it off first.flac", 10, 360),
        archive("Quindar", "NASA/Historical/Beeps", "Quindar sound 1.flac", 3, 120, level=0.4),
        archive("Sputnik", "NASA/Historical/Beeps", "Sputnik beep.flac", 12, 240, level=0.4),
        archive("Chorus", "NASA/Historical/Beeps", "Chorus radio waves in Earths atmosphere.flac", 30, 300, level=0.45, approach=0.3),
        archive("Saturn Radio", "NASA/Historical/Missions", "Cassini - Saturn radio emissions 1.flac", 40, 360, level=0.45, approach=0.3),
        archive("Enceladus", "NASA/Historical/Missions", "Cassini - Enceladus sound.flac", 30, 330, level=0.45, approach=0.3),
        archive("Jupiter Lightning", "NASA/Historical/Missions", "Voyager - Lightning on Jupiter.flac", 30, 360, level=0.45),
        archive("Kepler Star", "NASA/Historical/Missions", "Kepler - Star KIC12268220C light curve.flac", 30, 400, level=0.4),
        archive("Wind on Mars", "NASA/Beyond", "Perseverance SuperCam records wind on Mars.flac", 30, 300, level=0.5, approach=0.25),
        archive("Ingenuity", "NASA/Beyond", "Ingenuity Mars helicopter in flight.flac", 60, 480, level=0.45, approach=0.3),
        archive("Dust Devil", "NASA/Beyond", "Perseverance records a Martian dust devil.flac", 30, 400, level=0.45),
        archive("Laser on Mars", "NASA/Beyond", "First acoustic recording of laser shots on Mars.flac", 20, 300, level=0.5),
        archive("Marsquake", "NASA/Beyond", "Marsquake magnitude 3 7 22 May 2019.flac", 30, 420, level=0.5, approach=0.3),
        archive("Dinks and Donks", "NASA/Beyond", "InSight seismometer dinks and donks.flac", 20, 300, level=0.5),
        archive("Ganymede", "NASA/Beyond", "Juno Ganymede flyby.flac", 60, 540, level=0.45, approach=0.3),
        archive("Cosmic Cliffs", "NASA/Sonification/Webb", "Webb - Cosmic Cliffs.flac", 40, 480, level=0.45, approach=0.3),
        archive("Southern Ring", "NASA/Sonification/Webb", "Webb - Southern Ring Nebula side by side.flac", 40, 480, level=0.45, approach=0.3),
    ]),
]


def write_bank(path, array, cat_array, off_name, off_settings, families, what):
    nl = chr(10)
    lines = ["// Generated by Tools/make_layer_presets.py -- do not edit.",
             "// %s" % what,
             "const Preset %s[] = {" % array,
             '    { "%s", "%s" },' % (off_name, off_settings)]
    cats = []
    names = set()
    for fi, (fam, presets) in enumerate(families):
        for name, settings in presets:
            assert name not in names, "duplicate preset name: " + name
            names.add(name)
            # A settings string may carry a clip as "|<path>" after it (see fmt_clip); it becomes
            # the preset's texture field, a file of the library's archive relative to its root.
            clip = None
            if "|" in settings:
                settings, clip = settings.split("|", 1)
            if clip:
                lines.append('    { "%s", "%s", "%s" },' % (name, settings, clip))
            else:
                lines.append('    { "%s", "%s" },' % (name, settings))
            cats.append(fi)
    lines.append("};")
    lines.append("")
    lines.append("// The family each preset belongs to; 255 is the Off entry, which has none.")
    lines.append("const unsigned char %s[] = {" % cat_array)
    lines.append("    255,")
    for i in range(0, len(cats), 12):
        lines.append("    " + " ".join("%d," % c for c in cats[i:i + 12]))
    lines.append("};")
    lines.append("")
    lines.append("const char* const %sFamilyNames[] = {" % array)
    for fam, _ in families:
        lines.append('    "%s",' % fam)
    lines.append("};")
    lines.append("")
    with open(path, "w", encoding="utf-8") as f:
        f.write(nl.join(lines))
    return len(cats) + 1


def main():
    n = write_bank(COSMOS_OUT, "kCosmosPresets", "kCosmosPresetCategory", "Cosmos Off", "",
                   COSMOS, "%d presets in %d families, touching only the Cosmos section."
                   % (sum(len(p) for _, p in COSMOS) + 1, len(COSMOS)))
    print("wrote %s: %d presets in %d families" % (COSMOS_OUT, n, len(COSMOS)))
    fams = [(f, [(nm, fmt(**kw)) for nm, kw in ps]) for f, ps in STRIKE]
    n = write_bank(STRIKE_OUT, "kStrikePresets", "kStrikePresetCategory", "Strike Off",
                   "strike_level=0", fams,
                   "%d presets in %d families, touching only the Strike section."
                   % (sum(len(p) for _, p in fams) + 1, len(fams)))
    print("wrote %s: %d presets in %d families" % (STRIKE_OUT, n, len(fams)))
    # Every clip a near preset names must exist under Library/ -- the archive's file names went
    # through fetch_archive.py's safe_name (no dots, no commas), and two presets naming the titles
    # as written on the page pointed at nothing, silently. Fatal, not a warning: a bank entry
    # that loads no clip plays Source 4's instead, which is the wrong recording, not silence.
    library = os.path.join(ROOT, "Library")
    # A clip ending in a slash is a folder of recordings: the near source plays one of them per event.
    missing = [kw["__clip__"] for _, ps in NEAR for _, kw in ps
               if kw.get("__clip__") and os.path.isdir(os.path.join(library, "Archive"))
               and not (os.path.isfile(os.path.join(library, kw["__clip__"])) or os.path.isdir(os.path.join(library, kw["__clip__"])))]
    if missing:
        sys.exit("near bank: %d clip(s) not found under Library/:\n  %s" % (len(missing), "\n  ".join(missing)))
    def fmt_clip(kw):
        kw = dict(kw)
        clip = kw.pop("__clip__", None)
        return fmt(**kw) + ("|" + clip if clip else "")
    fams = [(f, [(nm, fmt_clip(kw)) for nm, kw in ps]) for f, ps in NEAR]
    n = write_bank(NEAR_OUT, "kNearPresets", "kNearPresetCategory", "Near Off",
                   "fore_level=0", fams,
                   "%d presets in %d families, touching only the Near Source and Near Events sections."
                   % (sum(len(p) for _, p in fams) + 1, len(fams)))
    print("wrote %s: %d presets in %d families" % (NEAR_OUT, n, len(fams)))


if __name__ == "__main__":
    main()
