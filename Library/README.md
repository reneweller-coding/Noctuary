# The Noctuary preset library

Fourteen thousand three hundred and thirty-six presets in fifty-six packs, the two hundred and
fifty-six compiled into the instrument, and the material they play.

The packs are text and live in the repository. The audio does not -- 93 GB of clips, tables and
impulse responses -- so it is generated locally and ignored by git, and a released build downloads
it as a content package. Everything is deterministic: the same seeds always produce the same files
with the same names, which is why a pack can name a sample that has not been rendered yet.

```
Library/
  Packs/*.ambientpack      56 files, 256 presets each                    (in git)
  Textures/*.flac          5748 tonal clips from Stable Audio 3 medium: <category>-<id>_<Note>.flac,
                           49 categories, 24-bit 44.1 kHz stereo, 13-135 s, peak -1 dBFS
  FieldRecordings/*.flac   2938 seamless loops: fr-<category>-<id>_loop.flac, 36 categories, 40-115 s
  Wavetables/<shelf>/*.wav + .json   2191 tables on three shelves (Tools/library/wavetable_folders.py):
                           Harmonic/ harmonic_* (551, HarmonicGen, the Harmonic type, 64 frames);
                           Classic/ akwf_* and wavedit_* (640, AKWF and WaveEdit Online, CC0, the
                           classic Wavetable type, clm chunk); Ambient/ ambient_* (1000,
                           Tools/HarmonicGen/ambientgen.py, for either type): consonant 150, overtone
                           110, vowel_bass/tenor/alto/soprano 40 each, bowed 90, tube 90, sampled 250,
                           otmorph 150. Every table's .json says where it came from, whether it is
                           grounded, and -- for a vowel -- the note range it was sung in.
  Wavetables/Classic/CREDITS-classic.md   the AKWF/WaveEdit credits (not .txt: sort_clips.py deletes those)
  Impulses/*.wav + .json   1100 rooms: 1000 generated (Tools/ImpulseGen/roomgen.py) -- chamber, hall,
                           cathedral, cavern, vast, plate, the coupled bloom and far, the designed
                           drift and swell -- plus the curated diffusion set, nine real rooms, and the
                           350 files the released packs named (Tools/library/legacy_impulses.json),
                           which keep their names forever because a session keeps the path of its room.
                           Names ending in a/b are Room Morph pairs cut from one noise (pack field 9).
  tonality.json            what every clip measures (Tools/library/clip_tonality.py): harmonicity,
                           flatness, centroid, and the three health figures that keep a clip out of
                           the library -- offset, hole, seam.
```

The clips were generated in `G:/Tools/VRAudio/StableAudio3` (`build/`, `build_fr/`, the prompt lists
in `prompts/`); the category `tape-loop` was renamed `tapeloop` on the way in, because a `-loop` in a
name makes the engine treat a clip as a seamless loop (`build/library_renames.json`). The previous
library is backed up on `M:/Samples/Noctuary-Backup/2026-09-10` and `2026-09-11`.

## How a preset is made

The prompt lists say more about a clip than its file name can: the category it belongs to, its
gesture (sustain, chord, swell, melodic, struck), its harmony (a single note, a mode, a chord), its
world (dark, tape, luminous, cosmic, cold, ritual, warm, industrial, wild), which source types it was
written for -- and the artists it was written for. That is what the generator lives on.

```
Tools/library/clip_catalog.py    the library as data: every clip, table and impulse with its metadata
Tools/library/artists.py         56 artist styles and 16 built-in families: ranges, module weights,
                                 material (own clips, borrowed clips, categories, worlds, gestures),
                                 source types, table families, room families, word pools
Tools/library/make_library.py    the packs themselves, and the built-ins as staging packs
Tools/library/write_builtins.py  the staging packs compiled into Core/src/Presets.cpp
Tools/library/artists_research.md  what each artist's music actually does, and where that comes from
```

A style's clips are the clips made for its artist and for the artists it borrows from, leaned by
category, world and gesture, and never one the catalogue keeps out. The three source types that read
a clip take only clips written for them. A table from the Harmonic shelf plays as Harmonic, one from
Classic as the classic Wavetable, an ambient one as either; a table without a fundamental never
carries the first slot, a sung vowel keeps the conductor inside the range it was sung in, and a table
measured from a recording gets a floor under its fundamental. The key follows the material: a voice
on a clip in F# Dorian plays in F#, on the scale that holds the mode where the style has that scale,
and a clip that follows the note is played at the octave nearest the one it was recorded in.

The built-ins have to sound right on a machine where the library was never installed, so not one of
them names a file and their rooms are the synthetic reverbs -- the convolution Room would otherwise
keep whatever impulse was loaded before it. They are generated as ordinary packs into a staging
folder because everything that measures a preset measures packs, and compiled in afterwards.

## Building it

The whole chain, in order, is `Tools/library/rebuild_all.py`:

