"""The mixing desk's four, and the hands, for the 191 compiled-in presets.

Tools/library/retrofit_presets.py does this for the generated library from each preset's own
settings; this does the same for the presets in Core/src/Presets.cpp, with one difference that
the built-ins have earned: every change that alters the sound is rendered first and kept only
if the preset still sounds like itself -- level within a decibel and a half, spectral centre
within a quarter, width within reach, no clipping. The hands (aftertouch, wheel, slide as matrix
routes) need no such check: written with the 0..1 flag they do nothing until a hand moves.

    python Tools/retrofit_builtins.py --dry-run
    python Tools/retrofit_builtins.py

Deterministic: the seed is the preset's name. Run twice, the second run finds nothing to add.
"""
import argparse
import hashlib
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
sys.path.insert(0, os.path.join(ROOT, "Tools", "library"))
sys.path.insert(0, HERE)
from enrich_presets import strip_line_comments, wrap_literal, PRESETS, RENDER  # noqa: E402
from retrofit_presets import HAND_TARGETS, ALWAYS, parse_settings, num  # noqa: E402


def rng_for(name):
    return random.Random(int(hashlib.sha1(name.encode("utf-8")).hexdigest()[:12], 16))


# ---------------------------------------------------------------- reading the table

def fields_of(entry):
    """The fields of one { ... } entry: each is None (nullptr) or the joined string literals."""
    body = entry.strip()
    assert body[0] == "{" and body[-1] == "}"
    body = body[1:-1]
    fields, cur, i, in_str, have_str = [], [], 0, False, False
    lit = []
    while i < len(body):
        c = body[i]
        if in_str:
            if c == "\\":
                lit.append(body[i:i + 2]); i += 2; continue
            if c == '"':
                in_str = False; i += 1; continue
            lit.append(c); i += 1; continue
        if c == '"':
            in_str = True; have_str = True; i += 1; continue
        if c == ",":
            fields.append("".join(lit) if have_str else None)
            lit, have_str = [], False
            i += 1; continue
        i += 1
    fields.append("".join(lit) if have_str else None)
    return fields


def parse_table():
    with open(PRESETS, encoding="utf-8") as f:
        raw = f.read()
    text = strip_line_comments(raw)
    start = text.index("const Preset Presets[]")
    end = text.index("int numCosmosPresets")
    out = []
    depth, i, entry_start = 0, start, None
    # Walk the braces by hand: a regex over entries with a variable number of fields, some of
    # them holding semicolons and '>' characters, is how the first version of this lost the
    # matrix of every preset that had one.
    in_str = False
    while i < end:
        c = text[i]
        if in_str:
            if c == "\\": i += 2; continue
            if c == '"': in_str = False
            i += 1; continue
        if c == '"':
            in_str = True; i += 1; continue
        if c == "{":
            depth += 1
            if depth == 2: entry_start = i
        elif c == "}":
            if depth == 2 and entry_start is not None:
                f = fields_of(text[entry_start:i + 1])
                if f and f[0]:
                    out.append((f, (entry_start, i + 1)))
                entry_start = None
            depth -= 1
        i += 1
    return text, out


def measure(name, seconds, extra):
    cmd = [RENDER, "--preset", name, "--seconds", str(seconds), "--measure"]
    for kv in extra:
        cmd += ["--set", kv]
    env = dict(os.environ, AMBIENT_PACKS=os.path.join(ROOT, "Library", "Packs"))
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT, env=env,
                       creationflags=getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0))
    lines = (r.stdout + r.stderr).strip().splitlines()
    for l in reversed(lines):
        if l.startswith("measure:"):
            return {k: float(v) for k, v in re.findall(r"(\w+)=(-?[\d.]+)(?=\s|$)", l)}
    return None


def same_character(before, after):
    if after is None or before is None:
        return "did not render"
    bad = []
    if abs(after["rms"] - before["rms"]) > 1.5: bad.append("level")
    if abs(after["centroid"] - before["centroid"]) / max(before["centroid"], 1.0) > 0.25: bad.append("centroid")
    if abs(after["width"] - before["width"]) > 0.25: bad.append("width")
    if after["peak"] > 0.99: bad.append("clips")
    return ", ".join(bad)


# ---------------------------------------------------------------- the rules

def hands_for(name, settings, mod):
    r = rng_for(name + "/hands")
    rows = [x for x in mod.split(";") if x] if mod else []
    if len(rows) >= 15:
        return mod, 0
    used = {x.split(">")[1].split(":")[0] for x in rows if ">" in x}
    added = 0
    for source in r.sample(["pressure", "wheel", "slide"], k=r.choice([1, 2, 2])):
        if any(x.startswith(source + ">") for x in rows) or len(rows) >= 16:
            continue
        choices = [(t, lo, hi) for t, lo, hi in HAND_TARGETS[source]
                   if (t in settings or t in ALWAYS) and t not in used]
        if not choices:
            continue
        t, lo, hi = choices[r.randrange(len(choices))]
        rows.append("%s>%s:%.3f:u" % (source, t, r.uniform(lo, hi)))
        used.add(t)
        added += 1
    return ";".join(rows), added


