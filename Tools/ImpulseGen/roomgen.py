"""RoomGen: impulse responses for Noctuary's convolution Room, from the statistical model of late
reverberation (Polack 1993; Moorer 1979; Jot 1992).

Why this model. The research round of 11.09.2026 measured the alternatives: neural room generators give a
quarter of a second at 16 kHz (or no weights at all), a texture under an envelope rings at the texture's
pitches, geometric simulation stays sparse for seconds, and a static delay network turns metallic at long
decays. What real late reverberation is, measured across hundreds of rooms, is Gaussian noise whose every
frequency band decays exponentially at its own rate (Traer and McDermott 2016). That is what this makes,
directly, at 48 kHz and up to a minute long.

One design per file:
  decay      T60 anchored at 500 Hz and 1 kHz (T_mid); 250, 125 and 63 Hz as ratios of it, held at or under
             1.1 x T_mid so no room turns to mud (PCHIP between the anchors); above 1 kHz the walls take more
             (hf_extra) and the air takes what ISO 9613-1 says it takes at the design's temperature and humidity
  colour     the initial spectrum's three knobs (slope below 500 Hz, slope above 1 kHz, a sub shelf) solved by
             bounded least squares for three long-term targets measured rooms have: energy below 150 Hz, the
             share between 150 and 500 Hz, the power centroid -- and a ceiling on the first 300 ms's centroid.
             A target the knobs cannot reach is flagged and the design redrawn, never clamped in silence
  stereo     the two ears' coherence per frequency, half-coherent in the bass and independent above it
  families   chamber, hall, cathedral, cavern, vast, plate (single slopes); bloom (a small room coupled to a
             huge one: dense start, long quiet hang) and far (the source in the huge one: the sound fades in),
             after Cremer and Mueller's coupled volumes; drift (a colour gliding down as the room dies) and
             swell (every band rising towards a release)
  pairs      a and b partners share their seed and their noise, so Room Morph between them never dips (the
             morph is a linear crossfade, and two independent noises would lose 3 dB half way): "grow" (b is
             the same room larger), "darken" (b the same room darker)

Every file passes the acceptance tests of the design before it is written -- octave decay against the design,
bass and low-mid ratios, treble, straightness, echo density, no fixed pitches (peakiness, spectral lines,
a T60-aware note spread), colour, coherence, clean ends -- or it is redrawn.

    python roomgen.py generate --out DIR [--count 1000] [--families hall,vast] [--jobs 12] [--tag roomgen-1]
    python roomgen.py default [--wav built_in_hall.wav]     the built-in hall's design, and its acceptance
    python roomgen.py check FILE.wav                         a file against the design in its .json
    python roomgen.py selftest
"""
import argparse
import collections
import concurrent.futures as cf
import json
import math
import os
import sys
import time
import zlib

for _var in ("OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ.setdefault(_var, "1")     # the processes are the parallelism: one maths thread each

import numpy as np
import scipy
import scipy.fft as sfft
import scipy.signal as ss
from scipy.interpolate import PchipInterpolator
from scipy.optimize import least_squares
from scipy.special import polygamma

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "library"))
import ir_metrics  # noqa: E402

SR = 48000
C = 343.2
LN1E6 = math.log(1e6)                     # energy is 60 dB down where the exponent reaches this
OCT = (63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000)
TAG = "roomgen-1"


# ---------------------------------------------------------------------------- physics

def iso9613_db_per_m(f, T_c=20.0, rh=50.0, pa=101.325):
    """Air absorption, ISO 9613-1 (defined 50 Hz..10 kHz; used above as an extrapolation, said so in every .json)."""
    f = np.asarray(f, dtype=float)
    T, T0, T01, pr = T_c + 273.15, 293.15, 273.16, 101.325
    csat = -6.8346 * (T01 / T) ** 1.261 + 4.6151
    h = rh * (10 ** csat) / (pa / pr)
    fro = (pa / pr) * (24 + 4.04e4 * h * (0.02 + h) / (0.391 + h))
    frn = (pa / pr) * (T / T0) ** -0.5 * (9 + 280 * h * np.exp(-4.170 * ((T / T0) ** (-1 / 3) - 1)))
    return 8.686 * f ** 2 * (1.84e-11 * (pa / pr) ** -1 * (T / T0) ** 0.5
                             + (T / T0) ** -2.5 * (0.01275 * np.exp(-2239.1 / T) / (fro + f ** 2 / fro)
                                                   + 0.1068 * np.exp(-3352.0 / T) / (frn + f ** 2 / frn)))


def t_air(f, T_c=20.0, rh=50.0):
    """The T60 air alone allows, whatever the room: ln(1e6) / (c m)."""
    return LN1E6 / (C * iso9613_db_per_m(f, T_c, rh) * math.log(10.0) / 10.0)


def hs(f):
    f = np.asarray(f, dtype=float)
    return (f / 2000.0) ** 2 / (1.0 + (f / 2000.0) ** 2)


def logf_interp(f, fr, vals):
    return np.interp(np.log2(np.maximum(np.asarray(f, dtype=float), 1.0)), np.log2(np.asarray(fr, dtype=float)),
                     np.asarray(vals, dtype=float))


def softknee(x, k=2.0):
    return np.logaddexp(0.0, k * np.asarray(x, dtype=float)) / k


