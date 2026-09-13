"""Upload the sample library (Textures, FieldRecordings) to a GitHub release as archives the installer
can take: AmbientSynth-library-<version>-partN.zip, each at most --max-part-mb (GitHub refuses a release
asset over 2 GiB). Files are stored as they are -- FLAC does not shrink any further -- under the paths
Textures/<name> and FieldRecordings/<name>, the layout the installer unpacks into the Noctuary folder.
A manifest in the shape make_content_pack.py writes (version, parts with name, bytes, sha256, files)
goes up last, so the installer's include can be generated from it.

Resumable and frugal with disk: the parts are planned once (sorted names, filled in order), then built,
uploaded, checked against the size GitHub reports and deleted, one at a time. The state file in the
staging folder records what is done; a rerun carries on and refuses to continue if the library changed.
The release is created as a draft: nothing is published until someone publishes it.

  python Tools/library/upload_library_assets.py --plan-only
  python Tools/library/upload_library_assets.py [--tag library-v5] [--version v5]
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LIB = os.path.join(ROOT, "Library")
FOLDERS = ("Textures", "FieldRecordings")
# The rest of the library, added after the recordings were already up (12.09.2026): the wavetables
# in their three shelves and the impulse responses, each table and each impulse with the .json
# beside it that describes it, and the credits files. Planned as their OWN group, which is what
# keeps the 51 archives already on the release byte for byte what they were -- filling the last of
# them up would have meant uploading ninety-four gigabytes again to add five.
#
# Both are ours to pass on: the wavetables are generated, apart from AKWF and WaveEdit, which are
# CC0; the impulses are generated apart from the Aula Carolina responses from the AIR database,
# whose licence permits distribution with the notice that Impulses/CREDITS.txt carries. Nothing
# from EchoThief is in the library -- that material was for developing the convolver and stays here.
EXTRA_FOLDERS = ("Wavetables", "Impulses")
# And the archive (13.09.2026): the recordings the near layer plays straight -- NASA's mission
# loops, the sounds from beyond and the sonifications, works of the United States government and
# free of copyright, fetched by Tools/library/fetch_archive.py with every source written into
# Archive/SOURCES.md, which travels with them. A third group of its own, for the same reason as
# the second: the fifty-four archives already on the release stay exactly what they are.
ARCHIVE_FOLDERS = ("Archive",)
KEEP = (".flac", ".wav", ".json", ".txt", ".md")
REPO = "reneweller-coding/Noctuary"
DEFAULT_STAGE = os.path.normpath(os.path.join(ROOT, "..", "Noctuary-Upload"))


def _walk(kind, audio_only):
    """Every file of a library folder, as [kind, relative path, bytes], deepest paths included."""
    out = []
    root = os.path.join(LIB, kind)
    for base, dirs, names in os.walk(root):
        dirs.sort()
        for name in sorted(names):
            if not name.lower().endswith((".flac", ".wav") if audio_only else KEEP):
                continue
            path = os.path.join(base, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            out.append([kind, rel, os.path.getsize(path)])
    return out


def _pack(files, max_bytes):
    parts, current, size = [], [], 0
    for entry in files:
        # zip headers cost about 100 bytes plus twice the name per file; keep a margin for them
        cost = entry[2] + 2 * len(entry[1]) + 256
        if current and size + cost > max_bytes:
            parts.append(current)
            current, size = [], 0
        current.append(entry)
        size += cost
    if current:
        parts.append(current)
    return parts


# The archive's subfolders in the order their groups went up. A new subfolder is appended after
# these (in name order); a name sorted before NASA/ -- LoC/ was -- must not move into the first
# group, whose part is on the release.
ARCHIVE_ORDER = ("NASA", "Radio", "LoC")


def archive_groups():
    """The archive's files in groups that keep an uploaded part what it was: the root files and
    the first subfolder together (that is part 55 as it went up: SOURCES.md and NASA/), and every
    later subfolder (Radio/, LoC/, ...) as a group of its own, appended in ARCHIVE_ORDER. One
    group for the whole folder would have packed a new subfolder into the part that already
    exists, and name order would have put LoC/ in front of NASA/."""
    roots, subs = [], {}
    for kind in ARCHIVE_FOLDERS:
        for e in _walk(kind, audio_only=False):
            top = e[1].split("/")[0] if "/" in e[1] else None
            if top is None:
                roots.append(e)
            else:
                subs.setdefault(top, []).append(e)
    order = [s for s in ARCHIVE_ORDER if s in subs] + sorted(s for s in subs if s not in ARCHIVE_ORDER)
    groups = []
    for i, s in enumerate(order):
        groups.append((roots if i == 0 else []) + subs[s])
    if not groups and roots:
        groups.append(roots)
    return groups


def plan(version, max_bytes, extras=True):
    # The recordings keep their own packing, unchanged, or every archive after the first added file
    # would have different contents and a different hash.
    first = []
    for kind in FOLDERS:
        first += _walk(kind, audio_only=True)
    second = []
    parts = _pack(first, max_bytes)
    if extras:
        for kind in EXTRA_FOLDERS:
            second += _walk(kind, audio_only=False)
        parts += _pack(second, max_bytes)
        for group in archive_groups():
            parts += _pack(group, max_bytes)
    return [{"name": f"AmbientSynth-library-{version}-part{i + 1}.zip", "files": p} for i, p in enumerate(parts)]


def gh(*args, check=True):
    r = subprocess.run(["gh", *args, "-R", REPO], capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        raise RuntimeError(f"gh {' '.join(args)} failed: {r.stderr.strip()}")
    return r


def remote_sizes(tag):
    out = gh("release", "view", tag, "--json", "assets").stdout
    return {a["name"]: a["size"] for a in json.loads(out)["assets"]}


def ensure_release(tag, version, summary):
    if gh("release", "view", tag, "--json", "name", check=False).returncode == 0:
        return
    notes = (f"Sample library {version}: {summary}. Draft, not published: the preset packs that use it are "
             "being rebuilt. Each archive unpacks into the Noctuary folder (Textures/, FieldRecordings/, "
             "Wavetables/, Impulses/, Archive/); "
             f"AmbientSynth-library-{version}-manifest.json lists every part with its SHA-256 and files.")
    gh("release", "create", tag, "--draft", "--title", f"Noctuary sample library {version}", "--notes", notes)


def build(part, path):
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_STORED, allowZip64=True) as z:
        for kind, name, _ in part["files"]:
            z.write(os.path.join(LIB, kind, name), arcname=f"{kind}/{name}")
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return os.path.getsize(path), h.hexdigest()


def upload(tag, path):
    for attempt in range(1, 5):
        r = gh("release", "upload", tag, path, "--clobber", check=False)
        if r.returncode == 0:
            return
        print(f"  upload attempt {attempt} failed: {r.stderr.strip()[:300]}", flush=True)
        time.sleep(60 * attempt)
    raise RuntimeError(f"upload of {os.path.basename(path)} failed four times")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tag", default="library-v5")
    ap.add_argument("--version", default="v5")
    ap.add_argument("--max-part-mb", type=int, default=1800)
    ap.add_argument("--stage", default=DEFAULT_STAGE)
    ap.add_argument("--plan-only", action="store_true")
    ap.add_argument("--no-extras", action="store_true",
                    help="only the recordings, the way the first upload ran (Wavetables and Impulses left out)")
    a = ap.parse_args()

    parts = plan(a.version, a.max_part_mb * 1024 * 1024, extras=not a.no_extras)
    kinds = FOLDERS if a.no_extras else FOLDERS + EXTRA_FOLDERS + ARCHIVE_FOLDERS
    counts = {k: sum(1 for p in parts for f in p["files"] if f[0] == k) for k in kinds}
    total = sum(f[2] for p in parts for f in p["files"])
    summary = ", ".join(f"{n} {k}" for k, n in counts.items()) + f", {total / 1e9:.1f} GB in {len(parts)} archives"
    print(summary)
    if a.plan_only:
        for p in parts[:3] + parts[-2:]:
            print(f"  {p['name']}: {len(p['files'])} files, {sum(f[2] for f in p['files']) / 2**20:.0f} MiB, "
                  f"{p['files'][0][0]}/{p['files'][0][1]} .. {p['files'][-1][0]}/{p['files'][-1][1]}")
        return

    os.makedirs(a.stage, exist_ok=True)
    state_path = os.path.join(a.stage, f"upload-{a.version}.json")
    if os.path.exists(state_path):
        with open(state_path, encoding="utf-8") as f:
            state = json.load(f)
        # A run may ADD archives -- the wavetables and the impulses went up after the recordings --
        # but it may never change one that is already on the release, because the manifest a
        # downloaded installer carries names it by its hash. So the old plan has to be a prefix of
        # the new one: same archives, same files, in the same order, with more after them.
        # Compared by name, not by size: a text file beside the recordings (Archive/SOURCES.md)
        # grows with every addition to the archive, and the copy in the part that already went up
        # stays what it was -- the current one is in git. The audio is never rewritten in place.
        old = [[f[:2] for f in p["files"]] for p in state["parts"]]
        now = [[f[:2] for f in p["files"]] for p in parts]
        if old != now[:len(old)]:
            sys.exit(f"the library changed since the upload began ({state_path}); not mixing two states")
        if len(now) > len(old):
            print(f"{len(now) - len(old)} archives to add to the {len(old)} already on the release")
            state["parts"] = parts
    else:
        state = {"tag": a.tag, "version": a.version, "parts": parts, "done": {}}

    def save():
        with open(state_path, "w", encoding="utf-8") as f:
            json.dump(state, f, indent=1)

    save()
    ensure_release(a.tag, a.version, summary)
    for i, part in enumerate(state["parts"], 1):
        name = part["name"]
        if name in state["done"]:
            continue
        path = os.path.join(a.stage, name)
        t0 = time.time()
        size, digest = build(part, path)
        t1 = time.time()
        upload(a.tag, path)
        t2 = time.time()
        remote = remote_sizes(a.tag).get(name)
        if remote != size:
            sys.exit(f"{name}: GitHub reports {remote} bytes, the archive has {size}; stopping with it kept at {path}")
        os.remove(path)
        state["done"][name] = {"bytes": size, "sha256": digest, "uploaded": time.strftime("%Y-%m-%d %H:%M:%S")}
        save()
        print(f"[{i}/{len(state['parts'])}] {name}: {size / 2**30:.2f} GiB, built {t1 - t0:.0f} s, "
              f"uploaded {t2 - t1:.0f} s ({size * 8 / 1e6 / max(t2 - t1, 1e-3):.0f} Mbit/s)", flush=True)

    manifest = {"version": a.version, "kind": "library", "folders": list(kinds),
                "parts": [{"name": p["name"], "bytes": state["done"][p["name"]]["bytes"],
                           "sha256": state["done"][p["name"]]["sha256"],
                           "files": [f"{k}/{n}" for k, n, _ in p["files"]]} for p in state["parts"]]}
    mpath = os.path.join(a.stage, f"AmbientSynth-library-{a.version}-manifest.json")
    with open(mpath, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    upload(a.tag, mpath)
    print(f"done: {len(state['parts'])} archives and the manifest on release {a.tag} (draft)")


if __name__ == "__main__":
    main()
