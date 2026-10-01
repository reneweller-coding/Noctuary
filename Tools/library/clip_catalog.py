"""The library as data: every clip, table and impulse together with what its generator wrote down
about it.

The sample library was generated from prompt lists, and every line of those lists says more than
the file name can: the clip's category, its gesture (sustain, chord, swell, melodic, struck), its
harmony (a single note, a mode, a chord), its world (dark, tape, luminous, cosmic, cold, ritual, warm,
industrial, wild), which source types it was written for (Texture, Stretch, Spectral) and -- the one
the preset generator lives on -- the artists whose music it was made for (`style_targets`). The
wavetables carry a .json each (family, grounded, the note range a vowel is sung in, the clip a sampled
table was measured from) and so do the generated impulses (family, partner, design).

This module reads all of it once, joins it with the measurements in Library/tonality.json
(Tools/library/clip_tonality.py) and hands it to Tools/library/make_presets.py.

    python Tools/library/clip_catalog.py            what the library holds, and what is kept out
"""
import collections
import glob
import json
import os
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
LIBRARY = os.path.join(ROOT, "Library")
PROMPTS = os.environ.get("AMBIENT_PROMPTS", os.path.normpath(os.path.join(ROOT, "..", "_Sources", "StableAudio3", "prompts")))
TEX_PROMPTS = "stable_audio3_granular_prompts.jsonl"
FR_PROMPTS = "stable_audio3_fieldrecording_prompts.jsonl"
NOTE_RE = re.compile(r"_([A-G][#b]?-?\d+)\.(?:flac|wav)$")
PITCH_CLASS = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6,
               "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}
ROOM_FAMILIES = ("chamber", "hall", "cathedral", "cavern", "vast", "plate", "bloom", "far", "drift", "swell")

# What makes a clip unusable, measured over the whole file (clip_tonality.py): an offset that is a
# fraction of the level, a hole, or -- for a loop -- a jump at the seam that clicks on every pass.
MAX_DC = 0.05
MAX_GAP_TEXTURE = 1.5
MAX_GAP_FIELD = 3.0
MAX_SEAM_LOOP = 6.0


def key(name):
    """An artist's name as the lists and the styles can both spell it: no accents, no punctuation."""
    s = unicodedata.normalize("NFKD", name)
    s = "".join(c for c in s if not unicodedata.combining(c)).lower()
    return re.sub(r"[^a-z0-9]+", "", s)


def note_midi(token):
    """'F#4' -> 66, 'Bb-1' -> 10. None when there is no note."""
    if not token:
        return None
    m = re.match(r"^([A-G][#b]?)(-?\d+)$", token)
    if not m or m.group(1) not in PITCH_CLASS:
        return None
    return (int(m.group(2)) + 1) * 12 + PITCH_CLASS[m.group(1)]


def _prompts(name):
    path = os.path.join(PROMPTS, name)
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                row = json.loads(line)
                out[row["id"]] = row
    return out


_CLIPS = None


def clips(root=LIBRARY):
    """Every clip in Textures and FieldRecordings, with its prompt's metadata and its measurement."""
    global _CLIPS
    if _CLIPS is not None:
        return _CLIPS
    tonality = {}
    silent = set()
    tpath = os.path.join(root, "tonality.json")
    if os.path.exists(tpath):
        with open(tpath, encoding="utf-8") as f:
            blob = json.load(f)
        tonality = blob.get("clips", {})
        silent = set(blob.get("silent", []))
    tex_rows = _prompts(TEX_PROMPTS)
    fr_rows = {k[2:] if k.startswith("FR") else k: v for k, v in _prompts(FR_PROMPTS).items()}
    out = []
    for folder, kind in (("Textures", "texture"), ("FieldRecordings", "field")):
        files = sorted(glob.glob(os.path.join(root, folder, "*.flac")) + glob.glob(os.path.join(root, folder, "*.wav")))
        for path in files:
            name = os.path.basename(path)
            rel = folder + "/" + name
            if kind == "texture":
                m = re.search(r"-(\d{5})_", name)
                row = tex_rows.get(m.group(1)) if m else None
            else:
                m = re.search(r"-(\d{4})_loop", name)
                row = fr_rows.get(m.group(1)) if m else None
            row = row or {}
            nm = NOTE_RE.search(name) if kind == "texture" else None
            token = nm.group(1) if nm else None
            meas = tonality.get(rel)
            why = ""
            if rel in silent:
                why = "silent"
            elif meas:
                if meas.get("dc", 0.0) > MAX_DC:
                    why = "dc"
                elif meas.get("gap", 0.0) > (MAX_GAP_FIELD if kind == "field" else MAX_GAP_TEXTURE):
                    why = "gap"
                elif kind == "field" and meas.get("seam", 0.0) > MAX_SEAM_LOOP:
                    why = "seam"
            out.append({
                "rel": rel, "kind": kind, "id": m.group(1) if m else "",
                "category": row.get("category", name.split("-")[0] if kind == "texture" else ""),
                "gesture": row.get("gesture", ""), "harmony": row.get("harmony", ""), "world": row.get("world", ""),
                "texture": row.get("texture", ""), "engines": tuple(row.get("engines", ())),
                "targets": tuple(key(t) for t in row.get("style_targets", ())),
                "note": token, "midi": note_midi(token), "measure": meas, "unusable": why,
            })
    _CLIPS = out
    return out


