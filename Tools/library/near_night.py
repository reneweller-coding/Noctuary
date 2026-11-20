"""The near layer's night guard (plan C3, 20.11.2026): how fast each near preset's event rises over the background
and how far it stands out of its own mean -- measured, the limits, and the correction the engine applies to it when
the near layer's Night switch is on.

Rene's first complaint of the week was noise bursts and hits in the foreground at night. The answer so far was to
take single presets out of the bank (Seed Pod, Geiger Counter, four short whistlers); nothing guaranteed that the
next variant would not do the same. This measures every preset of the bank as it is played, sets two limits from
what Rene heard -- the two he threw out break them, the thirteen he liked pass -- and gives every preset that
breaks one a correction the engine decides once, when an event begins: a longer fade-in and less level, nothing that
reacts while the event plays, nothing that touches when or what it plays.

What is measured, per event (Rene's two complaints, as numbers):

  * The level as heard over the background: the event alone (rendered over its background with every source of the
    background switched off, as Tools/library/near_loudness.py does), K-weighted (BS.1770), in 100 ms windows every
    10 ms, added in power to the background's own loudness taken as steady:  heard(t) = 10 log10(1 + E(t) / B),
    in dB over the background. An event under the background barely moves it; one 20 dB over it is heard at +20.
  * rise: the steepest climb of that level within 100 ms, in dB per 100 ms -- how sudden the event is.
  * peak: the loudest 100 ms of it over its mean as heard (in power, over the span it is within 20 dB of its peak),
    in dB -- how far a hit or a burst stands out of the event it belongs to.
  * and for the report: the loudest momentary loudness (400 ms) over the background in LU, the time from 20 dB under
    the event's peak to within 1 dB of it, and the share of the onset's energy above 4 kHz.

A preset's number is its worst event in the render (the clock is set fast, so a short event is heard five or six
times; the first comes out of silence). The background is one preset (BACKGROUND), the same for every near preset,
taken as steady -- its own moves are not counted against the event.

    python Tools/library/near_night.py measure [--jobs 4] [--only "A,B"] [--exe PATH] [--work DIR]
    python Tools/library/near_night.py negatives [--jobs 4]       # Seed Pod and Geiger Counter, from git
    python Tools/library/near_night.py report                     # the distribution against the limits
    python Tools/library/near_night.py table                      # near_night.json and Core/src/NearNight.inc
    python Tools/library/near_night.py refine                     # trims from a verify render (near_night_refine.json)
    python Tools/library/near_night.py verify [--night on|off] [--exe PATH] [--jobs 4]   # render, count violations
    python Tools/library/near_night.py identity --base-exe OLD --only "A,B" [--night off|on]   # bit for bit
    python Tools/library/near_night.py --check                    # ctest near_night: the static part

Every render runs at below-normal priority and is kept as a level curve under the work folder
(work/library/near-night/<what>/<slug>.npz), so a run that is stopped goes on where it stopped.
"""
import argparse
import concurrent.futures
import ctypes
import json
import os
import re
import subprocess
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TABLE = os.path.join(HERE, "near_night.json")
REFINE = os.path.join(HERE, "near_night_refine.json")
INC = os.path.join(ROOT, "Core", "src", "NearNight.inc")
BANK_INC = os.path.join(ROOT, "Core", "src", "NearPresets.inc")
WORK = os.environ.get("NEAR_NIGHT_WORK", os.path.join(ROOT, "work", "library", "near-night"))
RENDER = os.environ.get("AMBIENT_RENDER", os.path.join(ROOT, "bin", "msvc", "ambient_render.exe"))

BACKGROUND = "Kiln Descent"       # the background every near preset is measured over (see the report)
HOP = 0.01                        # seconds between two windows of the level curve
WIN = 0.1                         # seconds a window spans; also the span the rise is measured over
PRE = 0.3                         # seconds of the curve kept before an event begins
SPAN_MAX = 150.0                  # seconds of an event that are measured (a sequence's bloom peaks at its middle)
HEARD_DB = 20.0                   # the span of an event: from the first to the last window within this of its peak

# The limits (set from the data, see the report and plan_tools_2026-11-20/C3; accepted by Rene on 20.11.2026 after the
# listening comparison -- Night itself stays the player's switch, off by default): no event rises faster than RISE_DB
# within 100 ms over the background, none stands PEAK_DB over its own mean as heard. MARGIN is what the table keeps
# under them, because the table is computed from curves and the engine applies the fade to the voices.
RISE_DB = 15.0
PEAK_DB = 14.0
MARGIN_DB = 1.0
REFINE_DB = 0.5                   # how far under a limit a rendered event must stay before refine() leaves it alone
FADES = [0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0]   # the fade-ins tried, seconds
GAIN_FLOOR = -30.0                # dB: the most the level is taken down

# Rene's verdicts. LIKED: the thirteen he heard and liked (Tools/make_layer_presets.py LIKED, 19.11.2026);
# REMOVED: the two he threw out as noise (the bank of commit b96b23a^ still has them).
REMOVED_FROM = "b96b23a^"
REMOVED = ["Seed Pod", "Geiger Counter"]

