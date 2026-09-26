"""Render every preset of a pack library, then rewrite the packs from what was measured.

make_presets.py estimates a preset's descriptors from its settings, because it has to name
files before they exist. That estimate is good enough to lay a map out, but it is a prediction.
This tool replaces it with the real thing, the same way Tools/preset_map.py does for the
built-in presets -- five thousand renders is about twenty minutes on six threads, so there is
no reason to guess.

    python Tools/library/measure_packs.py --packs Library/Packs --jobs 6

Two things are written back into the pack files:

* the six descriptors and the map position, from the audio
* a corrected `master_gain`, so every preset lands inside the target loudness window. A preset
  that came out at -10 dBFS gets turned down by exactly the excess; one at -34 gets turned up.
  This is what stops a library of thousands from having a few dozen presets that jump out.

Nothing else in the settings is touched, and the run is idempotent: measuring an already
measured library changes the gains by fractions of a dB.
"""
import argparse
import hashlib
import json
import time
import concurrent.futures
import math
import os
import re
import subprocess
import tempfile
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
from make_presets import TAGS, layout, rank  # noqa: E402

RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")

# The window every preset is brought into. Drones live around -20..-28 dBFS; preset_check.py
# fails above -12, so the ceiling keeps a margin for a busier chord than the test's three notes.
TARGET_HI = -17.0
TARGET_LO = -30.0
# The loudness window in LUFS (25.09.2026), which replaces the unweighted RMS window above whenever
# the renderer prints its BS.1770 line (it does since the same day): -24 .. -18 LUFS integrated
# over the measured minute, the target docs/concept.md has quoted for the instrument's presets
# since the meter was built, K-weighted so a bass-heavy bed and a bright one are held to the same
# heard level. The RMS window stays as the fallback and the tenth meta token stays RMS, which is
# what the plugin's level match reads.
LUFS_HI = -16.0
LUFS_LO = -20.0
# -20 .. -16 since the second round of the guide (25.09.2026): the guide's window for dark ambient.
# The first round held -24 .. -18, the target this file's own comment had quoted; Rene chose the
# guide's numbers.
# The production guide's gates a gain cannot fix, reported rather than corrected: a crest (true
# peak over short-term loudness) under 12 dB is a bed without transients, a mono loss over 3 dB a
# stereo image that folds, a true peak over -1 dBTP a preset that clips a codec.
GUIDE_CREST_MIN = 12.0
GUIDE_MONO_MAX = 3.0
GUIDE_TRUEPEAK_MAX = -1.0
GAIN_MIN, GAIN_MAX = -40.0, 12.0


def sub_balance(m):
    """The guide's sub balance (25.09.2026): the sub's octaves (31.5 and 63 Hz) over 250 and 500 Hz,
    in dB of long-term power -- three to six is the window. None without a bands line."""
    b = m.get("bands") if m else None
    if not b or len(b) < 5:
        return None
    p = [10.0 ** (v / 10.0) for v in b]
    sub, mid = p[0] + p[1], p[3] + p[4]
    if mid <= 1e-30:
        return None
    return 10.0 * math.log10(max(sub, 1e-30) / mid)


# Every field of a pack line, in order. Reading a short list and writing it back is how the
# impulse, the matrix and the envelope shapes were silently dropped from all 5000 presets: the
# measurement pass rewrote each line with five fields instead of eight. Nine since impulse B.
FIELDS = ["name", "settings", "meta", "texture", "wavetable", "impulse", "mod", "envs", "impulse_b"]


def read_pack(path):
    head, rows = [], []
    for line in open(path, encoding="utf-8"):
        t = line.rstrip("\n")
        s = t.lstrip()
        # "format 2" belongs to the head. Read as a preset it would be written back as
        # "format 2||||||||", and the loader only takes a format line without a '|' -- so the pack
        # would fall back to format 1 and read every Harmonic source as a classic wavetable.
        if not s or s.startswith("#") or s.startswith("pack ") or (s.startswith("format ") and "|" not in s):
            head.append(t)
            continue
        f = t.split("|")
        while len(f) < len(FIELDS):
            f.append("")
        rows.append({k: f[i] for i, k in enumerate(FIELDS)})
    return head, rows


def write_pack(path, head, rows):
    with open(path, "w", encoding="utf-8") as f:
        for h in head:
            f.write(h + "\n")
        for r in rows:
            f.write("|".join(r[k] for k in FIELDS) + "\n")


