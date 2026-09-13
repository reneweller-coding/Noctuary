"""Noctuary -- render and measure every preset in the three section layers.

The Cosmos, Z-plane and Strike banks are generated (Tools/make_layer_presets.py and
Tools/make_zplane_bank.py), and a generated bank is exactly the kind of thing that can gain a
silent entry, a screaming one, or four hundred entries that all sound the same without anybody
noticing. So every one of them is rendered on top of the same carrier sound and measured:

    python Tools/check_layer_presets.py                 # all three banks
    python Tools/check_layer_presets.py --bank z        # one of cosmos / z / strike

What counts as a failure, and why:

  SILENT     more than 25 dB quieter than the carrier alone -- the layer swallowed the sound
  LOUD       peak above 0.99, or more than 9 dB above the carrier -- it will clip in a mix
  DEAD       every descriptor within a hair of the carrier's -- the preset does nothing at all
  TWIN       identical to another preset in the same bank: the same in every descriptor to three
             decimals AND the same audio hash. The descriptors alone are not enough -- a bank of
             narrow resonators fed with noise measures almost the same whatever its modes are,
             and five modal presets were flagged as twins whose rendered audio differed. The hash
             settles it: same hash, same sound; different hash, different sound.

The carrier is one fixed sound preset with a held chord, rendered once, so every measurement is
against the same thing. Twenty seconds each: long enough for a slow resonator to build up, which
is exactly where a feedback preset goes wrong if it is going to.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
CARRIER = "Consonant Expanse"   # the 2.0 built-in after the night of 13.09.; "Consonant Field" went with the ones before, "Consonant Hollow" with the old
SECONDS = 20

FIELDS = ("rms", "centroid", "flatness", "flux", "bass", "width", "peak")


def measure(settings, notes=None, near_preset=None):
    """One render with the carrier plus these key=value settings; the descriptor line as a dict.

    `notes` plays a held chord, and `extra` can silence the drone. The Strike bank needs both.
    The pluck fires on note-on, and with Fires = Keys it fires on nothing else, so without notes
    thirty of its presets measured exactly like the carrier -- true, and useless. And even with
    notes, a pluck of a few hundred milliseconds under twenty seconds of drone moves none of the
    descriptors: thirty-four of them then measured identically to each other, which was the
    measurement failing rather than the presets. So the Strike bank is measured on its own, with
    every source turned off, where the pluck is the whole signal.

    `near_preset` loads a near bank preset by name through the render tool, which is the only
    way its clip comes along: the Archive family names a recording of the library, and a preset
    applied as key=value settings would play Source 4's clip instead (or nothing). The settings
    given here are applied after it, so they override what the preset set.
    """
    cmd = [RENDER, "--preset", CARRIER, "--seconds", str(SECONDS), "--measure"]
    if notes:
        cmd += ["--notes", notes]
    if near_preset:
        cmd += ["--near-preset", near_preset]
    for kv in settings:
        cmd += ["--set", kv]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    # The descriptor line begins with "measure:"; it used to be the last line, until the render
    # tool learned to print the timbre vector after it, and then every carrier "did not render".
    lines = [l for l in (r.stdout + r.stderr).splitlines() if l.startswith("measure:")]
    if not lines:
        return None
    line = lines
    out = {}
    for k, v in re.findall(r"(\w+)=(-?[\d.]+)", line[-1]):
        out[k] = float(v)
    m = re.search(r"hash=([0-9a-f]+)", line[-1])
    if m:
        out["hash"] = m.group(1)
    return out if "rms" in out else None


BANK_FILES = {"cosmos": "CosmosPresets.inc", "z": "ZPlanePresets.inc", "strike": "StrikePresets.inc", "near": "NearPresets.inc"}


# A row is name and settings, and in the near bank possibly a third field: the clip the preset
# plays, relative to the library's root. A regex for two fields alone quietly skipped every
# preset that had one, and "41 presets, 0 flagged" was said of a bank of 69.
ROW = r'^\s*\{ "([^"]*)", "([^"]*)"(?:, "([^"]*)")? \},'


def settings_of(bank, index):
    """The preset's settings string, straight out of the generated .inc so nothing is retyped."""
    with open(os.path.join(ROOT, "Core", "src", BANK_FILES[bank]), encoding="utf-8") as f:
        text = f.read()
    rows = re.findall(ROW, text, re.M)
    return rows[index][:2] if index < len(rows) else None


