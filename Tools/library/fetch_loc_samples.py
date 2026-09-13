"""Noctuary -- the Library of Congress's Citizen DJ sample packs, into the archive.

Citizen DJ (citizen-dj.labs.loc.gov) is the Library of Congress's own sampling project: audio
from its collections that the Library has identified as free to use, already cut into
sampler-ready clips of a few seconds, each pack with a statement of why it is free. This fetches
the packs (16-bit WAV, from the Library's S3 bucket), converts every clip to FLAC without loss,
writes them under Library/Archive/LoC/<collection>/, and records the source, the Library's own
rights statement and its suggested credit line in Archive/SOURCES.md.

    python Tools/library/fetch_loc_samples.py --list                 # the packs and their sizes
    python Tools/library/fetch_loc_samples.py --only variety-stage   # one pack
    python Tools/library/fetch_loc_samples.py                        # every pack in COLLECTIONS

Which packs, and why these (13.09.2026, Rene's wish for a thousand or two): the ones whose
statement is a matter of law or a gift to the Library -- Edison's companies (the assets went to
the National Park Service), the Variety Stage (the same), the National Screening Room's
government films, Tony Schwartz's recordings (acquired by the Library), Joe Smith's interviews
(donated, attribution asked for), the MusicBox Project (rights relinquished), and the National
Jukebox's opera, classical and folk songs (published before 1923: public domain since 2022 under
the Music Modernization Act, and their composers long dead, which is what an EU listener has to
ask as well). Not taken: the Jukebox's popular, jazz, blues and musical theatre (the recordings
are free in the United States, but many of their songs are by composers who died after 1955 and
are still protected in Europe), the dialect interviews (private people, the Library asks for
care, and a voice at the ear is not the place), and the Free Music Archive subset (music, not
the near layer's material).
"""
import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import urllib.request
import zipfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
ARCHIVE = os.path.join(ROOT, "Library", "Archive")
WORK = os.path.join(ROOT, "build", "loc-work")
FFMPEG = os.environ.get("AMBIENT_FFMPEG") or "ffmpeg"
for cand in (r"C:\Anw\Tools\ffmpeg\bin\ffmpeg.exe",):
    if FFMPEG == "ffmpeg" and os.path.exists(cand):
        FFMPEG = cand
AGENT = "Noctuary/2.0 (library tool; +https://github.com/reneweller-coding/Noctuary)"
S3 = "https://s3.amazonaws.com/citizen-dj-assets.labs.loc.gov/samplepacks/"
SITE = "https://citizen-dj.labs.loc.gov/"

# slug on the site -> (folder under Archive/LoC, pack name on S3, the Library's statement)
COLLECTIONS = {
    "edison": ("Edison", "loc.gov_edison-company-motion-pictures-and-sound-recordings",
               "All recordings made by the companies of Thomas A. Edison between 1890 and 1929 are in the public "
               "domain because the assets of Edison Records were transferred to the National Park Service, a "
               "federal agency, in the 1950s."),
    "variety-stage": ("Variety-Stage", "loc.gov_variety-stage-sound-recordings-and-motion-pictures",
                      "Edison recordings of the variety stage, 1890s-1920s: the same transfer to the National Park "
                      "Service puts them in the public domain."),
    "national-screening-room": ("Screening-Room", "loc.gov_national-screening-room",
                                "A subset of films from the National Screening Room that were identified to have been "
                                "created by the U.S. government, thus in the public domain."),
    "tony-schwartz": ("Tony-Schwartz", "loc.gov_tony-schwartz",
                      "In 2007 Tony Schwartz's entire body of work was acquired by the Library of Congress, which "
                      "makes his recordings available for reuse; Citizen DJ excludes the ones with embedded material "
                      "he did not own."),
    "joe-smith": ("Joe-Smith", "loc.gov_joe-smith",
                  "Joe Smith, the copyright holder, donated the recordings to the Library of Congress and agreed to "
                  "make the material free to use and reuse with proper attribution; interviews with performances of "
                  "songs still in copyright were excluded."),
    # Not the MusicBox Project nor the National Jukebox's opera, classical and folk songs: they are
    # music, and the near layer wanted voices (Rene, 13.09.: "Wir wollten aber nur Sprache").
}
CREDIT = "Citizen DJ Project, Library of Congress"


