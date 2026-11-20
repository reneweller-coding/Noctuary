#!/usr/bin/env python3
"""One conductor for the family: write or check each instrument's copy of ClusterBrain.h (plan A1, 20.11.2026).

The conductor (ClusterBrain.h, header-only) is checked in three times: Noctuary (namespace ambient), Empyrean
(additiv) and Zodiacal (granular). The repositories are public one by one and each builds alone, so each keeps its
own copy; this script makes every copy from one canonical file, ClusterBrain.h.in beside it.

Home of record: Noctuary's repository, Tools/family/ (ClusterBrain.h.in and this script). G:/Tools/VRAudio/Family/core
mirrors both files for the family's working folder; --all checks that the mirror is the same.

The canonical file:
  - lines starting with //@@ are notes for the maintainer and are dropped;
  - @NAMESPACE@ and @PRODUCT@ become the product's namespace (ambient, additiv, granular);
  - a block between a line '// @@only <product>[,<product>...]' and a line '// @@end' is kept for the products named
    and dropped for the others (the two marker lines always go). Use it only where a BrainParams member or a read-out
    the others leave unused would change an instrument's behaviour or its random stream.

Usage:
  sync_brain.py --product ambient --out Core/include/ambient/ClusterBrain.h     write a copy
  sync_brain.py --check Core/include/ambient/ClusterBrain.h [--product ambient]  exit 1 and a short diff if it differs
  sync_brain.py --all [--root G:/Tools/VRAudio]                                  check the three copies and the mirror

A copy keeps the line endings it has (LF or CRLF); a new one is written with LF. The check compares text with the line
endings normalised, so a checkout that turned LF into CRLF does not fail it. Standard library only.
"""
import argparse
import difflib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CANONICAL = os.path.join(HERE, "ClusterBrain.h.in")
PRODUCTS = ("ambient", "additiv", "granular")
#: product -> (repository folder under the family root, the copy inside it)
COPIES = {
    "ambient": ("AmbientSynth", "Core/include/ambient/ClusterBrain.h"),
    "additiv": ("AdditivAmbientSynth", "Core/include/additiv/ClusterBrain.h"),
    "granular": ("GranularSynth", "Core/include/granular/ClusterBrain.h"),
}
HOME = ("AmbientSynth", "Tools/family")   # the home of record (Noctuary's repository)
MIRROR = ("Family", "core")               # the family folder's mirror
ONLY = re.compile(r"^\s*// @@only\s+([a-z, ]+?)\s*$")
END = re.compile(r"^\s*// @@end\s*$")


def read_text(path):
    """The file's text with LF line endings, and the line ending it had ("\\r\\n" or "\\n")."""
    with open(path, "rb") as f:
        data = f.read()
    eol = "\r\n" if b"\r\n" in data else "\n"
    return data.decode("utf-8").replace("\r\n", "\n"), eol


def generate(product, canonical=CANONICAL):
    """The copy for one product, LF line endings."""
    if product not in PRODUCTS:
        raise SystemExit(f"sync_brain: unknown product {product!r} (one of {', '.join(PRODUCTS)})")
    text, _ = read_text(canonical)
    out, keep, inside = [], True, False
    for n, line in enumerate(text.split("\n"), 1):
        if line.startswith("//@@"):
            continue
        m = ONLY.match(line)
        if m:
            if inside:
                raise SystemExit(f"sync_brain: {canonical}:{n}: @@only inside @@only")
            names = [s.strip() for s in m.group(1).split(",") if s.strip()]
            bad = [s for s in names if s not in PRODUCTS]
            if bad:
                raise SystemExit(f"sync_brain: {canonical}:{n}: unknown product {bad[0]!r}")
            inside, keep = True, product in names
            continue
        if END.match(line):
            if not inside:
                raise SystemExit(f"sync_brain: {canonical}:{n}: @@end without @@only")
            inside, keep = False, True
            continue
        if keep:
            out.append(line.replace("@NAMESPACE@", product).replace("@PRODUCT@", product))
    if inside:
        raise SystemExit(f"sync_brain: {canonical}: @@only without @@end")
    return "\n".join(out)


def product_of(path):
    """The product a copy belongs to, from its folder (.../include/<product>/ClusterBrain.h)."""
    parts = os.path.normpath(os.path.abspath(path)).replace("\\", "/").split("/")
    for p in reversed(parts):
        if p in PRODUCTS:
            return p
    raise SystemExit(f"sync_brain: cannot tell the product of {path}; pass --product")


