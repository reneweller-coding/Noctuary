"""Noctuary -- build the downloadable content package: the media the preset packs name.

The 56 packs in Library/Packs reference samples, wavetables and impulse responses by relative
path. They are far too big for git and are not in it; this makes the archives that the installer
downloads instead, and the manifest the installer needs to verify them.

    python Tools/make_content_pack.py                  # build everything into Deploy/content
    python Tools/make_content_pack.py --check-only     # just say what would go in and how big

Two things happen on the way in.

Only what is referenced travels. The library folder holds more than the packs use (unreferenced
textures from earlier generation runs, the .txt prompts beside each sample); shipping those would
add gigabytes nobody's preset asks for.

The samples are generated as 32-bit float and are converted to 24-bit PCM, which is a quarter off
the size for headroom no texture has: they are normalised material well inside +-1, and 24 bits
put the quantisation floor at -144 dBFS, far below anything the instrument's own arithmetic
contributes. Any file that turns out to peak above full scale is left as float rather than
clipped, and said so. The core's WAV reader takes 8/16/24/32-bit PCM and 32-bit float alike
(Core/src/WavFile.cpp), so nothing has to change to read them.
"""
import argparse
import time
import concurrent.futures
import hashlib
import json
import os
import posixpath
import struct
import sys
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
LIBRARY = os.path.join(ROOT, "Library")
PACKS = os.path.join(ROOT, "Library", "Packs")
OUT = os.path.join(ROOT, "Deploy", "content")
# Five now: the field recordings sit apart from the tonal material, because a swamp and a
# bowed cymbal are not the same kind of clip and only one of them may be transposed; and the
# archive holds the recordings the near layer plays straight (NASA's loops and sonifications,
# public domain, see Library/Archive/SOURCES.md), named by the near bank rather than by a pack.
KINDS = ("Textures", "FieldRecordings", "Wavetables", "Impulses", "Archive")
LEGACY = os.path.join(HERE, "library", "legacy_impulses.json")
NEAR_BANK = os.path.join(ROOT, "Core", "src", "NearPresets.inc")


def referenced():
    """Every media file the packs name, as (kind, path inside that folder) -> absolute source path."""
    want = {}
    # The near bank is compiled in, not a pack: its third field is a clip relative to the library's
    # root (Archive/NASA/.../x.flac), which is where resolveLibraryFile looks for it.
    if os.path.exists(NEAR_BANK):
        import re
        with open(NEAR_BANK, encoding="utf-8") as f:
            for field in re.findall(r'"((?:%s)/[^"]+)"' % "|".join(KINDS), f.read()):
                src = os.path.normpath(os.path.join(LIBRARY, field))
                kind, _, rest = field.partition("/")
                if os.path.isdir(src):
                    # A folder of recordings (the near layer plays one of them per event): all of it travels.
                    for base, _, names in os.walk(src):
                        for name in sorted(names):
                            if name.lower().endswith((".wav", ".flac")):
                                p = os.path.join(base, name)
                                want[(kind, os.path.relpath(p, os.path.join(LIBRARY, kind)).replace(os.sep, "/"))] = p
                elif field.lower().endswith((".wav", ".flac")):
                    want[(kind, rest)] = src
    for name in sorted(os.listdir(PACKS)):
        if not name.endswith(".ambientpack"):
            continue
        with open(os.path.join(PACKS, name), encoding="utf-8", errors="replace") as f:
            for line in f:
                t = line.strip()
                if not t or t.startswith("#"):
                    continue
                for field in t.split("|"):
                    # a texture field may carry up to four clips, one per slot, separated by ';'
                    for field in (x.strip() for x in field.split(";")):
                        # .flac as well as .wav: the clip library has been FLAC since it was
                        # regenerated, and a check for .wav alone quietly left every sample out of
                        # the package -- the tables and the impulse responses would have travelled
                        # alone and 14336 presets would have loaded nothing.
                        if not field.lower().endswith((".wav", ".flac")):
                            continue
                        src = os.path.normpath(os.path.join(PACKS, field))
                        # The kind is the library folder the file lies under, and what follows it is
                        # kept: the wavetables sit on shelves (Wavetables/Harmonic/x.wav, see
                        # Tools/library/wavetable_folders.py) and travel on them.
                        try:
                            rel = os.path.relpath(src, LIBRARY).replace(os.sep, "/").split("/")
                        except ValueError:          # another drive
                            rel = []
                        if len(rel) < 2 or rel[0] not in KINDS:
                            print("  ignored (not a library folder): %s" % field)
                            continue
                        want[(rel[0], "/".join(rel[1:]))] = src
    return want


