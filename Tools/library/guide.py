"""The production guide's windows for a preset's parameters (25.09.2026).

One table, used twice: `Tools/library/make_presets.py` passes every preset it generates through
`apply()` before it is written, and `Tools/library/retrofit_guide.py` passes every preset that was
written before through the same function. So the library complies by construction, and a future
change of a window is one edit.

What the guide asks for and what each window does -- the values are the guide's (docs/concept.md,
"The production guide, built in"):

    depth            the far plane must be reachable: the conductor's deep notes land at
                     Depth x (0.55 .. 1.0), and a Depth of 0.7 left the horizon unused. A value under
                     0.85 is mapped from the library's 0.4 .. 0.85 onto 0.85 .. 0.9625, the ordering
                     kept; 0.85 and above stays (so the pass is idempotent).
    far_highcut      the far room's low-pass at 2 to 3 kHz: capped at 3000 Hz (a preset written
                     darker stays darker).
    far_decay        the far room at 12 to 40 s; near_decay the near room at 0.6 to 1.5 s.
    far_width        the far layer is the widest: nothing under 1.0 (the funnel that pulled the
                     background in is what Near Width now does the other way round, on the source).
    presence         the near plane full and bright: at least 1 dB of the 3.2 kHz bell.
    pad_low_cut      the drone's fundament high-passed at 70 to 90 Hz, so the sub owns its register.
    bass_mono        no side under 100 Hz.
    subsonic         the 15 to 20 Hz high-pass on the output: 18 Hz.
    purity           pure intervals for a drone that stands longer than ten seconds: at least 0.9
                     wherever the scale is not 12-TET.
    detune           no unison cloud: the outer strands at most 4 cents apart; beat_ceiling at
                     0.5 Hz caps the beat any strand may make against its note.
    (air is not in the table: the guide's one quiet air layer is a mix decision, and Air here is
    band noise per voice -- sixteen voices of it would be a floor, not a layer.)
    width            the master's side no wider than 1.3.
    room_predelay    the main room at 10 to 25 ms; room_highcut at most 6 kHz (guide room B).
    srcN_grain       a texture's grains at least 80 ms; srcN_spread at most 5 % of the clip.
    cloud_size       the grain cloud's grains at least 80 ms.
    sub_octave       -2 (33 .. 62 Hz under a C3-B3 root).
    sub_binaural     a sub is mono: an offset between the ears becomes a Beat of 0.25 Hz in both
                     ears, the Lustmord swell -- except where Pulse rides on the offset (the
                     isochronic design of Foundation, which is kept).
    lfoN_sync        no two LFOs on bar divisions (2:1 period ratios): the first synced LFO keeps
                     its division, the others go free on their own rate.
    far_predelay     3 ms: the gap belongs to the source (Depth Gap) and the horizon has none.
"""
import re

TEXTURE_TYPES = ("Texture", "3")


def _num(p, key, default):
    try:
        return float(p.get(key, default))
    except (TypeError, ValueError):
        return default


def _fmt(v):
    return f"{v:.4g}"


