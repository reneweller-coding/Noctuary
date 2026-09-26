"""The review of the conductor against ambient harmony, for a preset's parameters (25.09.2026).

The review ("Review: Cluster Brain vs. Ambient-Harmonielehre", 25.09.2026) found eight places where
a model from tonal music psychology was set on modal, just, register-bound drone music. Most of its
fixes are in the engine (ClusterBrain.h, Tuning.cpp) and reach every preset by themselves. Five are
ranges -- rows of Tools/library/conductor_ranges.md -- and those are here, in one table used twice
like guide.py's: make_presets.py passes every preset it generates through apply(), and
retrofit_review.py every preset that was written before, the packs by their artist's family and the
built-ins by the family their section stands in for (conductor.BUILTIN_FAMILY).

    purity_adapt        F3: 0.4 .. 0.8 wherever the root moves (brain_wander or auto_root_move
                        above nought -- both are, by default). The brain's root wandering across a
                        just scale anchored at the tuning root makes wolf intervals; adaptive
                        intonation is the answer, and the generator wrote it one preset in five.
    brain2_low/high     F7, deep only: the background conductor's register from MIDI 20 (26 Hz) to
                        at most 40. It was 12 .. 24 at the bottom -- 16 to 33 Hz, which no speaker
                        plays and every limiter hears.
    brain_root_targets  F8: which intervals the root steps to. Modal (fifth 40 %, fourth 30 %, whole
                        tone 25 %) for deep, cold and the rest; Mediant (thirds and sixths 30 %) for
                        luminous; Phrygian (the falling semitone a fifth of the time) for ritual.
    brain_utonal        F2b: undertone sets count as rooted -- the dark families' sound.
    brain_series        section 4: the Harmonic Cloud, for sleep and space (Rich, Stearns).
    arc_harmony         section 4: the tension arc over the long form -- the hour's arc loosening
                        consonance, key and rootedness as it rises. The engine had it; the ranges
                        never routed it.

Fuzzy on purpose: a value that is drawn is drawn inside its family's window from a hash of the
preset's name and the key, so the same preset always gets the same value and two presets of one
pack do not all get the same one; a value that is already inside its window stays where it is.
"""
import hashlib

FAMILIES = ("sleep", "deep", "ritual", "cold", "brit", "luminous", "space")

ROOT_TARGETS = {"sleep": "Modal", "deep": "Modal", "ritual": "Phrygian", "cold": "Modal",
                "brit": "Modal", "luminous": "Mediant", "space": "Modal"}
UTONAL = {"sleep": (0.0, 0.2), "deep": (0.6, 0.9), "ritual": (0.4, 0.7), "cold": (0.3, 0.6),
          "brit": (0.0, 0.2), "luminous": (0.0, 0.1), "space": (0.0, 0.2)}
SERIES = {"sleep": (0.2, 0.5), "deep": (0.0, 0.1), "ritual": (0.0, 0.1), "cold": (0.0, 0.1),
          "brit": (0.0, 0.1), "luminous": (0.0, 0.2), "space": (0.2, 0.5)}
ARC_HARMONY = {"sleep": (0.2, 0.4), "deep": (0.1, 0.3), "ritual": (0.2, 0.4), "cold": (0.0, 0.2),
               "brit": (0.0, 0.15), "luminous": (0.3, 0.5), "space": (0.3, 0.5)}
PURITY_ADAPT = (0.4, 0.8)
DEEP_BRAIN2_LOW, DEEP_BRAIN2_HIGH = 20, 40

# The engine's defaults for the keys the rules read, where a preset does not name them (Params.cpp).
DEFAULTS = {"brain_wander": 0.3, "auto_root_move": 0.2, "brain2_low": 28, "brain2_high": 58,
            "purity_adapt": 0.0, "brain_utonal": 0.0, "brain_series": 0.0, "arc_harmony": 0.0}


def draw(name, key):
    """A number in [0, 1) that belongs to this preset and this key, for ever."""
    h = hashlib.sha1(f"{name}|{key}".encode("utf-8")).digest()
    return int.from_bytes(h[:8], "big") / float(1 << 64)


def _num(p, key):
    try:
        return float(p.get(key, DEFAULTS.get(key, 0.0)))
    except (TypeError, ValueError):
        return float(DEFAULTS.get(key, 0.0))


def _fmt(v):
    return f"{v:.4g}"


def apply(p, family, name):
    """Bring one preset's conductor ranges into the review's windows. `p` maps key -> value text in
    the preset's own order; `family` is one of FAMILIES (anything else leaves the preset alone);
    `name` seeds the draws. Returns the number of keys changed."""
    if family not in FAMILIES:
        return 0
    changed = 0

    def put(key, text):
        nonlocal changed
        if p.get(key) != text:
            p[key] = text
            changed += 1

    def window(key, lo, hi):
        """Inside the window it stays; outside, or not set at all, it is drawn inside it."""
        v = _num(p, key)
        if key in p and lo <= v <= hi:
            return
        if hi <= 0.0:
            if key in p and v != 0.0:
                put(key, _fmt(0.0))
            return
        put(key, _fmt(lo + (hi - lo) * draw(name, key)))

    # F3: adaptive intonation wherever the root moves.
    if _num(p, "brain_wander") > 0.0 or _num(p, "auto_root_move") > 0.0:
        window("purity_adapt", *PURITY_ADAPT)
    # F7: the deep family's background register above 26 Hz, the ordering of the old range kept.
    if family == "deep":
        lo, hi = _num(p, "brain2_low"), _num(p, "brain2_high")
        if lo < DEEP_BRAIN2_LOW:
            lo = DEEP_BRAIN2_LOW + round(4.0 * max(0.0, lo - 12.0) / 12.0)
            put("brain2_low", str(int(lo)))
        if hi > DEEP_BRAIN2_HIGH:
            hi = 36 + round(4.0 * min(1.0, max(0.0, hi - 36.0) / 12.0))
        hi = max(hi, lo + 12)
        if _num(p, "brain2_high") != hi:
            put("brain2_high", str(int(hi)))
    # F8, F2b, section 4.
    put("brain_root_targets", ROOT_TARGETS[family])
    window("brain_utonal", *UTONAL[family])
    window("brain_series", *SERIES[family])
    window("arc_harmony", *ARC_HARMONY[family])
    return changed


def parse(settings):
    out = {}
    for kv in settings.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def serialise(p):
    return ";".join(f"{k}={v}" for k, v in p.items())


def apply_to_settings(settings, family, name):
    """The same over a settings string; returns (new string, keys changed). "Init" stays empty."""
    if not settings.strip():
        return settings, 0
    p = parse(settings)
    n = apply(p, family, name)
    return serialise(p), n