def legacy():
    """The impulse files earlier packs named, shipped whether or not a current pack still names them. Sessions
    and user presets keep the absolute path of their room, and one reopened without the file plays whatever
    room was loaded before -- so these stay, under their names (Tools/library/legacy_impulses.py)."""
    if not os.path.exists(LEGACY):
        return {}
    with open(LEGACY, encoding="utf-8") as f:
        files = json.load(f).get("files", {})
    out = {}
    for name, info in files.items():
        src = info.get("source", "")
        out[("Impulses", name)] = src if os.path.isabs(src) else os.path.normpath(os.path.join(ROOT, src))
    return out


def read_wav(path):
    """(format tag, channels, rate, bits, raw data bytes) -- the file as it is, no conversion."""
    with open(path, "rb") as f:
        head = f.read(12)
        if head[:4] != b"RIFF" or head[8:12] != b"WAVE":
            return None
        fmt = None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                break
            cid, size = hdr[:4], struct.unpack("<I", hdr[4:8])[0]
            body = f.read(size + (size & 1))[:size]
            if cid == b"fmt ":
                tag, ch, rate, _, _, bits = struct.unpack("<HHIIHH", body[:16])
                if tag == 0xFFFE and len(body) >= 40:
                    tag = struct.unpack("<H", body[24:26])[0]
                fmt = (tag, ch, rate, bits)
            elif cid == b"data" and fmt is not None:
                return fmt + (body,)
    return None


def to_flac(path):
    """(bytes, name, why) -- the file as 24-bit FLAC, under its own name.

    FLAC is lossless: the same samples to the bit, in roughly 40 % of the float original against
    the 75 % that 24-bit PCM costs. Measured on this library it takes the download from 15.4 GB to
    8.4 GB. The core reads it without a framework (dr_flac in Core/src/WavFile.cpp), and a preset
    that names "x.wav" finds "x.flac" beside it, so nothing in any pack has to change.
    """
    try:
        import soundfile as sf
    except ImportError:
        return None, None, "no soundfile: run this with Tools/TextureGen/.venv"
    try:
        x, rate = sf.read(path, dtype="float32", always_2d=True)
    except Exception:
        return None, None, "unreadable"
    if x.size == 0:
        return None, None, "empty"
    import io
    buf = io.BytesIO()
    try:
        # PCM_24 for material that came in as float, and the file's own depth when it was already
        # integer -- a 16-bit wavetable gains nothing from being written as 24.
        info = sf.info(path)
        sub = "PCM_16" if info.subtype == "PCM_16" else "PCM_24"
        sf.write(buf, x, rate, format="FLAC", subtype=sub)
    except Exception as e:
        return None, None, "flac failed: %s" % type(e).__name__
    return buf.getvalue(), os.path.splitext(os.path.basename(path))[0] + ".flac", "flac"