# What makes sound of its own in a sound preset, switched off for the event alone: near_loudness.py's list and the
# Floor (Balance.h), which near_loudness.py left on -- the Floor is a voice of the background that answers the mix, so
# with only the event sounding it began with the event, at a level the event's gain did not touch (found 20.11.2026:
# a Bowl Line event "rose" 19 dB in 100 ms with Night on because its first seconds were the Floor's).
SILENCE = []
for _k in ("osc_level", "src2_level", "src3_level", "src4_level", "sub_level", "strike_level", "air", "floor"):
    SILENCE += ["--set", _k + "=0"]


def below_normal():
    """This process at below-normal priority, so the analysis does not compete with the user either."""
    if os.name == "nt":
        k = ctypes.windll.kernel32
        k.SetPriorityClass(k.GetCurrentProcess(), 0x4000)


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")


def parse_bank(text):
    """[(name, settings)] of a NearPresets.inc text, in order: the entries of the NearPresets array only (the file
    goes on with the families' names, which look alike)."""
    i = text.index("NearPresets[] = {")
    return re.findall(r'\{\s*"([^"]+)",\s*"([^"]*)"', text[i:text.index("};", i)])


def bank():
    """The near bank in the tree, in order, Near Off included."""
    return parse_bank(open(BANK_INC, encoding="utf-8").read())


def removed_presets():
    """The two presets Rene threw out, with their settings as they were when he heard them."""
    text = subprocess.run(["git", "show", REMOVED_FROM + ":Core/src/NearPresets.inc"], cwd=ROOT, capture_output=True,
                          text=True, encoding="utf-8", check=True).stdout
    found = dict(parse_bank(text))
    return [(n, found[n]) for n in REMOVED]


def liked():
    sys.path.insert(0, os.path.dirname(HERE))
    import make_layer_presets as m
    return list(m.LIKED)


def setting(settings, key, default):
    m = re.search(r"(?:^|;)%s=([^;]*)" % re.escape(key), settings)
    if not m:
        return default
    try:
        return float(m.group(1))
    except ValueError:
        return m.group(1)


def read_wav(path):
    """The render as float64, frames x channels, and its rate (scipy's reader: soundfile is in no environment here)."""
    import scipy.io.wavfile
    sr, x = scipy.io.wavfile.read(path)
    if x.dtype.kind == "i":
        x = x.astype(np.float64) / float(np.iinfo(x.dtype).max)
    else:
        x = x.astype(np.float64)
    return (x if x.ndim == 2 else x[:, None]), sr


def k_weight(x, sr):
    """BS.1770-4's K curve (near_loudness.py's), for 48 kHz."""
    import scipy.signal
    if sr != 48000:
        raise ValueError("K-weighting coefficients here are for 48 kHz, got %d" % sr)
    b1, a1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585]
    b2, a2 = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
    return scipy.signal.lfilter(b2, a2, scipy.signal.lfilter(b1, a1, x, axis=0), axis=0)


def window_power(x, sr, win=WIN):
    """K-weighted power summed over the channels, mean over `win` windows every HOP: window i starts at i*HOP."""
    p = np.sum(k_weight(x, sr) ** 2, axis=1)
    c = np.concatenate([[0.0], np.cumsum(p)])
    n, h = int(round(win * sr)), int(round(HOP * sr))
    starts = np.arange(0, len(p) - n + 1, h)
    return (c[starts + n] - c[starts]) / n


def render(exe, out, args, seconds):
    """One render at below-normal priority; returns its log."""
    env = dict(os.environ)
    env["AMBIENT_PACKS"] = os.path.join(ROOT, "Library", "Packs")
    env.pop("AMBIENT_MUTE", None)
    cmd = [exe, "--preset", BACKGROUND, "--seconds", str(int(round(seconds))), "--hour", "21", "--near-log",
           "--out", out] + args
    flags = 0x4000 if os.name == "nt" else 0
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
                       creationflags=flags)
    if r.returncode != 0:
        raise RuntimeError("render failed (%d): %s" % (r.returncode, r.stderr.strip()[-400:]))
    return r.stdout


def near_args(name, settings, in_bank):
    """The render tool's arguments that put a near preset on: its name from the bank, or its settings one by one."""
    if in_bank:
        return ["--near-preset", name]
    out = ["--near-preset", "Near Off"]
    for kv in settings.split(";"):
        if kv:
            out += ["--set", kv]
    return out


def span_of(settings):
    """Seconds of an event that are measured: its length (capped) and a little of its release."""
    length = float(setting(settings, "fore_length", 6.0))
    release = float(setting(settings, "fore_release", 2.0))
    return min(length, SPAN_MAX) + min(release, 8.0) + 1.0


def render_seconds(settings):
    return max(60.0, 16.0 + span_of(settings) + 4.0)


def background_power(exe):
    """The background's K-weighted power as it plays (mean over 20..140 s), measured once and kept."""
    path = os.path.join(WORK, "background.json")
    if os.path.isfile(path):
        d = json.load(open(path, encoding="utf-8"))
        if d.get("background") == BACKGROUND:
            return d["power"]
    os.makedirs(WORK, exist_ok=True)
    wav = os.path.join(WORK, "background.wav")
    render(exe, wav, [], 140)
    x, sr = read_wav(wav)
    os.remove(wav)
    p = float(np.mean(np.sum(k_weight(x, sr)[int(20 * sr):] ** 2, axis=1)))
    q = os.path.join(WORK, "silenced.wav")
    render(exe, q, SILENCE, 60)
    y, _ = read_wav(q)
    os.remove(q)
    floor = float(np.mean(np.sum(k_weight(y, sr)[int(10 * sr):] ** 2, axis=1)))
    d = {"background": BACKGROUND, "power": p, "lufs": -0.691 + 10 * np.log10(p + 1e-24),
         "silenced_lu": 10 * np.log10((floor + 1e-24) / p)}
    json.dump(d, open(path, "w", encoding="utf-8"), indent=1)
    return p


