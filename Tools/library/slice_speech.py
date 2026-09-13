"""Noctuary -- phrases out of old radio, for the near layer.

Takes an item of the Internet Archive (or recordings on disk), cuts the speech at its pauses into
phrases of two to six seconds, gives them the sound of the set they came out of, and writes them
into the library's archive -- where a near preset names the folder and plays one of them, chosen
at random, every few minutes, close to the ear:

    python Tools/library/slice_speech.py --item Quiet_Please --list             # what the item holds
    python Tools/library/slice_speech.py --item Quiet_Please --episodes 12 --per-episode 10
    python Tools/library/slice_speech.py --file "a recording.mp3" --out Radio/Test
    python Tools/library/slice_speech.py --item Quiet_Please --episodes 2 --dry-run   # the cuts, no files

Nobody cuts thirty hours of radio by hand, and nothing here listens: the cuts come from the
envelope. A frame of ten milliseconds is speech when it stands twelve decibels over the
recording's own floor (the 15th percentile of all its frames -- the hiss between words, which
every transfer of a 1947 transcription disc has); a pause is three hundred milliseconds of
frames that do not; what lies between two pauses is a phrase. Phrases under two seconds are
joined to the one that follows when the gap allows it, phrases over six are cut at the quietest
frame inside their window, and what is left is judged by its modulation: speech moves at the
rate of syllables, three to nine a second, and a segment whose envelope does not is the organ
sting, the theme, the closing chord -- kept out, because a near preset that plays a bar of the
theme where a voice was wanted is the wrong kind of surprise.

The sound: a band of 350 to 3200 Hz, which is what a receiver of the time passed and what makes
a voice read as coming out of one rather than out of the room; a little saturation; the set's
own hiss under it; and, unless told not to, the breath of squelch after the phrase, the sound of
a carrier dropping. --quindar puts the tones of the Apollo network around it instead, 2525 Hz to
open and 2475 Hz to close, for material that came off that loop.

Rights are not something a script can check. What it can do is write down where every file came
from and what the item claims for itself (Archive/SOURCES.md), and refuse an item whose licence
tag forbids derivatives -- a phrase cut out of a recording is one -- unless --anyway says the
matter has been looked at.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request

import numpy as np
from scipy.signal import butter, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ARCHIVE = os.path.join(ROOT, "Library", "Archive")
WORK = os.path.join(ROOT, "build", "speech-work")
FFMPEG = os.environ.get("AMBIENT_FFMPEG") or "ffmpeg"
for cand in (r"C:\Anw\Tools\ffmpeg\bin\ffmpeg.exe",):
    if FFMPEG == "ffmpeg" and os.path.exists(cand):
        FFMPEG = cand
SR = 48000
AGENT = "Noctuary/2.0 (library tool; +https://github.com/reneweller-coding/Noctuary)"
AUDIO_FORMATS = ("VBR MP3", "MP3", "128Kbps MP3", "64Kbps MP3", "Flac", "FLAC", "WAVE", "Ogg Vorbis")


# ------------------------------------------------------------------ the Internet Archive
def metadata(item):
    req = urllib.request.Request("https://archive.org/metadata/%s" % item, headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def audio_files(meta):
    """The item's recordings, one per episode, best format first where an episode has several."""
    rank = {f: i for i, f in enumerate(("FLAC", "Flac", "WAVE", "VBR MP3", "128Kbps MP3", "MP3", "64Kbps MP3", "Ogg Vorbis"))}
    files = [f for f in meta.get("files", []) if f.get("format") in AUDIO_FORMATS]
    best = {}
    for f in files:
        stem = os.path.splitext(f["name"])[0]
        if stem not in best or rank.get(f["format"], 99) < rank.get(best[stem]["format"], 99):
            best[stem] = f
    return [best[k] for k in sorted(best)]


