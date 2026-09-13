"""Noctuary -- which of the instrument's parameters the presets actually use.

The parameter table has 386 entries. The presets are 191 compiled in and 6000 in the library.
This counts, for every parameter, how many presets move it off its default and how many point a
modulation route at it -- so "we added a control and nobody uses it" becomes a number rather
than a suspicion.

    python Tools/param_usage.py                  the whole table, by section
    python Tools/param_usage.py --unused         only what no preset touches
    python Tools/param_usage.py --top 40         the forty most used
    python Tools/param_usage.py --json usage.json

A parameter counts as *set* when a preset's settings string names it. That is the honest
definition here: a preset's settings are exactly its departures from the default, so a
parameter absent from all of them is one every preset leaves alone. It counts as *modulated*
when a route in the preset's matrix targets it.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
RENDER = os.path.join(ROOT, "build", "Tools", "render", "Release", "ambient_render.exe")
PRESETS = os.path.join(ROOT, "Core", "src", "Presets.cpp")
PACKS = os.path.join(ROOT, "Library", "Packs")

KEY = re.compile(r"(?:^|;)\s*([a-z0-9_]+)=")

# A comment between two of a preset's string literals breaks any pattern that expects the run of
# literals to be unbroken -- and it does not break it loudly, it simply stops finding that preset.
# Two of the built-in presets carry one, and this file's parsers reported 194 of 196 for a while
# without a word. Comments that begin a line are removed before matching; a "//" inside a literal
# is left alone, because none of them start a line.
def strip_line_comments(text):
    out = []
    for line in text.split(chr(10)):
        stripped = line.lstrip()
        out.append("" if stripped.startswith("//") else line)
    return chr(10).join(out)



def parameters():
    """[(key, section)] in table order, from the instrument itself rather than a copy of it."""
    if not os.path.isfile(RENDER):
        sys.exit("build the render tool first (it is what knows the parameter table)")
    out = subprocess.run([RENDER, "--list"], capture_output=True, text=True, cwd=ROOT)
    rows = []
    for line in (out.stdout + out.stderr).splitlines():
        # One space, not two: the key column is padded to eighteen, and the two longest keys
        # (cosmos_shift_drift, cosmos_shimmer_pitch) fill it, so a two-space rule drops exactly
        # those two -- and then reports the 986 presets that use them as naming an unknown
        # parameter. The section is the run of words before the range bracket.
        m = re.match(r"^([a-z0-9_]+)\s+(.+?)\s{2,}\[", line)
        if m:
            rows.append((m.group(1), m.group(2).strip()))
    if not rows:
        sys.exit("could not read the parameter table from --list")
    return rows


def builtins():
    """[(name, settings, matrix)] over Core/src/Presets.cpp."""
    with open(PRESETS, encoding="utf-8") as f:
        text = strip_line_comments(f.read())
    body = text[text.index("const Preset kPresets[]"):text.index("int numCosmosPresets")]
    out = []
    # Two forms live side by side: { "name", "settings" } and the six-field one an enriched
    # preset carries, { "name", "settings", nullptr, nullptr, nullptr, "matrix" }.
    for m in re.finditer(r'\{\s*"([^"]+)",\s*((?:"[^"]*"\s*)+)(?:,([^}]*))?\}', body):
        settings = "".join(re.findall(r'"([^"]*)"', m.group(2)))
        tail = m.group(3) or ""
        mod = "".join(re.findall(r'"([^"]*)"', tail))
        out.append((m.group(1), settings, mod))
    return out


def packs():
    """[(name, settings, matrix)] over every *.ambientpack."""
    out = []
    if not os.path.isdir(PACKS):
        return out
    for fn in sorted(os.listdir(PACKS)):
        if not fn.endswith(".ambientpack"):
            continue
        with open(os.path.join(PACKS, fn), encoding="utf-8", errors="replace") as f:
            for line in f:
                t = line.strip()
                if not t or t.startswith("#") or t.startswith("pack ") or (t.startswith("format ") and "|" not in t):
                    continue
                p = t.split("|")
                if len(p) >= 2:
                    out.append((p[0].strip(), p[1].strip(), p[6].strip() if len(p) > 6 else ""))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--unused", action="store_true", help="only the parameters nothing touches")
    ap.add_argument("--sections", action="store_true", help="one line per section instead")
    ap.add_argument("--top", type=int, default=0, help="only the N most used, across all sections")
    ap.add_argument("--json", default="")
    ap.add_argument("--no-packs", action="store_true", help="count the built-in presets only")
    a = ap.parse_args()

    table = parameters()
    known = {k for k, _ in table}
    bi = builtins()
    pk = [] if a.no_packs else packs()

    set_bi, set_pk, modded, unknown = {}, {}, {}, {}
    for rows, counter in ((bi, set_bi), (pk, set_pk)):
        for _, settings, mod in rows:
            for k in set(KEY.findall(settings)):
                if k in known:
                    counter[k] = counter.get(k, 0) + 1
                else:
                    unknown[k] = unknown.get(k, 0) + 1
            for t in set(re.findall(r">\s*([a-z0-9_]+)\s*:", mod)):
                if t in known:
                    modded[t] = modded.get(t, 0) + 1

    total = len(bi) + len(pk)
    rows = [dict(key=k, section=s, builtin=set_bi.get(k, 0), pack=set_pk.get(k, 0),
                 total=set_bi.get(k, 0) + set_pk.get(k, 0), modulated=modded.get(k, 0))
            for k, s in table]

    print("%d parameters, %d presets (%d built in, %d in the library)"
          % (len(table), total, len(bi), len(pk)))
    print()

    def line(r):
        return ("  %-22s %6d %5.1f %%  %6d %6d %8d"
                % (r["key"], r["total"], 100.0 * r["total"] / max(total, 1),
                   r["builtin"], r["pack"], r["modulated"]))

    head = "  %-22s %6s %7s  %6s %6s %8s" % ("parameter", "set", "of all", "built", "packs", "as a target")

    if a.unused:
        dead = [r for r in rows if r["total"] == 0 and r["modulated"] == 0]
        quiet = [r for r in rows if r["total"] == 0 and r["modulated"] > 0]
        print("%d parameters no preset sets and no route targets:" % len(dead))
        print(head)
        for r in dead:
            print(line(r) + "   " + r["section"])
        if quiet:
            print()
            print("%d more that no preset sets but something modulates:" % len(quiet))
            for r in quiet:
                print(line(r) + "   " + r["section"])
    elif a.sections:
        # A section is used when any of its parameters is: how many presets reach into it at
        # all, and how much of it they use once they are there.
        order, by_section = [], {}
        for r in rows:
            if r["section"] not in by_section:
                by_section[r["section"]] = []
                order.append(r["section"])
            by_section[r["section"]].append(r)
        print("  %-18s %5s %7s  %8s %s" % ("section", "params", "unused", "busiest", "the busiest one"))
        for s in order:
            rs = by_section[s]
            best = max(rs, key=lambda r: r["total"])
            print("  %-18s %5d %7d  %7.1f %% %s"
                  % (s, len(rs), sum(1 for r in rs if r["total"] == 0 and r["modulated"] == 0),
                     100.0 * best["total"] / max(total, 1), best["key"]))
    elif a.top:
        print(head)
        for r in sorted(rows, key=lambda r: -r["total"])[:a.top]:
            print(line(r) + "   " + r["section"])
    else:
        section = None
        for r in rows:
            if r["section"] != section:
                section = r["section"]
                print()
                print("%s" % section)
                print(head)
            print(line(r))

    dead = sum(1 for r in rows if r["total"] == 0 and r["modulated"] == 0)
    half = sum(1 for r in rows if r["total"] >= total * 0.5)
    print()
    print("used by every preset: %d   by at least half: %d   by none at all: %d"
          % (sum(1 for r in rows if r["total"] == total), half, dead))
    if unknown:
        print("settings naming something the parameter table does not have: %s"
              % ", ".join("%s (%d)" % kv for kv in sorted(unknown.items(), key=lambda kv: -kv[1])[:10]))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump({"presets": total, "builtin": len(bi), "packs": len(pk), "parameters": rows},
                      f, indent=1)
        print("wrote %s" % a.json)


if __name__ == "__main__":
    main()