def fade(n, ms=100.0):
    k = max(1, min(n // 10, int(SR * ms / 1000.0)))
    w = np.ones(n)
    w[-k:] = np.cos(np.linspace(0.0, 0.5 * np.pi, k)) ** 2
    return w


def grid_fade2(ng, dt, ms=100.0):
    w = np.ones(ng)
    k = max(1, min(ng // 10, int(ms / 1000.0 / dt)))
    w[-k:] = np.cos(np.linspace(0.0, 0.5 * np.pi, k)) ** 4
    return w


# ---------------------------------------------------------------------------- the design

class Design:
    def __init__(self, family, T_mid=3.0, br63=0.9, br125=0.9, br250=1.0, hf_extra=0.5, rh=50.0, temp=20.0, air=True,
                 t60_profile=None, f_hp=45.0, f_lp=14000.0, rho_lo=0.5, rho_hi=0.05, f_rho=250.0, onset_ms=20.0,
                 coupled=None, sweep=None, swell=None, target=None, max_seconds=60.0, wall_min=0.05):
        self.family, self.T_mid, self.br63, self.br125, self.br250 = family, T_mid, br63, br125, br250
        self.hf_extra, self.rh, self.temp, self.air, self.t60_profile = hf_extra, rh, temp, air, t60_profile
        self.f_hp, self.f_lp, self.rho_lo, self.rho_hi, self.f_rho = f_hp, f_lp, rho_lo, rho_hi, f_rho
        self.onset_ms, self.coupled, self.sweep, self.swell, self.target = onset_ms, coupled, sweep, swell, target
        self.max_seconds, self.wall_min = max_seconds, wall_min
        self.a_lo, self.a_hi, self.g_sub = 0.0, 3.0, 0.0
        self._pc = self._r1k = self._secs = None
        self._int = {}

    # ---- decay
    def rair(self, f):
        f = np.asarray(f, dtype=float)
        return 1.0 / t_air(f, self.temp, self.rh) if self.air else np.zeros(f.shape)

    def law(self):
        if self._pc is None:
            tm = self.T_mid
            r1k = 1.0 / tm - float(self.rair([1000.0])[0])
            if r1k < self.wall_min / tm:
                raise ValueError(f"T_mid {tm:.1f} s is beyond what the air allows at 1 kHz")
            fa = np.array([20.0, 63.0, 125.0, 250.0, 500.0, 1000.0])
            rate = np.array([1.0 / (r * tm) for r in (self.br63, self.br63, self.br125, self.br250, 1.0, 1.0)])
            self._pc = PchipInterpolator(np.log2(fa), np.log(rate))
            self._r1k = r1k
            fchk = np.geomspace(20.0, 1000.0, 400)
            if (np.exp(self._pc(np.log2(fchk))) - self.rair(fchk)).min() < 0.0:
                raise ValueError("negative wall absorption below 1 kHz")
        return self._pc

    def t60(self, f):
        f = np.clip(np.asarray(f, dtype=float), 20.0, 24000.0)
        if self.t60_profile is not None:
            fr, mul = self.t60_profile
            return self.T_mid * logf_interp(f, fr, mul)
        pc = self.law()
        lo = np.exp(-pc(np.log2(np.minimum(f, 1000.0))))
        h1 = float(hs(1000.0))
        hi = 1.0 / (self._r1k * (1.0 + self.hf_extra * (hs(f) - h1) / (1.0 - h1)) + self.rair(f))
        return np.where(f < 1000.0, lo, hi)

    def D2(self, t, f):
        """Per-bin power envelope, t (T, 1) against f (1, F)."""
        t = np.maximum(t, 0.0)
        if self.coupled is not None:
            cp = self.coupled
            d1 = LN1E6 / cp["room1"].t60(f)
            d2 = LN1E6 / cp["room2"].t60(f)
            k1 = C * cp["S12"] / (4.0 * cp["V1"])
            k2 = C * cp["S12"] / (4.0 * cp["V2"])
            m11, m12, m21, m22 = -(d1 + k1), k1, k2, -(d2 + k2)
            tr, det = m11 + m22, m11 * m22 - m12 * m21
            disc = np.sqrt(np.maximum(tr * tr / 4.0 - det, 1e-12))
            l1, l2 = tr / 2.0 + disc, tr / 2.0 - disc              # the slow and the fast decay
            if cp.get("far"):                                       # the source in the large room: a rise, then the slow decay
                tp = np.log(l2 / l1) / (l1 - l2)
                return (np.exp(l1 * t) - np.exp(l2 * t)) / (np.exp(l1 * tp) - np.exp(l2 * tp))
            a1 = (l1 - m22) / (l1 - l2)
            return a1 * np.exp(l1 * t) + (1.0 - a1) * np.exp(l2 * t)
        T = self.t60(f)
        if self.swell is not None:
            return np.exp(-LN1E6 * np.maximum(self.swell["t_s"] - t, 0.0) / T)
        p = np.exp(-LN1E6 * t / T)
        if self.sweep is not None:
            sw = self.sweep
            u = np.clip(t / sw["t_sweep"], 0.0, 1.0)
            fc = sw["f0"] * (sw["f1"] / sw["f0"]) ** u
            bump = np.exp(-0.5 * (np.log2(np.maximum(f, 10.0) / fc) / sw["sigma_oct"]) ** 2)
            p = p * 10 ** (sw["gain_db"] * bump / 10)
        return p

    def ebb(self, t):
        """The broadband envelope, applied sample-exactly: the onset, and a swell's release."""
        k = np.clip(np.asarray(t, dtype=float) / max(self.onset_ms / 1000.0, 1e-4), 0.0, 1.0)
        e = np.sin(0.5 * np.pi * k) ** 2
        if self.swell is not None:
            ts, trel = self.swell["t_s"], self.swell["T_rel"]
            e = e * np.where(t > ts, np.exp(-0.5 * LN1E6 * (t - ts) / trel), 1.0)
        return e

    def seconds(self):
        if self._secs is None:
            tg = np.arange(0.0, self.max_seconds, 0.01)[:, None]
            fg = np.geomspace(40.0, 16000.0, 64)[None, :]
            p = self.D2(tg, fg) * (self.ebb(tg[:, 0]) ** 2)[:, None]
            p = p / p.max(axis=0, keepdims=True)
            alive = np.where((p > 1e-9).any(axis=1))[0]            # until -90 dB in every band
            self._secs = float(min(self.max_seconds, max(2.0, tg[alive[-1], 0] + 0.02)))
        return self._secs

    # ---- colour
    def B_db(self, f):
        f = np.maximum(np.asarray(f, dtype=float), 1.0)
        return (self.a_lo * softknee(np.log2(500.0 / f)) - self.a_hi * softknee(np.log2(f / 1000.0))
                + self.g_sub / (1.0 + (f / 100.0) ** 4)
                - 10 * np.log10(1 + (self.f_hp / f) ** 4) - 10 * np.log10(1 + (f / self.f_lp) ** 2))

    def integral(self, f, until=None):
        key = "all" if until is None else until
        if key not in self._int:
            secs = self.seconds() if until is None else until
            dt = 0.005
            tg = np.arange(0.0, secs, dt)
            fg = np.geomspace(10.0, SR / 2, 512)
            wt = self.ebb(tg) ** 2 * (grid_fade2(tg.size, dt) if until is None else 1.0)
            I = (self.D2(tg[:, None], fg[None, :]) * wt[:, None]).sum(axis=0) * dt
            self._int[key] = (fg, np.log(np.maximum(I, 1e-300)))
        fg, logi = self._int[key]
        return np.exp(np.interp(np.log(np.maximum(np.asarray(f, dtype=float), 10.0)), np.log(fg), logi))

    def energy_colour(self, f):
        return 10 ** (self.B_db(f) / 10) * self.integral(f)

    def rho(self, f):
        f = np.maximum(np.asarray(f, dtype=float), 1.0)
        return self.rho_hi + (self.rho_lo - self.rho_hi) / (1.0 + (f / self.f_rho) ** 2)

    # ---- the record
    def as_dict(self):
        d = dict(family=self.family, T_mid=self.T_mid, br63=self.br63, br125=self.br125, br250=self.br250,
                 hf_extra=self.hf_extra, rh=self.rh, temp=self.temp, air=self.air, f_hp=self.f_hp, f_lp=self.f_lp,
                 rho_lo=self.rho_lo, rho_hi=self.rho_hi, f_rho=self.f_rho, onset_ms=self.onset_ms,
                 knobs=dict(a_lo=self.a_lo, a_hi=self.a_hi, g_sub=self.g_sub))
        if self.target is not None:
            d["target"] = dict(lf_db=self.target[0], low_mid_pct=self.target[1], centroid_hz=self.target[2])
        if self.t60_profile is not None:
            d["t60_profile"] = [list(map(float, self.t60_profile[0])), list(map(float, self.t60_profile[1]))]
        if self.coupled is not None:
            cp = self.coupled
            d["coupled"] = dict(far=bool(cp.get("far")), V1=cp["V1"], V2=cp["V2"], S12=cp["S12"],
                                room1=cp["room1"].as_dict(), room2=cp["room2"].as_dict())
        else:
            d["t60_octaves"] = {str(fc): round(float(self.t60(np.array([float(fc)]))[0]), 3) for fc in OCT}
        if self.sweep is not None:
            d["sweep"] = dict(self.sweep)
        if self.swell is not None:
            d["swell"] = dict(self.swell)
        return d


def design_from_dict(d):
    cp = d.get("coupled")
    coupled = None
    if cp:
        coupled = dict(far=cp["far"], V1=cp["V1"], V2=cp["V2"], S12=cp["S12"],
                       room1=design_from_dict(cp["room1"]), room2=design_from_dict(cp["room2"]))
    prof = d.get("t60_profile")
    des = Design(d["family"], T_mid=d["T_mid"], br63=d["br63"], br125=d["br125"], br250=d["br250"], hf_extra=d["hf_extra"],
                 rh=d["rh"], temp=d["temp"], air=d["air"], t60_profile=(prof[0], prof[1]) if prof else None, f_hp=d["f_hp"],
                 f_lp=d["f_lp"], rho_lo=d["rho_lo"], rho_hi=d["rho_hi"], f_rho=d["f_rho"], onset_ms=d["onset_ms"],
                 coupled=coupled, sweep=d.get("sweep"), swell=d.get("swell"),
                 target=tuple(d["target"][k] for k in ("lf_db", "low_mid_pct", "centroid_hz")) if "target" in d else None)
    k = d.get("knobs", {})
    des.a_lo, des.a_hi, des.g_sub = k.get("a_lo", 0.0), k.get("a_hi", 3.0), k.get("g_sub", 0.0)
    return des


class Reversed:
    """A swell measured backwards: flipped in time, it is a decay, and every decay test applies."""
    def __init__(self, des, secs):
        self.des, self.secs = des, secs

    def D2(self, t, f):
        return self.des.D2(self.secs - t, f)

    def ebb(self, t):
        return self.des.ebb(self.secs - np.asarray(t, dtype=float))

    def energy_colour(self, f):
        return self.des.energy_colour(f)

    def rho(self, f):
        return self.des.rho(f)


# ---------------------------------------------------------------------------- families

RANGES = {
    "chamber": dict(T_mid=(1.2, 2.5), br125=(0.85, 1.0), br250=(0.95, 1.05), br63f=(0.8, 1.0), hf_extra=(0.5, 2.0), rh=(40, 60),
                    rho_lo=(0.3, 0.6), rho_hi=(0.0, 0.15), f_rho=(250, 500), onset_ms=(3, 15), f_hp=(35, 60)),
    "hall": dict(T_mid=(2.5, 6.0), br125=(0.9, 1.05), br250=(0.95, 1.08), br63f=(0.85, 1.0), hf_extra=(0.3, 1.0), rh=(40, 60),
                 rho_lo=(0.4, 0.7), rho_hi=(0.0, 0.15), f_rho=(200, 400), onset_ms=(10, 40), f_hp=(35, 60)),
    "cathedral": dict(T_mid=(5.0, 12.0), br125=(0.9, 1.08), br250=(0.95, 1.1), br63f=(0.85, 1.0), hf_extra=(0.0, 0.6), rh=(45, 65),
                      rho_lo=(0.5, 0.7), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=(20, 60), f_hp=(35, 60)),
    "cavern": dict(T_mid=(6.0, 16.0), br125=(0.8, 0.95), br250=(0.9, 1.0), br63f=(0.8, 0.95), hf_extra=(1.0, 3.0), rh=(70, 90),
                   temp=(8, 14), rho_lo=(0.5, 0.8), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=(30, 80), f_hp=(35, 60)),
    "vast": dict(T_mid=(15.0, 30.0), br125=(0.8, 0.95), br250=(0.9, 1.0), br63f=(0.8, 0.95), hf_extra=(0.0, 0.5), rh=(45, 70),
                 rho_lo=(0.5, 0.7), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=(60, 200), f_hp=(35, 60)),
    "plate": dict(T_mid=(1.8, 5.0), rho_lo=(0.0, 0.1), rho_hi=(0.0, 0.1), f_rho=(250, 500), onset_ms=(0.5, 2.0), f_hp=(50, 90)),
    "bloom": dict(T1=(1.0, 3.0), T2=(8.0, 25.0), V1=(1500, 8000), V2=(20000, 150000), S12=(30, 300),
                  rho_lo=(0.5, 0.7), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=(10, 40), f_hp=(35, 60)),
    "far": dict(T1=(1.0, 6.0), T2=(8.0, 25.0), V1=(800, 6000), V2=(20000, 150000), S12=(4, 60),
                rho_lo=(0.5, 0.7), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=0.5, f_hp=(35, 60)),
    "swell": dict(T_mid=(2.0, 8.0), br125=(0.85, 1.0), br250=(0.95, 1.05), br63f=(0.85, 1.0), hf_extra=(0.3, 1.0),
                  rho_lo=(0.4, 0.7), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=5.0, f_hp=(35, 60),
                  ts_ratio=(0.8, 1.3), T_rel=(0.08, 0.5)),
    "drift": dict(T_mid=(4.0, 12.0), br125=(0.85, 1.0), br250=(0.95, 1.05), br63f=(0.85, 1.0), hf_extra=(0.3, 1.0),
                  rho_lo=(0.4, 0.7), rho_hi=(0.0, 0.1), f_rho=(150, 300), onset_ms=(15, 40), f_hp=(35, 60),
                  f0=(2500, 5000), f1=(500, 900), t_sweep=(2.0, 10.0), sigma=(0.4, 0.8), gain=(4.0, 7.0)),
}
TMAX = {"chamber": 2.5, "hall": 6.0, "cathedral": 12.0, "cavern": 16.0, "vast": 30.0, "plate": 5.0, "swell": 8.0, "drift": 12.0}
# Long-term colour: energy below 150 Hz (dB), share 150-500 Hz (%), power centroid (Hz). Set from 48 measured rooms
# (median -15.5 dB, 14 %, 2.75 kHz) and moved per family; the windows are what a family may land in at all.
TARGET = {"chamber": (-15.0, 14.0, 2400.0), "hall": (-14.5, 15.0, 2200.0), "cathedral": (-14.0, 17.0, 1900.0),
          "cavern": (-12.5, 23.0, 1200.0), "vast": (-12.5, 22.0, 1300.0), "plate": (-15.0, 12.0, 3000.0),
          "bloom": (-13.5, 17.0, 2000.0), "far": (-13.5, 18.0, 1800.0), "swell": (-14.5, 15.0, 2300.0),
          "drift": (-14.5, 15.0, 2100.0)}
WIN = {"room": ((-18.0, -9.0), (8.0, 25.0), (1500.0, 3500.0)), "dark": ((-15.0, -7.0), (14.0, 30.0), (800.0, 2000.0)),
       "plate": ((-24.0, -12.0), (5.0, 18.0), (2200.0, 4500.0))}
FAMWIN = {"cavern": "dark", "vast": "dark", "plate": "plate"}
SINGLE = ("chamber", "hall", "cathedral", "cavern", "vast", "plate", "swell")
# How many files of each family a library of 1000 has: halls and cathedrals most, the huge and coupled rooms
# fewer (they are the long files), the designed gestures in between.
PLAN = {"hall": 150, "cathedral": 140, "chamber": 90, "plate": 90, "cavern": 80, "vast": 70, "bloom": 90, "far": 60,
        "drift": 120, "swell": 110}
KINDS = (("grow", 0.45), ("darken", 0.25), ("single", 0.30))


def clamp_target(fam, t):
    (lo, hi), (mlo, mhi), (clo, chi) = WIN[FAMWIN.get(fam, "room")]
    lf, lm, cen = t
    return (min(max(lf, lo + 1.0), hi - 1.0), min(max(lm, mlo * 1.1), mhi / 1.1), min(max(cen, clo * 1.05), chi / 1.05))


def make_design(fam, rng, T_scale=1.0):
    r = RANGES[fam]

    def U(key):
        v = r[key]
        return float(rng.uniform(*v)) if isinstance(v, tuple) else float(v)

    kw = dict(f_hp=U("f_hp"), rho_lo=U("rho_lo"), rho_hi=U("rho_hi"), f_rho=U("f_rho"), onset_ms=U("onset_ms"))
    kw["f_lp"] = float(rng.uniform(12000.0, 16000.0))
    if fam == "plate":
        tm = min(TMAX[fam], U("T_mid") * T_scale)
        prof = ([60, 125, 400, 3000, 8000, 16000], [float(rng.uniform(0.4, 0.6)), float(rng.uniform(0.55, 0.75)), 1.0, 1.0,
                                                    float(rng.uniform(0.5, 0.7)), float(rng.uniform(0.3, 0.45))])
        des = Design(fam, T_mid=tm, air=False, t60_profile=prof, **kw)
    elif fam in ("bloom", "far"):
        room1 = Design("room1", T_mid=U("T1"), br63=0.9, br125=0.9, br250=1.0, hf_extra=1.0 if fam == "bloom" else 0.8)
        b125 = float(rng.uniform(0.85, 0.95))
        room2 = Design("room2", T_mid=min(25.0, U("T2") * T_scale), br63=b125, br125=b125, br250=float(rng.uniform(0.9, 1.0)),
                       hf_extra=0.3)
        des = Design(fam, coupled=dict(room1=room1, room2=room2, V1=U("V1"), V2=U("V2"), S12=U("S12"), far=fam == "far"), **kw)
    else:
        for key in ("T_mid", "hf_extra", "rh", "temp", "br125", "br250"):
            if key in r:
                kw[key] = U(key)
        kw["br63"] = kw["br125"] * U("br63f")
        kw["T_mid"] = min(TMAX[fam], kw["T_mid"] * T_scale)
        tair = float(t_air(np.array([1000.0]), kw.get("temp", 20.0), kw.get("rh", 50.0))[0])
        kw["T_mid"] = min(kw["T_mid"], 0.8 * tair)
        if fam == "swell":
            kw["swell"] = {"t_s": min(10.0, kw["T_mid"] * U("ts_ratio")), "T_rel": U("T_rel")}
        if fam == "drift":
            kw["sweep"] = {"f0": U("f0"), "f1": U("f1"), "t_sweep": U("t_sweep"), "sigma_oct": U("sigma"), "gain_db": U("gain")}
        des = Design(fam, **kw)
    lf, lm, cen = TARGET[fam]
    des.target = clamp_target(fam, (lf + float(rng.uniform(-1.0, 1.0)), lm * float(rng.uniform(0.9, 1.1)),
                                    cen * float(rng.uniform(0.88, 1.12))))
    return des


class TableDesign(Design):
    """A design given as octave values, the way Convolver::makeDefaultImpulse (Core/src/Convolution.cpp) bakes
    the built-in hall: the initial level linear in log frequency, the T60 in its logarithm, the ends held, with
    12 dB an octave below 40 Hz and 6 dB an octave above 14 kHz."""
    OCT_B = (-3.7, -3.5, -2.5, -1.1, -1.4, -3.9, -7.5, -11.9, -17.9)
    OCT_T60 = (2.52, 2.66, 2.80, 2.80, 2.80, 2.06, 1.43, 0.85, 0.37)

    def __init__(self):
        super().__init__("hall", T_mid=2.8, f_hp=40.0, f_lp=14000.0, rho_lo=0.5, rho_hi=0.05, f_rho=300.0, onset_ms=15.0,
                         max_seconds=4.0)

    def t60(self, f):
        f = np.clip(np.asarray(f, dtype=float), OCT[0], OCT[-1])
        return np.exp(logf_interp(f, OCT, np.log(self.OCT_T60)))

    def B_db(self, f):
        f = np.maximum(np.asarray(f, dtype=float), 1.0)
        b = logf_interp(np.clip(f, OCT[0], OCT[-1]), OCT, self.OCT_B)
        lo = -10 * np.log10(1 + (40.0 / f) ** 4) + 10 * math.log10(1 + (40.0 / OCT[0]) ** 4)
        hi = -10 * np.log10(1 + (f / 14000.0) ** 2) + 10 * math.log10(1 + (OCT[-1] / 14000.0) ** 2)
        return b + np.where(f <= OCT[0], lo, 0.0) + np.where(f >= OCT[-1], hi, 0.0)


def default_design():
    """The built-in hall, with the long-term colour its own table predicts as the target."""
    des = TableDesign()
    des.target = predict(des)
    return des


# ---------------------------------------------------------------------------- colour solve

FG = np.geomspace(5.0, 23990.0, 6000)
DF = np.gradient(FG)


def shares(w, f, df=None):
    w = w * (df if df is not None else 1.0)
    tot = float(w.sum())
    return (10 * math.log10(float(w[f < 150.0].sum()) / tot), 100 * float(w[(f >= 150.0) & (f < 500.0)].sum()) / tot,
            float((f * w).sum()) / tot)


def predict(des, early=False):
    rho = des.rho(FG)
    if early:
        e = 10 ** (des.B_db(FG) / 10) * des.integral(FG, until=0.3) * (1 + rho) / 2
    else:
        e = des.energy_colour(FG) * (1 + rho) / 2
    return shares(e, FG, DF)


def solve_colour(des):
    lf_t, lm_t, cen_t = des.target
    swell = des.swell is not None

    def res(x):
        des.a_lo, des.a_hi, des.g_sub = x
        lf, lm, cen = predict(des)
        r = [(lf - lf_t) / 1.0, 10 * math.log10(lm / lm_t) / 1.0, 10 * math.log10(cen / cen_t) / 0.3]
        if not swell:
            r.append(max(0.0, 10 * math.log10(predict(des, early=True)[2] / 4000.0)) / 0.1)
        return r

    sol = least_squares(res, x0=[0.0, 3.0, 0.0], bounds=([-4.0, 0.0, -18.0], [4.0, 9.0, 9.0]), x_scale=[1.0, 1.0, 3.0],
                        diff_step=1e-3)
    des.a_lo, des.a_hi, des.g_sub = (float(v) for v in sol.x)
    lf, lm, cen = predict(des)
    flag = abs(lf - lf_t) > 1.5 or abs(10 * math.log10(lm / lm_t)) > 1.5 or abs(cen / cen_t - 1) > 0.10
    at_bound = [k for k, v, lo, hi in (("a_lo", des.a_lo, -4.0, 4.0), ("a_hi", des.a_hi, 0.0, 9.0), ("g_sub", des.g_sub, -18.0, 9.0))
                if abs(v - lo) < 1e-3 or abs(v - hi) < 1e-3]
    return flag, at_bound, (lf, lm, cen)


# ---------------------------------------------------------------------------- synthesis

def synth(des, seed, seconds=None, nfft=2048, hop=512):
    """Fresh Gaussian noise; colour and coherence in the full-length spectrum; the onset (and a swell's release)
    sample-exactly in time; every bin's decay in the STFT. Unit energy per channel on average."""
    secs = seconds or des.seconds()
    n = int(round(secs * SR))
    rng = np.random.default_rng(seed)
    nfull = sfft.next_fast_len(n)
    dt = 0.005
    tg = np.arange(0.0, secs, dt)
    fg = np.geomspace(10.0, SR / 2, 512)
    wt = des.ebb(tg) ** 2 * grid_fade2(tg.size, dt)
    integ = (des.D2(tg[:, None], fg[None, :]) * wt[:, None]).sum(axis=0) * dt
    log_b = np.log(des.energy_colour(fg) / np.maximum(integ, 1e-300))
    X = sfft.rfft(rng.standard_normal((2, nfull)), axis=-1)
    f = sfft.rfftfreq(nfull, 1.0 / SR)
    amp = np.exp(0.5 * np.interp(np.log(np.maximum(f, 10.0)), np.log(fg), log_b))
    th = 0.5 * np.arcsin(np.clip(des.rho(f), -0.999, 0.999))
    y = np.stack([sfft.irfft((np.cos(th) * X[0] + np.sin(th) * X[1]) * amp, nfull),
                  sfft.irfft((np.sin(th) * X[0] + np.cos(th) * X[1]) * amp, nfull)])[:, :n]
    del X
    y *= des.ebb(np.arange(n) / SR)[None, :]
    fs_, ts_, Z = ss.stft(y, fs=SR, window="hann", nperseg=nfft, noverlap=nfft - hop, boundary="zeros", padded=True, axis=-1)
    del y
    Z *= np.sqrt(des.D2(np.clip(ts_, 0.0, secs)[:, None], fs_[None, :])).T[None, :, :]
    _, y = ss.istft(Z, fs=SR, window="hann", nperseg=nfft, noverlap=nfft - hop, boundary=True, time_axis=-1, freq_axis=-2)
    del Z
    y = y[:, :n] * fade(n)[None, :]
    return y / math.sqrt(float((y ** 2).sum()) / 2.0), secs


# ---------------------------------------------------------------------------- measurement and acceptance

def band_sos(fc):
    return ss.butter(3, [fc / math.sqrt(2), min(fc * math.sqrt(2), 0.49 * SR)], "bandpass", fs=SR, output="sos")


def measured_bands(x):
    rows = []
    for fc in OCT:
        y = ss.sosfiltfilt(band_sos(fc), x, axis=-1)
        e = (y ** 2).sum(axis=0)
        edc = ir_metrics.edc_db(np.sqrt(e))
        t30, r2 = ir_metrics.fit_decay(edc, SR, -5.0, -35.0)
        corr = float((y[0] * y[1]).sum() / math.sqrt((y[0] ** 2).sum() * (y[1] ** 2).sum()))
        rows.append((fc, t30, r2, corr))
    return rows


def expected_bands(des, secs):
    """Octave T30 and coherence the design itself predicts, through the same zero-phase filters."""
    dt = 0.01
    tg = np.arange(0.0, secs, dt)
    fg = np.geomspace(20.0, 0.499 * SR, 1600)
    df = np.gradient(fg)
    wt = des.ebb(tg) ** 2 * grid_fade2(tg.size, dt)
    D2 = des.D2(tg[:, None], fg[None, :]) * wt[:, None]
    tint = D2.sum(axis=0) * dt
    B = des.energy_colour(fg) / np.maximum(tint, 1e-300)
    rows = []
    for fc in OCT:
        _, h = ss.sosfreqz(band_sos(fc), worN=fg, fs=SR)
        w = np.abs(h) ** 4 * B * df
        P = D2 @ w
        edc = np.cumsum(P[::-1])[::-1]
        edc = 10 * np.log10(edc / edc[0] + 1e-300)
        sel = (edc <= -5.0) & (edc >= -35.0)
        t30 = -60.0 / np.polyfit(tg[sel], edc[sel], 1)[0] if sel.sum() >= 4 else float("nan")
        rho = float((w * tint * des.rho(fg)).sum() / (w * tint).sum())
        rows.append((fc, t30, rho))
    return rows


def note_spread_sd(x):
    mono = x.mean(axis=0)
    N = 1 << int(math.ceil(math.log2(max(mono.size, SR * 4))))
    P = np.abs(np.fft.rfft(mono, N)) ** 2
    f = np.fft.rfftfreq(N, 1 / SR)
    dev = []
    for midi in range(45, 70):
        fn = 440.0 * 2 ** ((midi - 69) / 12)
        band = (f >= fn - 1.5) & (f <= fn + 1.5)
        third = (f >= fn * 2 ** (-1 / 6)) & (f <= fn * 2 ** (1 / 6))
        dev.append(10 * math.log10(P[band].mean() / P[third].mean()))
    return float(np.std(dev))


def note_spread_limit(des, f0=220.0, W=3.0):
    """What a pitchless decay of this design gives by chance (spectral maxima every 4/T60 Hz), plus 1 dB."""
    secs = des.seconds()
    dt = 0.002
    t = np.arange(0.0, secs, dt)
    D = des.D2(t[:, None], np.array([[f0]])).ravel() * des.ebb(t) ** 2
    u = np.linspace(-W, W, 241)
    C_ = (D[None, :] * np.exp(-2j * np.pi * u[:, None] * t[None, :])).sum(axis=1) * dt
    rho2 = np.abs(C_) ** 2 / float(np.abs(C_[120]) ** 2)
    v = float(np.trapezoid((1 - np.abs(u) / W) * rho2, u)) / W
    k = 1.0 / min(max(v, 1e-9), 1.0)
    return 10 / math.log(10) * math.sqrt(float(polygamma(1, k))) + 1.0


def line_share(x, lo=60.0, hi=5000.0):
    mono = x.mean(axis=0)
    N = 1 << int(math.ceil(math.log2(mono.size)))
    P = np.abs(np.fft.rfft(mono, N)) ** 2
    f = np.fft.rfftfreq(N, 1 / SR)
    sel = (f >= lo * 2 ** (-1 / 6)) & (f <= hi * 2 ** (1 / 6))
    fs_, ps = f[sel], P[sel]
    lf = np.log2(fs_)
    c = np.concatenate([[0.0], np.cumsum(ps)])
    a = np.searchsorted(lf, lf - 1 / 6)
    b = np.maximum(np.searchsorted(lf, lf + 1 / 6), a + 1)
    mean = (c[b] - c[a]) / (b - a)
    inb = (fs_ >= lo) & (fs_ <= hi)
    return float(ps[(ps > 10 * mean) & inb].sum() / ps[inb].sum())


def env_vs_design(des, x, secs):
    hop = int(0.05 * SR)
    e = (x ** 2).sum(axis=0)
    nfr = e.size // hop
    meas = e[: nfr * hop].reshape(-1, hop).mean(axis=1)
    tc = (np.arange(nfr) + 0.5) * hop / SR
    fg = np.geomspace(20.0, 0.499 * SR, 800)
    df = np.gradient(fg)
    tg = np.arange(0.0, secs, 0.005)
    tint = (des.D2(tg[:, None], fg[None, :]) * (des.ebb(tg) ** 2)[:, None]).sum(axis=0) * 0.005
    B = des.energy_colour(fg) / np.maximum(tint, 1e-300)
    expd = (des.D2(tc[:, None], fg[None, :]) @ (B * df)) * des.ebb(tc) ** 2
    mdb = 10 * np.log10(meas / meas.max() + 1e-300)
    d = mdb - 10 * np.log10(expd / expd.max() + 1e-300)
    live = (mdb > -40.0) & (tc < secs - 0.15)
    d = d[live] - np.mean(d[live])
    return float(np.std(d)), float(np.percentile(np.abs(d), 99))


def centroid_win(x, a, b):
    seg = x[:, int(a * SR):int(b * SR)]
    spec = (np.abs(np.fft.rfft(seg * np.hanning(seg.shape[1]), axis=-1)) ** 2).sum(axis=0)
    f = np.fft.rfftfreq(seg.shape[1], 1 / SR)
    return float((f * spec).sum() / spec.sum())


def accept(des, x, secs):
    """The design's acceptance tests. Returns (failures, measurements)."""
    fam = des.family
    fails = []
    swell = des.swell is not None
    mdes, mx = (Reversed(des, secs), x[:, ::-1].copy()) if swell else (des, x)
    m = ir_metrics.analyse_array(mx, SR)
    meas = measured_bands(mx)
    expd = expected_bands(mdes, secs)
    T = {r[0]: r[1] for r in meas}
    R2 = {r[0]: r[2] for r in meas}
    RH = {r[0]: r[3] for r in meas}
    TE = {r[0]: r[1] for r in expd}
    RHE = {r[0]: r[2] for r in expd}
    terr = []
    for fc in OCT:
        if not T[fc] or not np.isfinite(TE[fc]):
            fails.append(f"T30@{fc} n/a")
            continue
        e = abs(T[fc] / TE[fc] - 1)
        terr.append(e)
        if e > (0.10 if fc <= 125 else 0.05):
            fails.append(f"T30@{fc} {100 * e:.1f}%")
        lim = 0.15 if fc >= 500 else (0.25 if fc == 250 else (0.35 if secs >= 5.0 else 9.0))
        if abs(RH[fc] - RHE[fc]) > lim:
            fails.append(f"rho@{fc} {abs(RH[fc] - RHE[fc]):.2f}")
    if fails:
        return fails, dict(terr=100 * max(terr) if terr else float("nan"))
    if fam in SINGLE:
        for fc in (250, 500, 1000, 2000, 4000, 8000):
            if R2[fc] is not None and R2[fc] < 0.99:
                fails.append(f"R2@{fc} {R2[fc]:.3f}")
    tmid = 0.5 * (T[500] + T[1000])
    t8 = T[8000] / T[1000]
    if t8 > (0.8 if fam in ("chamber", "plate") else 0.6):
        fails.append(f"T8k/T1k {t8:.2f}")
    if fam != "drift":
        for lo, hi in ((1000, 2000), (2000, 4000), (4000, 8000), (8000, 16000)):
            if T[hi] > 1.1 * T[lo]:
                fails.append(f"treble rises {lo}->{hi}")
    r250 = T[250] / tmid
    rmax = max(T[63], T[125], T[250]) / tmid
    brb = (T[125] + T[250]) / (T[500] + T[1000])
    if fam != "plate":
        if r250 > 1.12:
            fails.append(f"T250/Tmid {r250:.2f}")
        if rmax > 1.15:
            fails.append(f"Tlow/Tmid {rmax:.2f}")
        if brb > 1.12:
            fails.append(f"bass ratio {brb:.2f}")
    if (m.get("ned_tail") or 0.0) < 0.95:
        fails.append(f"echo density {m.get('ned_tail') or 0.0:.2f}")
    if m.get("rerises"):
        fails.append("re-rises")
    if m["peaky_db"] > 7.7:
        fails.append(f"peaky {m['peaky_db']:.2f}")
    ls = 100 * line_share(x)
    if ls > 2.0:
        fails.append(f"lines {ls:.2f}%")
    ns, ns_lim = note_spread_sd(x), note_spread_limit(des)
    if ns > ns_lim:
        fails.append(f"note spread {ns:.2f} > {ns_lim:.2f}")
    mono = x.mean(axis=0)
    pw = np.abs(np.fft.rfft(mono)) ** 2
    lf, lm, cen = shares(pw, np.fft.rfftfreq(mono.size, 1 / SR))
    lf_t, lm_t, cen_t = des.target
    if abs(lf - lf_t) > (2.5 if fam == "bloom" else 1.5):
        fails.append(f"below 150 Hz {lf:.1f} dB")
    if abs(10 * math.log10(lm / lm_t)) > 1.5:
        fails.append(f"150-500 Hz {lm:.1f}%")
    if abs(cen / cen_t - 1) > 0.10:
        fails.append(f"centroid {cen:.0f}")
    (lo, hi), (mlo, mhi), (clo, chi) = WIN[FAMWIN.get(fam, "room")]
    if not (lo <= lf <= hi and mlo <= lm <= mhi and clo <= cen <= chi):
        fails.append("outside the family's window")
    er_sd, er_p99 = env_vs_design(mdes, mx, secs)
    if er_sd > 1.0 or er_p99 > 3.0:
        fails.append(f"envelope {er_sd:.2f}/{er_p99:.2f} dB")
    e = (x ** 2).sum(axis=0)
    k = int(0.05 * SR)
    loud = float(np.max(np.convolve(e, np.ones(k) / k, mode="valid")))
    if swell:
        early = centroid_win(x, max(0.0, secs - 0.8), secs - 0.2)
        end = 10 * math.log10(float(e[-int(0.02 * SR):].mean()) / loud + 1e-300)
        start = 10 * math.log10(float(e[: int(0.3 * SR)].mean()) / loud + 1e-300)
        if end > -70.0:
            fails.append(f"end {end:.0f} dB")
        if start > -30.0:
            fails.append(f"start {start:.0f} dB")
    else:
        early = centroid_win(x, 0.0, 0.3)
        end = 10 * math.log10(float(e[-int(0.1 * SR):].mean()) / loud + 1e-300)
        if end > -80.0:
            fails.append(f"end {end:.0f} dB")
    if early > 5000.0:
        fails.append(f"early centroid {early:.0f}")
    st = dict(seconds=round(secs, 3), t30_error_pct=round(100 * max(terr), 2), t30_octaves={str(fc): round(T[fc], 3) for fc in OCT},
              t250_over_tmid=round(r250, 3), low_over_tmid=round(rmax, 3), bass_ratio=round(brb, 3), t8k_over_t1k=round(t8, 3),
              echo_density=round(m.get("ned_tail") or 0.0, 3), peaky_db=round(m["peaky_db"], 3), lines_pct=round(ls, 4),
              note_spread_db=round(ns, 3), note_spread_limit_db=round(ns_lim, 3), below_150_db=round(lf, 2), low_mid_pct=round(lm, 2),
              centroid_hz=round(cen, 1), early_centroid_hz=round(early, 1), envelope_sd_db=round(er_sd, 3), end_db=round(end, 1),
              lr_corr=round(m.get("corr") or 0.0, 3))
    return fails, st


# ---------------------------------------------------------------------------- the library

def unit_names(fam, nnn, kind):
    if kind == "single":
        return [f"{fam}_{nnn:03d}"]
    return [f"{fam}_{nnn:03d}a", f"{fam}_{nnn:03d}b"]


def plan_units(families, count, tag):
    scale = count / float(sum(PLAN[f] for f in families))
    units = []
    for fam in families:
        want = max(1, int(round(PLAN[fam] * scale)))
        files = nnn = 0
        while files < want:
            r = np.random.default_rng(zlib.crc32(f"{tag}|{fam}|{nnn:03d}|kind".encode()))
            kind = str(r.choice([k for k, _ in KINDS], p=[p for _, p in KINDS]))
            if want - files < 2:
                kind = "single"
            units.append(dict(family=fam, nnn=nnn, kind=kind))
            files += 1 if kind == "single" else 2
            nnn += 1
    return units


def write_file(path_stem, x, des, meta):
    import soundfile as sf
    y = (x / float(np.abs(x).max()) * 10 ** (-1.0 / 20.0)).T.astype(np.float32)
    tmp = path_stem + ".tmp.wav"
    sf.write(tmp, y, SR, subtype="PCM_24")
    os.replace(tmp, path_stem + ".wav")
    with open(path_stem + ".json", "w", encoding="utf-8") as f:
        json.dump(meta, f, indent=1)


def render_unit(job):
    fam, nnn, kind, out, tag, legacy = job["family"], job["nnn"], job["kind"], job["out"], job["tag"], job["legacy"]
    names = unit_names(fam, nnn, kind)
    for nm in names:
        if nm + ".wav" in legacy:
            return dict(names=names, status="refused", reason="a legacy name")
    if all(os.path.exists(os.path.join(out, nm + ".wav")) and os.path.exists(os.path.join(out, nm + ".json")) for nm in names):
        return dict(names=names, status="present")
    reasons = collections.Counter()
    t0 = time.time()
    for attempt in range(8):
        design_seed = zlib.crc32(f"{tag}|{fam}|{nnn:03d}|{attempt}".encode())
        unit_rng = np.random.default_rng(zlib.crc32(f"{tag}|{fam}|{nnn:03d}|{attempt}|unit".encode()))
        try:
            if kind == "single":
                designs = [make_design(fam, np.random.default_rng(design_seed))]
            elif kind == "grow":
                grow = float(unit_rng.uniform(1.4, 2.0))
                a = make_design(fam, np.random.default_rng(design_seed), T_scale=0.75)
                b = make_design(fam, np.random.default_rng(design_seed), T_scale=0.75 * grow)
                if a.swell is not None:
                    b.swell = dict(a.swell)
                designs = [a, b]
            else:   # darken: the same room with its own, darker targets
                a = make_design(fam, np.random.default_rng(design_seed))
                b = make_design(fam, np.random.default_rng(design_seed))
                lf, lm, cen = a.target
                b.target = clamp_target(fam, (lf + 1.0, lm + 3.0, cen * 0.75))
                designs = [a, b]
            solved = [solve_colour(d) for d in designs]
        except ValueError as exc:
            reasons[f"infeasible: {exc}"] += 1
            continue
        if any(flag for flag, _, _ in solved):
            reasons["colour target unreachable"] += 1
            continue
        if kind == "single":
            seeds = [zlib.crc32(f"{tag}|{fam}|{nnn:03d}|noise|{attempt}".encode())]
            lengths = [designs[0].seconds()]
        else:   # a pair shares its seed and its length, so the morph between them is one noise in two rooms
            seeds = [zlib.crc32(f"{tag}|{fam}|{nnn:03d}|noise|{attempt}".encode())] * 2
            lengths = [max(d.seconds() for d in designs)] * 2
        results, ok = [], True
        for des, seed, secs in zip(designs, seeds, lengths):
            x, secs = synth(des, seed, seconds=secs)
            fails, st = accept(des, x, secs)
            if fails:
                reasons[fails[0].split(" ")[0]] += 1
                ok = False
                break
            results.append((des, seed, x, st))
        if not ok:
            continue
        morph_db = None
        if kind in ("grow", "darken"):
            xa, xb = results[0][2], results[1][2]
            morph_db = 10 * math.log10(float(((0.5 * xa + 0.5 * xb) ** 2).sum()) / 2.0)
            if morph_db < -0.5:
                reasons["morph dips"] += 1
                continue
        for i, (des, seed, x, st) in enumerate(results):
            partner = names[1 - i] + ".wav" if len(names) == 2 else None
            meta = dict(generator=tag, name=names[i] + ".wav", family=fam, kind=kind, partner=partner, seed=int(seed),
                        attempt=attempt, design=des.as_dict(), acceptance=st,
                        morph_midpoint_db=None if morph_db is None else round(morph_db, 3),
                        sample_rate=SR, air_absorption="ISO 9613-1 equations, extrapolated above 10 kHz",
                        numpy=np.__version__, scipy=scipy.__version__)
            write_file(os.path.join(out, names[i]), x, des, meta)
        return dict(names=names, status="written", attempts=attempt + 1, seconds=[st["seconds"] for _, _, _, st in results],
                    wall=round(time.time() - t0, 1), reasons=dict(reasons))
    return dict(names=names, status="failed", attempts=8, reasons=dict(reasons), wall=round(time.time() - t0, 1))


def load_legacy():
    path = os.path.join(ROOT, "Tools", "library", "legacy_impulses.json")
    if not os.path.exists(path):
        return set()
    with open(path, encoding="utf-8") as f:
        return set(json.load(f).get("files", {}))


def generate(out, count, families, jobs, tag):
    os.makedirs(out, exist_ok=True)
    legacy = load_legacy()
    units = plan_units(families, count, tag)
    total_files = sum(len(unit_names(u["family"], u["nnn"], u["kind"])) for u in units)
    print(f"{len(units)} units, {total_files} files, {len(legacy)} legacy names kept out, {jobs} processes", flush=True)
    for u in units:
        u.update(out=out, tag=tag, legacy=legacy)
    done, by_family = 0, collections.defaultdict(collections.Counter)
    failures, secs = [], collections.defaultdict(float)
    redraws, attempts = collections.defaultdict(collections.Counter), collections.defaultdict(collections.Counter)
    t0 = time.time()
    # the long rooms first, so the processes are busy with them while the short ones fill the gaps
    order = sorted(units, key=lambda u: -TMAX.get(u["family"], 25.0))
    with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
        futures = {ex.submit(render_unit, u): u for u in order}
        for fut in cf.as_completed(futures):
            u = futures[fut]
            try:
                r = fut.result()
            except Exception as exc:     # a crashed unit is reported, not the end of the run
                r = dict(names=unit_names(u["family"], u["nnn"], u["kind"]), status="crashed", reasons={type(exc).__name__: 1})
            done += 1
            by_family[u["family"]][r["status"]] += len(r["names"])
            if r["status"] in ("failed", "crashed", "refused"):
                failures.append(r)
            for s in r.get("seconds", []):
                secs[u["family"]] += s
            redraws[u["family"]].update(r.get("reasons", {}))
            if "attempts" in r:
                attempts[u["family"]][r["attempts"]] += 1
            if done % 25 == 0 or done == len(units):
                el = time.time() - t0
                print(f"  {done}/{len(units)} units, {el / 60:.1f} min, about {el / done * (len(units) - done) / 60:.0f} min to go",
                      flush=True)
    report = dict(tag=tag, count=count, families={f: dict(c) for f, c in by_family.items()},
                  minutes={f: round(s / 60, 1) for f, s in secs.items()},
                  redraw_reasons={f: dict(c.most_common()) for f, c in redraws.items()},
                  attempts={f: {str(k): v for k, v in sorted(c.items())} for f, c in attempts.items()}, failures=failures)
    with open(os.path.join(out, "roomgen_report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    for fam in families:
        print(f"  {fam:10s} {dict(by_family[fam])}  {secs[fam] / 60:6.1f} min  redrawn for {dict(redraws[fam].most_common(3))}")
    print(f"  total {sum(secs.values()) / 60:.1f} min of impulse; {len(failures)} units failed", flush=True)


# ---------------------------------------------------------------------------- commands

def read_wav(path):
    import soundfile as sf
    x, sr = sf.read(path, always_2d=True, dtype="float64")
    if sr != SR:
        raise SystemExit(f"{path}: {sr} Hz, the tests are written for {SR}")
    x = x.T
    if x.shape[0] == 1:
        x = np.vstack([x, x])
    return x / math.sqrt(float((x ** 2).sum()) / 2.0)


def cmd_default(wav):
    des = default_design()
    pred, early = des.target, predict(des, early=True)
    print("built-in hall (Convolver::makeDefaultImpulse)")
    print("  B dB  " + " ".join(f"{fc}:{float(des.B_db(np.array([float(fc)]))[0]):+.1f}" for fc in OCT))
    print("  T60 s " + " ".join(f"{fc}:{float(des.t60(np.array([float(fc)]))[0]):.2f}" for fc in OCT))
    print(f"  predicted: below 150 Hz {pred[0]:.1f} dB, 150-500 Hz {pred[1]:.1f} %, centroid {pred[2]:.0f} Hz, "
          f"first 300 ms {early[2]:.0f} Hz (the design round expected -15.6 dB, 15.8 %, 2040 Hz)")
    if wav:
        x = read_wav(wav)
        fails, st = accept(des, x, x.shape[1] / SR)
        print(f"  {os.path.basename(wav)}: {'PASS' if not fails else 'FAIL: ' + '; '.join(fails)}")
        print("  " + json.dumps(st))
        return 1 if fails else 0
    return 0


def cmd_check(wav):
    with open(os.path.splitext(wav)[0] + ".json", encoding="utf-8") as f:
        meta = json.load(f)
    des = design_from_dict(meta["design"])
    x = read_wav(wav)
    fails, st = accept(des, x, x.shape[1] / SR)
    print(f"{os.path.basename(wav)}: {'PASS' if not fails else 'FAIL: ' + '; '.join(fails)}")
    print(json.dumps(st))
    return 1 if fails else 0


def selftest():
    bad = 0

    def ok(label, cond):
        nonlocal bad
        bad += not cond
        print(f"  {'ok  ' if cond else 'FEHL'} {label}", flush=True)

    ta = float(t_air(np.array([1000.0]), 20.0, 50.0)[0])
    ok(f"air alone at 1 kHz, 20 C, 50 %: T60 {ta:.1f} s (ISO 9613-1: about 37.5)", 36.0 < ta < 39.0)
    des = make_design("chamber", np.random.default_rng(3))
    flag, _, _ = solve_colour(des)
    x1, secs = synth(des, 11)
    x2, _ = synth(des, 11)
    ok("the same seed gives the same file", np.array_equal(x1, x2))
    # A short room's 63 Hz octave holds few degrees of freedom: its T30 scatters from noise to noise (twelve
    # noises of this chamber: mean +3 %, sd 4.9 %, worst 13.9 %), so one draw may miss the 10 %.
    for noise in range(11, 15):
        x, s = synth(des, noise)
        fails, st = accept(des, x, s)
        if not fails:
            break
    ok(f"a chamber passes its own tests within four noises ({'; '.join(fails) or 'all'})", not fails and not flag)
    b = make_design("chamber", np.random.default_rng(3), T_scale=1.8)
    solve_colour(b)
    L = max(des.seconds(), b.seconds())
    xa, _ = synth(des, 5, seconds=L)
    xb, _ = synth(b, 5, seconds=L)
    xo, _ = synth(b, 6, seconds=L)
    mid = 10 * math.log10(float(((0.5 * xa + 0.5 * xb) ** 2).sum()) / 2.0)
    mid_o = 10 * math.log10(float(((0.5 * xa + 0.5 * xo) ** 2).sum()) / 2.0)
    ok(f"a pair on one noise morphs at {mid:+.2f} dB, on two noises {mid_o:+.2f} dB", mid > -0.5 > mid_o)
    for fam, seed in (("plate", 4), ("swell", 5), ("hall", 6)):
        for attempt in range(4):
            d = make_design(fam, np.random.default_rng(seed + 100 * attempt))
            flag, _, _ = solve_colour(d)
            if flag:
                continue
            x, s = synth(d, seed)
            fails, _ = accept(d, x, s)
            if not fails:
                break
        ok(f"a {fam} passes within four designs ({'; '.join(fails) or 'all'})", not fails)
    print(f"\n  {'alle bestanden' if not bad else f'{bad} fehlgeschlagen'}")
    return bad


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--out", required=True)
    g.add_argument("--count", type=int, default=1000)
    g.add_argument("--families", default=",".join(PLAN))
    g.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) - 8))
    g.add_argument("--tag", default=TAG)
    d = sub.add_parser("default")
    d.add_argument("--wav")
    c = sub.add_parser("check")
    c.add_argument("wav")
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "generate":
        fams = [f.strip() for f in a.families.split(",") if f.strip()]
        unknown = [f for f in fams if f not in PLAN]
        if unknown:
            sys.exit(f"unknown families: {unknown}")
        generate(a.out, a.count, fams, a.jobs, a.tag)
    elif a.cmd == "default":
        sys.exit(cmd_default(a.wav))
    elif a.cmd == "check":
        sys.exit(cmd_check(a.wav))
    else:
        sys.exit(1 if selftest() else 0)


if __name__ == "__main__":
    main()
