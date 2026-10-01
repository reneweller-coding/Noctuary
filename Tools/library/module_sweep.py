"""Does every control actually do something?

Rene turned the Cloud fully up and heard no difference; the far reverb likewise. Both times the
answer took an hour of bisecting one preset. This asks the question of every parameter at once,
and it asks it the sharpest way there is: the renderer prints a hash of the audio it made, so two
renders differing in one setting and agreeing in their hash differ in nothing at all.

A control is only meaningful when the thing behind it is running, so every section is given one or
more CONTEXTS -- the settings that switch it on -- and a control counts as dead only when it is
dead in every one of them. That distinction is the whole tool: a first version without it called
two hundred and fifty controls dead, of which almost all were simply being asked in a room where
they had nothing to do. `src3_stretch` does nothing to a wavetable; `brain_wander` does nothing
while a held note pins the root; an LFO at one cycle in thirty-three seconds does nothing in a
twelve-second render. None of those is a fault, and a report full of them is worse than no report.

    python Tools/library/module_sweep.py --jobs 14
    python Tools/library/module_sweep.py --section Cloud --seconds 20
"""
import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
RENDER = os.path.join(ROOT, "bin", "msvc", "ambient_render.exe")
MEASURE = re.compile(r"^measure: (.*)$", re.M)

SECONDS = {"Cluster Brain": 40.0, "Brain 2": 40.0, "Autoplay": 40.0, "Memory": 20.0}
HELD = "45,52,59"          # a chord, for everything that shapes a sounding note
FREE = ""                  # nothing held, for the conductors: a held note pins the root

# The nine things a source can be. Most of a slot's forty parameters belong to one or two of them,
# so each slot is asked once per type and a control is dead only if no type gives it anything.
TYPES = ["Additive", "Harmonic", "Wavetable", "Texture", "Stretch", "Spectral", "Noise", "FM", "Bow"]

# A clip and a table, because Texture, Stretch and Spectral have nothing to play without one and
# every knob behind them is then correctly dead. Reported as dead in the first two runs, all of it.
CLIP = os.path.join(ROOT, "Library", "Textures", "bowed-low-string-00712_D3.flac")
TABLE = os.path.join(ROOT, "Library", "Wavetables", "Harmonic", "harmonic_reed_034.wav")

ENV_SHAPE = "0:0:0/0.3:1:0/1:0.4:0.0!s2/2:0:0"      # with a sustain point, so the modes differ      # something with corners, so a mode or a time shows


def source_contexts(n):
    lvl = "osc_level" if n == 1 else f"src{n}_level"
    return [(t, [f"src{n}_type={t}", f"{lvl}=0.9"] + (["partials=16"] if t == "Additive" else []), HELD)
            for t in TYPES]