def short_diff(want, have, name, limit=40):
    """A unified diff of at most `limit` lines, as text."""
    d = list(difflib.unified_diff(want.split("\n"), have.split("\n"), "generated", name, n=1, lineterm=""))
    more = len(d) - limit
    return "\n".join(d[:limit]) + (f"\n... {more} more diff lines" if more > 0 else "")


def check(path, product, canonical=CANONICAL, quiet=False):
    """True when the copy at `path` is what the sync writes; prints a short diff when not."""
    if not os.path.isfile(path):
        print(f"sync_brain: {path}: missing")
        return False
    have, _ = read_text(path)
    want = generate(product, canonical)
    if have == want:
        if not quiet:
            print(f"sync_brain: {path}: in sync with {canonical} ({product})")
        return True
    if quiet:
        hunks = sum(1 for d in difflib.unified_diff(want.split("\n"), have.split("\n"), n=0, lineterm="")
                    if d.startswith("@@"))
        print(f"  {product:9s} {path}: DIFFERS ({hunks} hunks; --check it for the diff)")
        return False
    print(f"sync_brain: {path} differs from what the sync writes for {product} -- edit the canonical file, never the copy:")
    print(f"  python {os.path.abspath(__file__)} --product {product} --out {path}")
    print(short_diff(want, have, path))
    return False


def write(path, product, canonical=CANONICAL):
    """Write the copy, keeping the line endings an existing file has."""
    eol = "\n"
    if os.path.isfile(path):
        _, eol = read_text(path)
    text = generate(product, canonical).replace("\n", eol)
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    print(f"sync_brain: wrote {path} ({product}, {'CRLF' if eol == chr(13) + chr(10) else 'LF'})")


def find_root():
    """The family folder: the nearest parent of this script that holds AmbientSynth/."""
    d = HERE
    while True:
        if os.path.isdir(os.path.join(d, HOME[0])):
            return d
        up = os.path.dirname(d)
        if up == d:
            return "G:/Tools/VRAudio"
        d = up


def check_all(root):
    """Check the three copies against the home's canonical file and the mirror against the home. Writes nothing."""
    home = os.path.normpath(os.path.join(root, *HOME))
    canonical = os.path.join(home, "ClusterBrain.h.in")
    ok = True
    print(f"canonical: {canonical}")
    for product, (repo, rel) in COPIES.items():
        path = os.path.normpath(os.path.join(root, repo, rel))
        if not os.path.isdir(os.path.join(root, repo)):
            print(f"  {product:9s} {repo}: no such repository here, skipped")
            continue
        good = check(path, product, canonical, quiet=True)
        if good:
            print(f"  {product:9s} {path}: in sync")
        ok &= good
    mirror = os.path.normpath(os.path.join(root, *MIRROR))
    for name in ("ClusterBrain.h.in", "sync_brain.py"):
        a, b = os.path.join(home, name), os.path.join(mirror, name)
        if not os.path.isfile(b):
            print(f"  mirror    {b}: missing")
            ok = False
            continue
        same = read_text(a)[0] == read_text(b)[0]
        print(f"  mirror    {b}: {'same as the home' if same else 'DIFFERS from ' + a}")
        ok &= same
    return ok


def main():
    """Command line: --out, --check or --all; exit status 1 when a check finds a difference."""
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--out", metavar="PATH", help="write the copy for --product to PATH")
    mode.add_argument("--check", metavar="PATH", help="exit 1 with a short diff when PATH differs from the sync's copy")
    mode.add_argument("--all", action="store_true", help="check the three known copies and the mirror, write nothing")
    ap.add_argument("--product", choices=PRODUCTS, help="ambient (Noctuary), additiv (Empyrean), granular (Zodiacal)")
    ap.add_argument("--canonical", default=CANONICAL, help="the canonical file (default: ClusterBrain.h.in beside this)")
    ap.add_argument("--root", default=None, help="with --all: the family folder holding the repositories")
    a = ap.parse_args()
    if a.out:
        if not a.product:
            ap.error("--out needs --product")
        write(a.out, a.product, a.canonical)
        return 0
    if a.check:
        return 0 if check(a.check, a.product or product_of(a.check), a.canonical) else 1
    return 0 if check_all(a.root or find_root()) else 1


if __name__ == "__main__":
    sys.exit(main())
