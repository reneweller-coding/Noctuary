"""Noctuary -- a night of the night director (Core/include/ambient/NightDirector.h) to look at: the table and a
picture of the curve against what was chosen.

Runs ambient_night (no audio, no library) and prints, per step: when it begins on the clock, the preset and its pack,
how long it stays, its stillness and brightness as percentiles of the candidates against the curve's targets there,
its roughness, and its distance in sound from the step before (the journeys' distance). Then the night's numbers: how
closely stillness and brightness follow the curve (Spearman, and the mean and largest distance from it in percentile
points), the mean step between neighbours against the shipped pack Nights', and the stays. With --png it draws the two
curves (stillness above, brightness below) and the presets chosen as a step line over the hours, the phases marked
(Pillow; matplotlib is in no environment on this machine).

    python Tools/night_plot.py [--hours 8 | --until 7:00] [--start 23:00] [--seed 1] [--choice every|sleep|PACK]
                               [--prefer FILE] [--exclude FILE] [--txt FILE] [--png FILE] [--journey FILE]
"""
import argparse
import math
import os
import subprocess
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
EXES = [os.path.join(ROOT, "bin", "msvc", "ambient_night.exe"),
        os.path.join(ROOT, "build", "msvc", "Tools", "render", "Release", "ambient_night.exe")]