# section -> [(label, settings, notes), ...]
def contexts():
    c = {
        "Cloud":        [("on", ["cloud_send=0.8"], HELD)],
        "Cosmos":       [("on", ["cosmos_send=0.8", "cosmos_return=0.8"], HELD)],
        "Memory":       [("on", ["mem_send=0.8", "mem_return=0.8"], HELD)],
        "Room":         [("on", ["room_mix=0.8"], HELD)],
        "Early Room":   [("on", ["early_mix=0.8"], HELD)],
        "Strike":       [("on", ["strike_level=0.8", "strike_chance=1"], HELD)],
        "Z-Plane":      [("filter", ["z_mode=1", "z_mix=0.9", "z_depth=0.8"], HELD),
                         ("modal", ["z_mode=3", "z_mix=0.9", "z_depth=0.8"], HELD)],
        "Delay":        [("on", ["dly_mix=0.6", "dly_time_l=0.4", "dly_time_r=0.5", "dly_feedback=0.5"], HELD)],
        "Delay 2":      [("on", ["dly2_mix=0.6", "dly2_time_l=0.3", "dly2_time_r=0.4", "dly2_feedback=0.5"], HELD)],
        "Ensemble":     [("on", ["ens_mix=0.8"], HELD)],
        "Patina":       [("on", ["patina=0.8"], HELD)],
        "Feedback":     [("on", ["fb_amount=0.6"], HELD)],
        "Foundation":   [("on", ["sub_level=0.7"], HELD)],
        "Body":         [("on", ["body_level=0.8"], HELD)],
        "Blur":         [("on", ["blur_mix=0.8"], HELD)],
        "Near Reverb":  [("on", ["near_mix=0.8"], HELD)],
        "Far Reverb":   [("on", ["far_level=0.9", "depth=0.7"], HELD)],
        "Filter":       [("on", ["filter_on=on", "cutoff=1200", "resonance=0.5"], HELD)],
        "Air":          [("on", ["air=0.5"], HELD)],
        "Strands":      [("on", ["strands=5", "detune=15"], HELD)],
        "Space":        [("on", ["depth=0.7", "far_level=0.8", "near_mix=0.5"], HELD)],
        "Envelope":     [("held", [], HELD)],
        "Master":       [("on", [], HELD)],
        "Macros":       [("routed", ["filter_on=on", "cutoff=1200", "brightness=0.5"], HELD)],
        "Tuning":       [("on", ["purity=0.6", "purity_drift=0.4"], HELD)],
        "Vector":       [("on", ["vec_amount=0.9", "src2_type=Harmonic", "src2_level=0.7",
                                 "src3_type=Wavetable", "src3_level=0.7"], HELD)],
        "Coherence":    [("on", ["coh_amount=0.9", "coh_rate=1.5"], HELD)],
        # The conductors get no held note: a held note IS the root, and every control that moves the
        # root correctly does nothing while one is down.
        "Cluster Brain": [("free", ["brain_on=on", "brain_rate=1.5", "brain_density=7"], FREE)],
        "Brain 2":      [("free", ["brain_on=on", "brain2_on=on", "brain2_rate=2", "brain2_density=4"], FREE)],
        "Autoplay":     [("chords", ["brain_on=on", "auto_mode=Chords", "auto_rate=1.5", "brain_density=6"], FREE)],
    }
    for n in (1, 2, 3, 4):
        c[f"Source {n}"] = source_contexts(n)
        c[f"Src Env {n}"] = [("own", [f"src{n}_type=Harmonic" if n > 1 else "partials=16",
                                      (f"src{n}_level=0.9" if n > 1 else "osc_level=0.9"),
                                      f"src{n}_env=Own"], HELD)]
    for i in range(1, 9):
        c[f"LFO {i}"] = [("fast", [f"lfo{i}_rate=2", f"lfo{i}_depth=1", "filter_on=on", "cutoff=1200"], HELD)]
    for i in range(1, 7):
        c[f"Env {i}"] = [("shaped", [f"env{i}_time=3", f"env{i}_depth=1", "filter_on=on", "cutoff=1200"], HELD)]
    return c


# The route that lets a modulator be heard at all, and the envelope shapes behind it.
def mod_for(section):
    if section.startswith("LFO "):
        return f"lfo{section[4:]}>cutoff:0.9"
    if section.startswith("Env "):
        return f"env{section[4:]}>cutoff:0.9"
    if section == "Macros":
        return ";".join(f"macro_{c}>cutoff:0.9" for c in "abcdefgh")
    if section == "Coherence":
        return "kura1>cutoff:0.9;kura2>brightness:0.9;kura3>detune:0.9;kura4>far_level:0.9"
    return ""


def extra_args(section):
    """--env / --src-env shapes, so a mode or a time has a curve to act on."""
    if section.startswith("Env "):
        return ["--env", section[4:], ENV_SHAPE]
    if section.startswith("Src Env "):
        return ["--src-env", section[8:], ENV_SHAPE] + _material()
    if section.startswith("Source ") or section in ("Vector", "Strands", "Air", "Filter", "Z-Plane"):
        return _material()
    return []


def _material():
    out = []
    if os.path.exists(CLIP):
        out += ["--texture", CLIP]
    if os.path.exists(TABLE):
        out += ["--wavetable", TABLE]
    return out


