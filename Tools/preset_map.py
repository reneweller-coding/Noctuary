"""Measure every built-in preset and generate Core/src/PresetMeta.cpp: a 2-D map position,
perceptual descriptors (brightness, motion, width, noisiness, bass, density) and tags.

    python Tools/preset_map.py            renders all presets (needs build/.../ambient_render)
    python Tools/preset_map.py --stub     writes an all-zero table so the core compiles before
                                          the first measurement

Measurement per preset: 12 s at 48 kHz with a held chord (A2 E3 B3) and the brain at a
faster event rate; descriptors from the last 8 s. Positions: PCA of the standardised
descriptors, then a short repulsion pass so points do not sit on top of each other.
Tags: quantiles of the descriptors plus flags read from the preset's parameters.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
OUT_CPP = os.path.join(ROOT, "Core", "src", "PresetMeta.cpp")
PRESETS_CPP = os.path.join(ROOT, "Core", "src", "Presets.cpp")

TAGS = ["Dark", "Bright", "Calm", "Moving", "Tonal", "Noisy", "Wide", "Bass", "Dense", "Sparse", "Keys", "Generative",
        "Cosmos", "Feedback", "Sources", "JustIntonation", "Sub", "Stack", "Air",
        # The drone tags (1.11.0), in the same order as PresetTag in PresetMeta.h.
        "Still", "Evolving", "Smooth", "Rough", "Near", "Far"]


def families():
    """(start index, name) from the '// ---- N..M name' comments in Presets.cpp."""
    fams = []
    with open(PRESETS_CPP, encoding="utf-8") as f:
        for line in f:
            m = re.search(r"//\s*-{5,}\s*(\d+)\.\.(\d+)\s+(.+)$", line)
            if m:
                fams.append((int(m.group(1)), m.group(3).strip()))
    return fams


def family_of(index, fams):
    fi = 0
    for i, (start, _) in enumerate(fams):
        if index >= start:
            fi = i
    return fi


def read_wav(path):
    sys.path.insert(0, HERE)
    from analyze import read_wav as rw
    return rw(path)


def describe(x, sr):
    """Descriptors from the last 8 s of a stereo render."""
    n = x.shape[0]
    seg = x[max(0, n - 8 * sr):]
    mono = seg.mean(axis=1)
    win = sr // 2
    freqs = np.fft.rfftfreq(win, 1.0 / sr)
    cents, flats, specs, bass = [], [], [], []
    for s in range(0, len(mono) - win + 1, win // 2):
        p = np.abs(np.fft.rfft(mono[s:s + win] * np.hanning(win))) ** 2
        tot = p.sum() + 1e-20
        cents.append(np.log2(max((freqs * p).sum() / tot, 20.0)))
        band = p[(freqs > 60) & (freqs < 12000)] + 1e-20
        flats.append(np.exp(np.mean(np.log(band))) / np.mean(band))
        specs.append(p / tot)
        bass.append(p[freqs < 150].sum() / tot)
    specs = np.array(specs)
    flux = np.mean(np.sum(np.abs(np.diff(np.sqrt(specs), axis=0)), axis=1)) if len(specs) > 1 else 0.0
    L, R = seg[:, 0], seg[:, 1]
    corr = float(np.corrcoef(L, R)[0, 1]) if L.std() > 1e-6 and R.std() > 1e-6 else 1.0
    return {"centroid": float(np.mean(cents)), "flatness": float(np.log10(np.mean(flats) + 1e-6)), "flux": float(flux),
            "width": float(1.0 - abs(corr)), "bass": float(np.mean(bass)),
            "rms": float(20 * np.log10(np.sqrt((seg ** 2).mean()) + 1e-12))}


def run_preset(index, name):
    with tempfile.TemporaryDirectory() as td:
        wav = os.path.join(td, "p.wav")
        cmd = [RENDER, "--preset", name, "--seconds", "12", "--notes", "45,52,59", "--set", "brain_rate=6",
               "--set", "brain_hold_min=6", "--stats", "--dump", "--out", wav]
        res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if res.returncode != 0:
            raise RuntimeError(res.stderr.strip()[:200])
        voices, params = [], {}
        for line in res.stdout.splitlines():
            m = re.match(r"^\s*(\d+),\s*[-\d.]+,\s*[-\d.]+,\s*[\d.]+,\s*(\d+),", line)
            if m:
                voices.append(int(m.group(2)))
            m = re.match(r"^param\s+(\S+)\s*=\s*(.+)$", line)
            if m:
                params[m.group(1)] = m.group(2).strip()
        x, sr = read_wav(wav)
    d = describe(x, sr)
    d["voices"] = float(np.mean(voices[-8:])) if voices else 0.0
    d["params"] = params
    return d


def flags_from_params(p):
    f = set()
    num = lambda k: float(p.get(k, "0") or 0)
    if p.get("brain_on", "on") == "off": f.add("Keys")
    else: f.add("Generative")
    if num("cosmos_send") > 0.05 or num("cosmos_shimmer") > 0.05: f.add("Cosmos")
    if num("fb_bus") > 0.02 or num("fb_fm") > 0.02: f.add("Feedback")
    if p.get("src2_type", "Off") != "Off" or p.get("src3_type", "Off") != "Off": f.add("Sources")
    if any(k in p.get("scale", "") for k in ("JI", "Harmonic", "Subharmonic", "Pythag", "Bohlen", "Otonal", "Slendro")): f.add("JustIntonation")
    if num("sub_level") > 0.05: f.add("Sub")
    if p.get("stack", "Detune") != "Detune": f.add("Stack")
    if num("air") >= 0.3: f.add("Air")
    return f


def rank(v):
    order = np.argsort(np.argsort(v))
    return order / max(len(v) - 1, 1)


def spread(xy, iters=200, min_d=0.035):
    xy = xy.copy()
    for _ in range(iters):
        d = xy[:, None, :] - xy[None, :, :]
        dist = np.sqrt((d ** 2).sum(axis=2)) + 1e-9
        push = np.clip(min_d - dist, 0, None) / dist
        np.fill_diagonal(push, 0)
        xy += 0.5 * (d * push[:, :, None]).sum(axis=1)
        xy = np.clip(xy, 0.0, 1.0)
    return xy


def write_cpp(rows, fams, stub=False):
    # The file is Doxygen-documented like the rest of Core/src (docs/Doxyfile): the header block and
    # the three table briefs below are emitted here so that a re-measurement keeps the documentation.
    lines = ["/**",
             " * @file PresetMeta.cpp",
             " * @brief Generated by Tools/preset_map.py -- do not edit.",
             " *",
             " * " + ("STUB: run the tool to measure the presets." if stub else "Measured from renders of every preset."),
             " *",
             " * This is the measured side of the built-in library: for each of the compiled-in presets, in the",
             " * order of Presets.cpp, its position on the map, its perceptual descriptors, its family, its tag",
             " * bits, its loudness, its phrases and its cluster (see PresetMeta.h for what each field means and",
             " * how it was measured). The browser filters and sorts on it, the map draws it, PresetMap.cpp blends",
             " * between neighbours on it, and PresetPacks.cpp appends the packs' own metadata behind it so that",
             " * presetMeta(index) answers for the whole list. Nothing in here is edited by hand: a re-measurement",
             " * rewrites the file.",
             " */",
             '#include "ambient/PresetMeta.h"', '#include "ambient/Presets.h"', "", "namespace ambient {", "", "namespace {",
             "/** @brief The names of the built-in families, indexed by PresetMeta::family; a pack's presets take a family appended after these. */",
             "const char* const kFamilies[] = {"]
    for _, name in fams:
        lines.append(f'    "{name}",')
    lines += ["};",
              "/** @brief One name per PresetTag bit, in bit order, for the browser's tag filter. */",
              "const char* const kTagNames[kNumPresetTags] = {"]
    lines.append("    " + ", ".join(f'"{t}"' for t in TAGS))
    lines += ["};",
              "/**",
              " * @brief The measured metadata of every built-in preset, one row per preset in the order of",
              " *        Presets.cpp; the comment beside each row is the preset's name.",
              " */",
              "const PresetMeta kMeta[] = {"]
    for r in rows:
        lines.append("    { %.4ff, %.4ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %.3ff, %d, 0x%xu, %.1ff, %.3ff, %.3ff, %.3ff, { %d, %d }, %d },   // %s" %
                     (r["x"], r["y"], r["bright"], r["motion"], r["width"], r["noisy"], r["bass"], r["density"],
                      r["family"], r["tags"], r.get("rms", 0.0),
                      r.get("evolve", 0.5), r.get("rough", 0.5), r.get("wet", 0.5),
                      r.get("phrase", [-1, -1])[0], r.get("phrase", [-1, -1])[1],
                      r.get("cluster", -1), r["name"]))
    lines += ["};", "}", "",
              "int builtinPresetMetaCount() { return %s; }" % ("0" if stub else "static_cast<int>(sizeof(kMeta) / sizeof(kMeta[0]))"),
              "const PresetMeta& builtinPresetMeta(int index) { static const PresetMeta none = { 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 0.0f, 0.5f, 0.5f, 0.5f, { -1, -1 }, -1 };"
              " return (index >= 0 && index < builtinPresetMetaCount()) ? kMeta[index] : none; }",
              "int builtinPresetFamilyCount() { return static_cast<int>(sizeof(kFamilies) / sizeof(kFamilies[0])); }",
              "const char* builtinPresetFamilyName(int family) { return (family >= 0 && family < builtinPresetFamilyCount()) ? kFamilies[family] : \"\"; }",
              "const char* presetTagName(int bit) { return (bit >= 0 && bit < kNumPresetTags) ? kTagNames[bit] : \"\"; }",
              "", "} // namespace ambient", ""]
    with open(OUT_CPP, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stub", action="store_true")
    ap.add_argument("--json", default=os.path.join(ROOT, "docs", "preset-map.json"), help="also write the raw measurements here")
    a = ap.parse_args()
    fams = families()
    if a.stub:
        rows = [{"name": "stub", "x": 0.5, "y": 0.5, "bright": 0.5, "motion": 0.5, "width": 0.5, "noisy": 0.5, "bass": 0.5, "density": 0.5, "family": 0, "tags": 0, "rms": 0.0}]
        write_cpp(rows, fams, stub=True)
        print("stub written:", OUT_CPP); return 0
    names = subprocess.run([RENDER, "--list-presets"], capture_output=True, text=True, encoding="utf-8").stdout.splitlines()
    names = [n for n in names if n.strip()]
    measured = []
    for i, name in enumerate(names):
        try:
            d = run_preset(i, name)
        except Exception as e:
            print(f"FAIL {name}: {e}"); d = {"centroid": 8.0, "flatness": -3.0, "flux": 0.0, "width": 0.5, "bass": 0.3, "rms": -30.0, "voices": 3.0, "params": {}}
        d["name"] = name; d["index"] = i
        measured.append(d)
        print(f"{i:3d} {name:28s} centroid 2^{d['centroid']:.1f}  flat {d['flatness']:.2f}  flux {d['flux']:.3f}  width {d['width']:.2f}  bass {d['bass']:.2f}  voices {d['voices']:.1f}")
    keys = ["centroid", "flatness", "flux", "width", "bass", "voices"]
    M = np.array([[m[k] for k in keys] for m in measured], dtype=np.float64)
    Z = (M - M.mean(axis=0)) / (M.std(axis=0) + 1e-9)
    # a couple of parameter flags join the embedding so the cosmos/feedback/source presets cluster
    flagcols = np.array([[1.0 if "Cosmos" in flags_from_params(m["params"]) else 0.0,
                          1.0 if "Feedback" in flags_from_params(m["params"]) else 0.0,
                          1.0 if "Sources" in flags_from_params(m["params"]) else 0.0,
                          1.0 if "Keys" in flags_from_params(m["params"]) else 0.0] for m in measured])
    Z = np.hstack([Z, 1.5 * (flagcols - flagcols.mean(axis=0))])
    U, S, Vt = np.linalg.svd(Z, full_matrices=False)
    xy = U[:, :2] * S[:2]
    xy = (xy - xy.min(axis=0)) / (xy.max(axis=0) - xy.min(axis=0) + 1e-9)
    # Half rank, half raw PCA per axis: the plane fills evenly (a single strong flag no longer
    # squeezes everything else into a band) while clusters keep their shape.
    xy = 0.5 * np.column_stack([rank(xy[:, 0]), rank(xy[:, 1])]) + 0.5 * xy
    xy = 0.06 + 0.88 * spread(xy, min_d=0.045)
    r = {k: rank(M[:, i]) for i, k in enumerate(keys)}
    rows = []
    for i, m in enumerate(measured):
        tags = set(flags_from_params(m["params"]))
        if r["centroid"][i] < 0.3: tags.add("Dark")
        if r["centroid"][i] > 0.7: tags.add("Bright")
        if r["flux"][i] < 0.3: tags.add("Calm")
        if r["flux"][i] > 0.7: tags.add("Moving")
        if r["flatness"][i] < 0.4: tags.add("Tonal")
        if r["flatness"][i] > 0.7: tags.add("Noisy")
        if r["width"][i] > 0.7: tags.add("Wide")
        if r["bass"][i] > 0.7: tags.add("Bass")
        if r["voices"][i] > 0.7: tags.add("Dense")
        if r["voices"][i] < 0.3: tags.add("Sparse")
        bits = sum(1 << TAGS.index(t) for t in tags)
        rows.append({"name": m["name"], "x": float(xy[i, 0]), "y": float(xy[i, 1]), "bright": float(r["centroid"][i]), "motion": float(r["flux"][i]),
                     "width": float(r["width"][i]), "noisy": float(r["flatness"][i]), "bass": float(r["bass"][i]), "density": float(r["voices"][i]),
                     "family": family_of(i, fams), "tags": bits, "tagNames": sorted(tags),
                     "rms": float(m.get("rms", 0.0))})
    write_cpp(rows, fams)
    with open(a.json, "w", encoding="utf-8") as f:
        json.dump({"presets": rows, "families": [n for _, n in fams], "measurements": [{k: v for k, v in m.items() if k != "params"} for m in measured]}, f, indent=1)
    print(f"wrote {OUT_CPP} ({len(rows)} presets, {len(fams)} families) and {a.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
