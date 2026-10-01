"""Put the instrument's voice above its own noise bed. Measured, one preset at a time.

Every preset has two kinds of material in it. One is pitched to what is played -- the source slots
and everything downstream of them, their filters, their reverb. The other is not: Air and Breath,
a noise slot, the Foundation, the strike bed. Only the first can be heard as harmony, and if the
second is louder the preset is a wash that ripples when notes come and go.

That is what the library was. `make_presets.fill_slot` gives EVERY slot a level drawn from
0.15 .. 0.55 -- a figure meant for a supporting layer -- and the first slot is the voice. A preset
becomes Additive only when no type is drawn at all, and then keeps the parameter's own default of
1.0. So the additive presets stood at full level over the bed and every other preset stood at a
quarter to a third of it, over the same bed. Measured on two of them, source muted against the
whole preset:

    Bardo Stillness   voice 8.3 dB BELOW the rest of the preset
    Hymn Span         voice 11.5 dB below

Hence "only the additive presets are played tonally": the others were there, in tune, following
the note exactly -- and inaudible under their own Air.

Numbers, not guesses. Each preset is rendered twice at the same clock hour: once whole, once with
every source slot at zero. The difference in energy is the voice, what is left is the bed, and
their ratio says how much has to move. The voice is raised first, as far as 1.0, because that
leaves the preset its character and changes only the balance; Air and Breath are lowered only for
what the voice alone cannot cover, and never below a floor, so the air in the room stays.

Presets whose first slot is Noise are left alone: there the bed IS the piece.

    python Tools/library/rebalance_voice.py --limit 40 --dry-run
    python Tools/library/rebalance_voice.py --jobs 12

Afterwards the library has to be measured again: this changes loudness and every descriptor.
"""
import argparse, concurrent.futures as cf, glob, hashlib, json, math, os, re, statistics, subprocess, sys


def fingerprint(line, method=""):
    """What a preset's whole pack line amounts to, AND how it was measured. The name alone will not
    do -- regenerate the library and most names come back with other settings underneath them --
    and neither does the line alone: change the length of the window or the warm-up before it and
    every stored answer belongs to the old method while the line that keys it has not moved. The
    method is derived from the arguments in force and from this preset's own warm-up, so changing
    the rule re-balances exactly the presets whose warm-up moved and leaves the rest alone."""
    return hashlib.sha1((line + "\x1f" + str(method)).encode("utf-8")).hexdigest()[:16]


def warm_up(pairs, max_skip):
    """How long this preset is played and thrown away before the measured window begins."""
    return min(max_skip, max(0.0, get(pairs, "attack", 6.0) - 10.0))

SLOT_LEVEL = {"1": "osc_level", "2": "src2_level", "3": "src3_level", "4": "src4_level"}
# What the voice is measured against, and what may be taken down to make room for it. The
# Foundation joined the list on 12.09.2026. It had always been in the MEASUREMENT -- the bed render
# mutes the source slots, so whatever the sub makes is counted -- but it could not be touched, and
# the voice can only be raised as far as level 1.0. Measured on "Lamp Room": the voice stood 11.5 dB
# under its bed, the balance could lift it 3.2 dB and could cut nothing, because that preset has
# air at zero and all of its bed is the sub. Muting its three sources changed the output by 0.3 dB;
# muting the sub as well left silence. The whole preset WAS its foundation.
#
# Its floor is much higher than the room's. A drone without air stops sounding large; a drone
# without a foundation stops standing on anything at all, and Rene's own table has the foundation
# as the loudest, steadiest role there is. So it may be taken down, further than air and breath,
# but never below a quarter of the way up.
BED = ("air", "breath", "sub_level")
BED_DEFAULT = {"air": 0.15, "breath": 0.0, "sub_level": 0.0}
BED_FLOOR = {"air": 0.03, "breath": 0.03, "sub_level": 0.25}
RMS_RE = re.compile(r"measure: rms=(-?[\d.]+)")


