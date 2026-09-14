"""How loud each near preset's event stands against the background it plays over -- measured.

Rene waited twenty minutes in a journey and heard nothing near (13.09.2026). Part of that was rarity
and a fault in how Auto scaled the clock; part was loudness. The bank's presets were voiced by ear, one
at a time, and measured together their loudest moments lie some thirty decibels apart against the same
background: a singing bowl stands well over the drone, a whistler or a sequence of soft steps under it.
An event is one voice with one source slot and its presets' Level was never a calibration; this makes one.

For every near preset of the bank, over several background presets that differ in the ways that
decide masking (dense, sparse, bright, dark):

  1. the background alone, as it plays, once per background: its loudness;
  2. the same background with every source that makes sound of its own switched off -- the four slots,
     the Foundation, the Strike, the Air -- but its conductor, its rooms and its buses left as they are,
     with the near preset on top and its clock set fast so the first event comes early: the event alone,
     through everything it passes on its way out;
  3. the silenced background without the near preset, once, to prove it IS silent.

The first version took the difference of two renders, which are sample-identical until the event. It
was not the event alone: an event changes the background it plays in -- the conductor holds, a running
sequence halves its pace, the far reverb is ducked -- and the difference counted those changes as the
event. Every sequence of the bank came out at the same number for a given background (+5.5 over one,
-7.5 over another) whatever it was, and one preset spread over 54 dB.

What is reported per pair is loudness as the ear weights it, to ITU-R BS.1770 (the K curve, the
engine's own Loudness.h): the event's loudest momentary loudness -- 400 ms windows, 100 ms apart --
against the background's loudness over the same span. Not plain RMS. The first version of this
measurement used RMS and said every near type was quieter than the drone, by 5 to 33 dB; RMS is ruled
by the bass, a drone is mostly bass and a singing bowl has almost none, so a bowl the ear hears plainly
came out "10 dB under". The median over the backgrounds is the preset's ratio; the trim that brings it
to the target is target - ratio. Written as JSON for Tools/make_layer_presets.py.

    python Tools/library/near_loudness.py [--jobs 6] [--target 0] [--out build/library-work/near_loudness.json]
        [--only "Bunker Tube,Whistler"] [--backgrounds "A,B,C"] [--write-gains]
    python Tools/library/near_loudness.py --from-json build/library-work/near_loudness.json   # gains only

Then python Tools/make_layer_presets.py folds Tools/library/near_gain.json into Core/src/NearPresets.inc.
The measurement hears the bank as it is built, gains included, so a new trim is added to the gain the
file already holds: measuring again after a change corrects the calibration rather than replacing it.
"""
import argparse
import concurrent.futures
import json
import os
import re
import statistics
import subprocess
import sys

import numpy as np
import scipy.signal
import soundfile as sf


def k_weight(x, sr):
    """BS.1770-4's K curve: the head's high shelf, then the revised low-frequency B high pass.
    The published coefficients are for 48 kHz, which is what the renderer writes."""
    if sr != 48000:
        raise ValueError("K-weighting coefficients here are for 48 kHz, got %d" % sr)
    b1, a1 = [1.53512485958697, -2.69169618940638, 1.19839281085285], [1.0, -1.69065929318241, 0.73248077421585]
    b2, a2 = [1.0, -2.0, 1.0], [1.0, -1.99004745483398, 0.99007225036621]
    return scipy.signal.lfilter(b2, a2, scipy.signal.lfilter(b1, a1, x, axis=0), axis=0)


def lufs(power_sum):
    """Loudness of a summed channel power (BS.1770: -0.691 + 10 log10 of the sum over channels)."""
    return -0.691 + 10.0 * np.log10(power_sum + 1e-24)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
WORK = os.path.join(ROOT, "build", "library-work", "near-loudness")
# Candidates, in order: dense, dark, bright, then sparse ones. The first four that fall silent when their
# own sources are switched off are used. Warp Vigil did not -- something in it went on sounding at -24 LU
# of its own level with every source off, and every quiet event measured over it read as that floor.
BACKGROUNDS = ["Kiln Descent", "Soot Passage", "Rushes Interval", "Monument Reach", "Thimble Veil",
               "Icecube Field", "Aeon Reach", "Warp Vigil"]
USE = 4
FLOOR_LU = -40.0      # a silenced background must be at least this far under itself
EVENT_MAX = 240.0     # seconds of an event that are measured: a sequence's filter opens over its run
SECONDS = 95          # the background renders, and a short event's render; long ones render longer


