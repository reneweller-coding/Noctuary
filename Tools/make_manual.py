"""Noctuary -- turn the in-app help into an HTML manual and a PDF.

The help page inside the instrument is the manual, and its pictures are snapshots of the panel
itself: the real sections with their real values, taken as the page is opened. That is what keeps
them right -- a drawing of a section goes out of date the day the section changes, and nobody
notices for a year. It also means they only exist while an editor is running, so this is a two
step job:

    set AMBIENT_PRESET=Tidal Expanse
    set AMBIENT_MANUAL=docs\\manual
    build\\...\\Noctuary.exe            waits five seconds, writes the folder, quits
    python Tools/make_manual.py           folder -> Noctuary-Manual.html -> .pdf

AMBIENT_PRESET matters as much as the rest. The pictures are of the panel as it stands, so a
preset with a source switched off gives a picture of a greyed-out section, and the manual then
illustrates its Sources chapter with a section that is doing nothing. Pick one that has all three
sources and the effects in use; Deploy/build_release.ps1 does.

Five seconds because the pictures are of the running instrument: its spectrum, its stage and its
note roll have nothing in them until it has been playing for a while, and a manual whose displays
are empty boxes is worse than one with no pictures.

The PDF is printed by Edge in headless mode. If that is not available the HTML is still written
and is perfectly readable; the manual is not held hostage by a browser.
"""
import argparse
import html
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, ".."))
DIR = os.path.join(ROOT, "docs", "manual")

CSS = """
@page { size: A4; margin: 18mm 16mm 16mm 16mm; }
body { font: 10.5pt/1.55 "Segoe UI", "Helvetica Neue", Arial, sans-serif; color: #16181d;
       background: #fff; margin: 0; }
h1 { font-size: 30pt; margin: 0 0 2mm 0; letter-spacing: -0.5pt; }
h2 { font-size: 16pt; margin: 0 0 3mm 0; padding-bottom: 2mm; border-bottom: 1px solid #cfd4dc;
     color: #0c5c6b; }
h3 { font-size: 11pt; margin: 6mm 0 1.5mm 0; color: #0c5c6b; letter-spacing: 0.6pt;
     text-transform: uppercase; }
p  { margin: 0 0 3.2mm 0; }
.sub { color: #5b6470; font-size: 12pt; margin: 0 0 8mm 0; }
.cover { page-break-after: always; text-align: center; padding-top: 28mm; }
.cover img.panel { width: 100%; border: 1px solid #cfd4dc; margin-top: 10mm; }
.cover img.logo { width: 26mm; }
.facts { margin: 8mm auto 0 auto; color: #5b6470; font-size: 10pt; }
.toc { page-break-after: always; }
.toc ol { padding-left: 6mm; }
.toc li { margin: 1.2mm 0; }
.topic { page-break-before: always; }
.topic p { text-align: justify; hyphens: auto; }
figure { margin: 4mm 0 5mm 0; page-break-inside: avoid; }
figure img { max-width: 100%; border: 1px solid #d6dae1; border-radius: 3px; display: block; }
figcaption { font-size: 8.5pt; color: #6b7480; margin-top: 1.2mm; }
pre { font: 8.2pt/1.35 Consolas, "DejaVu Sans Mono", monospace; background: #f3f5f8;
      border: 1px solid #e2e6ec; border-radius: 3px; padding: 3mm;
      white-space: pre-wrap; page-break-inside: avoid; margin: 0 0 4mm 0; }
.params h3 { margin-top: 7mm; }
.params dl { margin: 0; }
.params dt { font-weight: 600; margin-top: 2.6mm; }
.params dt .key { font-weight: 400; color: #6b7480; font-family: Consolas, monospace;
                  font-size: 9pt; }
.params dd { margin: 0.4mm 0 0 0; color: #333a44; }
.block { margin: 6mm 0 8mm 0; page-break-inside: auto; }
.block h4 { font-size: 12.5pt; margin: 8mm 0 2mm 0; color: #16181d; letter-spacing: 0.3pt; page-break-after: avoid; }
.block h5 { font-size: 9.5pt; margin: 4mm 0 1mm 0; color: #0c5c6b; letter-spacing: 0.6pt; page-break-after: avoid; }
.block .params dt { margin-top: 1.8mm; font-size: 10pt; }
.block .params dd { font-size: 9.6pt; }
.cover img.header { width: 100%; border: 1px solid #cfd4dc; margin-top: 4mm; }
footer { margin-top: 10mm; padding-top: 3mm; border-top: 1px solid #cfd4dc; color: #6b7480;
         font-size: 8.5pt; }
"""


