"""Keep Near Auto's table (near_by_artist.json) in step with the near bank (19.11.2026).

The table names near presets in groups ("flute_note", "drops", ...), each artist weights the groups and
gives each a class (H often, G sometimes, S rarely), and each artist's list of presets is derived from
that: a preset's weight is its group's weight over the sum of the artist's group weights, shared equally
among the group's presets; its Every factor is its class's (H 0.6-1.0, G 1.0-2.0, S 2.0-4.0). All 1561
entries of the table followed that rule exactly when this was written, so the lists can be rebuilt
from the groups rather than edited by hand.

What it does: presets the bank no longer has leave their groups (a group left empty leaves the table
and every artist's weighting -- the others share its weight); every variant of make_layer_presets.py
(NEAR_VARIANTS) joins its parent's group; then every artist's list is rebuilt. Run it, then
Tools/make_near_auto.py.

    python Tools/library/near_table.py [--dry-run]
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TABLE = os.path.join(HERE, "near_by_artist.json")
CLASS_FACTOR = {"H": [0.6, 1.0], "G": [1.0, 2.0], "S": [2.0, 4.0]}


def bank():
    """The near bank as make_layer_presets.py builds it: {name: settings}, and its variants."""
    sys.path.insert(0, os.path.dirname(HERE))
    import make_layer_presets as m
    names = {}
    for _, ps in m.near_bank():
        for nm, kw in ps:
            names[nm] = kw
    # the new bases (19.11.2026) first, into their liked relative's group, then every variant after its parent
    relatives = [(parent, name, {}) for name, parent in m.GROUP_PARENT.items()]
    return names, relatives + m.NEAR_VARIANTS


def rebuild(d):
    """Every artist's preset list from its group weights (the rule above)."""
    for a in d["artists"].values():
        tot = sum(v["weight"] for g, v in a["groups"].items() if d["groups"].get(g, {}).get("presets"))
        old = {p["preset"]: p for p in a["presets"]}
        out = []
        for g, v in a["groups"].items():
            ps = d["groups"].get(g, {}).get("presets", [])
            if not ps or tot <= 0:
                continue
            for p in ps:
                rate = d["presets"].get(p, {}).get("rate")
                lo, hi = CLASS_FACTOR[v["klasse"]]
                e = {"preset": p, "group": g, "weight": round(v["weight"] / tot / len(ps), 4), "klasse": v["klasse"],
                     "rate_factor": [lo, hi]}
                if rate is not None:
                    e["fore_rate_s"] = [int(round(min(900, max(10, rate * lo)))), int(round(min(900, max(10, rate * hi))))]
                elif p in old and "fore_rate_s" in old[p]:
                    e["fore_rate_s"] = old[p]["fore_rate_s"]
                out.append(e)
        a["presets"] = out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    d = json.load(open(TABLE, encoding="utf-8"))
    names, variants = bank()
    removed = sorted({p for g in d["groups"].values() for p in g["presets"] if p not in names and p not in
                      {v for _, v, _ in variants}})
    for g in d["groups"].values():
        g["presets"] = [p for p in g["presets"] if p in names]
    for p in removed:
        d["presets"].pop(p, None)
    empty = [g for g, v in d["groups"].items() if not v["presets"]]
    added = []
    for parent, name, over in variants:
        if name not in names:
            continue
        for g in d["groups"].values():
            if parent in g["presets"] and name not in g["presets"]:
                g["presets"].insert(g["presets"].index(parent) + 1, name)
                added.append(name)
        if parent in d["presets"] and name not in d["presets"]:
            info = dict(d["presets"][parent])
            info["variant_of"] = parent
            if "rate" in info and "fore_rate" in names[name]:
                info["rate"] = names[name]["fore_rate"]   # its own Every, which a new base need not share
            d["presets"][name] = info
    for g in empty:
        d["groups"].pop(g)
        for art in d["artists"].values():
            art["groups"].pop(g, None)
    before = sum(len(x["presets"]) for x in d["artists"].values())
    rebuild(d)
    after = sum(len(x["presets"]) for x in d["artists"].values())
    print("removed from the table: %s; groups emptied and dropped: %s" % (", ".join(removed) or "none", ", ".join(empty) or "none"))
    print("variants placed in their parents' groups: %d; artists' entries %d -> %d" % (len(set(added)), before, after))
    lost = [v for _, v, _ in variants if v in names and v not in set(added)]
    if lost:
        print("variants whose parent is in no group (not in Auto): " + ", ".join(lost))
    if not a.dry_run:
        with open(TABLE, "w", encoding="utf-8", newline="\n") as f:
            json.dump(d, f, ensure_ascii=False, indent=1)   # as the file was written: no newline at its end
        print("wrote " + TABLE)
    return 0


if __name__ == "__main__":
    sys.exit(main())
