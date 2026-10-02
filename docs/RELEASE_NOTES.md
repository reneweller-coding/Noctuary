# Noctuary release notes

## 2.4.0 (03.10.2026): the family plays together

**The family jam** (Settings > Family jam: Off, Lead or Follow; in the plugin and in the standalone). The five
instruments -- Totality, Parhelion, Ephemeris, Phosphene and Noctuary -- play as one band on the local network.
Following, Noctuary's Cluster Brain holds the leader's root (the nearest such note to where it stands) and the voices'
filters close with a quiet section of the leader's and open with a loud one; it keeps its own tuning. Leading, it
gives the others its root and its tuning, and they move to them -- the root at their next bar line, the mode with
their next track. A leader that falls silent for four seconds leaves its followers to themselves.

**Score cues for a visualiser** (Settings > Score cues for a visualiser, off to begin with): the clock's bar line
(/noct/bar), the key the conductor plays in (/noct/key) and a new scene when a preset or a journey's step arrives
(/noct/scene), over OSC to 127.0.0.1:9000. KaleidoscopeEnhanced understands the score cues of all five instruments as
they come (since 02.10.2026) -- Totality's blocks and keys, Parhelion's sections, Ephemeris' phases, Phosphene's
sections and drops, Noctuary's bars, keys and scenes -- and cuts its pictures to them, on a drop at once.

**An Audio Unit on the Mac, an LV2 on Linux.** The macOS zip has the Audio Unit beside the standalone and the VST3
(Logic, GarageBand, MainStage), passed by Apple's `auval -strict` on GitHub's runners; the Linux archive has the LV2
(Ardour, Carla, Reaper, Qtractor), read by lilv there.

**Icons where the action is unambiguous.** The disk and the folder (a preset saved and loaded), the red disc (record),
and the journey's square (stop) and disk (save) are vector glyphs now, sharp at every window size, their words in the
tooltips; everything else keeps its name.

## 2.3.0 (02.10.2026): with a DAW, on a Mac, and heard

**MIDI out.** In a DAW the plugin sends what the conductors play, the moment it sounds: channel 1 the Cluster
Brain, 2 the second brain, 3 the near events. A MIDI track can keep a night's chords, or hand them to another
instrument. The keys you play are not echoed.

**The planes as outputs of their own.** Besides the main output a stereo output each for Near, Far, Cosmos and Room,
off until the DAW switches them on; each carries its plane while the main output plays on.

**Ableton Link** in the standalone (Settings > Ableton Link, off to begin with): with Clock > Source on Host, the
session's tempo, bars and start and stop drive the clock; alone in the session Noctuary offers its own tempo.

**On a Mac.** Every release gets a build for Apple Silicon (macOS 12 or newer) -- the standalone and the VST3 --,
built and tested on GitHub's runners by the workflow `macos` and attached to the release as `Noctuary-<version>-macOS.zip`.
It is signed ad hoc, not notarized (that takes a paid Apple account; README-macOS.txt in the zip says how to open it),
and it has not yet been played on a real Mac.