# ------------------------------------------------------------------ speech, not music
# The packs are mixed -- Edison's companies recorded rags and monologues alike -- and what the
# near layer wants is the voice. The classic discriminators (Scheirer & Slaney, 1997), measured
# on the packs themselves: interviews against opera and chamber music. Speech has more silence
# (frames under half the mean level: 0.39 against 0.15), a zero-crossing rate that jumps between
# vowels and fricatives (its spread 0.83 against 0.35), the syllable's four hertz on the subband
# envelopes (0.21 against 0.12), and fewer voiced windows than singing or an instrument (0.61
# against 0.85). Weighted into one score, every interview stood above zero and 93 % of the
# chamber music below it; the cut is at 0.5, with the voicing capped, so that a sung number with
# a piano under it stays out too.
SPEECH_SR = 16000
SPEECH_HOP = 0.01


def decode16k(path):
    r = subprocess.run([FFMPEG, "-v", "error", "-i", path, "-ac", "1", "-ar", str(SPEECH_SR), "-f", "f32le", "-"],
                       capture_output=True)
    return np.frombuffer(r.stdout, dtype="<f4").astype(np.float64) if r.returncode == 0 else np.zeros(0)


def speech_features(x, sr=SPEECH_SR, hop=SPEECH_HOP):
    n = int(sr * hop)
    m = len(x) // n
    if m < 100:
        return None
    fr = x[:m * n].reshape(m, n)
    rms = np.sqrt(np.mean(fr * fr, axis=1)) + 1e-9
    low_e = float(np.mean(rms < 0.5 * rms.mean()))
    zcr = np.mean(np.abs(np.diff(np.sign(fr), axis=1)) > 0, axis=1)
    zcr_v = float(np.std(zcr) / (np.mean(zcr) + 1e-9))
    spec = np.abs(np.fft.rfft(fr * np.hanning(n), axis=1)) ** 2
    fq = np.fft.rfftfreq(n, 1.0 / sr)
    mod4 = []
    for lo, hi in ((200, 600), (600, 1500), (1500, 3500), (3500, 7000)):
        env = np.log(spec[:, (fq >= lo) & (fq < hi)].sum(axis=1) + 1e-9)
        env = (env - env.mean()) * np.hanning(len(env))
        es = np.abs(np.fft.rfft(env)) ** 2
        ef = np.fft.rfftfreq(len(env), d=hop)
        mod4.append(es[(ef >= 3) & (ef <= 6)].sum() / (es[ef >= 0.5].sum() + 1e-12))
    w = int(0.05 * sr)
    thr = np.median(rms) * 10 ** (-6.0 / 20.0)
    lo, hi = int(sr / 1000), int(sr / 70)
    voiced, windows = 0, 0
    for i in range(0, len(x) - w, w):
        seg = x[i:i + w] - x[i:i + w].mean()
        windows += 1
        if np.sqrt(np.mean(seg * seg)) < thr:
            continue
        ac = np.correlate(seg, seg, "full")[w - 1:]
        k = lo + int(np.argmax(ac[lo:hi]))
        if ac[k] > 0.4 * ac[0]:
            voiced += 1
    return dict(low_e=low_e, zcr_v=zcr_v, mod4=float(np.mean(mod4)), voiced=voiced / max(1, windows))


def speech_score(f):
    return (f["low_e"] - 0.25) * 3.0 + (f["zcr_v"] - 0.5) * 1.0 + (f["mod4"] - 0.12) * 6.0 - (f["voiced"] - 0.75) * 3.0


# What the measurement lets through and should not: a brass band's march (its staccato is
# silence, its beat's subdivision sits right at the syllable rate) and a sung number with clear
# words. The pack's file names carry the catalogue titles, and a catalogue of 1910 says what a
# thing is -- "march", "rag", "polka", "with orchestra" -- so the title is asked first.
MUSIC_TITLE = re.compile(
    r"(?<![A-Za-z])(rag|rhapsod\w*|march|polka|waltz|overture|medley|selection|fox-?trot|one-?step|two-?step|tango|"
    r"mazurka|serenade|hymn|chorus|orchestra|band|quartet|quartette|trio|baritone|tenor|soprano|contralto|Messiah|"
    r"blues|symphony|sonata|aria|intermezzo|gavotte|minuet|nocturne|prelude|fantasi[ae]|caprice|elegie|berceuse|"
    r"cornet|violin|piano|banjo|xylophone|accordion|instrumental|vocal|song|songs|ballad|lullaby|carol|anthem)(?![A-Za-z])", re.I)