def lengths():
    """fore_length of every near preset, so a long event is rendered long enough to be heard whole."""
    s = open(os.path.join(ROOT, "Core", "src", "NearPresets.inc"), encoding="utf-8").read()
    out = {}
    for n, settings in re.findall(r'\{\s*"([^"]+)",\s*"([^"]*)"', s):
        m = re.search(r"fore_length=([\d.]+)", settings)
        out[n] = float(m.group(1)) if m else 6.0
    return out

env = dict(os.environ)
env["AMBIENT_PACKS"] = os.path.join(ROOT, "Library", "Packs")
env.pop("AMBIENT_MUTE", None)


def bank():
    s = open(os.path.join(ROOT, "Core", "src", "NearPresets.inc"), encoding="utf-8").read()
    return [n for n, _ in re.findall(r'\{\s*"([^"]+)",\s*"([^"]*)"', s) if n != "Near Off"]


def render(out, background, extra, seconds=SECONDS):
    cmd = [RENDER, "--preset", background, "--seconds", str(int(seconds)), "--hour", "21", "--near-log", "--out", out] + extra
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", env=env)
    return r.stdout


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "_", s).strip("_")


# What makes sound of its own in a sound preset. Off, the conductor still plays -- silently -- so the
# near events still find a cluster to be consonant with and a stillness to enter, and the rooms, the
# buses and the master still shape the event exactly as they would.
SILENCE = ["--set", "osc_level=0", "--set", "src2_level=0", "--set", "src3_level=0", "--set", "src4_level=0",
           "--set", "sub_level=0", "--set", "strike_level=0", "--set", "air=0"]


def k_loudness(x, sr, i0, i1):
    """K-weighted samples over [i0, i1), with a second before so the filters have settled."""
    pad = min(i0, int(sr))
    return k_weight(x[i0 - pad:i1], sr)[pad:]


def measure(near, background, off_path, length_hint):
    on_path = os.path.join(WORK, "%s__%s.wav" % (slug(background), slug(near)))
    seconds = max(SECONDS, 60.0 + min(length_hint, EVENT_MAX) + 5.0)
    log = render(on_path, background, SILENCE + ["--near-preset", near, "--set", "fore_rate=10"], seconds)
    m = re.search(r"near event 1 at \S+ \(([\d.]+) s\).*?length ([\d.]+) s", log)
    if not m:
        if os.path.exists(on_path):
            os.remove(on_path)
        return dict(near=near, background=background, error="no event in %d s" % seconds)
    at, length = float(m.group(1)), float(m.group(2))
    on, sr = sf.read(on_path, dtype="float64", always_2d=True)
    off, _ = sf.read(off_path, dtype="float64", always_2d=True)
    os.remove(on_path)
    n = min(len(on), len(off))
    i0 = int(at * sr)
    i1 = min(n, i0 + int(min(max(length, 1.0), EVENT_MAX) * sr))
    if i1 - i0 < sr:
        return dict(near=near, background=background, error="event too close to the end")
    ev = k_loudness(on, sr, i0, i1)
    bg = k_loudness(off, sr, i0, i1)
    win, hop = int(0.4 * sr), int(0.1 * sr)
    momentary = [lufs(float(np.sum(np.mean(ev[k:k + win] ** 2, axis=0)))) for k in range(0, len(ev) - win + 1, hop)]
    event = max(momentary)
    background_lufs = lufs(float(np.sum(np.mean(bg ** 2, axis=0))))
    return dict(near=near, background=background, at=at, length=length,
                event_lufs=round(event, 2), background_lufs=round(background_lufs, 2),
                ratio_db=round(event - background_lufs, 2))


GAIN_RANGE = (-24.0, 36.0)   # fore_gain range in the parameter table