def tables(root=LIBRARY):
    """Every wavetable on its shelf, with the family key the styles name ('harmonic:choir',
    'ambient:vowel_bass', 'classic:akwf', 'classic:wavedit')."""
    out = []
    base = os.path.join(root, "Wavetables")
    for path in sorted(glob.glob(os.path.join(base, "*", "*.wav"))):
        shelf = os.path.basename(os.path.dirname(path))
        name = os.path.basename(path)
        meta = {}
        js = os.path.splitext(path)[0] + ".json"
        if os.path.exists(js):
            with open(js, encoding="utf-8") as f:
                meta = json.load(f)
        recipe = meta.get("recipe", {}) or {}
        if shelf == "Classic":
            fam = "classic:" + ("akwf" if name.startswith("akwf_") else "wavedit")
        elif shelf == "Harmonic":
            fam = "harmonic:" + meta.get("family", name.split("_")[1] if "_" in name else "")
        else:
            fam = "ambient:" + meta.get("family", "")
        grounded = meta.get("grounded")
        if grounded is None:                       # the first HarmonicGen tables do not say; hollow ones are not
            grounded = meta.get("family") != "hollow"
        types = tuple(meta.get("types") or {"Harmonic": ("Harmonic",), "Classic": ("Wavetable",)}.get(shelf, ("Harmonic", "Wavetable")))
        src = str(recipe.get("source", ""))
        sid = re.search(r"(\d{5})", src)
        out.append({"rel": shelf + "/" + name, "shelf": shelf, "family": fam, "grounded": bool(grounded),
                    "types": types, "note_range": tuple(recipe["note_range"]) if recipe.get("note_range") else None,
                    "category": recipe.get("category", ""), "source_id": sid.group(1) if sid else ""})
    return out


def impulses(root=LIBRARY):
    """The impulses a new preset may take: the generated rooms (with family and partner), the curated
    diffusion set and the real rooms -- never the legacy files earlier packs named."""
    legacy = set()
    lpath = os.path.join(ROOT, "Tools", "library", "legacy_impulses.json")
    if os.path.exists(lpath):
        with open(lpath, encoding="utf-8") as f:
            legacy = set(json.load(f).get("files", {}))
    out = []
    for path in sorted(glob.glob(os.path.join(root, "Impulses", "*.wav"))):
        name = os.path.basename(path)
        if name in legacy:
            continue
        fam, partner, t_mid = "", "", None
        js = os.path.splitext(path)[0] + ".json"
        if os.path.exists(js):
            with open(js, encoding="utf-8") as f:
                meta = json.load(f)
            fam = meta.get("family", "")
            partner = meta.get("partner") or ""
            t_mid = (meta.get("design") or {}).get("T_mid")
        elif name.startswith("diffusion_"):
            fam = "diffusion"
        elif name.startswith("real_"):
            fam = "real"
        if not fam:
            continue
        out.append({"name": name, "family": fam, "partner": partner, "t_mid": t_mid})
    return out


def main():
    cl = clips()
    print(f"{len(cl)} clips: {sum(1 for c in cl if c['kind'] == 'texture')} textures, "
          f"{sum(1 for c in cl if c['kind'] == 'field')} field recordings")
    print("  without prompt metadata:", sum(1 for c in cl if not c["category"] or not c["targets"]))
    print("  without a measurement:", sum(1 for c in cl if c["measure"] is None))
    print("  kept out:", dict(collections.Counter(c["unusable"] for c in cl if c["unusable"])))
    arts = collections.Counter(t for c in cl if not c["unusable"] for t in c["targets"])
    print(f"  {len(arts)} artists named as targets")
    tb = tables()
    print(f"{len(tb)} tables:", dict(collections.Counter(t["family"] for t in tb)))
    im = impulses()
    print(f"{len(im)} impulses:", dict(collections.Counter(i["family"] for i in im)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