# ---------------------------------------------------------------- the pack line
def parse_settings(text):
    out = []
    for tok in text.split(";"):
        if "=" in tok:
            k, _, v = tok.partition("=")
            out.append([k, v])
        elif tok:
            out.append([tok, None])
    return out


def write_settings(pairs):
    return ";".join(k if v is None else f"{k}={v}" for k, v in pairs)


def get(pairs, key, default):
    for k, v in pairs:
        if k == key:
            try:
                return float(v)
            except (TypeError, ValueError):
                return default
    return default


def get_str(pairs, key, default=""):
    for k, v in pairs:
        if k == key:
            return v if v is not None else default
    return default


def put(pairs, key, value):
    text = f"{value:.4g}"
    for pair in pairs:
        if pair[0] == key:
            pair[1] = text
            return
    pairs.append([key, text])


def active_slots(pairs):
    """Which slots make a sound, and at what level."""
    out = {}
    for s in "1234":
        t = get_str(pairs, f"src{s}_type", "Additive" if s == "1" else "Off")
        if t in ("Off", "", "Noise"):
            continue
        out[s] = get(pairs, SLOT_LEVEL[s], 1.0 if s == "1" else 0.5)
    return out


# ---------------------------------------------------------------- the measurement
def render_rms(exe, name, seconds, hour, sets, env, skip=0.0):
    cmd = [exe, "--preset", name, "--seconds", str(seconds), "--hour", str(hour), "--measure"]
    if skip > 0.0:
        cmd += ["--skip", f"{skip:.1f}"]
    for k, v in sets:
        cmd += ["--set", f"{k}={v}"]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300, env=env)
    except subprocess.TimeoutExpired:
        return None
    m = RMS_RE.search(r.stdout)
    return float(m.group(1)) if m else None