def capture(exe, name, settings, in_bank, out_npz, extra=()):
    """Renders a near preset's events over the silenced background and keeps each event's level curve."""
    seconds = render_seconds(settings)
    wav = out_npz[:-4] + ".wav"
    log = render(exe, wav, SILENCE + near_args(name, settings, in_bank) + ["--set", "fore_rate=10"] + list(extra),
                 seconds)
    starts = [float(m) for m in re.findall(r"near event \d+ at \S+ \(([\d.]+) s\)", log)]
    x, sr = read_wav(wav)
    os.remove(wav)
    pw = window_power(x, sr)
    span = span_of(settings)
    events, onsets = [], []
    for k, t0 in enumerate(starts):
        t1 = min(starts[k + 1] if k + 1 < len(starts) else 1e9, t0 + span, len(x) / sr - WIN - 0.01)
        if t1 - t0 < 0.5:
            continue
        i0, i1 = int(round((t0 - PRE) / HOP)), int(round((t1 - WIN) / HOP))
        if i0 < 0 or i1 <= i0 + 10:
            continue
        events.append(pw[i0:i1].astype(np.float32))
        # The onset's colour: the share of the raw energy above 4 kHz in the 300 ms from where the event first comes
        # within 20 dB of its own peak.
        seg = pw[int(round(t0 / HOP)):i1]
        on = int(np.argmax(seg >= seg.max() * 10 ** (-HEARD_DB / 10))) if len(seg) else 0
        a = int((t0 + on * HOP) * sr)
        chunk = np.sum(x[a:a + int(0.3 * sr)], axis=1)
        spec = np.abs(np.fft.rfft(chunk * np.hanning(len(chunk)))) ** 2 if len(chunk) > 64 else np.zeros(2)
        f = np.fft.rfftfreq(len(chunk), 1.0 / sr) if len(chunk) > 64 else np.zeros(2)
        onsets.append(float(spec[f >= 4000].sum() / (spec.sum() + 1e-30)))
    np.savez_compressed(out_npz, starts=np.array(starts), onset_hf=np.array(onsets),
                        **{"e%d" % k: e for k, e in enumerate(events)})
    return len(events)


def load_events(npz):
    d = np.load(npz)
    ev = [d["e%d" % k].astype(np.float64) for k in range(len(d.files)) if "e%d" % k in d.files]
    return ev, list(d["onset_hf"])


def smoothstep(u):
    u = np.clip(u, 0.0, 1.0)
    return u * u * (3.0 - 2.0 * u)


def ramp(n, fade):
    """The engine's fade-in as a power factor on each window of an event's curve (window centres; before the event
    begins nothing is faded -- that is the tail of the one before)."""
    t = (np.arange(n) * HOP - PRE) + 0.5 * WIN
    if fade <= 0.0:
        return np.ones(n)
    r = np.where(t < 0.0, 1.0, smoothstep(t / fade))
    return r * r


def span_of_event(x):
    """The span an event is heard over, from its own curve before any correction: from the first to the last window
    within HEARD_DB of its peak (indices into the curve, PRE included before it). None for a silent one."""
    k = int(round(PRE / HOP))
    ev = x[k:]
    if len(ev) == 0 or ev.max() <= 0:
        return None
    idx = np.nonzero(ev >= ev.max() * 10 ** (-HEARD_DB / 10))[0]
    return k + int(idx[0]), k + int(idx[-1]) + 1


def metrics(curve, bg, gain_db=0.0, fade=0.0):
    """rise (dB per 100 ms) and peak (dB over the mean) of one event's curve as heard over the background, after a
    fade-in and a gain; and its loudest momentary loudness (400 ms) in LU over the background. The span the mean is
    taken over is the event's own, before the correction."""
    x = curve / bg
    sp = span_of_event(x)
    if sp is None:
        return 0.0, 0.0, -99.0
    y = x * ramp(len(x), fade) * 10 ** (gain_db / 10.0)
    heard = 10 * np.log10(1.0 + y)
    lag = int(round(WIN / HOP))
    rise = float(np.max(heard[lag:] - heard[:-lag])) if len(heard) > lag else 0.0
    span = 1.0 + y[sp[0]:sp[1]]
    peak = float(10 * np.log10(span.max() / span.mean()))
    ev = y[int(round(PRE / HOP)):]
    # momentary loudness: 400 ms = four 100 ms windows end to end
    m = (ev[:-30] + ev[10:-20] + ev[20:-10] + ev[30:]) / 4.0 if len(ev) > 30 else ev
    loud = float(10 * np.log10(m.max() + 1e-30))
    return rise, peak, loud


# ---------------------------------------------------------------- the table's own copy of the measurement

Q_FLOOR = -32.0   # dB over the background under which a window is stored as silence


