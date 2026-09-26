"""The circuit filters (26.09.2026), for a preset's parameters: which presets move to them, and where.

Noctuary got five filter models from Ephemeris that are circuits rather than textbook filters --
Moog, SEM, Prophet, Juno, Diode (Core/include/ambient/CircuitFilter.h). Rene heard the old Ladder
against the Moog and asked for the move, and for other presets to go to the other four as well, the
packs' among them, "aber nur die, die bislang nicht ausschliesslich den z-Filter benutzen". One table
used twice, like guide.py and review.py: make_presets.py passes every preset it generates through
apply(), and retrofit_circuit.py every preset written before -- the packs by their artist's family,
the built-ins by the family their section stands in for, the near bank (make_layer_presets.py) with
its own keys.

Which presets move:
    Ladder   every one, to the Moog -- the same four-pole ladder with its loop solved instead of
             broken by a sample of delay. At 1.55 times the Cutoff and the Resonance squared: where
             the old ladder had its corner and its feedback (k = 3.92 r^2 there, 3.94 r' here), the
             setting the A/B renders of 26.09.2026 were made with.
    LP 24    seven in ten, to a four-pole circuit by the family: the cold ones to the diode ladder,
             the deep and ritual ones to the Moog, the luminous and sleeping ones to the Juno, the rest
             between Prophet and Juno.
    Notch    six in ten, to the SEM at Morph 0.5 -- the SEM's notch is the one the instrument was
             famous for.
    HP 12    one in two, to the SEM at Morph 1.
    LP 12    one in five, to the SEM at Morph 0 -- the same two poles, with the SEM's saturating
             integrators. LP 12 is the voice's neutral default, and a circuit costs thirty times its
             cycles, so most of the library keeps it.
    the rest stays: LP 6, BP 12, Peak, Comb and Formant have no circuit among the five.
The near bank moves its Ladder and LP 24 presets only.
Which one moves is drawn from a hash of the preset's name, so a preset always gets the same answer.
Not moved: a preset whose voice filter is not heard -- a Replace z-plane, or a parallel one at a
Mix of 0.95 and more -- since what it would be moved to would only be heard once someone turned the
z-plane down, and then as a surprise.

Where the knobs go. The SEM is the state-variable filter the LP 12, HP 12 and Notch already are,
small-signal: the same two poles, so Cutoff stays and Resonance is set to the same Q
(k = 2 - 1.9 r there, 2R = 1.414 (1 - r') + 0.024 here). The four-pole circuits are fitted to the
LP 24's curve: for each Resonance the Cutoff factor and Resonance of the circuit whose small-signal
magnitude, from an octave and a half below Cutoff to two above, lies closest to the LP 24's in
decibels, both taken relative to their pass band. What the fit cannot move is the pass band
itself: a ladder or a cascade loses 1 / (1 + k) to its feedback and makes back only a part of it,
where the LP 24 keeps unity. That difference is written into master_gain, so the preset comes out
about as loud as it was without being measured again (Rene: "das Neuvermessen koennen wir uns
trotzdem sparen"). Checked on a sample in retrofit_circuit.py, the reckoning held for everything
but the diode ladder and one SEM high pass, and the old Ladder turned out to have been measured
wrongly: every move but LP 12 to the SEM was rendered on both filters instead, and corrected by that.
"""
import functools
import hashlib

import numpy as np

SR = 48000.0
MODELS = ("Moog", "SEM", "Prophet", "Juno", "Diode")
LADDER_CUTOFF = 1.55
SHARE = {"Ladder": 1.0, "LP 24": 0.7, "Notch": 0.6, "HP 12": 0.5, "LP 12": 0.2}
SEM_MORPH = {"LP 12": 0.0, "Notch": 0.5, "HP 12": 1.0}
# The four-pole circuit an LP 24 goes to, by the family (review.py's families; None for a preset
# without one, the near bank's among them): weights that add up to one.
LP24_TARGETS = {
    "deep":     (("Moog", 0.5), ("Prophet", 0.3), ("Diode", 0.2)),
    "ritual":   (("Moog", 0.4), ("Diode", 0.3), ("Prophet", 0.3)),
    "cold":     (("Diode", 0.5), ("Prophet", 0.3), ("Moog", 0.2)),
    "brit":     (("Prophet", 0.5), ("Juno", 0.3), ("Moog", 0.2)),
    "luminous": (("Juno", 0.6), ("Prophet", 0.4)),
    "space":    (("Juno", 0.4), ("Prophet", 0.4), ("Moog", 0.2)),
    "sleep":    (("Juno", 0.7), ("Moog", 0.3)),
    None:       (("Prophet", 0.5), ("Juno", 0.5)),
}
Z_ONLY_MIX = 0.95