# Which of a slot's knobs each source type lights up (by the key without its slot prefix).
TYPE_KEYS = {
    "Additive":  ("partials", "tilt", "bright", "odd_even", "inharmonic", "shimmer", "shimmer_rate", "drift"),
    "Harmonic":  ("table", "pos", "pos_drift", "drift"),
    "Wavetable": ("table", "pos", "pos_drift", "drift"),
    "FM":        ("fm_ratio", "fm_index", "pos_drift", "drift"),
    "Texture":   ("grain", "density", "density_sync", "follow", "grains", "spread", "pos", "pos_drift", "drift"),
    "Stretch":   ("grain", "stretch", "xfade", "pos", "pos_drift", "follow", "drift"),
    "Noise":     ("noise", "noise_q", "pos", "density", "density_sync", "follow"),
}


def paragraphs(text):
    """The help texts are plain prose with blank lines between paragraphs, and the occasional
    line that is a heading because it is short and ends without a full stop."""
    out = []
    for block in re.split(r"\n\s*\n", text.strip("\n")):
        raw_lines = block.strip("\n").split("\n")
        if raw_lines and all(l.startswith("    ") or not l.strip() for l in raw_lines):
            # A formula or a table, indented by hand: printed as it stands, never as a heading and
            # never with its spaces collapsed. Checked before the block is stripped, or the first
            # line's indentation is gone and the formula becomes a heading -- which it did.
            out.append("<pre>%s</pre>" % html.escape("\n".join(l[4:] for l in raw_lines)))
            continue
        block = block.strip()
        if not block:
            continue
        lines = block.split("\n")
        # A heading is a short single line in capitals without a full stop. The capitals matter:
        # a short line of prose that introduces a formula is not a heading.
        if len(block) < 70 and len(lines) == 1 and not block.endswith((".", ":", "?")) and block == block.upper():
            out.append("<h3>%s</h3>" % html.escape(block))
        elif len(lines) > 3 and (block.count("->") > 2 or block.count("  ") > 4):
            # A block drawn in text: the signal flow, a table of shortcuts. Collapsing its line
            # breaks into spaces turns a diagram into one very long sentence, which is exactly
            # what happened to the first page of the first draft of this manual.
            out.append("<pre>%s</pre>" % html.escape(block))
        else:
            out.append("<p>%s</p>" % html.escape(block).replace("\n", " "))
    return "\n".join(out)