# What a single control needs behind it, beyond its section's context.
NEEDS = {
    "src1_rise": ["src1_delay=2"],
    "src1_root": ["src1_type=Harmonic", "src1_table=User"],
    "src1_grains": ["src1_type=Texture", "src1_density=200"],
    "src1_density_sync": ["src1_type=Texture", "src1_density=1"],
    "src1_noise_q": ["src1_type=Noise", "src1_noise=Band"],
    "src1_uni_detune": ["src1_unison=4"],
    "src1_uni_width": ["src1_unison=4", "src1_uni_detune=25"],
    "src1_interp": ["src1_type=Texture", "src1_follow=Note"],
    "src2_rise": ["src2_delay=2"],
    "src2_root": ["src2_type=Harmonic", "src2_table=User"],
    "src2_grains": ["src2_type=Texture", "src2_density=200"],
    "src2_density_sync": ["src2_type=Texture", "src2_density=1"],
    "src2_noise_q": ["src2_type=Noise", "src2_noise=Band"],
    "src2_uni_detune": ["src2_unison=4"],
    "src2_uni_width": ["src2_unison=4", "src2_uni_detune=25"],
    "src2_interp": ["src2_type=Texture", "src2_follow=Note"],
    "src3_rise": ["src3_delay=2"],
    "src3_root": ["src3_type=Harmonic", "src3_table=User"],
    "src3_grains": ["src3_type=Texture", "src3_density=200"],
    "src3_density_sync": ["src3_type=Texture", "src3_density=1"],
    "src3_noise_q": ["src3_type=Noise", "src3_noise=Band"],
    "src3_uni_detune": ["src3_unison=4"],
    "src3_uni_width": ["src3_unison=4", "src3_uni_detune=25"],
    "src3_interp": ["src3_type=Texture", "src3_follow=Note"],
    "src4_rise": ["src4_delay=2"],
    "src4_root": ["src4_type=Harmonic", "src4_table=User"],
    "src4_grains": ["src4_type=Texture", "src4_density=200"],
    "src4_density_sync": ["src4_type=Texture", "src4_density=1"],
    "src4_noise_q": ["src4_type=Noise", "src4_noise=Band"],
    "src4_uni_detune": ["src4_unison=4"],
    "src4_uni_width": ["src4_unison=4", "src4_uni_detune=25"],
    "src4_interp": ["src4_type=Texture", "src4_follow=Note"],
    "osc_rise": ["src1_delay=2"],
    "cloud_tone": ["cloud_feedback=0.6"], "cloud_shift": ["cloud_feedback=0.6"],
    "cloud_res_mode": ["cloud_resonance=0.8"], "cloud_res_notes": ["cloud_resonance=0.8"],
    "cloud_res_decay": ["cloud_resonance=0.8"], "cloud_transpose": ["cloud_pitch=0"],
    "cloud_sync": ["cloud_density=1"],
    "mem_recall": ["mem_seek=0.8"], "mem_seek": ["mem_recall=0.8"], "mem_grain": ["mem_recall=0.8"],
    "fb_tape": ["fb_amount=0.6"], "fb_tone": ["fb_amount=0.6"], "fb_pm": ["fb_amount=0.6"],
    "z_morph": ["z_shape_b=40"], "z_shape_b": ["z_morph=0.6"], "z_keytrack": ["z_route=1"],
    "freeze": ["shimmer=0.9", "drift=10"],
    "purity_rate": ["purity_drift=0.6"], "purity_guard": ["purity_drift=0.6"],
    "far_unmask_spread": ["far_unmask=0.8"], "far_comod": ["far_envelop=0.6"],
    "brain_silence_len": ["brain_silence=1"], "brain_surprise": ["brain_homeostat=1"],
    "brain_homeostat": ["brain_surprise=1"], "brain_loop": ["brain_dejavu=0.9"],
    "brain_leading": ["brain_key=0.9"], "brain_release_gap": ["brain_rate=0.8"],
    "brain_overlap": ["brain_rate=0.8", "brain_hold_min=3", "brain_hold_max=6"],
    "brain_retrigger": ["brain_rate=0.8", "brain_hold_min=2", "brain_hold_max=4"],
    "brain_silence": ["brain_wander=1", "auto_root_move=1"],
    "brain_root_steps": ["brain_wander=1"], "brain_root_down": ["brain_wander=1"],
    "brain_home": ["brain_wander=1"], "brain_degree_swap": ["brain_wander=1"],
    "auto_lead": ["auto_root_move=0.5"], "auto_root_move": ["auto_tension=0.5"], "brain_breath_period": ["brain_rate_breath=1"],
    "brain_home_time": ["brain_home=1", "brain_wander=1"],
    "sub_pulse": ["sub_level=0.7"], "sub_glide": ["sub_level=0.7", "brain_on=on", "brain_rate=1.5"],
    "cosmos_shimmer_pitch": ["cosmos_shimmer=0.8"], "cosmos_shimmer_mode": ["cosmos_shimmer=0.8"],
    "cosmos_res_fb": ["cosmos_resonator=0.8"], "cosmos_res_pitch": ["cosmos_resonator=0.8"],
    "cosmos_smear": ["cosmos_nebula=0.8"], "cosmos_shift_drift": ["cosmos_shift=200"],
    "cosmos_vowel_rate": ["cosmos_vowel=0.8"], "cosmos_swell": ["brain_on=on", "brain_cascade=1", "brain_rate=1.5"],
    "body_material": ["body_level=0.8"], "body_decay": ["body_level=0.8"],
    "body_tone": ["body_level=0.8"], "body_spread": ["body_level=0.8"], "body_pitch": ["body_level=0.8"],
    "blur_smear": ["blur_mix=0.9"],
}

