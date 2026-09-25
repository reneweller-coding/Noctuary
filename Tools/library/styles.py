"""Style corpus for the generated preset library -- and, since 2.0, its shared ground.

The library is generated from Tools/library/artists.py now (56 packs and the 16 built-in
families, with the material each of them plays); what stays here is what both generations share:
BASE, MODULES_BASE, COMMON_SECOND, GRANULAR_BASE and NOISE_BASE, which artists.py builds on, and
the twenty-five styles the library before it was written from, kept as the record of where the
ranges came from.

The pack names are descriptive; the `inspiration` line names the artist whose sound world the
settings aim at. Nothing here is sampled from or affiliated with those artists -- the ranges
were chosen by ear from the synth's own parameters.

A style is a dictionary of parameter ranges over the table in Core/src/Params.cpp:

    3.5                 a constant
    (lo, hi)            uniform float
    ("log", lo, hi)     log-uniform float (for times, rates and frequencies)
    ("int", lo, hi)     uniform integer
    ["A", "B"]          one of these choice names

`granular` shapes the Texture slot: `spread` is the range the start-point scatter is drawn from
(a tight window freezes the clip into a drone, a wide one turns it into a scattered field), and
`grains` biases how many grains the slot may hold on top of what the density needs.

`modules` gives the probability that a preset of this style switches an optional block on;
`words` is the two-part vocabulary the preset names are drawn from; `prompts` is the texture
vocabulary handed to the text-to-audio models; `tables` names the wavetable recipes that suit
the style (see Tools/WavetableGen).
"""

# ---------------------------------------------------------------- shared ground

# Every style starts from this and overrides what it cares about. These are drone defaults:
# long envelopes, a generative cluster brain, a wide far reverb, conservative level.
BASE = {
    "master_gain":   (-14.0, -8.0),
    "partials":      ("int", 8, 24),
    "tilt":          (0.8, 1.8),
    "brightness":    (0.4, 0.8),
    "odd_even":      (-0.4, 0.4),
    "inharmonic":    (0.0, 0.2),
    "shimmer":       (0.2, 0.6),
    "shimmer_rate":  ("log", 0.03, 0.4),
    "strands":       ("int", 2, 5),
    "detune":        ("log", 3.0, 18.0),
    "drift":         (2.0, 9.0),
    "drift_rate":    ("log", 0.02, 0.15),
    "spread":        (0.5, 0.9),
    "bloom":         (0.0, 0.5),
    "bloom_time":    ("log", 30.0, 180.0),
    "rate_wander":   (0.1, 0.5),
    "attack":        ("log", 3.0, 20.0),
    "decay":         ("log", 2.0, 15.0),
    "sustain":       (0.7, 0.95),
    "release":       ("log", 8.0, 40.0),
    "cutoff":        ("log", 900.0, 5000.0),
    "resonance":     (0.05, 0.3),
    "filter_env":    (0.0, 0.4),
    "filter_drift":  (0.15, 0.5),
    "keytrack":      (0.3, 0.7),
    "depth":         (0.5, 0.9),
    "pan_drift":     (0.2, 0.6),
    "itd":           (0.4, 0.8),
    "arc":           (0.1, 0.5),
    "arc_period":    ("log", 20.0, 120.0),
    "breath":        (0.0, 0.4),
    "breath_rate":   ("log", 0.01, 0.06),
    # Air is filtered noise on the note, and measured against everything else in the instrument it
    # is the loudest source of noise there is: on a pure sine with every other block off, the
    # spectral flatness goes from 0.000001 to 0.0149 at air 0.2, where the far reverb, the near
    # reverb, the Cloud with all its extras, the Cosmos with all four of its characters, the
    # Memory, the delays, the filters, the wavefolder and the ensemble all measure 0.000007 or
    # below. Every style that does not name its own air inherits this range -- which is why every
    # preset of a fourteen-thousand-preset library carried a noise band, and why the whole of it
    # was heard as "extremely noisy". A style that is about air says so itself.
    "air":           (0.0, 0.08),
    "air_color":     ("log", 1.5, 8.0),
    "air_q":         ("log", 4.0, 20.0),
    "ens_mix":       (0.2, 0.6),
    "ens_depth":     (0.2, 0.6),
    "ens_rate":      ("log", 0.05, 0.5),
    "dly_time_l":    ("log", 0.4, 2.5),
    "dly_time_r":    ("log", 0.5, 3.2),
    "dly_feedback":  (0.3, 0.7),
    "dly_cross":     (0.1, 0.7),
    "dly_damp":      (0.4, 0.8),
    "dly_mix":       (0.1, 0.3),
    "dly_to_far":    (0.2, 0.7),
    "near_mix":      (0.1, 0.3),
    "near_decay":    ("log", 0.6, 3.0),
    "near_damp":     (0.2, 0.6),
    "far_level":     (0.6, 1.0),
    "far_size":      (1.4, 3.0),
    "far_decay":     ("log", 12.0, 60.0),
    "far_damp":      (0.3, 0.7),
    "far_predelay":  (0.0, 5.0),   # the hall's own gap: the source's is Depth Gap now (25.09.2026)
    "far_asym":      (0.3, 0.8),
    "far_highcut":   ("log", 1800.0, 8000.0),
    "bass_mono":     ("log", 90.0, 200.0),
    "side_air":      (1.0, 3.5),
    "width":         (0.9, 1.5),
    "brain_density": ("int", 3, 7),
    "brain_rate":    ("log", 12.0, 70.0),
    "brain_hold_min":("log", 20.0, 90.0),
    "brain_hold_max":("log", 90.0, 300.0),
    "brain_low":     ("int", 34, 46),
    "brain_high":    ("int", 66, 84),
    "brain_consonance": (0.5, 0.9),
    "brain_wander":  (0.15, 0.5),
    "scale":         ["JI Major (Ptolemy)", "JI Minor", "JI 7-limit", "JI Pentatonic", "Pythagorean"],
    "root":          ["C", "D", "E", "F", "G", "A"],
    "purity":        (0.85, 1.0),
    "sub_level":     (0.0, 0.25),
    "sub_tone":      (0.1, 0.4),
    "pad_low_cut":   ("log", 20.0, 90.0),
}

MODULES_BASE = {
    "zplane": 0.35, "cosmos": 0.20, "cloud": 0.15, "feedback": 0.12,
    "src2": 0.45, "src3": 0.25, "sub": 0.45, "stack": 0.35, "room": 0.45,
    # usertable raised from 0.20 with the shelf: it held nineteen ideas with thirty-two variants
    # each, and no style named more than three of them. It now holds forty-three recipes and a
    # thousand tables sliced from recorded material, which is worth reaching for more often.
    "texture": 0.20, "usertable": 0.35, "keys": 0.10, "delay2": 0.20,
    "coherence": 0.15, "portamento": 0.10,
    # the Rich refinements: how often a preset of this style reaches for them
    "phase": 0.45, "blur": 0.15, "filtermodel": 0.35, "strike": 0.12, "absorb": 0.5, "tide": 0.3, "rotate": 0.35,
    # The mixing desk. All four are off at their defaults, so a style that does not ask for them
    # is the style it was: "narrow" pulls the background in towards the centre as it goes back,
    # "haas" opens the foreground's own upper middle, "microshift" makes the Ensemble a static
    # detune instead of a chorus, and "fold" is the wavefolder after the filters.
    "narrow": 0.30, "haas": 0.22, "microshift": 0.25, "fold": 0.10,
    # And the hands: aftertouch, the wheel and the slide as routes in the matrix. Cheap and
    # neutral until they are moved, so most styles carry at least one.
    "hands": 0.55,
    # The two newest source types. Zero here on purpose, and read through a SEPARATE random
    # stream in make_presets, so that every pack made before they existed comes out of the
    # generator byte for byte as it did -- six thousand four hundred presets are keyed on the
    # sequence of draws, and one extra rng.random() anywhere in the path moves all of them.
    "bow": 0.0, "spectral": 0.0,

    # ---- what the library never touched -------------------------------------------------
    # Measured over the finished library: 177 of the instrument's 498 parameters appeared in not
    # one of its 6800 presets. The first slot was additive in every single one of them, and
    # everything built after the library was generated -- the cascade, the fields, the attractors,
    # the second conductor, adaptive tuning, the near field, the newer room axes -- was in nothing
    # at all. These weights are how a preset reaches for them. They are drawn from the `extra`
    # stream, like bow and spectral, so a style that leaves them alone is untouched.
    # Raised from 0.72 after the shelves grew: the tonal sample library roughly tripled, and a
    # measurement of both spaces said what Rene had said three times -- a grain slot carrying its
    # own recording differs from its neighbours more than an additive bank differs from another
    # additive bank. The bank keeps about a quarter of the library, which is the share where it
    # is the point rather than the default.
    "slot1": 0.82,          # the first slot is something other than the additive bank
    "noiseprimary": 0.0,    # ... and may be noise: only where a style asks for it by name
    "cascade": 0.28,        # the conductor's clock as a Hawkes process: events breed events
    "surprise": 0.22,       # entropy held to a target (Surprise + Homeostat)
    "dejavu": 0.24,         # the ring that brings figures back and lets them mutate
    "spreadbias": 0.30,     # the shape of its draws: grey average or soft-or-loud
    "blend": 0.20,          # a chord arriving as one object inside the fusion window
    "keyfind": 0.26,        # it finds the key it has drifted into
    "evensmooth": 0.22,     # how evenly spread and how smoothly voiced it wants its chords
    "timbre": 0.18,         # consonance judged from the spectrum actually sounding (Sethares)
    "quantize": 0.14,       # events on the clock's grid
    "brain2": 0.20,         # a second conductor for the background plane
    "adaptive": 0.22,       # each arriving note tuned pure against what is sounding
    "guard": 0.20,          # drifting beats kept out of the wobble band
    "match": 0.10,          # the partials bent onto the scale instead of the other way round
    "transpose": 0.08,      # the whole instrument a fourth, fifth or octave away, gliding
    "keysfilter": 0.10,     # which of the conductor's planes your keys land on
    "nearfield": 0.16,      # voices within reach: the low-frequency level difference
    "comod": 0.22,          # the whole background breathing as one
    "envelop": 0.28,        # the low lateral energy that says "inside a room"
    "depthlaw": 0.30,       # the depth knob made linear in heard distance
    "elev": 0.24,           # the planes given a height, heard through the pinna notch
    "binaural": 0.12,       # headphone modes, including the head-tracked one
    "farmode": 0.30,        # the far reverb's algorithm, including the rotating matrix
    "earlyroom": 0.22,      # the early-reflection room in front of the tail
    "arcclock": 0.06,       # the hour-long arc locked to the time of day
    "archarmony": 0.16,     # the arc loosening and tightening the harmony
    "presence": 0.20,       # the near plane's presence lift
    "externalise": 0.12,    # crossfeed for headphones
    "lenia": 0.18,          # a continuous cellular automaton as four modulation sources
    "chaos": 0.18,          # the Lorenz and Roessler attractors, minutes long
    "sympathy": 0.16,       # how hard the coherence ring pulls on the voices
    "pulse": 0.12,          # the sub as a slow amplitude pulse
    "fbbias": 0.10,         # even-order colour in the feedback loop
    "partialspread": 0.16,  # the bank's partials fanned across the field one at a time
    "cosmosswell": 0.35,    # the Cosmos following the cascade (only where there is a Cosmos)
    "transport": 0.14,      # how a wavetable's frames morph: fade or optimal transport
    "bodychar": 0.55,       # the body's material, pitch, tone and spread (where a body is on)
    "patina": 0.18,         # wow, hiss and age
    "roommorph": 0.14,      # two impulse responses, morphed between
    "farfreeze": 0.05,      # the far reverb held open for ever
    "fardiffuse": 0.22,     # how diffuse the tail is
    "lfomodes": 0.28,       # per-voice and retriggered LFOs, and the tempo-synced ones
    "banks": 0.45,          # a preset from the Cosmos, Strike or Z-plane banks as a layer
    "vector": 0.30,         # the four slots read as the corners of one square, the point moving
    "mastertilt": 0.18,     # the master's tilt and its pivot
    "sync": 0.16,           # delays, arcs and the conductor on the host's bars
    "expression": 0.35,     # what aftertouch and the slide do, as fixed routing
    # How much of this style's grain material is environment rather than tone. The field
    # recordings are a folder of their own now; a style that wants a swamp says so.
    "environment": 0.5,
}