def is_speech(path):
    if MUSIC_TITLE.search(os.path.basename(path)):
        return False
    f = speech_features(decode16k(path))
    return f is not None and speech_score(f) > 0.5 and f["voiced"] <= 0.8


def safe_name(s):
    """A file name that keeps what makes the pack's name unique. The packs name a clip
    <title>_<item id>_<cut>_<hh-mm-ss>, titles run to a hundred characters, and a plain cut at
    ninety took the id, the cut and the time off the long ones -- the Variety Stage lost 97 of
    its 250 clips to names that had become the same. The title is what gets shortened."""
    s = re.sub(r"[^A-Za-z0-9 _\-\.]+", " ", s)
    s = re.sub(r"\s+", " ", s).strip(" .")
    if len(s) <= 90:
        return s
    m = re.match(r"^(.*)_([A-Za-z0-9\-]+_\d+_\d\d-\d\d-\d\d)$", s)
    if m:
        return m.group(1)[:90 - len(m.group(2)) - 1].rstrip(" .-_") + "_" + m.group(2)
    return s[:60].rstrip(" .-_") + "~" + s[-29:]


def remote_size(url):
    req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": AGENT})
    with urllib.request.urlopen(req, timeout=60) as r:
        return int(r.headers.get("Content-Length") or 0)


def download(url, dest):
    size = remote_size(url)
    if os.path.exists(dest) and size > 0 and os.path.getsize(dest) == size:
        return dest, size
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    tmp = dest + ".part"
    done = 0
    with urllib.request.urlopen(req, timeout=300) as r, open(tmp, "wb") as f:
        for chunk in iter(lambda: r.read(4 << 20), b""):
            f.write(chunk)
            done += len(chunk)
            if done % (256 << 20) < (4 << 20):
                print("    %d / %d MB" % (done >> 20, size >> 20), flush=True)
    os.replace(tmp, dest)
    return dest, size