def q8(db):
    """Levels in dB over the background as int8 quarter decibels (-128: under Q_FLOOR)."""
    q = np.round(np.asarray(db, dtype=np.float64) * 4.0)
    q = np.where(np.asarray(db) < Q_FLOOR, -128, np.clip(q, -127, 127))
    return q.astype(np.int8)


def uq8(q):
    q = np.asarray(q, dtype=np.float64)
    return np.where(q <= -128, 0.0, 10 ** (q / 40.0))


def b64(q):
    import base64
    return base64.b64encode(np.asarray(q, dtype=np.int8).tobytes()).decode("ascii")


def unb64(t):
    import base64
    return np.frombuffer(base64.b64decode(t), dtype=np.int8)


def compact(curve, bg, fade):
    """What the ctest needs of one event to recompute its rise and peak under a correction with this fade and any
    gain, small enough for the repository: the curve itself only where the fade acts (and 100 ms after), then of every
    pair of windows 100 ms apart only those no other pair beats for every gain (the lower first, the higher second),
    and of the span after the fade its loudest window, its sum and its length."""
    x = curve / bg
    sp = span_of_event(x)
    if sp is None:
        return None
    lag = int(round(WIN / HOP))
    n_on = min(len(x), int(np.ceil((PRE + max(fade, 0.0)) / HOP)) + lag + 1)
    q = lambda v: q8(10 * np.log10(np.maximum(v, 1e-30)))
    pairs = []
    if len(x) > n_on:
        x1, x2 = x[n_on - lag:len(x) - lag], x[n_on:]
        order = np.lexsort((-x2, x1))
        best = -1.0
        for i in order:
            if x2[i] > best and x2[i] > x1[i]:
                pairs.append((x1[i], x2[i]))
                best = x2[i]
    rest = x[max(sp[0], n_on):sp[1]] if sp[1] > n_on else np.zeros(0)
    return {"onset": b64(q(x[:n_on])), "span": [sp[0], sp[1]],
            "pairs": b64(q(np.array(pairs).ravel())) if pairs else "",
            "rest_max_db": round(float(10 * np.log10(rest.max() + 1e-30)), 2) if len(rest) else None,
            "rest_mean": round(float(rest.mean()), 4) if len(rest) else 0.0, "rest_n": int(len(rest))}


def compact_metrics(c, gain_db, fade):
    """rise and peak of an event from its compact copy (see compact) under a correction."""
    g = 10 ** (gain_db / 10.0)
    on = uq8(unb64(c["onset"]))
    lag = int(round(WIN / HOP))
    y = on * ramp(len(on), fade) * g
    heard = 10 * np.log10(1.0 + y)
    rise = float(np.max(heard[lag:] - heard[:-lag])) if len(heard) > lag else 0.0
    if c["pairs"]:
        pr = uq8(unb64(c["pairs"])).reshape(-1, 2)
        rise = max(rise, float(np.max(10 * np.log10((1.0 + g * pr[:, 1]) / (1.0 + g * pr[:, 0])))))
    s0, s1 = c["span"]
    part = 1.0 + y[s0:min(s1, len(on))]
    top = part.max() if len(part) else 0.0
    total, n = float(part.sum()), len(part)
    if c["rest_n"]:
        top = max(top, 1.0 + g * 10 ** (c["rest_max_db"] / 10.0))
        total += c["rest_n"] * (1.0 + g * c["rest_mean"])
        n += c["rest_n"]
    peak = float(10 * np.log10(top / (total / n))) if n else 0.0
    return rise, peak


def rise_time(curve):
    """Seconds from where the event first comes within 20 dB of its peak to where it first comes within 1 dB."""
    ev = curve[int(round(PRE / HOP)):]
    if len(ev) == 0 or ev.max() <= 0:
        return 0.0
    a = int(np.argmax(ev >= ev.max() * 10 ** (-HEARD_DB / 10)))
    b = int(np.argmax(ev >= ev.max() * 10 ** (-0.1)))
    return max(0.0, (b - a) * HOP)


def preset_metrics(events, bg, gain_db=0.0, fade=0.0):
    """The worst event's rise and peak, the loudest event's loudness."""
    ms = [metrics(e, bg, gain_db, fade) for e in events]
    return max(m[0] for m in ms), max(m[1] for m in ms), max(m[2] for m in ms)


def correction(events, bg, length, rise_lim=None, peak_lim=None):
    """The guard for one preset: the fade-in and the gain that keep every event MARGIN_DB inside both limits with the
    least level taken away (a fade costs nothing at the peak of a held note; the shorter fade wins a tie).
    Returns (fade s, gain dB) -- (0, 0) when the preset is inside the limits as it is."""
    rl = (RISE_DB if rise_lim is None else rise_lim) - MARGIN_DB
    pl = (PEAK_DB if peak_lim is None else peak_lim) - MARGIN_DB
    r, p, _ = preset_metrics(events, bg)
    if r <= (RISE_DB if rise_lim is None else rise_lim) and p <= (PEAK_DB if peak_lim is None else peak_lim):
        return 0.0, 0.0
    best = None
    cap = min(8.0, max(0.5, length))
    for fade in FADES:
        if fade > cap:
            break
        g = 0.0
        while g >= GAIN_FLOOR:
            r, p, _ = preset_metrics(events, bg, g, fade)
            if r <= rl and p <= pl:
                break
            g -= 0.25
        if g < GAIN_FLOOR:
            continue
        if best is None or g > best[1] + 1e-9:
            best = (fade, g)
    return best if best is not None else (cap, GAIN_FLOOR)


