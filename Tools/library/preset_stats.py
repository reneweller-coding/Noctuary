"""What is actually in the generated library: a census of the packs, read from the pack files.

Answers the questions that keep coming up about the library and were previously answered by
guessing -- which source types carry it, how many parameters it never touches, how much of each
shelf it reaches for, how it is modulated. Everything here comes from the packs themselves, so it
describes the library as it will ship and not as it was intended.

    python Tools/library/preset_stats.py [--packs Library/Packs]
"""
import argparse
import collections
import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
RENDER = os.path.join(ROOT, "bin", "msvc", "ambient_render.exe")


def read(packs):
    rows = []
    for f in sorted(glob.glob(os.path.join(packs, "*.ambientpack"))):
        pack = os.path.splitext(os.path.basename(f))[0]
        for line in open(f, encoding="utf-8"):
            if line.startswith("#") or line.startswith("pack ") or "|" not in line:
                continue
            p = line.rstrip("\n").split("|")
            s = {}
            for kv in p[1].split(";"):
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    s[k.strip()] = v.strip()
            rows.append({"pack": pack, "name": p[0].strip(), "set": s,
                         "texture": p[3].strip() if len(p) > 3 else "",
                         "table": p[4].strip() if len(p) > 4 else "",
                         "impulse": p[5].strip() if len(p) > 5 else "",
                         "mod": p[6].strip() if len(p) > 6 else "",
                         "envs": p[7].strip() if len(p) > 7 else ""})
    return rows


def all_params():
    """Every parameter the engine has, for the coverage figure."""
    try:
        out = subprocess.run([RENDER, "--list"], capture_output=True, text=True,
                             encoding="utf-8", timeout=120).stdout
    except Exception:
        return []
    # "master_gain        Master         [-40 .. 12] default -6 dB"
    return [m.group(1) for m in re.finditer(r"^(\w+)\s+\S.*\[", out, re.M)]


