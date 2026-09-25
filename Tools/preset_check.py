"""Automatic sound test: render every preset and flag the ones that are too loud, clip, click,
carry DC or go non-finite. The measurement half of "measure, don't only listen", as a check
that can run before a commit or after a preset round.

    python Tools/preset_check.py [--seconds 10] [--json report.json] [--only "name"]

Limits (a preset fails if any is exceeded):
    rms      > -12 dBFS     (drones live around -20 .. -28)
    peak     > 0.98         (the soft clipper is working hard)
    jump     > 0.30         (sample-to-sample: a click)
    dc       > 0.02         (mean of the mixed signal)
    silent   rms < -60 dBFS after the chord and brain had 10 s
    mono     > 6 dB lost when summed to mono (the sides cancel the centre)
    non-finite samples
Exit code 1 when anything fails.
"""
import argparse
import concurrent.futures
import json
import os
import random
import re
import subprocess
import sys


HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
sys.path.insert(0, HERE)

# mono_loss 3 dB, crest 12 dB and true peak -1 dBTP are the production guide's gates (25.09.2026);
# the crest is the renderer's, true peak over short-term loudness, and is read over the render's
# settled half like everything else here.
LIMITS = {"rms_db": -12.0, "peak": 0.98, "jump": 0.30, "dc": 0.02, "silent_db": -60.0, "mono_loss": 3.0,
          "crest_min": 12.0, "truepeak": -1.0}


# Renders run below normal priority, so a long batch does not make the machine unusable.
LOW_PRIORITY = {"creationflags": subprocess.BELOW_NORMAL_PRIORITY_CLASS} if os.name == "nt" else {}


MEASURE = re.compile(r"^measure: (.*)$", re.M)
LOUDNESS = re.compile(r"^loudness: (.*)$", re.M)
NONFINITE = re.compile(r"non-finite (\d+)")


def check_preset(name, seconds, packs=None):
    """The synth measures its own render (--measure) and prints one line: no temporary WAV is
    written and read back, which used to cost the machine 1.8 MB of file cache per preset."""
    cmd = [RENDER]
    if packs:
        cmd += ["--packs", packs]
    cmd += ["--preset", name, "--seconds", str(seconds), "--notes", "45,52,59", "--set", "brain_rate=6", "--measure", "--loudness"]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace", **LOW_PRIORITY)
    m = MEASURE.search(res.stdout or "")
    if res.returncode != 0 and not m:
        return {"name": name, "error": (res.stderr or res.stdout).strip()[-200:], "fail": ["render"]}
    if not m:
        return {"name": name, "error": "no measurement line", "fail": ["render"]}
    d = {}
    for tok in m.group(1).split():
        if "=" in tok:
            k, v = tok.split("=", 1)
            try:
                d[k] = float(v)
            except ValueError:
                pass
    lm = LOUDNESS.search(res.stdout or "")
    if lm:
        for tok in lm.group(1).split():
            if "=" in tok:
                k, v = tok.split("=", 1)
                try:
                    d[k] = float(v)
                except ValueError:
                    pass
    nf = NONFINITE.search(res.stdout or "")
    nonfinite = int(nf.group(1)) if nf else 0
    rms_db = d.get("rms", -120.0)
    peak, jump, dc, mono_loss = d.get("peak", 0.0), d.get("jump", 0.0), d.get("dc", 0.0), d.get("monoloss", 0.0)
    fails = []
    if rms_db > LIMITS["rms_db"]: fails.append(f"loud {rms_db:.1f} dBFS")
    if peak > LIMITS["peak"]: fails.append(f"peak {peak:.2f}")
    if jump > LIMITS["jump"]: fails.append(f"click {jump:.2f}")
    if dc > LIMITS["dc"]: fails.append(f"dc {dc:.3f}")
    if rms_db < LIMITS["silent_db"]: fails.append(f"silent {rms_db:.1f} dBFS")
    if nonfinite: fails.append(f"non-finite {nonfinite}")
    if mono_loss > LIMITS["mono_loss"]: fails.append(f"mono -{mono_loss:.1f} dB")
    crest, truepeak = d.get("crest"), d.get("truepeak")
    if crest is not None and rms_db > LIMITS["silent_db"] and crest < LIMITS["crest_min"]: fails.append(f"crest {crest:.1f} dB")
    if truepeak is not None and truepeak > LIMITS["truepeak"]: fails.append(f"true peak {truepeak:.1f} dBTP")
    return {"name": name, "rms_db": rms_db, "peak": peak, "jump": jump, "dc": dc,
            "mono_loss": mono_loss, "crest": crest, "truepeak": truepeak, "lufs_i": d.get("lufs_i"),
            "nonfinite": nonfinite, "fail": fails}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=float, default=10.0)
    ap.add_argument("--json", default=None)
    ap.add_argument("--only", default=None, help="substring of preset names to check")
    ap.add_argument("--packs", default=None, help="also load the preset packs in this directory")
    ap.add_argument("--sample", type=int, default=0, help="check a random N of the presets instead of all")
    ap.add_argument("--seed", type=int, default=1, help="which random sample")
    ap.add_argument("--jobs", type=int, default=1,
                    help="renders in parallel; three is plenty, they run below normal priority")
    a = ap.parse_args()
    listcmd = [RENDER] + (["--packs", a.packs] if a.packs else []) + ["--list-presets"]
    names = [n for n in subprocess.run(listcmd, capture_output=True, text=True, encoding="utf-8").stdout.splitlines()
             if n.strip() and not n.startswith("packs: ")]
    if a.only:
        names = [n for n in names if a.only.lower() in n.lower()]
    if a.sample and a.sample < len(names):
        names = sorted(random.Random(a.seed).sample(names, a.sample))
    results = []
    failed = 0
    if a.jobs > 1:
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            done = list(ex.map(lambda n: check_preset(n, a.seconds, a.packs), names))
    else:
        done = (check_preset(n, a.seconds, a.packs) for n in names)
    for i, r in enumerate(done):
        name = r["name"]
        results.append(r)
        status = "FAIL " + ", ".join(r["fail"]) if r["fail"] else "ok"
        if r["fail"]: failed += 1
        if "error" in r:
            print(f"{i:4d} {name:28s} {status}: {r['error']}")
        elif r["fail"] or a.jobs == 1:
            print(f"{i:4d} {name:28s} rms {r['rms_db']:6.1f}  peak {r['peak']:.2f}  jump {r['jump']:.3f}  dc {r['dc']:.4f}  mono -{r['mono_loss']:.1f}  {status}")
    print(f"\n{len(names) - failed} of {len(names)} presets pass" + (f", {failed} FAIL" if failed else ""))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"limits": LIMITS, "results": results}, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