# Keys by layer: the voice's, and the near bank's (fore_*), which has no Morph and no z-plane and
# whose level is its own Gain. The defaults and ranges are Params.cpp's. The near bank moves only
# its ladders: its LP 12 is "the plain 12 dB for everything that should simply be near" (Help.cpp),
# and without a Morph it has no SEM notch or high pass to go to.
VOICE = {"model": "filter_model", "cutoff": "cutoff", "res": "resonance", "morph": "filter_morph",
         "gain": "master_gain", "gain_range": (-40.0, 12.0), "default_gain": -6.0,
         "default_model": "LP 12", "default_cutoff": 2500.0, "default_res": 0.15,
         "moves": ("Ladder", "LP 24", "Notch", "HP 12", "LP 12")}
NEAR = {"model": "fore_filter", "cutoff": "fore_cutoff", "res": "fore_resonance", "morph": None,
        "gain": "fore_gain", "gain_range": (-24.0, 36.0), "default_gain": 0.0,
        "default_model": "LP 12", "default_cutoff": 8000.0, "default_res": 0.1,
        "moves": ("Ladder", "LP 24")}


def draw(name, key):
    """A number in [0, 1) that belongs to this preset and this key, for ever."""
    h = hashlib.sha1(f"{name}|circuit|{key}".encode("utf-8")).digest()
    return int.from_bytes(h[:8], "big") / float(1 << 64)


# ---------------------------------------------------------------- small-signal curves (Filter.cpp)
def _warp(f, fc, rate):
    return 1j * np.tan(np.pi * f / (rate * SR)) / np.tan(np.pi * fc / (rate * SR))


def lp24(f, fc, r):
    """The LP 24: two state-variable low passes, the resonance shared (k = 2 - 1.9 * 0.75 r each)."""
    s = _warp(f, fc, 1.0)
    k = 2.0 - 1.9 * 0.75 * r
    h = 1.0 / (s * s + k * s + 1.0)
    return np.abs(h * h)


def ladder4(f, fc, k, makeup):
    """Moog, Prophet and Juno small-signal: four one-poles at twice the rate, the fourth fed back."""
    s = _warp(f, fc, 2.0)
    h = 1.0 / (1.0 + s)
    h4 = h ** 4
    return np.abs(h4 / (1.0 + k * h4)) * makeup


def diode(f, fc, r):
    """The diode ladder small-signal: the four nodes solved from the last one up."""
    k = 17.5 * r
    s = _warp(f, fc, 2.0) / 0.70710678
    a = s + 2.0
    v2 = 0.5 * a
    v1 = a * v2 - 1.0
    v0 = a * v1 - v2
    u = a * v0 - v1
    return np.abs(1.0 / (u + k)) * (1.0 + 0.3 * k)


def circuit_curve(model, f, fc, r):
    if model == "Moog":
        k = 4.0 * r * 0.985
        return ladder4(f, fc, k, 1.0 + 0.5 * k)
    if model in ("Prophet", "Juno"):
        k = 4.1 * r
        return ladder4(f, fc, k, 1.0 + (0.25 if model == "Juno" else 0.5) * k)
    if model == "Diode":
        return diode(f, fc, r)
    raise ValueError(model)


# ---------------------------------------------------------------- the fits
_FC = 1000.0
_F = _FC * np.logspace(np.log10(1.0 / 3.0), np.log10(4.0), 120)   # an octave and a half below Cutoff to two above
_F_PASS = np.array([_FC / 16.0])
_R_GRID = np.linspace(0.0, 1.0, 41)


def _db(x):
    return 20.0 * np.log10(np.maximum(x, 1e-6))


@functools.lru_cache(maxsize=None)
def lp24_table(model):
    """For each LP 24 Resonance on _R_GRID: (Cutoff factor, Resonance, pass-band change in dB) of the
    circuit whose small-signal curve, relative to its pass band, is nearest the LP 24's."""
    scales = np.exp(np.linspace(np.log(0.5), np.log(2.5), 81))
    res = np.linspace(0.0, 0.98, 50)   # short of self-oscillation
    # every candidate's curve, relative to its own pass band, once
    cand = np.empty((scales.size, res.size, _F.size))
    passband = np.empty((scales.size, res.size))
    for i, c in enumerate(scales):
        for j, r in enumerate(res):
            pb = circuit_curve(model, _F_PASS, _FC * c, r)[0]
            passband[i, j] = pb
            cand[i, j] = np.maximum(_db(circuit_curve(model, _F, _FC * c, r) / pb), -40.0)
    out = []
    for r in _R_GRID:
        want = np.maximum(_db(lp24(_F, _FC, r) / lp24(_F_PASS, _FC, r)[0]), -40.0)
        err = ((cand - want) ** 2).mean(axis=2)
        i, j = np.unravel_index(np.argmin(err), err.shape)
        pb_old = lp24(_F_PASS, _FC, r)[0]
        out.append((float(scales[i]), float(res[j]), float(_db(passband[i, j] / pb_old))))
    return tuple(out)