def write_gains(result):
    """The trims as fore_gain values for the generator. A trim that falls outside the parameter's
    range is held at its edge and said so: that preset needs its Level or its source looked at."""
    path = os.path.join(ROOT, "Tools", "library", "near_gain.json")
    before = json.load(open(path, encoding="utf-8")).get("gain_db", {}) if os.path.isfile(path) else {}
    gains, clipped = dict(before), []
    for name, v in result["presets"].items():
        if "trim_db" not in v:
            continue
        want = float(before.get(name, 0.0)) + v["trim_db"]   # what was measured already had this gain in it
        g = min(max(want, GAIN_RANGE[0]), GAIN_RANGE[1])
        if g != want:
            clipped.append("%s (%+.1f)" % (name, want))
        gains[name] = round(g, 1)
    doc = {"target_lu": result["target_db"], "backgrounds": result["backgrounds"],
           "how": "Tools/library/near_loudness.py: loudest momentary loudness (BS.1770, 400 ms) of the first event "
                  "against the background's loudness over the event, median over the backgrounds",
           "gain_db": dict(sorted(gains.items()))}
    json.dump(doc, open(path, "w", encoding="utf-8"), indent=1)
    print("wrote %s: %d gains%s" % (path, len(gains), ("; held at the range edge: " + ", ".join(clipped)) if clipped else ""))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--target", type=float, default=0.0,
                    help="LU of the event's loudest moment against the background's loudness")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "library-work", "near_loudness.json"))
    ap.add_argument("--only", default="")
    ap.add_argument("--backgrounds", default=",".join(BACKGROUNDS))
    ap.add_argument("--write-gains", action="store_true",
                    help="also write Tools/library/near_gain.json, which make_layer_presets.py folds into the bank")
    ap.add_argument("--from-json", default="", help="skip measuring: turn an earlier --out file into gains")
    a = ap.parse_args()
    if a.from_json:
        write_gains(json.load(open(a.from_json, encoding="utf-8")))
        return 0
    os.makedirs(WORK, exist_ok=True)
    names = bank()
    if a.only:
        want = [s.strip() for s in a.only.split(",")]
        names = [n for n in names if n in want]
    backs = [b.strip() for b in a.backgrounds.split(",") if b.strip()]

    # Each background as it plays, and silenced -- which has to be silent, or it is not the event
    # that is measured over it.
    offs, floors, chosen = {}, {}, []
    off_seconds = 60.0 + EVENT_MAX + 10.0    # as long as the longest event's render
    for b in backs:
        if len(chosen) >= USE:
            break
        p = os.path.join(WORK, "%s__OFF.wav" % slug(b))
        render(p, b, [], off_seconds)
        q = os.path.join(WORK, "%s__QUIET.wav" % slug(b))
        render(q, b, SILENCE, SECONDS)
        x, sr = sf.read(p, dtype="float64", always_2d=True)
        y, _ = sf.read(q, dtype="float64", always_2d=True)
        os.remove(q)
        i0 = int(10 * sr)
        loud = lufs(float(np.sum(np.mean(k_loudness(x, sr, i0, len(x)) ** 2, axis=0))))
        floor = lufs(float(np.sum(np.mean(k_loudness(y, sr, i0, len(y)) ** 2, axis=0))))
        ok = floor - loud <= FLOOR_LU
        print("background %-18s %6.1f LUFS as it plays, silenced %7.1f LUFS (%+.1f LU)%s"
              % (b, loud, floor, floor - loud, "" if ok else "  -- not silent, left out"), flush=True)
        if not ok:
            os.remove(p)
            continue
        offs[b] = p
        floors[b] = round(floor - loud, 1)
        chosen.append(b)
    backs = chosen
    hint = lengths()
    print("%d near presets over %d backgrounds" % (len(names), len(backs)), flush=True)

    rows = []
    jobs = [(n, b) for n in names for b in backs]
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        futs = [ex.submit(measure, n, b, offs[b], hint.get(n, 6.0)) for n, b in jobs]
        for i, f in enumerate(concurrent.futures.as_completed(futs), 1):
            rows.append(f.result())
            if i % 40 == 0:
                print("  %d/%d" % (i, len(jobs)), flush=True)

    result = {"target_db": a.target, "backgrounds": backs, "silenced_floor_lu": floors, "presets": {}}
    for n in names:
        mine = [r for r in rows if r["near"] == n and "error" not in r]
        if not mine:
            result["presets"][n] = {"error": "; ".join(r.get("error", "?") for r in rows if r["near"] == n)}
            continue
        ratios = [r["ratio_db"] for r in mine]
        med = statistics.median(ratios)
        result["presets"][n] = {"ratio_db": round(med, 2), "spread_db": round(max(ratios) - min(ratios), 2),
                                "trim_db": round(a.target - med, 1), "per_background": {r["background"]: r["ratio_db"] for r in mine}}
    for b in offs.values():
        os.remove(b)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(result, open(a.out, "w", encoding="utf-8"), indent=1)

    good = {n: v for n, v in result["presets"].items() if "ratio_db" in v}
    ratios = sorted(v["ratio_db"] for v in good.values())
    print()
    print("%d measured, %d failed" % (len(good), len(names) - len(good)))
    if ratios:
        print("event against background: quietest %.1f LU, median %.1f LU, loudest %.1f LU" % (ratios[0], statistics.median(ratios), ratios[-1]))
        print("spread over the backgrounds: median %.1f LU, worst %.1f LU"
              % (statistics.median(v["spread_db"] for v in good.values()), max(v["spread_db"] for v in good.values())))
        trims = sorted(v["trim_db"] for v in good.values())
        print("trims to %+.0f LU: %.1f .. %.1f, median %.1f" % (a.target, trims[0], trims[-1], statistics.median(trims)))
    if a.write_gains:
        write_gains(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
