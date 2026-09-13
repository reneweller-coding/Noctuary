"""Noctuary -- generate the Z-plane filter bank (Core/src/ZPlaneBank.inc).

The Z-plane filter is Dave Rossum's idea from the E-mu Morpheus (US 5,170,369, expired): four
filter frames on the corners of a square, and a point inside it is a filter whose poles and zeros
are interpolated between them. The Morpheus shipped 197 of these "cubes". Its coefficient tables
are proprietary firmware data and are not here; what is here is our own bank in the same
architecture, and the numbers come from published acoustics rather than from that firmware.

Which is the reason this file exists rather than a hand-typed table. Almost every shape is a set
of frequency RATIOS -- the mode series of a bar, a membrane, a pipe, a bell -- times a base
frequency, and typing out ninety-six shapes times four corners times six frequencies invites
arithmetic mistakes that sound like a filter and are simply wrong. Here the ratios are written
down once, with a note saying where they come from, and the multiplication is done by a machine.

    python Tools/make_zplane_bank.py            # writes Core/src/ZPlaneBank.inc

The first sixteen shapes are the original hand-written bank, carried over frequency for frequency:
the 5000 presets in the library name their shape as text ("z_shape=Glass"), so those names and
their order are frozen for good. New shapes are appended, never inserted.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
OUT = os.path.join(ROOT, "Core", "src", "ZPlaneBank.inc")
MAX_SECTIONS = 6

# The categories the editor groups the list by. Display order, not parameter order: the parameter
# keeps the historical numbering for ever, the combo box shows the bank sorted into families.
CATEGORIES = ["Voice", "Sweeps", "Combs & Phasers", "Strings & Bodies", "Bars & Bells",
              "Membranes", "Tubes & Pipes", "Rooms & Spaces", "Series & Exotic", "Extremes",
              "Instruments", "EQ & Speakers"]

SHAPES = []


# The third axis. A cube, not a square: the original is called a cube because a filter sits at
# each of EIGHT corners and the point moves in three dimensions. Writing eight corners for every
# shape would be twice the data for a face that is usually the same idea again, so instead each
# shape names a rule for how its far face differs, and the near face (Transform = 0) is exactly
# the filter that was there before -- which is what keeps every preset ever saved sounding the
# same, since none of them mention the new parameter and its default is 0.
def z_face(corners, rule, name):
    if rule == "none":
        return [tuple(c) for c in corners]
    out = []
    for hz, bw, zr, zbw, tilt in corners:
        if rule == "res":                                  # Transform sharpens everything
            out.append((hz, bw * 0.30, zr, zbw, tilt))
        elif rule == "damp":                               # ... or opens it up towards flat
            out.append((hz, min(bw * 3.5, 2.0), zr, zbw, tilt))
        elif rule == "notch":                              # peaks turn into notches
            assert zr > 0.0, "notch needs zeros in the near face too: " + name
            out.append((hz, bw, 1.0, 0.06, tilt))
        elif rule == "bright":
            out.append((hz, bw, zr, zbw, min(tilt * 1.5, 1.8)))
        elif rule == "dark":
            out.append((hz, bw, zr, zbw, tilt * 0.55))
        elif rule == "up":                                 # the whole structure an octave higher
            out.append(([round(h * 2) for h in hz], bw, zr, zbw, tilt))
        elif rule == "spread":                             # the partials fan out
            out.append(([round(h * (1.0 + 0.30 * i)) for i, h in enumerate(hz)], bw, zr, zbw, tilt))
        else:
            raise AssertionError("unknown z rule " + rule)
    return out


def shape(name, cat, note, corners, z="res"):
    """One shape: four corners at Transform = 0, in the order (0,0) (1,0) (0,1) (1,1), and a rule
    for the four at Transform = 1.

    Every corner must list the same number of frequencies -- the interpolation takes the smallest
    count of the eight, so one short corner silently drops sections from all the others.
    """
    assert cat in CATEGORIES, cat
    counts = {len(c[0]) for c in corners}
    assert len(corners) == 4 and len(counts) == 1, (name, counts)
    assert counts.pop() <= MAX_SECTIONS, name
    SHAPES.append(dict(name=name, cat=cat, note=note,
                       corners=[tuple(c) for c in corners] + z_face(corners, z, name), zrule=z))


def series(base, ratios):
    """A mode series: the ratios times a base frequency, rounded to whole Hz."""
    return [round(base * r) for r in ratios]


def freq_res(ratios, lo, hi, broad, narrow, zr=0.0, zbw=0.0, tilt=1.0):
    """The common pair of axes: X moves the whole structure up, Y tightens it.

    Four corners out of two numbers each, which is what most of these shapes want: the same body
    small or large, damped or ringing.
    """
    return [(series(lo, ratios), broad, zr, zbw, tilt),
            (series(hi, ratios), broad, zr, zbw, tilt),
            (series(lo, ratios), narrow, zr, zbw, tilt),
            (series(hi, ratios), narrow, zr, zbw, tilt)]


def vowels(a, b, c, d, bw=0.10, zr=0.0, zbw=0.0, tilt=0.75):
    """Four vowels on the corners: X takes the first to the second, Y the first to the third."""
    return [(a, bw, zr, zbw, tilt), (b, bw, zr, zbw, tilt),
            (c, bw, zr, zbw, tilt), (d, bw, zr, zbw, tilt)]


# ============================================================ the original sixteen, verbatim
# Frozen: the library names these by text, and the parameter's numbering is part of every preset
# ever saved. Copied frequency for frequency from the hand-written table they replace.
shape("Vowel Morph", "Voice", "a -> e across X, o -> i up Y; pure resonators, so the vowel takes the sound over",
      vowels([700, 1150, 2900], [400, 1600, 2700], [450, 800, 2830], [270, 2300, 3000],
             bw=0.12, tilt=0.70))
shape("Choir", "Voice", "four voiced formants as bell sections -- peaks on a flat background, not a hollow tube",
      vowels([320, 800, 2200, 3400], [400, 1100, 2600, 3800], [250, 700, 1900, 3000], [500, 1400, 3000, 4200],
             bw=0.10, zr=1.00, zbw=0.80, tilt=0.80))
shape("Nasal", "Voice", "formants with a deep notch just above each",
      vowels([300, 1000, 2400], [380, 1500, 2900], [260, 850, 2000], [480, 1900, 3600],
             bw=0.09, zr=1.35, zbw=0.15, tilt=0.85))
shape("Low Sweep", "Sweeps", "X = cutoff (120 Hz .. 1.8 kHz), Y = resonance; a zero far above tips it down",
      [([120, 180], 0.60, 3.0, 0.80, 0.90), ([1800, 2700], 0.60, 3.0, 0.80, 0.90),
       ([120, 180], 0.12, 3.0, 0.80, 0.90), ([1800, 2700], 0.12, 3.0, 0.80, 0.90)])
shape("High Sweep", "Sweeps", "the zero sits below the pole, so the low end is cut away",
      [([150, 220], 0.60, 0.35, 0.60, 0.90), ([2500, 3700], 0.60, 0.35, 0.60, 0.90),
       ([150, 220], 0.15, 0.35, 0.60, 0.90), ([2500, 3700], 0.15, 0.35, 0.60, 0.90)])
shape("Band Sweep", "Sweeps", "one band, X = position, Y = narrowness",
      [([200], 0.25, 0.0, 0.0, 1.0), ([3000], 0.25, 0.0, 0.0, 1.0),
       ([200], 0.06, 0.0, 0.0, 1.0), ([3000], 0.06, 0.0, 0.0, 1.0)])
shape("Phaser", "Combs & Phasers", "six octave-spaced pole/zero pairs, the whole comb sliding up",
      [([200, 400, 800, 1600, 3200, 6400], 0.35, 1.25, 0.30, 1.0),
       ([300, 600, 1200, 2400, 4800, 9600], 0.35, 1.25, 0.30, 1.0),
       ([150, 300, 600, 1200, 2400, 4800], 0.35, 1.25, 0.30, 1.0),
       ([450, 900, 1800, 3600, 7200, 12000], 0.35, 1.25, 0.30, 1.0)])
shape("Comb", "Combs & Phasers", "harmonic teeth, each a sharp bell (zero on the pole, ten times wider)",
      [([110, 220, 330, 440, 550, 660], 0.06, 1.00, 0.60, 0.95),
       ([165, 330, 495, 660, 825, 990], 0.06, 1.00, 0.60, 0.95),
       ([82, 164, 246, 328, 410, 492], 0.06, 1.00, 0.60, 0.95),
       ([220, 440, 660, 880, 1100, 1320], 0.06, 1.00, 0.60, 0.95)])
shape("Flanger", "Combs & Phasers", "a wider comb of bells whose spacing opens and closes",
      [([300, 600, 900, 1200, 1500, 1800], 0.20, 1.00, 1.20, 1.0),
       ([500, 1000, 1500, 2000, 2500, 3000], 0.20, 1.00, 1.20, 1.0),
       ([200, 400, 600, 800, 1000, 1200], 0.20, 1.00, 1.20, 1.0),
       ([800, 1600, 2400, 3200, 4000, 4800], 0.20, 1.00, 1.20, 1.0)])
shape("Notch Cluster", "Combs & Phasers", "four deep notches (zero on the pole, far narrower) in a wide filter",
      [([400, 900, 1800, 3600], 0.80, 1.0, 0.06, 1.0), ([600, 1300, 2600, 5200], 0.80, 1.0, 0.06, 1.0),
       ([300, 700, 1400, 2800], 0.80, 1.0, 0.06, 1.0), ([900, 2000, 4000, 8000], 0.80, 1.0, 0.06, 1.0)])
shape("Strings", "Strings & Bodies", "the inharmonic body resonances of a plucked box",
      [([190, 330, 510, 780, 1250], 0.08, 1.00, 0.80, 0.80), ([240, 420, 650, 990, 1580], 0.08, 1.00, 0.80, 0.80),
       ([150, 260, 400, 610, 980], 0.08, 1.00, 0.80, 0.80), ([310, 540, 830, 1270, 2030], 0.08, 1.00, 0.80, 0.80)])
shape("Metal Bars", "Bars & Bells", "sharp inharmonic bells, the partials of a struck bar",
      [([300, 760, 1490, 2470, 3700], 0.03, 1.00, 0.45, 0.85), ([420, 1060, 2080, 3450, 5170], 0.03, 1.00, 0.45, 0.85),
       ([210, 530, 1040, 1730, 2590], 0.03, 1.00, 0.45, 0.85), ([600, 1520, 2980, 4940, 7400], 0.03, 1.00, 0.45, 0.85)])
shape("Wood", "Strings & Bodies", "broad low-mid resonances, the inside of a hollow body",
      [([180, 420, 900, 1600], 0.22, 1.00, 1.50, 0.75), ([240, 560, 1200, 2100], 0.22, 1.00, 1.50, 0.75),
       ([140, 330, 700, 1250], 0.22, 1.00, 1.50, 0.75), ([320, 750, 1600, 2800], 0.22, 1.00, 1.50, 0.75)])
shape("Glass", "Bars & Bells", "high, sparse, very sharp",
      [([900, 2100, 3900], 0.020, 1.00, 0.30, 0.70), ([1300, 3000, 5600], 0.020, 1.00, 0.30, 0.70),
       ([700, 1600, 3000], 0.020, 1.00, 0.30, 0.70), ([1800, 4200, 7800], 0.020, 1.00, 0.30, 0.70)])
shape("Peaks", "Series & Exotic", "six harmonic needles -- the harmonic series pressed onto anything that goes through",
      [([220, 440, 660, 880, 1100, 1320], 0.025, 1.00, 0.375, 0.90),
       ([330, 660, 990, 1320, 1650, 1980], 0.025, 1.00, 0.375, 0.90),
       ([165, 330, 495, 660, 825, 990], 0.025, 1.00, 0.375, 0.90),
       ([440, 880, 1320, 1760, 2200, 2640], 0.025, 1.00, 0.375, 0.90)])
shape("Infinite", "Extremes", "two almost identical poles at the stability limit -- a ringing that beats",
      [([150, 151], 0.004, 0.0, 0.0, 1.0), ([1200, 1210], 0.004, 0.0, 0.0, 1.0),
       ([150, 153], 0.012, 0.0, 0.0, 1.0), ([1200, 1230], 0.012, 0.0, 0.0, 1.0)])

# ============================================================ voice
# Sung-vowel formants from the tables that have been in synthesis textbooks since the seventies
# (Sundberg's measurements of trained singers, as reprinted in the Csound and CMusic manuals).
# Four formants each; the fifth adds nothing a filter of this order can show.
SOP = dict(a=[800, 1150, 2900, 3900], e=[350, 2000, 2800, 3600], i=[270, 2140, 2950, 3900],
           o=[450, 800, 2830, 3800], u=[325, 700, 2700, 3800])
ALT = dict(a=[800, 1150, 2800, 3500], e=[400, 1600, 2700, 3300], i=[350, 1700, 2700, 3700],
           o=[450, 800, 2830, 3500], u=[325, 700, 2530, 3500])
TEN = dict(a=[650, 1080, 2650, 2900], e=[400, 1700, 2600, 3200], i=[290, 1870, 2800, 3250],
           o=[400, 800, 2600, 2800], u=[350, 600, 2700, 2900])
BAS = dict(a=[600, 1040, 2250, 2450], e=[400, 1620, 2400, 2800], i=[250, 1750, 2600, 3050],
           o=[400, 750, 2400, 2600], u=[350, 600, 2400, 2675])

shape("Soprano", "Voice", "a soprano's four formants: a -> e across, o -> i up. The highest voice, so the whole space sits high",
      vowels(SOP["a"], SOP["e"], SOP["o"], SOP["i"], bw=0.09, tilt=0.75))
shape("Alto", "Voice", "the same four vowels an alto's formants apart -- lower and closer together than the soprano's",
      vowels(ALT["a"], ALT["e"], ALT["o"], ALT["i"], bw=0.10, tilt=0.75))
shape("Tenor", "Voice", "a tenor: F2 and F3 crowd together, which is what gives the voice its ring",
      vowels(TEN["a"], TEN["e"], TEN["o"], TEN["i"], bw=0.10, tilt=0.80))
shape("Bass Voice", "Voice", "the lowest of the four, and the darkest: nothing above 3 kHz has any weight",
      vowels(BAS["a"], BAS["e"], BAS["o"], BAS["i"], bw=0.11, tilt=0.80))
shape("Rounded Vowels", "Voice", "u -> o across, and the voice type up: bass at the bottom, soprano at the top. Every corner is a closed mouth",
      [(BAS["u"], 0.10, 0.0, 0.0, 0.80), (BAS["o"], 0.10, 0.0, 0.0, 0.80),
       (SOP["u"], 0.10, 0.0, 0.0, 0.80), (SOP["o"], 0.10, 0.0, 0.0, 0.80)])
shape("Diphthong", "Voice", "one long vowel sliding: a -> i across, and closing (e -> u) upwards",
      [(TEN["a"], 0.09, 0.0, 0.0, 0.75), (TEN["i"], 0.09, 0.0, 0.0, 0.75),
       (TEN["e"], 0.09, 0.0, 0.0, 0.75), (TEN["u"], 0.09, 0.0, 0.0, 0.75)])
shape("Whisper", "Voice", "the same vowel shapes with the resonances left wide open: shape without pitch",
      vowels(SOP["a"], SOP["e"], SOP["o"], SOP["i"], bw=0.55, tilt=0.95))
shape("Throat", "Voice", "a constricted low tract: one strong low formant and everything above it damped away",
      freq_res([1.0, 2.4, 4.6], 110, 340, 0.30, 0.07, tilt=0.45))
shape("Nasal Cavity", "Voice", "m and n: a low formant with the antiformant of the side branch just above it, which is what makes a nasal sound blocked",
      [([250, 1000, 2200], 0.08, 1.30, 0.12, 0.80), ([250, 1800, 2600], 0.08, 1.30, 0.12, 0.80),
       ([320, 900, 2000], 0.08, 1.50, 0.10, 0.80), ([320, 1600, 2400], 0.08, 1.50, 0.10, 0.80)])
shape("Vocal Fry", "Voice", "the bottom of the register: resonances low enough to hear as separate pulses",
      freq_res([1.0, 1.9, 3.7, 6.1], 55, 165, 0.18, 0.03, tilt=0.70))
shape("Megaphone", "Voice", "the telephone band -- nothing below 400 Hz, nothing above 3 kHz, and a hard peak in the middle",
      [([420, 900, 1900, 3000], 0.20, 0.30, 0.50, 1.0), ([520, 1200, 2400, 3400], 0.20, 0.30, 0.50, 1.0),
       ([420, 900, 1900, 3000], 0.07, 0.30, 0.50, 1.0), ([520, 1200, 2400, 3400], 0.07, 0.30, 0.50, 1.0)])

# ============================================================ strings and bodies
# Body resonances as they are measured and published: the violin's A0 (air) and B1 corpus modes,
# the guitar's Helmholtz/top/back triple, the piano soundboard's lowest plate modes.
shape("Violin Body", "Strings & Bodies", "A0 the air mode, B1- and B1+ the corpus modes, and the bridge hill around 2.3 kHz",
      freq_res([1.0, 1.67, 1.93, 8.4], 275, 340, 0.09, 0.03, zr=1.0, zbw=0.9, tilt=0.75))
shape("Cello Body", "Strings & Bodies", "the same three modes an octave and a half down, with a lower bridge hill",
      freq_res([1.0, 1.76, 2.38, 11.4], 105, 135, 0.10, 0.035, zr=1.0, zbw=0.9, tilt=0.75))
shape("Double Bass", "Strings & Bodies", "a body so large its air mode is below the lowest string",
      freq_res([1.0, 1.67, 2.5, 11.7], 58, 78, 0.11, 0.04, zr=1.0, zbw=0.9, tilt=0.70))
shape("Guitar Box", "Strings & Bodies", "the Helmholtz air mode, the top plate and the back plate -- the triple that makes a guitar a guitar",
      freq_res([1.0, 2.0, 2.5, 4.3, 6.8], 100, 128, 0.10, 0.035, zr=1.0, zbw=0.9, tilt=0.70))
shape("Harp Body", "Strings & Bodies", "a long soundbox: low modes close together and a light top",
      freq_res([1.0, 2.0, 3.3, 5.8, 10.0], 90, 130, 0.12, 0.04, zr=1.0, zbw=0.9, tilt=0.65))
shape("Piano Board", "Strings & Bodies", "soundboard plate modes -- broad, low and very hard to make sound like anything else",
      freq_res([1.0, 1.9, 3.25, 5.25, 8.75], 80, 115, 0.14, 0.05, zr=1.0, zbw=1.1, tilt=0.70))
shape("Sitar", "Strings & Bodies", "the jawari: a flat bridge that throws energy into a bright band of high partials, and buzzes",
      freq_res([1.0, 1.75, 2.75, 4.17, 5.83], 1200, 1900, 0.045, 0.012, zr=1.0, zbw=0.5, tilt=0.92))
shape("Sympathetic", "Strings & Bodies", "undamped strings behind the played one: narrow harmonic peaks that keep ringing",
      freq_res([1, 2, 3, 4, 5, 6], 147, 294, 0.012, 0.003, zr=1.0, zbw=0.4, tilt=0.90))

# ============================================================ bars, plates and bells
# Bar and bell mode ratios as published in Fletcher & Rossing, The Physics of Musical Instruments:
# the free-free bar 1 : 2.756 : 5.404 : 8.933, the marimba undercut to 1 : 3.9 : 9.2, the
# vibraphone to 1 : 4 : 10, the tubular bell 2 : 3 : 4.16 : 5.43 : 6.79 : 8.21, and the bell's
# hum / prime / tierce / quint / nominal.
shape("Marimba", "Bars & Bells", "a rosewood bar undercut until its second mode is two octaves up: 1 : 3.9 : 9.2",
      freq_res([1.0, 3.9, 9.2], 175, 440, 0.030, 0.008, zr=1.0, zbw=0.5, tilt=0.55))
shape("Vibraphone", "Bars & Bells", "aluminium, tuned 1 : 4 : 10, and far less damped than wood",
      freq_res([1.0, 4.0, 10.0], 175, 440, 0.012, 0.0035, zr=1.0, zbw=0.45, tilt=0.65))
shape("Glockenspiel", "Bars & Bells", "a short steel bar, left untuned: the ideal free-free ratios 1 : 2.76 : 5.40 : 8.93",
      freq_res([1.0, 2.756, 5.404, 8.933], 900, 1800, 0.010, 0.003, zr=1.0, zbw=0.4, tilt=0.60))
shape("Tubular Bell", "Bars & Bells", "modes 2 : 3 : 4.16 : 5.43 : 6.79 : 8.21 -- the pitch you hear is not any of them, it is the missing fundamental",
      freq_res([1.0, 1.5, 2.08, 2.715, 3.395, 4.105], 262, 440, 0.008, 0.0025, zr=1.0, zbw=0.4, tilt=0.75))
shape("Church Bell", "Bars & Bells", "hum, prime, tierce, quint, nominal: the minor third in a bell is the tierce, and it is why bells sound sad",
      freq_res([0.5, 1.0, 1.2, 1.5, 2.0, 3.0], 300, 520, 0.006, 0.002, zr=1.0, zbw=0.4, tilt=0.80))
shape("Gong", "Bars & Bells", "a thick disc: dozens of modes, no order to them, everything low",
      freq_res([1.0, 1.37, 1.91, 2.63, 3.44, 4.71], 78, 150, 0.045, 0.012, zr=1.0, zbw=0.7, tilt=0.88))
shape("Tam Tam", "Bars & Bells", "the gong's untuned relative, and denser still -- a wash rather than a pitch",
      freq_res([1.0, 1.29, 1.71, 2.19, 2.83, 3.61], 120, 260, 0.09, 0.025, zr=1.0, zbw=0.9, tilt=0.95))
shape("Steel Plate", "Bars & Bells", "a hanging sheet: modes crowd together as they rise, which is what a plate does and a bar does not",
      freq_res([1.0, 1.59, 2.29, 3.13, 4.11, 5.24], 320, 620, 0.020, 0.006, zr=1.0, zbw=0.6, tilt=0.90))
shape("Handpan", "Bars & Bells", "a tuned dome: the note, its octave and its twelfth, over the shell's air mode",
      freq_res([0.5, 1.0, 2.0, 3.0, 5.0], 220, 392, 0.014, 0.004, zr=1.0, zbw=0.5, tilt=0.70))
shape("Kalimba", "Bars & Bells", "a short tine clamped at one end, so the free-free ratios again, but tiny and dry",
      freq_res([1.0, 2.756, 5.404], 330, 800, 0.035, 0.010, zr=1.0, zbw=0.6, tilt=0.50))

# ============================================================ membranes
# The ideal circular membrane 1 : 1.594 : 2.136 : 2.296 : 2.918 (Bessel zeros); the kettledrum's
# air load pulls its useful modes to something near 1 : 1.5 : 2 : 2.44 : 2.9, which is why a
# timpano has a pitch and a tom does not.
shape("Timpani", "Membranes", "the kettle's air load turns Bessel ratios into something almost harmonic: 1 : 1.5 : 2 : 2.44 : 2.9",
      freq_res([1.0, 1.5, 1.97, 2.44, 2.90], 110, 200, 0.030, 0.009, zr=1.0, zbw=0.7, tilt=0.72))
shape("Frame Drum", "Membranes", "an unloaded skin: the ideal circular membrane, and no pitch to speak of",
      freq_res([1.0, 1.594, 2.136, 2.296, 2.918], 90, 190, 0.055, 0.016, zr=1.0, zbw=0.8, tilt=0.85))
shape("Tabla", "Membranes", "the black patch loads the centre until the modes line up 1 : 2 : 3 : 4 -- a drum that plays notes",
      freq_res([1.0, 2.0, 3.0, 4.0, 5.0], 160, 330, 0.022, 0.006, zr=1.0, zbw=0.6, tilt=0.78))
shape("Snare Shell", "Membranes", "the shell's own ring under a high band of wires",
      freq_res([1.0, 1.62, 2.31, 5.9, 9.4], 180, 300, 0.070, 0.020, zr=1.0, zbw=0.9, tilt=0.95))

# ============================================================ tubes and pipes
shape("Open Pipe", "Tubes & Pipes", "open at both ends: every harmonic present",
      freq_res([1, 2, 3, 4, 5, 6], 110, 330, 0.030, 0.008, zr=1.0, zbw=0.55, tilt=0.85))
shape("Closed Pipe", "Tubes & Pipes", "stopped at one end: odd harmonics only, an octave lower for the same length",
      freq_res([1, 3, 5, 7, 9, 11], 82, 245, 0.030, 0.008, zr=1.0, zbw=0.55, tilt=0.82))
shape("Clarinet", "Tubes & Pipes", "the odd series with the bell's formant sitting on top of it around 1.5 kHz",
      [([147, 441, 735, 1500, 2400], 0.05, 1.0, 0.6, 0.85), ([294, 882, 1470, 1800, 2800], 0.05, 1.0, 0.6, 0.85),
       ([147, 441, 735, 1500, 2400], 0.014, 1.0, 0.6, 0.85), ([294, 882, 1470, 1800, 2800], 0.014, 1.0, 0.6, 0.85)])
shape("Flute", "Tubes & Pipes", "a weak fundamental, a strong second, and the breath noise band above everything",
      [([262, 524, 786, 3200, 5000], 0.06, 1.0, 0.7, 1.15), ([523, 1046, 1569, 4000, 6000], 0.06, 1.0, 0.7, 1.15),
       ([262, 524, 786, 3200, 5000], 0.018, 1.0, 0.7, 1.15), ([523, 1046, 1569, 4000, 6000], 0.018, 1.0, 0.7, 1.15)])
shape("Didgeridoo", "Tubes & Pipes", "a very low drone with the player's mouth as a moving formant far above it",
      [([65, 130, 700, 1300], 0.06, 0.0, 0.0, 1.05), ([73, 146, 1400, 2400], 0.06, 0.0, 0.0, 1.05),
       ([65, 130, 500, 900], 0.02, 0.0, 0.0, 1.05), ([73, 146, 1000, 1800], 0.02, 0.0, 0.0, 1.05)])
shape("Bottle", "Tubes & Pipes", "a Helmholtz resonator: one note, and almost nothing else",
      freq_res([1.0, 5.4, 9.1], 130, 420, 0.055, 0.012, zr=1.0, zbw=1.4, tilt=0.30))
shape("Pan Flute", "Tubes & Pipes", "stopped pipes again, but short and breathy, with the noise band riding high",
      [([440, 1320, 2200, 4500], 0.05, 1.0, 0.8, 1.0), ([880, 2640, 4400, 7000], 0.05, 1.0, 0.8, 1.0),
       ([440, 1320, 2200, 4500], 0.016, 1.0, 0.8, 1.0), ([880, 2640, 4400, 7000], 0.016, 1.0, 0.8, 1.0)])
shape("Organ Pipe", "Tubes & Pipes", "a principal rank: the harmonic series, very slightly stretched, and steady as a wall",
      freq_res([1.0, 2.01, 3.03, 4.06, 5.10, 6.16], 131, 262, 0.020, 0.006, zr=1.0, zbw=0.5, tilt=0.88))

# ============================================================ rooms and spaces
# Axial room modes are c / (2L): a 4.0 x 3.0 x 2.5 m room has its first three at 43, 57 and 69 Hz.
shape("Small Room", "Rooms & Spaces", "the axial modes of a room about four metres by three: 43, 57, 69 Hz and their seconds",
      [([43, 57, 69, 86, 114, 137], 0.10, 0.0, 0.0, 0.90), ([57, 76, 92, 114, 152, 183], 0.10, 0.0, 0.0, 0.90),
       ([43, 57, 69, 86, 114, 137], 0.030, 0.0, 0.0, 0.90), ([57, 76, 92, 114, 152, 183], 0.030, 0.0, 0.0, 0.90)])
shape("Stairwell", "Rooms & Spaces", "tall and narrow: one very low vertical mode and a stack of close horizontal ones",
      freq_res([1.0, 6.0, 6.9, 12.0, 13.8], 14, 24, 0.09, 0.025, tilt=0.95))
shape("Cave", "Rooms & Spaces", "modes low enough to feel rather than hear, and no two of them related",
      freq_res([1.0, 1.63, 2.31, 3.37, 4.72], 21, 44, 0.13, 0.035, tilt=0.92))
shape("Duct", "Rooms & Spaces", "a square metal duct: one strong cross mode and its odd multiples, which is why ventilation whistles",
      freq_res([1, 3, 5, 7], 570, 1150, 0.045, 0.012, zr=1.0, zbw=0.6, tilt=0.85))
shape("Concrete Pipe", "Rooms & Spaces", "a metre-wide pipe: harmonic, hard, and still ringing after you have stopped",
      freq_res([1, 2, 3, 4, 5, 6], 172, 300, 0.016, 0.005, zr=1.0, zbw=0.5, tilt=0.90))
shape("Plate Reverb", "Rooms & Spaces", "a steel sheet under tension: modes packed so closely that the ear gives up counting them",
      freq_res([1.0, 1.6, 2.33, 3.67, 5.67, 8.67], 300, 520, 0.030, 0.010, zr=1.0, zbw=0.8, tilt=0.97))
shape("Spring Reverb", "Rooms & Spaces", "a coil is dispersive: the high end arrives first, which is the chirp everybody recognises",
      freq_res([1.0, 1.47, 2.07, 2.87, 3.93], 1500, 2600, 0.035, 0.010, zr=1.0, zbw=0.7, tilt=0.95))
shape("Tunnel", "Rooms & Spaces", "long, round and empty: octaves, and a very long way down",
      freq_res([1, 2, 4, 8], 30, 62, 0.10, 0.030, tilt=0.85))

# ============================================================ sweeps
shape("Dual Peak", "Sweeps", "two peaks whose spacing opens across X while Y sharpens them: the simplest thing this filter can do that a normal one cannot",
      [([400, 560], 0.20, 0.0, 0.0, 1.0), ([400, 2800], 0.20, 0.0, 0.0, 1.0),
       ([400, 560], 0.045, 0.0, 0.0, 1.0), ([400, 2800], 0.045, 0.0, 0.0, 1.0)])
shape("Formant Shift", "Sweeps", "one vowel, moved bodily up and down: the Donald Duck axis, and much more useful than it sounds",
      [([215, 570, 1450], 0.11, 0.0, 0.0, 0.80), ([1400, 3700, 9400], 0.11, 0.0, 0.0, 0.80),
       ([215, 570, 1450], 0.035, 0.0, 0.0, 0.80), ([1400, 3700, 9400], 0.035, 0.0, 0.0, 0.80)])
shape("Ladder LP", "Sweeps", "four poles at one frequency: the classic ladder low pass, resonance on Y",
      [([120, 120, 120, 120], 0.70, 4.0, 0.9, 1.0), ([4000, 4000, 4000, 4000], 0.70, 4.0, 0.9, 1.0),
       ([120, 120, 120, 120], 0.10, 4.0, 0.9, 1.0), ([4000, 4000, 4000, 4000], 0.10, 4.0, 0.9, 1.0)])
shape("Vocal Wah", "Sweeps", "one loud peak walking from 300 Hz to 2.5 kHz over a low shelf -- a pedal, not a vowel",
      [([90, 320], 0.35, 0.0, 0.0, 1.6), ([90, 2500], 0.35, 0.0, 0.0, 1.6),
       ([90, 320], 0.05, 0.0, 0.0, 1.6), ([90, 2500], 0.05, 0.0, 0.0, 1.6)])
shape("Triple Notch", "Sweeps", "three deep notches sweeping together through an otherwise open filter",
      [([500, 1500, 4500], 0.9, 1.0, 0.05, 1.0), ([900, 2700, 8100], 0.9, 1.0, 0.05, 1.0),
       ([300, 900, 2700], 0.9, 1.0, 0.025, 1.0), ([1400, 4200, 12600], 0.9, 1.0, 0.025, 1.0)])
shape("Band Stack", "Sweeps", "three bands whose spacing opens on X: a fifth apart at one end, three octaves at the other",
      [([300, 450, 675], 0.15, 0.0, 0.0, 1.0), ([300, 1200, 4800], 0.15, 0.0, 0.0, 1.0),
       ([300, 450, 675], 0.035, 0.0, 0.0, 1.0), ([300, 1200, 4800], 0.035, 0.0, 0.0, 1.0)])
shape("Sub Sweep", "Sweeps", "the bottom two octaves only, where a resonance is felt before it is heard",
      [([28, 42], 0.40, 2.5, 0.9, 0.9), ([300, 450], 0.40, 2.5, 0.9, 0.9),
       ([28, 42], 0.06, 2.5, 0.9, 0.9), ([300, 450], 0.06, 2.5, 0.9, 0.9)])
shape("Air Lift", "Sweeps", "four peaks in the top two octaves and nothing below: all sheen, no body",
      [([4000, 6000, 9000, 13000], 0.30, 0.25, 0.6, 1.0), ([6000, 9000, 13500, 18000], 0.30, 0.25, 0.6, 1.0),
       ([4000, 6000, 9000, 13000], 0.08, 0.25, 0.6, 1.0), ([6000, 9000, 13500, 18000], 0.08, 0.25, 0.6, 1.0)])

# ============================================================ combs and phasers
shape("Phaser 12", "Combs & Phasers", "the phaser again with six tighter pairs -- twelve poles, which is all this filter has",
      [([300, 480, 770, 1230, 1970, 3150], 0.22, 1.20, 0.22, 1.0),
       ([450, 720, 1150, 1840, 2950, 4720], 0.22, 1.20, 0.22, 1.0),
       ([220, 350, 560, 900, 1440, 2300], 0.22, 1.20, 0.22, 1.0),
       ([680, 1090, 1740, 2790, 4460, 7140], 0.22, 1.20, 0.22, 1.0)])
shape("Barber Pole", "Combs & Phasers", "pairs spaced by a constant ratio, so moving the point sounds like a rise that never arrives",
      freq_res([1.0, 1.78, 3.16, 5.62, 10.0, 17.8], 120, 240, 0.28, 0.28, zr=1.22, zbw=0.28, tilt=1.0))
shape("Deep Comb", "Combs & Phasers", "narrow teeth over a wide range: the hollow, ringing end of comb filtering",
      freq_res([1, 2, 3, 4, 5, 6], 60, 190, 0.020, 0.006, zr=1.0, zbw=0.35, tilt=1.0))
shape("Chorus Comb", "Combs & Phasers", "each tooth is a pair a few cents apart, so every one of them beats",
      [([200, 203, 400, 406, 600, 609], 0.030, 1.0, 0.5, 1.0),
       ([300, 305, 600, 610, 900, 915], 0.030, 1.0, 0.5, 1.0),
       ([200, 201, 400, 402, 600, 603], 0.010, 1.0, 0.5, 1.0),
       ([300, 302, 600, 604, 900, 906], 0.010, 1.0, 0.5, 1.0)])
shape("Ring Comb", "Combs & Phasers", "teeth spaced by a fifth rather than an octave: a comb that will not resolve into a pitch",
      freq_res([1.0, 1.5, 2.25, 3.375, 5.06, 7.59], 110, 260, 0.025, 0.007, zr=1.0, zbw=0.4, tilt=0.95))
shape("Metal Comb", "Combs & Phasers", "a comb stretched the way a stiff string stretches -- the teeth drift sharp as they rise",
      freq_res([1.0, 2.06, 3.2, 4.44, 5.8, 7.3], 130, 300, 0.018, 0.005, zr=1.0, zbw=0.4, tilt=0.92))
shape("Micro Comb", "Combs & Phasers", "teeth so close together that the filter reads as a texture and not as a pitch",
      freq_res([1.0, 1.06, 1.12, 1.19, 1.26, 1.33], 700, 2400, 0.010, 0.003, zr=1.0, zbw=0.5, tilt=1.0))

# ============================================================ series and exotic
shape("Golden Ratio", "Series & Exotic", "powers of phi: the least harmonic series there is, in the strict sense that no two partials nearly agree",
      freq_res([1.0, 1.618, 2.618, 4.236, 6.854, 11.09], 120, 300, 0.020, 0.006, zr=1.0, zbw=0.45, tilt=0.85))
shape("Stretched", "Series & Exotic", "the harmonic series with the piano's stretch (n^1.03) -- close enough to be a pitch, wrong enough to shimmer",
      freq_res([1.0, 2.04, 3.09, 4.16, 5.23, 6.31], 130, 320, 0.016, 0.005, zr=1.0, zbw=0.45, tilt=0.88))
shape("Prime Peaks", "Series & Exotic", "partials at the primes: 1, 2, 3, 5, 7, 11 -- harmonic, but with holes where the ear expects something",
      freq_res([1, 2, 3, 5, 7, 11], 110, 275, 0.018, 0.005, zr=1.0, zbw=0.45, tilt=0.90))
shape("Octave Stack", "Series & Exotic", "six octaves, one section each: the Shepard ladder as a filter",
      freq_res([1, 2, 4, 8, 16, 32], 60, 150, 0.070, 0.020, tilt=1.0))
shape("Bohlen Pierce", "Series & Exotic", "a scale built on the twelfth rather than the octave, thirteen steps to a tritave",
      freq_res([1.0, 1.268, 1.608, 2.214, 3.0, 3.804], 165, 400, 0.020, 0.006, zr=1.0, zbw=0.45, tilt=0.88))
shape("Whole Tone", "Series & Exotic", "six peaks a whole tone apart: no leading note anywhere, so nothing resolves",
      freq_res([1.0, 1.122, 1.260, 1.414, 1.587, 1.782], 262, 660, 0.014, 0.004, zr=1.0, zbw=0.45, tilt=0.95))
shape("Cluster", "Series & Exotic", "six semitones in a row -- a fist on the keyboard, as a filter",
      freq_res([1.0, 1.059, 1.122, 1.189, 1.260, 1.335], 330, 880, 0.012, 0.0035, zr=1.0, zbw=0.45, tilt=0.97))
shape("Quarter Tone", "Series & Exotic", "three pairs a quarter tone apart: every peak beats against its twin",
      freq_res([1.0, 1.029, 2.0, 2.059, 3.0, 3.088], 180, 440, 0.010, 0.003, zr=1.0, zbw=0.4, tilt=0.92))
shape("Fibonacci", "Series & Exotic", "1, 2, 3, 5, 8, 13 -- harmonic at the bottom, and further from it with every step",
      freq_res([1, 2, 3, 5, 8, 13], 98, 245, 0.016, 0.005, zr=1.0, zbw=0.45, tilt=0.88))

# ============================================================ extremes
shape("Self Osc", "Extremes", "Infinite an octave and a half up: two poles at the stability limit, beating against each other",
      [([600, 604], 0.0035, 0.0, 0.0, 1.0), ([3000, 3020], 0.0035, 0.0, 0.0, 1.0),
       ([600, 610], 0.010, 0.0, 0.0, 1.0), ([3000, 3060], 0.010, 0.0, 0.0, 1.0)])
shape("Screech", "Extremes", "three needles in the region the ear is most sensitive to. Handle with the Mix knob",
      freq_res([1.0, 1.31, 1.72], 2600, 5200, 0.006, 0.0018, zr=1.0, zbw=0.35, tilt=0.9))
shape("Sub Bloom", "Extremes", "two poles under 60 Hz with almost no bandwidth: a note you feel in the room rather than hear",
      freq_res([1.0, 1.5], 27, 58, 0.020, 0.004, tilt=1.0))
shape("Hollow", "Extremes", "the zero sits exactly on the pole and is narrower: the filter takes away where it would normally give",
      freq_res([1.0, 2.0, 3.0, 4.0], 200, 700, 0.50, 0.50, zr=1.0, zbw=0.04, tilt=1.0))
shape("Anti Formant", "Extremes", "deep notches where a voice would put its formants: everything but a vowel",
      [([700, 1150, 2900], 0.85, 1.0, 0.05, 1.0), ([400, 1600, 2700], 0.85, 1.0, 0.05, 1.0),
       ([450, 800, 2830], 0.85, 1.0, 0.05, 1.0), ([270, 2300, 3000], 0.85, 1.0, 0.05, 1.0)])
shape("Ghost", "Extremes", "resonances so wide they barely colour anything -- the shape you use when the point is the movement",
      freq_res([1.0, 2.3, 4.7], 200, 900, 1.20, 0.60, tilt=1.0))
shape("Ice", "Extremes", "the top octave, needle-sharp, and nothing else at all",
      freq_res([1.0, 1.26, 1.59, 2.0], 5000, 9000, 0.005, 0.0015, zr=0.35, zbw=0.7, tilt=0.85))
shape("Rumble", "Extremes", "20 to 80 Hz, wide and heavy: the floor of the instrument",
      freq_res([1.0, 1.7, 2.6], 20, 62, 0.25, 0.06, tilt=0.8))


# ============================================================ notch families
# Flanging in this filter is done with zeros, not with a delay. The idea worth stealing from the
# original's flanger cubes is the SPACING: notches at octaves fall on a harmonic's neighbours all
# at once, while notches spaced by an irrational ratio meet the harmonics one at a time as they
# sweep, which is a very different and much less obviously electronic sound.
shape("Flange Octaves", "Combs & Phasers", "five notches an octave apart, from 60 Hz up: the spacing every flanger pedal has",
      freq_res([1, 2, 4, 8, 16], 60, 240, 0.55, 0.55, zr=1.0, zbw=0.10, tilt=1.0), z="notch")
shape("Flange Golden", "Combs & Phasers", "notches spaced by 1.618 instead of by 2, so they cross the harmonics one at a time rather than all together",
      freq_res([1.0, 1.618, 2.618, 4.236, 6.854, 11.09], 40, 380, 0.50, 0.50, zr=1.0, zbw=0.09, tilt=1.0), z="notch")
shape("Flange Wide", "Combs & Phasers", "six notches that start far apart and close up as the point moves across",
      [([56, 168, 500, 1500, 4500, 13500], 0.5, 1.0, 0.10, 1.0),
       ([300, 500, 830, 1390, 2320, 3870], 0.5, 1.0, 0.10, 1.0),
       ([56, 168, 500, 1500, 4500, 13500], 0.5, 1.0, 0.05, 1.0),
       ([300, 500, 830, 1390, 2320, 3870], 0.5, 1.0, 0.05, 1.0)], z="damp")
shape("Bright Flange", "Combs & Phasers", "notches swept upward until they all converge near 15 kHz, with a bump left behind to keep the top alive",
      [([225, 450, 900, 1800, 3600], 0.45, 1.0, 0.10, 1.15),
       ([9000, 11000, 13000, 15000, 17000], 0.45, 1.0, 0.10, 1.15),
       ([225, 450, 900, 1800, 3600], 0.45, 1.0, 0.05, 1.15),
       ([9000, 11000, 13000, 15000, 17000], 0.45, 1.0, 0.05, 1.15)], z="damp")
shape("Half Octave", "Combs & Phasers", "peaks and notches alternating every half octave -- the spacing sits between a comb and a formant bank",
      freq_res([1.0, 1.414, 2.0, 2.828, 4.0, 5.657], 66, 260, 0.20, 0.05, zr=1.19, zbw=0.25, tilt=1.0), z="notch")
shape("Linear 500", "Combs & Phasers", "poles and zeros alternating at 500 Hz intervals -- linear spacing, which no resonating object has and no ear expects",
      [([500, 1000, 1500, 2000, 2500, 3000], 0.30, 1.10, 0.20, 1.0),
       ([700, 1200, 1700, 2200, 2700, 3200], 0.30, 1.10, 0.20, 1.0),
       ([500, 1000, 1500, 2000, 2500, 3000], 0.08, 1.10, 0.20, 1.0),
       ([700, 1200, 1700, 2200, 2700, 3200], 0.08, 1.10, 0.20, 1.0)], z="notch")
shape("Odd Notches", "Series & Exotic", "notches on the odd harmonics only, so what comes through is the even series: an octave up, and hollowed",
      freq_res([1, 3, 5, 7, 9, 11], 110, 300, 0.35, 0.35, zr=1.0, zbw=0.08, tilt=1.0), z="notch")
shape("Even Notches", "Series & Exotic", "the mirror of that one: notches on the even harmonics, leaving the odd series -- the clarinet trick",
      freq_res([2, 4, 6, 8, 10, 12], 110, 300, 0.35, 0.35, zr=1.0, zbw=0.08, tilt=1.0), z="notch")
shape("Odd to Even", "Series & Exotic", "X walks the peaks from the odd harmonics to the even ones, which sounds like the note jumping an octave without changing pitch",
      [([110, 330, 550, 770, 990, 1210], 0.05, 1.0, 0.45, 0.92),
       ([220, 440, 660, 880, 1100, 1320], 0.05, 1.0, 0.45, 0.92),
       ([110, 330, 550, 770, 990, 1210], 0.015, 1.0, 0.45, 0.92),
       ([220, 440, 660, 880, 1100, 1320], 0.015, 1.0, 0.45, 0.92)], z="notch")

# ============================================================ vowel space
# The one idea in the original's vowel cubes that is worth more than all the fixed vowel morphs
# put together: put F2 on one axis and F1 on the other, and every vowel there is becomes a point
# in the square rather than a corner of it.
shape("Vowel Space", "Voice", "F2 across (500 Hz to 2.5 kHz), F1 up (300 to 850 Hz): every vowel there is, as a point in the square rather than a corner of it",
      [([300, 500, 2600, 3400], 0.09, 0.0, 0.0, 0.75), ([300, 2500, 2900, 3600], 0.09, 0.0, 0.0, 0.75),
       ([850, 500, 2600, 3400], 0.09, 0.0, 0.0, 0.75), ([850, 2500, 2900, 3600], 0.09, 0.0, 0.0, 0.75)],
      z="res")
shape("Vowel Stress", "Voice", "the same space, but Transform decides how far from schwa it goes: at 0 every vowel collapses to the same relaxed centre",
      [([500, 1400, 2500, 3400], 0.10, 0.0, 0.0, 0.78), ([500, 1500, 2500, 3400], 0.10, 0.0, 0.0, 0.78),
       ([520, 1450, 2500, 3400], 0.10, 0.0, 0.0, 0.78), ([520, 1550, 2500, 3400], 0.10, 0.0, 0.0, 0.78)],
      z="spread")
shape("Yeah", "Voice", "ee to ya and back: the two vowels a spoken 'yeah' is made of",
      vowels([300, 2300, 3000, 3800], [700, 1200, 2600, 3400], [350, 2000, 2800, 3600], [800, 1150, 2800, 3500], bw=0.09))
shape("Wow", "Voice", "u to a across, and closing to i upwards -- the whole mouth in one square",
      vowels([325, 700, 2530, 3500], [800, 1150, 2800, 3500], [450, 800, 2830, 3500], [350, 1700, 2700, 3700], bw=0.10))
shape("Ee to Yi", "Voice", "two close front vowels a hair apart: a small square, and a very precise one",
      vowels([270, 2140, 2950, 3900], [350, 1700, 2700, 3700], [290, 1870, 2800, 3250], [250, 1750, 2600, 3050], bw=0.08))
shape("Roar", "Voice", "a wide open tract taken down into the chest: the vowel a lion makes",
      freq_res([1.0, 1.9, 4.6, 6.4], 180, 420, 0.16, 0.05, tilt=0.55), z="dark")
shape("Chiff", "Voice", "notches shaped for noise rather than for a note: feed it hiss and it breathes",
      freq_res([1.0, 1.7, 2.6, 3.9, 5.6], 900, 2600, 0.40, 0.40, zr=1.0, zbw=0.12, tilt=1.05), z="notch")
shape("Vocoder", "Voice", "a bank of equal-width bands across the speech range -- the response a vocoder has when every channel is open",
      freq_res([1.0, 1.6, 2.56, 4.1, 6.55, 10.5], 200, 340, 0.22, 0.07, zr=1.0, zbw=0.9, tilt=0.95))

# ============================================================ standard filter shapes
shape("Brick LP", "Sweeps", "four poles at the cutoff and a zero right above them: about as steep as six sections can be made to go",
      [([100, 100, 100, 100], 0.45, 1.6, 0.5, 1.0), ([3000, 3000, 3000, 3000], 0.45, 1.6, 0.5, 1.0),
       ([100, 100, 100, 100], 0.09, 1.6, 0.5, 1.0), ([3000, 3000, 3000, 3000], 0.09, 1.6, 0.5, 1.0)], z="res")
shape("Two Pole LP", "Sweeps", "the plain one: two poles, gentle, and the reference everything else is heard against",
      [([90, 90], 0.75, 2.2, 0.8, 1.0), ([5000, 5000], 0.75, 2.2, 0.8, 1.0),
       ([90, 90], 0.14, 2.2, 0.8, 1.0), ([5000, 5000], 0.14, 2.2, 0.8, 1.0)], z="res")
shape("HP Sweep", "Sweeps", "a high pass with real resonance at the corner, from 180 Hz up past 3 kHz",
      [([180, 260], 0.50, 0.30, 0.55, 1.05), ([3000, 4300], 0.50, 0.30, 0.55, 1.05),
       ([180, 260], 0.09, 0.30, 0.55, 1.05), ([3000, 4300], 0.09, 0.30, 0.55, 1.05)], z="res")
shape("LP to HP", "Sweeps", "X is the cutoff, Y walks the whole thing from a low pass to a high pass through a band pass in the middle",
      [([400, 560], 0.30, 3.2, 0.8, 1.0), ([2400, 3400], 0.30, 3.2, 0.8, 1.0),
       ([400, 560], 0.30, 0.32, 0.6, 1.0), ([2400, 3400], 0.30, 0.32, 0.6, 1.0)], z="res")
shape("Band-aid", "Sweeps", "a band pass that cuts both ends away: X the centre, Y the width",
      [([700], 0.90, 0.16, 0.5, 1.0), ([2200], 0.90, 0.16, 0.5, 1.0),
       ([700], 0.10, 0.16, 0.5, 1.0), ([2200], 0.10, 0.16, 0.5, 1.0)], z="res")
shape("Var Slope", "Sweeps", "one cutoff, and Y decides how hard the fall is beyond it: two poles at one end, six at the other",
      [([600, 600, 4000, 12000, 16000, 18000], 0.35, 2.4, 0.8, 1.0),
       ([2500, 2500, 8000, 14000, 17000, 19000], 0.35, 2.4, 0.8, 1.0),
       ([600, 620, 640, 660, 680, 700], 0.35, 2.4, 0.8, 1.0),
       ([2500, 2560, 2620, 2680, 2740, 2800], 0.35, 2.4, 0.8, 1.0)], z="res")
shape("All Pass", "Sweeps", "poles and zeros at the same place: nothing changes in the spectrum, everything changes in the phase",
      freq_res([1.0, 1.9, 3.7, 7.2], 300, 1400, 0.40, 0.10, zr=1.0, zbw=1.0, tilt=1.0), z="damp")

# ============================================================ EQ and speakers
shape("Bass EQ", "EQ & Speakers", "one deep notch near 200 Hz morphing into a bump at 130 and a dip at 260: the cut that makes room for a kick",
      [([196, 400], 0.55, 1.0, 0.12, 1.0), ([132, 260], 0.55, 1.0, 0.30, 1.0),
       ([196, 400], 0.20, 1.0, 0.12, 1.0), ([132, 260], 0.20, 1.0, 0.30, 1.0)], z="notch")
shape("Bass Boost", "EQ & Speakers", "a broad lift under 100 Hz with a gentle rise at 320 Hz and 5 kHz above it",
      [([60, 320, 5000], 0.70, 0.0, 0.0, 0.75), ([90, 480, 7000], 0.70, 0.0, 0.0, 0.75),
       ([60, 320, 5000], 0.30, 0.0, 0.0, 0.75), ([90, 480, 7000], 0.30, 0.0, 0.0, 0.75)], z="damp")
shape("Kick EQ", "EQ & Speakers", "a low pass with one tunable peak: the shape a bass drum is usually cut into",
      [([55, 90, 3000], 0.30, 2.0, 0.7, 0.45), ([80, 140, 4200], 0.30, 2.0, 0.7, 0.45),
       ([55, 90, 3000], 0.08, 2.0, 0.7, 0.45), ([80, 140, 4200], 0.08, 2.0, 0.7, 0.45)], z="res")
shape("Snare EQ", "EQ & Speakers", "the 320-640 Hz body lift with the low end thinned out and the wires left in",
      [([200, 480, 3800], 0.45, 0.55, 0.6, 1.10), ([260, 640, 5200], 0.45, 0.55, 0.6, 1.10),
       ([200, 480, 3800], 0.14, 0.55, 0.6, 1.10), ([260, 640, 5200], 0.14, 0.55, 0.6, 1.10)], z="res")
shape("HiHat EQ", "EQ & Speakers", "everything below 5 kHz taken away and the 5-18 kHz band lifted: cymbals and nothing else",
      [([5000, 9000, 14000], 0.55, 0.30, 0.6, 1.15), ([7000, 12000, 18000], 0.55, 0.30, 0.6, 1.15),
       ([5000, 9000, 14000], 0.16, 0.30, 0.6, 1.15), ([7000, 12000, 18000], 0.16, 0.30, 0.6, 1.15)], z="res")
shape("Speaker Cab", "EQ & Speakers", "the uneven bumps and dips of a guitar cabinet: a rise at 100, the honk near 1.8 k, and a cliff above 5 k",
      [([100, 400, 1800, 5000], 0.40, 1.25, 0.35, 0.85), ([130, 520, 2400, 6300], 0.40, 1.25, 0.35, 0.85),
       ([100, 400, 1800, 5000], 0.16, 1.25, 0.35, 0.85), ([130, 520, 2400, 6300], 0.16, 1.25, 0.35, 0.85)], z="notch")
shape("Small Speaker", "EQ & Speakers", "a plastic cone in a plastic box: nothing under 200 Hz and a hard resonance where the box gives up",
      [([220, 700, 1900, 4000], 0.35, 0.40, 0.5, 1.0), ([300, 950, 2600, 5400], 0.35, 0.40, 0.5, 1.0),
       ([220, 700, 1900, 4000], 0.10, 0.40, 0.5, 1.0), ([300, 950, 2600, 5400], 0.10, 0.40, 0.5, 1.0)], z="res")
shape("Tilt", "EQ & Speakers", "one axis from dark to bright with the middle left alone: the simplest useful thing an equaliser does",
      [([120, 800, 5000], 0.90, 0.0, 0.0, 0.45), ([120, 800, 5000], 0.90, 0.0, 0.0, 1.70),
       ([120, 800, 5000], 0.45, 0.0, 0.0, 0.45), ([120, 800, 5000], 0.45, 0.0, 0.0, 1.70)], z="damp")

# ============================================================ instruments
shape("Rhodes", "Instruments", "the tine's two-peak spectrum over a body that rolls off from about 5 kHz down to 200",
      [([200, 900, 2400, 5000], 0.28, 1.0, 0.9, 0.62), ([320, 1400, 3600, 7500], 0.28, 1.0, 0.9, 0.62),
       ([200, 900, 2400, 5000], 0.09, 1.0, 0.9, 0.62), ([320, 1400, 3600, 7500], 0.09, 1.0, 0.9, 0.62)], z="res")
shape("Piano Sustain", "Instruments", "the soundboard with the dampers off: everything rings a little, and the peaks move as the point does",
      freq_res([1.0, 1.87, 3.12, 4.98, 7.85, 12.4], 90, 210, 0.045, 0.012, zr=1.0, zbw=0.9, tilt=0.80))
shape("Vel Marimba", "Instruments", "small closely spaced peaks over one wide resonance: a mallet on wood, and the harder you hit the further up it goes",
      [([180, 700, 1660, 3200], 0.10, 1.0, 0.55, 0.55), ([300, 1170, 2760, 5300], 0.10, 1.0, 0.55, 0.55),
       ([180, 700, 1660, 3200], 0.028, 1.0, 0.55, 0.55), ([300, 1170, 2760, 5300], 0.028, 1.0, 0.55, 0.55)], z="bright")
shape("Cymbal", "Instruments", "modes so dense above 3 kHz that they stop being modes and become a wash",
      freq_res([1.0, 1.19, 1.43, 1.72, 2.07, 2.49], 3200, 6400, 0.16, 0.05, zr=1.0, zbw=0.9, tilt=1.0), z="bright")
shape("Tambourine", "Instruments", "little metal discs: a bright band with a shell resonance holding it up from below",
      [([420, 3600, 6200, 9400], 0.20, 1.0, 0.7, 1.10), ([560, 4800, 8200, 12500], 0.20, 1.0, 0.7, 1.10),
       ([420, 3600, 6200, 9400], 0.06, 1.0, 0.7, 1.10), ([560, 4800, 8200, 12500], 0.06, 1.0, 0.7, 1.10)], z="res")
shape("Shakuhachi", "Instruments", "the bamboo flute: peaks near 400, 1600, 2700 and 3150 Hz with the breath left wide open above",
      [([400, 1600, 2700, 3150], 0.13, 0.0, 0.0, 0.95), ([600, 2100, 3400, 4200], 0.13, 0.0, 0.0, 0.95),
       ([400, 1600, 2700, 3150], 0.04, 0.0, 0.0, 0.95), ([600, 2100, 3400, 4200], 0.04, 0.0, 0.0, 0.95)], z="res")
shape("Acoustic Guitar", "Instruments", "the body close-miked at the sound hole, with Y walking the microphone away from it",
      [([100, 200, 430, 2600], 0.06, 1.0, 0.8, 0.70), ([128, 256, 550, 3300], 0.06, 1.0, 0.8, 0.70),
       ([100, 200, 430, 2600], 0.22, 1.0, 0.8, 0.70), ([128, 256, 550, 3300], 0.22, 1.0, 0.8, 0.70)], z="res")
shape("Pick Position", "Instruments", "a comb whose spacing is where the string was plucked: at the bridge the teeth are wide, at the twelfth fret they are close",
      [([1600, 3200, 4800, 6400, 8000, 9600], 0.14, 1.0, 0.5, 1.0),
       ([260, 520, 780, 1040, 1300, 1560], 0.14, 1.0, 0.5, 1.0),
       ([1600, 3200, 4800, 6400, 8000, 9600], 0.04, 1.0, 0.5, 1.0),
       ([260, 520, 780, 1040, 1300, 1560], 0.04, 1.0, 0.5, 1.0)], z="notch")
shape("String Squeak", "Instruments", "a finger sliding on a wound string: a narrow band very high up that moves when you do",
      freq_res([1.0, 1.42, 2.1], 2400, 6000, 0.09, 0.020, zr=1.0, zbw=0.8, tilt=1.0), z="res")
shape("Brass Swell", "Instruments", "the formant a brass instrument grows as it gets louder: a bump near 200 Hz that opens into the midrange",
      [([200, 800, 1600, 2600], 0.28, 0.0, 0.0, 0.85), ([320, 1300, 2600, 4200], 0.28, 0.0, 0.0, 0.85),
       ([200, 800, 1600, 2600], 0.09, 0.0, 0.0, 0.85), ([320, 1300, 2600, 4200], 0.09, 0.0, 0.0, 0.85)], z="bright")
shape("French Horn", "Instruments", "unevenly spaced peaks converging onto one as the point moves: the smooth, muted swell of a horn section",
      [([180, 420, 880, 1700, 3100], 0.20, 0.0, 0.0, 0.70), ([700, 760, 830, 900, 980], 0.20, 0.0, 0.0, 0.70),
       ([180, 420, 880, 1700, 3100], 0.07, 0.0, 0.0, 0.70), ([700, 760, 830, 900, 980], 0.07, 0.0, 0.0, 0.70)], z="res")
shape("Clarinet to Oboe", "Instruments", "the resonances of one reed instrument walking across to the other's: hollow to nasal, without passing through anything in between",
      [([1500, 3000, 4500], 0.14, 0.0, 0.0, 0.80), ([1100, 2700, 4300], 0.14, 0.0, 0.0, 1.05),
       ([1500, 3000, 4500], 0.05, 0.0, 0.0, 0.80), ([1100, 2700, 4300], 0.05, 0.0, 0.0, 1.05)], z="res")
shape("Trumpet Mute", "Instruments", "a cup mute: a peak near 1 kHz, a hole under it, and how far in it is pushed on X",
      [([260, 1000, 2400, 4000], 0.30, 0.45, 0.35, 1.0), ([180, 1400, 3000, 5000], 0.30, 0.45, 0.35, 1.0),
       ([260, 1000, 2400, 4000], 0.10, 0.45, 0.35, 1.0), ([180, 1400, 3000, 5000], 0.10, 0.45, 0.35, 1.0)], z="notch")
shape("Bowed", "Instruments", "the bright, slightly rasping peak a bow builds when it digs in, over the body underneath",
      [([260, 520, 1500, 3000], 0.16, 1.0, 0.8, 0.90), ([300, 620, 2400, 5000], 0.16, 1.0, 0.8, 0.90),
       ([260, 520, 1500, 3000], 0.05, 1.0, 0.8, 0.90), ([300, 620, 2400, 5000], 0.05, 1.0, 0.8, 0.90)], z="bright")
shape("Tube Amp", "Instruments", "the response of a small valve amplifier: soft at both ends, a push around 1 kHz, and nothing above 6",
      [([90, 900, 3000, 6000], 0.55, 0.0, 0.0, 0.70), ([130, 1300, 4200, 8000], 0.55, 0.0, 0.0, 0.70),
       ([90, 900, 3000, 6000], 0.22, 0.0, 0.0, 0.70), ([130, 1300, 4200, 8000], 0.22, 0.0, 0.0, 0.70)], z="damp")
shape("Water Drop", "Instruments", "one peak sliding up fast: the sound of a drop landing, as a filter rather than as a sample",
      [([300], 0.20, 0.0, 0.0, 1.0), ([2600], 0.20, 0.0, 0.0, 1.0),
       ([300], 0.025, 0.0, 0.0, 1.0), ([2600], 0.025, 0.0, 0.0, 1.0)], z="res")
shape("Craters", "Instruments", "peaks arranged so the gaps between them read as holes: hollow, throaty, and not quite a vowel",
      freq_res([1.0, 1.31, 2.05, 3.2, 5.0], 240, 620, 0.14, 0.04, zr=1.0, zbw=1.4, tilt=0.90), z="notch")
shape("Separator", "Instruments", "three inharmonic peaks wide apart: whistling wind on noise, hollow metal on anything with a pitch",
      [([420, 1370, 4100], 0.10, 0.0, 0.0, 1.0), ([700, 2500, 8300], 0.10, 0.0, 0.0, 1.0),
       ([420, 1370, 4100], 0.020, 0.0, 0.0, 1.0), ([700, 2500, 8300], 0.020, 0.0, 0.0, 1.0)], z="spread")
shape("Diffuser", "Instruments", "one resonant peak rising while the low end fills in behind it",
      [([120, 340], 0.45, 0.0, 0.0, 1.25), ([900, 2600], 0.45, 0.0, 0.0, 1.25),
       ([120, 340], 0.07, 0.0, 0.0, 1.25), ([900, 2600], 0.07, 0.0, 0.0, 1.25)], z="res")

# ============================================================ chords as filters
# Peaks tuned to the notes of a chord: a filter that makes anything you put through it agree with
# the harmony. Sweeping X moves the voicing from root position to its inversions.
shape("Major Triad", "Series & Exotic", "peaks on a major triad, moving from root position to the first inversion across X",
      [([220, 277, 330, 440, 554, 660], 0.030, 1.0, 0.5, 0.92),
       ([277, 330, 440, 554, 660, 880], 0.030, 1.0, 0.5, 0.92),
       ([220, 277, 330, 440, 554, 660], 0.010, 1.0, 0.5, 0.92),
       ([277, 330, 440, 554, 660, 880], 0.010, 1.0, 0.5, 0.92)])
shape("Minor Triad", "Series & Exotic", "the same idea a third flatter: root position to first inversion, and everything that goes through it turns minor",
      [([220, 262, 330, 440, 523, 660], 0.030, 1.0, 0.5, 0.92),
       ([262, 330, 440, 523, 660, 880], 0.030, 1.0, 0.5, 0.92),
       ([220, 262, 330, 440, 523, 660], 0.010, 1.0, 0.5, 0.92),
       ([262, 330, 440, 523, 660, 880], 0.010, 1.0, 0.5, 0.92)])
shape("Seventh", "Series & Exotic", "a dominant seventh as six peaks: unstable on purpose, and it stays unstable however long you hold it",
      [([220, 277, 330, 392, 440, 554], 0.030, 1.0, 0.5, 0.92),
       ([330, 392, 440, 554, 660, 784], 0.030, 1.0, 0.5, 0.92),
       ([220, 277, 330, 392, 440, 554], 0.010, 1.0, 0.5, 0.92),
       ([330, 392, 440, 554, 660, 784], 0.010, 1.0, 0.5, 0.92)])
shape("Fifths", "Series & Exotic", "bare fifths stacked: no third anywhere, so nothing is major and nothing is minor",
      freq_res([1.0, 1.5, 2.25, 3.375, 5.0625, 7.59], 110, 220, 0.020, 0.006, zr=1.0, zbw=0.5, tilt=0.90))
shape("Swirly", "Combs & Phasers", "notches whose spacing widens as the point crosses, which reads as a slow turn rather than a sweep",
      [([300, 620, 960, 1320, 1700, 2100], 0.30, 1.0, 0.18, 1.0),
       ([300, 750, 1400, 2300, 3500, 5000], 0.30, 1.0, 0.18, 1.0),
       ([300, 620, 960, 1320, 1700, 2100], 0.30, 1.0, 0.07, 1.0),
       ([300, 750, 1400, 2300, 3500, 5000], 0.30, 1.0, 0.07, 1.0)], z="notch")
shape("Bell Dissonant", "Bars & Bells", "bell partials pulled off their tuning until they beat against each other",
      [([300, 601, 745, 903, 1207], 0.008, 1.0, 0.4, 0.85), ([300, 618, 772, 941, 1264], 0.008, 1.0, 0.4, 0.85),
       ([300, 601, 745, 903, 1207], 0.003, 1.0, 0.4, 0.85), ([300, 618, 772, 941, 1264], 0.003, 1.0, 0.4, 0.85)],
      z="spread")
shape("Bell Wah", "Bars & Bells", "bell peaks with a wah underneath: the metal stays, the vowel moves",
      [([260, 520, 1080, 1750, 2600], 0.020, 1.0, 0.45, 0.60),
       ([260, 520, 1080, 1750, 2600], 0.020, 1.0, 0.45, 1.40),
       ([420, 840, 1750, 2830, 4200], 0.020, 1.0, 0.45, 0.60),
       ([420, 840, 1750, 2830, 4200], 0.020, 1.0, 0.45, 1.40)], z="res")


# ============================================================ presets
# One preset per filter, because a bank of a hundred and fifty-five shapes with no way in is a
# bank nobody uses: every shape gets a starting point that suits its family -- where the point
# sits, how far it wanders, how sharp, how much of it you hear. The name is the shape's name, so
# the preset list and the filter list read the same way.
FAMILY_SETTINGS = {
    #                 x     y     z    res   mix   rate  depth
    "Voice":         (0.30, 0.40, 0.15, 0.55, 0.85, 0.030, 0.40),
    "Sweeps":        (0.35, 0.55, 0.10, 0.60, 0.90, 0.040, 0.55),
    "Combs & Phasers": (0.40, 0.50, 0.20, 0.50, 0.65, 0.025, 0.60),
    "Strings & Bodies": (0.45, 0.55, 0.15, 0.70, 0.85, 0.020, 0.30),
    "Bars & Bells":  (0.40, 0.60, 0.15, 0.72, 0.80, 0.015, 0.28),
    "Membranes":     (0.40, 0.50, 0.15, 0.65, 0.80, 0.020, 0.30),
    "Tubes & Pipes": (0.40, 0.55, 0.15, 0.68, 0.85, 0.020, 0.32),
    "Rooms & Spaces": (0.45, 0.45, 0.15, 0.60, 0.70, 0.012, 0.35),
    "Series & Exotic": (0.40, 0.55, 0.15, 0.72, 0.80, 0.018, 0.35),
    "Extremes":      (0.35, 0.35, 0.10, 0.45, 0.50, 0.020, 0.30),
    "Instruments":   (0.40, 0.50, 0.15, 0.65, 0.85, 0.022, 0.30),
    "EQ & Speakers": (0.45, 0.45, 0.10, 0.40, 1.00, 0.010, 0.20),
}


def write_presets():
    """The Z-plane preset bank, as a C++ table. Only z_* keys: the bank is a layer that lands on
    top of whatever sound is loaded, the way the Cosmos bank does."""
    out = os.path.join(ROOT, "Core", "src", "ZPlanePresets.inc")
    nl = chr(10)
    # The families whose shapes are physical objects get a second preset each, in Modal mode:
    # the same mode series, read as a bank of resonators that rings rather than a filter that
    # shapes. Decay and damping per family, because a bell is not a drum is not a room.
    MODAL = {
        "Bars & Bells":     (6.0, 0.75),
        "Strings & Bodies": (2.5, 0.70),
        "Membranes":        (1.2, 0.85),
        "Tubes & Pipes":    (3.0, 0.60),
        "Rooms & Spaces":   (8.0, 0.50),
    }
    lines = ["// Generated by Tools/make_zplane_bank.py -- do not edit.",
             "// One preset per filter shape (%d), in the same families the shapes are grouped by." % len(SHAPES),
             "const Preset kZPresets[] = {",
             '    { "Z-Plane Off", "z_mode=Off" },']
    cats = []
    for s in SHAPES:
        x, y, z, res, mix, rate, depth = FAMILY_SETTINGS[s["cat"]]
        settings = ("z_mode=Series;z_shape=%s;z_x=%g;z_y=%g;z_z=%g;z_res=%g;z_mix=%g;z_rate=%g;z_depth=%g"
                    % (s["name"], x, y, z, res, mix, rate, depth))
        lines.append('    { "%s", "%s" },' % (s["name"], settings))
        cats.append(CATEGORIES.index(s["cat"]))
        if s["cat"] in MODAL:
            decay, damp = MODAL[s["cat"]]
            modal = ("z_mode=Modal;z_shape=%s;z_x=%g;z_y=%g;z_z=%g;z_res=%g;z_mix=%g;z_rate=%g;"
                     "z_depth=%g;z_decay=%g;z_damp=%g"
                     % (s["name"], x, y, z, res, 1.0, rate * 0.5, depth * 0.5, decay, damp))
            lines.append('    { "%s (modal)", "%s" },' % (s["name"], modal))
            cats.append(CATEGORIES.index(s["cat"]))
    lines.append("};")
    lines.append("")
    lines.append("// The family each preset belongs to, for the section headings in the list.")
    lines.append("// The first entry (Off) has none, which is what 255 means here.")
    lines.append("const unsigned char kZPresetCategory[] = {")
    lines.append("    255,")
    for i in range(0, len(cats), 12):
        lines.append("    " + " ".join("%d," % c for c in cats[i:i + 12]))
    lines.append("};")
    lines.append("")
    with open(out, "w", encoding="utf-8") as f:
        f.write(nl.join(lines))
    print("wrote %s: %d presets" % (out, len(SHAPES) + 1))


# ============================================================ output
def main():
    nl = chr(10)
    names, cats, corners = [], [], []
    for s in SHAPES:
        names.append('    "%s",' % s["name"])
        cats.append(CATEGORIES.index(s["cat"]))
    lines = []
    lines.append("// Generated by Tools/make_zplane_bank.py -- do not edit.")
    lines.append("// %d shapes in %d families. The ratios and the acoustics behind every one of them,"
                 % (len(SHAPES), len(CATEGORIES)))
    lines.append("// and the reason this is generated rather than typed, are in that file.")
    lines.append("")
    lines.append("const char* const kZCategoryNames[kZCategories] = {")
    for c in CATEGORIES:
        lines.append('    "%s",' % c)
    lines.append("};")
    lines.append("")
    lines.append("const char* const kZShapeNames[kZShapes] = {")
    for s in SHAPES:
        lines.append('    "%s",' % s["name"])
    lines.append("};")
    lines.append("")
    lines.append("// Which family each shape belongs to. Display only: the parameter's numbering is")
    lines.append("// historical and frozen, the editor sorts the list into families for the eye.")
    lines.append("const unsigned char kZShapeCategory[kZShapes] = {")
    for i in range(0, len(cats), 12):
        lines.append("    " + " ".join("%d," % c for c in cats[i:i + 12]))
    lines.append("};")
    lines.append("")
    lines.append("// Eight corners of a cube: (0,0) (1,0) (0,1) (1,1) at Transform 0, then the same")
    lines.append("// four at Transform 1.  { frequencies... }, bandwidth ratio, zero ratio, zero")
    lines.append("// bandwidth ratio, tilt")
    lines.append("const ZCornerSpec kZCorners[kZShapes][8] = {")
    for s in SHAPES:
        lines.append("    {   // %s: %s  [Transform: %s]" % (s["name"], s["note"], s["zrule"]))
        for hz, bw, zr, zbw, tilt in s["corners"]:
            hzs = ", ".join("%g" % h for h in hz)
            lines.append("        { { %s }, %.4ff, %.4ff, %.4ff, %.3ff }," % (hzs, bw, zr, zbw, tilt))
        lines.append("    },")
    lines.append("};")
    lines.append("")
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(nl.join(lines))
    write_presets()
    print("wrote %s: %d shapes, %d families" % (OUT, len(SHAPES), len(CATEGORIES)))
    for c in CATEGORIES:
        print("  %-18s %d" % (c, sum(1 for s in SHAPES if s["cat"] == c)))


if __name__ == "__main__":
    main()