# ---------------------------------------------------------------- runs

def run_capture(exe, jobs, items, where, extra_of=lambda n: ()):
    """Renders the items not yet done (resumable), jobs at a time; prints the time per preset."""
    os.makedirs(where, exist_ok=True)
    todo = [(n, s, b) for n, s, b in items if not os.path.isfile(os.path.join(where, slug(n) + ".npz"))]
    print("%d to render of %d (%s), %d at a time" % (len(todo), len(items), where, jobs), flush=True)
    t_start = time.time()

    def one(item):
        n, s, b = item
        t = time.time()
        k = capture(exe, n, s, b, os.path.join(where, slug(n) + ".npz"), extra_of(n))
        return n, k, time.time() - t

    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for f in concurrent.futures.as_completed([ex.submit(one, it) for it in todo]):
            try:
                n, k, dt = f.result()
            except Exception as e:   # one broken render must not stop the night
                print("  failed: %s" % e, flush=True)
                continue
            done += 1
            if done % 10 == 0 or done == len(todo) or done <= 3:
                el = time.time() - t_start
                print("  %d/%d  %.0f s so far, %.1f s a preset in the wall clock (last: %s, %d events, %.1f s)"
                      % (done, len(todo), el, el / done, n, k, dt), flush=True)


def measured(where, names, bg):
    out = {}
    for n in names:
        p = os.path.join(where, slug(n) + ".npz")
        if not os.path.isfile(p):
            continue
        ev, hf = load_events(p)
        if not ev:
            out[n] = {"error": "no event"}
            continue
        r, pk, loud = preset_metrics(ev, bg)
        out[n] = {"rise": r, "peak": pk, "loud": loud, "rise_time": min(rise_time(e) for e in ev),
                  "onset_hf": max(hf) if hf else 0.0, "events": len(ev), "_ev": ev}
    return out


def pct(v, q):
    return float(np.percentile(np.array(v), q)) if v else float("nan")


def report(a):
    exe = a.exe or RENDER
    bg = background_power(exe)
    names = [n for n, _ in bank() if n != "Near Off"]
    m = measured(os.path.join(WORK, "bank"), names, bg)
    neg = measured(os.path.join(WORK, "negatives"), REMOVED, bg)
    good = {n: v for n, v in m.items() if "rise" in v}
    print("background %s: %.1f LUFS as it plays" % (BACKGROUND, -0.691 + 10 * np.log10(bg)))
    print("%d of %d near presets measured\n" % (len(good), len(names)))
    fmt = "%-34s %7.1f %7.1f %7.1f %7.2f %6.2f %4d"
    head = "%-34s %7s %7s %7s %7s %6s %4s" % ("preset", "rise", "peak", "loud", "risetm", "hf", "ev")
    print("removed by Rene (the negatives):")
    print(head)
    for n in REMOVED:
        v = neg.get(n)
        if v and "rise" in v:
            print(fmt % (n, v["rise"], v["peak"], v["loud"], v["rise_time"], v["onset_hf"], v["events"]))
    print("\nliked by Rene:")
    print(head)
    for n in liked():
        v = good.get(n)
        if v:
            print(fmt % (n, v["rise"], v["peak"], v["loud"], v["rise_time"], v["onset_hf"], v["events"]))
    for key in ("rise", "peak", "loud", "rise_time", "onset_hf"):
        v = [x[key] for x in good.values()]
        print("\n%-9s over the bank: min %.2f  10%% %.2f  median %.2f  90%% %.2f  99%% %.2f  max %.2f"
              % (key, min(v), pct(v, 10), pct(v, 50), pct(v, 90), pct(v, 99), max(v)))
    over_r = sorted((v["rise"], n) for n, v in good.items() if v["rise"] > RISE_DB)
    over_p = sorted((v["peak"], n) for n, v in good.items() if v["peak"] > PEAK_DB)
    either = {n for _, n in over_r} | {n for _, n in over_p}
    print("\nlimits: rise %.1f dB/100 ms, peak %.1f dB over the mean" % (RISE_DB, PEAK_DB))
    print("over the rise limit: %d; over the peak limit: %d; over either: %d of %d"
          % (len(over_r), len(over_p), len(either), len(good)))
    for lim_r in (6, 8, 10, 12, 15, 20):
        for lim_p in (6, 8, 9, 10, 12):
            k = sum(1 for v in good.values() if v["rise"] > lim_r or v["peak"] > lim_p)
            neg_ok = all(neg[n]["rise"] > lim_r or neg[n]["peak"] > lim_p for n in REMOVED if n in neg and "rise" in neg[n])
            lik_ok = all(good[n]["rise"] <= lim_r and good[n]["peak"] <= lim_p for n in liked() if n in good)
            print("  rise %4.1f peak %4.1f: %3d of the bank over; negatives caught %s, liked all pass %s"
                  % (lim_r, lim_p, k, neg_ok, lik_ok))
    if a.json:
        out = {n: {k: (round(x, 3) if isinstance(x, float) else x) for k, x in v.items() if k != "_ev"}
               for n, v in list(good.items()) + list(neg.items())}
        json.dump(out, open(a.json, "w", encoding="utf-8"), indent=1)
        print("wrote %s" % a.json)
    return 0