```
python Tools/library/rebuild_all.py --work build/library-work --jobs 12
```

```
1  presets    make_library.py       the 56 packs and the built-ins' staging packs
2  verify     verify_packs.py       every key and value actually exists
3  balance    rebalance_voice.py    the voice above its own noise bed, two renders per preset
4  measure    measure_packs.py      60 s per preset: descriptors, excerpts, the loudness window (LUFS) and the guide's gates
5  builtins   write_builtins.py     into Core/src/Presets.cpp, rebuilt, then measured from the binary
6  clap       clap_embed.py         what a model says the excerpts sound like (CPU: see the note there)
7  map        map_all.py            one layout over the whole library, the groups, the phrases
8  build      cmake + the selftest
```

It refuses to start while the GPU or another of these tools is busy, skips steps whose output is
already there, and keeps the previous run's caches beside the new ones so two rounds can be compared.

The material itself is generated by hand, because it takes hours of GPU time and is committed
separately: `make_textures.py` and `make_field_recordings.py` for the clips (Stable Audio 3),
`Tools/HarmonicGen/harmonicgen.py` and `ambientgen.py` for the tables, `Tools/ImpulseGen/roomgen.py`
for the rooms. `sort_clips.py` has the last word on which folder a clip belongs in, and
`clip_tonality.py` measures every clip afterwards -- a clip with an offset, a hole or a clicking seam
never reaches a preset.

### Texture from statistics

A recording can also be used as a description rather than as material. McDermott and Simoncelli
showed that sound texture -- rain, wind, fire, a crowd -- is recognised from a small set of
time-averaged statistics of the auditory periphery, not from the waveform: measure those on one
recording, impose them on noise, and a listener hears the same texture without hearing the same
sound. That is an endless bed with the character of the original and none of its repetition.

```
python Tools/library/texture_statistics.py --selftest
python Tools/library/texture_statistics.py --in-dir Library/Textures --out-dir Library/Textures \
    --count 40 --seconds 30 --report stats.csv
```

Every run reports how close it got and how much of the source is literally in the result. On eight
library recordings the moments and the cross-band correlations land within a few per cent and the
largest cross-correlation with any source is 0.066, so nothing is copied. The modulation power is the
loose figure and it separates the material honestly -- heavy rain 0.12, a glacier 0.42, a resonant
bronze bowl 0.92. A bell struck once is not a texture; the number will say so.

## Installing it

The synth loads every `*.ambientpack` in `$AMBIENT_PACKS` (one folder, or several separated by `;`),
and otherwise in `Documents/Noctuary/Packs`. A pack names its files relative to its own folder, as
`../Textures/...`, `../Wavetables/<shelf>/...` and `../Impulses/...`, so keep the folders together:

```
Documents/Noctuary/Packs             <- Library/Packs
Documents/Noctuary/Textures          <- Library/Textures
Documents/Noctuary/FieldRecordings   <- Library/FieldRecordings
Documents/Noctuary/Wavetables        <- Library/Wavetables
Documents/Noctuary/Impulses          <- Library/Impulses
```

A preset whose sample is missing still loads; it just leaves that slot empty.

## The pack format

```
# comment
pack <pack name>
format 2
<name>|<settings>|<x y bright motion width noisy bass density tagbits [loudness ...]>|<texture>|
<wavetable>|<impulse>|<mod matrix>|<env shapes, '~' between them>|<impulse B>
```

Nine fields; everything after the settings is optional. The texture field may name one clip or four
separated by `;`, one per source slot. `format 2` matters: a pack without that line was written
before the classic wavetable arrived and called the spectral type "Wavetable", so the loader
translates the name for it -- every pack in the library and every pack anybody wrote keeps the sound
it was voiced with.

## What is in the packs

Each pack is one corner of the drone repertoire, written in the spirit of an artist who works there.
Nothing is sampled from or affiliated with any of them: the packs are ranges over this synth's own
parameters, chosen by ear, and the clips are generated. `Tools/library/artists_research.md` says what
each artist's music actually does and where that description comes from; `Core/src/Help.cpp` has a
paragraph on every pack.