def bar(n, total, width=28):
    k = int(round(width * n / max(total, 1)))
    return "#" * k + "." * (width - k)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", default=os.path.join(ROOT, "Library", "Packs"))
    a = ap.parse_args()
    rows = read(a.packs)
    n = len(rows)
    if not n:
        print("no packs"); return 1
    packs = len({r["pack"] for r in rows})
    print(f"{n} presets in {packs} packs\n")

    # ---- what makes the sound ------------------------------------------------------------
    print("PRIMARY SOURCE")
    c = collections.Counter(r["set"].get("src1_type", "Additive") for r in rows)
    for k, v in c.most_common():
        print(f"  {k:12s} {v:5d}  {100.0*v/n:5.1f} %  {bar(v, n)}")
    slots = collections.Counter(1 + sum(1 for k in ("src2_type", "src3_type", "src4_type")
                                        if r["set"].get(k, "Off") != "Off") for r in rows)
    print("\nSOURCE SLOTS IN USE")
    for k in sorted(slots):
        print(f"  {k} slot{'s' if k > 1 else ' '}      {slots[k]:5d}  {100.0*slots[k]/n:5.1f} %  {bar(slots[k], n)}")

    # ---- what it is played by ------------------------------------------------------------
    print("\nHOW IT IS PLAYED")
    keys = sum(1 for r in rows if r["set"].get("brain_on") == "off")
    print(f"  from the keys {keys:5d}  {100.0*keys/n:5.1f} %")
    print(f"  conducted     {n-keys:5d}  {100.0*(n-keys)/n:5.1f} %")
    sc = collections.Counter(r["set"].get("scale", "Equal") for r in rows)
    print(f"  {len(sc)} different tunings, {n - sc['Equal']} presets ({100.0*(n-sc['Equal'])/n:.0f} %) not in equal temperament")
    for k, v in sc.most_common(6):
        print(f"     {k:28s} {v:5d}")

    # ---- the modules ---------------------------------------------------------------------
    print("\nMODULES SWITCHED ON")
    def num(r, k, d=0.0):
        try:
            return float(r["set"].get(k, d))
        except ValueError:
            return d
    tests = [
        ("Cosmos",        lambda r: num(r, "cosmos_send") > 0.05 or num(r, "cosmos_shimmer") > 0.05),
        ("feedback bus",  lambda r: num(r, "fb_bus") > 0.02 or num(r, "fb_fm") > 0.02),
        ("sub",           lambda r: num(r, "sub_level") > 0.05),
        ("grain cloud",   lambda r: num(r, "cloud_send") > 0.05),
        ("z-plane",       lambda r: r["set"].get("z_mode", "Off") not in ("Off", "")),
        ("strike",        lambda r: num(r, "strike_level") > 0.02),
        ("convolution room", lambda r: bool(r["impulse"])),
        ("a wavetable",   lambda r: bool(r["table"])),
        ("a sample",      lambda r: bool(r["texture"])),
        ("arc",           lambda r: num(r, "arc") > 0.05),
        ("bloom",         lambda r: num(r, "bloom") > 0.05),
    ]
    for label, fn in tests:
        v = sum(1 for r in rows if fn(r))
        print(f"  {label:18s} {v:5d}  {100.0*v/n:5.1f} %  {bar(v, n)}")

    # ---- the shelves ---------------------------------------------------------------------
    print("\nWHAT IT REACHES FOR")
    for label, field, folder in (("samples", "texture", None), ("wavetables", "table", "Wavetables"),
                                 ("impulses", "impulse", "Impulses")):
        used = collections.Counter()
        for r in rows:
            for ref in r[field].split(";"):
                ref = ref.strip()
                if ref:
                    used[os.path.basename(ref)] += 1
        if folder:
            # recursive: the wavetables sit on shelves (Harmonic, Classic, Ambient)
            shelf = len(glob.glob(os.path.join(ROOT, "Library", folder, "**", "*.wav"), recursive=True))
        else:
            shelf = (len(glob.glob(os.path.join(ROOT, "Library", "Textures", "*.wav")))
                     + len(glob.glob(os.path.join(ROOT, "Library", "FieldRecordings", "*.wav"))))
        med = sorted(used.values())[len(used) // 2] if used else 0
        print(f"  {label:11s} {len(used):5d} of {shelf:5d} on the shelf are used "
              f"({100.0*len(used)/max(shelf,1):.0f} %), median {med} times each")

    # ---- modulation ----------------------------------------------------------------------
    print("\nMODULATION")
    routes = [len([x for x in r["mod"].split(";") if x.strip()]) for r in rows]
    srcs, tgts = collections.Counter(), collections.Counter()
    for r in rows:
        for row in r["mod"].split(";"):
            if ">" in row and ":" in row:
                srcs[row.split(">")[0]] += 1
                tgts[row.split(">")[1].split(":")[0]] += 1
    print(f"  {sum(routes)} routes, {sum(routes)/n:.1f} per preset, "
          f"{sum(1 for x in routes if x == 0)} presets with none")
    print(f"  {len(srcs)} different sources, {len(tgts)} different targets")
    print("  the ten commonest targets:")
    for k, v in tgts.most_common(10):
        print(f"     {k:22s} {v:5d}")
    envs = sum(1 for r in rows if r["envs"])
    print(f"  {envs} presets carry drawn envelopes ({100.0*envs/n:.0f} %)")

    # ---- where the movement comes from ----------------------------------------------------
    # Sources fall into four kinds, and the mix is the whole character of the library: a clock,
    # a hand, the instrument listening to itself, or a system with its own dynamics.
    KIND = {}
    for k in ("lfo1", "lfo2", "lfo3", "lfo4", "lfo5", "lfo6", "lfo7", "lfo8"):
        KIND[k] = "clocks (LFOs)"
    for k in ("macro1", "macro2", "macro3", "macro4", "macro5", "macro6", "macro7", "macro8",
              "macro_a", "macro_b", "macro_c", "macro_d", "wheel", "pressure", "slide", "velocity",
              "keytrack", "note"):
        KIND[k] = "your hands"
    for k in ("env1", "env2", "env3", "env4", "amp", "beat", "coherence", "tuning", "surprise",
              "consonance", "keyfind", "voices", "level"):
        KIND[k] = "itself"
    for k in ("lorenz_x", "lorenz_y", "lorenz_z", "rossler_x", "rossler_y", "rossler_z",
              "lenia_a", "lenia_b", "lenia_c", "lenia", "cascade", "random", "field_a", "field_b",
              "field_c", "field_d", "tide", "wander"):
        KIND[k] = "its own weather"
    kinds = collections.Counter()
    for src, v in srcs.items():
        kinds[KIND.get(src, KIND.get(src.rstrip("0123456789"), "other"))] += v
    total = sum(kinds.values())
    print("\n  WHERE THE MOVEMENT COMES FROM")
    for k, v in kinds.most_common():
        print(f"     {k:18s} {v:6d}  {100.0*v/total:5.1f} %  {bar(v, total, 22)}")
    print("  every source, by how many routes it drives:")
    for k, v in srcs.most_common():
        print(f"     {k:14s} {v:6d}  {100.0*v/total:5.1f} %")

    # How many routes a preset carries, and how deep they go.
    h = collections.Counter(routes)
    print("\n  ROUTES PER PRESET")
    for k in sorted(h):
        print(f"     {k:2d} routes {h[k]:6d}  {100.0*h[k]/n:5.1f} %  {bar(h[k], n, 22)}")
    depths, negative, unipolar, gated = [], 0, 0, 0
    for r in rows:
        for row in r["mod"].split(";"):
            if ">" not in row or ":" not in row:
                continue
            parts = row.split(":")
            try:
                d = float(parts[1])
            except (ValueError, IndexError):
                continue
            depths.append(abs(d))
            negative += d < 0
            unipolar += any(x == "u" for x in parts[2:])
            gated += any(x.startswith("macro") for x in parts[2:])
    if depths:
        import statistics
        print(f"\n  DEPTHS\n     median {statistics.median(depths):.3f} of the target's range, "
              f"{100.0*negative/len(depths):.0f} % pull downwards, "
              f"{100.0*unipolar/len(depths):.0f} % one-sided, {gated} gated by a macro")

    # The clocks themselves: how slow is slow.
    rates = []
    for r in rows:
        for i in range(1, 9):
            v = r["set"].get(f"lfo{i}_rate")
            if v:
                try:
                    hz = float(v)
                except ValueError:
                    continue
                if hz > 0:
                    rates.append(1.0 / hz)
    if rates:
        rates.sort()
        q = lambda f: rates[int(f * (len(rates) - 1))]
        print(f"  LFO PERIODS\n     {len(rates)} LFOs set; 10 % faster than {q(0.1):.0f} s, "
              f"median {q(0.5):.0f} s, 10 % slower than {q(0.9):.0f} s, longest {rates[-1]:.0f} s")
    shapes = collections.Counter(r["set"].get(f"lfo{i}_shape") for r in rows for i in range(1, 9)
                                 if r["set"].get(f"lfo{i}_shape"))
    if shapes:
        print("     shapes: " + ", ".join(f"{k} {v}" for k, v in shapes.most_common()))

    # Targets by the section of the instrument they sit in.
    sect = {}
    try:
        out = subprocess.run([RENDER, "--list"], capture_output=True, text=True,
                             encoding="utf-8", timeout=120).stdout
        for m in re.finditer(r"^(\w+)\s{2,}(.+?)\s{2,}\[", out, re.M):
            sect[m.group(1)] = m.group(2).strip()
    except Exception:
        pass
    if sect:
        bysec = collections.Counter()
        for t, v in tgts.items():
            bysec[sect.get(t, "?")] += v
        print("\n  WHAT IS MODULATED, BY SECTION")
        for k, v in bysec.most_common(14):
            print(f"     {k:20s} {v:6d}  {100.0*v/sum(bysec.values()):5.1f} %  {bar(v, sum(bysec.values()), 22)}")

    # ---- parameter coverage ---------------------------------------------------------------
    every = set(all_params())
    if every:
        touched = set()
        for r in rows:
            touched |= set(r["set"])
            for row in r["mod"].split(";"):
                if ">" in row and ":" in row:
                    touched.add(row.split(">")[1].split(":")[0])
        never = sorted(every - touched)
        print(f"\nPARAMETER COVERAGE\n  {len(every - set(never))} of {len(every)} parameters appear in at least one preset "
              f"({100.0*(len(every)-len(never))/len(every):.0f} %)")
        if never:
            print(f"  never touched ({len(never)}): {', '.join(never[:24])}"
                  + (" ..." if len(never) > 24 else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