# Not a fault when these show nothing: they need a time scale, a host, a gesture or a second engine
# that a twelve-second offline render does not have. Named so the report says so out loud rather
# than leaving a reader to wonder.
UNTESTABLE = {
    "Map": "the map blend needs a measured library and a cursor",
    "Route": "a route walks over minutes",
    "Morph": "morphing needs a second preset loaded",
    "Clock": "nothing here is synced to the clock unless a sync control is set",
    "Expression": "pressure, slide and bend come from a controller",
}
SLOW = {"bloom_time", "arc_period", "arc_sync", "arc_harmony", "chaos_period", "rate_wander",
        "mem_age", "mem_renew", "brain_home_time", "brain_memory", "brain_pivot",
        "brain_density_slew", "brain_hold_min", "brain_hold_max", "brain2_hold_min",
        "brain2_hold_max", "purity_rate", "release", "sub_glide", "portamento", "porta_gravity"}


def table():
    out = subprocess.run([RENDER, "--list"], capture_output=True, text=True, encoding="utf-8").stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"^(\S+)\s+(\S+(?: \S+)*?)\s+\[(-?[\d.e+-]+) \.\. (-?[\d.e+-]+)\] default (-?[\d.e+-]+)", line)
        if m:
            rows.append({"key": m.group(1), "section": m.group(2).strip(),
                         "lo": float(m.group(3)), "hi": float(m.group(4)), "def": float(m.group(5))})
    return rows


def probe_values(p):
    """The values worth asking. A float is asked at whichever end is further from where it sits; a
    short choice is asked at every one of its settings, because one of them may be the only one
    that does anything and another may be a fallback that does nothing. Probing lfo_shape at its
    far end landed on Table, which without a wavetable loaded IS a sine, and the control was
    reported dead while five of its seven shapes worked perfectly."""
    lo, hi, d = p["lo"], p["hi"], p["def"]
    span = hi - lo
    if span <= 8.0 and abs(span - round(span)) < 1e-6 and abs(lo - round(lo)) < 1e-6:
        return [v for v in (lo + i for i in range(int(span) + 1)) if abs(v - d) > 1e-9]
    return [hi if (hi - d) >= (d - lo) else lo]


def render(sets, seconds, notes, mod, extra):
    cmd = [RENDER, "--preset", "Init", "--seconds", str(seconds), "--measure", "--hour", "9"]
    if notes:
        cmd += ["--notes", notes]
    if mod:
        cmd += ["--mod", mod]
    cmd += extra
    for s in sets:
        cmd += ["--set", s]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=600)
    except subprocess.TimeoutExpired:
        return None
    m = MEASURE.search(r.stdout or "")
    if not m:
        return None
    d = {}
    for tok in m.group(1).split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                d[k] = float(v)
            except ValueError:
                d[k] = v
    return d