# Word pools the preset names are built from: "<first> <second>".
COMMON_SECOND = ["Drift", "Field", "Bloom", "Veil", "Hollow", "Expanse", "Passage", "Threshold",
                 "Vigil", "Descent", "Signal", "Reach", "Basin", "Interval", "Bed", "Span"]


# Default granular character: a moderate scatter, no extra grains beyond what the density needs.
GRANULAR_BASE = {"spread": (0.02, 0.35), "grains": 1.0}

# Which of the 200 generated impulses suit a style (name prefixes, see Tools/library/make_impulses.py)
# and which noise colours belong to it.
IMPULSES_BASE = ["room_hall", "room_chamber", "spectral"]
# All ten colours, not the four the library used to draw from. Pink and Brown stay likeliest --
# they are what a bed is made of -- but Blue, Violet, Grey, Band, Crackle and Digital are in the
# instrument and were in no preset at all.
# Without White, Grey and Violet: the hiss colours. Grey measured as the flattest setting in the
# whole library (median flatness x1.94, p90 0.18 against the built-ins' 0.03) -- it is white noise
# with a dip, and a drone does not want white noise under it. A style may still ask for one by
# name in its own list.
NOISE_BASE = ["Pink", "Pink", "Brown", "Brown", "Wind", "Band", "Blue", "Crackle", "Digital"]


def S(name, inspiration, params, modules=None, words=None, prompts=None, tables=None, granular=None,
      impulses=None, noise=None):
    st = {"name": name, "inspiration": inspiration, "params": dict(BASE), "modules": dict(MODULES_BASE),
          "words": words or (["Slow", "Long", "Deep"], COMMON_SECOND),
          "prompts": prompts or [], "tables": tables or ["Tilt walk", "Random walk"],
          "granular": dict(GRANULAR_BASE),
          "impulses": impulses or list(IMPULSES_BASE), "noise": noise or list(NOISE_BASE)}
    if granular:
        st["granular"].update(granular)
    st["params"].update(params)
    if modules:
        st["modules"].update(modules)
    return st


# ---------------------------------------------------------------- the twenty-five packs