def spread(items, n):
    """n of them, evenly across the run -- the first, the last and what lies between, so a series
    is sampled across its years rather than from its first month."""
    if n <= 0 or n >= len(items):
        return list(items)
    if n == 1:
        return [items[len(items) // 2]]
    return [items[round(i * (len(items) - 1) / (n - 1))] for i in range(n)]


def download(item, name, dest, size):
    if os.path.exists(dest) and (size <= 0 or os.path.getsize(dest) == size):
        return dest
    url = "https://archive.org/download/%s/%s" % (item, urllib.request.quote(name))
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    tmp = dest + ".part"
    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, "wb") as f:
        for chunk in iter(lambda: r.read(1 << 20), b""):
            f.write(chunk)
    os.replace(tmp, dest)
    return dest


# ------------------------------------------------------------------ audio in and out
def decode(path, sr=SR):
    """The recording as mono float at sr, through ffmpeg: MP3, Ogg, FLAC, WAV, or a film's soundtrack."""
    cmd = [FFMPEG, "-v", "error", "-i", path, "-vn", "-ac", "1", "-ar", str(sr), "-f", "f32le", "-"]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0 or not r.stdout:
        raise RuntimeError("ffmpeg could not read %s: %s" % (path, r.stderr.decode(errors="replace")[-300:]))
    return np.frombuffer(r.stdout, dtype="<f4").astype(np.float64)


def write_flac(y, sr, path):
    """24-bit FLAC, through ffmpeg from a float pipe."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cmd = [FFMPEG, "-v", "error", "-y", "-f", "f32le", "-ar", str(sr), "-ac", "1", "-i", "-",
           "-c:a", "flac", "-sample_fmt", "s32", "-bits_per_raw_sample", "24", path]
    r = subprocess.run(cmd, input=np.clip(y, -1.0, 1.0).astype("<f4").tobytes(), capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("ffmpeg could not write %s: %s" % (path, r.stderr.decode(errors="replace")[-300:]))


# ------------------------------------------------------------------ the cuts
HOP = 0.01   # seconds per envelope frame


def envelope_db(x, sr):
    n = int(sr * HOP)
    m = len(x) // n
    if m == 0:
        return np.zeros(0)
    frames = x[:m * n].reshape(m, n)
    rms = np.sqrt(np.mean(frames * frames, axis=1)) + 1e-9
    return 20.0 * np.log10(rms)


def runs_of(mask):
    """(start, end) frame pairs of every run of True."""
    out, start = [], None
    for i, v in enumerate(mask):
        if v and start is None:
            start = i
        elif not v and start is not None:
            out.append((start, i)); start = None
    if start is not None:
        out.append((start, len(mask)))
    return out


def syllable_share(env_db, lo=3.0, hi=9.0):
    """How much of the envelope's movement lies at the rate of syllables: the power of the
    (detrended) envelope between lo and hi Hz over its power from 0.5 Hz up. Speech sits around
    0.4 and above; a held organ chord or a sustained theme well under 0.2."""
    e = env_db - np.mean(env_db)
    if len(e) < 50:
        return 0.0
    e = e * np.hanning(len(e))
    spec = np.abs(np.fft.rfft(e)) ** 2
    freqs = np.fft.rfftfreq(len(e), d=HOP)
    total = np.sum(spec[freqs >= 0.5]) + 1e-12
    band = np.sum(spec[(freqs >= lo) & (freqs <= hi)])
    return float(band / total)


def centroid_hz(seg, sr):
    spec = np.abs(np.fft.rfft(seg * np.hanning(len(seg)))) ** 2
    freqs = np.fft.rfftfreq(len(seg), d=1.0 / sr)
    return float(np.sum(freqs * spec) / (np.sum(spec) + 1e-12))


def find_phrases(x, sr, min_s=2.0, max_s=6.0, pause_s=0.3, over_db=12.0):
    """[(start, end, why)] in seconds: the phrases kept, and, for the record, what was dropped."""
    env = envelope_db(x, sr)
    if len(env) == 0:
        return [], []
    floor = float(np.percentile(env, 15))
    thr = max(floor + over_db, -48.0)
    active = env > thr
    # A pause has to last: gaps shorter than pause_s are the space between two words, closed.
    pause = int(round(pause_s / HOP))
    segs = []
    for a, b in runs_of(active):
        if segs and a - segs[-1][1] < pause:
            segs[-1] = (segs[-1][0], b)
        else:
            segs.append((a, b))
    min_f, max_f = int(round(min_s / HOP)), int(round(max_s / HOP))
    # Short ones join the phrase after them, when the two together still fit.
    joined = []
    for a, b in segs:
        if joined and joined[-1][1] - joined[-1][0] < min_f and a - joined[-1][1] < int(0.8 / HOP) \
                and b - joined[-1][0] <= max_f:
            joined[-1] = (joined[-1][0], b)
        else:
            joined.append((a, b))
    # Long ones are cut at their quietest frame inside the window.
    cut = []
    for a, b in joined:
        while b - a > max_f:
            lo, hi = a + min_f, a + max_f
            k = lo + int(np.argmin(env[lo:hi]))
            cut.append((a, k)); a = k
        cut.append((a, b))
    kept, dropped = [], []
    for a, b in cut:
        if b - a < min_f:
            dropped.append((a * HOP, b * HOP, "short")); continue
        seg = x[a * int(sr * HOP): b * int(sr * HOP)]
        share = syllable_share(env[a:b])
        cen = centroid_hz(seg, sr)
        if share < 0.28:
            dropped.append((a * HOP, b * HOP, "no syllables (%.2f)" % share)); continue
        if not 250.0 <= cen <= 3200.0:
            dropped.append((a * HOP, b * HOP, "centroid %.0f Hz" % cen)); continue
        kept.append((a * HOP, b * HOP, "%.2f / %.0f Hz" % (share, cen)))
    return kept, dropped


# ------------------------------------------------------------------ the sound of the set
def vintage(seg, sr, rng, squelch=True, quindar=False, hiss_db=-48.0, tail_db=-30.0):
    band = butter(2, [350.0, 3200.0], btype="bandpass", fs=sr, output="sos")
    y = sosfilt(band, seg)
    # Level first, then the saturation, so every phrase is bent by the same amount.
    rms = np.sqrt(np.mean(y * y)) + 1e-9
    y = y * (10 ** (-20.0 / 20.0) / rms)
    y = np.tanh(1.5 * y) / np.tanh(1.5)
    # The set's own noise, in the same band, under everything.
    hiss = sosfilt(band, rng.standard_normal(len(y)))
    hiss *= 10 ** (hiss_db / 20.0) / (np.sqrt(np.mean(hiss * hiss)) + 1e-9)
    y = y + hiss
    fade = int(0.02 * sr)
    y[:fade] *= np.linspace(0.0, 1.0, fade)
    y[-fade:] *= np.linspace(1.0, 0.0, fade)
    if quindar:
        # 250 ms tones at -20 dB, a hair of silence between tone and voice, as on the loop.
        gap = np.zeros(int(0.06 * sr))
        def tone(hz):
            t = np.arange(int(0.25 * sr)) / sr
            w = np.sin(2 * np.pi * hz * t) * 10 ** (-20.0 / 20.0)
            e = int(0.005 * sr)
            w[:e] *= np.linspace(0, 1, e); w[-e:] *= np.linspace(1, 0, e)
            return w
        y = np.concatenate([tone(2525.0), gap, y, gap, tone(2475.0)])
    elif squelch:
        # The carrier dropping: a burst of the band's noise, gone in a hundred milliseconds.
        n = int(0.12 * sr)
        burst = sosfilt(band, rng.standard_normal(n))
        burst *= 10 ** (tail_db / 20.0) / (np.sqrt(np.mean(burst * burst)) + 1e-9)
        burst *= np.exp(-np.arange(n) / (0.03 * sr))
        y = np.concatenate([y, burst])
    peak = float(np.max(np.abs(y))) + 1e-9
    if peak > 10 ** (-1.0 / 20.0):
        y *= 10 ** (-1.0 / 20.0) / peak
    return y


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9 _\-\.]+", " ", s)
    s = re.sub(r"\s+", " ", s).strip(" .")
    return s[:64]


# ------------------------------------------------------------------ the record of it
def note_sources(out_rel, item, meta, used, note):
    """Archive/SOURCES.md: where every file came from, appended once per run."""
    path = os.path.join(ARCHIVE, "SOURCES.md")
    md = meta.get("metadata", {}) if meta else {}
    lines = ["", "## %s" % out_rel, ""]
    if item:
        lines.append("Internet Archive item [%s](https://archive.org/details/%s)%s, licence tag: %s."
                     % (item, item, (" -- " + str(md.get("title"))) if md.get("title") else "",
                        md.get("licenseurl") or "none given"))
    if note:
        lines.append(note)
    lines.append("Cut into phrases by `Tools/library/slice_speech.py` (band 350-3200 Hz, saturation, the set's hiss).")
    lines.append("")
    for src, count in used:
        lines.append("- %s -- %d phrases" % (src, count))
    # LF, explicitly: text mode on Windows would append CRLF lines to a file written with LF, and
    # a later normalisation then shows every line of the file as changed.
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--item", help="an Internet Archive item identifier (the last part of its /details/ URL)")
    ap.add_argument("--file", action="append", default=[], help="a recording on disk (repeatable)")
    ap.add_argument("--out", help="folder under Library/Archive; default Radio/<item>")
    ap.add_argument("--episodes", type=int, default=8, help="recordings taken from the item, spread across it")
    ap.add_argument("--per-episode", type=int, default=10, help="phrases kept per recording, spread across it")
    ap.add_argument("--min-seconds", type=float, default=2.0)
    ap.add_argument("--max-seconds", type=float, default=6.0)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--quindar", action="store_true", help="Apollo network tones around each phrase instead of the squelch")
    ap.add_argument("--no-squelch", action="store_true")
    ap.add_argument("--anyway", action="store_true", help="take an item whose licence tag forbids derivatives (looked at, decided)")
    ap.add_argument("--note", default="", help="a line for SOURCES.md on why this material may be passed on")
    ap.add_argument("--list", action="store_true", help="show the item's recordings and stop")
    ap.add_argument("--dry-run", action="store_true", help="show the cuts, write nothing")
    a = ap.parse_args()
    if not a.item and not a.file:
        ap.error("--item or --file")

    rng = np.random.default_rng(a.seed)
    meta, sources = None, []      # [(label, local path)]
    if a.item:
        meta = metadata(a.item)
        md = meta.get("metadata", {})
        files = audio_files(meta)
        lic = str(md.get("licenseurl") or "")
        print("%s: %s -- %d recordings, licence tag %s" % (a.item, md.get("title"), len(files), lic or "none"))
        if a.list:
            for f in files:
                print("  %-70s %6.1f MB  %s" % (f["name"][:70], int(f.get("size", 0)) / 1e6, f.get("format")))
            return 0
        if "creativecommons.org/licenses/by" in lic and "-nd" in lic and not a.anyway:
            sys.exit("the item's licence tag (%s) forbids derivative works, and a phrase cut out of it is one; "
                     "--anyway if the rights have been looked at and the tag is not the last word" % lic)
        work = os.path.join(WORK, safe_name(a.item))
        os.makedirs(work, exist_ok=True)
        for f in spread(files, a.episodes):
            dest = os.path.join(work, f["name"])
            print("  fetching %s (%.1f MB)" % (f["name"], int(f.get("size", 0)) / 1e6), flush=True)
            download(a.item, f["name"], dest, int(f.get("size", 0)))
            sources.append((f["name"], dest))
    for f in a.file:
        sources.append((os.path.basename(f), f))

    out_rel = a.out or ("Radio/" + safe_name(a.item or "Local"))
    out_dir = os.path.join(ARCHIVE, out_rel)
    used, total = [], 0
    for label, path in sources:
        x = decode(path)
        kept, dropped = find_phrases(x, SR, a.min_seconds, a.max_seconds)
        chosen = spread(kept, a.per_episode)
        print("%s: %.1f min, %d phrases found, %d dropped, %d taken" % (label, len(x) / SR / 60.0, len(kept), len(dropped), len(chosen)))
        if a.dry_run:
            for s, e, why in chosen:
                print("    %7.1f - %7.1f s  (%.1f s)  %s" % (s, e, e - s, why))
            continue
        stem = safe_name(os.path.splitext(label)[0])
        for i, (s, e, why) in enumerate(chosen, 1):
            pad = int(0.08 * SR)
            a0, b0 = max(0, int(s * SR) - pad), min(len(x), int(e * SR) + pad)
            y = vintage(x[a0:b0].copy(), SR, rng, squelch=not a.no_squelch, quindar=a.quindar)
            write_flac(y, SR, os.path.join(out_dir, "%s_%02d.flac" % (stem, i)))
        used.append((label, len(chosen)))
        total += len(chosen)
    if not a.dry_run and total:
        note_sources(out_rel, a.item, meta, used, a.note)
        print("wrote %d phrases into Library/Archive/%s" % (total, out_rel))
    return 0


if __name__ == "__main__":
    sys.exit(main())