def refined(name, gain):
    """A corrected preset's gain with the trim the engine's own render asked for on top (near_night_refine.json)."""
    if gain >= 0.0 or not os.path.isfile(REFINE):
        return gain
    return gain + float(json.load(open(REFINE, encoding="utf-8")).get(name, 0.0))


def refine(a):
    """The table's corrections are computed from the event's curve scaled by the gain; the engine's path is not all
    linear (the master and the Balance answer the level they get), and a corrected event measured through it came
    out up to two decibels over what the curve said. Every corrected preset whose render with Night on lies within
    REFINE_DB of a limit (or over it) gets the excess, plus that, as a further trim -- added to what the file holds,
    so a second round corrects the first. Then: table, build, verify the refined ones again."""
    exe = a.exe or RENDER
    bg = background_power(exe)
    doc = json.load(open(TABLE, encoding="utf-8"))
    where = os.path.join(WORK, "verify-on")
    have = json.load(open(REFINE, encoding="utf-8")) if os.path.isfile(REFINE) else {}
    rows = dict(doc["presets"])
    rows.update(doc.get("removed", {}))
    got = measured(where, [n for n in doc["presets"] if n != "Near Off"], bg)
    got.update(measured(os.path.join(where, "removed"), list(doc.get("removed", {})), bg))
    changed = []
    for n, v in got.items():
        r = rows.get(n, {})
        if "rise" not in v or not (r.get("gain_db", 0.0) < 0.0):
            continue
        excess = max(v["rise"] - (RISE_DB - REFINE_DB), v["peak"] - (PEAK_DB - REFINE_DB))
        if excess > 0.0:
            trim = -0.25 * np.ceil(excess / 0.25)
            have[n] = round(float(have.get(n, 0.0)) + trim, 2)
            changed.append((n, v["rise"], v["peak"], trim))
    json.dump(dict(sorted(have.items())), open(REFINE, "w", encoding="utf-8", newline="\n"), indent=1)
    for n, r, p, t in changed:
        print("  %-34s rise %5.1f peak %5.1f -> a further %.2f dB" % (n, r, p, t))
    print("%d presets refined (%s); now: table, build, verify --only those" % (len(changed), REFINE))
    return 0


def table(a):
    """Computes every bank preset's correction from its curves and writes near_night.json and NearNight.inc."""
    exe = a.exe or RENDER
    bg = background_power(exe)
    presets = bank()
    m = measured(os.path.join(WORK, "bank"), [n for n, _ in presets if n != "Near Off"], bg)
    settings = dict(presets)
    missing = [n for n, _ in presets if n != "Near Off" and n not in m]
    if missing:
        sys.exit("near_night: %d presets not measured yet (%s ...): run measure first" % (len(missing), missing[0]))
    rows = {}
    for n, _ in presets:
        if n == "Near Off":
            rows[n] = {"fade_s": 0.0, "gain_db": 0.0}
            continue
        v = m[n]
        if "_ev" not in v:
            rows[n] = {"fade_s": 0.0, "gain_db": 0.0, "note": v.get("error", "")}
            continue
        length = float(setting(settings[n], "fore_length", 6.0))
        fade, gain = correction(v["_ev"], bg, length)
        gain = refined(n, gain)
        r1, p1, l1 = preset_metrics(v["_ev"], bg, gain, fade)
        rows[n] = {"fade_s": fade, "gain_db": gain,
                   "rise": round(v["rise"], 2), "peak": round(v["peak"], 2), "loud": round(v["loud"], 2),
                   "rise_after": round(r1, 2), "peak_after": round(p1, 2), "loud_after": round(l1, 2),
                   "rise_time": round(v["rise_time"], 2), "onset_hf": round(v["onset_hf"], 3),
                   "events": [c for c in (compact(e, bg, fade) for e in v["_ev"]) if c is not None]}
    neg = measured(os.path.join(WORK, "negatives"), REMOVED, bg)
    removed = {}
    for n, s in removed_presets():
        if n in neg and "_ev" in neg[n]:
            fade, gain = correction(neg[n]["_ev"], bg, float(setting(s, "fore_length", 6.0)))
            gain = refined(n, gain)
            r1, p1, l1 = preset_metrics(neg[n]["_ev"], bg, gain, fade)
            removed[n] = {"fade_s": fade, "gain_db": gain, "rise": round(neg[n]["rise"], 2),
                          "peak": round(neg[n]["peak"], 2), "loud": round(neg[n]["loud"], 2),
                          "rise_after": round(r1, 2), "peak_after": round(p1, 2), "loud_after": round(l1, 2)}
    doc = {"how": "Tools/library/near_night.py: each event of a near preset rendered alone over the silenced "
                  "background %s, K-weighted level in 100 ms windows every 10 ms, heard over the background's "
                  "loudness taken as steady; rise = steepest climb within 100 ms (dB), peak = loudest window over "
                  "the mean as heard over the span within 20 dB of the peak (dB); worst event of the render" % BACKGROUND,
           "background": BACKGROUND, "background_power": bg,
           "limits": {"rise_db_per_100ms": RISE_DB, "peak_db": PEAK_DB, "margin_db": MARGIN_DB},
           "event_format": "per event (near_night.py compact): onset = base64 of the levels over the background as "
                           "int8 quarter decibels (-128: under -32 dB), 10 ms apart, from %.1f s before the event to "
                           "100 ms after the fade; pairs = the pairs of windows 100 ms apart no other pair beats, as "
                           "such levels; span = the windows the event is heard over; rest_* = the span's loudest "
                           "window, mean (power over the background) and length after the onset" % PRE,
           "removed": removed, "presets": rows}
    json.dump(doc, open(TABLE, "w", encoding="utf-8", newline="\n"), indent=1)
    open(INC, "w", encoding="utf-8", newline="\n").write(inc_text(doc))
    touched = [n for n, r in rows.items() if r["fade_s"] > 0 or r["gain_db"] < 0]
    print("wrote %s and %s: %d of %d presets corrected" % (TABLE, INC, len(touched), len(rows) - 1))
    return 0