MEASURE = re.compile(r"^measure: (.*)$", re.M)
LOUDNESS = re.compile(r"^loudness: (.*)$", re.M)
# The mel-cepstral fingerprint: 16 means and 16 spreads. Nine descriptors say what a preset
# is like; this says what it is, which is the difference between "dark and wide" and "a
# goods yard" -- two presets can agree on every descriptor and share no material at all.
TIMBRE = re.compile(r"^timbre:\s*(.*)$", re.M)
# Ten octave bands, 31.5 Hz to 16 kHz, in dB (the renderer's --bands, 25.09.2026): the guide's sub
# balance is read from them -- the sub (31.5 and 63 Hz) three to six decibels over 250 and 500 Hz.
BANDS = re.compile(r"^bands:\s*(.*)$", re.M)


# Renders run below normal priority. Five of them at full speed on a 24-thread machine still
# made the desktop stutter, and a batch that takes twenty minutes must not cost the machine.
LOW_PRIORITY = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}


# The engine loads its default pack folders unless AMBIENT_PACKS says otherwise, and a stale copy
# of an older library in ProgramData then wins every name collision -- which is how a measurement
# pass came to measure last week's presets under this week's names. Every render here is told
# exactly which folder to read.
def _packs_env(packs):
    env = dict(os.environ)
    env["AMBIENT_PACKS"] = os.path.abspath(packs)
    return env

def parse_measure(text):
    """The numbers out of one render's output, or None if it did not produce a usable line."""
    m = MEASURE.search(text or "")
    if not m:
        return None
    d = {}
    for tok in m.group(1).split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                d[k] = float(v)
            except ValueError:
                # Not every field on the line is a number. `hash=` was added to the measure
                # output after this tool was written, and treating an unparseable field as a
                # broken render made the whole run report "nothing rendered" -- except for the
                # seven presets in six thousand whose hash happened to read as a float. The
                # keys this tool needs are checked below; anything else on the line is not
                # its business.
                continue
    if not {"rms", "centroid", "flatness", "flux", "bass", "width", "voices"} <= set(d):
        return None
    d["rms_db"] = d.pop("rms")
    # The BS.1770 line (--loudness), when the renderer printed one: integrated and short-term
    # LUFS, loudness range, true peak and crest, measured over the same minute as the descriptors
    # (the renderer starts its meter after the warm-up).
    lm = LOUDNESS.search(text or "")
    if lm:
        for tok in lm.group(1).split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    d[k] = float(v)
                except ValueError:
                    continue
    b = BANDS.search(text or "")
    if b:
        try:
            d["bands"] = [float(x) for x in b.group(1).split()]
        except ValueError:
            pass
    t = TIMBRE.search(text or "")
    if t:
        try:
            d["timbre"] = [float(x) for x in t.group(1).split()]
        except ValueError:
            pass
    return d


def render_batch(names, packs, seconds, tapdir=None, skip=0.0):
    """One process, many presets: {name: measurements}. The renderer's --batch reuses the command
    line for every name in a list, so each preset is measured exactly as a single call would
    measure it -- what is saved is starting a process and reading the 42 pack files, which was
    0.44 s per preset of an eight-thousand-preset run."""
    listing = None
    try:
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8") as f:
            listing = f.name
            f.write("\n".join(names))
        # --hour pins the arc clock: without it the 496 presets that follow the time of day
        # measure differently every run, and the map's axes wander with them.
        cmd = [RENDER, "--packs", packs, "--batch", listing, "--seconds", str(seconds),
               "--notes", "45,52,59", "--set", "brain_rate=6", "--hour", "21", "--measure", "--loudness", "--bands"]
        if tapdir:
            cmd += ["--tap-dir", tapdir]
        if skip > 0.0:
            cmd += ["--skip", f"{skip:.0f}"]
        res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace",
                             env=_packs_env(packs), **LOW_PRIORITY)
    finally:
        if listing:
            try:
                os.remove(listing)
            except OSError:
                pass
    # The output is one "batch: <name>" line followed by that preset's own output. A preset whose
    # render failed simply has no measure line and comes back as None, exactly as before.
    out = {}
    chunks = re.split(r"^batch: (.*)$", res.stdout or "", flags=re.MULTILINE)
    for i in range(1, len(chunks) - 1, 2):
        out[chunks[i].strip()] = parse_measure(chunks[i + 1])
    return out