def clock(seconds, start_h):
    """Seconds into the night as the clock shows them, from start_h o'clock."""
    m = int(round(seconds / 60.0)) + int(round(start_h * 60))
    return "%d:%02d" % ((m // 60) % 24, m % 60)


def spearman(a, b):
    """Spearman's rank correlation of two equally long lists."""
    def ranks(v):
        order = sorted(range(len(v)), key=lambda i: v[i])
        r = [0.0] * len(v)
        for k, i in enumerate(order):
            r[i] = float(k)
        return r
    ra, rb = ranks(a), ranks(b)
    m = (len(a) - 1) / 2.0
    num = sum((x - m) * (y - m) for x, y in zip(ra, rb))
    den = math.sqrt(sum((x - m) ** 2 for x in ra) * sum((y - m) ** 2 for y in rb))
    return num / den if den else 0.0


def hours_of(text):
    h, _, m = text.partition(":")
    return int(h) + (int(m) / 60.0 if m else 0.0)


def read(exe, a):
    """Runs ambient_night; returns the head, the steps, the curve and the shipped Nights' mean step."""
    cmd = [exe, "--seed", str(a.seed), "--start", a.start]
    cmd += ["--until", a.until] if a.until else ["--hours", str(a.hours)]
    for flag in ("choice", "prefer", "exclude"):
        if getattr(a, flag):
            cmd += ["--" + flag, getattr(a, flag)]
    out = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", check=True).stdout
    head, steps, curve, shipped = {}, [], [], None
    cols = None
    for line in out.splitlines():
        if line.startswith("# night "):
            p = line[8:].split(" ", 4)
            head = {"hours": float(p[0]), "seed": p[1], "candidates": int(p[2]), "scale": float(p[3]), "choice": p[4]}
        elif line.startswith("# phases "):
            p = line[9:].split()
            head.update(asleep=float(p[0]), morning=float(p[1]), waves=int(p[2]))
        elif line.startswith("# journeys "):
            p = line.split()
            shipped = (float(p[2]), int(p[3]))
        elif line.startswith("# curve "):
            curve.append(tuple(float(x) for x in line.split()[2:5]))
        elif line.startswith("step\t"):
            cols = line.split("\t")
        elif cols and line.strip() and not line.startswith("#"):
            row = dict(zip(cols, line.split("\t")))
            for k in ("at", "stay", "still", "bright", "rough", "target_still", "target_bright", "distance"):
                row[k] = float(row[k])
            row["class"] = int(row["class"])
            steps.append(row)
    if a.journey:
        cmd2 = list(cmd) + ["--write", a.journey]
        subprocess.run(cmd2, capture_output=True, text=True, check=True)
    return head, steps, curve, shipped


def table(head, steps, shipped, start_h):
    """The night as text."""
    lines = ["Night of %g h from %s (seed %s, %s): %d presets of %d candidates; falling asleep %d min, %d waves, "
             "morning %d min" % (head["hours"], clock(0, start_h), head["seed"], head["choice"], len(steps),
                                  head["candidates"], round(head["asleep"] * head["hours"] * 60), head["waves"],
                                  round(head["morning"] * head["hours"] * 60)), "",
             "%-6s %-28s %-18s %6s  %-15s %-15s %5s %5s" % ("time", "preset", "pack", "stay", "stillness/goal",
                                                           "bright/goal", "rough", "step"),
             "-" * 108]
    for s in steps:
        lines.append("%-6s %-28s %-18s %6s  %4.0f %% / %3.0f %%  %4.0f %% / %3.0f %%  %4.0f%% %5.2f" % (
            clock(s["at"], start_h), s["preset"][:28], s["pack"][:18],
            "%d:%02d" % (s["stay"] // 60, s["stay"] % 60), 100 * s["still"], 100 * s["target_still"],
            100 * s["bright"], 100 * s["target_bright"], 100 * s["rough"], s["distance"]))
    rho = spearman([s["still"] for s in steps], [s["target_still"] for s in steps])
    rho_b = spearman([s["bright"] for s in steps], [s["target_bright"] for s in steps])
    err_s = [abs(s["still"] - s["target_still"]) for s in steps]
    err_b = [abs(s["bright"] - s["target_bright"]) for s in steps]
    mean = sum(s["distance"] for s in steps[1:]) / max(1, len(steps) - 1)
    asleep_end = head["asleep"] * head["hours"] * 3600.0
    rough_asleep = max([s["rough"] for s in steps if s["at"] + 0.5 * s["stay"] < asleep_end] or [0.0])
    stays = [0, 0, 0]
    for s in steps:
        stays[s["class"]] += 1
    lines += ["",
              "stillness follows the curve: Spearman %.2f, %.1f percentile points off on average, %.1f at most" % (
                  rho, 100 * sum(err_s) / len(err_s), 100 * max(err_s)),
              "brightness follows the curve: Spearman %.2f, %.1f points off on average, %.1f at most" % (
                  rho_b, 100 * sum(err_b) / len(err_b), 100 * max(err_b)),
              "mean step between neighbours %.2f spreads (the shipped pack Nights: %s; a random pair about 5.7)" % (
                  mean, "%.2f over %d steps" % shipped if shipped and shipped[1] else "not found"),
              "roughest preset while falling asleep: the %.0fth percentile (kept below the 60th)" % (100 * rough_asleep),
              "stays: %d short (3-6 min), %d middle (6-10), %d long (10-15), scaled by %.3f to fill the night" % (
                  stays[0], stays[1], stays[2], head["scale"]),
              "stillness, brightness and roughness are percentiles of the candidates: 0 % the stillest / darkest / "
              "smoothest"]
    return "\n".join(lines)


def plot(head, steps, curve, path, start_h):
    """The two curves and the night's steps, as a PNG."""
    from PIL import Image, ImageDraw, ImageFont
    W, H, L, R, T, B = 1600, 860, 150, 30, 64, 60
    img = Image.new("RGB", (W, H), (16, 20, 30))
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype("segoeui.ttf", 18)
        small = ImageFont.truetype("segoeui.ttf", 15)
    except OSError:
        font = small = ImageFont.load_default()
    total = head["hours"] * 3600.0
    panel_h = (H - T - B - 50) // 2
    ink, dim, goal = (220, 226, 236), (120, 130, 150), (240, 200, 120)
    colours = [(120, 200, 230), (190, 150, 240)]
    d.text((L, 16), "Noctuary night director: %g h from %s, seed %s, %s -- %d presets (curve dashed, chosen solid)" % (
        head["hours"], clock(0, start_h), head["seed"], head["choice"], len(steps)), fill=ink, font=font)
    for p, (key, name) in enumerate([("still", "stillness"), ("bright", "brightness")]):
        top = T + p * (panel_h + 50)
        x0, x1, y0, y1 = L, W - R, top, top + panel_h
        d.rectangle([x0, y0, x1, y1], outline=dim)
        d.text((10, y0 + panel_h // 2 - 10), name, fill=ink, font=font)
        for q in (0.0, 0.25, 0.5, 0.75, 1.0):
            y = y1 - q * (y1 - y0)
            d.line([x0, y, x1, y], fill=(40, 46, 60))
            d.text((x0 - 46, y - 9), "%3d%%" % (q * 100), fill=dim, font=small)
        for xph in (head["asleep"], 1.0 - head["morning"]):
            x = x0 + xph * (x1 - x0)
            d.line([x, y0, x, y1], fill=(80, 90, 110), width=2)
        if p == 0:
            d.text((x0 + 6, y0 + 4), "falling asleep", fill=dim, font=small)
            d.text((x1 - 70, y0 + 4), "morning", fill=dim, font=small)
        hours = int(head["hours"] + 0.999)
        for h in range(hours + 1):
            x = x0 + min(1.0, h * 3600.0 / total) * (x1 - x0)
            d.line([x, y1, x, y1 + 5], fill=dim)
            if p == 1:
                d.text((x - 18, y1 + 10), clock(h * 3600.0, start_h), fill=dim, font=small)
        pts = [(x0 + c[0] * (x1 - x0), y1 - c[1 + p] * (y1 - y0)) for c in curve]
        for i in range(0, len(pts) - 1, 2):   # dashed
            d.line([pts[i], pts[i + 1]], fill=goal, width=3)
        for s in steps:
            xa = x0 + s["at"] / total * (x1 - x0)
            xb = x0 + min(1.0, (s["at"] + s["stay"]) / total) * (x1 - x0)
            y = y1 - s[key] * (y1 - y0)
            d.line([xa, y, xb, y], fill=colours[p], width=4)
    img.save(path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--hours", type=float, default=8.0)
    ap.add_argument("--until", default=None, help="the wake time, e.g. 7:00 (instead of --hours)")
    ap.add_argument("--start", default="23:00", help="the clock at the night's start (23:00)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--choice", default=None, help="every (default), a family (sleep, deep, ...) or a pack's title")
    ap.add_argument("--prefer", default=None, help="presets preferred, one a line (the favourites file)")
    ap.add_argument("--exclude", default=None, help="presets never taken, one a line (the never-again file)")
    ap.add_argument("--exe", default=None)
    ap.add_argument("--txt", default=None, help="write the table there too")
    ap.add_argument("--png", default=None, help="draw the curves there (Pillow)")
    ap.add_argument("--journey", default=None, help="write the night as a .journey file there")
    a = ap.parse_args()
    exe = os.path.abspath(a.exe) if a.exe else next((e for e in EXES if os.path.exists(e)), None)
    if not exe:
        sys.exit("night_plot: no ambient_night (build it, or pass --exe)")
    head, steps, curve, shipped = read(exe, a)
    start_h = hours_of(a.start)
    text = table(head, steps, shipped, start_h)
    print(text)
    if a.txt:
        with open(a.txt, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text + "\n")
    if a.png:
        try:
            plot(head, steps, curve, a.png, start_h)
            print("picture: %s" % a.png)
        except ImportError:
            print("no Pillow in this Python: no picture")
    return 0


if __name__ == "__main__":
    sys.exit(main())
