"""Measure a handful of presets again -- after an engine fix, after a hand edit -- and leave the
rest of the library exactly as it was measured.

    python Tools/library/remeasure_presets.py --work work/library --names "Horn Movement,Somnolent Swell"
    python Tools/library/remeasure_presets.py --work work/library --file names.txt
    python Tools/library/remeasure_presets.py --work work/library --below-db -45 --evo-above 20

measure_packs.py can render a subset too (--resume renders what the cache lacks), but it then
applies the loudness window to EVERY preset again, and a preset that was already lifted the six
decibels one pass allows gets lifted six more: three thousand of the fourteen thousand sit under
the window, and a second pass would have moved all of them (13.09.2026). So this does the same
three things measure_packs does -- the balance, the sixty-second measurement with its warm-up,
the loudness window -- for the named presets only, writes their lines, their cache rows and their
fingerprints, and touches nothing else.

What it does not do is the layout: run map_all.py afterwards (from the caches, seconds), and
clap_embed.py --resume for the new excerpts before it, so the map knows the new sound.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import measure_packs as mp  # noqa: E402

METHOD = "sec={sec}|notes=45,52,59|hour=21|rate=6"


def load_json(path):
    if not os.path.exists(path):
        return {}
    try:
        return json.load(open(path, encoding="utf-8"))
    except (ValueError, OSError):
        return {}


def save_json(path, data):
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    os.replace(tmp, path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True, help="the library's work folder (packs.json, balance-Packs.json, taps)")
    ap.add_argument("--packs", default=os.path.join(ROOT, "Library", "Packs"))
    ap.add_argument("--names", default="", help="comma-separated preset names")
    ap.add_argument("--file", default="", help="a file with one preset name per line")
    ap.add_argument("--below-db", type=float, default=None, help="every preset the cache measured under this loudness")
    ap.add_argument("--evo-above", type=float, default=None, help="every preset whose level travelled more than this (dB) in its minute")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--no-balance", action="store_true", help="skip rebalance_voice.py for them")
    ap.add_argument("--dry-run", action="store_true", help="say which presets, measure nothing")
    a = ap.parse_args()

    cache_path = os.path.join(a.work, "packs.json")
    fp_path = cache_path + ".fp"
    balance_path = os.path.join(a.work, "balance-" + os.path.basename(os.path.normpath(a.packs)) + ".json")
    taps = os.path.join(a.work, "taps")
    cache = load_json(cache_path)
    fps = load_json(fp_path)

    files = sorted(f for f in os.listdir(a.packs) if f.endswith(".ambientpack"))
    packs = {f: mp.read_pack(os.path.join(a.packs, f)) for f in files}
    where = {r["name"]: f for f, (_, rows) in packs.items() for r in rows}

    names = []
    if a.names:
        names += [n.strip() for n in a.names.split(",") if n.strip()]
    if a.file:
        names += [l.strip() for l in open(a.file, encoding="utf-8") if l.strip() and not l.startswith("#")]
    if a.below_db is not None:
        names += [n for n, m in cache.items() if float(m.get("rms_db", 0.0)) < a.below_db]
    if a.evo_above is not None:
        names += [n for n, m in cache.items() if float(m.get("evo_level", 0.0)) > a.evo_above]
    seen = set()
    names = [n for n in names if not (n in seen or seen.add(n))]
    missing = [n for n in names if n not in where]
    if missing:
        sys.exit("not in the packs: " + ", ".join(missing[:10]))
    if not names:
        sys.exit("nothing to measure")
    print(f"{len(names)} presets to measure again")
    for n in names[:40]:
        m = cache.get(n, {})
        print(f"  {n:34s} was {float(m.get('rms_db', 0.0)):7.1f} dBFS, level travel {float(m.get('evo_level', 0.0)):5.1f} dB  ({where[n]})")
    if len(names) > 40:
        print(f"  ... and {len(names) - 40} more")
    if a.dry_run:
        return 0

    # 1. The balance, for these only: their entries leave the balance cache, and --resume then
    #    balances exactly what the cache no longer vouches for.
    if not a.no_balance:
        bal = load_json(balance_path)
        n_drop = sum(1 for n in names if bal.pop(n, None) is not None)
        save_json(balance_path, bal)
        # By name, not by --resume alone: the balance's fingerprint covers the whole pack line, which
        # the measurement and the map have rewritten since, so on its own --resume would balance
        # the whole library again.
        names_path = os.path.join(a.work, "rebalance-names.txt")
        with open(names_path, "w", encoding="utf-8") as f:
            f.write("\n".join(names) + "\n")
        print(f"\nbalance: {n_drop} entries dropped from {os.path.basename(balance_path)}; rebalance_voice.py --names-file --resume")
        r = subprocess.run([sys.executable, os.path.join(HERE, "rebalance_voice.py"), "--packs", a.packs,
                            "--jobs", str(a.jobs), "--cache", balance_path, "--resume", "--names-file", names_path], cwd=ROOT)
        if r.returncode:
            sys.exit("the balance failed")
        packs = {f: mp.read_pack(os.path.join(a.packs, f)) for f in files}   # the balance rewrote the lines

    rows = {r["name"]: r for f, (_, rr) in packs.items() for r in rr if r["name"] in names}

    # 2. The measurement: the same warm-up buckets, the same sixty seconds, the same excerpts.
    os.makedirs(taps, exist_ok=True)
    buckets = {}
    for n in names:
        buckets.setdefault(mp.warm_up(rows[n]["settings"]), []).append(n)
    print("\nmeasure: warm-up " + ", ".join(f"{int(k)} s x{len(v)}" for k, v in sorted(buckets.items())), flush=True)
    got = {}
    import concurrent.futures
    chunks = [(lead, ns[i:i + 10]) for lead, ns in sorted(buckets.items()) for i in range(0, len(ns), 10)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for part in ex.map(lambda c: mp.render_batch(c[1], a.packs, a.seconds, taps, c[0]), chunks):
            got.update(part)
    failed = [n for n in names if got.get(n) is None]
    if failed:
        print(f"  {len(failed)} did not render: {', '.join(failed[:8])}")

    # 3. The loudness window, on these rows only, exactly as measure_packs applies it.
    method = METHOD.format(sec=a.seconds)
    touched = set()
    print()
    print(f"{'preset':34s} {'was':>8s} {'measured':>9s} {'gain':>6s} {'now':>8s}")
    for n in names:
        m = got.get(n)
        if m is None:
            continue
        r = rows[n]
        rms = m["rms_db"]
        delta = 0.0
        if rms > mp.TARGET_HI:
            delta = mp.TARGET_HI - rms
        elif rms < mp.TARGET_LO:
            delta = min(mp.TARGET_LO - rms, 6.0)
        if abs(delta) > 0.2:
            r["settings"] = mp.set_gain(r["settings"], mp.get_gain(r["settings"]) + delta)
        else:
            delta = 0.0
        # The meta line's tenth token is the loudness the preset now plays at; the rest of the
        # line is the layout's, and map_all writes it again from the cache.
        toks = r["meta"].split()
        if len(toks) >= 10:
            toks[9] = f"{rms + delta:.1f}"
            r["meta"] = " ".join(toks)
        was = float(cache.get(n, {}).get("rms_db", 0.0))
        cache[n] = dict(m, rms_db=rms + delta)
        fps[n] = mp._fingerprint(r, method)
        touched.add(where[n])
        print(f"{n:34s} {was:8.1f} {rms:9.1f} {delta:+6.1f} {rms + delta:8.1f}")

    for f in sorted(touched):
        head, rr = packs[f]
        mp.write_pack(os.path.join(a.packs, f), head, rr)
    save_json(cache_path, cache)
    save_json(fp_path, fps)
    print(f"\nwrote {len(touched)} packs, {len(names) - len(failed)} cache rows; now: clap_embed.py --resume, then map_all.py")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