| Pack | In the spirit of | Pack | In the spirit of |
| --- | --- | --- | --- |
| Sleep Concert | Robert Rich | Street Resonance | BJ Nilsen |
| Deep Earth | Lustmord | Bowl Temple | Klaus Wiese |
| Desert Ember | Steve Roach | Temple of Air | Oöphoi |
| Ritual Machine | Deutsch Nepal | Star Rite | Inade |
| Arctic Loop | Biosphere | Dream Depth | Troum |
| Far Relay | Martin Stürtzer | Long Transit | S.E.T.I. |
| Planetary | Michael Stearns | Signal Algorithm | Bad Sector |
| Gentle Systems | Brian Eno | Cosmic Procession | Phelios |
| Vast Chord | Mathias Grassow | Pulsar Field | Arecibo |
| Ritual Stone | Raison d'Être | Slow Unfolding | Moljebka Pvlse |
| Frozen Gong | Thomas Köner | Field Absence | Francisco López |
| Harbour Rain | Loscil | Machine Depths | Tho-So-Aa |
| Hull Rumble | Sleep Research Facility | Field Recordings | Chris Watson |
| Forest Rite | Ulf Söderberg | Shaman Objects | Voice of Eye |
| Weather Station | Hazard | Ghost Signal | Bass Communion |
| Painted Field | Andrew Chalk | Atom Space | Atomine Elektrine |
| Millstone | Jonathan Coleclough | Hybrid Melancholy | Polygon |
| Glass Vitrine | Mirror | Corridor | Kammarheit |
| Slow Carousel | Mimir | Northern Dark | Gustaf Hildebrand |
| Chamber Grey | In Camera | Void Station | Tholen |
| Loop Studio | Colin Potter | Strings at Rest | Stars of the Lid |
| Still Meadow | Darren Tate | Tape Saturation | Tim Hecker |
| Water Hymn | ora | Drum Procession | Apoptose |
| Vegetal Drone | Monos | Shortwave Dark | Land:Fire |
| Sustain | Paul Bradley | Modular Nocturne | Ian Boddy |
| Fog City | Jeff Greinke | River Loops | Vidna Obmana |
| Low Brass Swell | Tom Heasley | Guitar Twilight | Jeff Pearce |
| Silk Road Space | Amir Baghiri | Water Tower Voices | Jim Cole |

The sixteen built-in families are the instrument's own sections rather than an artist each: Just
Drones, Glass and Bells, Choirs and Vowels, Deep and Sub, Bowed Strings, Organs and Reeds, Played
Keys, Generative Chords, Cosmos, Clouds and Memory, Weather and Noise, Metal and Feedback, Space and
Motion, Slow Worlds, Microtonal, Strike and Modal.

## Modulation in the library

Every preset carries a matrix -- about four and a half rows on average -- and most carry one or two
envelope shapes, plus the entrance contours of the sources that come in late. Which targets a preset
may use depends on what it actually switched on: `z_x` only where the z-plane filter runs,
`cloud_density` only where the cloud does.

Depths are read per target, because a depth is a fraction of the target's own range: 0.3 on a mix is
a third of it, 0.3 on the cutoff would be 5.4 kHz. The rates are drone rates, log-uniform between one
cycle in eight seconds and one in forty minutes, so the median route takes a bit over two minutes to
come round. That is deliberate, and it means a short render will not show much: the instrument is
built for half-hour pieces. The shade decides how much of it there is -- *still* gets one to three
slow rows, *astir* four to seven at 2.6 times the rate.

## Metadata

Every pack line carries a map position, six descriptors (brightness, motion, width, noisiness,
weight, density), tag bits, the loudness it plays at, three drone descriptors, two phrases and its
group, so the browser filters and the point map work on the library the same way they work on the
built-in presets.

Both halves are measured, not described: `map_all.py` renders the built-ins from the binary and reads
the packs' measurements from the cache `measure_packs.py` wrote, then lays one cloud out over all of
them. `make_library.py` alone only estimates the descriptors from the settings, which is enough to
place a preset on a provisional map but is a prediction.

## Checking it

`verify_packs.py` is the cheap pass: it parses every line and fails on an unknown parameter, a choice
name the synth does not know, a value outside its range, a duplicate preset name, a named file that
is not there, or two presets on the same spot of the map. A second for fourteen thousand presets, and
it runs before and after the map is laid out.

```
python Tools/library/verify_packs.py --packs Library/Packs
python Tools/preset_check.py --packs Library/Packs --sample 300 --jobs 6
```

`preset_check.py` renders presets and fails the ones that are too loud, clip, click, carry DC or come
out silent -- and, since 25.09.2026, the production guide's gates: a mono loss over 3 dB, a crest (true
peak over short-term loudness) under 12 dB, a true peak over -1 dBTP. `measure_packs.py` holds every
preset to -24 .. -18 LUFS integrated over its minute (it used to be an unweighted RMS window) and
writes `guide-report.json` beside its cache with the presets that fail a gate a gain cannot fix.

`Tools/library/retrofit_guide.py` is the pass that fitted the guide's depth model to the presets
written before it: the far reverb's own pre-delay to 3 ms in every preset (the gap between a sound
and its room belongs to the source now and follows its distance), the sub two octaves under the root.
It has run; after any change of this kind the library is re-measured, because the loudness of every
preset moves with it.

## Fitting new features into old presets

An instrument that gains a feature gains it for the presets written afterwards. `retrofit_presets.py`
walks a library and gives each preset the new things that suit it, judged from its own settings;
`Tools/retrofit_builtins.py` did the same for the compiled-in presets while those were written by
hand. Since 2.0 both halves are generated from the same styles, so a new feature belongs in
`artists.py` and the library is rebuilt instead.