def apply(p):
    """Bring one preset's parameters into the guide's windows. `p` maps key -> value text, in the
    preset's own order; returns the number of keys changed. Keys absent from `p` take the engine's
    defaults, which are the guide's since 25.09.2026, so only a key that stands in the way is set."""
    changed = 0

    def set_num(key, value):
        nonlocal changed
        new = _fmt(value)
        if p.get(key) != new:
            p[key] = new
            changed += 1

    def clamp(key, lo, hi, default=None):
        if key not in p and default is None:
            return
        v = _num(p, key, default if default is not None else 0.0)
        c = min(max(v, lo), hi)
        if c != v:
            set_num(key, c)

    def floor(key, lo, default=None):
        if key not in p and default is None:
            return
        v = _num(p, key, default if default is not None else 0.0)
        if v < lo:
            set_num(key, lo)

    def cap(key, hi):
        if key in p and _num(p, key, 0.0) > hi:
            set_num(key, hi)

    # the plane: the library's 0.4 .. 0.85 onto 0.85 .. 0.9625, the ordering kept; a value already
    # at 0.85 or above is left where it is, so the pass can run twice without moving anything
    if "depth" in p:
        d = _num(p, "depth", 0.7)
        if d < 0.85:
            t = min(max((d - 0.4) / 0.45, 0.0), 1.0)
            set_num("depth", 0.85 + 0.1125 * t)
    else:
        set_num("depth", 0.9)   # the engine's 0.7 would leave the horizon unused
    floor("presence", 1.0, default=0.0)
    set_num("far_predelay", 3.0)
    # the rooms
    cap("far_highcut", 3000.0)
    clamp("far_decay", 12.0, 40.0, default=25.0)
    clamp("near_decay", 0.6, 1.5, default=1.2)
    floor("far_width", 1.0)
    if _num(p, "room_level", 0.0) > 0.0:
        clamp("room_predelay", 10.0, 25.0, default=20.0)
        cap("room_highcut", 6000.0)
    # the low end
    clamp("pad_low_cut", 70.0, 90.0, default=0.0)
    floor("bass_mono", 100.0, default=150.0)
    floor("subsonic", 18.0, default=0.0)
    if _num(p, "sub_level", 0.0) > 0.0:
        if p.get("sub_octave", "-2") != "-2":
            p["sub_octave"] = "-2"; changed += 1
        if _num(p, "sub_binaural", 0.0) > 0.0 and _num(p, "sub_pulse", 0.0) <= 0.0:
            set_num("sub_binaural", 0.0)
            set_num("sub_beat", 0.25)
    # clarity
    if p.get("scale", "JI 7-limit") not in ("12-TET", "0"):
        floor("purity", 0.9, default=1.0)
    cap("detune", 4.0)
    if _num(p, "strands", 3) >= 2:
        bc = _num(p, "beat_ceiling", 0.0)
        if bc <= 0.0 or bc > 0.5:
            set_num("beat_ceiling", 0.5)
    cap("width", 1.3)
    # grains
    for i in range(1, 5):
        if str(p.get(f"src{i}_type", "")) in TEXTURE_TYPES:
            floor(f"src{i}_grain", 80.0, default=200.0)
            cap(f"src{i}_spread", 0.05)
    if _num(p, "cloud_send", 0.0) > 0.0:
        floor("cloud_size", 80.0, default=250.0)
    # motion: at most one LFO on a bar division
    synced = 0
    for i in range(1, 9):
        v = p.get(f"lfo{i}_sync", "Free")
        if v not in ("Free", "0", "", None):
            synced += 1
            if synced > 1:
                p[f"lfo{i}_sync"] = "Free"; changed += 1
    return changed


def parse(settings):
    """A settings string into an ordered dict of key -> value text."""
    out = {}
    for kv in settings.split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            out[k] = v
    return out


def serialise(p):
    return ";".join(f"{k}={v}" for k, v in p.items())


def apply_to_settings(settings):
    """The same over a settings string; returns (new string, keys changed)."""
    if not settings.strip():
        return settings, 0   # "Init": every default is the guide's
    p = parse(settings)
    n = apply(p)
    return serialise(p), n


ROW = re.compile(r'(\{\s*"([^"]+)",\s*)((?:"(?:[^"\\]|\\.)*"\s*)+)(?=[,}])')   # the name, then the settings literals up to the next field
WIDTH = 100


def wrap_literal(text, indent="      "):
    """One C++ string literal per line, split after ';' (write_builtins.py's wrap())."""
    parts, line = [], ""
    for piece in text.split(";"):
        piece = piece + ";"
        if line and len(line) + len(piece) > WIDTH:
            parts.append(line); line = piece
        else:
            line += piece
    if line:
        parts.append(line)
    if parts and parts[-1].endswith(";"):
        parts[-1] = parts[-1][:-1]
    return ("\n" + indent).join('"' + p + '"' for p in parts if p)


def apply_to_builtins(text):
    """Every settings literal of Core/src/Presets.cpp's kPresets rows; returns (text, presets changed)."""
    start = text.index("const Preset kPresets[] = {")
    end = text.index("\n};", start)
    head, body, tail = text[:start], text[start:end], text[end:]
    changed = 0

    def row(m):
        nonlocal changed
        settings = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(3)))
        new, n = apply_to_settings(settings)
        if n == 0:
            return m.group(0)
        changed += 1
        return m.group(1) + wrap_literal(new)
    body = ROW.sub(row, body)
    return head + body + tail, changed