def probe(job):
    exe, name, seconds, hour, slots, env, skip, target_db, floors = job
    mute = [(SLOT_LEVEL[s], 0) for s in slots]
    whole = render_rms(exe, name, seconds, hour, [], env, skip)
    bed = render_rms(exe, name, seconds, hour, mute, env, skip)
    # A third render, only where the bed will have to give: the bed with every key it may touch
    # taken down to its floor. What is left is what the balance CANNOT reach -- the foundation
    # already at its floor, a patina, a room -- and without this number the decision below cut
    # keys that made no difference, four decibels a run, and called the preset balanced each
    # time. Measured on "Bog Winter", 12.09.2026: breath at its floor moved the bed by 0.00 dB,
    # because the bed was the sub, and the sub was at its floor already. Two runs cut breath by
    # eight decibels between them and the voice stood 7.9 dB under the bed after both.
    floor_bed = None
    if whole is not None and bed is not None and floors:
        e_whole, e_bed = 10.0 ** (whole / 10.0), 10.0 ** (bed / 10.0)
        e_voice = e_whole - e_bed
        if e_voice > 0.0 and e_bed > 0.0:
            have = 10.0 * math.log10(e_voice / e_bed)
            headroom_db = 20.0 * math.log10(min(1.0 / max(l, 1e-6) for l in slots.values()))
            if target_db - have - headroom_db > 0.25:
                floor_bed = render_rms(exe, name, seconds, hour, mute + floors, env, skip)
    return name, whole, bed, floor_bed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--packs", default="Library/Packs")
    ap.add_argument("--render", default="bin/msvc/ambient_render.exe")
    ap.add_argument("--target-db", type=float, default=0.0,
                    help="how far the voice stands above the bed. Not a taste: the additive presets, the ones "
                         "that already sound played, measure at a median of -0.1 dB over a sample of 90.")
    ap.add_argument("--max-bed-cut", type=float, default=4.0,
                    help="how far Air and Breath may be taken down. They are the room, and a preset that loses "
                         "them stops sounding large; better to leave one short of the target than hollow.")
    ap.add_argument("--seconds", type=float, default=30.0, help="the measured window, the same for every preset")
    ap.add_argument("--max-skip", type=float, default=75.0, help="cap on the warm-up played before it")
    ap.add_argument("--max-sub-cut", type=float, default=9.0,
                    help="how far the Foundation may be taken down when it is what buries the voice. Further than "
                         "air and breath, because a sub that drowns the body is not a floor, it is the whole house.")
    ap.add_argument("--hour", type=float, default=12.0, help="the clock is an input; hold it still or nothing repeats")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 8) - 2))
    ap.add_argument("--limit", type=int, default=0, help="measure only the first N, to try the thing out")
    ap.add_argument("--only-type", default="", help="only presets whose first slot is this type -- for asking "
                                                    "the additive presets, which sound right, what the target should be")
    ap.add_argument("--every", type=int, default=1, help="take every Nth preset, for a spread sample")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--cache", default="", help="JSON of what this run left balanced, so the next one can skip it")
    ap.add_argument("--resume", action="store_true", help="leave alone every preset the cache says is already balanced")
    # A list of names, and nothing else is looked at. --resume alone cannot narrow a run after the
    # measurement has been through the packs: its fingerprint covers the whole line, and
    # measure_packs and map_all rewrite every line (gain, descriptors, positions), so after them
    # every preset looks new to it. A run meant for six hundred presets started on fourteen thousand
    # that way (13.09.2026).
    ap.add_argument("--names-file", default="", help="balance only the presets named in this file, one per line")
    a = ap.parse_args()

    if not os.path.exists(a.render):
        sys.exit(f"no renderer at {a.render}")
    # CreateProcess wants a real path, not the shell's idea of one.
    a.render = os.path.normpath(os.path.abspath(a.render))
    files = sorted(glob.glob(os.path.join(a.packs, "*.ambientpack")))
    if not files:
        sys.exit(f"no packs in {a.packs}")

    # Read everything first: one pass to measure, one to write.
    book = []           # (path, line index, name, pairs)
    raw = {}            # path -> list of lines
    for path in files:
        raw[path] = open(path, encoding="utf-8").read().split("\n")
        for i, line in enumerate(raw[path]):
            if line.startswith("#") or "|" not in line:
                continue
            fields = line.split("|")
            book.append((path, i, fields[0].strip(), parse_settings(fields[1])))
    if a.only_type:
        book = [b for b in book if get_str(b[3], "src1_type", "Additive") == a.only_type]
    if a.every > 1:
        book = book[::a.every]
    if a.limit:
        book = book[:a.limit]
    if a.names_file:
        wanted = {l.strip() for l in open(a.names_file, encoding="utf-8") if l.strip() and not l.startswith("#")}
        book = [b for b in book if b[2] in wanted]
        print(f"  {len(book)} of the {len(wanted)} named presets found in the packs")

    env = dict(os.environ)
    env.setdefault("AMBIENT_PACKS", os.path.abspath(a.packs))

    # What this run may skip. Balancing CHANGES a preset -- it moves the slot levels and the bed --
    # so what is remembered is the fingerprint of the line as it was LEFT, and a preset whose line
    # still looks like that has already been balanced and needs neither of its two renders. Change
    # fifteen hundred presets out of fourteen thousand and this run costs a quarter of an hour
    # instead of two and a half hours. (The measurement beside it has worked this way for a while;
    # the balance never did, which is why every round re-measured a library that had hardly moved.)
    def method_of(pairs):
        return (f"sec={a.seconds}|hour={a.hour}|target={a.target_db}|cut={a.max_bed_cut}"
                f"|subcut={a.max_sub_cut}|bed={'+'.join(BED)}|skip={warm_up(pairs, a.max_skip):.1f}")

    balanced = {}
    if a.cache and a.resume and os.path.exists(a.cache):
        try:
            balanced = json.load(open(a.cache, encoding="utf-8"))
        except (ValueError, OSError):
            balanced = {}
    done_already = set()
    if balanced:
        for path, i, name, pairs in book:
            if balanced.get(name) == fingerprint(raw[path][i], method_of(pairs)):
                done_already.add(name)
        print(f"  {len(done_already)} presets are already balanced and are left alone")

    jobs, skipped_noise = [], 0
    for path, i, name, pairs in book:
        if name in done_already:
            continue
        slots = active_slots(pairs)
        if not slots:                      # a preset whose voice is a noise slot: the bed is the piece
            skipped_noise += 1
            continue
        # Measured after the preset has arrived, not while it is arriving. A flat forty seconds was
        # written when the slowest attack in the library was under it; the 2.0 library has a median
        # attack of 18.5 s and an eighth of it above forty. Measured on six of those: the voice
        # stands 4.0 dB lower against its bed at forty seconds than at ninety, every one in the same
        # direction -- so the balance was raising them about four decibels too far, and they arrive
        # too loud once the attack finally gets there. The warm-up is thrown away, and the window
        # that follows is the same length for every preset, so the ratios stay comparable.
        lead = warm_up(pairs, a.max_skip)   # the same rule the fingerprint above is keyed on
        floors = [(key, BED_FLOOR[key]) for key in BED if get(pairs, key, BED_DEFAULT[key]) > BED_FLOOR[key]]
        jobs.append((a.render, name, a.seconds, a.hour, slots, env, lead, a.target_db, floors))

    print(f"{len(jobs)} Presets messen ({skipped_noise} uebersprungen), {a.jobs} parallel, "
          f"{a.seconds:.0f} s je Render, zwei Renders je Preset")
    result = {}
    done = 0
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for name, whole, bed, floor_bed in ex.map(probe, jobs):
            result[name] = (whole, bed, floor_bed)
            done += 1
            if done % 100 == 0:
                print(f"  {done}/{len(jobs)}", flush=True)

    # ------------------------------------------------------------ decide and write
    before, after = [], []
    raised = cut = failed = ok_already = 0
    slot_db, bed_db = [], []
    at_limit = []          # presets the balance cannot bring to target, and by how much they miss it
    touched_files = set()
    for path, i, name, pairs in book:
        if name in done_already:
            ok_already += 1
            continue
        whole, bed_rms, floor_rms = result.get(name, (None, None, None))
        if whole is None or bed_rms is None:
            failed += 1
            continue
        e_whole, e_bed = 10.0 ** (whole / 10.0), 10.0 ** (bed_rms / 10.0)
        e_voice = e_whole - e_bed
        if e_voice <= 0.0 or e_bed <= 0.0:
            # Either the bed is silent (nothing to fix) or the voice measured as nothing at all.
            ok_already += 1
            continue
        have = 10.0 * math.log10(e_voice / e_bed)
        before.append(have)
        need = a.target_db - have
        if need <= 0.25:
            ok_already += 1
            after.append(have)
            continue
        slots = active_slots(pairs)
        # Raise every slot by the same factor, so the mix between them is kept. The factor is
        # capped by whichever slot reaches 1.0 first.
        headroom = min(1.0 / max(l, 1e-6) for l in slots.values())
        want = 10.0 ** (need / 20.0)
        factor = min(want, headroom)
        if factor > 1.0001:
            for s, level in slots.items():
                put(pairs, SLOT_LEVEL[s], min(1.0, level * factor))
            slot_db.append(20.0 * math.log10(factor))
            raised += 1
        # Whatever the voice could not reach comes off the bed. The room (air, breath) gives up
        # little, because a drone that loses it stops sounding large; the foundation gives up more,
        # because a foundation that drowns the body is not a foundation, it is the whole building.
        # And no more than the bed can actually give: the third render says how far the bed falls
        # with every movable key at its floor, and a cut past that point changes the preset's
        # numbers without changing its sound. Where that leaves the voice short, the preset is
        # written down as at its limit rather than counted as balanced -- that list is the
        # generator's to answer, not this tool's.
        still = need - 20.0 * math.log10(factor)
        reach = None
        if floor_rms is not None:
            e_floor = 10.0 ** (floor_rms / 10.0)
            reach = max(0.0, 10.0 * math.log10(e_bed / max(e_floor, 1e-12)))
        rest = min(still, max(a.max_bed_cut, a.max_sub_cut))
        if reach is not None:
            rest = min(rest, reach)
        if still > 0.25 and (reach is not None and reach < 0.25):
            at_limit.append({"name": name, "short_db": round(still, 2), "reach_db": round(reach, 2),
                             "voice": [get_str(pairs, f"src{s}_type", "Additive" if s == "1" else "Off") for s in slots],
                             "bed": {key: get(pairs, key, BED_DEFAULT[key]) for key in BED}})
        got = 0.0
        if rest > 0.25:
            moved = False
            for key in BED:
                v = get(pairs, key, BED_DEFAULT[key])
                floor = BED_FLOOR[key]
                if v <= floor:
                    continue
                allowed = min(rest, a.max_sub_cut if key == "sub_level" else a.max_bed_cut)
                # What the floor actually let through, which is what the report may claim.
                after_v = max(floor, v * (10.0 ** (-allowed / 20.0)))
                got = max(got, 20.0 * math.log10(v / max(after_v, 1e-9)))
                put(pairs, key, after_v)
                moved = True
            if moved:
                bed_db.append(-got)
                cut += 1
        after.append(have + 20.0 * math.log10(factor) + max(0.0, got))
        fields = raw[path][i].split("|")
        fields[1] = write_settings(pairs)
        raw[path][i] = "|".join(fields)
        touched_files.add(path)

    if not a.dry_run:
        for path in touched_files:
            open(path, "w", encoding="utf-8", newline="\n").write("\n".join(raw[path]))
        # And what the lines look like now, so the next run can tell a preset it has already
        # balanced from one that was regenerated underneath it. Written for every preset in the
        # book, not only the ones that moved: a preset already in balance is just as done.
        if a.cache:
            keep = {name: fingerprint(raw[path][i], method_of(pairs)) for path, i, name, pairs in book}
            json.dump(keep, open(a.cache, "w", encoding="utf-8"))

    def dist(v, label):
        if not v:
            return
        v = sorted(v)
        q = lambda p: v[int(p * (len(v) - 1))]
        print(f"  {label:32s} Median {statistics.median(v):+6.1f} dB   10% {q(.1):+6.1f}   90% {q(.9):+6.1f}"
              f"   unter 0 dB: {100 * sum(x < 0 for x in v) / len(v):3.0f} %")

    print(f"\nStimme gegen Rauschteppich (Ziel {a.target_db:+.0f} dB)")
    dist(before, "vorher")
    dist(after, "nachher (gerechnet)")
    print(f"\n  schon in Balance   {ok_already}")
    print(f"  Stimme angehoben   {raised}" + (f", Median +{statistics.median(slot_db):.1f} dB" if slot_db else ""))
    print(f"  Teppich gesenkt    {cut}" + (f", Median {statistics.median(bed_db):.1f} dB" if bed_db else ""))
    print(f"  nicht messbar      {failed}")
    if at_limit:
        short = [x["short_db"] for x in at_limit]
        print(f"  am Anschlag        {len(at_limit)}, Median {statistics.median(short):.1f} dB unter dem Ziel: "
              f"die Stimme steht am Maximum und das Bett kann nichts mehr hergeben")
        if a.cache and not a.dry_run:
            where = a.cache + ".limits.json"
            json.dump(at_limit, open(where, "w", encoding="utf-8"), indent=1)
            print(f"  Liste: {where}")
    print("\n(Probelauf -- nichts geschrieben)" if a.dry_run
          else "\nDie Bibliothek muss neu vermessen werden: das aendert Lautheit und alle Deskriptoren.")


# Guarded, after 12.09.2026: a script that imported this module for its helpers started a full
# balance of the library -- fourteen thousand presets, no cache -- before its own first line ran.
if __name__ == "__main__":
    main()