def lp24_to(model, r):
    """(Cutoff factor, Resonance, pass-band change in dB) for an LP 24 at Resonance r, interpolated."""
    t = lp24_table(model)
    x = min(max(r, 0.0), 1.0) * (len(_R_GRID) - 1)
    i = min(int(x), len(_R_GRID) - 2)
    w = x - i
    return tuple((1.0 - w) * a + w * b for a, b in zip(t[i], t[i + 1]))


def sem_resonance(r):
    """The SEM's Resonance for the Q of a state-variable filter at Resonance r."""
    return min(max(1.0 - (1.976 - 1.9 * r) / 1.414, 0.0), 1.0)


def ladder_to_moog(cutoff, r):
    """The old Ladder to the Moog: its corner and its feedback where they were."""
    return cutoff * LADDER_CUTOFF, r * r


# ---------------------------------------------------------------- the rule
def _num(p, key, default):
    try:
        return float(p.get(key, default))
    except (TypeError, ValueError):
        return default


def z_only(p):
    """The voice filter is not heard: a Replace z-plane, or a parallel one at full Mix."""
    z = p.get("z_mode", "Off")
    if z == "Replace":
        return True
    return z != "Off" and p.get("z_route", "Series") == "Parallel" and _num(p, "z_mix", 0.7) >= Z_ONLY_MIX


def target(model, family, name):
    """The circuit a preset's model goes to, or None: drawn from the name."""
    share = SHARE.get(model)
    if share is None or draw(name, "move") >= share:
        return None
    if model == "Ladder":
        return "Moog"
    if model in SEM_MORPH:
        return "SEM"
    options = LP24_TARGETS.get(family, LP24_TARGETS[None])
    x, acc = draw(name, "which"), 0.0
    for m, w in options:
        acc += w
        if x < acc:
            return m
    return options[-1][0]


def fmt(v):
    return f"{v:.4g}"


def apply(p, family, name, keys=VOICE, measured=None):
    """Move one preset's filter to a circuit model where the rule says so. `p` is its settings as an
    ordered dict of text; returns what was done ("Ladder->Moog", ...) or None. Idempotent: a preset
    already on a circuit model is left alone. `measured` maps a preset's name to how many decibels
    louder it measured on its circuit than on the filter it had (retrofit_circuit.py --measure);
    where it has the preset, that is what its level is corrected by instead of the reckoning."""
    model = p.get(keys["model"], keys["default_model"])
    if model in MODELS or model not in keys["moves"]:
        return None
    if keys is VOICE and model != "Ladder" and z_only(p):
        return None
    new = target(model, family, name)
    if new is None:
        return None
    cutoff = _num(p, keys["cutoff"], keys["default_cutoff"])
    r = _num(p, keys["res"], keys["default_res"])
    gain_db = 0.0
    if model == "Ladder":
        cutoff, r = ladder_to_moog(cutoff, r)
    elif new == "SEM":
        r = sem_resonance(r)
        if keys["morph"]:
            p[keys["morph"]] = fmt(SEM_MORPH[model])
    else:
        c, r, gain_db = lp24_to(new, r)
        cutoff *= c
    if measured and name in measured:
        gain_db = measured[name]
    p[keys["model"]] = new
    p[keys["cutoff"]] = fmt(min(max(cutoff, 40.0), 18000.0))
    p[keys["res"]] = fmt(min(max(r, 0.0), 1.0))
    if gain_db:
        # What the circuit's pass band gives up, given back where the preset's level is set.
        lo, hi = keys["gain_range"]
        g = _num(p, keys["gain"], keys["default_gain"]) - gain_db
        p[keys["gain"]] = fmt(min(max(g, lo), hi))
    return f"{model}->{new}"


def parse(settings):
    out = {}
    for kv in settings.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def serialise(p):
    return ";".join(f"{k}={v}" for k, v in p.items())


def apply_to_settings(settings, family, name, keys=VOICE, measured=None):
    """The same over a settings string; returns (new string, what was done or None). "Init" stays empty."""
    if not settings.strip():
        return settings, None
    p = parse(settings)
    what = apply(p, family, name, keys, measured)
    return (serialise(p), what) if what else (settings, None)


if __name__ == "__main__":
    for m in ("Moog", "Prophet", "Juno", "Diode"):
        print(m)
        for r in (0.0, 0.2, 0.4, 0.6, 0.8, 1.0):
            c, rr, g = lp24_to(m, r)
            print(f"   LP 24 res {r:.1f} -> cutoff x{c:.2f}, res {rr:.2f}, pass band {g:+.1f} dB")
    for r in (0.0, 0.3, 0.5, 0.8, 1.0):
        print(f"SEM for a state-variable res {r:.1f}: {sem_resonance(r):.2f}")