def to_24bit(path):
    """(bytes, why) -- the file as 24-bit PCM, or (None, why) when it is left exactly as it is.

    `why` is one of "converted", "already pcm" (the wavetables are 16-bit and the impulse
    responses 24-bit already) or "above full scale" (a float file that would clip, and is
    therefore not touched). Three separate answers, because reporting them as one number said
    that four hundred files would have clipped when in fact none of them would.
    """
    got = read_wav(path)
    if got is None:
        return None, "unreadable"
    tag, ch, rate, bits, data = got
    if tag != 3 or bits != 32:
        return None, "already pcm"
    n = len(data) // 4
    samples = np.frombuffer(data[:n * 4], dtype="<f4")
    if samples.size == 0 or float(np.max(np.abs(samples))) > 1.0:
        return None, "above full scale"    # louder than full scale: leave it alone, do not clip
    # Round to nearest rather than truncate: truncation is a DC-biased error, and on quiet
    # material that bias is the one thing a listener could actually hear.
    ints = np.rint(samples.astype(np.float64) * 8388607.0).astype(np.int32)
    np.clip(ints, -8388608, 8388607, out=ints)
    out = ints.astype("<i4").view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    block = ch * 3
    header = b"RIFF" + struct.pack("<I", 36 + len(out)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, ch, rate, rate * block, block, 24) + b"data" + struct.pack("<I", len(out))
    return header + bytes(out), "converted"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_installer_include(manifest):
    """The [Files] lines for the installer, generated rather than kept by hand: the sizes and the
    hashes change with every rebuild of the package, and a hash that does not match what is on the
    release is a download that fails at the very last moment. Included by Deploy/Noctuary.iss."""
    total = sum(p["bytes"] for p in manifest["parts"])
    nl, cont = chr(10), chr(92)          # a line ending, and Inno's line-continuation backslash
    head = ("; Generated by Tools/make_content_pack.py -- do not edit." + nl
            + "; Content package %s: %d archives, %.2f GB." % (
                manifest["version"], len(manifest["parts"]), total / 1e9) + nl)

    # Two includes rather than one, because Pascal Script wants a procedure declared before the
    # line that calls it: the code goes near the top of [Code], the file list into [Files].
    files = os.path.join(ROOT, "Deploy", "content-files.iss")
    with open(files, "w", encoding="utf-8") as f:
        f.write(head)
        for p in manifest["parts"]:
            f.write('Source: "{tmp}%s%s"; DestDir: "{code:LibDir}"; Check: ContentDownloaded; %s%s'
                    '    Flags: external extractarchive recursesubdirs ignoreversion%s'
                    % (cont, p["name"], cont, nl, nl))

    code = os.path.join(ROOT, "Deploy", "content-code.iss")
    with open(code, "w", encoding="utf-8") as f:
        # Inside [Code] a comment is Pascal's "//", not the script's ";" -- a header in the wrong
        # dialect is a compile error on line 1, column 1.
        f.write(head.replace("; ", "// "))
        f.write("procedure AddContentDownloads(Page: TDownloadWizardPage);" + nl + "begin" + nl)
        for p in manifest["parts"]:
            f.write("  Page.Add('{#ContentBaseUrl}/%s', '%s', '%s');%s"
                    % (p["name"], p["name"], p["sha256"], nl))
        f.write("end;" + nl)

    with open(os.path.join(ROOT, "Deploy", "content-size.txt"), "w", encoding="utf-8") as f:
        f.write("%.1f" % (total / 1e9))
    print("installer includes: %s and %s (%d archives, %.2f GB)"
          % (os.path.basename(files), os.path.basename(code), len(manifest["parts"]), total / 1e9))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="v1", help="content package version, part of every name")
    ap.add_argument("--max-part-mb", type=int, default=1800,
                    help="largest archive to write; GitHub refuses a release asset over 2 GB")
    ap.add_argument("--check-only", action="store_true")
    ap.add_argument("--jobs", type=int, default=12,
                    help="files encoded at a time; libsndfile releases the lock, so these are real cores")
    ap.add_argument("--wav", action="store_true",
                    help="ship 24-bit WAV as before instead of FLAC (twice the download)")
    ap.add_argument("--add-new", action="store_true",
                    help="pack only the files the existing manifest does not have, as a new part")
    ap.add_argument("--emit-only", action="store_true",
                    help="regenerate the installer's include from an existing manifest, no rebuild")
    a = ap.parse_args()

    if a.emit_only:
        with open(os.path.join(OUT, "content-manifest.json"), encoding="utf-8") as f:
            write_installer_include(json.load(f))
        return

    want = referenced()
    for key, src in legacy().items():      # what a current pack names wins; the rest of the old rooms come along
        want.setdefault(key, src)
    if a.add_new:
        # Adding presets to the library adds a handful of samples to what it needs. Repacking
        # three gigabytes to ship forty megabytes of them would be silly: this writes the
        # difference as one more part and appends it to the manifest, so the installer downloads
        # what it already downloaded plus the new one.
        import zipfile as zf
        with open(os.path.join(OUT, "content-manifest.json"), encoding="utf-8") as f:
            man = json.load(f)
        # Compared by kind and path without the extension: the archives hold x.flac where the
        # pack says x.wav, and a comparison of whole names found every file new.
        have = set()
        for p in man["parts"]:
            with zf.ZipFile(os.path.join(OUT, p["name"])) as z:
                for n in z.namelist():
                    kind, _, rest = n.partition("/")
                    have.add((kind, os.path.splitext(rest)[0]))
        new = {k: v for k, v in want.items() if (k[0], os.path.splitext(k[1])[0]) not in have}
        print("%d files already packed, %d new" % (len(have), len(new)))
        if not new:
            write_installer_include(man)
            return
        want = new
    kinds = {k: 0 for k in KINDS}
    total = 0
    for (kind, _), src in want.items():
        if not os.path.isfile(src):
            sys.exit("missing: %s" % src)
        kinds[kind] += 1
        total += os.path.getsize(src)
    print("referenced: %d files, %.2f GB" % (len(want), total / 1e9))
    for k in KINDS:
        print("  %-12s %5d" % (k, kinds[k]))
    if a.check_only:
        return

    os.makedirs(OUT, exist_ok=True)
    items = sorted(want.items())
    for kind in {k for (k, _), _ in items}:
        os.makedirs(os.path.join(OUT, kind), exist_ok=True)

    # Encoding five thousand files is the long half of this, it is the same work five thousand
    # times over, and libsndfile lets go of the interpreter lock while it does it -- so threads
    # really do run side by side here. Twelve of the machine's twenty-four, which leaves it usable.
    def convert(item):
        (kind, name), src = item
        if src.lower().endswith(".flac"):
            # Already FLAC -- copied through, bit for bit. Re-encoding seven thousand files into the
            # format they are already in costs an hour and changes nothing.
            with open(src, "rb") as f:
                conv = f.read()
            out_name, why = posixpath.basename(name), "already flac"
        else:
            conv, out_name, why = (None, None, "") if a.wav else to_flac(src)
        if conv is None:
            out_name = posixpath.basename(name)
            conv, why = to_24bit(src)
            if conv is None:
                with open(src, "rb") as f:
                    conv = f.read()
        out_name = posixpath.join(posixpath.dirname(name), out_name)    # the shelf comes along
        dst = os.path.join(OUT, kind, *out_name.split("/"))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(conv)
        return kind, out_name, dst, why, os.path.getsize(src) - len(conv)

    staged, why_count, saved = [], {}, 0
    done = 0
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
        for kind, out_name, dst, why, gain in ex.map(convert, items):
            why_count[why] = why_count.get(why, 0) + 1
            saved += gain
            staged.append((kind, out_name, dst))
            done += 1
            if done % 200 == 0:
                el = time.time() - t0
                print("  %d/%d  %.0f s, noch etwa %.0f s"
                      % (done, len(items), el, el / done * (len(items) - done)), flush=True)
    staged.sort()
    print("%s -- saved %.2f GB" % (", ".join("%s: %d" % kv for kv in sorted(why_count.items())), saved / 1e9))

    # ---------------------------------------------------------------- archives
    # Deflated at level 6. Measured on the converted samples: stored is the full size, level 6
    # gets to 85 %, level 9 also gets to 85 % and takes no longer -- so 6, and the last 15 % of
    # three gigabytes is worth the inflating at the other end.
    limit = a.max_part_mb * 1024 * 1024
    first_part = 1
    if a.add_new:
        with open(os.path.join(OUT, "content-manifest.json"), encoding="utf-8") as f:
            first_part = len(json.load(f)["parts"]) + 1
    parts, part, size = [], [], 0
    for kind, name, path in staged:
        n = os.path.getsize(path)
        if part and size + n > limit:
            parts.append(part)
            part, size = [], 0
        part.append((kind, name, path))
        size += n
    if part:
        parts.append(part)

    manifest = {"version": a.version, "parts": []}
    if a.add_new:
        with open(os.path.join(OUT, "content-manifest.json"), encoding="utf-8") as f:
            manifest = json.load(f)
    for idx, part in enumerate(parts, first_part):
        zip_name = "Noctuary-content-%s-part%d.zip" % (a.version, idx)
        zip_path = os.path.join(OUT, zip_name)
        print("writing %s (%d files)" % (zip_name, len(part)), flush=True)
        with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, allowZip64=True, compresslevel=6) as z:
            for kind, name, path in part:
                z.write(path, "%s/%s" % (kind, name))
        manifest["parts"].append({"name": zip_name, "bytes": os.path.getsize(zip_path),
                                  "sha256": sha256(zip_path), "files": len(part)})
        print("  %.2f GB" % (os.path.getsize(zip_path) / 1e9))

    with open(os.path.join(OUT, "content-manifest.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
    print("manifest: %s" % os.path.join(OUT, "content-manifest.json"))

    write_installer_include(manifest)
    for p in manifest["parts"]:
        print("  %s  %.2f GB  %s" % (p["name"], p["bytes"] / 1e9, p["sha256"][:16] + "..."))


if __name__ == "__main__":
    main()