def render(name, packs, seconds, tapdir=None):
    """The synth measures its own render and prints one line. It used to write a twelve-second
    stereo WAV to a temporary file and read it straight back -- five thousand presets is
    twenty-three gigabytes written and read for nothing, and every byte stayed in the file cache
    afterwards, which is what made the machine unusable."""
    cmd = [RENDER, "--packs", packs, "--preset", name, "--seconds", str(seconds),
           "--notes", "45,52,59", "--set", "brain_rate=6", "--hour", "21", "--measure", "--loudness", "--bands"]
    if tapdir:
        # A twelve-second mono excerpt beside the numbers, for a learned embedding to listen to.
        safe = re.sub(r"[^A-Za-z0-9]+", "_", name)[:80]
        cmd += ["--tap", os.path.join(tapdir, safe + ".wav")]
    res = subprocess.run(cmd,
                         capture_output=True, text=True, encoding="utf-8", errors="replace",
                         env=_packs_env(packs), **LOW_PRIORITY)
    if res.returncode != 0:
        return None
    return parse_measure(res.stdout)



def set_gain(settings, gain_db):
    parts = [p for p in settings.split(";") if p and not p.startswith("master_gain=")]
    g = min(max(gain_db, GAIN_MIN), GAIN_MAX)
    return ";".join([f"master_gain={g:.4g}"] + parts)


def get_gain(settings):
    for p in settings.split(";"):
        if p.startswith("master_gain="):
            try:
                return float(p.split("=", 1)[1])
            except ValueError:
                return -6.0
    return -6.0


def tag_bits(settings, d):
    p = {}
    for kv in settings.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            p[k] = v
    num = lambda k: float(p.get(k, "0") or 0)
    t = set()
    t.add("Keys" if p.get("brain_on") == "off" else "Generative")
    if num("cosmos_send") > 0.05 or num("cosmos_shimmer") > 0.05: t.add("Cosmos")
    if num("fb_bus") > 0.02 or num("fb_fm") > 0.02: t.add("Feedback")
    if p.get("src2_type", "Off") != "Off" or p.get("src3_type", "Off") != "Off": t.add("Sources")
    if any(k in p.get("scale", "") for k in ("JI", "Harmonic", "Subharmonic", "Pythag", "Bohlen", "Otonal", "Slendro")):
        t.add("JustIntonation")
    if num("sub_level") > 0.05: t.add("Sub")
    if p.get("stack", "Detune") != "Detune": t.add("Stack")
    if num("air") >= 0.3: t.add("Air")
    if d["bright"] < 0.3: t.add("Dark")
    if d["bright"] > 0.7: t.add("Bright")
    if d["motion"] < 0.3: t.add("Calm")
    if d["motion"] > 0.7: t.add("Moving")
    if d["noisy"] < 0.4: t.add("Tonal")
    if d["noisy"] > 0.7: t.add("Noisy")
    if d["width"] > 0.7: t.add("Wide")
    if d["bass"] > 0.7: t.add("Bass")
    if d["density"] > 0.7: t.add("Dense")
    if d["density"] < 0.3: t.add("Sparse")
    return sum(1 << TAGS.index(x) for x in t)


def warm_up(settings):
    """How long this preset is played and thrown away before the measured minute begins. A preset
    is described after it has ARRIVED: the library's median attack is eighteen seconds and an
    eighth of it is above forty, and measured during the climb a preset reads quieter, thinner,
    drier and far more 'evolving' than it really is. Bucketed to fifteen seconds, because one
    --batch call shares one command line -- a bucket is a run of presets that wait equally long."""
    try:
        attack = float(dict(kv.split("=", 1) for kv in str(settings).split(";") if "=" in kv).get("attack", 6.0))
    except (ValueError, AttributeError):
        attack = 6.0
    step = 15.0
    return min(75.0, step * round(max(0.0, attack - 15.0) / step))


