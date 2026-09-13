"""Noctuary -- the logo, drawn rather than painted.

Every candidate is built from something the instrument actually is, in the palette the editor
already uses (Plugin/NoctuaryLookAndFeel.h): the cold turquoise is the instrument's own colour, the
warm orange means something is sounding, and the deep blue-black is the ground everything sits on.

    python Tools/make_logo.py            # all candidates, previews at 512 / 64 / 32 / 16
    python Tools/make_logo.py --pick horizon --out docs/logo   # one candidate, PNGs and an .ico

An icon is decided at sixteen pixels, so every candidate is rendered there too and nothing that
falls apart at that size is worth keeping.
"""
import argparse
import math
import os
from PIL import Image, ImageDraw, ImageFilter

# The editor's palette, as the source of truth for the brand.
BG0 = (0x08, 0x0b, 0x12)
BG1 = (0x0d, 0x14, 0x20)
ACCENT = (0x6f, 0xd8, 0xd2)     # cold, wide: the instrument's own colour
LIVE = (0xe8, 0xa4, 0x5c)       # warm: something is sounding
FAR = (0x35, 0x50, 0x6a)        # the far plane, as the keyboard strip draws it
VOICE = (0x74, 0xb6, 0xe0)

SS = 4                          # supersampling: everything is drawn large and shrunk once


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def ground(size, radius_frac=0.22):
    """The rounded square everything sits on: a vertical lift from the window's own two blacks."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    grad = Image.new("RGBA", (size, size))
    px = grad.load()
    for y in range(size):
        c = lerp(BG1, BG0, y / max(size - 1, 1))
        for x in range(size):
            px[x, y] = c + (255,)
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1],
                                           radius=int(size * radius_frac), fill=255)
    img.paste(grad, (0, 0), mask)
    return img, mask


def glow(layer, radius):
    """A soft copy underneath, so a lit line seems to glow rather than to be outlined."""
    return layer.filter(ImageFilter.GaussianBlur(radius))


# ---------------------------------------------------------------- candidates

def horizon(size):
    """The harmonic series as a landscape.

    Six arcs, spaced like the partials of a harmonic series (1, 1/2, 1/3 ...), so they bunch
    towards the top exactly the way a spectrum does. Their colour runs from the instrument's
    turquoise at the front to the far plane's dim blue at the back -- the same interpolation the
    keyboard strip uses for near and far. One warm dot sits on the nearest arc: something is
    sounding. The additive engine and the depth model in one glyph.
    """
    s = size * SS
    img, mask = ground(s)
    lines = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(lines)
    # Four arcs, not six: at sixteen pixels a line needs four of them to itself, and a mark that
    # has to be squinted at is not a mark. They are spread evenly with a little compression
    # towards the back, which is perspective rather than decoration.
    #
    # Below thirty-two pixels the mark sheds its faintest layers and thickens what is left. Every
    # icon set worth the name is hand-tuned at the small sizes; a single drawing scaled down is
    # how a logo turns into three grey smudges in a taskbar.
    n = 4 if size >= 32 else (3 if size >= 20 else 2)
    bottom, top = s * 0.755, s * 0.285
    if n < 4:
        bottom, top = s * 0.735, s * 0.335
    for i in range(n):
        e = i / (n - 1)
        t = e * (0.86 + 0.14 * e)
        y = bottom - (bottom - top) * t
        col = lerp(ACCENT, FAR, e)
        alpha = int(255 * (1.0 - 0.5 * e))
        w = s * (0.76 - 0.20 * e)
        bow = s * (0.085 - 0.055 * e)
        x0, x1 = (s - w) / 2, (s + w) / 2
        thick = (0.038 - 0.016 * e) * (1.0 if n == 4 else 1.45)
        d.arc([x0, y - bow, x1, y + bow], start=180, end=360,
              fill=col + (alpha,), width=max(1, int(s * thick)))
    lit = Image.alpha_composite(glow(lines, s * 0.022), lines)
    # The note that is sounding, sitting on the nearest arc rather than floating above it.
    dot = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dot)
    r = s * (0.070 if n == 4 else 0.095)
    cx, cy = s * 0.5, bottom - s * (0.085 if n == 4 else 0.075) - s * 0.006
    dd.ellipse([cx - r, cy - r, cx + r, cy + r], fill=LIVE + (255,))
    lit = Image.alpha_composite(lit, Image.alpha_composite(glow(dot, s * 0.035), dot))
    out = Image.alpha_composite(img, Image.composite(lit, Image.new("RGBA", (s, s), (0, 0, 0, 0)), mask))
    return out.resize((size, size), Image.LANCZOS)


def bank(size):
    """The partial bank itself: amplitudes falling as 1/h, with the two ears split apart.

    The left half leans one way and the right half the other, which is the instrument's whole
    stereo idea (a voice is never in one place) and gives the mark a symmetry that reads small.
    """
    s = size * SS
    img, mask = ground(s)
    lines = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(lines)
    # Six partials a side, thick enough that the outer ones still exist at sixteen pixels, and
    # fading towards the far plane's colour: a spectrum that also has depth.
    n = 6
    base = s * 0.775
    wdt = max(1, int(s * 0.042))
    for i in range(n):
        amp = (1.0 / (i + 1)) ** 0.5
        for side in (-1, 1):
            x = s * 0.5 + side * (s * 0.068 + i * s * 0.072)
            top = base - (base - s * 0.235) * amp
            e = i / (n - 1)
            col = lerp(ACCENT, FAR, e * 0.75)
            d.rounded_rectangle([x - wdt / 2, top, x + wdt / 2, base],
                                radius=wdt / 2, fill=col + (int(255 * (1.0 - 0.42 * e)),))
    lit = Image.alpha_composite(glow(lines, s * 0.022), lines)
    out = Image.alpha_composite(img, Image.composite(lit, Image.new("RGBA", (s, s), (0, 0, 0, 0)), mask))
    return out.resize((size, size), Image.LANCZOS)


def planes(size):
    """The stage seen from above: the listener at the front, voices on their planes.

    This is the STAGE display of the editor, reduced to what survives at sixteen pixels: three
    ellipses receding, one bright voice near, two dim ones far.
    """
    s = size * SS
    img, mask = ground(s)
    lines = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(lines)
    # Two rings, not three, and thick: the third one was the first thing to disappear.
    for i, depth in enumerate((0.0, 1.0)):
        w = s * (0.355 + 0.075 * depth)
        hgt = s * (0.105 + 0.030 * depth)
        y = s * (0.665 - 0.245 * depth)
        col = lerp(ACCENT, FAR, depth)
        d.ellipse([s * 0.5 - w, y - hgt, s * 0.5 + w, y + hgt],
                  outline=col + (int(255 * (1.0 - 0.42 * depth)),), width=max(1, int(s * 0.036)))
    dots = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    dd = ImageDraw.Draw(dots)
    for (x, y, r, col) in ((0.50, 0.665, 0.072, LIVE), (0.31, 0.44, 0.042, VOICE)):
        dd.ellipse([s * (x - r), s * (y - r), s * (x + r), s * (y + r)], fill=col + (255,))
    lit = Image.alpha_composite(glow(lines, s * 0.018), lines)
    lit = Image.alpha_composite(lit, Image.alpha_composite(glow(dots, s * 0.03), dots))
    out = Image.alpha_composite(img, Image.composite(lit, Image.new("RGBA", (s, s), (0, 0, 0, 0)), mask))
    return out.resize((size, size), Image.LANCZOS)


CANDIDATES = {"horizon": horizon, "bank": bank, "planes": planes}


def contact_sheet(path):
    """One picture with every candidate at the sizes that decide it."""
    sizes = [256, 64, 32, 16]
    pad, label = 18, 30
    width = pad + sum(s + pad for s in sizes)
    height = label + len(CANDIDATES) * (256 + pad) + pad
    sheet = Image.new("RGB", (width, height), (24, 26, 32))
    d = ImageDraw.Draw(sheet)
    x = pad
    for s in sizes:
        d.text((x, 8), f"{s} px", fill=(190, 195, 205))
        x += s + pad
    for row, (name, fn) in enumerate(CANDIDATES.items()):
        y = label + row * (256 + pad)
        x = pad
        for s in sizes:
            sheet.paste(fn(s), (x, y + (256 - s) // 2))
            x += s + pad
        d.text((pad + 4, y + 236), name, fill=(230, 235, 245))
    sheet.save(path)
    return path


def write_icon(fn, out_base):
    """A .ico with every size Windows asks for, and the PNGs beside it."""
    sizes = [256, 128, 64, 48, 32, 24, 16]
    images = [fn(s) for s in sizes]
    for s, im in zip(sizes, images):
        im.save(f"{out_base}-{s}.png")
    images[0].save(f"{out_base}.ico", sizes=[(s, s) for s in sizes])
    return f"{out_base}.ico"


def write_android(fn, res_dir):
    """The launcher icon in the densities Android asks for, ready for aapt2."""
    for folder, px in (("mipmap-mdpi", 48), ("mipmap-hdpi", 72), ("mipmap-xhdpi", 96),
                       ("mipmap-xxhdpi", 144), ("mipmap-xxxhdpi", 192)):
        d = os.path.join(res_dir, folder)
        os.makedirs(d, exist_ok=True)
        fn(px).save(os.path.join(d, "ic_launcher.png"))
    return res_dir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pick", choices=sorted(CANDIDATES))
    ap.add_argument("--out", default=os.path.join("docs", "logo"))
    ap.add_argument("--sheet", default=os.path.join("docs", "logo-candidates.png"))
    ap.add_argument("--android", help="also write the Android launcher densities into this res/ directory")
    a = ap.parse_args()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    if a.pick:
        fn = CANDIDATES[a.pick]
        print("wrote", write_icon(fn, a.out))
        if a.android:
            print("wrote", write_android(fn, a.android))
    else:
        print("wrote", contact_sheet(a.sheet))


if __name__ == "__main__":
    main()