def to_flac(src, dst, max_seconds=20.0):
    """16-bit WAV to 16-bit FLAC, the samples untouched (a 16-bit file gains nothing from 24) --
    except that an excerpt longer than max_seconds ends there, faded out over its last four
    tenths: the Library cuts up to thirty seconds, and a clip at the ear is a phrase, not a
    reel (Rene: twenty seconds at the very most)."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    fade = "afade=t=out:st=%.2f:d=0.4" % (max_seconds - 0.4)
    r = subprocess.run([FFMPEG, "-v", "error", "-y", "-i", src, "-t", "%.2f" % max_seconds, "-af", fade,
                        "-c:a", "flac", "-compression_level", "8", dst], capture_output=True)
    return r.returncode == 0


def spread(items, n):
    """n of them, evenly across the sorted run: the packs hold thousands of cuts (four thousand of
    Edison alone, several per item), and a spread by name keeps every item represented where the
    first n would be the first few items over and over."""
    if n <= 0 or n >= len(items):
        return list(items)
    if n == 1:
        return [items[len(items) // 2]]
    return [items[round(i * (len(items) - 1) / (n - 1))] for i in range(n)]


def note_sources(slug, folder, statement, count, total):
    """Archive/SOURCES.md: the pack, the count, the Library's statement -- not the file names,
    which are the pack's own and run to thousands."""
    path = os.path.join(ARCHIVE, "SOURCES.md")
    lines = ["", "## LoC/%s" % folder, "",
             "Library of Congress, Citizen DJ sample pack [%s](%sloc-%s/use/): %d of its %d clips -- the ones that "
             "measure as speech rather than music, spread across the pack -- cut by the Library, taken as 16-bit "
             "WAV and stored as FLAC without loss (at most twenty seconds each); each file keeps the pack's name "
             "(item title, item id, cut number, start time)." % (slug, SITE, slug, count, total),
             "Rights, in the Library's words: %s" % statement,
             "Suggested credit: %s." % CREDIT]
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--only", action="append", default=[], help="a slug of COLLECTIONS (repeatable)")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--per-collection", type=int, default=250,
                    help="clips kept per pack, spread across it (the packs hold thousands; 0 = all)")
    ap.add_argument("--mp3", action="store_true", help="the 192 kbps packs instead of the WAV ones (a fifth of the download)")
    ap.add_argument("--all-sounds", dest="speech_only", action="store_false",
                    help="keep the music as well; by default only clips that measure as speech are taken")
    a = ap.parse_args()
    slugs = a.only or list(COLLECTIONS)
    for slug in slugs:
        if slug not in COLLECTIONS:
            sys.exit("unknown collection %s; one of %s" % (slug, ", ".join(COLLECTIONS)))
    os.makedirs(WORK, exist_ok=True)
    total = 0
    for slug in slugs:
        folder, pack, statement = COLLECTIONS[slug]
        url = S3 + pack + ("_mp3.zip" if a.mp3 else "_wav.zip")
        if a.list:
            try:
                print("%-24s %6.0f MB  %s" % (slug, remote_size(url) / 1e6, url))
            except Exception as e:
                print("%-24s ??? %s" % (slug, e))
            continue
        print("%s: fetching %s" % (slug, url), flush=True)
        dest, size = download(url, os.path.join(WORK, os.path.basename(url)))
        print("  %d MB" % (size >> 20), flush=True)
        out_dir = os.path.join(ARCHIVE, "LoC", folder)
        extracted = []
        with zipfile.ZipFile(dest) as z:
            members = sorted(m for m in z.namelist() if m.lower().endswith((".wav", ".mp3")) and not m.startswith("__MACOSX"))
            # A pack holds every clip twice: under excerpts/ as a few seconds of the recording, and
            # under one_shots/ as the same cut trimmed to a hit for a drum machine. The excerpts are
            # the near layer's material; the same names in both folders were the other half of the
            # collisions.
            excerpts = [m for m in members if m.replace("\\", "/").lower().startswith("excerpts/")]
            if excerpts:
                members = excerpts
            src_dir = os.path.join(WORK, pack)
            os.makedirs(src_dir, exist_ok=True)
            unpacked = []
            for m in members:
                target = os.path.join(src_dir, os.path.basename(m))
                if not os.path.exists(target):
                    with z.open(m) as s, open(target, "wb") as f:
                        f.write(s.read())
                unpacked.append(target)
            # The voice, not the band: every excerpt is judged before the choice is made.
            if a.speech_only:
                with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
                    verdicts = list(ex.map(is_speech, unpacked))
                speech = [p for p, ok in zip(unpacked, verdicts) if ok]
            else:
                speech = unpacked
            extracted = spread(speech, a.per_collection)
            print("  %d clips in the pack%s, %d speech, %d taken"
                  % (len(members), " (excerpts)" if excerpts else "", len(speech), len(extracted)), flush=True)
        names, jobs = [], []
        for src in sorted(extracted):
            stem = safe_name(os.path.splitext(os.path.basename(src))[0])
            dst = os.path.join(out_dir, stem + ".flac")
            names.append(stem + ".flac")
            if not os.path.exists(dst):
                jobs.append((src, dst))
        failed = 0
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as ex:
            for ok in ex.map(lambda j: to_flac(*j), jobs):
                failed += 0 if ok else 1
        # What an earlier, wider run left in the folder goes: the folder is the selection, exactly.
        stale = 0
        if os.path.isdir(out_dir):
            keep = set(names)
            for f in os.listdir(out_dir):
                if f.lower().endswith(".flac") and f not in keep:
                    os.remove(os.path.join(out_dir, f)); stale += 1
        print("  %d converted (%d already there, %d failed, %d stale removed) -> Library/Archive/LoC/%s"
              % (len(jobs) - failed, len(names) - len(jobs), failed, stale, folder), flush=True)
        note_sources(slug, folder, statement, len(names), len(members))
        total += len(names)
    if not a.list:
        print("done: %d clips" % total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