def candidates(name, settings):
    """The sound-changing additions this preset qualifies for, each as a list of key=value."""
    r = rng_for(name + "/desk")
    out = []
    far, depth = num(settings, "far_level", 0.8), num(settings, "depth", 0.5)
    if "far_width" not in settings and far > 0.35 and depth > 0.45 and r.random() < 0.7:
        out.append(("far width", ["far_width=%.3f" % (r.uniform(0.55, 0.9) - 0.15 * depth)]))
    width, near = num(settings, "width", 1.0), num(settings, "near_mix", 0.0)
    if "haas" not in settings and width < 1.25 and (near > 0.05 or num(settings, "dly_mix", 0.0) > 0.1) \
            and r.random() < 0.4:
        out.append(("haas", ["haas=%.3f" % r.uniform(0.12, 0.35), "haas_time=%.1f" % r.uniform(11.0, 20.0)]))
    ens, rate, dep = num(settings, "ens_mix", 0.0), num(settings, "ens_rate", 0.2), num(settings, "ens_depth", 0.4)
    if "ens_mode" not in settings and 0.1 < ens < 0.6 and rate < 0.35 and dep < 0.7 and r.random() < 0.5:
        out.append(("microshift", ["ens_mode=Microshift", "ens_depth=%.3f" % r.uniform(0.35, 1.0),
                                   "ens_rate=%.4f" % (10 ** r.uniform(-1.7, -1.05))]))
    drive = max(num(settings, "filter_drive", 0.0), num(settings, "fb_drive", 0.0), num(settings, "patina", 0.0) * 0.5)
    if "filter_fold" not in settings and drive > 0.2 and r.random() < 0.5:
        out.append(("fold", ["filter_fold=%.3f" % r.uniform(0.08, 0.3)]))
    return out


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--seconds", type=float, default=14.0)
    ap.add_argument("--only", default="")
    a = ap.parse_args()
    if not os.path.isfile(RENDER):
        sys.exit("build the render tool first")

    text, entries = parse_table()
    print("%d built-in presets" % len(entries))
    edits, log = [], {"hands": 0, "far width": 0, "haas": 0, "microshift": 0, "fold": 0, "rejected": 0}
    for fields, span in entries:
        name = fields[0]
        if name == "Init" or (a.only and a.only.lower() not in name.lower()):
            continue
        settings_text = fields[1] or ""
        settings = parse_settings(settings_text)
        texture = fields[2] if len(fields) > 2 else None
        wavetable = fields[3] if len(fields) > 3 else None
        impulse = fields[4] if len(fields) > 4 else None
        mod = fields[5] if len(fields) > 5 else None
        envs = fields[6] if len(fields) > 6 else None

        new_mod, added = hands_for(name, settings, mod or "")
        log["hands"] += added
        adds = []
        cands = candidates(name, settings)
        if cands:
            before = measure(name, a.seconds, [])
            for why, kv in cands:
                after = measure(name, a.seconds, adds + kv)
                verdict = same_character(before, after)
                if verdict:
                    log["rejected"] += 1
                    continue
                adds += kv
                log[why] += 1
        if added == 0 and not adds:
            continue
        new_settings = settings_text + (";" if settings_text and adds else "") + ";".join(adds)
        edits.append((span, name, new_settings, texture, wavetable, impulse, new_mod or None, envs))

    print("touched %d presets:" % len(edits) + "".join("  %s %d" % (k, v) for k, v in log.items()))
    if a.dry_run:
        return 0
    out = text
    nl = chr(10)
    for (lo, hi), name, settings, texture, wavetable, impulse, mod, envs in sorted(edits, key=lambda e: -e[0][0]):
        lit = wrap_literal(settings)
        tail = []
        if mod or envs or texture or wavetable or impulse:
            tail.append("      %s, %s, %s," % tuple('"%s"' % v if v else "nullptr" for v in (texture, wavetable, impulse)))
            tail.append('      %s%s' % ('"%s"' % mod if mod else "nullptr", (',' + nl + '      "%s"' % envs) if envs else ""))
        entry = '{ "%s",' % name + nl + lit + ("," + nl + nl.join(tail) if tail else "") + " }"
        out = out[:lo] + entry + out[hi:]
    with open(PRESETS, "w", encoding="utf-8") as f:
        f.write(out)
    print("rewrote %s" % PRESETS)
    return 0


if __name__ == "__main__":
    sys.exit(main())
