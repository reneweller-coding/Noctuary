"""The whole preset library, from the clips on disk to the presets compiled into the instrument.

Eight steps, each of which can be run on its own; this driver exists so the order and the arguments
are written down once instead of living in a session's scrollback. It never generates audio -- the
clip generators (make_textures.py, make_field_recordings.py, make_wavetables.py, make_impulses.py,
ImpulseGen/roomgen.py) are run by hand because they take hours of GPU time and their output is
committed.

    1  presets    Tools/library/make_library.py      the 56 packs and the built-ins' staging packs
    2  verify     Tools/library/verify_packs.py      every key and value actually exists
    3  balance    Tools/library/rebalance_voice.py   the voice above its own noise bed, measured
    4  measure    Tools/library/measure_packs.py     60 s per preset: descriptors, and the loudness window
    5  builtins   Tools/library/write_builtins.py    the staging packs into Core/src/Presets.cpp,
                  cmake + map_all.py --dry-run       rebuilt, then measured from the binary
    6  clap       Tools/library/clap_embed.py        what a model says the excerpts sound like
    7  map        Tools/library/map_all.py           one layout, the groups, the phrases, the tables
    8  build      cmake --build ... && the selftest

  python Tools/library/rebuild_all.py --work <dir> [--from measure] [--jobs 12] [--dry-run]

Both halves of the library go through the same mill: the built-ins are generated as packs into
<work>/builtin_packs and balanced and gain-matched there, because every tool that measures a preset
measures packs. Only then are they compiled in -- and measured once more from the binary, at the
same sixty seconds as the packs, so one ranking covers the whole library.

It refuses to start while the GPU or another of these tools is busy: every step is minutes to
hours long, and two at once is how a workstation stops responding (--anyway overrides).

--work holds everything that is not committed: the measurement caches, the excerpts and the CLAP
file. Steps are skipped when their output is already there, so an interrupted run continues where
it stopped; --force redoes them anyway.

A run that starts at step 4 or later compiles the built-ins from <work>/builtin_packs as step 1 last
wrote them. Since 25.09.2026 Core/src/Presets.cpp has been changed in place (the production guide's
retrofit, the harmony review's ranges, the measured gains of round two), so a staging folder older
than that would undo all of it: re-measure the built-ins through builtin_pack.py instead (export, the
two passes of measure_packs.py, import), as Library/README.md describes.

A few presets again -- after an engine fix, a hand edit -- is NOT a --from balance run of this: the
balance's --resume would take the whole library (its fingerprint covers the line the measurement
rewrote) and the measurement's loudness window would lift every preset still under it once more.
That is remeasure_presets.py, by name, followed by clap_embed.py --resume and map_all.py.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
PACKS = os.path.join(ROOT, "Library", "Packs")
STEPS = ["presets", "verify", "balance", "measure", "builtins", "clap", "map", "build"]


def gpu_busy(limit_mb=4000):
    """How much of the card is already spoken for. A generative model loading beside a batch of
    renders is what froze the machine once: the driver starts paging VRAM into system memory under
    pressure and Windows stops answering. Cheap to ask, so it is asked before every long step."""
    try:
        out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                             capture_output=True, text=True, timeout=20).stdout
        used = max(int(v.strip()) for v in out.split() if v.strip().isdigit())
    except Exception:
        return 0
    return used if used > limit_mb else 0


def other_batches():
    """Another generator already at work, from this repo's own toolchain."""
    try:
        out = subprocess.run(["wmic", "process", "where", "name like '%python%'", "get", "commandline"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return []
    mine = os.path.basename(__file__)
    names = ("texturegen_worker", "make_textures", "make_field_recordings", "make_wavetables",
             "make_impulses", "clip_affinity", "clap_embed", "measure_packs", "map_all")
    return sorted({n for n in names if n in out and n != mine})


def run(cmd, dry, cwd=ROOT):
    print("\n$ " + " ".join(str(c) for c in cmd), flush=True)
    if dry:
        return 0
    t = time.time()
    r = subprocess.run(cmd, cwd=cwd)
    print(f"   [{time.time() - t:.0f} s, exit {r.returncode}]", flush=True)
    return r.returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True, help="folder for the caches, the excerpts and the CLAP file")
    ap.add_argument("--from", dest="start", default="presets", choices=STEPS)
    ap.add_argument("--to", dest="stop", default="build", choices=STEPS)
    ap.add_argument("--jobs", type=int, default=12,
                    help="renders at a time. Twelve of the machine's twenty-four logical cores: "
                         "each render is about 100 MB, so this is CPU, not memory. Keep it well "
                         "clear of a GPU job -- that combination is what froze the machine once")
    ap.add_argument("--anyway", action="store_true",
                    help="start even though the GPU or another generator is busy")
    ap.add_argument("--clusters", type=int, default=14)
    ap.add_argument("--clap-device", default="cpu", choices=("cpu", "cuda", "auto"),
                    help="where CLAP listens (see the note at the clap step)")
    ap.add_argument("--clap-python", default="", help="interpreter with torch and transformers "
                    "(default: Tools/TextureGen/.venv, the one the clip generator uses)")
    ap.add_argument("--force", action="store_true", help="redo steps whose output is already there")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    # Nothing here starts while something else is already using the machine. Every step below is
    # minutes to hours long, and two of them at once is how a workstation stops responding.
    if not a.dry_run and not a.anyway:
        busy = gpu_busy()
        others = other_batches()
        if busy or others:
            print(f"refusing to start: {busy} MB of the GPU is in use" if busy else "refusing to start:", end=" ")
            if others:
                print(f"{', '.join(others)} already running")
            else:
                print("")
            print("wait for it, or pass --anyway if you know it is small enough")
            return 1

    work = os.path.abspath(a.work)
    os.makedirs(work, exist_ok=True)
    stage = os.path.join(work, "builtin_packs")     # the built-ins as packs, until they are compiled in
    cache = os.path.join(work, "packs.json")
    scache = os.path.join(work, "staging.json")     # the built-ins measured as packs (for the gain)
    bcache = os.path.join(work, "builtins.json")    # and measured again from the binary (for the map)
    taps = os.path.join(work, "taps")
    clap = os.path.join(work, "clap.json")
    py = sys.executable
    lo = STEPS.index(a.start)
    hi = STEPS.index(a.stop)

    def want(step, output=None):
        i = STEPS.index(step)
        if i < lo or i > hi:
            print(f"-- {step}: not in range")
            return False
        if output and os.path.exists(output) and not a.force:
            print(f"-- {step}: {os.path.basename(output)} is already there (--force to redo)")
            return False
        return True

    cpy = a.clap_python or os.path.join(ROOT, "Tools", "TextureGen", ".venv", "Scripts", "python.exe")
    if want("presets"):
        if run([py, os.path.join(HERE, "make_library.py"), "--builtins", "--builtins-out", stage], a.dry_run):
            return 1
        # A new library invalidates every measurement of the old one: same names, other sounds.
        # The old caches are kept beside the new ones rather than deleted: comparing a run against
        # the one before it is how "did that change help?" gets an answer instead of an opinion.
        stamp = time.strftime("%Y%m%d-%H%M%S")
        for f in (cache, scache, bcache, clap):
            if os.path.exists(f) and not a.dry_run:
                os.replace(f, f + "." + stamp + ".bak")
            if os.path.exists(f + ".done") and not a.dry_run:
                os.remove(f + ".done")
    if want("verify"):
        for folder in (PACKS, stage):
            if run([py, os.path.join(HERE, "verify_packs.py"), "--packs", folder], a.dry_run):
                return 1
    # The balance has to come before the measurement and after the packs: it CHANGES levels, so
    # every descriptor and every loudness figure measured before it is about a library that no
    # longer exists. It is here rather than in the generator because it is a measurement of its own
    # -- two renders a preset -- and because a hand-written pack deserves the same treatment.
    if want("balance"):
        for folder in (PACKS, stage):
            if run([py, os.path.join(HERE, "rebalance_voice.py"), "--packs", folder, "--jobs", str(a.jobs),
                    "--cache", os.path.join(work, "balance-" + os.path.basename(folder) + ".json"), "--resume"], a.dry_run):
                return 1
            if run([py, os.path.join(HERE, "verify_packs.py"), "--packs", folder], a.dry_run):
                return 1

    if want("measure", cache if os.path.exists(cache + ".done") else None):
        # A minute, not the twelve seconds measure_packs defaults to: the evolution descriptors
        # measure how far a drone travels, and over twelve seconds every drone stands still. The
        # built-ins are measured at sixty by map_all as well, and both halves of the library have to
        # be measured under the same conditions or the ranks are meaningless.
        #
        # The gain correction is written here (no --no-write): a preset that lands outside the
        # loudness window has its master gain moved by exactly the distance, and the cache follows
        # the settings it wrote. The excerpts are for CLAP.
        if run([py, os.path.join(HERE, "measure_packs.py"), "--packs", PACKS, "--cache", cache,
                "--seconds", "60", "--taps", taps, "--jobs", str(a.jobs), "--resume"], a.dry_run):
            return 1
        # The built-ins, still as packs: the same window, the same sixty seconds. Their descriptors
        # are measured again from the binary in the next step; what is kept from here is the gain.
        if run([py, os.path.join(HERE, "measure_packs.py"), "--packs", stage, "--cache", scache,
                "--seconds", "60", "--jobs", str(a.jobs), "--resume"], a.dry_run):
            return 1
        if not a.dry_run:
            open(cache + ".done", "w").close()   # the caches are complete, not just partial
    if want("builtins", bcache):
        # Compiled in, rebuilt, and only then measured: from here on the built-ins are the
        # instrument's own, and the renderer has to know them before anything can ask it for one.
        if run([py, os.path.join(HERE, "write_builtins.py"), "--packs", stage], a.dry_run):
            return 1
        # The twelve routes name built-in presets, and the built-ins were just renamed: rewritten
        # from the new metadata, or the selftest at the end of this pipeline fails on every route
        # (it did, 13.09.2026: forty-four failures, all "route preset parses").
        if run([py, os.path.join(HERE, "make_routes.py")], a.dry_run):
            return 1
        if run(["cmake", "--build", "build", "--config", "Release"], a.dry_run):
            return 1
        # The layout is thrown away here; what is kept is the built-ins' measurements and their
        # excerpts, which have to exist before CLAP listens or they would have no line.
        if run([py, os.path.join(HERE, "map_all.py"), "--pack-cache", cache, "--builtin-cache", bcache,
                "--taps", taps, "--jobs", str(a.jobs), "--dry-run"], a.dry_run):
            return 1
    if want("clap", clap if os.path.exists(clap + ".done") else None):
        # CLAP needs torch, and the interpreter that runs the rest of this does not have it. The
        # clip generator's environment does, so that is the default rather than a second install.
        if not os.path.exists(cpy):
            print(f"-- clap: no interpreter at {cpy}; pass --clap-python")
            return 1
        # On the processor by default. This machine crashed three times in one afternoon, every
        # time during CUDA work and twice during exactly this step -- with the card at three
        # percent of its memory, so it is not a load that can be tuned away. Forty minutes on the
        # processor against ten on the card is a price worth paying for a step that cannot take
        # the machine with it. --clap-device cuda for anyone whose card behaves.
        if run([cpy, os.path.join(HERE, "clap_embed.py"), "--taps", taps, "--out", clap,
                "--resume", "--device", a.clap_device], a.dry_run):
            return 1
        if not a.dry_run:
            open(clap + ".done", "w").close()
    if want("map"):
        if run([py, os.path.join(HERE, "map_all.py"), "--pack-cache", cache, "--builtin-cache", bcache,
                "--taps", taps, "--clap", clap, "--clusters", str(a.clusters), "--jobs", str(a.jobs)],
               a.dry_run):
            return 1
        # And verify again afterwards: verify_packs runs before the map is laid out, so it has never
        # once seen a finished pack file. That is how it came to reject the sixteen metadata fields
        # map_all had been writing since 1.11 -- the check that was meant to catch a malformed
        # library had never read one.
        if run([py, os.path.join(HERE, "verify_packs.py"), "--packs", PACKS], a.dry_run):
            return 1
    if want("build"):
        if run(["cmake", "--build", "build", "--config", "Release"], a.dry_run):
            return 1
        env = dict(os.environ, AMBIENT_MUTE="1", AMBIENT_PACKS=PACKS)
        print("\n$ ambient_selftest", flush=True)
        if not a.dry_run:
            r = subprocess.run([os.path.join(ROOT, "build", "Tests", "Release", "ambient_selftest.exe")],
                               cwd=ROOT, env=env)
            if r.returncode:
                print("selftest failed")
                return 1
    print("\ndone")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
