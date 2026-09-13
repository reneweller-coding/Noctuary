"""How tonal is the library really? Read from the packs: which clips the sample slots carry
(Textures = a pitch was detected, FieldRecordings = none), whether they follow the note, and
what the measurement said (noisy rank, Tonal/Noisy tags)."""
import glob
import io
import os
import re
import collections

ROOT = r"G:\Tools\VRAudio\Noctuary"
CLIP = re.compile(r"(Textures|FieldRecordings)/[^|;,\s]+")
TAG_TONAL, TAG_NOISY = 1 << 4, 1 << 5
# The spectral table type is called Harmonic since the classic wavetable arrived, and leaving it
# out of this set counted every preset anchored by one as "no tonal slot" -- 17.7 % of the 2.0
# library against the 3 % that are really unanchored (the two field-recording packs).
TONAL_TYPES = {"Additive", "Harmonic", "Wavetable", "FM", "Bow"}
SAMPLE_TYPES = {"Texture", "Stretch", "Spectral"}

import sys
PACKS = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "Library", "Packs")
rows = []
for pack in sorted(glob.glob(os.path.join(PACKS, "*.ambientpack"))):
    for line in io.open(pack, encoding="utf-8"):
        if line.startswith("#") or line.startswith("pack ") or "|" not in line:
            continue
        cols = line.rstrip("\n").split("|")
        if len(cols) < 4:
            continue
        name, settings, meas, texcol = cols[0], cols[1], cols[2], cols[3]
        kv = dict(t.split("=", 1) for t in settings.split(";") if "=" in t and ">" not in t)
        m = meas.split()
        try:
            noisy = float(m[5]); bright = float(m[2]); tags = int(m[8], 0) if len(m) > 8 else 0
        except (ValueError, IndexError):
            noisy = bright = float("nan"); tags = 0
        clips = CLIP.findall(texcol) or CLIP.findall(line)
        folders = set(clips)
        # the slots: what sounds, and whether it sits on the note
        slots = []
        for n in range(1, 5):
            t = kv.get(f"src{n}_type", "Additive" if n == 1 else "Off")
            if t == "Off":
                continue
            slots.append((t, kv.get(f"src{n}_follow", "Note")))
        has_tonal_slot = any(t in TONAL_TYPES or (t in SAMPLE_TYPES and f == "Note") for t, f in slots)
        only_free_samples = bool(slots) and not has_tonal_slot
        rows.append(dict(name=name, pack=os.path.basename(pack), noisy=noisy, bright=bright, tags=tags,
                         folders=folders, primary=kv.get("src1_type", "Additive"),
                         slots=slots, only_free_samples=only_free_samples,
                         nslots=len(slots)))

N = len(rows)
print(f"{N} pack presets")

def share(sel):
    k = sum(1 for r in rows if sel(r))
    return f"{k:5d}  {100.0 * k / N:5.1f} %"

print("\nWHICH CLIPS")
print("  no sample                  ", share(lambda r: not r["folders"]))
print("  Textures only (pitched)    ", share(lambda r: r["folders"] == {"Textures"}))
print("  FieldRecordings only       ", share(lambda r: r["folders"] == {"FieldRecordings"}))
print("  both                       ", share(lambda r: len(r["folders"]) == 2))

print("\nDOES ANYTHING SIT ON THE NOTE?")
print("  a tonal slot (Additive/Wavetable/FM/Bow or a sample following the note)", share(lambda r: not r["only_free_samples"]))
print("  ONLY sample slots, all Free -- the pitch played is irrelevant               ", share(lambda r: r["only_free_samples"]))
print("  ... of which primary = Texture                                              ", share(lambda r: r["only_free_samples"] and r["primary"] == "Texture"))
print("  ... of which primary = Stretch/Spectral                                     ", share(lambda r: r["only_free_samples"] and r["primary"] in ("Stretch", "Spectral")))

print("\nMEASURED, BY WHAT THE SLOTS CARRY (noisy = rank of spectral flatness across the whole library, 0..1)")
groups = [
    ("no sample", lambda r: not r["folders"]),
    ("Textures only", lambda r: r["folders"] == {"Textures"}),
    ("FieldRecordings only", lambda r: r["folders"] == {"FieldRecordings"}),
    ("only Free samples", lambda r: r["only_free_samples"]),
    ("primary Texture", lambda r: r["primary"] == "Texture"),
    ("primary Additive", lambda r: r["primary"] == "Additive"),
    ("primary Wavetable", lambda r: r["primary"] == "Wavetable"),
]
print(f"  {'group':24s} {'n':>5s} {'noisy':>6s} {'bright':>6s} {'tag Noisy':>10s} {'tag Tonal':>10s}")
for label, sel in groups:
    g = [r for r in rows if sel(r)]
    if not g:
        continue
    import math
    nz = [r["noisy"] for r in g if not math.isnan(r["noisy"])]
    br = [r["bright"] for r in g if not math.isnan(r["bright"])]
    tn = sum(1 for r in g if r["tags"] & TAG_NOISY); tt = sum(1 for r in g if r["tags"] & TAG_TONAL)
    print(f"  {label:24s} {len(g):5d} {sum(nz)/max(1,len(nz)):6.2f} {sum(br)/max(1,len(br)):6.2f} {100.0*tn/len(g):9.1f}% {100.0*tt/len(g):9.1f}%")

# The built-ins, from the generated table, as the yardstick.
meta = io.open(os.path.join(ROOT, "Core", "src", "PresetMeta.cpp"), encoding="utf-8").read()
built = re.findall(r"\{\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*([0-9.]+)f,\s*(-?\d+),\s*(0x[0-9a-f]+)u", meta)
if built:
    nz = [float(b[5]) for b in built]; br = [float(b[2]) for b in built]
    tn = sum(1 for b in built if int(b[9], 16) & TAG_NOISY); tt = sum(1 for b in built if int(b[9], 16) & TAG_TONAL)
    print(f"  {'BUILT-IN (yardstick)':24s} {len(built):5d} {sum(nz)/len(nz):6.2f} {sum(br)/len(br):6.2f} {100.0*tn/len(built):9.1f}% {100.0*tt/len(built):9.1f}%")

print("\nPACKS WITH THE MOST 'ONLY FREE SAMPLES' PRESETS")
c = collections.Counter(r["pack"] for r in rows if r["only_free_samples"])
tot = collections.Counter(r["pack"] for r in rows)
for pk, k in c.most_common(10):
    print(f"  {pk:36s} {k:4d} of {tot[pk]:4d}  {100.0*k/tot[pk]:5.1f} %")