def _fingerprint(row, method=""):
    """What this measurement was made from. The name is not enough: regenerate the library and most
    names come back -- same seed, same generator -- with other settings underneath.

    And neither are the settings enough, which cost an afternoon to learn. Change HOW a preset is
    measured -- the length of the window, the notes held, the warm-up before it -- and every cached
    number belongs to the old method while the settings that key it have not moved. The safe answer
    used to be to throw the whole cache away, which is how a change touching an eighth of the
    library came to re-measure all of it. So the method is part of the key, and it is DERIVED
    rather than remembered: a preset whose warm-up is zero before and after keeps its measurement,
    one whose warm-up moved is measured again, and nobody has to bump a version by hand."""
    parts = [str(row.get(k, "")) for k in ("settings", "texture", "wavetable", "impulse", "mod", "envs", "impulse_b")]
    parts.append(str(method))
    parts.append(f"skip={warm_up(row.get('settings', '')):.0f}")
    return hashlib.sha1("".join(parts).encode("utf-8")).hexdigest()[:16]

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", default=os.path.join(ROOT, "Library", "Packs"))
    ap.add_argument("--jobs", type=int, default=3,
                    help="renders in parallel; three leaves a 24-thread machine usable")
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--limit", type=int, default=0, help="only the first N presets (a dry run)")
    # The render pass is the expensive part -- three hours for the whole library at a minute a
    # preset -- and the layout on top of it is seconds. So the measurements are cached: render
    # once into a JSON, then iterate on the embedding from that file as often as it takes.
    ap.add_argument("--cache", default="", help="write the raw measurements here after rendering")
    ap.add_argument("--from-cache", default="", help="read them from here instead of rendering")
    ap.add_argument("--no-write", action="store_true", help="stop after the cache; leave the packs alone")
    # The gain correction moves master_gain and therefore changes the SOUND. A round that is only
    # about where a preset sits on the map has no business doing that: pass this and the pass
    # rewrites positions, descriptors and tags, and not one sample of what the library plays.
    ap.add_argument("--no-gain", action="store_true", help="do not touch master_gain")
    ap.add_argument("--taps", default="", help="also write a 12 s mono excerpt of every preset here")
    ap.add_argument("--chunk", type=int, default=25,
                    help="presets per renderer process (one process for many; 1 = one each)")
    ap.add_argument("--resume", action="store_true",
                    help="skip presets the cache already holds (the cache is written as it goes)")
    # A second correction pass (26.09.2026): with --resume the window is otherwise applied to every
    # preset again, and one that sat under it after a capped lift would be lifted a second time.
    ap.add_argument("--gain-only-rendered", action="store_true",
                    help="correct the gain only of the presets this run rendered, not of those taken from the cache")
    # And by name, for a pass that was cut off: the gain is written only at the end of a run, so the
    # presets an interrupted pass had already measured sit in the cache without it -- and a rerun
    # renders only the rest (26.09.2026, a pass C stopped from outside at 3317 of 5603).
    ap.add_argument("--gain-names", default="",
                    help="a file of preset names, one per line: correct the gain of these only")
    a = ap.parse_args()
    a.chunk = max(1, a.chunk)   # zero would be range(0, n, 0)
    rendered = set()

    if a.taps:
        os.makedirs(a.taps, exist_ok=True)
    files = sorted(f for f in os.listdir(a.packs) if f.endswith(".ambientpack"))
    packs = [(f, *read_pack(os.path.join(a.packs, f))) for f in files]
    rows = [r for _, _, rr in packs for r in rr]
    if a.limit:
        rows = rows[:a.limit]
    print(f"{len(rows)} presets in {len(packs)} packs, {a.jobs} renders in parallel")

    if a.from_cache:
        cached = json.load(open(a.from_cache, encoding="utf-8"))
        for r in rows:
            r["m"] = cached.get(r["name"])
        print(f"  read {sum(1 for r in rows if r['m'])} measurements from {a.from_cache}")
    else:
        # The cache is written as the run goes, not at the end. Eight thousand presets is hours,
        # and a run that loses everything to one interruption is a run nobody dares interrupt --
        # which is how an afternoon went once. With --resume a second run picks up what is there.
        have = {}
        if a.cache and a.resume and os.path.exists(a.cache):
            try:
                have = json.load(open(a.cache, encoding="utf-8"))
            except (ValueError, OSError):
                have = {}
        # A cached measurement belongs to the preset it was made from, and the only thing tying the
        # two together is the name. Regenerate the library and most names come back -- same seed,
        # same generator -- carrying other settings underneath, and --resume then keeps yesterday's
        # numbers for them without a word. It happened: a work directory left over from an earlier
        # run held ninety entries, and ninety presets went into the map with the descriptors and
        # the excerpt of a library that no longer existed. So the settings are part of the key.
        method = f"sec={a.seconds}|notes=45,52,59|hour=21|rate=6|loud=1"
        fps = {r["name"]: _fingerprint(r, method) for r in rows}
        fpfile = a.cache + ".fp" if a.cache else ""
        known = {}
        if fpfile and os.path.exists(fpfile):
            try:
                known = json.load(open(fpfile, encoding="utf-8"))
            except (ValueError, OSError):
                known = {}
            stale = [n for n in have if known.get(n) != fps.get(n)]
            for n in stale:
                del have[n]
            if stale:
                print(f"  {len(stale)} cached measurements belong to other settings and are dropped")
        elif have:
            print("  no fingerprints beside the cache: trusting it by name, as before")
        todo = [r for r in rows if r["name"] not in have]
        rendered = {r["name"] for r in todo}
        for r in rows:
            r["m"] = have.get(r["name"])
        if have:
            print(f"  {len(have)} already measured, {len(todo)} to go")

        def flush():
            if not a.cache:
                return
            tmp = a.cache + ".part"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump({r["name"]: r["m"] for r in rows if r["m"]}, f)
            os.replace(tmp, a.cache)          # never a half-written cache on disk
            with open(fpfile + ".part", "w", encoding="utf-8") as f:
                json.dump({r["name"]: fps[r["name"]] for r in rows if r["m"]}, f)
            os.replace(fpfile + ".part", fpfile)

        t0 = time.time()
        # Chunks, not presets: one process measures a run of them (see render_batch). Small enough
        # that an interruption loses little and the cache is written often, large enough that the
        # fixed cost of a process is paid once for many.
        by_name = {r["name"]: r for r in todo}
        # A preset is described AFTER it has arrived. The 2.0 library has a median attack of
        # eighteen seconds and an eighth of it above forty: those presets were being described, and
        # their loudness set, while they were still climbing -- measured on six of them, the sound
        # stands about 3 dB under where it settles. The renderer plays a warm-up and throws it away
        # (--skip); the measured window that follows is sixty seconds for every preset, so the
        # descriptors stay comparable. The warm-up is bucketed because one --batch call shares one
        # command line: a bucket is a run of presets that wait the same length of time.
        def lead_of(row):
            try:
                attack = float(dict(kv.split("=", 1) for kv in row["settings"].split(";") if "=" in kv)
                               .get("attack", 6.0))
            except (ValueError, AttributeError):
                attack = 6.0
            step = 15.0
            return min(75.0, step * round(max(0.0, attack - 15.0) / step))
        buckets = {}
        for r in todo:
            buckets.setdefault(lead_of(r), []).append(r["name"])
        chunks = [(lead, names[i:i + a.chunk])
                  for lead, names in sorted(buckets.items())
                  for i in range(0, len(names), a.chunk)]
        if len(buckets) > 1:
            print("  warm-up before the measured minute: "
                  + ", ".join(f"{int(k)} s x{len(v)}" for k, v in sorted(buckets.items())), flush=True)
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            done = 0
            for got in ex.map(lambda c: render_batch(c[1], a.packs, a.seconds, a.taps or None, c[0]), chunks):
                for name, m in got.items():
                    if name in by_name:
                        by_name[name]["m"] = m
                before = done
                done += len(got)
                el = time.time() - t0
                # Whenever a multiple of the chunk is crossed, not landed on: one short chunk (a
                # preset that did not render) put every later count off the multiples, and the
                # progress went silent for the rest of the run (26.09.2026).
                if done // max(a.chunk, 1) != before // max(a.chunk, 1) or done >= len(todo):
                    print(f"  {done}/{len(todo)}  {el/60:.0f} min, noch etwa {el/max(done,1)*(len(todo)-done)/60:.0f} min", flush=True)
                flush()
        flush()
        if a.cache:
            print(f"  cached {sum(1 for r in rows if r['m'])} measurements in {a.cache}")
        if a.no_write:
            print("cache only (--no-write): the pack files were not touched")
            return 0
    bad = [r["name"] for r in rows if r["m"] is None]
    if bad:
        print(f"{len(bad)} presets failed to render: {', '.join(bad[:5])}")
    good = [r for r in rows if r["m"] is not None]
    if not good:
        raise SystemExit("nothing rendered")

    # Loudness: move each preset's master gain by exactly the distance to the window -- the LUFS
    # window where the renderer measured LUFS (25.09.2026), the RMS window otherwise.
    moved = 0
    def has_lufs(r):
        return r["m"].get("lufs_i", -200.0) > -100.0
    gain_names = None
    if a.gain_names:
        with open(a.gain_names, encoding="utf-8") as fh:
            gain_names = {ln.strip() for ln in fh if ln.strip()}
    for r in [] if a.no_gain else good:
        if a.gain_only_rendered and r["name"] not in rendered:
            continue
        if gain_names is not None and r["name"] not in gain_names:
            continue
        delta = 0.0
        if has_lufs(r):
            lufs = r["m"]["lufs_i"]
            if lufs > LUFS_HI:
                delta = LUFS_HI - lufs
            elif lufs < LUFS_LO:
                # Never shout a quiet preset awake -- but a lift of twelve decibels, not six, since
                # 26.09.2026: the guide's engine (the horizon twenty decibels down, the halls fed no
                # fundamentals, the sub an octave lower) took the whole library down by about eight,
                # median -28.1 LUFS measured, and at six the window stayed out of reach for most of
                # it. The true peak below still has the last word.
                delta = min(LUFS_LO - lufs, 12.0)
            # And never past the guide's true peak (25.09.2026): a lift stops at -1 dBTP, and a
            # preset already over it comes down to it, whatever that leaves of the window.
            tp = r["m"].get("truepeak", -120.0)
            if tp > -100.0 and tp + delta > GUIDE_TRUEPEAK_MAX:
                delta = GUIDE_TRUEPEAK_MAX - tp
        else:
            rms = r["m"]["rms_db"]
            if rms > TARGET_HI:
                delta = TARGET_HI - rms
            elif rms < TARGET_LO:
                delta = min(TARGET_LO - rms, 6.0)
        if abs(delta) > 0.2:
            r["settings"] = set_gain(r["settings"], get_gain(r["settings"]) + delta)
            r["gain_delta"] = delta       # the meta line stores the loudness after this correction
            moved += 1
    rmsv = np.array([r["m"]["rms_db"] for r in good])
    print(f"loudness before: median {np.median(rmsv):.1f} dBFS, {int((rmsv > TARGET_HI).sum())} above "
          f"{TARGET_HI:.0f}, {int((rmsv < TARGET_LO).sum())} below {TARGET_LO:.0f}; {moved} gains corrected")
    withl = [r for r in good if has_lufs(r)]
    if withl:
        lv = np.array([r["m"]["lufs_i"] for r in withl])
        print(f"LUFS before: median {np.median(lv):.1f}, {int((lv > LUFS_HI).sum())} above {LUFS_HI:.0f}, "
              f"{int((lv < LUFS_LO).sum())} below {LUFS_LO:.0f} (window {LUFS_LO:.0f} .. {LUFS_HI:.0f} LUFS)")
        # The guide's gates a gain does not fix: listed, counted, written beside the cache.
        report = {"crest_under_12": [], "mono_loss_over_3": [], "true_peak_over_-1": [],
                  "correlation_outside_0.3_0.7": [], "lra_under_8": [], "sub_balance_outside_3_6": []}
        for r in withl:
            m = r["m"]; g = r.get("gain_delta", 0.0)
            if m.get("crest", 99.0) < GUIDE_CREST_MIN: report["crest_under_12"].append([r["name"], round(m["crest"], 1)])
            if m.get("monoloss", 0.0) > GUIDE_MONO_MAX: report["mono_loss_over_3"].append([r["name"], round(m["monoloss"], 2)])
            if m.get("truepeak", -99.0) + g > GUIDE_TRUEPEAK_MAX: report["true_peak_over_-1"].append([r["name"], round(m["truepeak"] + g, 1)])
            if "corr" in m and not (0.3 <= m["corr"] <= 0.7): report["correlation_outside_0.3_0.7"].append([r["name"], round(m["corr"], 2)])
            if "lra" in m and m["lra"] < 8.0: report["lra_under_8"].append([r["name"], round(m["lra"], 1)])
            sb = sub_balance(m)
            if sb is not None and not (3.0 <= sb <= 6.0): report["sub_balance_outside_3_6"].append([r["name"], round(sb, 1)])
        print("guide gates: crest < %.0f dB: %d, mono loss > %.0f dB: %d, true peak > %.0f dBTP: %d (of %d measured)"
              % (GUIDE_CREST_MIN, len(report["crest_under_12"]), GUIDE_MONO_MAX, len(report["mono_loss_over_3"]),
                 GUIDE_TRUEPEAK_MAX, len(report["true_peak_over_-1"]), len(withl)))
        print("guide targets: correlation outside 0.3 .. 0.7: %d, loudness range under 8 LU: %d, sub balance outside 3 .. 6 dB: %d"
              % (len(report["correlation_outside_0.3_0.7"]), len(report["lra_under_8"]), len(report["sub_balance_outside_3_6"])))
        if a.cache:
            path = os.path.join(os.path.dirname(os.path.abspath(a.cache)), "guide-report.json")
            with open(path, "w", encoding="utf-8") as f:
                json.dump(report, f, indent=1)
            print("guide report:", path)

    # Descriptors: rank each measurement across the library, exactly as preset_map.py does.
    raw = np.array([[r["m"]["centroid"], r["m"]["flux"], r["m"]["width"], r["m"]["flatness"],
                     r["m"]["bass"], r["m"]["voices"]] for r in good], dtype=np.float64)
    raw[:, 0] = np.log(np.maximum(raw[:, 0], 1.0))     # brightness is heard logarithmically
    desc = np.zeros_like(raw)
    for c in range(raw.shape[1]):
        desc[:, c] = rank(raw[:, c])
    xy = layout(desc)
    for i, r in enumerate(good):
        d = {"bright": desc[i, 0], "motion": desc[i, 1], "width": desc[i, 2],
             "noisy": desc[i, 3], "bass": desc[i, 4], "density": desc[i, 5]}
        bits = tag_bits(r["settings"], d)
        r["meta"] = (" ".join(f"{v:.3f}" for v in (xy[i, 0], xy[i, 1], d["bright"], d["motion"],
                                                   d["width"], d["noisy"], d["bass"], d["density"]))
                     + f" {bits} {r['m']['rms_db'] + r.get('gain_delta', 0.0):.1f}")   # tenth token: the loudness it now plays at

    if a.limit:
        print("dry run (--limit): the pack files were not rewritten")
        return 0
    for name, head, rr in packs:
        head = [h for h in head if "estimated" not in h]
        if not any("measured" in h for h in head):
            head.insert(1, "# Descriptors and map positions measured by Tools/library/measure_packs.py "
                           "from a 12 s render of every preset.")
        write_pack(os.path.join(a.packs, name), head, rr)
    # The gain correction rewrote the settings these measurements were made from. The cache follows
    # them -- only the loudness moved, and by exactly the gain -- and so do the fingerprints beside
    # it. Without this a second --resume run calls every corrected preset stale and renders the
    # whole library again for nothing.
    if a.cache and not a.from_cache:
        # Every level the correction moved, not the RMS alone: with only rms_db following the gain, a
        # --resume run read the LUFS of before the correction and applied it a second time (25.09.2026).
        def moved(m, g):
            out = dict(m, rms_db=m["rms_db"] + g)
            for k in ("lufs_i", "lufs_s", "lufs_m", "truepeak"):
                if k in out and out[k] > -100.0:
                    out[k] = out[k] + g
            return out
        with open(a.cache, "w", encoding="utf-8") as fh:
            json.dump({r["name"]: moved(r["m"], r.get("gain_delta", 0.0)) for r in rows if r["m"]}, fh)
        with open(a.cache + ".fp", "w", encoding="utf-8") as fh:
            json.dump({r["name"]: _fingerprint(r, f"sec={a.seconds}|notes=45,52,59|hour=21|rate=6|loud=1")
                       for r in rows if r["m"]}, fh)
    # Report what actually survived the rewrite: dropping a field silently is exactly how the
    # impulses, the matrix and the envelope shapes disappeared from all 5000 presets once.
    kept = {k: sum(1 for _, _, rr in packs for r in rr if r.get(k, "").strip("~ "))
            for k in ("texture", "wavetable", "impulse", "mod", "envs", "impulse_b")}
    print(f"rewrote {len(packs)} packs; presets carrying "
          + ", ".join(f"{k} {v}" for k, v in kept.items()))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