def inc_text(doc):
    lines = ["// Generated by Tools/library/near_night.py -- do not edit.",
             "/// The near layer's night guard (plan C3, 20.11.2026), one row per near preset in the bank's order: the",
             "/// fade-in in seconds and the gain in dB an event of it gets when Night is on. Limits: rise %.1f dB per "
             "100 ms," % doc["limits"]["rise_db_per_100ms"],
             "/// peak %.1f dB over the event's mean, both as heard over the background (%s)."
             % (doc["limits"]["peak_db"], doc["background"]),
             "const NearGuardRow NearGuardRows[] = {"]
    for n, r in doc["presets"].items():
        lines.append('    { "%s", %.2ff, %.2ff },' % (n, r["fade_s"], r["gain_db"]))
    lines.append("};")
    return "\n".join(lines) + "\n"


def check(a):
    """The static part (ctest near_night): the table names every preset of the bank in its order, the generated
    NearNight.inc is what the table makes, and every correction keeps its preset's curves inside the limits."""
    if not os.path.isfile(TABLE):
        print("near_night: no %s" % TABLE)
        return 1
    doc = json.load(open(TABLE, encoding="utf-8"))
    bad = []
    names = [n for n, _ in bank()]
    if list(doc["presets"].keys()) != names:
        miss = [n for n in names if n not in doc["presets"]]
        extra = [n for n in doc["presets"] if n not in names]
        bad.append("the table is not the bank: %d missing (%s), %d extra (%s)"
                   % (len(miss), ", ".join(miss[:3]), len(extra), ", ".join(extra[:3])))
    if not os.path.isfile(INC) or open(INC, encoding="utf-8").read() != inc_text(doc):
        bad.append("Core/src/NearNight.inc is not what the table makes (run: near_night.py table)")
    rl, pl = doc["limits"]["rise_db_per_100ms"], doc["limits"]["peak_db"]
    over = 0
    for n, r in doc["presets"].items():
        if not r.get("events"):
            continue
        ms = [compact_metrics(c, r["gain_db"], r["fade_s"]) for c in r["events"]]
        rise, peak = max(m[0] for m in ms), max(m[1] for m in ms)
        # the levels are stored in quarter decibels: allow that rounding, not more
        if rise > rl + 0.3 or peak > pl + 0.3:
            over += 1
            if over <= 5:
                bad.append("%s: rise %.1f, peak %.1f after its correction (fade %.2f s, %.2f dB)"
                           % (n, rise, peak, r["fade_s"], r["gain_db"]))
    if over > 5:
        bad.append("... %d presets over a limit after their correction" % over)
    for b in bad:
        print("near_night: " + b)
    print("near_night: %s (%d presets)" % ("FAIL" if bad else "every preset has its row and keeps inside the limits",
                                            len(doc["presets"]) - 1))
    return 1 if bad else 0


def verify(a):
    """Renders the bank (and the two removed presets) through the engine with Night on or off and counts the
    presets over a limit. The rendered curves are kept under work/.../verify-<night>."""
    exe = a.exe or RENDER
    bg = background_power(a.base_exe or exe)
    presets = [(n, s, True) for n, s in bank() if n != "Near Off"]
    if a.only:
        want = [x.strip() for x in a.only.split(",")]
        presets = [p for p in presets if p[0] in want]
    where = os.path.join(WORK, "verify-" + a.night)
    extra = ["--set", "fore_night=%d" % (1 if a.night == "on" else 0)]
    run_capture(exe, a.jobs, presets, where, lambda n: extra)
    if a.night == "on" and os.path.isfile(TABLE):
        doc = json.load(open(TABLE, encoding="utf-8"))
        rem = dict(removed_presets())
        items = [(n, rem[n], False) for n in REMOVED if n in doc.get("removed", {})]
        guard = {n: ["--near-guard", "%g,%g" % (doc["removed"][n]["fade_s"], doc["removed"][n]["gain_db"])]
                 for n, _, _ in items}
        run_capture(exe, a.jobs, items, os.path.join(where, "removed"), lambda n: extra + guard[n])
    m = measured(where, [p[0] for p in presets], bg)
    over = [(n, v["rise"], v["peak"]) for n, v in m.items() if "rise" in v and (v["rise"] > RISE_DB or v["peak"] > PEAK_DB)]
    print("Night %s: %d presets rendered, %d over a limit (rise %.1f dB/100 ms, peak %.1f dB)"
          % (a.night, len(m), len(over), RISE_DB, PEAK_DB))
    for n, r, p in sorted(over, key=lambda x: -max(x[1] - RISE_DB, x[2] - PEAK_DB))[:40]:
        print("  %-34s rise %5.1f  peak %5.1f" % (n, r, p))
    if a.night == "on":
        rm = measured(os.path.join(where, "removed"), REMOVED, bg)
        for n, v in rm.items():
            if "rise" in v:
                print("  removed %-26s rise %5.1f  peak %5.1f  (with its own row)" % (n, v["rise"], v["peak"]))
    # The guard draws no die and moves no event: every event begins when it began in the measurement of the bank.
    same = moved = 0
    for n, _, _ in presets:
        f0, f1 = os.path.join(WORK, "bank", slug(n) + ".npz"), os.path.join(where, slug(n) + ".npz")
        if os.path.isfile(f0) and os.path.isfile(f1):
            if np.array_equal(np.load(f0)["starts"], np.load(f1)["starts"]):
                same += 1
            else:
                moved += 1
                print("  events moved: %s" % n)
    print("event times against the measurement: %d presets the same, %d moved" % (same, moved))
    if a.json:
        json.dump({n: {k: x for k, x in v.items() if k != "_ev"} for n, v in m.items()},
                  open(a.json, "w", encoding="utf-8"), indent=1)
    return 0