def parameter_reference(text):
    """The generated topic is
           SECTION
             Name  (key, range)
                 help text
       which is a definition list wearing plain-text clothes."""
    out, in_dl = [], False
    dt = None
    for line in text.split("\n"):
        if not line.strip():
            continue
        if not line.startswith(" "):                       # a section heading
            if in_dl:
                out.append("</dl>")
                in_dl = False
            out.append("<h3>%s</h3>" % html.escape(line.strip().title()))
            continue
        if line.startswith("      "):                       # the help text of the entry before it
            if dt is not None:
                out.append("<dd>%s</dd>" % html.escape(line.strip()))
            continue
        if not in_dl:
            out.append("<dl>")
            in_dl = True
        m = re.match(r"\s*(.*?)\s\s+\((.*)\)\s*$", line)
        if m:
            dt = m.group(1)
            out.append('<dt>%s <span class="key">(%s)</span></dt>'
                       % (html.escape(dt), html.escape(m.group(2))))
        else:
            dt = line.strip()
            out.append("<dt>%s</dt>" % html.escape(dt))
    if in_dl:
        out.append("</dl>")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=DIR, help="the folder AMBIENT_MANUAL wrote")
    ap.add_argument("--no-pdf", action="store_true")
    a = ap.parse_args()

    src = os.path.join(a.dir, "manual.json")
    if not os.path.isfile(src):
        sys.exit("no manual.json in %s -- run the standalone with AMBIENT_MANUAL set first" % a.dir)
    with open(src, encoding="utf-8") as f:
        man = json.load(f)
    topics = man["topics"]
    # A manual with a tab missing from it is not printed. The export counts, this refuses.
    cov = os.path.join(a.dir, "coverage.txt")
    if os.path.isfile(cov):
        holes = [l.strip() for l in open(cov, encoding="utf-8") if l.strip() and not l.strip().endswith(": 1")]
        if holes:
            sys.exit("tabs not photographed exactly once: " + "; ".join(holes))
    # Every parameter, by section, for the per-block lists. An older manual.json has no "params".
    by_section = {}
    for r in man.get("params") or []:
        by_section.setdefault(r["section"], []).append(r)

    body = []
    logo = os.path.relpath(os.path.join(ROOT, "docs", "logo-256.png"), a.dir).replace("\\", "/")
    body.append('<div class="cover">')
    body.append('<img class="logo" src="%s" alt="">' % logo)
    body.append("<h1>Noctuary</h1>")
    body.append('<p class="sub">Manual &middot; version %s</p>' % html.escape(man.get("version", "")))
    if os.path.isfile(os.path.join(a.dir, "panel.png")):
        body.append('<img class="panel" src="panel.png" alt="The instrument">')
    if os.path.isfile(os.path.join(a.dir, "header.png")):
        body.append('<img class="header" src="header.png" alt="The header">')
    body.append('<p class="facts">%s built-in presets and 6200 in the library &middot; '
                '%s filter shapes &middot; %s Cosmos, %s filter and %s Strike presets</p>'
                % (man.get("presets", "?"), man.get("shapes", "?"), man.get("cosmos", "?"),
                   man.get("zpresets", "?"), man.get("strike", "?")))
    body.append("</div>")

    body.append('<div class="toc"><h2>Contents</h2><ol>')
    for t in topics:
        body.append("<li>%s</li>" % html.escape(t["title"]))
    body.append("</ol></div>")

    for i, t in enumerate(topics):
        params = t["title"] == "All parameters"
        body.append('<div class="topic%s">' % (" params" if params else ""))
        body.append("<h2>%d. %s</h2>" % (i + 1, html.escape(t["title"])))
        body.append(parameter_reference(t["text"]) if params else paragraphs(t["text"]))
        for im in t["images"]:
            # Newer exports carry a caption with every picture; the caption says what is in it.
            img, cap = (im["file"], im.get("caption", "")) if isinstance(im, dict) else (im, "")
            body.append('<figure><img src="%s" alt="">%s</figure>'
                        % (html.escape(img), ("<figcaption>%s</figcaption>" % html.escape(cap)) if cap else ""))
        # The tabs of this chapter, each captioned with the name it wears on its own bar. The help
        # page inside the instrument does not show these -- its picture column has room for two or
        # three -- but a reader of the manual has no instrument in front of them, so every tab of
        # the panel is printed here.
        tabs = t.get("tabs") or []
        if tabs:
            body.append('<h3>The blocks of this chapter, one by one</h3>')
            for tab in tabs:
                cap = tab.get("name") or tab.get("caption", "")
                body.append('<div class="block">')
                body.append('<h4>%s</h4>' % html.escape(cap))
                body.append('<figure><img src="%s" alt=""><figcaption>%s</figcaption></figure>'
                            % (html.escape(tab["file"]), html.escape(tab.get("caption") or cap)))
                if tab.get("blurb"):
                    body.append(paragraphs(tab["blurb"]))
                # The parameters of the sections under this picture, each with its help text.
                # Sources 2, 3 and 4 are the same twenty-six knobs three times over, so they are
                # printed once, under Source 2; a type in the gallery lists only the knobs that
                # type lights up; the rest of the slot is on the Source 2 page.
                for sec in tab.get("sections") or []:
                    rows = by_section.get(sec) or []
                    if sec in ("Source 3", "Source 4"):
                        body.append('<p><i>The same controls as Source 2, listed there.</i></p>')
                        continue
                    if cap.startswith("TYPE "):
                        want = TYPE_KEYS.get(cap[5:], ())
                        rows = [r for r in rows if r["key"].split("_", 1)[-1] in want]
                        if not rows:
                            continue
                    if not rows:
                        continue
                    body.append('<div class="params"><h5>%s</h5><dl>' % html.escape(sec.upper() if not cap.startswith("TYPE ") else "WHAT THIS TYPE USES"))
                    for r in rows:
                        body.append('<dt>%s <span class="key">(%s, %s)</span></dt><dd>%s</dd>'
                                    % (html.escape(r["name"]), html.escape(r["key"]), html.escape(r["range"]),
                                       html.escape(r["help"])))
                    body.append('</dl></div>')
                body.append('</div>')
        body.append("</div>")

    body.append('<footer>Noctuary %s &middot; the pictures in this manual are snapshots of the '
                'instrument itself, taken while it was running. AGPL-3.0.</footer>'
                % html.escape(man.get("version", "")))

    out_html = os.path.join(a.dir, "Noctuary-Manual.html")
    with open(out_html, "w", encoding="utf-8") as f:
        f.write("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                "<title>Noctuary Manual</title>\n<style>%s</style></head><body>\n%s\n"
                "</body></html>\n" % (CSS, "\n".join(body)))
    print("wrote %s (%.0f KB)" % (out_html, os.path.getsize(out_html) / 1024))
    if a.no_pdf:
        return 0

    edge = next((p for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                             r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
                             r"C:\Program Files\Google\Chrome\Application\chrome.exe")
                 if os.path.isfile(p)), None)
    if edge is None:
        print("no Edge or Chrome found -- the HTML is written, print it yourself")
        return 0
    pdf = os.path.join(a.dir, "Noctuary-Manual.pdf")
    if os.path.exists(pdf):
        os.remove(pdf)
    # --headless=new, not --headless. On Edge 152 the old flag exits without a word and without
    # a file; the new one prints in a second. Tried in that order so an older browser still works.
    url = "file:///" + out_html.replace("\\", "/")
    # A profile folder of its own, and a NEW one every time. With the default profile, or with one
    # this script used before, the launcher returns at once -- rc 0, not a word -- and no file
    # ever comes: measured, a reused folder printed nothing in twenty seconds, a fresh one in two.
    # And the process we start is only the launcher; it can return before the child that does
    # the printing has written anything, so the file is waited for, not just looked for. That is
    # how a print that worked in the morning produced nothing in the afternoon.
    for flag in ("--headless=new", "--headless"):
        profile = tempfile.mkdtemp(prefix="noctuary-manual-print-")
        cmd = [edge, flag, "--disable-gpu", "--no-pdf-header-footer", "--user-data-dir=" + profile,
               "--print-to-pdf=" + pdf, url]
        try:
            subprocess.run(cmd, timeout=180, capture_output=True)
        except subprocess.TimeoutExpired:
            print("%s did not finish in three minutes" % flag)
            continue
        for _ in range(60):
            if os.path.isfile(pdf):
                break
            time.sleep(0.5)
        if os.path.isfile(pdf):
            # and finished: a PDF still being written grows between two looks
            size = -1
            while size != os.path.getsize(pdf):
                size = os.path.getsize(pdf)
                time.sleep(0.5)
        shutil.rmtree(profile, ignore_errors=True)
        if os.path.isfile(pdf):
            break
    if os.path.isfile(pdf):
        print("wrote %s (%.1f MB)" % (pdf, os.path.getsize(pdf) / 1e6))
        return 0
    print("the browser produced no PDF; the HTML is there and prints fine by hand")
    return 1


if __name__ == "__main__":
    sys.exit(main())