STYLES = [

    S("Sleep Concert", "Robert Rich",
      {"scale": ["JI 7-limit", "JI Major (Ptolemy)", "JI Minor", "Otonality 1-11", "Slendro (JI)"],
       "brain_density": ("int", 4, 7), "brain_rate": ("log", 25.0, 90.0),
       "brain_hold_min": ("log", 45.0, 150.0), "brain_hold_max": ("log", 150.0, 420.0),
       "attack": ("log", 8.0, 30.0), "release": ("log", 15.0, 50.0),
       "bloom": (0.3, 0.8), "bloom_time": ("log", 60.0, 240.0),
       "sub_level": (0.15, 0.4), "sub_binaural": (2.0, 7.0), "sub_source": ["Root", "Difference"],
       "far_decay": ("log", 20.0, 55.0), "far_highcut": ("log", 1800.0, 4500.0),
       "brightness": (0.45, 0.75), "arc": (0.3, 0.7), "arc_period": ("log", 40.0, 180.0),
       "purity": (0.95, 1.0), "depth": (0.7, 0.95), "air": (0.1, 0.35)},
      {"sub": 0.9, "zplane": 0.3, "cosmos": 0.2, "texture": 0.3, "usertable": 0.25, "feedback": 0.05},
      words=(["Sleep", "Somnia", "Lullaby", "Trance", "Night", "Dream", "Rainforest", "Gaudi", "Lattice"],
             COMMON_SECOND + ["Concert", "Cycle", "Hours", "Chorus"]),
      prompts=["a slow just-intoned drone chorus of glass flutes, no rhythm, endless sustain",
               "warm analog drone cluster, soft beating partials, sleep concert",
               "bowed glass harmonica sustained chord, breathing slowly",
               "deep binaural sine bed with faint overtone shimmer",
               "gentle overtone singing bowl cloud, very slow"],
      tables=["Glass thinning", "Odd breathing", "Formant sweep", "Singing bowl", "Stretched string"],
      granular={"spread": (0.004, 0.05), "grains": 1.3},
      impulses=['tuned', 'room_cathedral', 'room_hall', 'shimmer'], noise=['Pink', 'Brown', 'Pink']),

    S("Deep Earth", "Lustmord",
      {"master_gain": (-13.0, -8.0),
       "brightness": (0.1, 0.35), "tilt": (1.6, 2.8), "partials": ("int", 6, 14),
       "cutoff": ("log", 250.0, 1200.0), "far_highcut": ("log", 900.0, 2200.0),
       "far_decay": ("log", 35.0, 85.0), "far_size": (2.2, 3.0), "far_damp": (0.5, 0.85),
       "brain_low": ("int", 24, 34), "brain_high": ("int", 48, 66), "brain_density": ("int", 3, 6),
       "sub_level": (0.35, 0.7), "sub_octave": ["-2"], "sub_tone": (0.0, 0.25),
       "bass_mono": ("log", 130.0, 260.0), "air": (0.0, 0.15), "shimmer": (0.05, 0.3),
       "inharmonic": (0.1, 0.4), "depth": (0.75, 1.0), "pad_low_cut": ("log", 20.0, 45.0),
       "scale": ["JI Minor", "Subharmonic 16-8", "JI 7-limit", "Pythagorean"]},
      {"sub": 1.0, "feedback": 0.3, "cloud": 0.3, "cosmos": 0.35, "zplane": 0.3, "texture": 0.3},
      words=(["Abyssal", "Mantle", "Tectonic", "Subterranean", "Obsidian", "Chthonic", "Bedrock", "Magma", "Void"],
             COMMON_SECOND + ["Chamber", "Pressure", "Weight", "Fault"]),
      prompts=["immense subterranean rumble, cavern resonance, no melody",
               "deep infrasonic drone with distant geological groan",
               "vast dark cave tone, low pressure hum, dread",
               "monolithic bass drone, black and slow",
               "tectonic low frequency shudder, long decay"],
      tables=["Tilt walk", "Sub fold", "Sub bloom", "Gong wash", "Pink resonance"],
      granular={"spread": (0.006, 0.08), "grains": 1.2},
      impulses=['room_cavern', 'room_bunker', 'spectral'], noise=['Brown', 'Pink']),

    S("Permafrost", "Thomas Koener",
      {"master_gain": (-15.0, -10.0),
       "brightness": (0.05, 0.3), "tilt": (1.8, 3.0), "partials": ("int", 4, 10),
       "cutoff": ("log", 180.0, 800.0), "far_highcut": ("log", 700.0, 1600.0),
       "far_decay": ("log", 30.0, 70.0), "shimmer": (0.0, 0.2), "shimmer_rate": ("log", 0.01, 0.08),
       "brain_density": ("int", 2, 4), "brain_rate": ("log", 60.0, 200.0),
       "brain_hold_min": ("log", 90.0, 300.0), "brain_hold_max": ("log", 240.0, 600.0),
       "brain_low": ("int", 24, 32), "brain_high": ("int", 42, 58),
       "attack": ("log", 15.0, 50.0), "release": ("log", 30.0, 90.0),
       "sub_level": (0.3, 0.6), "air": (0.0, 0.1), "ens_mix": (0.0, 0.2),
       "drift": (0.5, 3.0), "detune": ("log", 2.0, 8.0), "depth": (0.85, 1.0)},
      {"sub": 0.95, "cloud": 0.4, "zplane": 0.25, "cosmos": 0.2, "texture": 0.35, "feedback": 0.1,
       "src2": 0.35, "src3": 0.15},
      words=(["Permafrost", "Nunatak", "Glacier", "Teimo", "Nuuk", "Polar", "Ice", "Tundra", "Frost"],
             COMMON_SECOND + ["Shelf", "Night", "Silence", "Plain"]),
      prompts=["frozen wind over ice, almost no pitch, glacial",
               "dark filtered noise field, arctic night, motionless",
               "distant ice sheet groaning under pressure",
               "muffled low rumble under snow, no melody",
               "black glacial drone, barely moving"],
      tables=["Random walk", "Tilt walk", "Pink resonance", "Breath band", "Spectral erosion"],
      granular={"spread": (0.2, 0.8), "grains": 1.4},
      impulses=['room_cavern', 'room_bunker', 'spectral', 'scatter'], noise=['Brown', 'Wind', 'Pink']),

    S("Field Absence", "Francisco Lopez",
      {"master_gain": (-18.0, -12.0),
       "brightness": (0.2, 0.6), "inharmonic": (0.3, 0.7), "partials": ("int", 10, 26),
       "cutoff": ("log", 400.0, 4000.0), "air": (0.25, 0.6), "air_color": ("log", 3.0, 14.0),
       "air_q": ("log", 2.0, 8.0), "brain_density": ("int", 2, 5),
       "brain_rate": ("log", 40.0, 180.0), "brain_consonance": (0.05, 0.4),
       "attack": ("log", 10.0, 45.0), "release": ("log", 20.0, 70.0),
       "shimmer": (0.0, 0.25), "far_decay": ("log", 8.0, 30.0), "depth": (0.6, 1.0)},
      {"cloud": 0.7, "texture": 0.5, "sub": 0.3, "zplane": 0.4, "cosmos": 0.25, "feedback": 0.2,
       "src2": 0.7, "src3": 0.45},
      words=(["Untitled", "Belle", "Buildings", "La Selva", "Wind", "Rain", "Absence", "Presence", "Machine"],
             COMMON_SECOND + ["No. 1", "No. 2", "Study", "Recording"]),
      prompts=["dense insect and rain field recording texture, no music",
               "amplified silence, faint hiss and distant machinery",
               "tropical night field recording, layered noise, no melody",
               "granular noise bed, unidentifiable source, very quiet",
               "hum of a large empty building, air conditioning"],
      tables=["Random walk", "Comb", "Pink resonance", "Waterphone", "Spectral erosion"],
      granular={"spread": (0.35, 1.0), "grains": 1.8},
      impulses=['scatter', 'comb', 'spectral', 'room_chamber'], noise=['White', 'Crackle', 'Digital', 'Wind']),

    S("Aldebaran", "Inade",
      {"brightness": (0.2, 0.5), "inharmonic": (0.2, 0.5), "partials": ("int", 10, 22),
       "cutoff": ("log", 500.0, 2500.0), "far_decay": ("log", 30.0, 75.0), "far_size": (2.0, 3.0),
       "brain_low": ("int", 28, 40), "brain_high": ("int", 55, 74), "brain_consonance": (0.2, 0.6),
       "sub_level": (0.25, 0.55), "air": (0.1, 0.35), "shimmer": (0.2, 0.55),
       "scale": ["Subharmonic 16-8", "JI Minor", "Bohlen-Pierce (JI)", "JI 7-limit"],
       "depth": (0.7, 1.0), "arc": (0.3, 0.7)},
      {"cosmos": 0.7, "sub": 0.85, "zplane": 0.5, "feedback": 0.25, "cloud": 0.3, "texture": 0.3,
       "stack": 0.5},
      words=(["Aldebaran", "Sirius", "Oneiric", "Ritual", "Aureole", "Sphere", "Sonic", "Cosmic", "Beyond"],
             COMMON_SECOND + ["Rite", "Order", "Sphere", "Ascent"]),
      prompts=["ritual metallic drone, gongs and deep space, ceremonial",
               "vast cosmic hum with metallic overtones, sacred",
               "resonant bronze bowl cluster in an enormous hall",
               "dark ceremonial drone with distant choir",
               "interstellar wind with ringing metal"],
      tables=["Glass thinning", "Bell partials", "Ring bell", "Shepard stack", "Bohlen-Pierce"],
      granular={"spread": (0.02, 0.3), "grains": 1.2},
      impulses=['modal', 'tuned', 'room_cathedral'], noise=['Brown', 'Wind', 'Pink']),

    S("Ritual Machine", "Deutsch Nepal",
      {"master_gain": (-13.0, -9.0),
       "brightness": (0.25, 0.6), "inharmonic": (0.25, 0.6), "tilt": (1.0, 2.2),
       "cutoff": ("log", 400.0, 2200.0), "resonance": (0.15, 0.5),
       "fb_bus": (0.06, 0.2), "fb_drive": (0.5, 0.95), "fb_tone": ("log", 500.0, 3000.0),
       "fb_tape": (0.3, 0.8), "brain_rate": ("log", 8.0, 40.0), "brain_consonance": (0.1, 0.5),
       "dly_feedback": (0.5, 0.85), "dly_mix": (0.15, 0.4), "sub_level": (0.2, 0.5),
       "far_decay": ("log", 15.0, 45.0), "depth": (0.5, 0.85)},
      {"feedback": 1.0, "sub": 0.7, "cloud": 0.35, "cosmos": 0.3, "zplane": 0.45, "texture": 0.35,
       "delay2": 0.4},
      words=(["Deflagration", "Erosion", "Benevolence", "Wrought", "Iron", "Furnace", "Rust", "Sermon", "Grind"],
             COMMON_SECOND + ["Loop", "Engine", "Wheel", "Liturgy"]),
      prompts=["grinding industrial loop, corroded metal, ritual",
               "distorted tape loop of machinery, dark and repetitive",
               "rusted factory drone with feedback",
               "slow mechanical churn, saturated and dirty",
               "ominous metal scrape over a low hum"],
      tables=["Comb", "Saw to square", "Metal bar", "Lo-fi bits", "Ring cluster"],
      granular={"spread": (0.05, 0.4), "grains": 1.1},
      impulses=['comb', 'modal', 'scatter'], noise=['White', 'Crackle', 'Digital']),

    S("Planetary", "Michael Stearns",
      {"scale": ["Harmonic 8-16", "Otonality 1-11", "JI Major (Ptolemy)", "JI 7-limit"],
       "brightness": (0.55, 0.9), "partials": ("int", 14, 30), "tilt": (0.6, 1.3),
       "shimmer": (0.4, 0.85), "shimmer_rate": ("log", 0.05, 0.5),
       "far_size": (2.2, 3.0), "far_decay": ("log", 25.0, 70.0), "far_highcut": ("log", 4000.0, 12000.0),
       "spread": (0.7, 1.0), "width": (1.2, 1.8), "side_air": (2.0, 5.0),
       "arc": (0.4, 0.9), "arc_period": ("log", 30.0, 150.0), "depth": (0.6, 0.9),
       "air": (0.15, 0.45), "bloom": (0.3, 0.8)},
      {"cosmos": 0.55, "zplane": 0.4, "stack": 0.6, "sub": 0.6, "usertable": 0.35, "texture": 0.25},
      words=(["Planetary", "Beam", "Encounter", "Solar", "Orbital", "Chronos", "Ascension", "Sacred", "Lyra"],
             COMMON_SECOND + ["Unfolding", "Arc", "Halo", "Corona"]),
      prompts=["enormous shimmering harmonic drone, planetary scale, awe",
               "beam of overtones sweeping through a vast space",
               "sacred wide chorus of metallic strings, cinematic",
               "solar wind choir, bright and endless",
               "huge harmonic series pad with slow shimmer"],
      tables=["Shepard stack", "Bell partials", "Stretched octave", "Harmonic gate", "Glass thinning"],
      granular={"spread": (0.01, 0.15), "grains": 1.4},
      impulses=['shimmer', 'tuned', 'room_cathedral'], noise=['Violet', 'Blue', 'Pink']),

    S("Temple of Air", "Ooephoi",
      {"master_gain": (-15.0, -10.0),
       "scale": ["JI Major (Ptolemy)", "JI 7-limit", "JI Pentatonic", "Otonality 1-11"],
       "purity": (0.97, 1.0), "brightness": (0.4, 0.75), "partials": ("int", 8, 18),
       "attack": ("log", 20.0, 55.0), "release": ("log", 40.0, 110.0),
       "decay": ("log", 10.0, 40.0), "sustain": (0.85, 0.98),
       "brain_density": ("int", 3, 5), "brain_rate": ("log", 60.0, 220.0),
       "brain_hold_min": ("log", 120.0, 400.0), "brain_hold_max": ("log", 300.0, 600.0),
       "drift": (1.0, 5.0), "drift_rate": ("log", 0.01, 0.06), "shimmer": (0.15, 0.45),
       "far_decay": ("log", 30.0, 80.0), "far_damp": (0.25, 0.55), "depth": (0.7, 1.0),
       "air": (0.15, 0.4), "ens_mix": (0.1, 0.4)},
      {"sub": 0.5, "zplane": 0.35, "cosmos": 0.25, "usertable": 0.3, "texture": 0.25, "feedback": 0.03,
       "cloud": 0.1},
      words=(["Temple", "Aeon", "Hymn", "Athanor", "Bardo", "Aureum", "Stone", "Nocturne", "Aether"],
             COMMON_SECOND + ["of Air", "Ascending", "Ceremony", "Stillness"]),
      prompts=["ancient stone temple drone, bowed metal and breath, timeless",
               "pure sustained harmonium chord in a vast crypt",
               "slow bowed cymbal wash, sacred and still",
               "glass bowl choir, immaculate and unmoving",
               "deep ceremonial breath tone, no vibrato"],
      tables=["Organ mixture", "Vowel choir", "Three vowels", "Formant beat", "Breath band"],
      granular={"spread": (0.003, 0.04), "grains": 1.5},
      impulses=['tuned', 'room_cathedral', 'shimmer'], noise=['Pink', 'Pink', 'Blue']),

    S("Vast Chord", "Mathias Grassow",
      {"scale": ["JI Major (Ptolemy)", "JI Minor", "JI 7-limit", "Pythagorean"],
       "partials": ("int", 16, 32), "brightness": (0.45, 0.8), "tilt": (0.8, 1.5),
       "strands": ("int", 4, 6), "detune": ("log", 6.0, 25.0), "stack": ["Major", "Minor", "Fifths", "Seventh"],
       "brain_density": ("int", 6, 10), "brain_consonance": (0.7, 1.0),
       "brain_rate": ("log", 40.0, 150.0), "brain_hold_min": ("log", 90.0, 300.0),
       "attack": ("log", 12.0, 40.0), "release": ("log", 25.0, 80.0),
       "far_decay": ("log", 25.0, 70.0), "ens_mix": (0.35, 0.8), "ens_depth": (0.3, 0.7),
       "depth": (0.6, 0.9), "sub_level": (0.15, 0.45)},
      {"stack": 0.9, "sub": 0.7, "zplane": 0.35, "cosmos": 0.2, "usertable": 0.3, "cloud": 0.1},
      words=(["Himavat", "Expansion", "Solitude", "Mercurial", "Cathedral", "Choir", "Immense", "Sanctum", "Hymnus"],
             COMMON_SECOND + ["Chord", "Wall", "Mass", "Continuum"]),
      prompts=["gigantic sustained just-intoned chord wall, warm and dense",
               "layered harmonium and choir drone, endlessly sustained",
               "immense consonant pad, no attack, no end",
               "thick overtone chord, slowly beating",
               "warm analog string mass, cathedral sized"],
      tables=["Vowel choir", "Three vowels", "Organ drawbars", "Formant beat", "Fifth stack"],
      granular={"spread": (0.004, 0.06), "grains": 1.6},
      impulses=['tuned', 'room_cathedral', 'room_hall'], noise=['Pink', 'Pink']),

    S("Desert Ember", "Steve Roach",
      {"brightness": (0.35, 0.7), "tilt": (1.0, 2.0), "partials": ("int", 10, 22),
       "cutoff": ("log", 600.0, 3000.0), "resonance": (0.1, 0.4), "filter_drift": (0.3, 0.7),
       "shimmer": (0.25, 0.6), "shimmer_rate": ("log", 0.04, 0.3),
       "ens_mix": (0.3, 0.7), "ens_rate": ("log", 0.08, 0.6),
       "dly_time_l": ("log", 0.3, 1.4), "dly_time_r": ("log", 0.45, 2.0), "dly_mix": (0.15, 0.4),
       "dly_feedback": (0.4, 0.8), "far_decay": ("log", 15.0, 45.0),
       "sub_level": (0.2, 0.5), "breath": (0.15, 0.55), "arc": (0.2, 0.6), "depth": (0.5, 0.85)},
      {"sub": 0.8, "delay2": 0.45, "zplane": 0.4, "cosmos": 0.25, "cloud": 0.3, "texture": 0.3,
       "usertable": 0.3, "coherence": 0.3},
      words=(["Dreamtime", "Structures", "Mojave", "Ember", "Slow", "Serpent", "Quiet", "Origin", "Cavern"],
             COMMON_SECOND + ["Return", "Pulse", "Circle", "Heat"]),
      prompts=["warm analog desert drone with slow pulses, organic",
               "didgeridoo-like low breath drone, earthy",
               "sun-bleached analog pad with echoing percussion tails",
               "slow tribal ambient bed, no beat, warm",
               "dry desert wind with distant flute"],
      tables=["Reed", "Breath band", "Tilt walk", "Bowed string", "Pluck point"],
      granular={"spread": (0.03, 0.35), "grains": 1.1},
      impulses=['room_hall', 'tuned', 'room_plate'], noise=['Brown', 'Pink', 'Wind']),

    S("Modular Nocturne", "Ian Boddy",
      {"brightness": (0.45, 0.85), "partials": ("int", 8, 20), "resonance": (0.2, 0.6),
       "cutoff": ("log", 700.0, 6000.0), "filter_env": (0.2, 0.7), "filter_drift": (0.3, 0.8),
       "dly_time_l": ("log", 0.2, 1.2), "dly_time_r": ("log", 0.3, 1.6), "dly_mix": (0.2, 0.45),
       "dly_feedback": (0.45, 0.85), "dly_cross": (0.3, 0.9),
       "brain_rate": ("log", 8.0, 40.0), "brain_density": ("int", 3, 6),
       "shimmer_rate": ("log", 0.08, 0.7), "ens_mix": (0.25, 0.6), "far_decay": ("log", 10.0, 35.0)},
      {"src2": 0.85, "src3": 0.5, "zplane": 0.6, "delay2": 0.5, "feedback": 0.2, "usertable": 0.4,
       "cosmos": 0.3, "cloud": 0.2, "coherence": 0.3},
      words=(["Nocturne", "Cerulean", "Voltage", "Patch", "Slow", "Nightfall", "Analogue", "Elemental", "Aeolian"],
             COMMON_SECOND + ["Sequence", "Patch", "Study", "Circuit"]),
      prompts=["analog modular drone with slow filter movement, night",
               "evolving synthesizer texture, resonant sweeps, no beat",
               "voltage controlled pad with echoing sequences",
               "cold analog bell tones in a long delay",
               "buchla-like burbling texture, sparse and dark"],
      tables=["Saw to square", "Harmonic gate", "PPG digital", "Bi-phase", "Comb"],
      granular={"spread": (0.05, 0.45), "grains": 1.0},
      impulses=['comb', 'reverse', 'room_plate'], noise=['White', 'Digital', 'Band']),

    S("Millstone", "Jonathan Coleclough",
      {"master_gain": (-16.0, -11.0),
       "brightness": (0.2, 0.5), "inharmonic": (0.25, 0.6), "tilt": (1.3, 2.5),
       "cutoff": ("log", 300.0, 1600.0), "brain_rate": ("log", 30.0, 120.0),
       "brain_density": ("int", 2, 5), "brain_consonance": (0.15, 0.55),
       "air": (0.15, 0.45), "air_q": ("log", 2.0, 8.0),
       "far_decay": ("log", 12.0, 40.0), "far_highcut": ("log", 1200.0, 3500.0),
       "sub_level": (0.2, 0.5), "depth": (0.6, 0.95)},
      {"cloud": 0.6, "texture": 0.5, "sub": 0.7, "zplane": 0.35, "feedback": 0.2, "src2": 0.7,
       "src3": 0.4, "room": 0.35},
      words=(["Millstone", "Husk", "Cane", "Torch", "Rain", "Period", "Windlass", "Mill", "Ash"],
             COMMON_SECOND + ["Wheel", "Grain", "Turn", "Work"]),
      prompts=["heavy stone grinding slowly, acoustic drone, dust",
               "bowed metal object in a stone room, resonant",
               "muffled mechanical rotation with granular debris",
               "burning wood crackle stretched into a drone",
               "low acoustic hum with rough granular surface"],
      tables=["Metal bar", "Gong wash", "Waterphone", "Prepared piano", "Spectral erosion"],
      granular={"spread": (0.25, 0.9), "grains": 1.5},
      impulses=['scatter', 'comb', 'modal'], noise=['Crackle', 'Brown', 'White']),

    S("Slow Carousel", "Mimir",
      {"brightness": (0.4, 0.75), "partials": ("int", 8, 18), "inharmonic": (0.1, 0.35),
       "shimmer": (0.25, 0.6), "shimmer_rate": ("log", 0.05, 0.35),
       "fb_tape": (0.35, 0.85), "ens_mix": (0.3, 0.7), "ens_depth": (0.3, 0.8), "ens_rate": ("log", 0.1, 0.8),
       "dly_mix": (0.2, 0.45), "dly_feedback": (0.45, 0.8), "dly_damp": (0.5, 0.9),
       "far_decay": ("log", 10.0, 35.0), "brain_rate": ("log", 10.0, 45.0),
       "drift": (4.0, 14.0), "drift_rate": ("log", 0.05, 0.3), "depth": (0.4, 0.8)},
      {"feedback": 0.6, "usertable": 0.45, "texture": 0.4, "cloud": 0.3, "zplane": 0.35,
       "delay2": 0.35, "cosmos": 0.2},
      words=(["Carousel", "Zither", "Bell", "Hazel", "Toy", "Waltz", "Faded", "Paper", "Lantern"],
             COMMON_SECOND + ["Loop", "Turn", "Song", "Memory"]),
      prompts=["warped music box loop soaked in tape hiss, nostalgic",
               "hazy looping zither phrase, degraded",
               "faded carousel organ, wobbling tape",
               "gentle bell loop under a blanket of hiss",
               "old reel to reel loop of a small ensemble"],
      tables=["Ring cluster", "Comb", "Beating pairs", "Bi-phase", "Ring bell"],
      granular={"spread": (0.008, 0.12), "grains": 1.2},
      impulses=['reverse', 'comb', 'room_plate'], noise=['Crackle', 'Pink']),

    S("Glass Vitrine", "Mirror",
      {"master_gain": (-16.0, -11.0),
       "brightness": (0.35, 0.7), "partials": ("int", 10, 22), "tilt": (1.0, 2.0),
       "air": (0.2, 0.55), "air_color": ("log", 3.0, 12.0), "air_q": ("log", 3.0, 12.0),
       "attack": ("log", 10.0, 40.0), "release": ("log", 20.0, 70.0),
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 40.0, 150.0),
       "far_decay": ("log", 20.0, 55.0), "far_damp": (0.35, 0.7), "ens_mix": (0.2, 0.6),
       "depth": (0.6, 0.95), "shimmer": (0.15, 0.45)},
      {"texture": 0.45, "cloud": 0.35, "zplane": 0.4, "sub": 0.4, "cosmos": 0.25, "usertable": 0.3},
      words=(["Vitrine", "Eye", "Nachtvlinder", "Etching", "Pale", "Glass", "Salon", "Ghost", "Cabinet"],
             COMMON_SECOND + ["Interior", "Room", "Curtain", "Hour"]),
      prompts=["ghostly harmonium under heavy tape hiss, faint strings",
               "pale string drone dissolving into noise",
               "distant orchestra heard through a wall, hazy",
               "hiss and faint bowed glass, spectral",
               "victorian parlour recording, degraded and ghostly"],
      tables=["Glass thinning", "Singing bowl", "Bowed cymbal", "Resonator bank", "Bell partials"],
      granular={"spread": (0.1, 0.6), "grains": 1.3},
      impulses=['reverse', 'shimmer', 'room_chamber'], noise=['Pink', 'Crackle', 'Pink']),

    S("Chamber Grey", "In Camera",
      {"master_gain": (-17.0, -12.0),
       "brightness": (0.25, 0.6), "partials": ("int", 8, 18), "tilt": (1.2, 2.2),
       "cutoff": ("log", 400.0, 2200.0), "near_mix": (0.2, 0.5), "near_decay": ("log", 0.6, 2.5),
       "far_level": (0.3, 0.7), "far_decay": ("log", 6.0, 25.0), "far_size": (0.8, 2.0),
       "depth": (0.3, 0.7), "brain_density": ("int", 2, 5), "brain_rate": ("log", 30.0, 120.0),
       "air": (0.15, 0.45), "shimmer": (0.1, 0.35)},
      {"room": 0.6, "texture": 0.45, "cloud": 0.3, "sub": 0.35, "zplane": 0.3, "cosmos": 0.1,
       "keys": 0.3},
      words=(["Chamber", "Interior", "Quiet", "Small", "Dim", "Grey", "Attic", "Study", "Window"],
             COMMON_SECOND + ["Room", "Corner", "Light", "Afternoon"]),
      prompts=["quiet room tone with faint acoustic resonance",
               "small dim chamber, soft sustained strings, close",
               "muted piano resonance in an empty flat",
               "close bowed object in a dry room, intimate",
               "gentle indoor hum with distant traffic"],
      tables=["Prepared piano", "Stretched string", "Pluck point", "Resonator bank", "Bowed string"],
      granular={"spread": (0.1, 0.5), "grains": 1.0},
      impulses=['room_chamber', 'room_bunker'], noise=['Pink', 'Pink', 'Crackle']),

    S("Painted Field", "Andrew Chalk",
      {"master_gain": (-16.0, -11.0),
       "brightness": (0.4, 0.75), "partials": ("int", 8, 20), "tilt": (0.9, 1.7),
       "attack": ("log", 12.0, 45.0), "release": ("log", 25.0, 80.0), "sustain": (0.8, 0.97),
       "drift": (2.0, 8.0), "drift_rate": ("log", 0.01, 0.08),
       "ens_mix": (0.3, 0.7), "far_decay": ("log", 18.0, 50.0), "far_damp": (0.4, 0.75),
       "air": (0.15, 0.45), "brain_density": ("int", 3, 6), "brain_consonance": (0.65, 0.95),
       "depth": (0.55, 0.9), "shimmer": (0.2, 0.5)},
      {"noiseprimary": 0.30, "usertable": 0.4, "texture": 0.4, "zplane": 0.3, "sub": 0.4, "cloud": 0.2, "cosmos": 0.15,
       "feedback": 0.08},
      words=(["Skyline", "Violin", "Ferry", "Cloud", "Wood", "Painted", "Fenn", "Sun", "Meadow"],
             COMMON_SECOND + ["Green", "Morning", "Water", "Light"]),
      prompts=["blurred harmonium wash, soft and warm, no edges",
               "sunlit organ drone through heavy tape",
               "gentle bowed strings smeared into a haze",
               "warm analog wash with soft hiss, pastoral",
               "slow melting chord, gauzy and bright"],
      tables=["Random walk", "Pink resonance", "Breath band", "Waterphone", "Tilt walk"],
      granular={"spread": (0.006, 0.1), "grains": 1.4},
      impulses=['shimmer', 'room_hall', 'tuned'], noise=['Pink', 'Pink']),

    S("Loop Studio", "Colin Potter",
      {"brightness": (0.35, 0.7), "partials": ("int", 8, 20), "inharmonic": (0.1, 0.4),
       "dly_time_l": ("log", 0.25, 1.5), "dly_time_r": ("log", 0.35, 2.2),
       "dly_feedback": (0.5, 0.88), "dly_mix": (0.2, 0.45), "dly_damp": (0.4, 0.85),
       "fb_tape": (0.2, 0.7), "ens_mix": (0.25, 0.6), "far_decay": ("log", 12.0, 40.0),
       "brain_rate": ("log", 10.0, 50.0), "depth": (0.45, 0.85)},
      {"delay2": 0.6, "feedback": 0.45, "texture": 0.4, "cloud": 0.35, "zplane": 0.4,
       "usertable": 0.35, "room": 0.25},
      words=(["Ic", "Tape", "Rehearsal", "Studio", "Loop", "Northampton", "Reel", "Splice", "Machine"],
             COMMON_SECOND + ["Session", "Take", "Echo", "Room"]),
      prompts=["long tape delay loop of a processed organ, dark",
               "echoing processed guitar drone, saturated tape",
               "layered loop of bowed metal, repeating and decaying",
               "old spring reverb over a slow synth loop",
               "reel to reel feedback loop, growing and dying"],
      tables=["Tilt walk", "Lo-fi bits", "PPG digital", "Comb", "Spectral erosion"],
      granular={"spread": (0.02, 0.25), "grains": 1.1},
      impulses=['comb', 'reverse', 'room_plate'], noise=['White', 'Crackle', 'Pink']),

    S("Ghost Signal", "Bass Communion",
      {"brightness": (0.3, 0.7), "inharmonic": (0.2, 0.55), "partials": ("int", 10, 26),
       "cutoff": ("log", 500.0, 4000.0), "spread": (0.7, 1.0), "width": (1.2, 1.8),
       "far_decay": ("log", 20.0, 60.0), "far_asym": (0.5, 1.0),
       "brain_consonance": (0.25, 0.7), "brain_rate": ("log", 15.0, 60.0),
       "sub_level": (0.2, 0.5), "depth": (0.6, 0.95), "shimmer": (0.2, 0.55)},
      {"noiseprimary": 0.50, "cloud": 0.65, "texture": 0.5, "sub": 0.7, "zplane": 0.5, "cosmos": 0.4, "feedback": 0.25,
       "src2": 0.7, "src3": 0.45},
      words=(["Ghost", "Molotov", "Haze", "Cenotaph", "Pacific", "Drift", "Litany", "Signal", "Atmospherics"],
             COMMON_SECOND + ["Transmission", "Static", "Smoke", "Wave"]),
      prompts=["heavily processed guitar drone, granular and wide",
               "shortwave radio static shaped into a chord",
               "smeared spectral cloud from a stringed instrument",
               "dark widescreen drone with grain and hiss",
               "reversed cymbal wash stretched to a minute"],
      tables=["Lo-fi bits", "PPG digital", "Pink resonance", "Spectral erosion", "Harmonic gate"],
      granular={"spread": (0.2, 0.9), "grains": 1.7},
      impulses=['scatter', 'spectral', 'comb'], noise=['White', 'Wind', 'Digital']),

    S("Sustain", "Paul Bradley",
      {"master_gain": (-15.0, -10.0),
       "brightness": (0.35, 0.7), "partials": ("int", 10, 24), "tilt": (1.0, 1.9),
       "attack": ("log", 20.0, 60.0), "release": ("log", 40.0, 110.0), "sustain": (0.88, 1.0),
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 90.0, 300.0),
       "brain_hold_min": ("log", 150.0, 450.0), "brain_hold_max": ("log", 300.0, 600.0),
       "drift": (1.0, 5.0), "drift_rate": ("log", 0.01, 0.05), "shimmer_rate": ("log", 0.01, 0.1),
       "far_decay": ("log", 30.0, 80.0), "spread": (0.7, 1.0), "width": (1.1, 1.7),
       "depth": (0.65, 0.95), "arc": (0.05, 0.35)},
      {"sub": 0.55, "zplane": 0.3, "usertable": 0.35, "texture": 0.3, "cosmos": 0.2, "cloud": 0.15,
       "feedback": 0.06},
      words=(["Sustain", "Chroma", "Liquid", "Ephemera", "Twenty", "Slow", "Sea", "Amber", "Long"],
             COMMON_SECOND + ["Sustain", "Hour", "Line", "Tone"]),
      prompts=["one long unchanging drone, wide and warm",
               "single sustained tone with slowly moving overtones",
               "minimal drone, no events, thirty minutes",
               "stretched cello note, endless",
               "smooth wide pad, imperceptible change"],
      tables=["Vowel choir", "Tilt walk", "Sub fold", "Stretched string", "Organ mixture"],
      granular={"spread": (0.003, 0.03), "grains": 1.5},
      impulses=['tuned', 'room_hall', 'shimmer'], noise=['Pink', 'Brown', 'Pink']),

    S("Hull Rumble", "SleepResearch_Facility",
      {"master_gain": (-14.0, -9.0),
       "brightness": (0.05, 0.25), "tilt": (2.0, 3.0), "partials": ("int", 4, 10),
       "cutoff": ("log", 150.0, 700.0), "far_highcut": ("log", 600.0, 1500.0),
       "far_decay": ("log", 25.0, 70.0), "far_damp": (0.6, 0.9),
       "brain_low": ("int", 24, 30), "brain_high": ("int", 38, 52), "brain_density": ("int", 2, 4),
       "brain_rate": ("log", 90.0, 300.0), "brain_hold_min": ("log", 150.0, 450.0),
       "sub_level": (0.4, 0.75), "sub_octave": ["-1", "-2"], "sub_tone": (0.0, 0.2),
       "bass_mono": ("log", 150.0, 300.0), "shimmer": (0.0, 0.15), "air": (0.0, 0.1),
       "drift": (0.5, 2.5), "ens_mix": (0.0, 0.2), "depth": (0.8, 1.0)},
      {"noiseprimary": 0.45, "sub": 1.0, "cloud": 0.3, "zplane": 0.2, "cosmos": 0.15, "feedback": 0.15, "texture": 0.3,
       "stack": 0.2},
      words=(["Nostromo", "Hull", "Deck", "Stasis", "Cargo", "Engine", "Vessel", "Bulkhead", "Dead"],
             COMMON_SECOND + ["Hum", "Watch", "Hold", "Sleep"]),
      prompts=["endless spaceship hull rumble, mechanical hum, motionless",
               "engine room drone deep below deck, steady",
               "low ventilation hum in a sealed corridor",
               "static machine bass with faint metallic ring",
               "submarine hull under pressure, deep and constant"],
      tables=["Sub fold", "Sub bloom", "Gong wash", "Pink resonance", "Metal bar"],
      granular={"spread": (0.004, 0.05), "grains": 1.1},
      impulses=['room_bunker', 'room_cavern', 'spectral'], noise=['Brown', 'Pink']),

    S("Corridor", "Kammarheit",
      {"master_gain": (-15.0, -10.0),
       "brightness": (0.15, 0.45), "inharmonic": (0.15, 0.45), "tilt": (1.5, 2.6),
       "cutoff": ("log", 300.0, 1600.0), "far_decay": ("log", 20.0, 55.0), "far_size": (1.8, 3.0),
       "far_highcut": ("log", 1200.0, 3000.0),
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 30.0, 120.0), "brain_consonance": (0.3, 0.7),
       "sub_level": (0.25, 0.55), "air": (0.05, 0.3), "depth": (0.7, 1.0),
       "near_mix": (0.1, 0.3), "shimmer": (0.1, 0.35)},
      {"sub": 0.8, "cloud": 0.4, "cosmos": 0.35, "zplane": 0.4, "room": 0.35, "texture": 0.4,
       "feedback": 0.15},
      words=(["Corridor", "Unearthed", "Vitrail", "Threshold", "Asylum", "Hollow", "Grim", "Sleepwalker", "Vault"],
             COMMON_SECOND + ["Corridor", "Door", "Stair", "Depth"]),
      prompts=["long dark corridor tone with distant metal impacts",
               "abandoned institution ambience, reverberant and cold",
               "dark ambient room with faint machinery below",
               "hollow reverberant drone, sparse and uneasy",
               "distant door slam tail in a huge concrete space"],
      tables=["Pink resonance", "Waterphone", "Bowed cymbal", "Resonator bank", "Random walk"],
      granular={"spread": (0.15, 0.7), "grains": 1.3},
      impulses=['room_cavern', 'room_bunker', 'comb', 'scatter'], noise=['Brown', 'Wind', 'Crackle']),

    S("Northern Dark", "Gustaf Hildebrand",
      {"brightness": (0.2, 0.5), "partials": ("int", 8, 20), "tilt": (1.3, 2.4),
       "cutoff": ("log", 350.0, 2000.0), "far_size": (2.0, 3.0), "far_decay": ("log", 25.0, 65.0),
       "sub_level": (0.35, 0.65), "sub_octave": ["-1", "-2"], "bass_mono": ("log", 120.0, 240.0),
       "width": (1.2, 1.8), "spread": (0.7, 1.0), "side_air": (1.5, 4.5),
       "brain_density": ("int", 3, 6), "brain_consonance": (0.35, 0.75),
       "depth": (0.7, 1.0), "arc": (0.2, 0.6), "shimmer": (0.15, 0.45)},
      {"sub": 0.95, "cosmos": 0.4, "zplane": 0.45, "cloud": 0.3, "stack": 0.4, "texture": 0.3,
       "delay2": 0.3},
      words=(["Nordheim", "Northern", "Ephemeral", "Ilmatar", "Aurora", "Cinder", "Winter", "Boreal", "Steel"],
             COMMON_SECOND + ["Sky", "Coast", "Storm", "Dusk"]),
      prompts=["cinematic dark ambient with deep sub bass swells",
               "northern winter drone, wide and cold",
               "vast icy atmosphere with slow bass pulses",
               "brooding orchestral drone, low and wide",
               "aurora shimmer over deep bass, cinematic"],
      tables=["Sub bloom", "Tilt walk", "Gong wash", "Spectral erosion", "Bell partials"],
      granular={"spread": (0.02, 0.3), "grains": 1.3},
      impulses=['room_cavern', 'room_cathedral', 'spectral'], noise=['Brown', 'Wind', 'Pink']),

    S("Void Station", "Tholen",
      {"brightness": (0.3, 0.65), "partials": ("int", 6, 16), "tilt": (1.0, 2.0),
       "inharmonic": (0.15, 0.45), "cutoff": ("log", 600.0, 4000.0),
       "z_rate": ("log", 0.008, 0.06), "far_decay": ("log", 20.0, 60.0),
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 40.0, 160.0), "brain_consonance": (0.2, 0.6),
       "sub_level": (0.2, 0.5), "air": (0.1, 0.35), "depth": (0.7, 1.0),
       "scale": ["Bohlen-Pierce (JI)", "Subharmonic 16-8", "JI 7-limit", "Slendro (JI)"]},
      {"noiseprimary": 0.30, "zplane": 0.75, "cosmos": 0.5, "sub": 0.7, "cloud": 0.3, "src2": 0.6, "src3": 0.3,
       "texture": 0.3, "coherence": 0.3},
      words=(["Void", "Station", "Derelict", "Cygnus", "Orbit", "Hollow", "Signal", "Cold", "Drift"],
             COMMON_SECOND + ["Station", "Vector", "Silence", "Node"]),
      prompts=["cold science fiction drone, thin high tone over deep hum",
               "derelict space station ambience, sparse metallic clicks",
               "slowly sweeping filtered drone, alien and empty",
               "vacuum hum with faint radio artefacts",
               "deep space monitoring room, quiet and tense"],
      tables=["Shepard stack", "Harmonic gate", "Bi-phase", "PPG digital", "Bohlen-Pierce"],
      granular={"spread": (0.1, 0.6), "grains": 1.2},
      impulses=['comb', 'spectral', 'modal'], noise=['Band', 'Digital', 'Wind']),

    S("Strings at Rest", "Stars of the Lid",
      {"master_gain": (-15.0, -10.0),
       "scale": ["JI Major (Ptolemy)", "JI Minor", "JI Pentatonic", "Pythagorean"],
       "brightness": (0.4, 0.75), "partials": ("int", 10, 24), "tilt": (0.9, 1.7),
       "odd_even": (-0.6, 0.0), "attack": ("log", 8.0, 30.0), "release": ("log", 20.0, 70.0),
       "brain_density": ("int", 3, 6), "brain_consonance": (0.75, 1.0), "brain_wander": (0.05, 0.3),
       "brain_rate": ("log", 30.0, 120.0), "ens_mix": (0.3, 0.7), "ens_depth": (0.2, 0.5),
       "far_decay": ("log", 15.0, 45.0), "far_damp": (0.4, 0.75), "depth": (0.5, 0.85),
       "shimmer": (0.15, 0.45), "purity": (0.9, 1.0)},
      {"usertable": 0.4, "zplane": 0.35, "sub": 0.45, "texture": 0.3, "cosmos": 0.15, "cloud": 0.12,
       "stack": 0.5},
      words=(["Requiem", "Ballad", "Articulate", "Apreludes", "Tape", "Lull", "December", "Elegy", "Adagio"],
             COMMON_SECOND + ["for Dying", "Movement", "Rest", "Sleep"]),
      prompts=["slow bowed string swell, mournful and consonant",
               "orchestral strings sustained and processed into a drone",
               "warm cello and viola chord, endlessly held",
               "soft brass and string pad, elegiac",
               "gentle string quartet stretched into ambience"],
      tables=["Bowed string", "Stretched string", "Three vowels", "Formant beat", "Fifth stack"],
      granular={"spread": (0.004, 0.06), "grains": 1.5},
      impulses=['room_hall', 'tuned', 'shimmer'], noise=['Pink', 'Pink']),

    S("Tape Saturation", "Tim Hecker",
      {"master_gain": (-13.0, -9.0),
       "brightness": (0.5, 0.9), "inharmonic": (0.2, 0.55), "partials": ("int", 12, 28),
       "cutoff": ("log", 900.0, 8000.0), "resonance": (0.15, 0.5),
       "fb_bus": (0.06, 0.22), "fb_drive": (0.55, 1.0), "fb_tone": ("log", 800.0, 5000.0),
       "fb_tape": (0.35, 0.9), "ens_mix": (0.3, 0.75), "ens_depth": (0.3, 0.8),
       "dly_feedback": (0.5, 0.85), "dly_mix": (0.2, 0.45),
       "brain_rate": ("log", 8.0, 40.0), "brain_consonance": (0.35, 0.8),
       "far_decay": ("log", 12.0, 40.0), "width": (1.1, 1.8), "depth": (0.4, 0.8)},
      {"noiseprimary": 0.35, "feedback": 1.0, "cloud": 0.45, "zplane": 0.5, "texture": 0.4, "usertable": 0.4,
       "cosmos": 0.3, "delay2": 0.35},
      words=(["Harmony", "Ravedeath", "Virgins", "Radio", "Amps", "Hatred", "Music", "Analog", "Chimeras"],
             COMMON_SECOND + ["Decay", "Ruin", "Sunburn", "Wash"]),
      prompts=["distorted pipe organ chord drowned in tape saturation",
               "blown out analog synth wash, bright and corroded",
               "overdriven harmonium loop, beautiful and damaged",
               "clipped orchestral swell smeared into noise",
               "loud degraded drone, bright saturated haze"],
      tables=["Lo-fi bits", "PPG digital", "Pink resonance", "Tilt walk", "Comb"],
      granular={"spread": (0.03, 0.4), "grains": 1.4},
      impulses=['comb', 'scatter', 'modal', 'room_plate'], noise=['White', 'Violet', 'Digital']),

    # ------------------------------------------------------------------ the five later packs
    #
    # The twenty-five above were written against an earlier instrument. These five exist for the
    # parts that came after them: the modal resonator bank, three source slots of the same kind,
    # the conductor exchanging one voice at a time, the hundred and fifty-five filter shapes, and
    # a modulation matrix fed by the instrument's own tuning.

    S("Struck Bodies", "Bernhard Guenter",
      {"scale": ["JI 7-limit", "JI Minor", "Slendro (JI)", "JI Pentatonic"],
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 12.0, 45.0),
       "brain_hold_min": ("log", 20.0, 60.0), "brain_hold_max": ("log", 60.0, 200.0),
       "attack": ("log", 0.05, 1.5), "release": ("log", 6.0, 30.0),
       "strike_level": (0.3, 0.7), "strike_decay": ("log", 0.3, 3.0), "strike_damp": (0.1, 0.7),
       "strike_type": ["String", "Wood", "Metal"], "strike_who": ["Keys + Brain"],
       "partials": ("int", 6, 16), "brightness": (0.3, 0.65), "inharmonic": (0.05, 0.35),
       "far_decay": ("log", 12.0, 40.0), "depth": (0.5, 0.85),
       "body_level": (0.2, 0.6), "body_decay": ("log", 2.0, 10.0)},
      {"zplane": 1.0, "sub": 0.35, "texture": 0.15, "cosmos": 0.1, "room": 0.6,
       "strike": 1.0, "src2": 0.35, "src3": 0.15, "cloud": 0.08},
      words=(["Struck", "Bell", "Bar", "Anvil", "Temple", "Wooden", "Iron", "Bronze", "Kalimba", "Gamelan"],
             COMMON_SECOND + ["Body", "Frame", "Course", "Vigil"]),
      prompts=["struck metal bar ringing in a stone room, very slow decay",
               "wooden bars and soft mallets, sparse, breathing",
               "distant temple bells over a low drone"],
      tables=["Prepared piano", "Metal bar", "Bell partials", "Waterphone", "Ring bell"],
      granular={"spread": (0.01, 0.15), "grains": 1.0},
      impulses=['modal', 'tuned', 'room_chamber'], noise=['Brown', 'Pink']),

    S("Three Alike", "Eliane Radigue",
      {"scale": ["JI 7-limit", "Otonality 1-11", "JI Major (Ptolemy)"],
       "brain_density": ("int", 3, 6), "brain_rate": ("log", 40.0, 140.0),
       "attack": ("log", 15.0, 50.0), "release": ("log", 30.0, 90.0),
       "partials": ("int", 10, 28), "tilt": (1.0, 2.4), "odd_even": (-0.7, -0.1),
       "brightness": (0.35, 0.7), "drift": (2.0, 12.0), "detune": ("log", 2.0, 9.0),
       "far_decay": ("log", 25.0, 70.0), "depth": (0.75, 0.98), "purity": (0.9, 1.0),
       "arc": (0.4, 0.8), "arc_period": ("log", 90.0, 300.0)},
      {"src2": 1.0, "src3": 1.0, "zplane": 0.45, "sub": 0.5, "stack": 0.5,
       "coherence": 0.6, "texture": 0.2, "usertable": 0.3, "portamento": 0.2},
      words=(["Triple", "Three", "Trine", "Parallel", "Braided", "Thrice", "Layered", "Threefold"],
             COMMON_SECOND + ["Alike", "Strand", "Chorus", "Weave"]),
      prompts=["three identical drones a just fifth apart, very slow",
               "layered sine banks beating against each other, endless",
               "triple organ drone, no attack, no end"],
      tables=["Fifth stack", "Prime sieve", "Sub fold", "Beating pairs", "Formant beat"],
      granular={"spread": (0.006, 0.08), "grains": 1.4},
      impulses=['room_hall', 'room_cathedral', 'tuned'], noise=['Pink', 'Brown', 'Pink']),

    S("Turning Harmony", "Pauline Oliveros",
      {"scale": ["JI 7-limit", "JI Major (Ptolemy)", "JI Minor", "Pythagorean"],
       "brain_density": ("int", 4, 7), "brain_rate": ("log", 30.0, 90.0),
       "attack": ("log", 10.0, 40.0), "release": ("log", 20.0, 70.0),
       "auto_mode": ["Chords"], "auto_rate": ("log", 20.0, 120.0),
       "auto_lead": (2.0, 9.0), "auto_tension": (0.05, 0.6), "auto_root_move": (0.1, 0.7),
       "brightness": (0.4, 0.75), "purity": (0.92, 1.0), "depth": (0.6, 0.9),
       "far_decay": ("log", 20.0, 60.0), "sub_level": (0.1, 0.35)},
      {"zplane": 0.5, "sub": 0.7, "cosmos": 0.25, "room": 0.5, "coherence": 0.4,
       "src2": 0.5, "texture": 0.2, "delay2": 0.25},
      words=(["Turning", "Shifting", "Slow", "Patient", "Wandering", "Circling", "Drifting", "Moving"],
             COMMON_SECOND + ["Harmony", "Progression", "Chord", "Cadence"]),
      prompts=["a chord that changes one note at a time over minutes",
               "slow just-intoned progression, voices exchanged one by one",
               "deep sustained harmony, never repeating"],
      tables=["Singing bowl", "Beating pairs", "Formant beat", "Fifth stack", "Three vowels"],
      granular={"spread": (0.01, 0.2), "grains": 1.2},
      impulses=['room_cathedral', 'room_hall', 'tuned'], noise=['Pink', 'Brown']),

    S("Filter Cubes", "Alva Noto",
      {"scale": ["12-TET", "JI 7-limit", "Bohlen-Pierce (JI)", "Harmonic 8-16"],
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 15.0, 60.0),
       "attack": ("log", 2.0, 20.0), "release": ("log", 8.0, 40.0),
       "partials": ("int", 8, 32), "tilt": (0.6, 1.6), "brightness": (0.5, 0.9),
       "cutoff": ("log", 300.0, 6000.0), "resonance": (0.2, 0.7),
       "z_rate": ("log", 0.01, 0.2), "z_depth": (0.4, 1.0), "z_res": (0.35, 0.9),
       "depth": (0.4, 0.8), "far_decay": ("log", 8.0, 35.0)},
      {"zplane": 1.0, "filtermodel": 0.9, "src2": 0.4, "src3": 0.25, "cosmos": 0.3,
       "cloud": 0.15, "feedback": 0.15, "delay2": 0.35},
      words=(["Cube", "Vertex", "Lattice", "Corner", "Facet", "Prism", "Grid", "Axis", "Morph"],
             COMMON_SECOND + ["Cube", "Face", "Edge", "Transform"]),
      prompts=["a filter sweeping through impossible shapes over a cold drone",
               "resonant formant structures morphing slowly, metallic",
               "precise digital drone with a moving resonance"],
      tables=["PPG digital", "Lo-fi bits", "Harmonic gate", "Bi-phase", "Saw to square"],
      granular={"spread": (0.02, 0.3), "grains": 1.1},
      impulses=['modal', 'comb', 'spectral', 'room_plate'], noise=['White', 'Pink', 'Digital']),

    S("Own Tuning", "Catherine Christer Hennix",
      {"scale": ["JI 7-limit", "Otonality 1-11", "Subharmonic 16-8", "JI Major (Ptolemy)"],
       "brain_density": ("int", 5, 8), "brain_rate": ("log", 45.0, 180.0),
       "brain_hold_min": ("log", 90.0, 300.0), "brain_hold_max": ("log", 300.0, 900.0),
       "attack": ("log", 20.0, 60.0), "release": ("log", 40.0, 120.0),
       "purity": (0.85, 1.0), "purity_drift": (0.25, 0.9), "purity_rate": ("log", 0.004, 0.04),
       "partials": ("int", 12, 32), "tilt": (0.8, 2.0), "odd_even": (-0.6, 0.1),
       "brightness": (0.4, 0.8), "depth": (0.7, 0.95), "far_decay": ("log", 30.0, 80.0),
       "sub_level": (0.15, 0.45), "sub_source": ["Difference"]},
      {"sub": 0.95, "zplane": 0.55, "coherence": 0.7, "cosmos": 0.3, "src2": 0.6,
       "src3": 0.35, "stack": 0.6, "texture": 0.2, "room": 0.4},
      words=(["Own", "Inner", "Friction", "Beating", "Ratio", "Comma", "Drifting", "Tuning", "Intonation"],
             COMMON_SECOND + ["Tuning", "Ratio", "Beat", "Comma"]),
      prompts=["a just-intoned drone slowly losing and finding its tuning",
               "sustained harmonium chord with audible beating between partials",
               "endless chord breathing in time with its own mistuning"],
      tables=["Prime sieve", "Bohlen-Pierce", "Stretched octave", "Fifth stack", "Fibonacci"],
      granular={"spread": (0.005, 0.09), "grains": 1.3},
      impulses=['tuned', 'room_cathedral', 'modal'], noise=['Pink', 'Brown', 'Pink']),

    # ------------------------------------------------------------------ the field recordings
    #
    # Places rather than instruments: the five hundred seamless clips from
    # make_field_recordings.py, read by the Stretch type as weather that never repeats, with the
    # additive bank underneath as the tonal centre a field recording does not have. Up to four
    # sources, so a preset can be four landscapes in the Vector's four corners.
    S("Field Recordings", "Chris Watson",
      {"scale": ["JI 7-limit", "JI Minor", "Slendro (JI)", "12-TET"],
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 30.0, 120.0),
       "brain_hold_min": ("log", 60.0, 200.0), "brain_hold_max": ("log", 200.0, 600.0),
       "attack": ("log", 12.0, 45.0), "release": ("log", 25.0, 90.0),
       "osc_level": (0.2, 0.5), "partials": ("int", 4, 12), "tilt": (1.4, 2.4),
       "brightness": (0.25, 0.55), "cutoff": ("log", 400.0, 3000.0),
       "depth": (0.6, 0.95), "far_decay": ("log", 15.0, 60.0), "far_lowcut": ("log", 120.0, 400.0),
       # Quieter sources than the other styles (a stretched recording has no attack to carry it),
       # so the gain sits higher; measure_packs trims the rest to the common window.
       "air": (0.0, 0.08), "sub_level": (0.1, 0.35), "master_gain": (-8.0, -3.0)},
      {"stretch": 1.0, "src2": 0.95, "src3": 0.7, "src4": 0.45, "texture": 0.0, "usertable": 0.05,
       "zplane": 0.3, "sub": 0.5, "cosmos": 0.15, "room": 0.5, "cloud": 0.1, "coherence": 0.1},
      words=(["Rain", "Harbour", "Cave", "Moor", "Ice", "Distant", "Night", "Wind", "Forest", "Roof", "Shore", "Fog"],
             COMMON_SECOND + ["Weather", "Country", "Hour", "Field"]),
      prompts=["steady rain on a tin roof, far thunder",
               "a harbour in fog at night",
               "wind over a high moor"],
      tables=["Pink resonance", "Breath band", "Random walk", "Waterphone", "Spectral erosion"],
      granular={"spread": (0.02, 0.2), "grains": 1.0},
      impulses=['room_cathedral', 'room_hall', 'room_chamber'], noise=['Pink', 'Brown', 'Pink']),

    # The thirty-second pack, written for the mixing desk this instrument grew: the background
    # narrowed as it goes back, the foreground's own upper middle opened by the Haas band, the
    # Ensemble as a static detune rather than a chorus, the wavefolder where a saturation used to
    # be -- and the convolution room loaded with a struck object instead of a hall, so the pad is
    # played on a piece of the world rather than in a room.
    S("Cryo Chamber", "Atrium Carceri",
      {"scale": ["JI Minor", "JI 7-limit", "Pythagorean", "12-TET"],
       "brain_density": ("int", 3, 6), "brain_rate": ("log", 25.0, 110.0),
       "brain_hold_min": ("log", 50.0, 180.0), "brain_hold_max": ("log", 180.0, 520.0),
       "attack": ("log", 8.0, 35.0), "release": ("log", 20.0, 80.0),
       "osc_level": (0.25, 0.6), "partials": ("int", 5, 16), "tilt": (1.3, 2.3),
       "brightness": (0.2, 0.5), "cutoff": ("log", 300.0, 2600.0),
       "depth": (0.55, 0.95), "keys_depth": (0.3, 0.8),
       "far_decay": ("log", 12.0, 50.0), "far_lowcut": ("log", 140.0, 420.0),
       "far_highcut": ("log", 2200.0, 9000.0), "near_mix": (0.1, 0.4),
       "room_level": (0.25, 0.7), "room_lowcut": ("log", 90.0, 260.0),
       "sub_level": (0.2, 0.5), "sub_tone": (0.0, 0.35), "bass_mono": ("log", 110.0, 190.0),
       "air": (0.0, 0.12), "master_gain": (-9.0, -4.0), "subsonic": ("log", 18.0, 30.0)},
      # Everything the desk has, and the field recordings underneath it.
      {"narrow": 1.0, "haas": 0.8, "microshift": 0.75, "fold": 0.35, "hands": 0.9,
       "stretch": 0.55, "src2": 0.9, "src3": 0.6, "src4": 0.3, "texture": 0.25,
       "room": 0.95, "zplane": 0.4, "sub": 0.75, "cosmos": 0.25, "cloud": 0.15,
       "strike": 0.25, "delay2": 0.35, "absorb": 0.6, "rotate": 0.5, "tide": 0.35},
      words=(["Cryo", "Vault", "Ossuary", "Concrete", "Relic", "Sunken", "Iron", "Ash",
              "Chamber", "Derelict", "Hangar", "Cistern", "Foundry", "Bell"],
             COMMON_SECOND + ["Chamber", "Vault", "Hull", "Works", "Station"]),
      prompts=["a flooded concrete cistern, distant machinery",
               "an abandoned hangar in wind, iron settling",
               "a bell struck once in a stone vault"],
      tables=["Gong wash", "Metal bar", "Sub bloom", "Ring bell", "Resonator bank"],
      granular={"spread": (0.02, 0.25), "grains": 1.0},
      impulses=['struck', 'room_bunker', 'room_cavern', 'modal'],
      noise=['Brown', 'Pink', 'Pink']),

    # ---- the two packs built on the newest source types -----------------------------------
    #
    # These two are here because a source type nobody has heard is a source type nobody uses.
    # Everything else in the library was voiced before the bowed string and the spectral model
    # existed, and none of it was touched: these are the thirty-third and thirty-fourth packs,
    # and the other thirty-two come out of the generator byte for byte as they did.

    S("Rosin and Wire", "the bowed-string tradition, from Tony Conrad to Ellen Fullman",
      {"attack": ("log", 2.0, 9.0), "release": ("log", 6.0, 22.0),
       "cutoff": ("log", 700.0, 5000.0), "resonance": (0.05, 0.35),
       "detune": (3.0, 14.0), "drift": (0.2, 0.7), "drift_rate": ("log", 0.02, 0.12),
       "near_mix": (0.15, 0.45), "near_size": (0.3, 0.7),
       "far_level": (0.3, 0.7), "far_decay": ("log", 5.0, 24.0), "far_predelay": (20.0, 90.0),
       "far_highcut": ("log", 2400.0, 9000.0),
       "early_level": (0.15, 0.5), "early_size": (5.0, 16.0), "early_absorb": (0.2, 0.55),
       "ens_mix": (0.1, 0.4), "delay_mix": (0.0, 0.25),
       "sub_level": (0.1, 0.35), "air": (0.02, 0.16),
       "master_gain": (-9.0, -5.0), "stretch": (0.0, 4.0)},
      # Bow in most slots, and the early room on: a string is an object in a place.
      {"bow": 0.7, "src2": 0.9, "src3": 0.6, "src4": 0.25, "texture": 0.1, "stretch": 0.0,
       "room": 0.5, "zplane": 0.3, "sub": 0.5, "strike": 0.2, "hands": 0.9,
       "phase": 0.5, "microshift": 0.35, "narrow": 0.35, "coherence": 0.3, "absorb": 0.4},
      words=(["Rosin", "Horsehair", "Sul", "Tasto", "Ponticello", "Arco", "Gut", "Wire",
              "Bridge", "Nut", "Bowed", "Long", "Drawn", "Sustained"],
             COMMON_SECOND + ["String", "Wire", "Bow", "Bridge", "Drone"]),
      prompts=["a bowed double bass held for a minute, rosin audible",
               "a bowed metal wire stretched across a hall",
               "two bowed strings a fifth apart, beating slowly"],
      tables=["Bowed string", "Bowed cymbal", "Stretched string", "Beating pairs", "Resonator bank"],
      impulses=['room_hall', 'room_chamber', 'modal', 'struck'],
      noise=['Pink', 'Brown', 'Wind']),

    S("Held Moment", "the frozen spectrum, after Eliane Radigue and Thomas Koner",
      {"attack": ("log", 4.0, 14.0), "release": ("log", 10.0, 30.0),
       "cutoff": ("log", 400.0, 4000.0), "resonance": (0.0, 0.25),
       "detune": (2.0, 9.0), "drift": (0.15, 0.6),
       "near_mix": (0.05, 0.3),
       "far_level": (0.4, 0.85), "far_decay": ("log", 12.0, 55.0), "far_predelay": (30.0, 140.0),
       "far_highcut": ("log", 1400.0, 6000.0), "far_width": (0.35, 0.9),
       "far_unmask": (0.1, 0.5), "far_unmask_spread": (0.2, 0.8),
       "early_level": (0.0, 0.3), "early_size": (8.0, 26.0),
       "ens_mix": (0.15, 0.5), "ens_mode": ["Velvet", "Velvet", "Microshift"],
       "delay_mix": (0.05, 0.3), "sub_level": (0.15, 0.45),
       "air": (0.0, 0.1), "master_gain": (-10.0, -5.0)},
      # Spectral in nearly every slot: the whole point of the pack is a recording held still.
      {"spectral": 0.85, "src2": 0.95, "src3": 0.75, "src4": 0.4,
       "texture": 0.05, "stretch": 0.0, "usertable": 0.05,
       "room": 0.6, "zplane": 0.45, "sub": 0.6, "cosmos": 0.3, "cloud": 0.2,
       "hands": 0.85, "phase": 0.6, "narrow": 0.55, "blur": 0.25, "tide": 0.4, "absorb": 0.6},
      words=(["Held", "Suspended", "Arrested", "Still", "Standing", "Frozen", "Motionless",
              "Hovering", "Fixed", "Poised", "Lingering", "Dwelling", "Endless", "Unmoving"],
             COMMON_SECOND + ["Moment", "Instant", "Second", "Frame", "Breath"]),
      prompts=["a single second of a cathedral held for an hour",
               "the spectrum of rain, stopped and sustained",
               "one frame of a choir, frozen and transposed"],
      tables=["Resonator bank", "Spectral erosion", "Stretched string", "Singing bowl", "Harmonic gate"],
      impulses=['room_hall', 'room_cavern', 'spectral'],
      noise=['Pink', 'Brown', 'Pink', 'Wind']),

    # ---------------------------------------------------------------- five packs for what the
    # library never used. Each one is built around a family of the instrument that appeared in
    # none of its 6800 presets, because the library was generated before those existed. They are
    # not demonstrations: a pack is two hundred pieces of music that happen to lean on one thing.

    S("Cascade", "the Hawkes clock: events that breed events, after Koelsch, Vuust and Friston",
      {"brain_rate": ("log", 6.0, 40.0), "brain_density": ("int", 3, 8),
       "brain_hold_min": ("log", 12.0, 60.0), "brain_hold_max": ("log", 60.0, 240.0),
       "attack": ("log", 1.5, 12.0), "release": ("log", 6.0, 30.0),
       "strike_level": (0.25, 0.7), "strike_decay": ("log", 0.15, 2.0), "strike_damp": (0.1, 0.8),
       "far_decay": ("log", 8.0, 40.0), "far_size": (1.8, 3.0),
       "depth": (0.5, 0.9), "keys_depth": (0.0, 0.3), "air": (0.05, 0.3),
       "master_gain": (-13.0, -8.0)},
      # Everything about this pack is the clock: the cascade always, the strike thinned and
      # clustered with it, the Cosmos swelling where the events crowd.
      {"cascade": 1.0, "surprise": 0.6, "spreadbias": 0.7, "dejavu": 0.45, "quantize": 0.2,
       "strike": 1.0, "cosmos": 0.55, "cosmosswell": 0.9, "slot1": 0.5, "brain2": 0.3,
       "src2": 0.7, "src3": 0.35, "texture": 0.35, "usertable": 0.35, "zplane": 0.4,
       "room": 0.4, "sub": 0.5, "hands": 0.7, "lenia": 0.2, "chaos": 0.2},
      words=(["Cascade", "Chain", "Branching", "Aftershock", "Trigger", "Ripple", "Relay",
              "Sequence", "Volley", "Flurry", "Onset", "Break", "Rush", "Swarm"],
             COMMON_SECOND + ["Cluster", "Handful", "Burst", "Chain", "Silence"]),
      prompts=["distant thunder that answers itself", "a swarm of small bells, then nothing",
               "stones dropped into a cistern at irregular intervals"],
      tables=["Pluck point", "Prepared piano", "Ring cluster", "Beating pairs", "Fibonacci"],
      impulses=['room_hall', 'room_plate', 'tuned'],
      noise=['Crackle', 'Digital', 'Pink', 'Band']),

    S("True Intonation", "adaptive just intonation, after Hermode and Sethares",
      {"scale": ["JI 7-limit", "JI Major (Ptolemy)", "JI Minor", "Otonality 1-11", "Harmonic 8-16",
                 "Pythagorean", "Timbre (Sethares)"],
       "purity": (0.9, 1.0), "purity_drift": (0.0, 0.35), "purity_rate": ("log", 0.004, 0.05),
       "brain_density": ("int", 3, 6), "brain_rate": ("log", 20.0, 90.0),
       "brain_consonance": (0.4, 0.85), "brain_hold_min": ("log", 30.0, 120.0),
       "brain_hold_max": ("log", 120.0, 420.0),
       "partials": ("int", 10, 28), "inharmonic": (0.0, 0.35), "detune": ("log", 1.0, 8.0),
       "attack": ("log", 4.0, 20.0), "release": ("log", 10.0, 40.0),
       "far_decay": ("log", 15.0, 50.0), "master_gain": (-13.0, -8.0)},
      {"adaptive": 0.95, "guard": 0.8, "match": 0.55, "timbre": 0.7, "keyfind": 0.7,
       "evensmooth": 0.6, "blend": 0.5, "transpose": 0.25, "slot1": 0.3,
       "src2": 0.6, "src3": 0.3, "stack": 0.6, "sub": 0.6, "zplane": 0.25,
       "cosmos": 0.15, "room": 0.45, "hands": 0.6, "partialspread": 0.4},
      words=(["Pure", "Comma", "Just", "Untempered", "Ratio", "Septimal", "Syntonic", "Otonal",
              "Undertone", "Beatless", "Lattice", "Whole", "Exact", "Drifting"],
             COMMON_SECOND + ["Interval", "Ratio", "Comma", "Lattice", "Consonance"]),
      prompts=["a choir tuning itself to a bell", "beatless fifths in a stone room",
               "an organ in seventh-limit intonation"],
      tables=["Fifth stack", "Prime sieve", "Stretched octave", "Organ mixture", "Fibonacci"],
      impulses=['room_cathedral', 'room_hall', 'tuned'],
      noise=['Pink', 'Pink', 'Brown']),

    S("Lenia Fields", "continuous cellular automata, after Bert Chan",
      {"brain_rate": ("log", 20.0, 120.0), "brain_density": ("int", 3, 7),
       "attack": ("log", 6.0, 25.0), "release": ("log", 12.0, 45.0),
       "shimmer": (0.3, 0.8), "shimmer_rate": ("log", 0.02, 0.2),
       "cloud_send": (0.15, 0.6), "cloud_density": ("log", 2.0, 14.0),
       "far_decay": ("log", 20.0, 60.0), "far_size": (2.2, 3.0),
       "depth": (0.6, 0.95), "width": (1.0, 1.5), "master_gain": (-13.0, -8.0)},
      # The field is the piece: its four readings drive whatever the preset has.
      {"lenia": 1.0, "chaos": 0.45, "sympathy": 0.5, "coherence": 0.6,
       "slot1": 0.55, "src2": 0.75, "src3": 0.45, "src4": 0.2,
       "cloud": 0.7, "texture": 0.4, "usertable": 0.4, "zplane": 0.5, "cosmos": 0.4,
       "room": 0.35, "sub": 0.45, "hands": 0.6, "comod": 0.4, "envelop": 0.4},
      words=(["Lenia", "Colony", "Bloom", "Culture", "Membrane", "Orbium", "Spore", "Drifting",
              "Dividing", "Living", "Cell", "Tissue", "Growth", "Pulsing"],
             COMMON_SECOND + ["Field", "Colony", "Membrane", "Culture", "Bloom"]),
      prompts=["something alive under a microscope, breathing",
               "a colony of small organisms drifting and dividing",
               "warm liquid seen through glass, slowly moving"],
      tables=["Random walk", "Waterphone", "Ring cluster", "Fibonacci", "Spectral erosion"],
      impulses=['room_cavern', 'spectral', 'room_hall'],
      noise=['Pink', 'Wind', 'Band', 'Pink']),

    S("Strange Attractor", "Lorenz and Roessler, integrated over minutes",
      {"brain_rate": ("log", 25.0, 150.0), "brain_density": ("int", 2, 6),
       "brain_wander": (0.2, 0.7), "brain_hold_min": ("log", 40.0, 160.0),
       "brain_hold_max": ("log", 160.0, 520.0),
       "attack": ("log", 5.0, 25.0), "release": ("log", 15.0, 60.0),
       "arc": (0.3, 0.8), "arc_period": ("log", 30.0, 180.0),
       "far_decay": ("log", 25.0, 70.0), "tide": (0.0, 8.0), "tide_period": ("log", 4.0, 20.0),
       "master_gain": (-13.0, -8.0)},
      {"chaos": 1.0, "lenia": 0.35, "archarmony": 0.5, "spreadbias": 0.5, "dejavu": 0.3,
       "slot1": 0.5, "src2": 0.7, "src3": 0.4, "zplane": 0.55, "cosmos": 0.45,
       "cloud": 0.3, "room": 0.4, "sub": 0.5, "hands": 0.6, "farmode": 0.6, "rotate": 0.6},
      words=(["Lorenz", "Roessler", "Attractor", "Lobe", "Trajectory", "Orbit", "Divergent",
              "Unrepeating", "Sensitive", "Wandering", "Butterfly", "Phase", "Strange", "Chaotic"],
             COMMON_SECOND + ["Orbit", "Lobe", "Trajectory", "Phase", "Attractor"]),
      prompts=["a path that never crosses itself", "weather over a long night",
               "a slow spiral that suddenly climbs"],
      tables=["Random walk", "Bohlen-Pierce", "Ring cluster", "Shepard stack", "Beating pairs"],
      impulses=['room_hall', 'room_cavern', 'tuned'],
      noise=['Brown', 'Pink', 'Violet', 'Wind']),

    S("Within Reach", "the near field: sound closer than a metre",
      {"depth": (0.05, 0.4), "keys_depth": (0.0, 0.25), "presence": (1.0, 4.0),
       "near_mix": (0.15, 0.5), "near_decay": ("log", 0.5, 3.0),
       "far_level": (0.1, 0.45), "far_decay": ("log", 6.0, 25.0),
       "attack": ("log", 0.5, 8.0), "release": ("log", 4.0, 20.0),
       "strike_level": (0.15, 0.55), "strike_decay": ("log", 0.1, 1.2),
       "air": (0.15, 0.45), "width": (0.9, 1.35), "master_gain": (-12.0, -7.0)},
      # Everything that says "an arm's length away": the near-field level difference, the elevation
      # of the planes, the head model, and a strike that is touched now and then rather than played.
      {"nearfield": 0.95, "elev": 0.7, "binaural": 0.6, "presence": 0.8, "depthlaw": 0.7,
       "earlyroom": 0.65, "strike": 0.7, "slot1": 0.6, "src2": 0.7, "src3": 0.3,
       "texture": 0.45, "bow": 0.35, "usertable": 0.4, "zplane": 0.35,
       "room": 0.5, "sub": 0.4, "hands": 0.85, "haas": 0.4, "narrow": 0.2},
      words=(["Near", "Close", "Within", "Arm", "Breath", "Whisper", "Beside", "Against",
              "Touching", "Held", "Intimate", "Hand", "Cheek", "Skin"],
             COMMON_SECOND + ["Reach", "Distance", "Whisper", "Touch", "Presence"]),
      prompts=["someone breathing beside you in the dark",
               "a string touched, not bowed, very close",
               "small wooden objects moved on a table by your ear"],
      tables=["Breath band", "Stopped pipe", "Three vowels", "Bowed string", "Prepared piano"],
      impulses=['room_chamber', 'room_plate', 'struck'],
      noise=['Pink', 'Pink', 'Crackle', 'Band']),

    # ---------------------------------------------------------------- and three that were asked
    # for by name. Nothing here is sampled from or affiliated with anybody: the ranges were set by
    # ear towards the sound world each name stands for.

    S("Discreet Loops", "Brian Eno",
      # Music for Airports and Discreet Music: a handful of gentle voices on tape loops of
      # different lengths, so they drift apart and never repeat the same combination. Diatonic,
      # unhurried, and deliberately unremarkable -- as ignorable as it is interesting.
      {"scale": ["JI Major (Ptolemy)", "JI 7-limit", "Pythagorean", "JI Pentatonic"],
       "stack": ["Major", "Fifths", "Octaves"],
       "brain_density": ("int", 3, 6), "brain_rate": ("log", 18.0, 70.0),
       "brain_consonance": (0.55, 0.9), "brain_wander": (0.05, 0.3),
       "brain_hold_min": ("log", 20.0, 70.0), "brain_hold_max": ("log", 70.0, 240.0),
       "attack": ("log", 3.0, 14.0), "release": ("log", 8.0, 30.0),
       "partials": ("int", 6, 16), "tilt": (1.2, 2.4), "brightness": (0.35, 0.65),
       "odd_even": (-0.1, 0.35), "inharmonic": (0.0, 0.06), "detune": ("log", 1.0, 6.0),
       "dly_time_l": ("log", 2.0, 9.0), "dly_time_r": ("log", 3.0, 14.0),
       "dly_feedback": (0.35, 0.7), "dly_mix": (0.15, 0.4), "dly_damp": (0.4, 0.8),
       "far_decay": ("log", 8.0, 30.0), "far_level": (0.4, 0.8), "far_highcut": ("log", 2000.0, 6000.0),
       "room_level": (0.15, 0.5), "air": (0.05, 0.25), "depth": (0.4, 0.75),
       "sub_level": (0.0, 0.25), "master_gain": (-13.0, -8.0)},
      {"delay2": 0.55, "room": 0.7, "sub": 0.35, "stack": 0.7, "zplane": 0.15, "cosmos": 0.1,
       "cloud": 0.1, "texture": 0.15, "usertable": 0.45, "src2": 0.6, "src3": 0.25, "slot1": 0.35,
       "keys": 0.2, "strike": 0.25, "keyfind": 0.5, "evensmooth": 0.55, "blend": 0.45,
       "adaptive": 0.35, "guard": 0.3, "depthlaw": 0.35, "envelop": 0.3, "presence": 0.25,
       "phase": 0.5, "microshift": 0.4, "hands": 0.5, "banks": 0.3, "cascade": 0.12},
      words=(["Discreet", "Airport", "Unhurried", "Plain", "Ignorable", "Ambient", "Patient",
              "Gentle", "Lateral", "Oblique", "Quiet", "Music", "Another", "Everyday"],
             COMMON_SECOND + ["Loop", "Room", "Airport", "Music", "Afternoon", "Window"]),
      prompts=["a tape loop of a choir, worn soft by the tenth pass",
               "an empty terminal at six in the morning",
               "a piano recorded in the next room and left running"],
      tables=["Organ drawbars", "Stopped pipe", "Vowel choir", "Odd breathing", "Fibonacci"],
      impulses=['room_hall', 'room_chamber', 'room_plate'],
      noise=['Pink', 'Pink', 'Brown']),

    S("Permafrost North", "Biosphere",
      # Substrata: the Arctic. A cold, narrow band of sound, sub bass a long way under it, small
      # bright details a long way off, and a dub delay that is more space than rhythm.
      {"scale": ["JI Minor", "JI 7-limit", "Slendro (JI)", "Pythagorean"],
       "brain_density": ("int", 2, 5), "brain_rate": ("log", 30.0, 140.0),
       "brain_hold_min": ("log", 40.0, 160.0), "brain_hold_max": ("log", 160.0, 600.0),
       "brain_low": ("int", 26, 40), "brain_high": ("int", 52, 76),
       "attack": ("log", 5.0, 22.0), "release": ("log", 12.0, 45.0),
       "brightness": (0.25, 0.55), "tilt": (1.4, 2.6), "shimmer": (0.1, 0.4),
       "sub_level": (0.3, 0.6), "sub_tone": (0.1, 0.4), "sub_binaural": (2.0, 6.0),
       "dly_time_l": ("log", 0.8, 4.0), "dly_time_r": ("log", 1.2, 6.0),
       "dly_feedback": (0.45, 0.75), "dly_damp": (0.5, 0.85), "dly_mix": (0.1, 0.35),
       "far_decay": ("log", 20.0, 60.0), "far_highcut": ("log", 1200.0, 3500.0),
       "far_size": (2.4, 3.0), "depth": (0.7, 0.95), "width": (1.1, 1.5),
       "air": (0.1, 0.35), "master_gain": (-14.0, -9.0)},
      {"noiseprimary": 0.30, "texture": 0.7, "stretch": 0.5, "cloud": 0.4, "sub": 0.95, "delay2": 0.5,
       "src2": 0.8, "src3": 0.5, "slot1": 0.5, "room": 0.4, "zplane": 0.3, "cosmos": 0.2,
       "narrow": 0.55, "comod": 0.45, "envelop": 0.45, "elev": 0.4, "nearfield": 0.2,
       "depthlaw": 0.5, "farmode": 0.4, "chaos": 0.3, "lenia": 0.25, "dejavu": 0.3,
       "cascade": 0.2, "hands": 0.5, "absorb": 0.7, "tide": 0.4, "banks": 0.35},
      words=(["Permafrost", "Tundra", "Ice", "Kobresia", "Hyperborea", "Circumpolar", "Glacial",
              "Meltwater", "Snow", "Aurora", "Fjord", "Frozen", "Northern", "Silent"],
             COMMON_SECOND + ["North", "Plateau", "Shelf", "Crossing", "Latitude", "Winter"]),
      prompts=["wind over a frozen plateau, a long way from anything",
               "meltwater under a metre of ice",
               "a distant engine across a fjord at night"],
      tables=["Pink resonance", "Sub fold", "Spectral erosion", "Breath band", "Stretched octave"],
      impulses=['room_cavern', 'room_bunker', 'spectral'],
      noise=['Wind', 'Brown', 'Pink', 'Pink']),

    S("Ritual Stone", "Raison d'etre",
      # Dark ritual ambient: a cathedral, a bell, a choir at the edge of hearing, and a long
      # metallic drone under all of it. Consonance kept low on purpose -- this is the one style
      # where the beating between partials is the point.
      {"scale": ["JI Minor", "Pythagorean", "Harmonic 8-16", "Otonality 1-11"],
       "brain_density": ("int", 3, 7), "brain_rate": ("log", 25.0, 110.0),
       "brain_consonance": (0.1, 0.45), "brain_wander": (0.2, 0.6),
       "brain_hold_min": ("log", 30.0, 120.0), "brain_hold_max": ("log", 120.0, 480.0),
       "attack": ("log", 4.0, 20.0), "release": ("log", 15.0, 60.0),
       "partials": ("int", 12, 30), "inharmonic": (0.15, 0.5), "tilt": (0.7, 1.6),
       "odd_even": (-0.5, 0.1), "detune": ("log", 6.0, 24.0),
       "far_decay": ("log", 35.0, 90.0), "far_size": (2.6, 3.0), "far_level": (0.6, 0.95),
       "far_highcut": ("log", 1500.0, 4500.0), "room_level": (0.3, 0.7),
       "sub_level": (0.35, 0.65), "depth": (0.75, 0.98), "air": (0.05, 0.2),
       "master_gain": (-14.0, -9.0)},
      {"room": 0.85, "sub": 0.9, "strike": 0.55, "cosmos": 0.45, "zplane": 0.55, "feedback": 0.3,
       "src2": 0.75, "src3": 0.5, "slot1": 0.45, "texture": 0.45, "usertable": 0.35,
       "bow": 0.25, "spectral": 0.2, "roommorph": 0.35, "farmode": 0.45, "fardiffuse": 0.4,
       "envelop": 0.5, "elev": 0.35, "bodychar": 0.7, "patina": 0.35, "timbre": 0.4,
       "spreadbias": 0.4, "cascade": 0.3, "dejavu": 0.3, "banks": 0.6, "hands": 0.6,
       "archarmony": 0.3, "brain2": 0.35},
      words=(["Ritual", "Reliquary", "Ossuary", "Procession", "Vigil", "Sanctum", "Crypt",
              "Liturgy", "Requiem", "Stone", "Iron", "Ash", "Votive", "Solemn"],
             COMMON_SECOND + ["Stone", "Rite", "Vigil", "Procession", "Chamber", "Bell"]),
      prompts=["a bell struck once in a stone crypt and left to ring",
               "a choir heard through a wall, very slowed",
               "iron and rust, a cathedral, a long way down"],
      tables=["Organ mixture", "Gong wash", "Singing bowl", "Bell partials", "Three vowels"],
      impulses=['room_cathedral', 'room_cavern', 'modal', 'struck'],
      noise=['Brown', 'Pink', 'Pink', 'Wind']),
]