def identity(a):
    """Night off against the build before the guard, bit for bit: each named near preset over its background as it
    plays (its own clock, 150 s), and the soundcheck's cases (Tools/soundcheck.py), rendered by both and compared
    sample for sample. --night on renders the new build with Night on instead (for a preset the guard leaves
    alone, that must be bit-identical too)."""
    if not a.base_exe:
        sys.exit("identity: --base-exe (the renderer before the change) is needed")
    exe = a.exe or RENDER
    sys.path.insert(0, os.path.dirname(HERE))
    import soundcheck
    out = os.path.join(WORK, "identity")
    os.makedirs(out, exist_ok=True)
    jobs = []
    for n in [x.strip() for x in a.only.split(",") if x.strip()]:
        jobs.append((n, ["--near-preset", n, "--seconds", "150", "--hour", "21"]))
    for name, args in soundcheck.CASES:
        jobs.append(("soundcheck " + name, list(args)))
    env = dict(os.environ)
    env["AMBIENT_PACKS"] = os.path.join(ROOT, "Library", "Packs")
    env.pop("AMBIENT_MUTE", None)
    flags = 0x4000 if os.name == "nt" else 0

    def one(job):
        n, args = job
        res = []
        for tag, e, extra in (("old", a.base_exe, []), ("new", exe, ["--set", "fore_night=%d" % (a.night == "on")])):
            w = os.path.join(out, "%s_%s.wav" % (slug(n), tag))
            cmd = [e] + (["--preset", BACKGROUND] if not n.startswith("soundcheck ") else []) + args + extra + ["--out", w]
            r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env,
                               creationflags=flags)
            if r.returncode != 0:
                return n, "render failed: " + r.stderr.strip()[-200:]
            x, _ = read_wav(w)
            res.append(x)
            os.remove(w)
        same = res[0].shape == res[1].shape and np.array_equal(res[0], res[1])
        return n, "identical" if same else "DIFFERENT (max %.3g)" % float(np.max(np.abs(res[0] - res[1])))

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs // 2)) as ex:
        results = list(ex.map(one, jobs))
    bad = 0
    for n, verdict in results:
        print("  %-40s %s" % (n, verdict))
        bad += verdict != "identical"
    print("identity (Night %s): %d of %d identical" % (a.night, len(results) - bad, len(results)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("what", nargs="?", default="report",
                    choices=["measure", "negatives", "report", "table", "refine", "verify", "identity"])
    ap.add_argument("--check", action="store_true", help="the static part, for ctest near_night")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", default="")
    ap.add_argument("--exe", default=None, help="the render tool (default bin/msvc, or $AMBIENT_RENDER)")
    ap.add_argument("--base-exe", default=None,
                    help="verify: the renderer the background was measured with; identity: the build before the change")
    ap.add_argument("--night", default="on", choices=["on", "off"])
    ap.add_argument("--json", default=None, help="report/verify: also write the numbers there")
    a = ap.parse_args()
    if a.check:
        return check(a)
    below_normal()
    exe = a.exe or RENDER
    if a.what == "measure":
        background_power(exe)
        items = [(n, s, True) for n, s in bank() if n != "Near Off"]
        if a.only:
            want = [x.strip() for x in a.only.split(",")]
            items = [it for it in items if it[0] in want]
        run_capture(exe, a.jobs, items, os.path.join(WORK, "bank"))
        return 0
    if a.what == "negatives":
        background_power(exe)
        run_capture(exe, a.jobs, [(n, s, False) for n, s in removed_presets()], os.path.join(WORK, "negatives"))
        return 0
    if a.what == "report":
        return report(a)
    if a.what == "table":
        return table(a)
    if a.what == "refine":
        return refine(a)
    if a.what == "verify":
        return verify(a)
    if a.what == "identity":
        return identity(a)
    return 0


if __name__ == "__main__":
    sys.exit(main())