def bank_rows(bank):
    with open(os.path.join(ROOT, "Core", "src", BANK_FILES[bank]), encoding="utf-8") as f:
        return re.findall(ROW, f.read(), re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bank", choices=["cosmos", "z", "strike", "near", "all"], default="all")
    ap.add_argument("--limit", type=int, default=0, help="only the first N of each bank (a quick look)")
    a = ap.parse_args()
    if not os.path.isfile(RENDER):
        sys.exit("build the render tool first: " + RENDER)

    # Two carriers: one the conductor plays on its own, one with a held chord for the banks whose
    # presets only do something when a key goes down.
    NOTES = "45,52,57,64"
    # The Strike bank is measured with the drone silenced, so the pluck is what is being measured.
    BARE = ["osc_level=0", "src2_level=0", "src3_level=0", "sub_level=0", "body_level=0",
            "cosmos_send=0", "brain_rate=2", "brain_density=4"]
    # And fired by the conductor for the whole render, whatever the preset says its trigger is.
    # Not because that is how the preset is meant to be played, but because --measure analyses the
    # second half of the render only: a pluck that fires once at t=0 is long gone by then, which
    # is why thirty-six of these measured as identical twins. What is being measured here is the
    # timbre of the pluck, so it has to be sounding while the measurement is taken.
    FIRE = ["strike_who=Keys + Brain"]
    # The Z-plane bank is measured on white noise and nothing else. Two reasons, both learned the
    # hard way. A filter can only be heard where the source has energy, and a dark ambient carrier
    # has almost none above 5 kHz -- so Ice (5-9 kHz) measured as "the carrier, unchanged". And
    # the sub, the body and the effects do not go through the voice filter at all, so even at full
    # wet they keep the drone in the measurement: with the whole instrument playing, four extreme
    # shapes came out identical to three decimals. Noise in, filter out, nothing else in the way.
    BRIGHT = ["osc_level=0", "src2_level=0", "sub_level=0", "body_level=0", "cosmos_send=0",
              "dly_mix=0", "dly2_mix=0", "far_level=0", "room_level=0", "cloud_send=0",
              "src3_type=Noise", "src3_noise=White", "src3_level=0.8", "src3_pos=0.5"]
    # The near bank (13.09.2026) is measured with the events hurried: a foreground that comes
    # every few minutes is not in a twenty-second render, so the rate is set to eight seconds --
    # the scheduler never waits longer than its rate for the first event -- and what is measured
    # is the carrier with one event sounding in its second half. A preset whose event does not
    # sound then measures as the carrier, which is the DEAD line below.
    NEAR = ["fore_rate=8"]
    base = measure([])
    base_notes = measure(BARE + FIRE, NOTES)
    base_bright = measure(BRIGHT)
    if base is None:
        sys.exit("the carrier itself did not render")
    print("carrier %s: rms %.2f dB, peak %.3f" % (CARRIER, base["rms"], base["peak"]))

    banks = ["cosmos", "z", "strike", "near"] if a.bank == "all" else [a.bank]
    bad = 0
    for bank in banks:
        rows = bank_rows(bank)
        if a.limit:
            rows = rows[:a.limit]
        print("%s: %d presets" % (bank, len(rows)))
        seen, dead, twins = {}, 0, 0
        for i, (name, settings, clip) in enumerate(rows):
            kvs = [kv for kv in settings.split(";") if kv]
            notes = NOTES if bank == "strike" else None
            pre = BARE + FIRE if notes else (BRIGHT if bank == "z" else [])
            ref = base_notes if notes else (base_bright if bank == "z" else base)
            if bank == "near":
                # By name, through the render tool: that loads the preset's clip as well, and the
                # settings that hurry the events are applied on top of it.
                m = measure(NEAR, None, near_preset=name)
            else:
                m = measure(pre + kvs, notes)
            if m is None:
                print("  FAILED TO RENDER  %s" % name)
                bad += 1
                continue
            why = []
            # The bare carrier is near silence, so "quieter than it" is not the test for Strike:
            # there, a preset is silent if it is under -70 dBFS in absolute terms.
            if (m["rms"] < -70.0) if notes else (m["rms"] < ref["rms"] - 25.0):
                why.append("SILENT (%.1f dB)" % m["rms"])
            if m["peak"] > 0.99 or (not notes and m["rms"] > ref["rms"] + 9.0):
                why.append("LOUD (peak %.2f, %.1f dB)" % (m["peak"], m["rms"]))
            key = tuple(round(m.get(f, 0.0), 3) for f in FIELDS) + (m.get("hash", ""),)
            if i > 0 and key[:len(FIELDS)] == tuple(round(ref.get(f, 0.0), 3) for f in FIELDS):
                why.append("DEAD (identical to the carrier)")
                dead += 1
            if key in seen:
                why.append("TWIN of %s" % seen[key])
                twins += 1
            seen.setdefault(key, name)
            if why:
                print("  %-26s %s" % (name[:26], ", ".join(why)))
                bad += 1
            elif (i + 1) % 32 == 0:
                print("  %d/%d ok" % (i + 1, len(rows)), flush=True)
        print("  %s: %d dead, %d twins" % (bank, dead, twins))
    print("%d presets flagged" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