def moved(m, b):
    """How far one render stands from another, in the terms the map itself is drawn in."""
    if m is None or b is None:
        return None
    return {
        "rms": m.get("rms", 0.0) - b.get("rms", 0.0),
        "centroid": m.get("centroid", 0.0) / max(b.get("centroid", 1.0), 1e-9) - 1.0,
        "flatness": m.get("flatness", 0.0) - b.get("flatness", 0.0),
        "flux": m.get("flux", 0.0) - b.get("flux", 0.0),
        "width": m.get("width", 0.0) - b.get("width", 0.0),
        "wet": m.get("wet", 0.0) - b.get("wet", 0.0),
        "rough": m.get("rough", 0.0) - b.get("rough", 0.0),
        "same": m.get("hash") == b.get("hash"),
    }


def audible(d):
    return (abs(d["rms"]) >= 0.05 or abs(d["centroid"]) >= 0.01 or abs(d["flux"]) >= 0.01
            or abs(d["width"]) >= 0.01 or abs(d["wet"]) >= 0.005 or abs(d["flatness"]) >= 1e-4
            or abs(d["rough"]) >= 5e-4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--jobs", type=int, default=14)
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--section", default="")
    ap.add_argument("--out", default="")
    a = ap.parse_args()

    CTX = contexts()
    rows = [p for p in table() if p["section"] not in UNTESTABLE]
    if a.section:
        rows = [p for p in rows if p["section"] == a.section]
    by_section = {}
    for p in rows:
        by_section.setdefault(p["section"], []).append(p)

    jobs = []
    for section, params in sorted(by_section.items()):
        for label, pre, notes in CTX.get(section, [("plain", [], HELD)]):
            switches = {s.split("=")[0] for s in pre}
            for p in params:
                if p["key"] in switches:
                    continue
                jobs.append((section, label, pre, notes, p))
    print(f"{len(jobs)} questions over {len(by_section)} sections, {a.jobs} renders in parallel", flush=True)

    def one(job):
        section, label, pre, notes, p = job
        mod, extra = mod_for(section), extra_args(section)
        need = NEEDS.get(p["key"], [])
        secs = SECONDS.get(section, a.seconds)
        b = render(pre + need, secs, notes, mod, extra)
        out = []
        for v in probe_values(p):
            m = render(pre + need + [f"{p['key']}={v:g}"], secs, notes, mod, extra)
            out.append((v, moved(m, b)))
        return section, label, p, out

    best = {}
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for section, label, p, out in ex.map(one, jobs):
            done += 1
            if done % 200 == 0:
                print(f"  {done}/{len(jobs)}", flush=True)
            key = (section, p["key"])
            # Keep the context AND the value at which the control did the most: it only has to work
            # somewhere, at some setting, to be a working control.
            for v, d in out:
                score = -1.0 if d is None else (2.0 if (not d["same"] and audible(d)) else (1.0 if not d["same"] else 0.0))
                cur = best.get(key)
                if cur is None or score > cur[0]:
                    best[key] = (score, label, p["def"], v, d)

    dead, tiny, ok, failed = [], [], [], []
    for (section, key), (score, label, dflt, v, d) in sorted(best.items()):
        row = [section, key, dflt, v, label,
               None if d is None else round(d["rms"], 3),
               None if d is None else round(100 * d["centroid"], 2),
               None if d is None else round(d["wet"], 4)]
        (failed if score < 0 else dead if score == 0 else tiny if score == 1 else ok).append(row)

    def show(title, rows_):
        print(f"\n{title}: {len(rows_)}")
        for section, key, dflt, v, label, drms, dcen, dwet in rows_:
            note = "   [slow by nature]" if key in SLOW else ""
            print(f"  {section:14s} {key:22s} {dflt:>9.4g} -> {v:<9.4g} [{label}]"
                  + (f"  rms {drms:+6.2f} dB  centroid {dcen:+6.1f} %  wet {dwet:+.3f}" if drms is not None else "")
                  + note)

    show("CHANGED NOTHING AT ALL, in any context (same audio, bit for bit)", dead)
    show("CHANGED THE AUDIO BUT MOVED NO DESCRIPTOR", tiny)
    print(f"\nworking: {len(ok)}")
    if failed:
        show("DID NOT RENDER", failed)
    print("\nnot asked, and why:")
    for s, why in sorted(UNTESTABLE.items()):
        print(f"  {s:14s} {why}")
    if a.out:
        json.dump({"dead": dead, "tiny": tiny, "ok": ok, "failed": failed},
                  open(a.out, "w", encoding="utf-8"), indent=1)
        print(f"\nwrote {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
