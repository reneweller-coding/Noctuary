"""Noctuary -- the picture GitHub shows when the project is linked: docs/social-preview.png (1280 x 640).

    python Tools/make_preview.py

The logo, the name, one sentence, and the panel (docs/screenshot.png). As for Ephemeris
(BerlinSchoolGenerator/Tools/manual/make_preview.py, 26.09.2026). GitHub takes it by hand:
Settings, General, Social preview.

docs/screenshot.png is the standalone's window, muted, a chord sounding, photographed large so that
it stays sharp when it is made small here:

    set AMBIENT_SHOT=build\\shot-ignore.png      (plays the chord at start, quits after ten seconds)
    set AMBIENT_MUTE=1
    set AMBIENT_PRESET=Tidal Expanse
    powershell -File Tools\\shoot_gui.ps1 -Out docs\\screenshot.png -Width 1700 -Height 1240 -Wait 8

The top 40 pixels of it are the standalone's own title bar and are left out.
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
W, H = 1280, 640
BG = (13, 17, 25)            # the panel's own background
NAME = (86, 208, 196)        # the logo's arcs
TEXT = (224, 228, 232)
SMALL = (138, 147, 160)
FRAME = (44, 53, 68)


def font(name, size):
    try:
        return ImageFont.truetype(os.path.join("C:/Windows/Fonts", name), size)
    except OSError:
        return ImageFont.load_default()


img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)

# The whole panel without the standalone's title bar: voice, filters, space, the rooms, the Cosmos,
# the conductor and the modulation strip, as it looks when it is opened.
shot = Image.open(os.path.join(ROOT, "docs", "screenshot.png")).convert("RGB")
sw = shot.width
crop = shot.crop((0, 40, sw, shot.height))
pw = 810
ph = int(crop.height * pw / crop.width)
panel = crop.resize((pw, ph), Image.LANCZOS)
x0, y0 = W - pw - 30, (H - ph) // 2
img.paste(panel, (x0, y0))
d.rectangle([x0 - 1, y0 - 1, x0 + pw, y0 + ph], outline=FRAME, width=2)

logo = Image.open(os.path.join(ROOT, "docs", "logo-256.png")).convert("RGBA").resize((140, 140), Image.LANCZOS)
img.paste(logo, (56, 78), logo)
d.text((56, 240), "NOCTUARY", font=font("segoeuib.ttf", 48), fill=NAME)
y = 318
for line in ("A drone instrument for", "slowly breathing clusters:", "just intonation, no rhythm,", "a piece that plays all night."):
    d.text((58, y), line, font=font("segoeui.ttf", 24), fill=TEXT)
    y += 34
for line in ("VST3 and standalone for Windows,", "free and open source"):
    d.text((58, y + 16), line, font=font("segoeui.ttf", 18), fill=SMALL)
    y += 25

out = os.path.join(ROOT, "docs", "social-preview.png")
img.save(out, optimize=True)
print("wrote", os.path.relpath(out, ROOT), img.size)