**Demos.** The release "demos" holds a track per style as an MP3 and one of them as a video with pictures by
[KaleidoscopeEnhanced](https://github.com/reneweller-coding/KaleidoscopeEnhanced), its cuts placed by the track's own
score cues; `Tools/demo/make_demos.py` renders them all again.

**Behind the panel.** A sound check (`Tools/soundcheck.py`, `ctest -L sound`): three presets, a minute each, and their four planes, measured by loudness
(BS.1770) against `Tests/golden/soundcheck.json`, so a change that makes a style louder, quieter or emptier shows up
before anybody listens. A CI run on every push (GitHub Actions: the build and the tests on Windows, the documentation
check on Linux) that nobody waits for. One release script for the family (`Deploy/publish_release.ps1`: the notes from
this file, the checksums, the tag, the release).

**On Linux** (the same evening, attached to the release afterwards): an archive for x86-64 with the
standalone, the VST3 and the renderer, built by the workflow `linux` on GitHub's Ubuntu 22.04 runners
(GCC 12), its quick tests run there, and tried under WSL: the window, and the sound check against the
Windows renders.

## 2.2.0 (01.10.2026): the family's panel and layout

**The family's panel.** Undo, Redo, Help and the settings at the right of the header, as every instrument of the family
has them; the layout, the session recall and About in the settings; the VR controls only while a headset sends.

**One layout for the repositories.** Every instrument of the family builds the same way now: `build.ps1` (msvc, icx,
release, quest) on the presets of `CMakePresets.json`, the build trees under `build\<preset>`, everything that can be
started -- the standalone, the VST3, the renderer -- flat in `bin\msvc` and `bin\icx` (and the Quest APK in
`bin\quest`), the release in `dist\`, local data, renders and logs in `work\` (`cmake/Family.cmake`).

**Every line explained.** Every class, function, variable, macro and table of the sources -- the core, the plugin,
the Quest app, the tools and the tests -- has its Doxygen comment now, and the test `doccheck` (`cmake/Family.cmake`)
fails as soon as one is missing. The scripts that generate tables write the comments into what they generate.

## 2.1.0

**Five analogue filters modelled on their circuits, a production guide built into the engine, and a
conductor held to the harmony of the genre -- and every preset measured again for it.**

### Five circuit filters

The voice filter has five new models behind the same knobs: **Moog** (the transistor ladder),
**SEM** (Oberheim's state-variable filter), **Prophet** and **Juno** (the OTA cascades of the
SSM2040 and the IR3109) and **Diode** (the diode ladder of the EMS and the TB-303). They are the
circuits' differential equations with their nonlinearities where the circuits have them, and the
loop through the resonance is solved exactly every sample -- three Newton-Raphson steps on the
circuit's own Jacobian -- rather than broken by a sample of delay. So the resonance rings on
Cutoff at every setting: after an impulse at 440 Hz all four ladders and cascades ring within
1.4 % of it, and with Key Track at 1 the ringing plays the note. Prophet, Juno and Diode sing on
their own at full Resonance; the Moog stops a hair short and rings out.

**Morph** (Filter section, new) is the SEM's knob: low pass, a notch at 0.5, high pass at 1, and
every blend between -- slowly modulated it moves a drone's colour without the sound of a sweep. On
the five circuits, **Drive** is the level into the circuit, whose own stages saturate.

They run at twice the rate, both channels in one register: a full-scale sine into a fully driven
Moog aliases at -75 dB at the base rate and at -101 dB, the measurement's floor, at twice the rate,
and four times measured no better. They are still the dearest filters in the instrument -- 470 to
730 cycles a stereo sample against 32 for LP 12 -- and cost only where a preset uses one.

**Faster everywhere else.** The oversampler's sums are vector sums now: the four-times round trip
that every Drive, wavefolder, Patina, air and feedback stage uses went from 197 to 96 cycles.

### The library moved to them

3583 presets play a circuit filter now: every one that used the Ladder (to the Moog, its corner and
its feedback where the old ladder had them), seven in ten LP 24 (to a four-pole circuit by the
family: the cold ones to the diode ladder, the deep and ritual ones to the Moog, the luminous and
sleeping ones to the Juno), six in ten Notch and one in two HP 12 (to the SEM's notch and high
pass), and one in five LP 12 (to the SEM). None whose voice filter is not heard -- a Replace
z-plane, or a parallel one at full mix. Cutoff and Resonance were set so that each new filter's
curve lies as close to the old one's as the circuit allows.

Every move but LP 12 to the SEM was rendered on both filters, 1832 presets, and each preset's
level corrected by what it measured. Rendered as the library is measured, 96 of the moved presets
came out at a median of 0.0 dB against their measured loudness, all within 2 dB.

The setup and the portable zip carry the new packs; **Noctuary-2.1.0-Packs.zip** has them on their
own, for updating an installation by copying a folder. They need 2.1.0: an older version does not
know the five filters' names and would play those presets through LP 6.

**The old Ladder was measured wrongly.** Its sample of delay turns the loop positive at Nyquist,
and with its corner pushed near the top by the envelope or key tracking it oscillated at 24 kHz at
full scale. Nobody hears that; the library's measurement did, and fifteen presets had been turned
down for it, "Envelope Hollow" by sixteen decibels. They play at their real loudness now, and the
Ladder, which stays in the menu, can no longer do it.

### The production guide, built in

The guide for dark ambient and drone production is in the engine now, with its numbers as the
defaults, so every preset plays by it without being touched:

* **Depth** (Space): a source loses Range dB between the ear and the horizon (20 by default, where
  it was a fixed 6), its far send waits a Gap (40 ms) at the ear and none on the horizon, and its
  strands fan out to Near Width of their spread at the ear and to all of it on the horizon.
* **The low end**: the sends to the rooms are high-passed (Send Low Cut, 150 Hz), the sub sits two
  octaves down with its second and third harmonics (Harmonics) and an optional slow beat, and it
  has a ceiling of its own (-6 dBFS) before the sum.
* **The rooms in series**: the convolution room feeds the far hall (To Far, 0.15), so the horizon
  sounds like the same place going on; the far return's middle is high-passed from 300 Hz (Mid Low
  Cut) and its sides are not.
* **Ducking in seven bands**: the foreground ducks the background in seven bands an octave apart
  (150 Hz to 4.8 kHz), with a 20 ms attack and a half-second return, and the room is ducked as well.
* **Four times the rate on everything nonlinear**: Drive, the wavefolder, the Patina, the air
  ahead of the far hall and the feedback loop.
* A **correlation meter** in the loudness panel (amber under 0).

### The conductor and the harmony of the genre

A review of the Cluster Brain against modal, just, register-bound drone music found eight places
where a model of major-minor tonality was doing the listening. All of them are changed:
consonance is **harmonic entropy** now; the key is judged by a profile made from the active
**scale** (12-TET keeps Krumhansl-Kessler); Thirds and Seventh set a **prime limit**; **Utonal**
lets undertone sets count as rooted; **Root Targets** chooses where the root steps (Modal by
default, with the whole tone the genre moves by; Mediant; Phrygian; Classic); **Series** draws
chords from the harmonics of one unsounded fundamental (the Harmonic Cloud); scales without an
octave get rules in cents. The packs got these ranges family by family.

### Measured again

Every preset of the library was measured again for the guide and the review: loudness in the
guide's window of -20 to -16 LUFS for 12215 of the pack presets, the correlation between 0.3 and
0.7 for 12487 (4817 before), mono loss of 3 dB or less for 14334 (11534), and the sub three to six
decibels over the low mids for 5792 of the 9043 presets that have one (741). The map, the groups and
the sound search were made again from
that measurement (the filter round above came after it and was not measured into the map).

### Checked

The self test, the host test and the race test in the release configuration (Intel oneAPI);
pluginval at strictness 10 (with fifteen minutes a test: its parameter thread-safety test outlasts
the default thirty seconds on an instrument this size);
the new filter test on every vector path -- as the desktop is built, through the NEON path on the
x86 shim, and scalar -- and compiled for the Quest's arm64. The sample library is that of 2.0.0.

## 2.0.0

The foreground, and a library made anew.

**A near layer.** Everything the instrument had was a plane — a bank, a table, a recording, a
string, all made to be sustained and sent into the far reverb. 2.0 gives it a foreground: a fifth
source of its own, played by a second, small conductor that waits, chooses a degree consonant with
what sounds (a fifth over the highest voice, never inside a critical band of it), plays one thing
close to the ear, and waits again. Its own preset bank (108 presets in twelve families) sits beside
the sound presets and stays while they change under it. The sources were made for it: a blown
flute (a jet-drive waveguide that overblows to its octave when the embouchure shortens the jet), a
singing bowl and creaking ice (friction on modes that come in doublets, so they beat as real bronze
does), water drops (a bubble whose pitch rises as it decays, into a vessel), a murmuring voice on a
radio with its squelch and its Quindar tones, and the signals — a VLF whistler falling through the
magnetosphere, a seed pod, struck bronze, a Geiger tube, a fluorescent tube, the Krell's circuits,
a beacon's data packet, a number station's Morse, a shortwave dial. A Berlin-school sequence kind
with a shift register that mutates, a breathing tempo and a filter that blooms over the run. Events
can arrive out of the horizon or leave into it (Distance and Approach), the near field lifts 120 to
300 Hz as a thing comes close, Dry lets a click past every reverb, and two sends of its own throw a
share of every event into the second delay or into the Cosmos while the bed is left where it is --
a beacon that answers itself across a minute, circuits shifted and smeared into deep space.

**Recordings, played straight.** A Clip source type plays a recording once, unbroken, because a
sentence in grains is not a sentence. The library's archive holds NASA's mission loops and
sonifications, twelve episodes of *Quiet, Please* cut into phrases at their pauses, and seven
hundred spoken clips from the Library of Congress's Citizen DJ packs — every file's source and its
rights statement in `Archive/SOURCES.md`. A preset may name a folder: every event then plays
another phrase, never the same twice running.

**Auto and journeys.** A sound preset from a pack brings its own foreground: a table per artist
says which near presets belong to that music and how often, and the preset's name decides, the
same way every time. Journeys are presets in a row, each held for a while drawn from a range and
crossfaded into the next over a drawn fade, cyclic for an evening that plays itself; 119 come with
the instrument (two per pack, and a crossing per family of artists), and your own are a text file.

**The library, made anew.** 56 artist packs of 256 presets and 256 built-ins, generated from the
material rather than by hand: textures regenerated, a thousand generated rooms, a thousand
wavetables on three shelves, the rule-book conductor with thirty new parameters (register, interval
colour, breathing rate, silence, root steps, home) measured against Rene's rule book by an audit
that reads an hour of the conductor's notes. Every preset balanced, measured over a minute, mapped
and rated.

**Under it:** the room convolver in three partition sizes with the work spread over the block
(sixty seconds of hall for 1.4 % of a core), the cloud and memory expansions, a spectral shifter,
a stereo effect chain where the hall and the early room hear a stereo input, source envelopes of
their own, slot roles (a slot that plays only the lowest, the inner or the highest note), and the
far reverb's return after the foreground has ducked it as a parameter.

**Favourites** are kept by name in `Documents\Noctuary\favourites.txt` -- the same stars in
the standalone and in every DAW, and they survive a library made anew -- and *favourites first*
puts them at the top of the browser's list whatever the sort; the map rings them in gold. The
table pictures (Harmonic, Wavetable) light the cycle between two frames where the sound is,
blended as the engine blends it, and say so as a number.

**Found by the release build's own tests, fixed before it shipped:** a preset change within a
third of a second of the one before it could arrive without its samples; *Freeze* held back
every delayed source, so a preset frozen from its first note with nothing but delayed sources was
silent; and a recording with a small offset, integrated by a forty-second hall, could drive the
Patina's clipper onto its rail and the instrument into silence. The hall now blocks direct
current at its input, a loaded recording loses its offset, and the Patina's clipper has a blocker
ahead of it.

Checked: self test, host test, race test in the release configuration; the near bank measured
preset by preset; the rule-book audit; the full library rated.

**The sample library** — 57 archives, 100 GB: the samples, wavetables, impulse responses and the
archive the presets name — is the release `library-v5`. The installer offers to download it.
