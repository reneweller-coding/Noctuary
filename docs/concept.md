# Noctuary — concept and architecture

## Goal

A synthesizer for the kind of music Robert Rich played at his sleep concerts:
dense, slowly breathing clusters in just intonation, no rhythm, changes that
take minutes, a piece that can run all night without repeating and without
anyone touching it. It must exist as a VST3 for the studio, as a standalone
program for a night's run, and later as a native Quest app with hand-driven
control and a visual world around the sound.

## Guiding decisions

1. **The synthesizer is a library, not a plugin.** Everything that makes sound
   lives in `Core/` and depends on nothing but the C++20 standard library.
   `process(L, R, n)` never allocates; parameters are atomics written from any
   thread. JUCE is only a shell. On the Quest the same library will sit behind
   Oboe (audio) and OpenXR (input, rendering) instead.
2. **Every movement is continuous.** Random values never step; the `Drifter`
   produces smoothstep-interpolated random curves with zero slope at the
   knots. Envelopes are exponential, partial levels ramp across a control
   block, filter cutoff, delay and reverb lengths glide. A drone must never click.
3. **Measure, don't only listen.** `ambient_render` renders deterministically
   and prints per-second RMS, peak, voice count, root and arc; `analyze.py`
   reports clicks, stereo correlation, spectral centroid and the strongest
   peaks. The self test checks the tuning maths to 1e-9 and the effects by
   impulse and sine measurements.
4. **One parameter table.** `Core/include/ambient/Params.h` lists every
   parameter with range, default, skew, unit and section. The engine, the
   JUCE parameter layout, the GUI, the presets and the command-line renderer
   all iterate that table. Adding a parameter is one line in the table plus
   one line in `Engine::readParams`.
5. **Space is a landscape, not an effect** (after Rich). Depth comes from
   contrast between planes, width from time differences, focus from a mono
   low end; nothing is compressed.

## Signal path

```
MIDI / Cluster Brain
      │ note on/off + distance (0 = at the ear, 1 = infinite background)
      ▼
Voice (×16) = Strand (×1..6) = additive bank of ≤32 partials
      │  each partial: own phase, own slow amplitude drift ("Shimmer"),
      │  optionally phase-modulated by the feedback loop (h·θ per partial)
      │  strand: detune offset + slow pitch drift, pan around a wandering voice centre
      │  spectrum: h^-tilt · odd/even weight · brightness window · inharmonic stretch
      │  + Air: band-passed noise around a drifting multiple of f0
      │  → TPT state-variable low-pass (key track, env, drift, −2.5 oct per unit distance)
      │  → ADSR (seconds to minutes) · (1 − 0.5·distance)
      │  → interaural time difference from the centre pan (≤ 0.65 ms, the far ear later)
      │  → near gain cos(d·π/2) → NEAR bus ; far gain sin(d·π/2) → FAR bus
      ▼
NEAR: Ensemble → StereoDelay (asymmetric L/R, cross-feed, damping) → Delay 2 (in series)
      ─┬─ mix → + Near reverb (small room)
       ├─ "to far" (both delays) ─┐
       └─ Cloud send → GrainCloud (grains of the recent foreground, sprayed back in
          time, octave/fifth transposed, stereo-scattered) ──► FAR ◄──────┘
FAR:  (+ delay echoes) → Far reverb: 8-line FDN, 4 input all-passes, per-line   │
      damping, slow length modulation, right group 8 % longer + right output   ◄┘
      delayed ≤ 10 ms (asymmetry), tail low-pass, freeze; 100 % wet · level
      ▼
Mid/Side: side high-passed at Bass Mono (low end centred), broad +N dB bell at
3 kHz on the side, width → master gain → cubic soft clip (no compressor)
      │
      └─ Feedback: the mix before mid/side → low-pass, saturation, level throttle →
         next chunk: into the NEAR bus (To Bus) and/or into the partials (To Pitch)
```

Why additive: partials above Nyquist are simply not generated, so there is no
aliasing at any pitch; every partial can have its own life (that is the
"breathing" of a Rich drone); brightness and inharmonicity become continuous
parameters instead of waveform switches.

### The spatial model in numbers

* Distance `d` per note. Brain notes are bimodal: 40 % land at `Depth·0.15·u`
  (close), 60 % at `Depth·(0.55 + 0.45·u)` (deep). MIDI notes take *Keys Depth*.
* Per unit distance: cutoff −2.5 octaves, level −6 dB, dry→wet crossfade by
  cos/sin, so the far plane is only heard through the dark far reverb.
* Interaural time difference: `0.65 ms · Time Width · |centre pan|`, applied
  as a fractional delay on the far ear's channel; it glides, never jumps.
* Far reverb asymmetry: lines 4–7 stretched by `1 + 0.08·a`, right output
  delayed `10 ms·a`. Left and right therefore hear different reflections.
* Mid/side: second-order high-pass on the side at *Bass Mono* (default 150 Hz),
  side bell at 3 kHz, Q 0.6, up to +6 dB.
* Measured on the default patch, 180 s: stereo correlation 0.11 (was 0.36
  before the spatial model), no sample jump above 0.06, level −21 … −28 dBFS.

### Carving the planes inside the voice

Rich lays the depth in the sound design, not in the mix: the background is
dark (air absorption), the foreground carries a presence lift and stays
dry, wide pads give up the sub register to one mono bass, and a very slow
modulation lets the room breathe. All four live inside the voice, in the
additive domain where they cost nothing per sample:

* **Presence** (Space, 0–6 dB): a parabolic bell in log frequency centred
  on 3.2 kHz, zero at ±0.8 octave (about 1.8–5.6 kHz), multiplied into the
  partial targets and scaled by `1 − d`, so a note at the ear gets the full
  lift and a note on the far plane none. Measured with a 32-partial C4 at
  +6 dB: the 2–5 kHz band gains a factor > 1.8 against partials 1–4 on the
  near plane and is unchanged on the far plane.
* **Pad Low Cut** (Foundation, 0–300 Hz): partials below the cut fall by
  `(f/fc)²` (12 dB/oct). The pads leave the bottom to the Foundation sub,
  which is mono anyway. Measured: a C3 with a 300 Hz cut loses more than
  80 % of its fundamental relative to its third partial.
* **Breath** and **Breath Rate** (Space): every voice's distance wanders by
  up to ±0.35 (at Breath 1) on its own `Drifter` at Breath Rate (default
  0.03 Hz, half a minute per swing). Everything hanging on the distance
  moves with it: dry/wet balance, level, filter, presence. `noteDistance`
  reports the breathing value, so the picture in VR breathes too. Measured:
  spread > 0.1 over 15 s at 0.2 Hz, no step above 0.05 per 100 ms.
* **Source = Difference** (Foundation): see the Foundation section — the
  sub follows the combination tone instead of the root.

Every partial is a rotating phasor: a (cos, sin) pair turned once per
sample by its own rotation (cos, sin of 2π·f_h/sr, refreshed at control
rate), renormalised once per control block so it stays on the unit circle.
No table lookup and no phase accumulator in the inner loop, and the
partials are independent of each other, so the loop pipelines and
vectorises. Measured on one core of an i9-12900K, 48 kHz: the effect chain
alone costs 1 % of a core; `ambient_render --bench` (all presets, held
chord plus brain, 256-sample blocks) went from a median of 22× realtime
(table lookups) to 39×, the slowest preset from 11× to 16×; five voices with
six strands of 32 partials run at 17× harmonic or inharmonic alike. Two
dead ends on the way, kept here so they are not tried again: caching the
control-rate spectrum shape and doubling the control block gained nothing
(the inner loop dominated), and a single angle-addition recurrence per
strand was latency-bound; four interleaved chains helped (median 30×) but
the independent phasors beat them and are simpler. The presence bell, the
pad low cut and the breathing distance (below) were added afterwards in
the control-rate part of the voice; a re-run of the bench on an idle
machine gave a median of 43× and a slowest preset of 27×, so they are
free.

## Tuning

* `FixedScale`: up to 64 ratios per period, period ratio (2 = octave, 3 =
  tritave), copied into the audio thread without allocation.
* Built-in scales: 12-TET, Ptolemy major, JI minor, 7-limit (11 notes),
  Pythagorean, JI pentatonic, harmonic series 8–16, subharmonic 16–8, slendro,
  Bohlen-Pierce, otonality 1-3-5-7-9-11.
* Scala `.scl` parser (ratios and cents), user slot persisted in plugin state.
* Keyboard mapping: *Snap* (12 keys per octave, each snapped to the nearest
  degree — the intuitive way to play a 7- or 11-note JI scale) or
  *Consecutive* (n keys per period, for EDOs and non-octave scales). Snap
  falls back to Consecutive automatically when the period is not an octave.
* `intervalConsonance(ratio)`: octave-reduce, find the simplest p/q within
  10 cents (q ≤ 32), score 1/(1+log2(p·q)). Unison 1.0, fifth 0.28, major
  third 0.19, semitone 0.11, anything unrecognised 0.05.

### Purity, purity drift, freeze

*Purity* (Tuning) blends every note's frequency between 12-TET (0) and
the chosen scale (1) in the log domain: at 1 the partials of different
notes lock into each other and the beating is gone, at 0 they beat like a
piano, in between the beating slows as the intervals close in on their
ratios. *Purity Drift* lets the blend wander on a Drifter at *Drift Rate*
(default one swing per 100 s), so the lock-in comes and goes over minutes;
sounding voices follow with a one-second log-domain glide, no retrigger.
Measured: E4 over a C root is 327.03 Hz pure and 329.63 Hz tempered, 0.5
gives their geometric mean, and a held voice moves from one to the other
within five seconds. *Freeze* (Oscillator) stops the movement clock of
every voice — shimmer, pitch drift, breath, bloom — while envelopes,
filters and effects keep their own time: the spectrum stands still.
Measured: the third partial of a frozen voice varies less than a quarter as
much as with shimmer at 2 Hz.

### Ghost, portamento with gravity, inertia, tape, coherence

Five small mechanisms from a second round of suggestions, all inside the
existing structure:

* **Ghost** (Air → Mode): the Air noise goes through six sharp resonators
  on the note's harmonics 1 2 3 5 7 9 instead of one band-pass, Q from
  *Air Q* times eight — the harmony is filtered out of the chaos, Rich's
  string and pipe resonances. Measured: with the partial bank silent,
  harmonics 3 and 5 of an A3 carry eight times the energy of 4 and 6.
* **Portamento** and **Gravity** (Tuning): a new key slides in from the
  last one in the log domain over *Portamento* seconds; *Gravity* slows the
  slide near consonant ratios to the root (unison, octave and fifth to about
  15 % speed, `intervalConsonance`), so the glissando dwells on the harmonic
  nodes and hurries across the dissonant stretches — the microtonal
  portamento of a lap steel. Measured: half-way through an even 2-second
  glide from A3 to E4 the pitch sits between the two, at the end it has
  arrived; with gravity the pitch is still lower at the half-way mark.
* **Inertia** (Macros, performance state): every float parameter the
  engine reads glides to its value with this time constant in the skew
  domain, the analogue slew — a knob torn open still arrives slowly.
  0 (default) is bit-exact bypass.
* **Tape** (Feedback): in the loop an asymmetric saturation (an even-order
  term the DC blocker cleans up on the next pass), wow (an irregular
  Drifter, up to 3 ms) and flutter (6 Hz, 0.3 ms) as a fractional read
  position, and a noise floor that rises with the loop's level — every
  generation through the loop goes a little softer and less stable, like a
  forty-year-old tape. Measured: a full loop with tape stays bounded and
  DC-free for 12 s.
* **Coherence** (its own section): four Kuramoto oscillators with natural
  periods 23, 31, 41 and 53 s over *Rate*, coupled by *Coherence*
  (dθᵢ = ωᵢ + K/N Σ sin(θⱼ − θᵢ), K up to 0.6 rad/s times *Rate*, so the
  lock is the same at every tempo), their sines added by
  *Depth* to brightness, brain depth, pan and the z-plane point. Independent
  at 0, pulsing as one unit at 1, and everything between. Measured with the
  Kuramoto order parameter after 20 minutes of phase: above 0.9 when
  coupled, below without.

### Sleep and the rest zone

After two seconds with no voice and the output below −90 dBFS the engine
sleeps: the effect chain is skipped and zeros go out, so a silent Quest
costs nothing; the brain keeps ticking inside the render and a note (MIDI,
OSC, brain) wakes the engine in the same block. The gesture layer has a
**rest zone** (default 8 % of the calibrated height, `rest_zone=` in
`ambient.cfg`): both hands hanging low means nothing is written, so the
arms can drop without touching the sound; it only arms once real hand data
arrived, so knobs and OSC alone never "rest". On the Quest the map cursor
now uses both hands — left reach and height for the position, right height
for the blend radius (sharp low, blurred high).

## Cluster Brain

Runs at control rate inside the engine. State: up to 12 slots (note, time
remaining), a root note, a timer.

* Event timing: exponentially distributed intervals around *Event Rate*
  (clamped 0.5 s … 4× rate). First event 0.5 s after switching on.
* When a slot's hold time expires the note is released (the voice's long
  release does the fade).
* If *Density* is reached, an event either retires the note ending soonest
  (50 %) or does nothing.
* *Wander*: with probability 0.35·wander the root moves to the note in range
  whose ratio to the old root is closest to 3/2, 4/3, 5/4, 6/5, 5/3 or 8/5.
  A held MIDI key pins the root instead.
* Note choice: every note in [Lowest, Highest] not already sounding gets
  weight consonance(ratio to root)^(3·Consonance) × register bell curve;
  octave doublings of sounding notes ×0.15; the root's pitch class ×3 when
  nothing sounds it, ×0.25 when something does; exact duplicate pitches
  (possible with snapped keys) are excluded. Hold time uniform in
  [Hold Min, Hold Max], velocity 0.5–0.9.
* *Arc*: a `Drifter` with period *Arc Period* (minutes) scaled by *Arc*
  shifts density by up to ±2 voices, brightness by ±25 % and depth by ±30 %,
  so an all-night run has tides instead of a flat sea.

### Autoplay: Free and Chords

The conductor above is *Free*, and Free stays the default: notes come and go
on their own timers, which makes the cluster breathe but never really move --
after ten minutes it is the same harmony, differently arranged.

*Chords* keeps the cluster **full** and exchanges exactly **one voice at a
time**. Every *Every* seconds (or on the clock, via *Sync*, or the moment
somebody presses *Step* / the panel's button / a mapped controller) the voice
that has been sounding longest leaves and one note takes its place. The
candidate is scored for three things at once:

* **How it sits against the voices that stay** -- the mean consonance with
  each of them and with the root, not just with the root. That is what makes
  the result a chord rather than a heap. *Tension* is the exponent on that
  term: at 0 only notes that fit the whole chord are considered, turned up the
  progression is allowed to lean.
* **How far that voice has to travel** -- *Voice Lead*, in semitones, is both
  a hard limit and a preference within it. Small is voice leading: the note
  that leaves is replaced by a near neighbour, and the ear hears the chord
  shift rather than one note being cut and another started. Large lets the
  harmony jump.
* **Doubling** -- an octave of a pitch class already sounding scores ×0.2, the
  root's own class ×0.5, so an exchange brings a new colour instead of
  thickening one that is already there.

* **What left recently** -- the last eight notes to leave carry a penalty that fades with age
  (×0.12 the moment they go, back to full after eight exchanges). Without it the harmony keeps
  picking up the note it has just put down: it is the nearest candidate and it fitted a moment
  ago, so it wins again. Nothing is banned, only postponed.

*Root Move* decides how often the exchange moves the root as well (the same
`wanderRoot` the Free mode uses); without it the harmony circles one centre
for ever, with it the piece travels. A small random factor (0.85–1.15) sits on
the final score so the same chord does not always resolve the same way.

The note being replaced is removed from the cluster **before** the candidates
are scored, so it does not vote on its own successor -- and is itself excluded
from the running, because at zero distance it would always win and the chord
would never move. That was a real bug; the self test caught it, and the
descriptor oracle never would have, because both modes render sound that
measures the same.

Filling an empty cluster adds one note per tick rather than all of them at
once, so the first minutes are an entrance and not a chord.

**Does it actually form chains?** Measured over an hour of simulated time, per
setting: how many exchanges happen, how many of the resulting chords are ones
it has never been in, how often it swings back to the chord two exchanges ago
(that would be a pendulum), and how far the mean pitch of the chord travels.

| Setting | Exchanges | New chords | Pendulum | Drift |
| --- | --- | --- | --- | --- |
| Lead 3, Tension 0.15, Root Move 0.25 | 56 | 56 | 0 | 6.4 st |
| Lead 7, Tension 0.5, Root Move 0.8 | 98 | 98 | 0 | 5.7 st |
| Lead 12, Tension 0.7, Root Move 0.5 | 116 | 116 | 0 | 6.0 st |
| Lead 5, Tension 0.25, Root Move 0.35 | 86 | 83 | 0 | 3.6 st |
| Lead 2, Tension 0, Root Move 0 | 116 | 35 | 0 | 2.6 st |

The last row is the corner where everything is set to its tightest: the root
is pinned, a voice may move two semitones, and only notes that fit the whole
chord are allowed. There the harmonic field really is finite, and circling it
is the correct answer rather than a fault -- *Root Move* is what opens it, and
its default is 0.2, not 0. (Before the recent-notes penalty that row managed
35 chords in name only: 18, with 98 revisits.) The self test holds the
property for the default range: over an hour, dozens of exchanges, nearly all
of them into a chord it has not been in, never one two steps back, and a chord
that has moved in pitch.

### Filter models

`Core/include/ambient/Filter.h`. The voice filter is one of ten models
behind the same five knobs (*Model*, *Cutoff*, *Resonance*, *Env Amount*,
*Drift*, *Key Track*, plus *Drive*): LP 6 (one pole), LP 12 (the
state-variable low pass the instrument always had, bit-identical), LP 24 (two
stages, shared resonance), HP 12, BP 12 (unity at the cutoff), Notch, Peak (a
bell of up to +14 dB, narrower with resonance), Ladder (four one-poles with
the last fed back through a soft saturation; the corner sits 1.55× above the
knob so the -3 dB point lands near Cutoff; resonance squared, because the
interesting range is at the top) and Comb (a feedback comb tuned to Cutoff
with a low pass in the loop, output scaled by 1 - fb so the peaks stay at
unity and Resonance deepens the dips instead of raising the level -- on a
sustained cluster less a filter than a second resonating body). *Drive* is a
soft saturation ahead of the filter, level-compensated. Every model reports
its own magnitude response (`VoiceFilter::magnitude`, the same maths as the
audio path), which is what the filter display draws. A model change resets the
filter states, since they mean different things in different models.

The voice filter and the z-plane are two filters, each with its own switch
(*On* in the Filter section; the z-plane's *Mode*, where *Replace* is the
z-plane alone), and with both on the z-plane's *Route* puts them in **series**
(the z-plane hears the filter, *Mix* is its dry/wet) or in **parallel** (both
hear the dry sum and *Mix* balances them). The display combines the two
responses the same way; the parallel sum ignores the phase between the
branches, which is the one thing the picture cannot show. Switching the
filter back in starts it from rest.

### Z-plane filter

After the idea Dave Rossum built into the E-mu Morpheus: filter frames sit
on the corners of a **cube**, and a point (X, Y, Transform) inside it is a
filter whose poles *and zeros* are interpolated between them — move the
point and the whole resonant structure glides, always through stable
filters. The patent (US 5,170,369) expired long ago and the manuals
describe what the filters do, but the coefficient tables in the original
firmware are proprietary data; this bank is our own, built in the same
architecture, with its numbers from published acoustics.

**155 shapes in twelve families** (voice, sweeps, combs and phasers,
strings and bodies, bars and bells, membranes, tubes and pipes, rooms,
series and exotic, extremes, instruments, EQ and speakers) — the original
shipped 197 cubes, and this is the same order of thing. They are generated
by `Tools/make_zplane_bank.py` rather than typed, because almost every one
of them is a set of frequency *ratios* — the mode series of a bar, a
membrane, a pipe, a bell — times a base frequency, and ninety-six shapes
times eight corners times six frequencies is an invitation to type 2.756
as 2.576 and never find out. The ratios are written down once, with a note
saying where each comes from (Fletcher & Rossing for the bars and bells,
the sung-vowel formant tables for the voices, `c/2L` for the room modes),
and a machine does the multiplication.

The first sixteen shapes are the original hand-written bank, carried over
frequency for frequency and frozen: the library's 6000 presets name their
shape as text (`z_shape=Glass`), so those names and their order can never
change. New shapes are appended, never inserted.

**The third axis** is what makes it a cube rather than a square, and the
original's name for its filters says so. Writing eight corners for every
shape would be twice the data for a face that is usually the same idea
again, so each shape instead names a *rule* for how its far face differs —
sharper, damped, peaks turned into notches, an octave up, the partials
fanned out. Transform's default is 0, which is exactly the square that was
there before the axis existed, so every preset ever saved sounds as it did.

`Core/include/ambient/ZPlane.h`:

* A frame is up to **six cascaded two-pole/two-zero sections** — a 12-pole
  filter, the order the Morpheus used. Interpolation is trilinear in log
  centre frequency and log bandwidth on the *pole and zero parameters*,
  never on coefficients, so every point inside the cube is stable by
  construction. The self test checks that claim rather than repeating it:
  all 155 shapes at three corners each, every section tested for stability,
  every cascade for a finite and sane level, and every shape for whether
  moving the point actually changes the sound — a shape whose corners agree
  is not a filter you can morph, it is a filter with three dead knobs.
* Sections come in two flavours, and the difference matters: a **bell**
  (zero on the pole, wider) boosts its frequency and leaves the rest at
  unity, so six of them in series shape a spectrum; a **resonator** (no
  zero, or a zero elsewhere) passes only its band, so a few in series take
  the sound over completely. A first attempt made every cluster shape a
  cascade of narrow resonators — six of those cancel each other out, and
  the self test found them at −120 dBFS.
* Normalisation happens twice per control block, from one pass over a
  probe grid (16 fixed logarithmic points plus every pole and zero angle of
  the frame — a fixed grid alone walks straight past a needle-sharp
  resonance). Each section is scaled to its geometric mean over the grid,
  which is what keeps a bell's background at unity, capped at 30 dB of
  boost; then the finished cascade is scaled so its loudest point is unity.
  A shape can therefore be extremely resonant without being loud.

Sixteen shapes in six families: Vowel Morph, Choir, Nasal · Low Sweep,
High Sweep, Band Sweep · Phaser, Comb, Flanger, Notch Cluster · Strings,
Metal Bars, Wood, Glass · Peaks · Infinite (two poles a hair apart at the
stability limit). Per voice: *Mode* Series (after the state-variable
filter) or Replace (instead of it), *X* / *Y*, *Rate* and *Depth* (two
Drifters move the point — the "LFO", in this synth's continuous,
non-repeating form), *Resonance* (quarters or doubles the bandwidths),
*Key Track* (the frame follows the note), *Mix*. Twelve presets
(*Morphing Vowels* … *Endless Resonance*) show the families off. Measured:
all sixteen shapes stay finite, audible and below a peak of 2 at their four
corners and their centre; on a 32-partial A2 the Vowel corner a favours
700 Hz over 2300 Hz more than three times as strongly as corner i; the six
sections cost 14 % more render time than no filter at all on the heaviest
preset (23× → 21× realtime).

### Stack and Rate Wander

* **Stack** (Oscillator): instead of detuned copies, the strands sit at
  pure ratios to the note — Octaves (1 2 ½ 4 ¼), Fifths (1 3/2 2 3 ½ 9/4),
  Major (1 3/2 5/4 2 5/2 ½), Minor (1 3/2 6/5 …), Seventh (… 7/4), the
  harmonic and the subharmonic series — ordered so that two strands already
  make root + fifth and three a triad. Detune and drift still apply on top,
  so Detune 0 makes the chord beat-free: one key, one just chord, and the
  partials of the strands lock into each other (the fifth's second partial
  is the root's third). Measured: three strands, Major, one partial each,
  A3 → 220, 330 and 275 Hz, nothing else within 20×.
* **Rate Wander** (Oscillator, default 0.3): one 100-second `Drifter` per
  voice scales the pitch-drift, pan, filter, air, shimmer and breath rates
  by 2^(±wander), the nested-LFO idea — the movement itself speeds up and
  slows down, so a stretch of five minutes never resembles the previous
  five.

### Four sources

Every voice has four equal source slots (three until the Vector arrived; the
fourth exists so the square's four corners are four sources rather than three
and the three together). Source 1's *Type* defaults to
**Additive**, which is the strand bank described above (unison, detune,
stacks, bloom -- the classic Oscillator; its Octave, Ratio and Pan move the
whole bank); set to anything else the bank falls silent and the slot renders
in its place, so a voice can be four granular players or four FM pairs.

Each slot has its own clip. The engine used to hold one texture for the whole
instrument, which meant two Texture slots always played the same recording;
now `setTexture(slot, ...)` fills one of four double-buffered clips, the
slotless call fills all four (what a preset naming a single file always
meant), and a pack preset's texture field may carry up to four paths
separated by `;`, an empty one meaning that slot has none. Adding the fourth
slot had one trap worth writing down: the slots fork the voice's random
stream in order, and a fourth fork advanced that stream by one draw and moved
every random decision after it -- the oracle reported all 39 presets changed.
Slot 4 seeds from a side stream instead, and the oracle is back to 39
identical.

### Each source enters on its own clock

The Envelope section is one envelope for the whole voice. Until `src{n}_delay`
existed, all four sources therefore started on the same note at the same
instant, and a preset of a wavetable against a texture was one chord struck
twice at once however different the two materials were -- which is a large
part of why two presets built from the same parts sounded like the same
preset. Rene, hearing the library after the level repair: *"dann könnte man
Oszillator 2 erst einige Zeit nach Oszillator 1 starten lassen, was die
Variabilität deutlich erhöhen würde."*

Each slot now has three parameters of its own. **Delay** holds it silent for
up to thirty seconds after the note; **Rise** fades it in afterwards; **Env**
gives it one of the preset's six sixteen-breakpoint shapes as its level
contour instead. The shape is the same one the modulation matrix reads, with
that envelope's own Mode and Time -- a slot does not get a seventh envelope,
it borrows one -- and the difference is that here it runs from the note
rather than from the phrase clock, so every note gets it and not only the
first one after a silence. Read as a level the shapes are bipolar: -1 is
silence, +1 the slot at its written Level.

All three default to off, so every preset written before them sounds exactly
as it did; the gain is a multiplier of one and the copy of `SlotParams` that
carries it costs one struct per slot per control block. The generator gives a
delay to about two in five of the later slots -- never to the first sounding
one, because a note has to start somewhere -- drawn log-uniformly between two
and a half and twenty-five seconds, and a quarter of those take an envelope
shape rather than a plain fade.

### Stretch: a recording as a continuum

The Texture type reads a clip as grains; Stretch reads the same clip as a
continuum. It is Paulstretch inside a voice: a window of the recording
(Grain, up to a third of a second) is transformed, its magnitudes are kept
and its phases thrown away and drawn afresh, and the frame is overlap-added
at a quarter of the window -- the Cosmos's Nebula, pointed at a clip rather
than at the mix. Every frame is a plausible slice of the clip's spectrum with
no memory of where its transients were, so the analysis position can crawl
through the recording at a thousandth of its speed and what comes out has no
grain rhythm and no attack left standing. This is the tool the ambient
literature describes for Rich's *Perpetual*: field material stretched by
factors up to a thousand until only the overtone weave remains.

Two decisions shape it. Pitch is applied when the window is *read*, as a
resampling step through the clip, and the stretch is applied to how far the
read moves between frames; the two do not know about each other, so
Follow = Note plays a chromatic sample across the keyboard without a high
note ending sooner than a low one -- measured, the pitch comes out identical
to the granular player's in both modes (110.7 Hz Free, 220.3 Hz at A3). And
the loop's seam is crossfaded over the last part of the clip into its first,
so the wrap lands on the sample the fade has been arriving at, unless the
file name carries `_loop`: the generator marks clips it has made seamless,
and those wrap straight round.

Cost: a 16384-point transform per hop per voice per slot. Nine voices with
four Stretch slots each render at 6x realtime on the development machine
against 20x for four Texture slots; one slot in a few voices, which is what a
patch actually does, is not a number anyone will notice. The transforms are
shared across all slots of all voices, built once in `prepare()`, read-only
after; the buffers are sized once for the longest window and never touched
by an allocation in `render()`.

**The field recordings.** Five hundred places for it, from the same
text-to-audio worker that made the textures, pointed at environments instead
of instruments: twelve categories of six prompt seeds each -- rain on a tin
roof, a harbour in fog, pack ice, a power station through a wall -- crossed
with weather, distance and hour (`Tools/library/make_field_recordings.py`).
Stable Audio Open 1.0 only: 44.1 kHz stereo and the best of the three models
at places; AudioLDM 2 is 16 kHz and has no air. Twenty-six seconds each,
because at forty times slower than life that is a quarter of an hour and a
minute per clip would be a gigabyte the package does not need.

Every one of them is seamless by construction rather than by luck: the last
1.5 s are faded into the first (equal power), the overlap is trimmed, the
seam is *measured* -- the largest sample-to-sample step across the wrap
against the largest inside the clip -- and the file is marked `_loop`, which
is what the Stretch type reads to skip its own crossfade. The mark goes in
front of a trailing pitch token, not after it, because the engine reads the
clip's pitch from the last token of the name and a `_loop` after `_A3` would
have hidden every tonal recording's pitch. They land in `Library/Textures`
under the `field_recordings_` prefix, which is the slug the preset generator
uses to find a style's own clips, and the *Field Recordings* pack is built
from them with the Stretch type in every slot it fills, each slot drawing its
own clip: a preset can be four landscapes in the Vector's four corners.

Three measurements that had to be made rather than assumed: the type makes
sound (a silent new source type is the easiest thing in the world to ship),
a different stretch factor is a different render (the factor moves the read,
and a render blind to it would mean the read was not moving), and the
seamless mark is read from the name. The first draft of the pitch test failed
at 327 Hz for a 110 Hz clip -- not the stretch, the Air band, on by default at
three times the note and counted by a zero-crossing detector. With Air off:
110.7.
Additive in Source 2 or 3 is a single 32-partial bank inside the slot with
the same spectrum formula (partials, tilt, brightness window, odd/even,
inharmonic stretch, per-partial shimmer) -- one strand, the cost of a
wavetable slot. The type index for Additive sits last so the indices the
five thousand presets store for the other types did not move; the switch was
measured sound-neutral on 37 presets (identical descriptors, because the new
random streams are seeded from side streams and never touch the voice's).

Each slot has a type, level, octave, a just
ratio to the note (1/1 … 2/1, so a slot can sit a fifth or a seventh above Each slot has a type, level, octave, a just
ratio to the note (1/1 … 2/1, so a slot can sit a fifth or a seventh above
the key), and a pan; all of it goes through the voice's filter, envelope,
distance and ITD like the bank. `Core/include/ambient/Sources.h`.

* **Wavetable** — a table of *spectra*, not of samples: up to 64 frames of
  32 partial amplitudes. *Position* interpolates between frames, *Pos
  Drift* lets it wander on a 50-second curve, and the result is rendered by
  a rotating-phasor bank like the main oscillator. Alias-free, and
  presence, low cut and the feedback's phase modulation treat it like any
  other partials. Built-in tables are generated (Classic: sine → triangle
  → saw → square → pulse; Organ drawbars; Vocal formants a-e-i-o-u; Glass;
  Metal); *User* is analysed from a WAV with 2048-sample single-cycle
  frames (the Serum/Vital layout) — FFT per frame, bins 1–32, up to 64
  frames picked evenly. Measured: Classic at 0 is a sine (no second
  partial within 100×), at 0.5 a saw (second partial at 1/4 the power,
  third present); ratio 3/2 with octave +1 puts A3 at 660 Hz.
* **FM** — carrier at the slot pitch, modulator at *FM Ratio*, *FM Index*
  up to 8, shrinking above 3 kHz so high notes do not alias; *Pos Drift*
  wanders the index by up to a factor two. Measured: index 0 is a sine,
  index 3 puts more than a tenth of the carrier's energy on the sideband.
* **Texture** — a granular player over a loaded sample: up to 8 Hann grains
  of *Grain* ms at *Density* per second, around *Position* (wandering with
  *Pos Drift*, sprayed ±3 %), each grain with its own small pan. *Pitch* =
  Free plays the sample at its speed (octave and ratio become speed
  multipliers); Note pitches it to the key, assuming the sample was
  recorded at C4. The texture is double-buffered in the engine so a new
  file never touches the buffer the audio thread reads. Measured: a 440 Hz
  sample plays at 440 Hz in Free and at 370 Hz on A3 in Note; an empty
  slot is silent.

The plugin loads textures in any format JUCE reads (paths kept in the
state), the render tool takes `--texture file.wav [baseHz]` and
`--wavetable file.wav`, and the Quest app picks up `texture*.wav` and
`wavetable.wav` from its data folder. The core has its own WAV reader for
that (PCM 8–32 and float, any channel count mixed to mono). A note token
at the end of the file name (`bowed_metal_sao_123_A3.wav`, also `C#4`,
`Bb2`) sets the texture's base pitch; without one, C4 is assumed.

**Where textures come from: `Tools/TextureGen`.** A PySide6 program with a
prompt, model choice, length, steps, guidance, seed and a variation count;
the models run in a child process that stays loaded between jobs (torch
never inside a Qt thread). Models: Stable Audio Open 1.0 (44.1 kHz stereo,
≤ 47 s — the first choice for textures, field recordings, metal, air;
gated on Hugging Face), MusicGen Large/Medium/Small (32 kHz, tonal drones
and choirs), AudioLDM 2 Large (16 kHz, dark effects). Output is 32-bit
float WAV at −6 dBFS peak, plus a `.txt` with the settings; the worker
detects the base pitch by autocorrelation and appends the note to the
file name only when the clip is clearly periodic, so rain stays unpitched
and a bowed plate becomes `…_A3.wav`.

**Where user wavetables come from: `Tools/WavetableGen`.** Same
environment, own GUI. Three sources: *Audio* slices single cycles out of
any WAV (pitch-tracked with the TextureGen rules, each frame the average
of a few cycles, frames spread over the selection, phase-aligned by
circular cross-correlation so morphing does not click); *Prompt* asks a
TextureGen model for a sustained note and slices that; *Procedural* walks
spectral recipes over the table position (saw → square, tilt, formant
sweep, comb, glass, odd breathing, random walk) with optional per-partial
random walk and phase scatter, and can morph the current table into a
recipe. A frame/spectral-image view, a preview sweep at a chosen note, and
export in the 2048-frame layout. Measured: a 220 Hz saw-to-square sweep
comes back with the second partial at 0.5 in the first frame and gone in
the last; the MusicGen C2 drone yields a 32-frame table the render tool
loads and plays. A learned latent space (WaveSpace-style) was considered
and left out: the tables are spectra already, and the three sources cover
the space from sound, language and rules without a checkpoint.

**The shelf, rebuilt (1.11.0).** Rene looked at the folder and said the wavetables seemed fewer
than they looked, and full of variations of one thing. The arithmetic agreed: 608 tables were 19
recipes × 32 variants, so nineteen ideas with neighbours. Three separate things kept it that way,
and looking for the third turned up a bug that had been there the whole life of the library.

*Nineteen ideas became forty-three.* Fourteen recipes written for what a drone wants out of a
table -- Singing bowl, Gong wash, Bowed string, Bowed cymbal, Prepared piano, Stretched string
(the Railsback stiffness of a real piano wire), Formant beat, PPG digital, Lo-fi bits, Resonator
bank, Bi-phase, Pink resonance, Bohlen-Pierce, Sub bloom -- and ten more after that: Shepard stack,
Fibonacci, Organ mixture, Three vowels, Stopped pipe, Beating pairs, Ring bell, Stretched octave
(2.02^n, the same stretched octave the instrument tunes with), Waterphone, Spectral erosion.

*And the frame grew a second half.* A single cycle is periodic, so its spectrum is harmonic whether
one likes it or not: a wavetable cannot hold an inharmonic partial. What it can hold is a harmonic
set the EAR hears as inharmonic -- the sparse, widely spaced pattern struck metal actually has --
and partials that turn against each other as the table is scanned. A recipe may now return
`(magnitudes, phases)`, and Singing bowl puts two twin partials beside the bowl's own set whose
phase turns once across the table: swept slowly they beat against their neighbours. That is the
shimmer a drone lives on, and no fixed spectrum can give it.

Two traps came with the phases, both of them the continuity rule in a new costume. Three recipes
drew their scatter from the rng -- which advances every frame, so each frame got a different set,
consecutive cycles stopped being related, and scanning would have been a noise burst instead of a
movement (frame step 0.32; with a scatter that is deterministic in the partial index, 0.011, at
the same spectral travel). And a partial set computed from a ratio has to be rounded somewhere:
rounding INSIDE the table is a step, the partial jumping from 7 to 8 as t grows. The energy now
sits as a narrow bell around the unrounded position and slides from one partial to the next
(`_peaks`), which took Stretched octave from 0.137 to 0.054.

*A thousand tables out of the library's own material.* WavetableGen has always been able to slice
single cycles out of a recording; nobody had ever pointed it at our own shelves. Every tonal clip
with a steady pitch becomes a table, at most two per prompt idea -- grouping by idea rather than by
file, or four seeds of one prompt would be four neighbours again. They are chosen by what they are
of, not taken wholesale: struck metal, bowed strings, voices, glass, reeds, organs, prepared piano.
A siren has a pitch too, but sliced it is a sine with extra steps. Of 2218 pitched clips, 1163 from
736 ideas qualified and 1064 produced a table (the rest were not steady enough, and a table built
from frames that were never the same note is noise with a period). A table is named
`clip_<the clip>`, so it inherits that clip's place in the CLAP affinity map: a table made from a
recording the model put near Permafrost is drawn there.

*Why so few were reached for.* The 42 styles named eight recipes between them -- and one of those
eight, "Harmonic drawbars", does not exist (it is "Organ drawbars"). Four styles pointed at
nothing, and the slug match simply never fired: a dead name costs nothing, says nothing and is
invisible. The twelve drone recipes added in an earlier round were named by nobody at all. Every
style now names five of the forty-three, all forty-three are named by someone, `make_presets`
refuses to run if a style names a recipe that does not exist, and the module weight for the
wavetable slot went from 0.20 to 0.35. Measured on a 840-preset sample: the wavetable is the
primary source in 29.9 % of presets rather than 23.6 %, and 46 % rather than 35 % name a table at
all. The shelf is 2096 tables over about 780 ideas.

*And the scanning itself.* A wavetable's position is the one control where a rigid eight-second
sine is audibly wrong -- scanning a table is a slow walk through a spectrum, and a stiff LFO turns
the drone into an LFO with a spectrum attached. A route aimed at a position now takes a chaotic
source (Lorenz, Rössler, Lenia) 55 % of the time rather than 34 %, and when it does get an LFO the
period is 30 to 180 seconds and the shape is one that does not jump.

## Foundation, Bloom, Hold, Macros

* **Foundation** (`Engine::renderChunk`, after the mid/side stage so the
  binaural offset survives Bass Mono): one sine/triangle voice per ear on
  the brain's root ÷ 2 or ÷ 4, frequency glides in the log domain with time
  constant *Glide*, level rises over 2 s, ears offset by ±*Binaural*/2 Hz.
  Measured: root A3 gives 110 Hz an octave below; with 6 Hz binaural the
  left ear sits at 107 Hz and the right at 113 Hz.
  *Source = Difference* makes the sub follow the **ghost tone** instead: in
  just intonation two voices produce a combination tone `f2 − f1` in the
  ear itself (a fifth 3:2 gives f/2, a fourth 4:3 gives f/3, a major third
  5:4 gives f/4). Rich reports these tones between 55 and 440 Hz carrying so
  much energy that he tames them in mastering; here the Foundation doubles
  the one the two lowest sounding voices make, folded into the octave the
  root mode would use (so the register never changes), gliding as usual.
  With fewer than two distinct pitches it falls back to the root. Measured:
  A3 + D4 (4:3, 220 + 293.3 Hz) put the sub at 146.7 Hz, one voice left
  returns it to 110 Hz.
* **Bloom** (per voice): brightness = brightness × (1 − bloom × (1 −
  smoothstep(t / bloomTime))). Measured: with bloom 1 the first second
  carries less than a fifth of the high-partial energy of the opened state.
* **Hold**: note-on on a latched key releases it; note-off is ignored;
  switching Hold off releases all latched keys.
* **Macros** are the gesture inputs Custom0..7 fed from eight parameters, so
  the same mapping table serves hands and knobs. A mapping only starts to
  write once its input has moved from its rest value (also true for head
  yaw), which keeps presets intact until the performer touches a macro.
  They carry Rich's vocabulary, not the engine's: *Space* (far level, decay,
  depth, size), *Alien* (cosmos send, nebula, shift), *Motion* (drift and
  shimmer rates, pan drift, ensemble), *Bloom* (brightness, air, cutoff,
  presence), *Density* (brain density, cloud, strands), *Distance* (depth,
  keys depth, cutoff down, far level up, presence down), *Evolution* (rate
  wander, source position drift, breath, arc), *Air* (air, air colour, side
  air, tail cut). The plugin's **Perform** page shows only these eight as
  large knobs plus the morph, for playing a set without the editor.

## Cosmos path (science fiction / deep space)

A parallel send off the near bus after the delay; the dry path is untouched
and *Return* / *To Far* mix the processed signal back into the foreground and
the background. Chain, in order:

1. **FreqShifter** — single-sideband shifter built from Niemitalo's
   90° all-pass pair (eight second-order all-passes); measured sideband
   rejection 44 dB. The right channel is shifted 3 % less than the left, so a
   200 Hz shift beats at 6 Hz between the ears: alien, but wide.
2. **CombResonator** — two combs (right 0.3 % longer) tuned to the brain's
   current root × *Res Pitch*, feedback to 0.97, output normalised by
   (1 − feedback) so the resonant peak stays near unity gain.
3. **VowelFilter** — three SVF band-passes (Q 8) on formant tables for
   a/e/i/o/u, position driven by a `Drifter` at *Vowel Rate*, log-interpolated
   between neighbouring vowels; right channel 1 % higher.
4. **Nebula** — STFT (2048/512, Hann) magnitude smoothing with random phases
   per frame: Paulstretch-style smearing. Update coefficient
   α = (1 − smear)², so *Smear* = 1 freezes what is in the buffer. Latency
   2048 samples on this path only. Own FFT (radix-2), no dependencies.
5. **Shimmer** (in the engine, not the chain) — the far reverb's output is
   pitch-shifted (granular two-head shifter, 80 ms window) and fed back into
   the far reverb's input on the next block, low-passed at 4 kHz. The loop is
   throttled by the reverb's own level (feedback → 0 at a mean level of
   0.12), so it blooms and then holds; measured: stable at 1.0 for 20 s with
   the resonator at 0.97 feedback in the same patch.

## Room (convolution reverb, optional and additional)

A third reverb next to the near room and the far FDN: `Convolver`
(`Core/include/ambient/Convolution.h`) plays an impulse response by
uniform partitioned convolution — input blocks of 512 samples, each
block's spectrum into a frequency-domain delay line, every output block
the sum over all partitions of input spectrum × impulse-partition
spectrum, one inverse FFT per block, overlap-add. Latency is one block,
which the far plane does not notice. Real input, so only bins 0…N/2 are
multiplied and the mirror is rebuilt before the inverse transform.
True-stereo impulses keep L and R apart, a mono impulse serves both
channels with their own inputs. Impulses are double-buffered like
textures, resampled to the engine rate and energy-normalised, so a room
never changes the level of what it reverberates; the maximum is 8 s.
Without a file, `generateDefault` builds a dark hall at `prepare` (three
noise bands with RT60 5 / 3 / 1.2 s, a 20 ms diffuse onset, eight early
reflections, independent noise per ear), so the Room works out of the box.

Parameters (section Room): *Level* (0 = off and no CPU; the convolver
keeps running for one impulse length after the level reaches zero so the
tail can finish), *Source* (Far = the far sends before the FDN, Near = the
finished foreground), *Pre-Delay*, *Tail Cut*. Files: the plugin's
*Impulse…* (mono or stereo, path in the state), `ambient_render --ir`,
`impulse.wav` on the Quest (applied once the engine is prepared). Measured:
a unit impulse comes back one block late at unity, a tap 1500 samples in
lands at the right place across partitions, the default hall decays by
more than 10 dB per two seconds with an ear correlation below 0.3, Room
level 1 leaves a tail after a note where level 0 leaves silence.

**Where impulses come from: `Tools/ImpulseGen`.** Design (per-band RT60,
size, pre-delay, width, tone, modulation; eight room presets from dark
cathedral to infinite plate), Recording (onset, trim, floor, tail
extension for cut-off renders), Prompt (a TextureGen model renders a clap
in a described room, which is then cut — text-to-audio models do not know
impulse responses but they know claps in cathedrals), Hybrid (a
recording's spectrum colours noise, the designed decay shapes it). GUI and
CLI, energy decay curve and a chord preview.

## Feedback loop (the sound feeds itself)

Rich's drones are not a chain but a circle: what comes out of the reverb
goes back in front of the filter or into the oscillators. `Engine::renderChunk`
keeps the previous chunk's output mix (before mid/side, sub and master) in a
ring, low-passed at *Tone*, driven into a gain-compensated rational tanh
(*Drive* 0 → unity, 1 → ×10 into the curve). The next chunk reads it back
two ways:

* **To Bus** adds it to the near bus after the voices, i.e. before
  ensemble, both delays, the cloud and cosmos sends and the near reverb —
  the "after the reverb, back before the filter" loop through the whole
  foreground and background chain. This path adds energy, so it is
  throttled by the mix's own mean level (feedback → 0 at 0.1, about
  −20 dBFS, 50 ms follower, ramped across the chunk).
* **To Pitch** phase-modulates every partial of every voice by h·θ, θ =
  3·amount·feedback radians. In the phasor bank that is one extra
  small-angle rotation per partial (tan clamped to ±0.4, then two Newton
  steps of 1/√ so the phasor stays on the unit circle to 1e-4 per sample);
  the clamp makes deep modulation saturate softly on the high partials
  instead of tearing. Phase modulation moves energy between partials
  without adding any, so this path is not throttled — a loud drone keeps
  its modulation. Only the FM path pays: a four-note Slow Chorus Field
  renders at 27× realtime without and 15× with it.

The loop is one chunk late (≤ 512 samples, ~10 ms), which is nothing in a
loop that runs through a 25-second reverb. Silence stays silence (nothing
in, nothing to feed back). Measured: To Bus 1.0 with Drive 1.0 on a held
A3 stays below a peak of 0.95 for 20 s and is louder than the dry note;
To Pitch 1.0 moves more than 30 % of a C4's energy away from its exact
harmonics. The throttle ceiling was first 0.25: Distant Storm then climbed
10 dB and collapsed to a stereo correlation of 0.6, because a loop near
unity gain circulates through the delay's cross-feed until both ears carry
the same thing. At 0.1 the loop thickens a drone without taking it over.

### Output DC blocker

One high pass at 4 Hz sits between the mid/side stage and the master gain, below the lowest
sub the Foundation can reach (at 20 Hz it costs 0.17 dB). Several paths can leave an offset
behind -- FM at an integer ratio, the tape stage's asymmetric term, a granular window over a
clip that carries one, the shimmer's pitch shifter -- and an offset costs headroom in the soft
clipper without ever being heard. The blockers inside the voice's FM slot and inside the
feedback loop stay: they exist to stop a loop locking onto a DC operating point, which a filter
at the end cannot do.


## Real-time budget

What the audio thread is allowed to do, and what was measured.

**No allocation, no lock.** Every buffer is sized in `prepare()`; a scan of `Core/src/*.cpp`
finds no `new`, `assign`, `resize` or lock inside `process`, `render`, `readParams`, `control`,
`routeStep` or the note calls. The plugin's `processBlock` takes one `ScopedTryLock` for the
recorder (never blocks) and calls `setSize` on its scratch buffer only if the host hands it a
block larger than `prepareToPlay` announced. Parameters are cached `std::atomic<float>*` taken
once in the constructor -- never a string lookup. Data that crosses threads (user scale, user
wavetable, texture, route) is double-buffered and published with an atomic version.

**Control rate.** Modulation runs once per `kControlBlock` = 64 samples (1.3 ms); levels that
must not step use per-sample `Smoother`s.

**No transcendental in a per-sample loop.** `sin01` is a table with linear interpolation, and
every oscillator, window and formant runs as a rotating phasor seeded by `phasorFrom` (Dsp.h),
renormalised with one Newton step so it stays on the unit circle. Two places had been missed and
were found by scanning for `std::sin|cos|exp|pow|log` inside sample loops: the GrainCloud's Hann
window (a `std::cos` per sample per grain, up to 32 sounding) and the Foundation's sub, whose
glide is a one-pole in the log domain and took a `std::exp` per sample -- the block's end point
is known in closed form, so two `exp` per block and a linear walk do the same job. Measured on a
cloud- and sub-heavy render, interleaved A/B over four runs: 1.00-1.10 s before, 0.86-0.94 s
after, about 15 % (19.6x to 23.0x realtime).

**Denormals.** Flush-to-zero and denormals-are-zero are set at the top of `Engine::process` and
restored at the end: `_mm_setcsr` on x86, and FPCR bit 24 on ARM64 -- which had been missing, so
the Quest ran every decaying reverb tail into denormals.

**Voices.** A voice is rendered only while its envelope is not idle; release ends at level 1e-4
(-80 dB). The allocator takes a free voice, else the quietest releasing one, else the oldest.

**SIMD, in one place.** The partial bank is a flat `float` array per strand (structure of
arrays), and the compiler vectorises parts of it -- but not the reduction, because floating-point
addition is not associative and it may not reorder a sum on its own. That one loop is written by
hand in `Core/include/ambient/Simd.h`, AVX2 and NEON beside a scalar path that every other
platform compiles (median 39 to 52 times realtime when it was written). The scalar and the
vectorised path are the same arithmetic in a different order; the difference between them is the
last bit or two of the sum.

**Measured again, 13.09.2026, and it found what the header had left behind.** The bank has three
inner loops and only the mono one was ever vectorised. Timed on the same patch -- one additive
source, 32 partials, six strands, every effect shut, the fixed cost of starting the process
removed by taking the slope of two render lengths:

| the bank's inner loop | of a core, scalar | vectorised | against the mono one |
| --- | --- | --- | --- |
| mono, hand-written AVX2 | -- | 4.9 % | -- |
| stereo (Partial Spread above 0) | 10.3 % | 5.6 % | +111 % -> +15 % |
| feedback FM (To Pitch above 0) | 16.7 % | 6.4 % | +241 % -> +31 % |

Both were the vectorised loop plus a little: the stereo has two accumulators instead of one, the
FM adds a phase term and a Newton renormalisation. 2332 presets of the library carry Partial
Spread and 1038 carry feedback FM -- about a quarter of it between them -- and for those the voice
had been costing two to three and a half times what it needed to. Both are written out now, AVX2
and NEON, so the Quest gets them too; the plain loop got the NEON path it had never had, having
been AVX-only since it was written. A whole-preset measurement puts it in scale: the dearest
preset of the library (Umbra Drift) went from 19.9 % of a core to 16.4 %, the cheapest (Wire Span)
stayed at 3.0 % because it uses neither.

`Tests/BankChecks.h` holds all three to the definition, worked out in double, at lengths that land
on a lane boundary and lengths that leave a tail on eight lanes, on four, or on both; the self test
runs them and `ambient_banktest` runs them once per vector path (AVX2, NEON through the x86 shim,
scalar), the way the convolver's checks have been run since the Room was built.

The same round found the grain ring (`GrainRing.h`, the loop the Cloud and the Memory share) with
an AVX2 path and no NEON one at all: the Quest rendered every grain of both of them scalar. It has
one now, written like the texture source's -- four lanes for the window, the level and the two
accumulations, the interpolation left scalar because NEON has no gather and four places in a ring
are four loads however they are spelled. The selftest had been holding that loop's vector path
against its scalar one since the Cloud was built and had been green throughout, which it would
have been with no vector path at all: a check of the form "the two agree" is worth nothing until
something says there are two. `ringGrainPath()` says which one was compiled, and banktest holds it
to the path its variant was built for. Writing that test
cost two mistakes worth keeping: a double-precision reference run ALONGSIDE a single-precision
recurrence diverges -- the Newton correction pulls each onto its own circle -- and failed all three
paths including the scalar one that had not been touched, which is the tell that an oracle is
measuring itself rather than the code; and a sum of partials that cancel is near zero, so the error
of a reordered sum has to be measured against the sum of the terms' MAGNITUDES and not against the
sum, or a correct vector path fails for arithmetic nobody got wrong.

A profiler was not needed for any of it, and would have been the slower way round: what said
where to look was the disassembly of the release build (797 packed against 756 scalar float
instructions in the sources object, but four against thirty-one inside `renderAdditive`) and then
the source itself. `renderAdditive` turned out to be the wrong suspect -- it runs once a block
over at most 32 partials and calls the real loop -- which is the reason to read the code the
instruction counts point at rather than trusting the count.

No fast-math: the offline render is the determinism oracle of the self test, and reassociation
makes it drift. LTO is an option (`AMBIENT_LTO`), off by default -- measured on MSVC it changed
nothing (1.05-1.16 s either way).

**Known hazard.** `processBlock` writes host parameters through `setValueNotifyingHost` for the
gesture layer, the route cursor and, on the block where the map is switched off, for every
parameter at once. JUCE allows this from the audio thread, but what a host does with it is the
host's business; the burst on map exit is the one place where it is more than a handful.

## Modulation

`Core/{include/ambient/Modulation.h, src/Modulation.cpp}`. Until this existed, every modulator in
the instrument was soldered to one destination and carried its own depth and rate: the pitch
drifter to pitch, the filter drifter to the cutoff, the Kuramoto ring to brightness, distance,
pan and the z-plane point. Adding a source always meant adding another pair of knobs. This is the
general form; the soldered drifters stay, because they are per-partial and per-strand and a
matrix row cannot reach in there.

**Eight LFOs**, shapes Sine, Triangle, Ramp Up/Down, Square, Random, Steps and Table. Rates from
one cycle in twenty minutes to 20 Hz. The square's edges are ramped over 7 % of a cycle and the
random shapes interpolate: the standing rule that a modulator may not step holds here too. `Table`
reads a frame of the loaded wavetable, so any of the generated tables -- and any curve drawn into
one -- is an LFO shape, without a second mechanism for drawable modulators.

**Six envelopes**, up to sixteen breakpoints each, a curve per segment, an optional sustain point
and an optional loop between two points. Their clock is a phrase clock: it restarts when a note
arrives into silence, not on every note of a cluster, or a shape spanning a minute would never
get anywhere.

**A matrix** of thirty-two rows, `source -> target x depth`, with an optional second source as the
amount and a unipolar flag. Depth is a fraction of the target's own range, so the same number
means the same thing on a cutoff in hertz and on a mix in 0..1. One source may appear in as many
rows as it likes -- that is the whole point, and what the soldered drifters could never do.
Performance state (morph, macros, the map cursor, the route) is never a target: modulating the
morph position from inside would fight the hand holding it.

The LFO settings and the envelope times are parameters, so a host automates them. The shapes and
the matrix rows are **data**, the way a Scala scale and the gesture mappings already are: a text
form that travels in the preset (fields 7 and 8 of a pack line), in `.noctuary` files and in
the plugin state. Thirty-two rows as four parameters each would put a hundred and twenty entries
into the automation list for very little gain.

Modulation is added after the inertia glide: a modulator moves at its own rate, it is not slewed
by the setting that exists to slow the performer's hand down.

### Measuring without a temporary file

The measurement line also carries a `hash=` of the audio itself. Descriptors are averages: two
renders can agree on every one of them and still not be the same sound, and a regression that
moves energy around without changing its statistics would pass unnoticed. The samples are
quantised to about -120 dB before hashing, so a real change in what is played trips it while the
last bit of a sum does not (the vectorised partial bank moves those, and moving them is not a
change). Verified: the same render twice gives the same hash, and a cutoff moved by one hertz
gives a different one.

`ambient_render --measure` renders and prints the descriptors as one line instead of writing a
WAV. The measurement pass over the library used to render each preset to a temporary file and
read it straight back: five thousand presets times twelve seconds of stereo float is twenty-three
gigabytes written and read for nothing, and all of it stayed in the file cache. On a machine with
64 GB that filled the standby list to 38 GB, at which point Windows began trimming the working
sets of the applications on screen and switching between them stuttered. The WAV reader also asks
for `FILE_FLAG_SEQUENTIAL_SCAN`, so the clips it reads are aged out of the cache instead of kept.
Measured afterwards: a full pass grows the standby list by well under a gigabyte instead of
tens of them.

## The Rich refinements

Ten small mechanisms, each a way the sound stops being a synthesizer and starts being a place.
Every one is off by default and was measured sound-neutral there (37 presets identical before
and after); each was measured effective on its own.

* **Phase Width / Phase Rate** (Space): two first-order all-passes per ear whose corner
  frequencies (300 and 1500 Hz) drift apart and back on one slow curve, the left ear up and the
  right ear down by up to 1.5 octaves. The phase relation between the ears changes, the level
  does not, so the ear reads a room changing size rather than a sound moving. Measured: width
  descriptor 0.73 to 0.99 on the default patch at full depth.
* **Doppler** (Space): the breathing distance has a velocity; a voice coming closer rises, one
  receding falls, up to 3 % (one plane unit taken as about twenty metres).
* **Blur** (Foreground, a second Nebula): the near bus through a spectral smear ahead of every
  effect, so an attack is wiped into texture and one note flows into the next. Mix and Smear;
  the blurred part is 43 ms late, the dry part is not.
* **Formant** (tenth filter model): three band passes on the formants of u-o-a-e-i, Cutoff
  morphs the vowel, Resonance narrows the formants, Drift and an LFO make it breathe.
* **Strike** (its own tab): a Karplus-Strong loop excited with a noise burst at note-on --
  String at the note, Wood two octaves up with heavy damping and a short decay, Metal with an
  all-pass in the loop -- on the near plane whatever the voice's distance. Fires for keys, or
  for the brain's notes too. The intimate impulse that makes the background behind it vast.
  Since 1.10.0 it also has a *Chance* and a *Cluster* (Rene: "wird ein Strike eigentlich ab und
  zu mal ausgelöst wenn man die Drone automatisch laufen lässt?"). It was all or nothing: on
  Keys nothing ever struck while the instrument played by itself -- measured, a fifteen-minute
  render with the strike at 0.8 and one with it at 0 differed by exactly zero -- and on
  Keys + Brain every conductor note struck, roughly one every 14 to 39 s depending on the
  preset. Chance is the middle: the conductor's notes strike with that probability, the keys
  always. Cluster weighs the coin by the cascade's excitation *against the average it has been
  running at*, `p = clamp(chance * (1 + 2*cluster*(exc - excAvg)), 0, 1)`, so the strikes gather
  where the events gather and the count still follows Chance; weighing by the excitation itself
  saturated the probability at one and struck everything. The coin has an Rng of its own and is
  drawn only below Chance 1, so older presets render bit for bit (oracle 41/41). In the library
  2116 of 8400 presets have a strike at all, 1663 of them on Keys + Brain -- so 453 carry a
  strike that never sounded unless somebody played.
* **The Cosmos swelling with the cascade** (`cosmos_swell`, 1.10.0, on at 0.5 by default). Rene's
  next question was what the Cosmos does when the drone runs by itself. Nothing had to be fixed
  about triggering -- it is a send, not an event -- but the stems said the other thing: over five
  minutes with the conductor alone, the Cosmos bus was at -34.4 dBFS with exactly one second of
  300 below 2 % of its peak. It never rests. Swell makes the send follow the conductor's
  excitation, relative to the average that piece has been running at:
  `send * clamp(1 + 3*swell*(now - avg)/max(avg, 0.15), 0, 2)`, with `now` the excitation
  smoothed over two seconds (the Hawkes kick is a step, and a step on a send is a click) and
  `avg` over ninety. Relative and not absolute, so a piece whose cascade only ever reaches a
  fifth swells as much as one that runs hot -- and so that no cascade at all means no change at
  all. Default-on was safe to give it because **not one of the 6800 library presets sets
  brain_cascade**: with no cascade the excitation is zero, the deviation is zero, and the self
  test holds it to the sample (largest difference 0). With a cascade at 1: the level's
  coefficient of variation goes 0.41 -> 0.60 and its correlation with the (smoothed) excitation
  +0.13 -> +0.51. The same excitation is also a modulation source of its own now (`cascade`,
  `exc/(1+exc)`, a CASC card on the strip), the only source that comes from what the piece is
  doing rather than from a clock or a field of its own.
* **Drift** per source (Source 1..3): an independent slow pitch drift in cents. Three sources
  on just ratios each drifting on their own curve beat like an ensemble in a room whose
  temperature moves; nothing is symmetric, nothing cancels for long.
* **Absorb** (both delays): with Absorb up the feedback loop also loses its low end, and its
  high cut sinks as the feedback rises (at full feedback and full absorb the loop keeps
  320 Hz to 1 kHz), every repeat passing through the band again: echoes drown in a fog
  instead of merely getting quieter.
* **Tide / Tide Period** (Tuning): the whole instrument's pitch leans by up to 30 cents on a
  minute-scale curve; the sub follows, so the harmony stays.
* **Rotate** (Far Reverb): the far field's left and right rotate into each other on a slow
  curve; the background turns.
* **Golden-ratio LFO defaults**: the eight LFOs start at 0.03 Hz times the golden ratio to
  the n-th power, so their cycles share no common period and their extremes never line up.
  The coherence ring's natural periods were already primes (23 / 31 / 41 / 53 s).

Every new random source (the phase field's drifter, the sources' pitch drifters, the tide and
the rotation) seeds from a side stream rather than from the voice's or the engine's, so
switching one on never moves the brain's dice or a preset's random phases. Ten built-in
presets (168..177, "rich studies") show each one; the library generator draws them per style.

## The instrument as a place: body, unmasking, patina, externalisation

Four stages that are about the room the sound is in rather than the sound itself. All four are
off by default and were measured sound-neutral there.

* **Body** (`Core/include/ambient/Body.h`): twelve modes tuned to the brain's root, fed from the
  finished mix and returned to it -- the soundboard a pad sits on. Four materials, which are four
  sets of mode ratios: Wood (a plate's irregular low modes), Plate (the stretched series of flat
  metal), Bell (hum, prime, tierce, quint, nominal), String (harmonic with a little stiffness).
  Decay is the lowest mode's T60; the higher ones die away faster at a rate belonging to the
  material; Tone tilts the gains, Spread scatters the modes across the field. Two things had to
  be measured rather than assumed: a resonator normalised to unity at its peak passes almost
  nothing of a broadband signal (the Body knob moved the mix by 0.02 dB), so the level is
  normalised on the expected *power* instead, the way the Air band is; and modes scattered
  randomly around the centre are all driven by the same mono signal and therefore correlate the
  two channels (width 0.69 collapsed to 0.11), so neighbouring modes sit on opposite sides. Q is
  capped at 300, because a mode narrower than that is never excited by a drone that drifts.
* **Unmask** (Far Reverb): the background steps aside for the foreground band by band -- three
  bands, the near bus as the side chain, 50 ms to duck and 1.2 s to return. It is what a mixing
  engineer does by riding the reverb return, and what keeps a dense pad from swallowing its own
  notes.
* **Patina** (master): tape wow and flutter as a moving read point, the top end a worn machine
  has lost, a noise floor that rises a little with the signal (modulation noise, which is what
  makes a floor sound like tape rather than like dither), and a gentle saturation. Bypassed
  entirely at zero.
* **Externalise** (Space): the two cues a headphone image needs to sit outside the head -- the
  notch the pinna cuts into what arrives from the side, whose frequency moves with the voice's
  angle, and the reflection off the shoulder a quarter of a millisecond later. Brown and Duda's
  structural model, the parts of it that need no measured data.

## Playing it: expression, two conductors, the grid

* **Expression** (MPE, aftertouch, CC 74, bend): pressure pulls a voice towards the listener --
  the plane already decides brightness, level, dryness and presence, so one finger moves all of
  them the way leaning into a note does -- and can also open brightness and level directly; the
  sideways slide moves that voice's filter and its point in the z-plane; bend is per note. With
  MPE on, channels 2 to 16 each carry one note with its own three; without it, the wheel and
  channel pressure apply to every sounding voice. Everything is smoothed inside the voice.
* **Brain Quantize**: the conductor's decisions wait for the next note value of the clock, and
  all of the waiting time is handed over at the tick, so the mean rate is unchanged.
* **Autoplay**: the conductor in *Chords* mode keeps the cluster full and exchanges one voice at
  a time, on a timer, on the clock or by hand (see above). *Free* -- the original conductor -- is
  the default and is not going anywhere.
* **Brain 2**: a second conductor with its own register, pace, density and plane, on the first
  one's root plus an interval, with its own random stream. Two of them play a slow counterpoint
  neither would play alone.
* **Room Morph**: a second impulse response and a crossfade between the two rooms. The second
  convolution only runs while the morph is actually between them.
* **Level matching** (plugin): every preset's loudness was measured from a twelve-second render
  and stored in its metadata; while level matching is on, loading a preset trims the master gain
  towards a common target (at most 12 dB) so that auditioning a hundred presets is not a ride on
  the volume knob. A preset that was never measured is left alone.

## Stems, and a score

**Stems.** `Engine::setStemBuffers` takes eight pointers -- near, far, cosmos and room, left and
right -- and fills them alongside the mix; `ambient_render --stems <prefix>` writes the four
files. Each stem is what its plane contributes where it joins the output, so the four sum to the
mix *before* the master stage (the mid/side split, the output DC blocker and the soft clipper come
after them). Two details had to be measured rather than assumed: the Cosmos return is added into
the near bus, so it is subtracted from the near stem or it would be counted twice (it was, and the
four summed 1.6 dB loud); and what the Cosmos sends into the far plane cannot be separated at all,
because the reverb has already mixed it with everything else -- it belongs to the far stem, which
is where it is heard. The self test renders four hundred blocks and compares the mid channel of
the mix with the sum of the stems; measured, the difference sits below -30 dB, and what is left is
the master stage: it is in the side channel (-14.5 dB) and below 50 Hz (-16 dB), which is exactly
the Bass Mono high-pass and the DC blocker.

**A score** (`Core/include/ambient/Score.h`). The set timeline records what you did; the route
walks the map; neither lets you write a piece. A score is a text file of timed ramps:

```
0:00    cosmos_send 0
6:00    cosmos_send 0.45 over 8:00
34:00   far_decay 90 over 6:00
40:00   brain_on off
```

`<time> <parameter key> <value> [over <duration>]`, times as m:ss or h:mm:ss. Without *over* the
value is set at that moment; with it the parameter travels there from wherever it was when the
ramp began, in the parameter's own skewed domain -- the same one the morph and the map blend use,
so a logarithmic knob moves the way a hand would move it. Choices and switches change when their
ramp ends rather than halfway. `ambient_render --score <file>` plays one offline and takes its
length from the score; `docs/example.score` is a forty-minute piece to start from.

## Clock and sync

`Core/include/ambient/Clock.h`. Where the tempo comes from is one setting,
*Clock Source*: **Internal** (the *Tempo* parameter, counting beats while *Run*
is on -- the standalone's own clock), **Host** (the DAW's play head: tempo,
position in quarter notes, playing; the plugin hands it over once per block),
or **MIDI** (MIDI clock at the input, 24 ticks a quarter; the tempo settles
over a beat's worth of ticks so interface jitter does not wobble every synced
LFO; Start/Continue/Stop; two seconds without a tick and it falls back). Host
and MIDI fall back to the internal clock when nothing arrives. The clock's
parameters are performance state like the morph: no preset touches the tempo.

Every rate that wants the grid keeps its free knob and gains a *Sync* choice
(Free, 64 bars … 1 bar, 1/2 … 1/32 with dotted and triplet values); when set,
the division at the current tempo replaces the knob: the eight LFOs (one
cycle per division, and the phase follows the beat position, so a synced LFO
stays on the grid however long it runs and wherever the transport jumps), the
six envelopes (the whole shape spans one division), both delays' left and
right times, the ensemble rate, the cloud's grain rate, the brain's event
rate, the arc period, and each source slot's grain density. Measured: a delay
on 1/4 at 90 bpm renders identically to 0.6667 s typed in; an LFO on one bar
changes the render between 90 and 180 bpm; every preset (all Free) is
unchanged.

### BEAT: the instrument listening to its own tuning

The Foundation's ghost tone already takes the two lowest sounding voices and
sings their frequency difference as a bass note. That difference is only half
the story. What the ear reacts to in a held chord is not the combination tone
but whether the interval is *in tune*: two voices a fifth apart beat at
|2f₂ − 3f₁|, which is silent when the fifth is just and quicker the further it
has drifted. **BEAT** is a modulation source whose rate is exactly that.

It finds the simplest just ratio near the interval the two lowest voices make
(from a short list — small numbers only, because those are the ones whose
harmonics are close enough together to beat audibly) and runs at the difference
between the harmonics that would coincide if the interval were exact. For a
mistuned unison that is |f₂ − f₁|, the difference tone itself. Below a fiftieth
of a hertz the phase simply holds: a chord in tune should leave whatever it is
driving exactly where it is, not creep.

So the sound breathes at the rate of its own mistuning. Purity Drift is what
sets it moving; route BEAT at a filter, at the Nebula's smear, at anything.

The test compares two intervals in one tuning rather than one interval in two
tunings, and the difference matters. The first version played a fifth with the
scale set to just and again in equal temperament, expecting near-zero and about
half a hertz. Equal temperament gave 0.469 Hz, which is textbook — two cents
narrow at that pitch. "Just" gave 5.8 Hz, because it was measuring the tuning
system and the per-voice pitch drift rather than this source. An octave is
exactly 2:1 in every temperament there is, so the test now uses that as its
zero: octave 0.000 Hz, tempered fifth 0.469 Hz.

### Six small things, and what measuring them cost

Six changes that each sound like a one-line tweak. Four were; two were not, and
the two that were not are the interesting ones.

**Gravity is now a magnet, not a mood.** The portamento's pull towards
consonant ratios braked in proportion to `intervalConsonance()` of the ratio
the slide happened to be passing through -- a quantity that rises and falls
smoothly across the whole glide, so the pull was everywhere and nowhere, more
like wading than like a magnet. It is now the distance, in cents, to the
nearest just ratio, with the strength `1 / (1 + (d/30)^2)`: half strength at
thirty cents, eight per cent at a semitone, nothing at all in between the
nodes. A magnet has almost no reach and then all of it.

**The Kuramoto ring is asymmetric.** Every pair pulled on the other equally,
so a high Coherence settled into exact synchrony and stayed there -- four
oscillators behaving as one, which is the opposite of what the section is for.
Each is now pulled a little harder by the oscillator behind it in the ring than
by the one in front (`w = 1 + 0.22 sin(2*pi*(j-i)/4)`). An antisymmetric
perturbation has no synchronous fixed point, so the bank locks in frequency and
keeps a slowly turning spread of phase, which is what a ring of coupled
biological oscillators does.

**The air ahead of the far reverb saturates.** A very gentle asymmetric shaper
between the diffuser and the reverb's feedback network, riding on Diffuse so
there is no new knob: a dense cluster fired into the hall comes back thickened
rather than reflected.

**A mono safety net in the master.** Everything upstream is built to widen, and
a drone that is gigantic in stereo can vanish when a phone sums it. The side is
measured against the mid over about a second and a half, and if the side is
half again the power of the mid the width is eased back -- by a quarter at
most, at two per cent a second, never touching the middle of the mix. It is a
parameter (*Mono Safe*, on) because someone may prefer the width and their own
ears.

That threshold cost a measurement. At "side louder than mid" the guard engaged,
minutely, on thirty of the thirty-seven reference presets -- by an amount too
small to move any descriptor, but it engaged, and a safety net that is always
slightly on is not a safety net, it is a change to the sound. At half again it
leaves the median preset bit for bit identical and pulls back the few that were
genuinely collapsing: the worst case gains 0.44 dB of level and loses 0.45 dB
of mono loss, which is the trade the feature exists to make.

**The delay ducks.** While the input is loud the high cut inside the feedback
loop drops, so a fresh attack does not fight the brightness of the last one's
tail. Three things had to be got right, and each of the first two produced a
confident wrong answer first:

* The trigger. A level follower simply darkens any loud passage, which is a
  tone control with extra steps. It is now the fast envelope against the slow
  one -- how far the input stands above its own average -- so a drone ducks
  nothing and an attack ducks hard.
* The release. Ten milliseconds, which meant the loop was dark for the first
  echo and open for the rest; the measured effect was five per cent and looked
  like nothing. A second lets a whole train of echoes stay out of the way.
* The measurement. The first version compared high-frequency energy *relative
  to total* energy and reported that ducking makes the loop brighter -- a
  darker feedback loop builds up less of everything, so the ratio rises while
  the sound plainly darkens. And it measured during the plucks, where the wet
  output is the unfiltered delay read and the first repeat is as bright as the
  attack by design. Measured absolutely, in the echoes, it is fifteen per cent
  of the high end.

One claim is deliberately not asserted anywhere: that the loop "opens again
once the note has gone". The envelope does release, but it cannot be shown in
the audio, because a darkened feedback loop also loses energy faster -- by the
time the filter has opened there is almost no tail left to be brighter. The
test says so in a comment rather than asserting something the signal does not
do.

### Modal mode: the same bank, read as objects

The cascade shapes; a modal bank rings. That is the whole difference, and it is
larger than it sounds. Six biquads in series take away what is not wanted and
leave what is: stop the input and the filter stops. Six two-pole resonators in
*parallel*, each with its own decay time, keep sounding after the input has
gone -- which is what a bar, a bell, a membrane or a room actually does. It is
modal synthesis (Smith, *Physical Audio Signal Processing*; Bilbao, *Numerical
Sound Synthesis*), and it costs almost nothing here because **the data was
already in the building**: the 155 shapes are mode series of struck and blown
objects taken from the acoustics literature, and the cascade was using them as
filter frequencies. Modal mode uses them as what they are.

`z_mode = Modal` (a fourth value, appended, so no preset that names Series or
Replace is touched) with two parameters of its own: *Decay*, the T60 of the
lowest mode, up to forty seconds; and *Damping*, how much shorter the higher
modes ring -- 0 for everything holding equally, which no real object does, 1
for decay inversely proportional to frequency, which is roughly what wood,
metal and skin do.

Every resonator is normalised to unity gain at its own frequency, so a steady
tone at a mode cannot make the bank run away however long the decay is set, and
the bank as a whole is normalised on expected power rather than on the sum of
the peaks -- the modes are at different frequencies and almost never in phase,
so adding peaks would leave a bank of six far quieter than a bank of two.

The self test measures the two claims rather than repeating them: an impulse
in, and the tail has to be clearly present at half the stated decay and roughly
sixty decibels down at the decay itself, for 0.5, 2 and 8 seconds; and damping
has to leave less energy in the tail than no damping. The 44 modal presets --
one for every shape that is a physical object -- are rendered and measured with
the rest.

**And the descriptors were not enough this time.** Five modal presets came out
as identical twins: a bank of narrow resonators fed with noise measures almost
the same whatever its modes are. Their rendered audio hashes were all
different. The twin test now needs both -- the same descriptors *and* the same
hash -- which is the third time in this instrument that a measurement had to be
sharpened before it meant anything, and the third time the symptom was a
confident report that everything was the same.

### The ZDF question

The first thing in the literature on modulating filters is Zavalishin's
topology-preserving transform: solve the zero-delay feedback rather than let a
unit delay sit in the loop, and a filter stops detuning and clicking when it is
swept. That has been in this instrument since the beginning -- `Svf` in
`Dsp.h` is exactly that structure (`g = tan(pi f / sr)`, the trapezoidal
integrators, the feedback resolved algebraically), and the low pass, high pass,
band pass, notch, peak and formant models are all built on it.

The one place that is *not* zero-delay is the four-pole Ladder, which keeps a
sample of delay in its feedback path on purpose: that delay is part of what the
model sounds like, and every preset that uses it was voiced with it. A
zero-delay ladder would be a different filter and belongs beside it as a new
model rather than in place of it.

### Envelopes, and a control that was only a picture

The voice's amplitude envelope has always been a plain ADSR -- Attack, Decay,
Sustain, Release, with times up to a minute for the attack and two for the
release, which is what a drone wants. The six modulation envelopes have always
been more than that: up to sixteen breakpoints, a curve on every segment, an
optional sustain point and an optional loop.

What they did not have was any way to reach them. The four knobs under each
curve set the mode, the time scale, the depth and the sync; the *shape* could
only arrive from a preset or from the text form. So the curve on the panel was
a drawing of something the user could not touch, and the honest impression it
gave was that the instrument could not manage an ADSR -- which it could, twice
over, in two places nobody could get at.

The curves are edited on the curves now: drag a breakpoint, double-click the
line to add one or a point to remove it, right-click for the sustain point, the
loop, the curvature of a segment, and ten shapes to start from with ADSR at the
top of the list. The shapes live in the core rather than in the editor so that
the self test can check that each of them parses, starts at zero, keeps its
times in order and survives being written and read back -- a shape string with
a typo in it does not fail loudly, it simply does nothing when the menu item is
picked, and the only symptom is somebody clicking ADSR and watching nothing
happen.

One thing worth remembering from building it. The first version of the hit test
had its own copy of the row's geometry -- 8 and 16 and 0.34 against the
drawing's 5 and 10 and 0.36 -- and left the Depth knob out of the vertical
entirely. It compiled, it ran, and it would have grabbed points several pixels
away from where they were drawn, harder to explain than a crash. The drawing
and the mouse now ask one function for that geometry, which is the only way two
of them can never disagree.

**And then nobody used it.** Counted across all 6191 presets afterwards: 3986
carried an envelope shape, every one of them routed, every one curved rather
than linear, and 45 % looping -- so the feature was being used, in the library.
But the ceiling was nowhere near: envelopes 1 and 2 only, at most six of the
sixteen breakpoints, and *Sustain Loop*, one of the three modes, used by none of
six thousand. And not one of the built-in presets carried a shape at all, so
somebody clicking through the Sound box would never have met the thing.

Both of those were the generator's ceiling rather than the instrument's
(`Tools/library/make_presets.py` drew "nought to two envelopes, three to six
points" and never a sustain point), and both are lifted: up to six envelopes on
a golden ladder of times, up to sixteen points, and a sustain point on a third
of the shapes that are long enough to have something to rise through and
something left to run out. Five built-in presets show the range -- *Long Arc*,
*Held Breath*, *Six Hands*, *Sixteen Points*, *Dwelling Curve*.

Building *Held Breath* found that the sustain point did not actually work.
The clock the envelopes run on is a phrase clock, restarted when a note arrives
into silence; while a note was held the shape correctly waited at the sustain
point, but at the moment the last voice let go the release found that clock far
past the end of the shape and **snapped** to its final value. Not a glide, a
jump -- the one thing this instrument is not allowed to do. Each envelope now
has its own clock, and on that transition the clock of a Sustain Loop envelope
is put exactly on the sustain point, so the tail plays from the value the hold
ended on. Nothing that existed changed: no preset used the mode, and the 37-
preset sound oracle reports 39 identical, 0 different. The self test drives the
engine through hold and release and fails if the value moves by more than 0.02
across the release -- which it did, before the fix, by half the shape.

The wider lesson repeats one from the presets round. *Sixteen Points* was built
first with the envelope pointed at a formant filter over a bright thin drone,
and measured, its envelope did nothing at all: the centroid moved 3.0 % with the
envelope on and 3.1 % with it off. Two reasons, both invisible from the settings.
The *Air* band, at 0.24, is broadband noise sitting on top of the spectrum and
pins it -- with it in, sweeping the cutoff from 400 Hz to 4 kHz moved the
centroid by 8 %. And the conductor changing notes every few seconds moves the
centroid far more than any filter does, so the thing being demonstrated was
buried under the thing that was not. Rebuilt sparse, long-held and nearly
airless, the same envelope moves it from 3.9 % to 11.1 %. A demonstration has to
put the thing being demonstrated in front of the microphone, which is the same
sentence as three of the traps in this file.

### Loudness, and why an ambient synth measures it

The literature on this music is unanimous that loudness is the enemy: a
brickwall limiter takes the finest amplitude movement out of a reverb tail and
leaves it grainy and flat, and the impression of an enormous room comes from
the distance between the quietest texture and the loudest swell rather than
from the average level. Targets quoted for the genre are -18 to -24 LUFS
integrated, a crest factor above 14 dB and a true peak at -1 dBTP, against
-9 LUFS and 6 to 9 dB for commercial pop.

So the instrument measures itself, to ITU-R BS.1770-4: integrated (both gates),
short-term, momentary, loudness range, true peak between the samples, and the
crest factor. `Core/src/Loudness.cpp`, framework-free like the rest of the core,
fed from the engine after the master stage -- the integrated figure is a number
about the whole piece, and a meter that only sees what the interface happened to
ask for has holes in it.

Two things were worth getting right rather than approximately right. The
K-weighting is **not** the RBJ cookbook: given the standard's own f0, Q and gain,
the cookbook's shelf comes out about two per cent away from the coefficients
BS.1770 prints for 48 kHz -- close enough to look correct and wrong enough to be
wrong. The formulation used here reproduces the published numbers to sixteen
digits and is derived from the analogue prototype, so it holds at 44.1 and 96 kHz
too. And the whole meter was checked against an independent implementation of
the published coefficients over the same sixty-second render: -23.62 against
-23.62 integrated, -21.62 against -21.62 short-term. The self test pins a
constant that came from that reference, not from this code: a 997 Hz sine at
-20 dBFS RMS in both channels reads -16.99 LUFS.

### Four things the panel got wrong, and what it says now

Four observations from the same round, all right, and worth writing down as
reasoning rather than as a changelog.

*Strands had a tab of its own.* It is not a source: it is the strand bank of
Source 1's additive type -- unison, detune, stack, bloom -- and it is greyed
out the moment Source 1 is anything else. A tab suggested a fifth thing beside
the four sources. The ten controls moved under Source 1's own display first (a
page could name a section to place *under* its display), and since 1.9.0 they
stand beside Source 1 on its page, the bank's picture to the right of both, and
are not on the page at all while the slot is anything but additive; the row
lost a tab that only ever meant "Source 1".

*Strike sat among the sources.* It is a sound source, but not one of the four
oscillators: it fires at note-on, independent of what the slots do, exactly
the way the Cosmos is independent of them. It now shares the Cosmos group as a
tab -- COSMOS | STRIKE -- and the sources row is the four sources and the
Vector.

*There was no button for the main page.* Perform, Browse and Help each had a
toggle; the panel was where you landed by switching them off, a rule nobody
should have to learn. **Main** is a button now, lit while the panel shows;
Help sits last, where a manual belongs; and Calibrate and Gestures -- the two
controls only a headset needs -- live under one **VR** button as a menu
instead of taking two places on the toolbar of an instrument that mostly
plays on a desk.

*The Matrix tab was a text box.* One route per line, "lfo1>cutoff:0.4", and an
Apply button: exact, scriptable, and the wrong thing to put in front of a
musician, who opens a tab called Matrix expecting to see routes rather than
their spelling. A grid of every source against every parameter is not the
answer either -- thirty-odd sources by four hundred targets is a wall with a
dozen live cells in it. It is a table now (`EditorMatrix.cpp`): one row per
route, each a source, a target grouped by section, a depth you can drag, an
optional via source that scales it, and whether the source is read as 0..1 --
with "+ route" and a remove on every row. It hands the whole matrix back to
the engine as the same text the box used, so the parser, the presets and the
drag-a-card-onto-a-knob path see nothing new.

### The manual

The help page inside the instrument is the manual, and it exists as a PDF as
well, so that somebody can read it before installing anything. It is not
written twice. `AMBIENT_MANUAL=<folder>` makes the standalone export its own
help -- every topic's text, and its pictures -- and `Tools/make_manual.py`
turns that folder into an HTML book and prints it.

The pictures are the point. They are snapshots of the panel itself, taken from
a running instrument as each help topic is opened: the real sections with their
real values, and the live displays with something actually in them. A drawing
of a section goes stale the day the section changes and nobody notices for a
year; these cannot, because they are made again every time the manual is.

Three things had to be learned to make that work, and all three are the kind
that produce a plausible-looking wrong answer rather than an error:

* The export waits five seconds before taking its pictures. The displays of a
  running instrument -- its spectrum, its stage, its note roll -- are empty
  until it has been playing for a while, and a manual illustrated with empty
  boxes is worse than one with no pictures at all.
* It runs with a preset in which all three sources and the effects are in use.
  The pictures are of the panel as it stands, so the first draft illustrated
  its Sources chapter with a section greyed out because Source 3 was off.
* `juce::String::formatted` is wide-character, so `%s` handed a `const char*`
  writes the bytes as UTF-16. The first run produced a file called
  `topic-00-汦睧.png`.

And a fourth, in the printing: Edge's old `--headless` flag exits with status
zero and writes nothing at all on version 152. `--headless=new` prints in a
second. The tool tries the new flag first and falls back, so an older browser
still works, and if there is no browser at all the HTML is still written --
the manual is not held hostage by one.

### The manual, second draft: every block, every type, every tab

The first manual was a book of chapter texts with two or three section
pictures each and an appendix of every parameter. Read by somebody who did
not have the instrument in front of them it was thin in exactly the places a
manual is for: what does the *Feedback + Room* tab contain, what do its knobs
do, what does the *Stretch* type look like when it is playing. The second
draft answers those questions mechanically, so they cannot go unanswered
again:

* **Every tab of the panel is a picture**, photographed as the tab (its bar,
  its sections, its display, and on Source 1 the strand bank under the
  display), captioned with the name it wears on its bar. Twenty-four tabs,
  the three tabs of the modulation strip, the Perform and Browse pages, the
  sections that have no tab (Space, Foundation, Strands, the Master in its
  corner of the header). The in-app help page does not draw these -- its
  picture column holds two or three -- so they are a second list
  (`tabPics`) that only the export reads.
* **Under every picture a paragraph says what the block is for**, and under
  that every parameter of the sections in the picture with its help text.
  The paragraphs are a table in `Help.cpp` (`tabHelp`) keyed on the tab's
  name; the parameter lists come from the same `paramHelp` the tooltips use,
  exported structured (`params` in `manual.json`) rather than as the text
  blob the appendix is. Sources 2, 3 and 4 are the same twenty-six knobs
  three times over, so they are printed once under Source 2 and the other
  two pages say so.
* **The gallery of source types.** A slot's picture shows whatever type the
  preset happens to use. For the manual the export sets Source 2 to each type
  in turn -- Additive, Wavetable, FM, Texture, Stretch, Noise -- loads a field
  recording for the two that need a clip (`AMBIENT_MANUAL_CLIP`), waits for
  the display and the greyed-out knobs to follow, and photographs the tab.
  That is why `exportManual` became a list of steps a third of a second apart
  rather than one function: a parameter set from the message thread reaches
  the display on the next timer tick, not in the same call.
* **The presets chapter explains the groups**: the twelve families of the
  built-in presets and all fifty-six packs, each in a sentence -- who it is
  written in the spirit of and what corner of the repertoire it covers --
  rather than the list of numbers it was.

Two pictures were wrong in the first draft and both for the same reason. The
Master section is painted in the header, so its bounds are the editor's, not
the content's; photographed from the content it came out as a strip of the
panel's top-left corner, which is what a reader saw under "MIDI, OSC, files".
And the signal-flow diagram is a fixed canvas scaled to fit its component, so
one of the two dimensions is always left over; photographed whole it was a
diagram with a field of black under it. Both now photograph what is drawn
(`drawn()`), from the component that draws it.

The diagram itself was redrawn: it still showed three sources, nine filter
models and no Vector, Strike, Blur, Body or Patina, and it drew the master
chain in an order the code does not run it in. It now shows the four slots,
the Vector and the Strike, both filters with the fold, the foreground with
the Haas band and the microshift, the background with its own width, and the
output chain in its real order -- body, mid/side, sub, patina, subsonic,
master, clip.

And the printing broke a second time, differently. Edge with the default
profile -- or with any profile the script had used before -- takes the
command over and exits at once, without a file; a reused folder printed
nothing in twenty seconds, a fresh one in two. The launcher also returns
before the child that prints has written anything. `make_manual.py` now
makes a new profile folder per print, removes it afterwards, and waits for
the file to appear and stop growing.

### The mixing desk

The dark-ambient production literature -- a second paper after the ambient
sound-design one -- is mostly about mixing: stereo width that survives mono,
depth as a funnel, a wavefolder where a saturator would be, aftertouch on
three things at once. Measured against the instrument, most of it was there
(bass mono, side air, three reverb tiers with pre-delay and low cuts, air
absorption with distance, free-running LFOs, comb filters, Paulstretch, the
LUFS meter). Five things were not, and all five are now parameters that are
neutral at their default, so six thousand finished presets sound as they did:

* **Aftertouch, the wheel and the slide as modulation sources**
  (`pressure`, `wheel`, `slide`). Pressure and slide already reached the
  sound through fixed routes; the wheel reached it only through MIDI learn,
  one knob per controller. Now all three are ordinary sources, read from the
  loudest voice (pressure, slide) or the instrument (wheel, smoothed over
  30 ms so a 7-bit controller never steps a cutoff). They rest at zero, which
  through the bipolar mapping is -1, so a route wanting "nothing until I move
  it" carries the 0..1 flag -- and that is what makes them safe to put into
  every preset in the library, which the retrofit did.
* **The background's own width** (`far_width`). A mix in which everything is
  spread as wide as it goes is a flat wall; the far plane pulled in towards
  the centre while the foreground stays wide is what the ear reads as
  distance. A mid/side stage on the far bus alone, before it joins the near
  bus. Measured: width 0 leaves no side at all, 1 leaves the reverb's own,
  1.5 is wider, and the mid never moves.
* **Microshift** (`ens_mode`). The two channels detuned a few cents against
  each other, at different base delays, with nothing modulated. The textbook
  construction -- two taps half a cycle apart under a Hann pair -- has both
  taps audible all the time at a fixed delay difference, which on a sustained
  tone is a comb filter: measured, it lost a fifth of the signal. The version
  that ships uses a long ramp (200 ms of travel) and a short hand-over
  (25 ms), so the two taps overlap for a thousandth of the cycle and the
  shifter is a plain delay line at a slowly changing delay. Measured by
  counting zero crossings of a 440 Hz sine: left 443.06 Hz, right 436.96 Hz,
  twelve cents each way to within half a hertz; no step at the wrap; no dip.
* **The band-limited Haas effect** (`haas`, `haas_time`). Delaying a whole
  channel widens it and destroys it in mono. The 1.2-4 kHz band of the centre
  is delayed and put into the side channel -- added on the left, taken off on
  the right -- so the edges open, the low end stays, and a mono sum is exactly
  the picture it was, to the sample. The first version cross-fed the delayed
  band symmetrically and produced, from a mono input, no width at all: the
  test that found it fed mono noise and measured zero side. What "hard to the
  opposite side" comes to once it is made symmetrical is the side channel.
* **The wavefolder** (`filter_fold`), after both filters. A clipper flattens
  what will not fit; a folder reflects it, and the mirrored wave grows a
  family of high partials no saturation makes. `sin` is the smooth version of
  that curve, divided by its own gain so small signals pass unchanged; the
  positive half is driven a third harder than the negative, which is where the
  even harmonics and the body come from; the amount both drives and mixes, so
  the knob leaves the identity continuously. The makeup gain is measured: 2.2
  holds a 0.3-amplitude sine within 1.3 dB across the knob, and a loud input
  loses about 9 dB at the top, which is not a fault -- past the first fold the
  fundamental itself is being folded away.

The sound oracle -- forty presets rendered and hashed before and after --
came back forty identical. Then the library was retrofitted on purpose
(`Tools/library/retrofit_presets.py`, `Tools/retrofit_builtins.py`): every
preset judged from its own settings, the hands almost everywhere, the funnel
where there is a background deep enough to matter, the Haas band where there
is a foreground and it is not already at the edges, the microshift only where
the chorus was slow and quiet enough to have been a widener, the fold only
where saturation was already asked for, and a quiet spectrally-stretched
field recording under presets in the packs that are about places. The
built-in presets get the same rules, each sound-changing addition rendered
first and kept only if the preset still sounds like itself. Measured on a
sample before and after: level median 0.00 dB, worst +3.4 dB (which the
measurement pass then corrected), width shifts modest, mono loss unchanged,
no clipping, no silence, no click.

One more piece of the literature came in on the content side: **convolution
with a struck object**. The Room is a convolution reverb, and an impulse
response does not have to be a room. `make_impulses.py` gained a family cut
from the field recordings -- the sharpest event in a recording of a
foundry, a cistern, a hangar, shaped into a decaying impulse of a tenth to
half a second -- and a pad convolved with one is played on that object.
Measured on the room stem: five to fourteen decibels RMS different from a
hall across the third-octave bands, peaks of up to 27 dB where the object
rings. Forty of them, three megabytes, in the library and in the
thirty-second pack, *Cryo Chamber*, which was written for all of the above.

### After the classics: four things the newer literature asked for

The design chapters of the manual lean on the classic psychoacoustics, and a
check of where the field has moved produced four changes, all neutral at their
defaults and all measured:

* **Stretch** (Tuning): the octave a few cents wider than 2:1 -- Ward's octave
  enlargement, the Railsback curve -- as a slope about the reference pitch, so
  A4 stays and every octave away from it is `s` cents wider. Applied after
  Purity, retunes held notes through the same glide. Measured 1212.00 cents at
  twelve, twice that over two octaves.
* **Far reverb Mode = Scattering** (Schlecht and Habets 2020): a Schroeder
  all-pass with a mutually prime length inside every line's loop. Echo density
  after 50 ms 0.60 -> 0.76 of Gaussian (Abel and Huang's measure), decay
  unchanged, late-tail flatness 0.487 -> 0.501 -- so the honest claim is
  density, not colour, and the help text says so.
* **Unmask Spread**: the upward spread of masking. A band's effective
  side-chain envelope gains half of the band below, a quarter of the one two
  below and a tenth of the one above. Measured: a bass note in front ducks the
  background's middle to less than half of what it did without the spread.
* **Binaural = Headphones** (Space) with head tracking: pan becomes azimuth,
  Woodworth's ITD, full head shadow, a lower pinna notch behind the head, and
  the head's yaw -- from the Quest's OpenXR pose or an OSC `/ambient/head`
  tracker, through a new `OscSink::setHeadYaw` -- turns the field the other way.
  Measured: a centred voice with the head turned ninety degrees has the
  interaural lag of a hard-panned voice with the head straight (34 samples:
  Woodworth's 31.5 plus the shadow filter's group delay); off, bit-identical.

Two measurement lessons from the round. The reverb's coloration barely moved
because a modulated eight-line network is already nearly free of fixed modes;
the first test asserted a ten per cent gain in flatness and was wrong to. And
the unmask's one-pole crossovers leak: a 6 dB/octave split lets a ducked low
band show up in a middle-band measurement, so the surviving claims are
relative ones. The manual's design chapters were corrected in three places
the newer literature contradicts the classics -- loudness adaptation is small
(Scharf), the darkening with distance is a recording convention rather than
air absorption (ISO 9613 gives half a decibel over twenty metres), and bass
mono is a production rule rather than a perceptual limit -- and gained the
harmonicity account of consonance (McDermott et al. 2010, 2016; Harrison and
Pearce 2020) beside the roughness one.

### The three section layers

A layer is a preset bank that touches one section and nothing else, so it
lands on top of whatever sound is loaded. There are three; each lives in the
section it belongs to rather than on the header, which keeps only the Sound
box — the one preset that is about the whole instrument. All three are
generated and then *measured*:

* **Cosmos** — 257 presets in sixteen families (shift, beating, resonators,
  deep, vowels, nebula, shimmer, metallic, glass, drift, wide, ghost, choir,
  machine, bloom, edge). Each family is a designed grid: two parameters over
  four values each, chosen so every step is audible.
* **Z-plane** — one preset per filter shape, 156 with the off entry, grouped
  by the same twelve families as the shapes. A bank of 155 filters with no
  way in is a bank nobody uses.
* **Strike** — 41 presets for the Karplus-Strong pluck in four families
  (strings, wood, metal, and *conducted*, where the pluck fires from the
  conductor as well as from the keys, which turns it from something you play
  into something the piece does on its own).

`Tools/check_layer_presets.py` renders and measures every one of them, and
the three banks each needed their own way of being measured before the
measurement meant anything:

* The **Strike** bank first measured 30 presets as identical to the carrier,
  because the pluck fires on note-on and the offline render plays no keys.
  With notes added, 34 measured as identical to *each other*, because
  `--measure` analyses the second half of the render and a pluck of a few
  hundred milliseconds is long gone by then. Measured with the drone
  silenced and the conductor firing plucks throughout, all 41 are distinct.
* The **Z-plane** bank measured four extreme shapes as identical, because a
  filter can only be heard where the source has energy and a dark drone has
  none at 9 kHz — and because the sub, the body and the effects do not pass
  through the voice filter at all, so even at full wet they keep the drone
  in the measurement. On bare white noise, all 156 are distinct.
* The **Cosmos** bank came out with one dead preset out of 257, and that one
  was a real bug rather than a bad preset. *Smear* sets the Nebula's
  magnitude smoothing as `alpha = (1 - smear)^2`, which at Smear = 1 is
  exactly zero: the smoother then never updates, every bin stays at the zero
  it started from, and the Nebula falls silent. The top of that knob was not
  "the smoothest setting", it was "off", with nothing anywhere to say so.
  The default is 0.7 and no preset had ever sat on the end stop, so it had
  been there unnoticed since the section was written; a generated grid put
  one preset exactly on the corner and the render found it in a minute.
  `alpha` now has a floor of 2e-4, which at this hop rate is a time constant
  of minutes — which is what the top of a smear control should be.

The pattern is the same one the autoplay work ran into. A measurement that
does not put the thing being measured in front of the microphone will
happily report that everything is fine, or that everything is identical, and
both answers are worthless.

## The research round

Late in the instrument's life the design was checked against the current
literature rather than against the classic accounts it was built on. Nine
things were found. Eight were built; each is neutral at its default, so every
preset written before them renders to the bit as it did. The ninth, a neural
sound model in the audio path, was read and deliberately not built: it would
put a hundred megabytes of weights and a hard real-time constraint into a core
that is meant to run on a headset, and it would make the instrument's sound
something nobody could read. Every other decision here can be checked by
reading a formula.

* **Velvet noise** as a third Ensemble mode: sparse signed impulses instead of
  a chorus. The same width, measured, with 1.32 dB of colouration against the
  chorus's 5.41.
* **A colourless far reverb**: the same network with its eight line lengths
  searched by `Tools/optimise_fdn.py` rather than chosen. 0.314 dB of spectral
  spread in the tail against the classic set's 0.474.
* **The spherical head shadow** of Brown and Duda, in the binaural mode.
* **Critical-band spacing** in the conductor: it can be told to avoid or to
  seek notes that fall inside one critical band of a sounding one.
* **The Early Room**, a scattering delay network with one node per wall — the
  only stage in the instrument whose purpose is that the sound moves when the
  source does.
* **Bow**, a waveguide bowed string, the first physical model here.
* **Spectral**, a clip measured into 32 bands and rebuilt from them, which
  finally separates pitch from speed.
* **Loudness in sones** beside LUFS, and
  `Tools/library/texture_statistics.py`, which makes new texture from the
  statistics of old.

The long version, with the mathematics and the references, is the Design
chapters of the manual — and, more usefully, the traps: a masking slope that
made the loudness model four times too quiet while every relative test still
passed, a friction curve that was silent rather than wrong, a scattering loop
two samples long that destroyed the very thing it existed to carry, and a
pitch estimator that reported a clean A3 as 440 Hz.

## The harmony round

The same treatment was then given to the note generators, and the gaps there
turned out to be older and closer to home than the acoustic ones.

* **Harmonic.** The conductor scored a chord as the mean consonance over all
  its pairs. For two tones that is the whole question; for five it inverts the
  ranking. Measured on the instrument's own function, the pairwise rule prefers
  a stack of fifths (4:6:9, 0.233) to a just major triad (4:5:6, 0.212) and puts
  a plain segment of the harmonic series last of all (8:9:10:11:12, 0.165) —
  neighbouring members of one series make complicated ratios two at a time,
  however perfectly the set fits together. *Harmonic* asks instead how strongly
  the whole set implies one virtual root.
* **Key.** Krumhansl and Kessler's probe-tone profiles, correlated against a
  pitch-class distribution weighted by how long each class has been sounding.
  Nothing sets the key; it is found, and the correlation is both the confidence
  and the weight. Over three minutes the conductor's key confidence goes from
  0.73 to 0.90 with all nine pitch classes still in play.
* **Even** and **Smooth**, after Tymoczko. Evenness is the property that lets a
  chord move rather than leap; *Smooth* picks which voice moves so the chord
  travels least. His other measure needed nothing: for an exchange of one voice
  the voice-leading distance is exactly that voice's leap, which is the number
  the conductor had been using all along.
* **Timbre (Sethares)**, the thirteenth tuning: the instrument's own dissonance
  curve, swept across the octave while it plays, with a degree wherever it dips.

Four measurement traps, in the same spirit as the acoustic round's:

1. **A circular measure.** "How well do the notes fit the key?" — where the key
   is found from those very notes. It reported that *Key* made things worse.
   What the parameter claims is that the music sits more clearly in *one* key,
   which is the correlation itself.
2. **A window shorter than the thing measured.** Twenty-one seconds against a
   three-minute memory said the key confidence *fell*. Over three minutes it
   rises.
3. **A wrong reason nearly written into the code.** The fix for (2) was first
   attributed to a change that, remeasured, does nothing for the headline
   number. It was kept — root and key should know about each other — but the
   comment now says so plainly.
4. **A flag assigned over.** The timbre scale was rebuilt correctly and the flag
   that retunes the sounding notes was set, and three lines further down that
   flag was overwritten. The table test passed throughout; only a test that asks
   what a key actually *sounds at* catches it.

## The third pass

Four smaller ones, each measured: *Match* (Sethares' other direction — partials
bent onto the chosen scale; 12-TET's eleven intervals go from roughness 0.1263
to 0.1191), *Arc Harmony* (Lerdahl & Krumhansl's tension model as the arc's
lean on Harmonic, Key and Consonance), *Blend* (Rasch and Bregman — the first
five onsets span 6.29 s one by one, 0.020 s fused) and the fluctuation *Guard*
(Fastl & Zwicker — beat time in the 2–8 Hz band 22 % → 18 %, an honest four
points). One trap worth its own line: the matched partial in the voice is a
*branch*, not a blend, because `(f*h)*s` and `f*(h*s)` differ in the last bit
and the oracle would have heard it on every preset.

## The last acoustic pass

Three more, each measured. *Height* — the pinna's elevation notch (Hebrank &
Wright 1974) and Blauert's 8 kHz "above" band, one height per plane; the
10 kHz-to-7 kHz energy ratio goes 0.509 below, 0.227 flat, 0.028 overhead.
*Envelop* — Bradley & Soulodre's low-frequency lateral energy, lifted on the far
bus between Bass Mono's corner and 500 Hz; +3.0 dB side, 0.00 dB mid on the
output. *Depth Law* — Zahorik's compressive exponent inverted, d^1.85 at full;
half depth is heard at 0.277 instead of 0.500, the horizon unmoved. One trap:
both engine-level tests placed their note before the first `process()`, when
Keys Depth had not yet been read, and measured a note standing on the wrong
plane — nought decibels of lift and nought distance, from code that worked.

## The sixth round

A survey of the state of the art, written by a research model against the
v1.2.0 manual, proposed eleven things; nine were built (`Core/`, each with a
selftest), two declined (port-Hamiltonian rewrites of the physical models --
a passivity guarantee, not a sound; and a biosignal loop, which needs hardware).

*Cascade* -- the conductor's clock as a Hawkes process (time-rescaling: the
gap is drawn as before and consumed faster while the excitation is up;
branching 0.65 at full). Gaps' CV 0.91 -> 1.41, 2.8x the events. *Surprise* /
*Homeostat* -- a fading histogram of chosen interval classes, its Shannon
entropy held to a target by flattening or sharpening the draw; 3.52 -> 2.92
bits downwards. The untouched conductor already sits at 98 % of the maximum
twelve classes allow, so upwards there is almost nothing. *Adaptive* -- a new
note tuned pure against the sounding set (nearest of twelve ratios per voice,
weighted by simplicity, capped at 30 cents) and a common comma offset that
returns the ensemble's centre at 3 cents/min while every interval stays pure:
third 0.00 cents off 5:4, fifth 0.00 off 3:2, centre -3.9 -> -0.9 cents after a
minute. Found on the way: `brain_blend` (round 3) was never read into `bp_`;
its test drove `ClusterBrain` directly and passed. Wired, with a test through
the engine (2 -> 5 voices).

*Comodulate* -- one random envelope (a Drifter at 9 Hz, own RNG) on the far
bus: modulation index 0.15 -> 0.67, low/high envelope correlation 0.41 -> 0.98
(Hall, Haggard & Fernandes 1984). *Rotating* -- fourth far-reverb mode: fixed
lengths, eight Givens rotations (per-sample rotation recurrence, renormalised
every 4096) before the Householder; frozen it loses 0.4 dB more than the fixed
network over 4 s, i.e. nothing beyond the interpolated delays' own loss, and its
modes drift second to second as much as the wobbling lines' do (0.51 vs 0.55)
-- a match, not a win, and the manual says so. *Near Field* -- low shelf per
ear below 1 kHz (-18 dB far, +4 dB near) scaled by lateral x nearness^2; ILD
below 400 Hz -5 -> +10 dB with the 3-6 kHz shadow unchanged (Brungart &
Rabinowitz 1999).

*Transport* (slot field 32) -- the 1-D Wasserstein barycentre between wavetable
frames: masses walked by cumulative distribution, each slice put down at
(1-f) from + f to; halfway energy 0.50 -> 1.00, spread 5 partials -> 0.
*Pulse* -- raised-cosine AM of the Foundation at the Binaural rate (`sin01`
reads a table: the phase must be wrapped before the quarter-turn offset).
*Bias* -- the feedback shaper's input pushed off centre by the 5 Hz envelope of
its content below 60 Hz. *Lenia* -- 32x32 torus, ring kernel R=5, growth
2 exp(-(u-mu)^2/2 sigma^2)-1, dt 0.1, rows spread over blocks, stepped only
while a matrix route reads `lenia1..4`; 599 steps in 30 s at 20 Hz, one reseed,
readings never move more than 1 % per block.

Test traps of the round: an engine-level test that calls `noteOn` before the
first `process()` (Keys Depth unread, again); third-octave bands cannot see
modes move (0.92 vs 0.94 -- bin resolution can); Bass Mono folds the very lows a
near-field test is about; the engine sleeps without a voice, so a Foundation
test needs a silent note to stay awake; the feedback loop throttles itself off
above a mean level of 0.1, so a loop test must play quietly.

## The seventh round

A second survey, of instruments rather than papers (Osmose/EaganMatrix, SOMA
Terra, Waldorf Iridium, Madrona Sumu, Borderlands, Surge XT, Mutable Marbles,
RAVE, FluCoMa, Ambisonics). Six things built, each with a selftest; the rest
either already existed under another name (bandwidth-enhanced partials = the
Spectral source; MPE; Scala/KBM; gesture recording = Sets; a 2D surface = the
map; per-voice microfluctuation; `inertia`) or was declined (Lua in the audio
thread, a 16-channel HOA bus on a headphone instrument, corpus navigation as a
project of its own, and the neural/port-Hamiltonian proposals again).

*Deja Vu / Loop* -- Marbles' ring in the conductor (`viaDejaVu`): each choice
moves one place; with probability Deja Vu the note already there is replayed
and kept, else the fresh one overwrites it; a ring note still sounding is passed
over. Loop of four at full: a note equals the one four back 100 % of the time
(29 % without). *Spread / Bias* -- `BrainParams::shaped()` on every velocity
and hold draw: |2v|^k with k from 4 (gather) to 1/4 (push to the extremes),
then a power for bias; identity at the defaults, bit for bit. At full Spread
62 % of velocities sit more than 0.15 from the centre; Bias 0.8 moves the mean
0.70 -> 0.80. *lorenz_x/y/z, rossler_x/y/z* -- RK4 in natural time scaled by
`chaos_period`, substeps of 0.01, integrated only while a route reads one
(`chaosUsed_`), readings clamped to the attractors' extents. Over two minutes at
period 10 s: Lorenz best self-match at any 5-55 s lag 0.40, Roessler 0.95 --
the spiral is nearly a cycle, only its climb varies; documented as such.
*MPE Filter: One Euro* -- cutoff 0.6 + 30 |x - y| Hz per unit of range; the
speed is read from the distance left to travel because controllers send steps
and a derivative between messages is zero. *Transpose* -- a pure 4:3, 3:2 or
2:1 either way in `frequencyOf`, glided in log2 at an octave per two seconds,
`retune_` set while it moves; lands within 0.01 cent, the voice follows at its
own glide (8 s to within 3 cents). *Partial Spread* -- `phasorBankStepStereo`:
per-partial equal-power weights on a golden-angle pattern turning at 0.02 Hz;
a single strand's L/R correlation drops from ~0.96 to below 0.7 at unchanged
energy (+0.06 dB). Found on the way: Time Width at its default gives even a
centred single strand an interaural delay (render tool: width 0.88 vs 0.04).

## The voicing rules (12.09.2026)

Rene's *Regelwerk für einen selbstspielenden Noten- und Akkordgenerator*, built
as 31 parameters and three modulation sources, every one of them appended to
the table and neutral at its default, so a preset saved before them plays
exactly as it did. The document's own numbering is kept in the help texts.

*Register as a role* (`brain_layers`, `brain_bass_hold`, `brain_top_soft`,
`brain_low_spacing`, `brain_third_floor`, `brain_leading`). A conductor drawing
from one weighted list treats the bottom of its register like the top, and the
ear does not: the critical band is a fixed fraction of the frequency, so a
third is a chord at C4 and mud at C2. Layers turns the draw into a pyramid --
one voice in the foundation, most in the body, few above, at most two to an
octave and one below MIDI 36. Low Spacing is a **veto at 1**, not a penalty:
built as `w *= 1 - 0.95x` it still let five per cent through, which measured as
one basspair in ten closer than a fourth. Now zero of 567.

*Interval colour* (`brain_thirds`, `brain_seconds`, `brain_seventh`,
`brain_degree_swap`), judged against every sounding note rather than against
the root alone -- a tone that is a fifth to the bass and a second to a middle
voice *is* a second. Degree Swap exchanges one degree of the supply at a root
change, so the mode wanders instead of being switched.

*Time* (`brain_rate_breath` + `brain_breath_period`, `brain_overlap`,
`brain_onset_guard`, `brain_release_gap`, `brain_retrigger`,
`brain_density_slew`, `brain_silence` + `brain_silence_len`). The mean gap
breathes on two sines whose periods stand in the golden ratio, so the tide
never repeats and never becomes a pulse; measured over eight minutes the mean
gap moves 7.9 s -> 3.3 s where a plain clock moves 5.8 -> 4.8. The Onset Guard
is Rasch's and Bregman's thirty milliseconds: an event falling between 30 ms
and 3 s is postponed to the far side, never dropped, and the entrance after a
preset change is exempt or a cluster walking in at a third of a second would
never assemble. Density is walked one voice at a time (3 voices, not 8, half a
minute after 1 -> 8 with a two-minute slew). A planned silence is allowed only
at a root change, and the voices are released *staggered* -- all of them ending
together is the one ending this music must not have.

*Root and long form* (`brain_root_steps`, `brain_root_down`, `brain_pivot`,
`brain_home` + `brain_home_time`, `brain_memory`). The ascending semitone is
excluded outright by every step set but Any: 0 of 202 root moves climb one.
Pivot announces a change rather than declaring it -- a tone belonging to both
roots begins, the root follows at the middle of the window, the old root's
voice is released at its end. Memory forbids a *constellation* within its
minutes while single pitches may return as often as they like, which is Deja
Vu's business; a repeated chord is where a listener starts to hear a loop.

*Outside the conductor*: `brain2_golden` stretches the background layer's clock
by the one ratio with no good rational approximation; `tuning_hold_sounding`
keeps a sounding voice's frequency when the root moves, so the harmony changes
by what enters rather than by everything sliding; `beat_ceiling` bounds the
beat the strands make (`f (2^(c/1200) - 1)`, halved under 150 Hz) and
`strand_low_detune` thins the detuning towards the bottom, where a beat that is
warmth at 300 Hz is a wobble at 100; `env_vel_attack` reads velocity as an
attack time; `layer_depth` takes a note's plane from its role instead of the
dice. Three sources join the matrix: `root_age`, `layer` and `section` (the
Arc as five steps). The table asks for `layer` per voice, and that is not what
it is: the matrix is computed once a block for the whole instrument, so the
source carries the role of the loudest sounding voice, and a route from it
moves every voice together. Per-voice evaluation would be a different matrix.

Three traps, all found by measuring rather than by reading. **The parameter
table is indexed by position** (`paramTable()[int(id)]`) and nothing checked
that row *i* describes parameter *i* -- one row in the wrong place silently
turns the wrong knob for every parameter after it; now a selftest, and the same
for the modulation sources' names, which carry the text form of every route.
**Note-offs are emitted at five places** in the conductor, and a guard that
stamps only the main loop measures Retrigger and Release Gap wrong.
**`brain2_interval` was drawn as an amount**, 0.1 to 0.7, though the parameter
is semitones: the background conductor stood ten to seventy cents beside the
foreground's root -- a mistuned unison instead of a degree of its own, which is
the one thing it was there to be.

`Tools/library/conductor_ranges.json` (seven families, 56 artists, each
artist's ranges resolved against its family) now decides **what is played**,
while the style still decides **how it sounds**; `Tools/library/conductor.py`
draws from it and half of every pack gets the document's "astir" variant --
except where the artist's row pins a value, because "nothing wanders" is the
whole of Paul Bradley. Applying it once was not enough: the style pipeline
writes twenty-six of the same keys further down, so it is applied again, last,
minus the four the material is allowed to take back (`brain_low`, `brain_high`,
`scale`, `brain_on`). Checked over the generated library: 92 pinned values
kept, and 745490 drawn values all inside their artist's range.

## Every control, asked (12.09.2026)

Two sweeps in one day, one by machine and one by eye, after a preset was found
whose Stretch and Cloud could be turned to the stop without being heard.

*The machine sweep* (`Tools/library/module_sweep.py`). Every parameter set to
each end of its range in a context where it should be audible, rendered twice,
and the two renders compared by the hash the render tool prints: identical
bytes mean the control did nothing. 1772 questions, and the harness is most of
the work -- a source needs a clip, a conductor needs forty seconds and no held
note, a modulator needs a route and a target, a macro needs somewhere to point,
`brain_surprise` is multiplied by `brain_homeostat` and reads as dead when that
is zero. Three passes of fixing the harness took 250 "dead" controls to 155 to
59, and every one of those 59 was then reproduced by hand. Fifty-eight were the
harness or the physics -- `brain2_interval` at ±12 is an octave and consonance
is octave-invariant; a held note pins the root, so `brain_wander` cannot move
it; three routes at one target saturate past the filter's floor. One was real:

- **`lfoN_mode` was set by 9644 presets and read by nothing.** `Lfo::step` uses
  rate, phase, depth and table; the mode never reached it. Retrigger is built
  now -- the phase returns to the Phase knob when a note begins in silence,
  which is a phrase rather than every note of a cluster. Per Voice cannot be
  built honestly: the matrix is computed once a block for the whole instrument,
  not per voice, so there is no copy to hand out. The name stays so old presets
  load, and the help text says plainly that it behaves as Global and where
  per-voice movement actually comes from (strand drift, shimmer, rate wander).

Three more came out of the same week's listening and are measured, not argued:
`far_freeze=on` rendered bit-identical to `far_level=0` (930 presets had it, the
generator no longer writes it); the Cloud at its stop moved the sum by 0.6 dB
because its level was clamped at 2 and the parameter stopped at 1 (ceiling
raised, `cloud_to_near` added so it can be heard on the near plane at all); and
the balance measured a flat forty seconds while an eighth of the library has a
longer attack than that, which was 4.0 dB of error on the six presets it was
measured on (`--skip`, and the warm-up is now part of the fingerprint so
changing the rule re-measures exactly the presets whose warm-up moved).

*The reading* went through the source for algorithms that do something other
than what their name promises. Six were real, and all six are fixed:

- **The bowed string played sharp, unevenly.** Its two halves were whole
  numbers of samples, and `a + (len/2 - a)` is `floor(period/2)` whatever the
  bow position: the period was rounded to an EVEN number of samples. A3 sits on
  218 and was fine; C5 was +28 cents and C6 +63, which breaks intervals and not
  merely pitch. The bridge filter's own delay, `(1-g)/g` samples, was in the
  loop and never subtracted, so the pitch also moved with Bright -- 17 cents
  across the knob. Both halves are fractions now, read between two taps, and
  the filter's share is subtracted. Measured against the note the instrument
  itself asked for: +0.2, +0.8, +0.5 cents at A3, C5, C6, and 1.4 cents across
  Bright. 492 presets play a bowed slot, half of them above C5.
- **Partial Spread deleted partials off the centre.** The spread hands each
  partial a pair of weights -- an angle in the field -- and the pair was then
  multiplied by the strand's two pan gains, one side each. That is not a pan:
  a partial leaning away from it is attenuated by how far it leans, eleven
  decibels at the outer strand of a six-strand fan, and since the pattern turns
  once every fifty seconds they come and go. The two angles are composed now
  (which needs no trigonometry of its own -- the pair's sum and difference are
  the cosine and sine of the composition), stopped AT the ear rather than
  wrapped past it into an inverted partial, so every partial keeps its power
  wherever the pan puts it. Measured, one partial panned hard, over a quarter
  turn of the pattern: 0.03 dB of level swing. 2343 presets spread partials.
  The old test missed it for a good reason: it placed its one strand in the
  middle, and in the middle the two ways of combining agree exactly.
- **A slot's unison copies were placed the same way, and thinned the same way.**
  Reading the one above made the question worth asking of every stereo pair in
  the instrument, and the Harmonic and Wavetable types spread their copies
  across the field exactly as the bank spreads its partials -- with the same
  multiplication afterwards, against the code's own comment that "Pan then
  moves the whole group". A copy standing where the pan came from was
  attenuated by how far it stood there and, at the end of the knob, deleted:
  2048 slots in the library have a spread group and an off-centre pan, 526 of
  them near the stop. The Pan is composed into each copy's place now, in the
  same pan units and with the same clamp a voice uses for its strand fan, so a
  group panned hard is squeezed against that ear rather than halved. The test
  is the beating: two copies detuned twenty-five cents, spread to the ends,
  slot panned hard -- 5.4 dB of it, where a deleted copy leaves nothing to beat
  against. (The remaining pairs are fine by construction: a texture's grains
  and a noise slot's two channels carry the slot's Pan in their own place, and
  every other source type is mono until the slot places it.)
- **A struck string was cut off by the amplitude envelope.** The block loop ran
  `while (env.isActive())`, and the strike has its own physics and its own
  decay of up to 4.5 s; a short release, or a sustain of nought with a short
  decay, ended the loop mid-swing. The engine renders a voice while either is
  still going now. No preset in the library can reach it -- the shortest
  release is 6.0 s and sustain never falls below 0.7 -- but a hand-built
  staccato patch reaches it in one note.
- **A texture slot could fall silent for good.** A grain longer than the clip
  can give is shortened to fit, and what is left for Position to choose from is
  then very nearly nothing -- and exactly nothing when `(len-2)/rate` lands on
  a whole number, which it does at every power-of-two rate, so at every Free
  slot standing at its own speed. `span <= 0` then spawned no grain at all,
  forever, rather than the one grain the clip can hold. No library preset
  reaches it (the shortest clip is 13.5 s against a 1 s grain), a user with a
  short sample reaches it at once.
- **The body's modes jumped around the stereo field.** `Body::set` rebuilds
  whenever any of its five arguments moves, and it drew each mode's place from
  the random generator while it was there. Tone, Decay and the root all move,
  so an LFO on Tone re-drew all twelve places a hundred times a second. Drawn
  once now, in `prepare`. No pack preset uses the body; the GUI reaches it.

Two of the reported findings were **not** defects, and the arithmetic says so.
The frequency shifter lets its phase run to 10^6 before resetting it to zero,
which looks like a click every few hours; but the right ear runs at 0.97 of it
and 0.97·10^6 is a whole number too, so both ears cross with a discontinuity
smaller than a single sample's advance. And the Harmonic type is not a
time-domain wavetable player and does not claim to be: it reads a file's
*spectra* into a bank of 32 partials, which is what its name, the help text for
`srcN_table` and the manual's "wavetable as spectra" all say. Waveforms are the
`Wavetable` type, through `CycleTable`, added for exactly that distinction.

*A second reading, of the conductor against the rule book.* Eight findings;
five real, one of them larger than reported, two not defects, one a design
choice worth stating. Every fix below is measured in `testVoicingRules`.

- **The rules lived in one of the two draws.** Free mode weighs every
  candidate by the register roles, the spacing floor, the third floor, the
  leading note and the interval colours; `chooseNote()`, which scores the
  exchanges of Chords mode and the extra notes of a Blend, had none of them.
  No preset uses Chords mode, but 14335 have Blend above nought -- at a median
  of 0.03 it adds nothing, at the top decile it adds a note to every fill --
  and those notes arrived without any rule at all. The rules are one function
  now (`ruleWeight`), both draws consult it, and `chooseNote()` also honours
  Retrigger and Memory. Measured with Blend at one, so every note after the
  first takes that path: 0 close thirds on a bass note under the floor in
  10068 pairs, against 728 of 13208 with the floor off.
- **The third floor was one-sided and soft.** It fired when the *candidate*
  stood under the floor, so a C3 over a sounding A2 -- a minor third whose
  bass note stood under it -- passed; and it left two per cent, where a floor
  is a veto. It also counted pitch classes, so a tenth, the open voicing that
  avoids the mud, was forbidden along with the third. Now: the lower note of
  the pair, the close interval only, and nought. 0 of 10213 pairs in Free mode.
- **Blend widened past thirty milliseconds.** The window grew to fifty as Blend
  came down, and thirty to three thousand milliseconds is exactly what R5.3
  forbids; the Onset Guard cannot see it, those notes are timed after it runs.
  Twenty-nine milliseconds now, whatever Blend says. At five-millisecond ticks:
  139 onsets fused, 0 in the forbidden zone.
- **Two changeovers in three went unannounced.** The pivot tone had to be a
  root, fourth or fifth of both roots, and that exists only when the roots
  stand a second, a fourth or a fifth apart -- four of the six steps the root
  takes are thirds and sixths. A second tier admits the imperfect consonances,
  and the tone has to pass `ruleWeight` like any other. 291 of 291 root moves
  announced over half an hour, where the old set could manage two kinds in six.
  (The report's other two pivot claims do not hold: the tone *is* emitted, by
  `startIn`, and the old root *is* released by name -- the code keeps
  `pivotOld_` for exactly that, and the quoted `root_ % 12` is not in the file.)
- **Chords mode exchanged voices on one tick.** Note-off and note-on of an
  exchange fell together; Overlap (R5.4) lived in Free mode only. An exchanged
  voice now waits out Overlap in a holding list, still counted as sounding, and
  goes through the release gap like any other. 190 note-offs, every one ten
  seconds after the note-on that replaced it; the chord is one voice larger
  while it waits.
- **The ascending semitone was gated.** The veto stood inside `if (rootSteps
  != Any)`, so Any still climbed -- in a narrow register whose better
  candidates are out of range, 192 moves and it would. Anti 8 is unconditional
  and so is the veto now; no library preset uses Any (Fifths 7685, Diatonic
  4523, Falling 2128), so nothing in the library changes for it.
- **`layer` is not per voice**, as noted above, and the help text now says so.
- **`brain2_interval` stays in semitones.** The table asks for a choice of
  Root, Fifth, Fourth, Octave and Seventh; the parameter is -24..24 semitones,
  which contains all five and is what fourteen thousand presets already store
  (an enum would read their 7 as an index). The generator draws only the
  table's degrees.

*And one the reviews did not raise*, found while checking whether the
conductor's changes moved the balance: **the balance did not converge, and
could not tell.** A sample of 48 presets balanced a second time cut the bed of
15 of them by exactly the 4 dB a run may cut, and a third time cut them again.
"Bog Winter" explains it: its bed is the Foundation, already at its floor of
0.25; breath at its floor moved the bed by 0.00 dB; the voice -- three clip
sources -- stood at level 1.0 and 7.9 dB under the bed. Each run cut breath by
four decibels, changed nothing, and re-stamped the line as balanced. The tool
now renders the bed a third time, with every movable key at its floor, only
where a cut is due; what that render still contains is what the balance cannot
reach, and a preset the voice cannot be brought up to is written down as *am
Anschlag* rather than counted. Over the sample: 34 in balance, 0 with a cut
left to make, **9 at the limit, a median 6 dB under the target** -- one preset
in five, every one a clip-voiced or wavetable preset whose whole bed is the
sub at its floor. That is the generator's to answer (a quieter foundation
under a clip voice, or more level for the clip), not the balance's.
(`rebalance_voice.py` also ran `main()` at import, which a helper script found
out by starting a full balance of the library; guarded, with two siblings.)

## The rule book, measured (12.09.2026)

Reading the conductor against the rule book found where the rules were not
applied; it could not say whether what comes out of it *obeys* them. The rule
book says how to find out (its section 11): generate an hour and measure it.
So `Tools/render/brain_audit.cpp` steps the engine's own conductors for an hour
with the parameters exactly as a render reads them -- no audio, a minute of
wall clock -- and `Tools/library/brain_audit.py` turns the events into the
table of section 11, the fifteen anti-rules and the roles of table 2.
`--rulebook` sets section 14's parameters to the values the document names;
`--seeds 8` runs eight hours, because one hour is one throw of the dice;
`--sweep 200` asks the same of every two-hundredth preset in the library.

The first hour kept 19 of 30 checks. What the other eleven were:

| Broken | Why | Now |
|---|---|---|
| 115 s of silence, 16 changes landing on nothing | the LAST voice went when its hold ran out, Overlap or no | it stays until its replacement is in (G3) |
| 8 close pairs in the bass, 9 low tritones, 195 s with three notes in an octave | the spacing rules judged the CANDIDATE's register, not the pair's lower note; R2.2 was a 10 % penalty; R3.2 had no rule at all | all three on the lower note, all three vetoes |
| a leading note under a sounding root | the rule fired only when the root already sounded | at 1 the leading note is out either way; the pivot tone is the rule's own exception |
| a pitch back inside its 30 s rest | Deja Vu offered a past note and no rule was asked of the offer | the ring's offer passes the same tests as a draw |
| 51 chord changes under 20 s | the exponential clock draws short gaps, and nothing stopped them | a floor of 20 s (or half the rate), as a guard and as a shifted draw -- clipping it made a pulse AT the floor |
| a four-note chord landing on an empty room | Blend filled a chord that was not there yet | the entrance is one voice (R2.3); Blend fills what joins a chord already sounding |
| 10 constellations back inside ten minutes | the memory held 48 entries for an hour's 100-200, stamped them at birth rather than at their end, counted a doubling as a new chord, and never saw the voice sounding out its overlap | 256 entries, stamped whenever one ends, doublings excepted, the leaving voice counted |
| the hour ending a step away from home | Home pulled the root CLOSER but the target interval still decided; and once home, it left again | ripe, home outranks the interval and holds |
| 16 root changes an hour, or none at all | the root moved even when the draw found no note; Home, Key and Root Down together could put every candidate above the threshold | the root moves only when a note is actually chosen, not inside four minutes (R6.1), and past twelve whatever it scores |
| Chords: note-offs on the tick of the note-on, 10-13 voices, a pulse at the rate | Overlap lived in Free mode; nothing shed the pivot tone's slot; the clock was a fixed interval | the overlap list, a shedding exchange, and Free mode's own clock |
| Chords: thirds and seconds under a voice that was still sounding | the voice being exchanged was taken out of its slot for the vote and the rules stopped hearing it | it is out of the vote and still in the rules |

Four more came out of measuring what was left, and each of them was a rule that
was implemented and still not kept:

- **A constellation that ends by an ARRIVAL was never stamped at its end.** The
  memory recorded a set when it was created and again when a voice left it, but
  a set that stops because a note joins it was remembered only from its birth:
  a chord sounding until minute 48 was on record as of minute 42 and came back
  at 57, nine minutes after it was last heard. Every path that changes the
  sounding set now stamps the set it is ending. 10 returns an hour at the start
  of the day, 1 in three hours of eight now.
- **The way home was undone by R6.1's own release.** Past twelve minutes the
  best root goes through whatever it scores -- which, once the music had come
  home late in the hour, let it wander off again: one hour in eight ended a step
  away, having been home at minute 33. The release does not apply at home when
  the night is ripe. Eight hours of eight end at home now, in both modes.
- **The major seventh was a tenth of everything played**, where section 4.2's
  table has no line for it at all. It is harmonicity that puts it there -- the
  fifteenth harmonic is a major seventh -- so this is two of the document's own
  instructions disagreeing rather than a fault; but Seconds is the knob for the
  minor second, and a major seventh is a minor second turned upside down, so it
  avoids that too now (and the major second at a third of the amount, which is
  the proportion the table has between them). 0.10 to 0.03.
- **A root change could make a sounding note into its own leading note**, which
  breaks Anti 6 the moment it arrives and cannot be repaired afterwards, since
  R4.7 forbids retuning what already sounds. The root is now chosen with that
  in mind, which is the only place where there is still a choice.

Eight hours in each mode, measured on the finished conductor: **207 of 216
checks kept in Free, 209 of 216 in Chords.** What is left is stated rather than
claimed, and no rule is broken in every hour:

- the **interval histogram** sits at a total variation of 0.20 (Free) and 0.14
  (Chords) against section 4.2's weights where the rule asks for under 0.2, and
  four hours of eight in Free are a little over. What is heavy is the major
  third and the minor sixth -- 5:4 and 8:5, the intervals `brain_harmonic` is
  built to find. Section 9 asks for Harmonic high and section 4.2 weights those
  two at 0.08 and 0.05: the two cannot both be had, and this is where the
  instrument sits between them.
- **one pitch class over a quarter of the sounding time** in one hour of eight,
  and **two root changes** in one or two hours of eight where section 11 wants
  three. Both are the way home holding the root still through the last third of
  the hour, which is R6.4 doing exactly what it says.
- **one constellation returns** inside its ten minutes in two or three hours of
  eight, always a three-note set in a register the vetoes have made narrow.

The library's own presets are a different matter again: they are drawn from the
artist ranges, and a Paul Bradley whose root never moves is not breaking R6.1,
he is P5. The sweep is there to be read that way.

## The foreground (12.09.2026, late)

A third reading of the day asked not what was broken but why a soundscape can
come out flat, and answered it well: depth is contrast, a dry near reference
against a dark far plane, and the instrument has every tool for it. What it
could not know was whether the *library* uses them, so that was counted:

| Cue | The reading asks | The library had |
|---|---|---|
| `far_unmask` | 0.3–0.5 | nought in all 14336 presets -- the generator never wrote the key |
| `presence` | 2.5–4.5 dB | nought in 79 %; written with a 6–22 % chance |
| `far_highcut` | 1.8–2.5 kHz | median 3.6 kHz, half the library's tail inside the presence bell's band |
| `layer_depth`, `near_mix`, `bass_mono`, the scales | -- | already where it wants them |

The two empty rows are the same defect the balance had measured that afternoon
from the other side: one preset in five with its voice under a bed that is the
sub at its floor, and the tool for exactly that -- the background stepping
aside under the voice, band by band -- lying unused. `conductor.depth_cues()`
now writes all three, by family (the dark and the cold profiles want nothing
near; P1, P6 and P7 want it most), from a stream seeded by the preset's name so
nothing else in a preset moves, and `add_depth_cues.py` wrote the same values
into the existing packs, to the byte a regeneration would give. Two of the
reading's numbers were refused: `pad_low_cut` at 120–180 Hz would take the
fundamental from the whole body register (the concept's own measurement: a
300 Hz cut costs a C3 80 % of it), and "purity 0.98, beat-free" contradicts
P1, whose Rich wants the beats -- the generator's 0.76 with drift is the rule
book's side, not the reading's.

## Presets

`Core/src/Presets.cpp`: a preset is a name and a `key=value;…` string over the
parameter table (choices by name). The engine, the render tool (`--preset`)
and the plugin's program list all use the same table. 158 presets in thirteen
families; all render finite with a held chord, levels −29 … −11 dBFS.

The granular family (148..157) sets the Texture slot up and keeps the partial
bank sounding, so the preset makes sense before a clip is loaded and gains its
granular layer the moment one is. In the grain-forward ones the layer carries
the sound: measured against the same preset without a clip, it sits 0.9 dB
below the whole in *Grain Swarm* and 1.5 dB in *Pulverised*.
User presets are saved by the plugin as `.noctuary` XML files (full
state including a loaded Scala scale).

### Preset packs

`Core/src/PresetPacks.cpp`: a pack is a UTF-8 text file (`.ambientpack`) read
at start, so a library of thousands does not sit in the binary. One preset per
line, `name|settings|x y bright motion width noisy bass density tags|texture|
wavetable`; everything after the settings is optional, and the two file fields
are resolved against the pack's own folder. Packs come from `$AMBIENT_PACKS`
(one folder, or several separated by `;`), otherwise from
`Documents/Noctuary/Packs`, and on the Quest from `<externalDataPath>/Packs`.

The list the rest of the program sees is `builtinPresetCount()` compiled-in
presets followed by every loaded pack, and `numPresets`, `preset`, `presetMeta`,
`numPresetFamilies` and `presetFamilyName` dispatch across both, so the DAW
programs, the browser, the map and routes-by-name pick packs up without knowing
they exist. Each pack becomes one family after the built-in ones.
`PresetMap::warmup` rebuilds when the count changes; the host loads a pack
preset's own sample and wavetable when it applies it, through
`presetFilePath(index, 0|1)`.

`Library/` holds a generated library of 14336 presets in 56 packs, with 8686
clips (5748 tonal in Textures, 2938 environments in FieldRecordings), 2191
wavetables on three shelves and 1100 impulse responses (see
`Library/README.md` and `Tools/library/`). Its descriptors and map positions
are measured, not estimated: `Tools/library/measure_packs.py` renders every one
of them for a minute and writes the result back into the pack files, and
corrects each preset's master gain to the loudness it actually came out at. The
256 compiled-in presets are generated from the same styles and go through the
same mill, as packs in a staging folder, before they are compiled in
(`Tools/library/write_builtins.py`).

**The library rebuilt from what the material knows (2.0).** The clips were
generated from prompt lists, and every line of those lists says more than a file
name can: the clip's category, its gesture, its harmony, its world, the source
types it was written for -- and the artists it was written for. Until 2.0 the
generator matched clips to styles by ear-tuned affinity over file names; now
`Tools/library/clip_catalog.py` joins the prompt lists, the wavetable and impulse
sidecars and the measurements in `Library/tonality.json` into one table, and
`Tools/library/artists.py` names, for each of 56 artists, the clips it owns, the
neighbours it borrows from at a weight, and the categories, worlds and gestures
that lean the draw. A style with no clips of its own is seeded by its categories.
Health is part of the join: a clip with an offset above five per cent of its
level, a hole longer than 1.5 s, or -- for a loop -- a jump at the seam above
6 dB never reaches a preset (358 of 8686 are kept out that way).

Four rules follow the material rather than the style. The three types that read a
clip take only clips written for them. A table plays as the type its shelf says
(Harmonic, classic Wavetable, or either for the ambient shelf), a table without a
fundamental never carries the first slot, a sung vowel keeps the conductor inside
the range it was sung in, and a table measured from a recording gets a floor
under its fundamental. The key follows the voice's clip: its note becomes the
root, its mode picks the scale where the style has that scale (F# Dorian is E
major's notes, so it plays as JI Major on E), and a clip that follows the note is
played at the octave nearest the one it was recorded in -- a transposition of two
octaves is an artefact, not a performance. And a designed room's partner is its
Room Morph B, so the morph never dips.

Three traps on the way, all silent by nature. `measure_packs.py` read the pack's
own `format 2` line as a preset and would have written it back as
`format 2||||||||` -- and the loader only counts a format line without a `|`, so
every pack would have fallen back to format 1 and read its Harmonic sources as
classic wavetables. The renderer's batch mode inserted `--preset` at the
front of the command line, ahead of `--packs`: the arguments are read in order,
so every pack preset came back as unknown. It had never shown because the callers
also set `AMBIENT_PACKS`; the insert now goes behind `--packs`.

And the third was not in the library at all. A crossfade hands the leaving
conductor's cluster to the arriving one, and the plugin read it into
`int notes[ambient::kSlots]` -- four, the number of source slots -- while
`ClusterBrain::soundingNotes` writes up to `ClusterBrain::kSlots`, which is
twelve. Two constants of the same name, one scope apart. A conductor holding
more than four notes therefore wrote eight ints and eight floats past the end of
two stack arrays, and `/GS` answered with `__report_gsfailure` and `int 29h`:
the process dies on the spot, so no crash handler runs, Windows writes no dump
and no event-log entry, and the exit looks clean from outside. That is exactly
the vanishing act the crash log in `PluginProcessor.cpp` had been written for an
earlier round, and the log could never catch it. It needs a preset whose
conductor is holding a full chord, which is why the transition test -- which
changes preset a fifth of a second after the first one, when the conductor holds
one note -- had never seen it in hundreds of runs. The host test now waits for
five voices before it changes.

**Where the noise was (2.0).** The first library was heard as "nearly all
presets sound extremely noisy", and the way to an answer was not to listen harder
but to feed the instrument something that cannot be noisy: a pure sine, as a
single-cycle wavetable and as a clip, through every source type with every other
block switched off. All six types came back transparent -- Additive, Harmonic,
the classic Wavetable, Texture's grains, Stretch's Paulstretch and Spectral's
resynthesis all measured a spectral flatness of 0.000001 to 0.000004 against
white noise's 1.0. Then the blocks were switched back on one at a time:

    Air 0.2                                          0.0149
    Air 0.4                                          0.0257
    a Noise slot at 0.2                              0.0055
    Spectral with Breath +0.8                        0.0027
    Patina 0.5 with Hiss 0.5                         0.0019
    Patina 0.5                                       0.0008
    the Foundation at 0.35, Feedback with Tape       0.00003
    everything else                                  0.000007 or below

"Everything else" is the far reverb, the near reverb, the Cloud with scatter,
swarm, resonators and feedback, all four Cosmos characters, the Memory with blur,
drive and age, both delays, every filter model, the wavefolder, the ensemble,
coherence, blur, the Strike, the z-plane, shimmer, inharmonicity, purity drift,
grains from 60 to 400 ms, a stretch factor of 300 and a frozen spectral read
head. One knob accounted for the whole impression, and the library measured at
0.012 -- exactly Air at its median of 0.2.

The reason it was in every preset was not the styles but the ground they stand
on: `styles.BASE` carried `air: (0.05, 0.3)`, so a style that never mentioned air
still drew one, and `air`'s own default of 0.15 means a preset has to write a
zero to be without it. Air is the exception now (73 % of presets at zero, the
rest under a tenth unless the style is about air), Breath's hiss is capped at a
fifth, and the spectral Breath leans tonal.

**The conductor's judgement, put to use (2.0).** The instrument had grown a great
deal of musical judgement that almost nothing used. The chord-level harmonicity
(Harrison and Pearce 2020) was set in 8 % of the generated presets and in none of
the 195 hand-written built-ins; the key profiles (Krumhansl and Kessler 1982) in
23 %; the critical-band spacing (Glasberg and Moore 1990) in 23 % and never
negative, although negative is the setting that *seeks* the crowding a cluster is
made of; the second conductor, which is what makes the background plane a plane
of its own, was rare. Measured on nine presets with the clock run fast so that
the conductor actually decides, switching the chord harmonicity on halves the
roughness of what it plays (median 0.0058 against 0.0026, better in five cases of
nine, worse in two). So the weights are used now: harmonicity in 60 % of presets,
spacing in 43 % with a fifth of the library seeking clusters rather than avoiding
them, the key in 42 %, evenness and step size in 38 %, a second conductor in 33 %.

Two of them changed in the core rather than in the generator. *Smooth* was the
voice-leading distance of an exchange and did nothing at all in Free mode -- the
mode nearly every preset uses -- although the same idea is what a line is made
of; it now weights a candidate by how far it stands from the note chosen before
it, so the conductor steps oftener than it leaps, and a leap stays possible. And
the two conductors' clocks are no longer drawn independently: the background's
period is a crooked multiple of the foreground's (2.3, 3.7, 5.3 or 7.1 times), so
the state the two of them share comes round after hours instead of after a few
minutes. Drawn on their own the two rates landed within a factor of two often
enough that the planes were heard as one.

**The impulse shelf, rebuilt (1.11.0).** The same question as the wavetables, asked about the
Room, and the same answer: 240 impulses were eight families, and 238 of them were used, so the
coverage was never the problem -- the number of ideas was. Six families were added, chosen as
shapes a designed room cannot make because a room that dark or that empty would be a broken room:
**diffusion** (velvet noise -- taps at random times, all the same size, random sign: what a
diffusion network converges to, and the smoothest tail there is, no comb colour and no grain),
**echoes** (a handful of separate reflections and almost nothing between them -- a stone circle, a
cliff; what makes it a place is that you can count them), **tube** (one resonance and the air
around it: the Room as a body with a pitch rather than a space with a size), **underwater** (four
poles of low-pass and a very long tail), **chord** (several combs at once in just intonation: a
room that answers with a chord whatever you play into it) and **sheet** (a steel plate: dozens of
inharmonic modes decaying at different rates, so the metal changes colour while it rings).

And the same silent narrowing as the styles' dead recipe name. The `struck` family finds its
sources by matching a file name, and the rule wanted the prefix `field_recordings_` -- which only
the clips generated per style ever carried. Everything made from the prompt lists was invisible to
it, so the shelf had quietly stopped growing while thousands of struck objects piled up next to
it. Sources are now chosen by material (bell, metal, glass, stone, wood) across both clip folders,
and there are 160 rather than 40. A second cross-synthesis came with it: **space** takes not a
recording's sharpest event but its steadiest window -- the least movement in the log of its
envelope -- and shapes that into a tail, so a minute of rain or of a ventilation shaft becomes a
room with that texture in its walls, irregular in a way no designed hall is. 546 impulses in
sixteen families, 274 of them cut from real recordings.

Two layers, loadable independently and combinable (`PresetScope`):
*Sound* = every parameter outside the Cosmos section, *Cosmos* = the Cosmos
section. Applying a preset in one scope resets only that scope's parameters
to their defaults and then to the preset's values; the other layer is
untouched. The 148 full presets serve as the Sound bank (their sound layer)
and as DAW programs (both layers); a separate 32-entry Cosmos bank ("Cosmos
Off", shifters, resonators, vowels, nebulae, shimmers, the sci-fi
combinations) serves the Cosmos box. All 160 render finite.

### Texture slot (granular)

Not a sample player: grains are spawned at exponentially distributed intervals around *Density*,
each with its own start point, playback rate, Hann window and pan, and they overlap freely.
*Grains* (1..128) is the ceiling on how many a slot may have sounding at once; *Spread* scatters
the start point around *Position*, from a 0.1 % window up to the whole clip. The window runs as
a rotating phasor rather than a `std::cos` per sample -- at 64 grains that call was the most
expensive thing in the voice.

Two things were wrong here and are worth writing down. A clip enters at its own level, while the
wavetable and FM slots normalise themselves to unity, so `Texture::measure()` now sets a reading
gain from the clip's RMS. And the overlap normalisation was `0.7/sqrt(N)`: a Hann-windowed stream
at overlap N has RMS `sqrt(N)*0.612*source`, so the constant that returns the source's own level
is `1/0.612 = 1.63`. Together those two were 22 dB: at the same *Level*, a Texture slot was
inaudible next to a Wavetable slot. All three source types now land within 0.8 dB of each other.

Cost, measured: eight voices with both slots granular at 60 grains/s and 800 ms grains --
1024 concurrent grains -- render at 6.5x realtime with *Grains* at 64, 8.6x at 32 and 16x at 8.

**The overlap is the thing, and it was never drawn.** Rene: *"der Granular-Oszillator erzeugt
bislang eine relativ geringe Anzahl an Grains, dadurch klingen die Wolken nicht wirklich luftig
und dicht, sondern eher dünn."* What decides whether a cloud is heard as a cloud is not the grain
rate but *Density* times *Grain* -- how many are sounding at once -- and the generator drew those
two independently, which left the overlap as a by-product. Measured over 7858 texture slots: median
overlap **2.37**, 44 % of them under two, 68 % under four. At two the ear counts the grains. The
ceiling was reached by **45 slots of 7858**, so the thinness was never the engine running out of
them; the presets were asking for a rattle. The generator now draws the overlap (4 .. 30,
log-uniform) and computes the density from it: median 10.9, none under two.

*Density* also had a ceiling that made short grains structurally sparse. At 60 a second, a
60 ms grain cannot exceed an overlap of 3.6 however far the knob is turned -- the arithmetic
forbids it. It now reaches 200 a second, and the slot holds 128 grains rather than 64 so that the
top of that range means something.

Whether the loop needs SoA and hand-written AVX was asked and measured rather than assumed: over a
whole preset, **eight grains against sixty-four is two percent of the render**. The arithmetic of
the instrument is the phasor bank (`Simd.h`), which is already vectorised by hand; the grain loop
is small beside it, and the reverbs are the cost. It remains worth doing as an efficiency measure,
and the right cut is across SAMPLES within one grain (lane *k* = sample *i+k*, the eight window
phasors seeded at r^0..r^7 and turned by r^8 each pass) rather than across grains: that way the
accumulation into the output is a plain vector add instead of a horizontal reduction per sample,
and a grain's lifetime stays scalar. The one real obstacle is that `Grain::pos` is a `double`
walking through a clip of millions of samples -- in `float` a 96000-sample clip leaves 1/128 of a
sample of resolution, which is audible pitch jitter -- so a gather needs the position split into an
integer index and a fraction.

**Against the field**, since the question is fair: Tasty Chips' GR-1, a dedicated granular
instrument, advertises *"128 grains per voice, which can add up to a total of 1000+ grains
simultaneously"*; Waldorf's Iridium runs 1 to 8 grains per voice in Particle mode; Omnisphere
documents *"up to eight voices of granularity per Layer"*; Pigments and Novum have a ceiling but do
not publish it. Noctuary is at 128 per slot, four slots per voice, sixteen voices. The GR-1's
number is the interesting one: the instrument that does nothing else picked 128 per voice as the
figure worth printing, which is the same one this arrived at from the other direction.

### Noise slot

The fourth source type. Ten colours: the textbook slopes (white, pink, brown, blue, violet) plus
grey (white with the ear's most sensitive region taken out, so it *sounds* flat rather than
measuring flat), a resonant Band that can track the played note, Wind (the same band with a
wandering centre and less damping), Crackle (sparse decaying impulses -- vinyl, embers, rain) and
Digital (sample-and-hold white). Position sets the band centre or the colour, Pos Drift lets it
wander, Density is the crackle rate or the hold rate, Noise Q the width.

Two things were measured rather than assumed. The three-pole short form of Kellet's pink filter
is 1.7 dB per octave too steep, so the full seven-term version is used: measured slopes are white
-0.1, pink -3.1, brown -6.0, blue +2.8, violet +5.5 dB per octave. And every colour is
level-matched against a Wavetable slot at the same Level (-22.0 dBFS with the voice filter open);
before that a violet slot sat eleven decibels above a pink one at the same setting.

### GrainCloud

History ring of 4 s fed from the near bus × *Send*. Grains are spawned at
exponentially distributed intervals around *Density*; each has a Hann window
of *Grain* × (0.7 … 1.3), a start point up to *Spray* seconds back, a random
pan, and a playback rate of 1 ± 2 % or, with probability *Pitch*, ×2, ×½
(70 %) or ×1.5, ×4 (30 %). Up to 32 grains overlap; the output gain is
normalised by √(density · grain length) so density does not change loudness.
The cloud is added to the far bus only: it exists in the background and the
far reverb smears it.

## Preset browser and the preset map

Every built-in preset is measured, not described: `Tools/preset_map.py`
renders each one for 12 s (held chord plus a busy brain) and takes the
spectral centroid, spectral flatness, spectral flux, stereo width, energy
below 150 Hz and the mean voice count from the last 8 s, plus flags read
from the parameters (keys or generative, cosmos, feedback, sources, just
intonation, sub, stack, air). The result is generated into
`Core/src/PresetMeta.cpp`: per preset a rank 0..1 for brightness, motion,
width, noisiness, bass and density, a family index (from the comment
blocks in `Presets.cpp`), tag bits (quantiles of the ranks plus the flags)
and a position in a plane — the first two principal components of the
standardised descriptors, with the flags weighted in so cosmos, feedback
and source presets form their own clusters, followed by a short repulsion
pass so no two points overlap. The stub tool mode writes an empty table so
the core compiles before the first measurement; the self test accepts the
stub and checks the measured table.

**The map as a cloud (1.11.0).** Rene sent a description of Absynth 6's sound browser -- a free
point cloud, no grid, dense where the sounds are alike, an axis of brightness across and one of
"static to evolving" up, and macro sliders that condense the field -- and asked whether that was
something for us, with other descriptors, "da wir ja praktisch nur Drones haben". It was, and the
diagnosis fitted exactly what our map got wrong. Four things came of it.

*The descriptors.* Absynth's vertical axis is attack: plucks and keys below, evolving pads above.
Every preset here is a pad, so that axis is empty for us. Three measurements were added to
`--measure` in their place, all of them things a drone can differ in. **Evolution**: the render is
cut into seconds and the spread of the per-second centroid (in octaves) and of the per-second
level (in decibels) is taken -- how far the sound travels over a minute, which flux cannot say
because a fast tremolo has flux and goes nowhere. **Roughness**: the peaks of the average spectrum
put through the Plomp-Levelt curve (`ambient::peakRoughness`, the same curve the conductor judges
its chords with, so there is one implementation and not two) -- smooth and fused against beating
and grinding. **Wetness**: the four buses the renderer already carries, far plus room plus cosmos
against the near plane -- the spatial model's own axis. The measurement runs a minute per preset
now rather than twelve seconds, which is what an evolution figure needs; the whole library takes
about four hours at four renders in parallel, cached to JSON so the layout can be re-run in
seconds afterwards.

*The cloud.* `Tools/library/mapembed.py`: a k-nearest-neighbour graph in the descriptor space laid
out with springs -- neighbours attract, a grid-approximated repulsion pushes everything apart --
then the whole cloud is rotated and flipped to whichever orientation correlates best with
brightness across and evolution up, because a cloud you cannot orient yourself in is a worse
browser than a grid. The old layout's rank-flattening and its de-collision grid are gone; both
existed to spread the library evenly, which is precisely what destroyed the information. Measured
on a synthetic library of five blobs, the share of each preset's ten nearest neighbours that
survive as one of its twenty nearest on the plane: rank grid 0.065, plain PCA 0.08, springs 0.16.

*One map instead of two.* `Tools/preset_map.py` embedded the built-ins and
`Tools/library/measure_packs.py` the pack presets, each standardising its own descriptors,
and the browser drew both on one square -- so a built-in and a pack preset at the same spot had
nothing to do with each other. `Tools/library/map_all.py` now ranks and lays out the union and
writes all three products: `Core/src/PresetMeta.cpp`, the packs' meta fields, and
`Core/src/PresetClusters.inc`.

*Groups, and the sliders.* k-means over the nine descriptors; each group is named after the two
descriptors furthest from the library's middle ("Dark Smooth", "Wide Far"). Only the centroids are
generated -- `presetClusterOf()` decides a preset's group in the core from its own descriptors, so
a built-in and a pack preset are grouped by the same yardstick although two tools measured them.
The map colours by group with a switch back to the pack families, and four two-value sliders
(dark-bright, still-moving, smooth-rough, near-far) narrow the cloud to a range of each descriptor;
the view then closes in on what survived, which is Absynth's "die Punktewolke verdichtet sich"
from the other side -- our points cannot move, since where they are is what they mean. Six tags
came with them (Still, Evolving, Smooth, Rough, Near, Far), so the Columns view filters on the
same things.

**What it is like, what it is, what it sounds like (1.11.0).** The nine descriptors have a blind
spot that no tenth descriptor of the same kind would close. Two presets can agree on brightness,
motion, width, noisiness, bass, density, evolution, roughness and wetness and still be, to an ear,
a goods yard and a beehive. Rene put it back to us after reading that sentence in a report: "Können
wir das nicht irgendwie erfassen?" Two more layers were added, and they are kept apart on purpose,
because they know different things.

*The fingerprint.* `--measure` prints a second line now, `timbre:` -- sixteen mel-cepstral means
and their sixteen spreads over the render. That is not a property anybody can name; it is the shape
of the spectrum itself, and it separates the yard from the hive when every named property agrees.
It costs nothing: the render was already being analysed.

*What a model hears.* `--tap` writes a twelve-second mono excerpt beside the numbers, and
`Tools/library/clap_embed.py` puts every excerpt through CLAP (`laion/clap-htsat-unfused`), which
places audio and language in one space. Out of it come a 512-number embedding and, scored against
a written vocabulary of ninety-two phrases in six groups, what the library sounds like in words.
CLAP cannot write a sentence, only choose one that a person wrote, which is the honest half of the
bargain: every line the browser shows can be traced to a phrase in `clap_embed.py` and a distance
a model measured.

Choosing the phrase is where this goes wrong twice. Ranked by raw score, four thousand drones are
told they are "smooth and fused, one single body of sound", because CLAP is quite right that a
drone is a drone. Ranked by the lift over the library instead -- how far above the library's mean
this preset scores for that phrase -- a soft pad is called a bronze gong for being a hair more
gong-like than average. So both are used: a phrase must be plausible for the preset at all (its raw
score in the top quarter of the vocabulary) and is then chosen from those by the lift, and the
second phrase must come from another group, so the line says two things rather than one thing
twice. It appears in the info panel under SOUNDS LIKE, kept apart from the sentence above it: the
sentence is written from the settings and knows what is switched on, the phrase knows only the
sound.

*What the layout uses.* All three: the nine descriptors standardised, six principal components of
the fingerprint, six of the CLAP embedding, weighted 9 : 6 : 6 so the descriptors still dominate
and the orientation promise (dark to bright across, still to evolving up) survives. `mapembed.embed`
takes that space as its `graph` argument while the two named descriptors still decide which way
round the finished cloud is turned. The groups are formed in the same space -- which means they
can no longer be recomputed from the nine numbers, so the group index is stored in `PresetMeta`
and `presetClusterOf()` returns it, falling back to the nearest centroid for anything that has
none. `Tools/library/rebuild_all.py` runs the seven steps in order, since the order matters: the
excerpts have to exist before CLAP listens, and the built-ins' excerpts are written by a `--dry-run`
pass of `map_all.py` before the real one.

One thing had to be repaired on the way, and the self test found it: `PresetMap::neighbours` was
given a radius that follows the local density, because a cloud has gaps where a fixed radius
collapses the blend onto whichever point is nearest. That broke the guarantee that the cursor
standing on a preset's own point plays that preset and nothing else. The radius now grows only
when the nearest point is already further away than the radius itself -- when the cursor really is
in the empty space -- and not one pixel sooner.

The **Browse** page of the plugin has two views. **Columns** is the
classic browser (Omnisphere / Absynth style): Family | Character (dark,
bright, tonal, noisy, wide, bass) | Motion (calm, moving, dense, sparse) |
Features (keys, generative, cosmos, feedback, sources, just intonation,
sub, stack, air), each row with the number of presets it leaves; rows in
one column combine with OR (Ctrl-click), columns with AND, "All" clears;
below, the result list with family and descriptor bars. **Map** keeps the
list with tag toggles and shows the plane. Both share text search, sort by
any descriptor, favourites (a star per row, an "only favourites" filter,
kept in the plugin state), a click loads, → A / → B fill the morph slots.
The **map**: points coloured by family,
sized by density, filtered points bright and the rest dimmed, a hover
shows name, family and tags, a click loads. With *Map blend* on, the
cursor (MapX / MapY) is dragged across the plane and the engine plays the
blend of the presets around it: `PresetMap::neighbours` takes the six
nearest points with Gaussian weights of the distance (*Radius* = sigma,
default 0.08 of the plane), `PresetMap::blend` mixes floats in the skew
domain, rounds ints and takes choices and switches from the strongest
neighbour, and `Engine::updateBlend` glides every parameter toward that
blend with the Morph Glide time constant (95 % after *Glide* seconds). On
a point the blend is that preset; between points it is a sound nobody
saved. Switching the map off copies the gliding values into the live
parameters, so the sound stays where the map left it. The map is a
parameter set (MapActive, MapX, MapY, MapRadius, never part of a preset),
so MIDI, OSC, automation and the gesture layer can steer it; on the Quest
the hand menu has MAP ON/OFF and the left hand's reach and height move the
cursor. `ambient_render --map x y [radius]` renders any cursor position
offline and prints the neighbours and weights. Measured: on a preset's own
point the blend reproduces its parameters to 0.1 %; half-way between two
presets a float lies between their values; the engine glides to the far
decay of the preset under the cursor within the glide time and reports the
value through `blendValue`.

## Route (the map plays itself)

`Route` (`Core/include/ambient/Route.h`) is a list of up to 32 waypoints
on the map — position, blend radius, a travel time to reach the point and
a hold time to stay — walked by the engine: `routeStep` moves the map
cursor with smoothstep travel between points (no jumps), holds, and either
loops or stops at the last point and switches itself off, leaving the
cursor where it is. A route always plays through the map blend, so a whole
set becomes a path: parameters *Route Play*, *Speed* (0.25–4×) and *Loop*
are performance state like the map itself. The text form
`Preset Name|travel|hold[|radius]` (or `x,y|travel|hold[|radius]`) names
presets rather than coordinates, so routes survive a re-measurement of the
map; twelve **route presets** (`Core/src/Route.cpp`) are 20–40 minute
sets — Night Descent from Sleeping Drone into Bedrock Field, Glass to Storm,
Cosmos Crossing, Ninety Minute Arc and so on. The plugin's map view has the
route strip (route preset, play, loop, speed, *+ point* appends the cursor
as a waypoint, *Route…* edits the text) and draws the route with numbered
points and the segment being walked; the Quest hand menu has ROUTE
PLAY/STOP with the route from `ambient.cfg`; `ambient_render --route
"Night Descent" 40` renders a set at 40× speed. Measured: every route
preset parses and round-trips through text, a two-point route reaches the
smoothstep midpoint half-way through its travel, and at speed 2 the engine
walks a 12-second route in 6 seconds and switches itself off.

## Sets as timelines, and the automatic sound test

**Set recording.** `SetTimeline` (`Core/include/ambient/Timeline.h`) holds
every parameter change and every note with its time. The plugin's Perform
page has *Record set*: the starting state goes in at t = 0, then every
block logs the parameters that changed (hands, knobs, OSC, MIDI, macros,
routes — all of it ends up as parameter changes) and the notes; *Stop*
saves a `.ambientset` text file (`12.500 param far_decay 42`, `13.02 on 57
0.8`, `40 off 57`). *Play set…* replays one through the host parameters,
and `ambient_render --set-file set.ambientset` renders it again offline
at any sample rate, with the set's length plus 20 s of tail by default. A
good evening becomes reproducible, and can be rendered in higher quality
than it was played.

**Sound test.** `Tools/preset_check.py` renders every preset for 10 s
(held chord plus a busy brain) and fails a preset that is louder than
−12 dBFS in its second half, peaks above 0.98, jumps more than 0.3 between
samples, carries more than 0.02 DC or goes non-finite; exit code 1 when
anything fails, `--json` for a report. Its first run found four: Alto
Voices too loud (−11.4 dBFS, master trimmed) and three presets with DC of
0.03–0.11 — every preset that uses the feedback's phase modulation. A
partial that modulates its own phase carries a DC term (J1 of the index,
the same mechanism as an FM pair at ratio 1), so the voice now blocks DC
after the bank whenever the FM path is on; and a saturating feedback loop
whose DC gain exceeds one locks onto a DC operating point (Feedback Hiss
sat at +0.11), so the loop now blocks everything below 10 Hz before the
saturation. The measurement caught what listening had not.

**Quality, as opposed to soundness.** `Tools/rate_presets.py` scores every
preset on four axes that can be measured: ALIVE (how far the descriptors
travel between an early window of a render and the whole of it), MOVING
(spectral flux), REACH (how many of the instrument's twelve families the
settings touch) and APART (distance to the nearest other preset in
descriptor space, on a bucketed grid so six thousand presets are a few
hundred thousand distances rather than eighteen million). It exists because
"make the presets better" needs something to aim at, and the first thing it
found was that the built-in bank reached a median of three families out of
twelve: most of those presets predate the modulation matrix, the second and
third source slots, the z-plane's 155 shapes and the BEAT source.

`Tools/enrich_presets.py` (built-ins) and `Tools/enrich_packs.py` (the
library) fill those gaps, and both work under the same two rules. Every rule
fires into an *empty slot only*, so a preset that already has a matrix, or a
z-plane, or three sources, keeps exactly what it had. And every preset is
rendered before and after: if the change moved its level by more than 1.5 dB,
its centroid by a quarter, or its bass or width by 0.12, the change is thrown
away and the preset is put back as it was. 190 of 191 built-ins kept (Init is
left blank deliberately), 5637 of 5969 library presets kept. REACH on the
built-ins went from 0.25 to 0.42; nothing else moved.

Two things this measured that are worth keeping. The first is that the
enrichment must vary per preset: the first version gave all 191 the same
four LFO rates and the same four routes, which is the opposite of what APART
asks for -- the ladder is now anchored on each preset's own base period
between 34 s and 78 s, and the routes are drawn from a character-appropriate
pool without repeats, giving 190 distinct matrices. The second is that
deeper modulation does not make a drone more alive. Measured over three
minutes, the timescale these LFOs actually run at, forcing every added LFO
to full depth moves the median ALIVE from 0.425 to 0.429 and costs up to
3.7 dB of level. A drone's ALIVE is made of its own slow architecture -- the
arc, the bloom, the conductor -- not of a modulator going round.

## Morph (the performance control)

Two full parameter snapshots live in the engine (`slotA_`, `slotB_`, atomics
per parameter, writable from any thread). While *MorphActive* is on,
`Engine::effectiveParam(id)` replaces the live value with the blend at the
current position: floats interpolate in the skewed domain the knobs use
(`p = ((v−min)/span)^skew`, lerp p, invert), integers round, choices and
switches take A below 0.5 and B above. The position glides toward *MorphPos*
at `1/MorphGlide` per second (glide 0 = jump), so a controller or a hand can
jump while the sound follows over minutes. The Morph section is excluded
from every preset scope: loading presets never disturbs a running morph.
Slots are saved in the plugin state and in `.noctuary` files.

## Toward VR (the reason for the design)

The target is a 30-minute drone set built with nothing but slow movements
in a headset: hands and head change parameters, the picture is the sound.
What that already dictates here:

* the whole instrument is a framework-free library with atomics as its only
  control surface, so an OpenXR app can drive it directly;
* every control is continuous and glides, nothing steps;
* the morph is one scalar — the natural quantity for a hand to own;
* the engine exposes observers (`soundingNotes`, `noteDistance`, `arcValue`,
  `morphPosition`, `brainRoot`) for a synaesthetic visualisation to read;
* the distance model maps one-to-one onto placing sources in a 3D scene.
Both of these exist now (`ambient/Osc.h`, `ambient/Gesture.h`): the OSC
server (framework-free UDP, Winsock/BSD, own parser for messages and
bundles) writes parameters through an `OscSink` and hand/head data into the
`GestureLayer`; the gesture layer maps inputs to parameters with range,
smoothing, dead-zone and a clutch, and its mappings are a small text format
saved with the plugin state. Decision 2026-09-04: the headset app is native
(NDK, OpenXR + `XR_EXT_hand_tracking`, Vulkan/GLES, Oboe), starting from
Meta's native hand-tracking sample; the core already cross-compiles for
arm64-v8a. Details in `docs/quest-plan.md`.

## Plugin shell

* MIDI learn: right-click on a control arms it; the next controller message
  binds (one controller per parameter, one parameter per controller); the
  map lives in the plugin state. Controllers write the parameter through the
  host, so the DAW sees the movement.

* `AudioProcessorValueTreeState` built from the parameter table; the raw
  atomic values are copied into the engine at the top of every block.
* MIDI note on/off and all-notes-off; sample-accurate splitting is
  deliberately absent (nothing here is faster than a control block).
* Programs = presets; state = APVTS XML + Scala text + display name.

* **Session recall** (standalone only). JUCE writes the whole state into the
  standalone's settings file when the window is closed and reads it back on the
  next start. Two things were wrong with that as a feature. It only happens on a
  clean exit, so a crash, a kill or a power cut loses the evening; and the state
  carried every knob but never the *name* of the preset they came from, so a
  restored session played the right sound under the label "Init" and looked
  like a feature that did not work. Now a timer writes the state whenever it
  has actually changed -- the block is hashed, which is cheaper and safer than
  deciding what counts as a change -- and the two preset names travel with it.
  The names, not the indices: a pack added between two sessions renumbers every
  preset behind it, and an index would then name a different sound. **Recall**
  in the header switches it off and drops what was stored; the switch lives in
  the same settings file, and is read before JUCE gets the chance to restore
  anything. The plug-in deliberately has none of this: there the host saves the
  state with the project, and a fresh instance quietly loading somebody else's
  last session would be a bug.

* **Deployment** (`Deploy/`). The build that other people get differs from the
  everyday one in three ways, each for a reason worth writing down. The MSVC
  runtime is linked in, so there is no redistributable to chase -- and the
  release script proves it with `dumpbin` rather than trusting the flag: it
  refuses to package a binary whose imports still name `VCRUNTIME`. It is built
  for AVX2 (52x realtime against 39x), which every x86-64 processor since 2013
  has; the setup asks the processor first, because a machine without it does
  not fail gracefully, it takes an illegal instruction and dies with nothing
  said. And it builds in its own tree, so the everyday one is left alone.
  The tests are run in that configuration, not in the developer's: a static
  runtime and a missing AVX2 are exactly the kind of change that is fine until
  it is not. The setup itself (Inno Setup) installs the standalone, the VST3
  and the preset packs, each with its own checkbox, for the machine or -- for
  anyone without administrator rights -- for one user, the same script either
  way because every path in it is an `{auto...}` one. The packs go into a
  folder of the installer's own rather than into Documents, so that removing
  them again can never take a pack the user put there themselves with it.

* **The sample library** (`Tools/make_content_pack.py`). The 14336 presets in
  the packs name 9983 samples, wavetables and impulse responses that are far
  too big for git -- so they are a downloaded package, and the setup fetches
  and unpacks it. Two things happen on the way in. Only what is referenced
  travels: the library folder holds more than the packs use, and shipping the
  rest would add gigabytes nobody's preset asks for. And the samples, which
  are generated as 32-bit float, become 24-bit PCM. That is a quarter off the
  size for headroom they do not have: measured, every one of them peaks at
  exactly 0.5, and the error the conversion adds sits at -149 dBFS RMS. The
  wavetables (16-bit) and impulse responses (24-bit) were already PCM and are
  copied untouched -- which is worth saying, because the first version of the
  script reported them as "would have clipped", a number that was simply
  false. The proof that it is inaudible is not the arithmetic but the render:
  four pack presets measured against both libraries agree in every descriptor
  to the last digit printed, and differ only in the sample hash, as they must.
  4.86 GB of source becomes 3.06 GB in three archives -- deflate at level 6,
  measured at 85 % where level 9 is also 85 % -- because a release asset may
  not exceed 2 GB. Names, sizes and SHA-256 are generated into an include the
  installer reads, since a hash that does not match what is on the release is
  a download that fails at the last possible moment.
* Editor: sections flow-laid-out from the table (knobs, toggles, combo boxes),
  header with preset box, voice count, brain root, scale, arc value and a
  keyboard strip where near notes are bright and far notes dim; *Load Scala…*
  file chooser. The page is laid out once in a fixed design space and scaled
  as a whole, so dragging the corner zooms and never reflows. Everything is
  in sight at once, nothing scrolls: two columns (voice and morph on the
  left; effects, cosmos and conductor on the right), and rows whose sections
  are of a kind -- the three sources, the two filters, the effect pairs, the
  conductor's tables, morph and macros -- page through tabs, each tabbed row
  as tall as its tallest page so switching moves nothing else. The room a
  row's knobs leave goes to a live display drawn from the engine's numbers:
  the oscillator's cycle and partials, the source slot's table, grain window
  or noise colour, the two filters' response, the conductor's notes as a
  scrolling roll. Along the bottom the modulation strip: one card per source
  (dragged onto a knob it becomes a route; right-click lists and removes its
  routes), and tabs with the eight LFO editors, the six envelopes and the
  matrix as text. A modulated knob wears a thin ring in its source's colour
  and a second arc from its value to where the modulation is pushing it this
  instant; its right-click menu lists what drives it, each route removable.
  The editor is four translation units (`EditorCommon.h` holds what they share): the frame and
  the pages, the modulation strip, the in-grid displays, the browse and perform pages. Each
  section's title carries a die: a click draws that section's parameters again (in the
  parameter's own skewed domain, inside the middle 70 % of its range), shift nudges them.
  Undo, redo and an A/B compare work on whole parameter snapshots. The header carries the
  output's own spectrum with its peak level, and a layout button that cycles three shapes:
  Normal (one page, tabs), Compact (since 1.9.0: the same page in columns 88 % as wide, every
  page refitted into them; before that the rows above ten cells wrapped into two -- wrapping
  every wide row gave 1.27 : 1, worse than the shape it started from) and Expanded (every page
  of every tab row laid out under one another with a title in the tab's place, no tabs: the
  four sources, the two filters and the conductor's six tables all in sight, for a tall
  screen or for reading a preset through).
  The mode is kept in the state; `AMBIENT_EXPANDED=1` / `AMBIENT_COMPACT=1` set it for a run,
  and the manual's tab pictures are always taken in Normal.

  The GUI round (after a third report, on the interface): a knob shows everything that moves
  it -- the matrix as before, in its source's colour; the morph or the map's blend as a
  neutral arc from the knob's value to the live one (`effectiveParam` + `modAmount` against
  the raw value); and the arc and the tide, whose swing is too slow to see as motion, as a
  dot on the outer ring (`halo`) saying where in it they are. The Tuning page's display draws
  the timbre's roughness curve across the octave from the same `BrainSpectrum` the conductor
  judges with (computed every block now, a dozen pows; nothing reads it unless Timbre is up,
  so the sound is untouched: oracle 41/41), the scale's degrees as ticks -- they sit in the
  dips when the scale is the timbre's own -- the key the conductor found, the comma, the
  tide. The Coherence page's display: the Kuramoto ring, the Lenia field as a grey grid, the
  six attractor readings; asleep, they say so. The stage's three planes (conductor, keys,
  second conductor) are lines a hand can drag, through the host's parameter system, so the
  drag is automated and undone like a knob; the voices stay a picture, because the conductor
  put them where they are. A glyph where the loop closes (Feedback says it returns to the
  sources, Source 1 that it is fed), a sentence under a one-strand additive bank saying why it
  is one strand, and fixed-width figures in the knobs and the header line (shrunk to fit --
  the first draft cut "40 min" to "40 mi"). Declined from that report, with reasons in the
  manual: the three-zone rebuild, the isometric stage with grabbable voices, a 3D filter cube,
  a second preset explorer. `AMBIENT_SHOT=<png>` writes a picture of the whole editor after ten
  seconds of a chord, for looking at the panel without a screen grab (a screen grab takes
  whatever else is on the screen, and did). From the reply to that reply, four more small
  things: the header names the conductor's key and confidence and glows with the cascade's
  excitation; the notes roll shows the deja-vu ring (place lit), the cascade as a glow at its
  newest edge and the homeostat's lean as a needle; the Coherence display draws the attractors'
  x/y orbits from the last forty seconds; and with Arc Clock on the Arc knob's halo becomes a
  24-hour dial with marks at 4, 10, 16 and 22 (`clockHour` property). Declined from it: tying
  the Expanded layout to a pixel height (the editor scales as a whole, and a reflow at a
  threshold is exactly the surprise calm technology forbids -- it stays a mode), component
  encapsulation for its own sake, and the three-zone phase, whose dock, drawer and soundstage
  are the strip, its tabs and the Perform page under other names.

  The layout critique, after 1.5.0, was right about Expanded: at 2080 x 2604 it was 4 : 5, and a
  page that scales as a whole lands at 41 % on a 1080-line screen. The height was the greyed
  knobs -- every source strip the union of every type's cells, five rows for two or three
  live ones, and Off slots, Off filters, inactive morphs all fully open. Three changes, all
  under one principle, that the layout is a function of the parameter state and never of the
  window: (1) a cell the slot's type does not use is `unused` -- not laid out, not drawn -- so a
  strip is as tall as its type (`updateSourceCells` sets the flags and calls `rebuildLayout`
  when they change; the Texture and Wavetable buttons follow their types); (2) a section whose
  switch is off is `collapsed` to its title and the switch's own cells (`closers()`: the slots'
  Type, Z-Plane's Mode, Cosmos Send, Strike level, Morph Active, Brain 2 Active, Early Room,
  Body, Room, Cloud, Feedback's two paths, Patina), the manual export sets `openAll_` so its
  pictures are of open sections, and a closed page hides its display; (3) Expanded is three
  columns -- the voice; the room, the effects, the Cosmos and the spectrum; morph and the
  conductor -- with the last group of each column stretched so the three end level. Measured:
  Normal 2269 x 1260 (86 % at 1080 lines), Expanded 2269 x 1352 (80 %, from 41 %) -- and then,
  after Rene's next look ("the middle column has room"), Expanded became a page of its own
  (`expandedGroups_`, no tabs): the shaping (Air + Filter, Envelope + Expression, Z-Plane +
  Vector, each with its display) moved to the middle column; Feedback, Room, Early Room, Body,
  Patina, Cosmos and Strike share one row, as do Morph, Macros and Brain 2; rows without a
  display wrap at `kWrapW` = 1400 px, so a row of closed sections is one line and a row of open
  ones is two, never a column a screen wide; the spectrum is the middle column's filler at 80 px.
  Measured: 2269 x 958, about 2.4 : 1, 113 % at 1080 lines. Then, since the rows without a
  display still left their right halves empty: rows of a group that have no display -- by
  design, or because their only section is closed -- merge into one flow (`rowsOf`), so the
  effects, the background with every closed section, and morph + macros + brain 2 + clock are
  each one wrapping line, two Off slots share a line, the strand bank gets the bank's scope as
  its display and the stage stretches to close the voice column. 2269 x 923, 2.5 : 1; on a
  1920 x 1080 screen the width limits now, at 85 %. A type
  change or a switch thrown relays out the page and resizes the window to the new ratio, on
  the player's own action -- never on the window's. `AMBIENT_LAYOUT=<0|1|2>` sets the mode for
  a run over the recalled one (session recall keeps it, which is why the `AMBIENT_EXPANDED` run
  left the next start expanded).

  Then Rene sent a picture of the tabbed page itself -- "viel zu viele leere Flächen, das sieht
  einfach furchtbar aus" -- and it had the same disease from the other side: a tabbed row was
  as tall as its tallest page and as wide as its widest, so the Clock stood alone in a box three
  rows high, a closed Cosmos in one two rows high, the macros in a band with nothing beside
  them, and the strand bank under Source 1's picture left a row of nothing under Source 1's
  knobs. 1.9.0 gives the tabbed page its own layout (`layoutTabbed`), on three rules. (1) Every
  page is FITTED to its column: the fewest rows at which its sections, each as narrow as that
  allows, stand side by side in the column's width (`fit`, `widthsAt`: the search runs rows
  1..11 and per section widens from its widest cell until the rows suffice); so the sections of
  a page come out the same height and nothing is under a short one; what the row has over goes
  to the display, or -- where there is none -- a cell at a time, round robin, to the sections
  that can take it without losing a row, because a last row that is not full reads as a
  section and a band at the end of the row reads as a hole. The hand-set widths (`wideUnits`)
  now decide one thing only: the column widths, from the widest page at those widths; Expanded's
  flows keep their own (`flowUnits`, the old ones). (2) A row is as tall as the page that is
  open on it, not as its tallest page -- a tab click is the player's action and may relay out
  the page -- while the design height is still the sum of the tallest pages (`colYMax`), so the
  window never changes shape on a tab click, and the last group of each column grows into the
  difference: its page is refitted for the new height (as many rows as it holds, so the
  sections stand tall and the display takes the width), which is why the cluster brain is nine
  cells wide when the columns are level and four when the left column is much the taller.
  (3) The pages are cut so that each fills its row and every page that can have a display has
  one: Strands beside Source 1 (and not on the page at all while the slot is not additive --
  its cells are `unused` then, not greyed), the two filter pages sharing the filter picture,
  Envelope + Expression sharing the envelope's, Morph + Macros + Vector one row with the vector's
  square (the Vector left the sources' tabs, the Morph row its own), Coherence + Clock one page,
  and a closed page keeps its display, which says why it is empty ("cosmos: send is off",
  "choose a type to the left"). Compact is no longer a wrapping rule but a width: the columns
  at `kCompactFactor` = 0.88 of Normal's, never narrower than the narrowest page, and the fit
  does the rest. Measured at design size (`AMBIENT_SHOT` now sets the window to it): Normal
  2522 x 1483 (1.70 : 1; before, at the same scale, 1.80 : 1 and the same zoom on a 1080-line
  screen), Compact 2224 x 1712 (1.30 : 1, from 1.52), Expanded unchanged at 2.46 : 1.

  What was left after that was a page whose sections are all closed and that has no display --
  Early Room + Body + Patina all off, Strike off -- one row with a band beside it. Rene's
  question settled it: "Ehe wir da Löcher haben, sollten wir sie nicht lieber sofort
  ausklappen?" A section is closed to save the page room; where the row has the room anyway,
  closing buys nothing and a title beside an empty band is worse than the section it hides. So
  the layout opens what fits: on a fitted page, each closed section in turn is opened if the
  page still fits its column at the same number of rows (`Section::opened`, set by the layout
  and cleared at the start of every pass -- `collapsed` stays what the parameters say, or the
  timer that watches for a thrown switch would see the layout's own doing and rebuild for
  ever); in an Expanded flow, if it fits on the line as it stands. Now Early Room, Body and
  Patina stand open side by side, Strike shows its type, decay, damping and bank, the Morph
  page its A and B pickers. Expanded pays for it: 2.40 : 1 instead of 2.46, since a filled line
  wraps sooner. What is left is only a section's own last row, which need not be full.
  Help (`Core/include/ambient/Help.h`): one or two sentences for every
  parameter (`paramHelp`, families share their text so the three slots and
  eight LFOs cannot drift apart; the self test insists every parameter has
  one) and the manual by topic (`helpTopicText`). The plugin shows them as
  tooltips, as the header line that follows the mouse (name, value, text,
  how many routes drive it), and as the Help page (button or F1): topics on
  the left, the text at a readable line length, and to its right the
  pictures -- a drawn signal-flow diagram for the overview, and for the other
  topics snapshots of the topic's sections taken from the panel that moment
  (the tab is switched in for the picture and back) plus a second, live copy
  of the unit's display. Texts live in the core so every shell tells the
  same story.

## The near layer (13.09.2026)

Everything the instrument played was a plane: sources made to be sustained
and sent back into the far reverb, and the one thing in front of them was the
Strike, which decorates a note the conductor was playing anyway. Rich's
foreground is something else -- a flute blown once, water falling into a
bowl, a voice on a radio, a rim rubbed until it sings, and on *Strata* and
*Empetus* a sequence that arrives out of the horizon, taps in front of you
for a few minutes and dissolves again. This section is that foreground, built
as a **layer of its own** rather than as a property of the sound presets,
which was Rene's call and the better one: the 14336 backgrounds stay exactly
as they were measured, and a foreground chosen for the night stays while the
backgrounds change under it.

**Two sections, one bank.** *Near Source* is a fifth source slot that only
the near events render: any source type, with its own envelope, filter and
Karplus-Strong strike, so it owes the sound preset nothing but the room
around it (the space model, the reverbs). *Near Events* is the foreground's
own conductor. Together they are `PresetScope::Near`, a bank like the
Cosmos bank (`NearPresets.inc`, 37 presets in seven families, generated by
`Tools/make_layer_presets.py`): a sound preset neither carries nor clears
the layer, and the crossfade at a preset change hands the scheduler over
(`Engine::adoptNear`) as it hands the cluster over, so a sequence that was
running keeps its ring. The keys are `fore_*` -- `near_*` already belonged
to the near reverb -- and the render tool takes `--near-preset`.

**Five near sources** (`SourcesNear.cpp`), each a physical caricature small
enough for every voice, each calibrated against the harmonic table at the
same Level and held to it by the selftest:

- *Flute*: Cook's jet-drive waveguide with the loop closed as an open pipe
  closes it. The jet's travel sets the register -- half a period speaks the
  fundamental, a quarter the octave -- and Position is the embouchure between
  them, so the same pipe overblows when it shortens. Measured in tune across
  the register like the Bow (the loop is the period less the filter's own
  delay and the feedback sample).
- *Murmur*: a glottal pulse through three formants walking between vowels at
  a syllable's pace, fricatives, stops, phrases and pauses, a pitch that
  falls over the phrase and rises on the stresses. Position is the medium:
  a voice in the room at 0, and towards 1 a radio -- the 300-3000 Hz band, a
  saturation, the carrier's hiss, the squelch that closes after every
  phrase, and from 0.7 up the Quindar tones (2525 Hz to key, 2475 Hz to
  release) that every Apollo air-to-ground loop opened and closed with.
- *Bowl* and *Ice*: the Bow's friction curve driving a modal body instead of
  a string. The bowl's six modes sit on the (n^2 - 1) ladder every
  thin-walled bowl has, every mode a doublet a hair apart -- the beating a
  real bowl warbles with, and the beating P1 asks for -- and the tone builds
  over seconds as a rubbed rim does. Ice is the same friction on low, dense,
  short modes with a slip clock in front: under a slow load the stick creeps
  rather than glides, and every give is a pulse into the modes.
- *Drops*: van den Doel's bubble -- a sine whose pitch rises as it decays,
  with the damping the physics gives it -- and the click of the impact
  ringing a vessel of two modes. Density is drops a second on a Poisson
  clock. And a noise colour, *Cicada*: two insects per ear, pulse bursts
  ringing in a band, resting between.

**The scheduler** (`Near.h`, header-only like the conductor, stepped by the
render and by the audit alike). Every so often -- the gap drawn as the
conductor draws its own, a shifted exponential, so there is never a pulse
-- it plays one thing on the near source: a *Note* held for Length; a
*Phrase* of two to four tones over the Length, each a slide to another
consonant degree on the voice's portamento and its gravity, the last one
home, and at the end the shakuhachi's meri, a quarter tone down and back;
or a *Sequence*, a ring of Steps notes on a clock of eighths that breathes
(the tempo on a drifter, every step a few milliseconds early or late on a
drifter of its own, never on white noise), transposed with the root,
mutating one step per cycle with probability Mutation -- its degree, its
octave, its gate, its velocity or its accent, so the articulation drifts
with the notes (the Turing machine's shift register) -- thinning and
thickening on a slow density gate, ghost notes between the steps and the
odd accent sent to the far plane, the filter breathing two octaves under
the cutoff to one over it and back across the run (Bloom), arriving out of
the far plane and leaving into it (Approach). The pitch is chosen against
the harmony: Consonant weighs every degree from a fifth under the cluster's
middle to a twelfth over its top by its interval to the root (the fifth
first, then the ninth and the pure third) times its consonance with every
sounding note by the conductor's own ear; it prefers the soloist's register
(a fifth above the highest body voice, or a gap of a third or more), avoids
what already sounds, and never sits within a critical band (Glasberg and
Moore's ERB) of a sounding voice, where the drone would mask it. Rules, all
measured: never in the conductor's planned silence, never within six
seconds of a root change, never two at once, never into nothing -- and into
stillness: an event that is due waits, up to half the rate, for four
seconds without an onset or a release of the conductor's, which is what
makes it an answer rather than an interruption. Its two asks of the
background (Rene, 13.09.): while a Note or a Phrase sounds, and for ten
seconds after, the conductor begins no new note -- its holds run on, its
releases happen; stopping its clock outright was tried and is heard as a
pause button -- and under a sequence the root stays and the events come half
as often, so the line is never thrown across a changeover. Proximity is the
near field's lift on the event's voice, up to six decibels between 120 and
300 Hz -- a band, not a shelf, so the Foundation and Bass Mono keep their
ground -- and a function of the event's plane. The far reverb's return
after a duck is now a parameter (`far_unmask_return`, 1.2 s as it always
was; five to ten seconds make the horizon's return a gesture).

**The archive and the Clip.** Rene's second wish for the foreground was
the real thing: astronauts on the loop. `Library/Archive/` is fetched, not
generated (`Tools/library/fetch_archive.py`, every file with its source in
`SOURCES.md`): NASA's historical sounds -- the Discovery, Shuttle, Apollo
and Mercury loops, the Quindar tones, Sputnik, Saturn's radio emissions,
Jupiter's lightning -- the Mars recordings of Perseverance and InSight,
Juno's Ganymede flyby, and the Webb sonifications, 87 files, all works of
the United States government and free of copyright (the two songs on the
pages, made for NASA by named artists, were left out). A sixth source type,
*Clip*, plays a recording straight through, once, from Position, at its own
speed or pitched to the note, because every other way this instrument has
of playing a clip takes it apart and a sentence in grains is not a sentence.
The near source has a clip of its own (`Engine::setNearTexture`, carried
across preset changes with the rest of the layer; Source 4's clip where it
has none), a near preset names one relative to the library's root
(`resolveLibraryFile`: `$AMBIENT_LIBRARY`, beside every loaded pack folder,
the user's and the installer's folders, the source tree's `Library`), and
the bank's *Archive* family plays them: "Houston, We've Had a Problem" every
seven minutes, "Wind on Mars" arriving out of the horizon.

A preset may name a folder instead of a file (`Archive/Radio/Quiet-Please/`):
the near source then holds a pool of its recordings, and every event plays one
of them, drawn at random and never the one just played -- a voice that says
something else each time, at the moments the scheduler chooses by its rules,
not a loop. Phrases, not beds: a clip of the pool is kept to twenty seconds,
and the ones the slicer makes are two to six. The pool is cut by
`Tools/library/slice_speech.py`: it takes an item of the Internet Archive or a
recording on disk, cuts the speech at its pauses (a frame is speech twelve
decibels over the recording's own floor, a pause three hundred milliseconds
without; what does not move at the rate of syllables, three to nine a second,
is the organ sting and is dropped), and gives each phrase the sound of the set
it came out of: a band of 350 to 3200 Hz, a little saturation, the hiss, and
the breath of the squelch as the carrier drops. Every file's origin and the
item's licence tag go into `SOURCES.md`; an item whose tag forbids
derivatives is refused unless the matter has been looked at, because a phrase
cut out of a recording is one.

The larger store of such voices is the Library of Congress's own sampling
project, Citizen DJ: audio from the Library's collections that it has
identified as free to use, already cut into clips of a few seconds, each pack
with a statement of why it is free. `Tools/library/fetch_loc_samples.py` takes
five of the packs -- Edison's cylinders, the variety stage, the government
films of the National Screening Room, Tony Schwartz's New York, Joe Smith's
interviews -- and of each only what is a voice: the packs are mixed, a rag
beside a monologue, and the near layer wanted speech. The catalogue title is
asked first (a "march", a "polka", "with orchestra" is music by its own
account), then the clip is measured with the classic discriminators of
Scheirer and Slaney, calibrated on the packs themselves, interviews against
opera and chamber music: speech has more silence, a zero-crossing rate that
jumps between vowel and fricative, the syllable's four hertz on the subband
envelopes, and fewer voiced windows than singing. Seven hundred and twenty-four
clips remained, spread across the packs, at most twenty seconds each, as
FLAC under `Archive/LoC/`, with the Library's statement in `SOURCES.md`. Not
taken, and why: the National Jukebox and the MusicBox Project (music), the
Jukebox's popular songs in particular (free in the United States since 2022,
but many of their composers died after 1955 and are still protected in
Europe), the dialect interviews (private people; the Library asks for care,
and a voice at the ear is not the place), and the Free Music Archive subset
(music). A rule learned the same night, for the films
of the 1950s and 60s that are "public domain" in the United States for want
of a copyright notice: that status does not travel. A package published from
Germany is under German law, seventy years after the death of director,
writer and composer, and the German-American copyright treaty of 1892 (the
Federal Court of Justice's *Tarzan* ruling of 2014) keeps the shorter American
term from applying -- so those films stay out.

**The signals (the second foreground round, the same night).** Rene's list
of eight and a few more, built as nine small source types beside the
instruments, each the sound of a thing rather than an instrument: a
*Whistler* (a lightning stroke's pulse dispersed through the magnetosphere,
Eckersley's law, the whistle falling as one over the square of time), a
*Shaker* (Cook's PhISEM, an energy that shakes top up and beans that hit the
shell on its probability), a *Chime* (struck bronze in five modes, the prime a
doublet a hair apart, which is where a pair of cymbals' beating comes from; a
hum under it for a church bell), a *Geiger* tube (discharges on a Poisson
clock with clusters that leap to forty-five a second and fall back), a
fluorescent *Tube* (the starter's clicks, the choke's hum at twice the mains,
the plasma's hiss chopped by the same half-waves), the *Krell*'s circuits (FM
whose carrier and index a Rössler attractor steers), a *Beacon* (a chirp and
eight bits of frequency-shift keying, Tukey-edged), a number station's
*Morse* (five-figure groups at a chosen speed under the ionosphere's flutter),
and a shortwave *Dial* (heterodyne whistles wandering over the band's noise).
Two parameters of the events came with them, because the list asked for
places the layer could not name: *Distance* is where an event sits between
the planes (a foghorn at 0.85 deep in the far field, a rattle at 0.05 in front
of the nose; Approach arrives there from the horizon, and negative Approach
leaves from there, the whistler falling away), and *Dry* is the share of an
event that goes past every reverb and delay straight to the output, the
Geiger's needle that no room may soften. What the near layer still cannot do
per event is choose its sends into Delay 2 or the Cosmos: it takes the mix of
whatever sound is playing, and Distance and Dry were the two that covered
most of the list without giving each event a mixer of its own.

**Auto, and the journeys (the same morning).** Two things Rene asked for once
the foreground existed. *Auto*: a sound preset from a pack brings its own
foreground. Rene wrote a table per artist -- what share of the artist's
presets get one at all, which near presets they draw from and with what
weight, and a class per group (often, now and then, seldom) that scales the
near preset's Every -- and `Tools/make_near_auto.py` compiles it into the
instrument keyed by pack (`NearAuto.inc`). The draw is a hash of the preset's
name, so a preset brings the same foreground every time it is chosen and its
neighbour in the pack brings another; a near preset chosen by hand switches
Auto off and stays, and Auto switched on draws for the preset that is
playing. The render tool keeps Auto off unless asked (`--near-auto`), because
the library is measured, mapped and rated without a foreground, and must be.

*Journeys*: presets in a row, each held for a while drawn from a range, each
crossfaded into the next over a drawn fade, round and round when cyclic -- an
evening that plays itself, and never the same way twice. A plain text file
(`*.journey`, one preset a line with its dwell and its fade, and optionally the
foreground it wants), a player in the plugin that advances on the preset pump
and asks the transition for the drawn fade, the box in the Morph section that
starts one, `+ now` and `Save...` to write one's own into
`Documents/Noctuary/Journeys`, and `--journey` in the render tool (which
cuts where the plugin crossfades). The templates the instrument ships,
`Tools/make_journeys.py` makes from the library's own measurement: for every
pack a *Journey* of twelve presets spread across the pack's space (farthest
points on the standardised descriptors, then chained by nearest neighbour so
every crossfade is a small step) and a *Night* of its twelve stillest, and for
every family of artists a *Crossing* that walks from pack to pack, each step
the nearest neighbour under another name. The neighbour in that space is a
preset that measures alike, which is the promise of a gentle crossfade and not
of a good evening; the good ones are Rene's own, written beside these.

**Slot roles** came with it, for the sound presets: each slot may sound in
every note, or only in the lowest, an inner or the highest note of what its
owner is sounding -- the cello under the chord, the chime on top -- re-read
as the cluster changes and faded over a second and a half. Every preset ever
written has every slot on All, so nothing already measured moved.

## What the release build found (13.09.2026)

The 2.0 release is built twice over, in the configuration that ships, and
its host test failed in that configuration on a check that had passed all
day in the working build: *when the old preset is gone the new one is already
audible*. The arriving preset was silent -- exactly zero, nine voices sounding
nothing. Three faults came out of that one failure, none of them in the
release build.

**A transition within the burst window.** A program change that arrives
within 300 ms of the one before it has its files -- the clips, the wavetable,
the rooms -- put off to the preset pump, so a host automating the program
number does not read a sixth of a second of disk per step. The host test's two
changes are two seconds of audio apart and, on a fast machine, a fifth of a
second of wall time: the second one counted as a burst, its textures were
deferred, and the pump that would have read them never runs in a test. In a
host it does run, a quarter of a second later, into whichever engine is live
by then -- the right one once the audio thread has swapped, the one on its way
out when it has not (a transport that is stopped). A transition now reads its
files as part of building the incoming engine, before that engine is made the
instrument, whatever the clock says; the deferral stays for program sweeps.

**Freeze stopped the entrances.** Five presets of the fourteen thousand
measured as silence in the night run, and four more had only one slot left.
All of them had *Freeze* on and every slot delayed or shaped. Each slot's
entrance was read off the voice's movement clock, the clock Freeze stops --
so a delayed slot never entered while Freeze was on, and a preset frozen from
its first sample with nothing but delayed slots was silent for good. The
entrance is an envelope, and the rule on the page above says the envelopes
keep their time: it now runs on a clock of its own that Freeze does not touch,
and the self test holds a frozen voice with a delayed slot and waits for it.
The generator writes Freeze into six per cent of the library, and 559 of
those presets carry a delayed or shaped slot: their minute was measured with
that slot missing, so they are measured again (`remeasure_presets.py`, which
does for a list of names what `measure_packs.py` does for the library, and
leaves every other preset's gain where the night run put it -- a second full
pass would have lifted the three thousand presets under the loudness window by
another six decibels).

**An offset, integrated.** The other three silent presets (Azimuth Circle,
Etching Bed, Tapehead Expanse) played for fifteen to thirty seconds and then
stopped, exactly to zero, while every one of their stems played on. Finding
that took a wrong turn first: a bisection that dropped each of a preset's 240
keys in turn and measured the render's RMS reported every variant alive,
because a preset that dies at fifteen seconds still averages to a healthy
level over twenty-four -- the measured window has to begin *after* the death
(`--skip`), and the rating's descriptors would have hidden the same thing
had the night run not skipped its warm-up. With the window in the right place
one key came up in all three: *Patina*, and then only its amount, not its
wow, hiss or age. The stems told the rest. The far bus of Tapehead Expanse
carried a direct current that grew from nothing to -2.1 in thirty seconds
while its music stayed at 0.4 peak: a texture with a few per cent of offset
(the near bus already stood at -0.12), and a hall whose loop gain is a hair
under one -- a forty-second tail -- integrating it with a gain of hundreds.
The Patina's soft clipper railed on the offset and flattened the music
riding on it; the output DC blocker, one stage later, took the rail away and
left nothing. Three answers, each with its own check: the hall blocks direct
current at its input (a constant 0.3 in, a tail mean under 0.05 after ten
seconds), a texture loses its mean as it is loaded (a recording's offset is
never wanted, and subtracting a constant keeps a seamless clip seamless), and
the Patina has the output blocker's twin ahead of its clipper, computed only
while the Patina is on so every preset without it renders sample for sample as
before. The whole recipe -- an offset texture, a forty-second hall, the Patina
at 0.4 -- is rendered for thirty seconds in the self test and has to be
audible at the end.

**Two small things Rene asked for while that ran.** Favourites had been kept
in the plugin state by preset index; the 2.0 library renumbered every index,
so every star pointed at a stranger. They are now a file of names in
Documents\Noctuary\favourites.txt (one a line, a name whose preset is not
installed is kept), the same in the standalone and in every DAW, and the
browser gained *favourites first* -- the starred presets to the top of the
list in whatever order the sort left them (a stable partition, so "by
brightness, favourites first" means what it says) -- remembered in the same
file, and the map rings the favourites in the star's gold. The other was the
table pictures: the installed 1.12.1 lit whole frames, and its lit cycle
jumped to the next frame while the sound blended between them; the depth view
of 11.09. already lights the blend at its fractional place, and now the
frame count says "3.4 / 8" rather than a whole number, and the flat Harmonic
picture blends with the slot's Transport, the way the engine reads the table.

**The offline render cut where the instrument crossfades**, which turned out
to be worse than it sounded. `ambient_render --journey` had one engine, so a
step applied the next preset to the engine that was playing -- and that engine
keeps its sounding voices, its conductor's cluster and its tails. Measured
between two presets forty decibels apart (Nomad Wind at -16.8 dBFS, Grid
Expanse at -59.5): after the cut the render sat at -20 dB for the rest of its
length. It had not arrived at the next preset at all; it was playing the old
preset's voices through the new preset's parameters, which is exactly the
"ninety-three switches flipped at half way" that made a preset change a
crossfade of two engines in the plugin in the first place. The render tool now
does the same as the plugin: the preset that is leaving plays on the engine it
is on while the next is built on a second engine -- reset, the live state
copied in so `--set` survives, the preset applied over it, its rooms and clips
read, the conductor's cluster and the foreground's state handed over -- and the
two are mixed under a sine/cosine pair, with the same eight-second head start
that waits for a slow attack to speak. The same journey now ends at -60 dB,
where the preset itself measures. A fade of zero is still a cut, a fade longer
than what is left to render is shortened, and a render without a journey never
builds the second engine, so every measurement of the library is the single
engine it always was (checked: four presets re-measured against the night run's
cache, identical to three decimals).

**The foreground's own sends** were the last item of Rene's routing list
(distance, dry, sends) that had not been built. *To Delay 2* and *To Cosmos*
take a share of every near event into the second delay's input and into the
Cosmos send, ADDED to what those already hear from the near bus rather than
diverted from it -- an aux send on a desk, not a routing switch, so the event
still sounds where Distance and Dry put it. Into the delay's INPUT and not onto
the plane: on the plane the event would pass through everything else a second
time. With the Cosmos' own Send at zero this puts the foreground alone into the
deep space -- the events shifted, resonated and smeared while the bed is left
dry, which is the thing the section could not do before.

That a send raises the level proves nothing by itself, so the self test asks
for the pair: open the send and shut the effect's own way back (the second
delay's Mix, the Cosmos' Return), and the level has to fall back exactly onto
the render that had no send. Measured, with every source off so the foreground
is the only thing sounding: +31.8 dB with no send, +36.5 with To Delay 2 and
+30.4 with that delay's Mix shut -- the same as +30.4 with no send and the Mix
shut; +35.9 with To Cosmos and +31.8 with the Return shut -- the base, to a
tenth of a decibel. Both default to zero, so all 108 near presets sound exactly
as they did; six of them now use one, where it was always what they wanted: the
Sonar Ping and the Echo Sounder answer themselves down the long chain, the
Krell's circuits and the Deep Space Beacon go into the Cosmos, the Number
Station and the Lost Transmission carry down the delay.

### The FFT, which was doing twice its work (13.09.2026)

With the bank's loops written out, a profiler on the dearest preset of the library put
`Fft::transform` at 2.16 seconds of processor against 0.32 for the next thing on the list -- seven
times the second item, and more than a third of the whole render. It serves the Nebula, the
spectral shifter, the Memory's chroma, the spectral source and the Room.

It is a textbook radix-2 transform and there was nothing wrong with it. What was wrong was what it
was being asked to do: every caller had just written a row of zeros into the imaginary half. A
real signal's spectrum costs a COMPLEX transform of half the length plus one rotation per bin --
the samples are read in pairs as `z[k] = x[2k] + i x[2k+1]`, and the even and odd spectra come
apart again afterwards. Going back the other way the same applies, and here it was already spelled
out in the callers: the Nebula and the spectral source build a conjugate-symmetric spectrum on
purpose, bin by bin, and then asked for a full complex inverse of it.

`RealFft` does both. Measured on the same preset and the same thirty seconds: the transform's own
time fell from 2.16 s to 1.00 s, with 0.12 s for the untangling on top. Across both rounds -- the
bank's two scalar loops and this -- the dearest preset of the library went from 19.9 % of a core
to 12.9 %, and the FFT-heavy ones with it.

Two things it is worth knowing about the implementation. The inverse needs a scratch half, because
its last step interleaves two half-length arrays into one and that shuffle cannot be done in place
by walking it in either direction: writing the pair for index k lands on the imaginary part that
index 2k - m has still to read. The round trip came back 1, 5, 3, 7, 5, 7, 7, 8 for 1 to 8 before
that was understood -- every odd sample taken from two places further on. And because the spectral
source shares one transform between every slot of every voice, the scratch can be handed in rather
than kept in the object; an instance that writes into itself and is shared is a race waiting for
the day somebody renders two voices at once. The `im` array itself serves, every write landing on
a bin the step has already read.

`Tests/FftChecks.h` holds the real transform against the complex one at seven lengths on seven
signals -- an impulse, a constant, noise, a ramp, and a single frequency at the middle bin and its
two neighbours, where k and n/2-k are the same bin and the pair formula degenerates -- then the
round trip, then the inverse against the complex inverse on a Hermitian spectrum built the way the
Nebula builds one, so the check does not only ever see spectra the forward has just made, and last
the aliasing both directions rely on.

## The production guide, built in (25.09.2026)

A guide to mixing dark ambient in the manner of Rich and Lustmord -- three planes, four rooms,
width by plane, a sub that is an instrument -- was read against the code, then against the
library. Most of what it asks for the instrument had, often in a more elaborate form than the
guide describes: one distance per note driving brightness, wet share, presence, the near-field
level difference and the elevation; three reverb tiers with modulated lines; the unmask; bass mono;
the mono guard; a dry sub added past every reverb; just intonation as the default; drifters on
every scale of time; a BS.1770 meter; nothing on the master. What it did not have was the arithmetic
that makes depth a *distance*: the difference between the planes in every cue at once. Measured on
stems of six presets before this round, the far bus stood 1 to 12 dB under the near one (the guide:
20 to 36), a source at the ear reached the far hall at the same instant as its direct sound, the
strands fanned out as wide wherever a note stood, every hall was fed the fundamentals, and the sub
sat at 65 to 123 Hz under a C3-B3 root. Six things changed, all of them parameters with the guide's
numbers as their defaults, so a preset that never heard of them plays by the guide and one that
wants the old behaviour can ask for it.

**Range** (`depth_range`, Space, 6 .. 36 dB, default 20). The level law over the plane was
1 - 0.5 d: six decibels at the horizon and linear in between. It is now exponential in d --
10^(-Range d / 20) -- so every step of the plane costs the same number of decibels and the plane's
ends are as far apart as the guide's near and far layer: at Range 20 a note at Depth 0.7 stands
14 dB under one at the ear (the guide's middle plane, -10 to -18), one on the horizon 20 under it.
The crossfade into the far bus is unchanged, so what the far reverb receives falls with the same law.
Measured through the engine: a note half way in, Range 20, -13.1 dB against one at the ear (10 from
the law, 3 from the crossfade); Range 6 against 20, 7.0 dB apart, as the arithmetic says. The
default patch's stems, before and after: the far bus from +0.8 dB over the near bus to -3.2 under
it, with the conductor's notes at Depth 0.7 as before -- the far bus still carries the near notes'
own send, which is what a room does.

**Gap** (`depth_predelay`, Space, 0 .. 80 ms, default 40). The pre-delay had belonged to the halls
-- the near room's fixed at 5 ms, the far hall's a knob the library drew at 20 to 200 ms -- which is
the cue upside down: the gap between a sound and its room says how far the source stands from the
walls, so it belongs to the source and shrinks with its distance. Each voice now delays its far
send by Gap x (1 - d) through a ring of its own (80 ms at the rate, on the heap, so an engine on a
test's stack stays the size it was), glided per sample as the plane breathes, and the far hall's
own pre-delay defaults to 3 ms: a source on the horizon has no gap, one at the ear has Gap. Measured
as the lag of the far bus against the near bus at Depth 0.5 with Gap 40: 20.0 ms, the two buses
carrying the note's energy equally. The library's `far_predelay` was rewritten to 3 ms in all 14591
presets (`Tools/library/retrofit_guide.py`); it had said 20 to 200 in every one, which with the gap
on top would have given the horizon a pre-delay of its own.

**Near Width** (`depth_width`, Space, 0 .. 1, default 0.35). Width by plane is the guide's second
strongest cue after level -- the near layer placeable, correlation 0.5 to 0.9; the far layer a
surround, under 0.2 -- and nothing in the voice read the distance for it. The strands' fan and the
Partial Spread are now Near Width of themselves at the ear and all of themselves on the horizon.
Far Width on the far reverb stays: a preset that wants the funnel (the background pulled in, R6)
still has it, on the tail rather than on the source.

**Send Low Cut** (`send_lowcut`, Far Reverb, 20 .. 400 Hz, default 150). The guide's first rule of
reverb: a filter before every send, 150 to 300 Hz, because fundamentals fed into a hall come back as
a low-frequency mass that no filter on the return takes out again -- by then they have recirculated
for the length of the decay. Every reverb input had a 5 Hz DC blocker and nothing else, and the
three Low Cut knobs sit on the tails; in the library, none of 14591 presets had a Low Cut at 150 Hz
or above. One second-order Butterworth now sits on the wet path of the near room, the far hall and
the convolution room's send, after the DC blocker and before the pre-delay. Measured: a 50 Hz tone
into the far hall, 100 % wet, comes back 19.1 dB lower with the cut at 150 Hz than with it off.
The default patch's far stem, 63 Hz octave band: from +2 dB against the loudest band to -19.

**The Foundation**: Octave -2 by default (33 .. 62 Hz under a C3-B3 root; the guide's sub lives at
25 to 70, under the drone's fundamental rather than inside it -- only 257 of 9043 sub presets had
asked for it, 255 had asked for -1 and were rewritten); `sub_harmonics` (Foundation, default 0.6),
the second and third partial at -24 and -27 dB at full, made from the sub's own phase so they alias
nothing, because a speaker that cannot move at 40 Hz still gives the ear the note from them (the
residue pitch); `sub_beat` (Foundation, 0 .. 1 Hz, default 0), a second sine Beat Hz above the
first in both ears alike, so the pair swells and fades once every 1/Beat seconds -- the slow breath
of a Lustmord sub, a beat on the basilar membrane where Binaural is one between the ears and gone in
mono. Measured: 55 Hz under an A root by default, the partials at -24.0 and -26.8 dB, the level
swinging 0.3 dB over half seconds without Beat and more than 10 dB at 0.5 Hz.

**The near events at the guide's pace.** The bank's presets had been written with an event every
40 to 720 seconds (median 300) and Near Auto's factor on top; the guide wants the ear's reference
point for "near" every 20 to 90 seconds. `Tools/make_layer_presets.py` maps the designed span onto
the guide's (40 -> 20, 720 -> 90, the ordering kept), and an arrival or departure of an event that
already holds for half a minute takes at least a minute, the event lengthened to what its share of
approach needs, up to 300 s; the longest approach in the bank had been 75 s, now thirty of them are
60 s and more. Nine-second flute notes keep their design: a note is a note, not a journey.

**Measuring by the guide.** `measure_packs.py` renders with `--loudness` as well, the renderer's
meter started after the warm-up (`--skip` used to leave it counting the climb), and holds every
preset to -24 .. -18 LUFS integrated instead of the unweighted -30 .. -17 dBFS RMS window -- the
target this document has quoted since the meter was built, K-weighted so a bass-heavy bed and a
bright one are held to the same heard level. The gates a gain cannot fix -- crest under 12 dB, mono
loss over 3 dB, true peak over -1 dBTP -- are counted and written to `guide-report.json` beside the
cache; `preset_check.py` fails them, and its mono limit went from 6 dB to the guide's 3.

**What the library needs now.** Every preset's loudness has moved: the background stands lower, the
halls carry no fundamentals, the sub sits an octave down. The tenth meta token and `master_gain`
of every pack are from before this round and `rebuild_all.py` from step 4 on brings them back into
the window. The 256 built-ins, measured before that re-measurement under the pipeline's own
conditions (60 s after a 45 s warm-up, notes 45/52/59, brain rate 6, hour 21), before and after:

```
                                            before   engine  presets   (engine: the six parameters; presets: guide.py over the built-ins as well)
LUFS integrated, median                      -25.3    -26.2    -26.7
crest (true peak over short-term), median     11.9     11.9     12.1   presets at 12 dB and over: 122 -> 122 -> 134 of 256
mono loss, median                             2.2      2.2      2.1    presets under 3 dB: 192 -> 194 -> 209
|L/R correlation| of the mix, median          0.22     0.23     0.24   presets in 0.3 .. 0.7: 89 -> 93 -> 100
63 Hz octave band under the loudest band      -8.5    -10.1    -10.7   the halls' low end, then the pads' low cut
31.5 Hz octave band under the loudest        -19.9    -14.6    -15.7   the sub an octave down
wet share of the energy, median               0.24     0.19     0.21   the background 14 dB under, per note
true peak, loudest preset                     -2.6     -3.9     -3.3
```

The whole-mix figures move little, as they should: the mix is the near bus, and what changed is
the far bus under it. The gates the pipeline now counts moved the right way with the presets' pass
(crest, mono loss and the mix's correlation each gained a tenth of the library); the sub against the
250-500 Hz band did not (-9.4 -> -10.3 dB median, three presets in the guide's +3..+6), because that
is a level the generator draws and the measurement re-levels, not a window a parameter can hold --
the pipeline's next step is a band-measured sub balance in rebalance_sub.py, which today compares
knob values. The default patch's stems say it: far against near from +0.8 dB to -3.2, the
far stem's 63 Hz band from +2 dB against the mix's loudest band to -19, the near stem's correlation
from 0.44 to 0.61 (the strands a third as wide at the ear) with the far stem's at 0.08. The
crest and the correlation of the mix are what the re-measurement and the library's own generator
have to move next; the engine now carries the cues they need.

**The presets, moved into the guide's windows (25.09.2026, later the same day).** The engine's
defaults reach every preset that never names a key; what the generator had written stood in the
way in twenty places, and one table now holds the guide's window for each of them
(`Tools/library/guide.py`), applied to every preset the generator makes and, by
`Tools/library/retrofit_guide.py`, to the 14591 that existed: `depth` under 0.85 mapped from the
library's 0.4 .. 0.85 onto 0.85 .. 0.9625 so the conductor's deep notes reach the horizon (median
0.71 before, 0.91 after; the pass is idempotent, which its first version was not -- the packs were
mapped twice before that was noticed and restored from the commit before);
`far_highcut` capped at 3 kHz, `far_decay` held to 12 .. 40 s, `near_decay` to 0.6 .. 1.5 s;
`far_width` never under 1 (the funnel of R6 is now Near Width's, on the source); `presence` at
least 1 dB; `pad_low_cut` at 70 .. 90 Hz, `bass_mono` at least 100 Hz, `subsonic` 18 Hz;
`purity` at least 0.9 wherever the scale is not 12-TET; `detune` at most 4 cents and
`beat_ceiling` 0.5 Hz, so the strands are a warmth and not a cloud; the master's `width` at most
1.3 (Air was tried as a floor of 0.08 and taken out again the same hour: the guide's one quiet air
layer is a mix decision, and Air here is band noise per voice, which sixteen voices turn into a floor); the room's pre-delay 10 .. 25 ms and its low-pass at
most 6 kHz; texture grains at least 80 ms with at most 5 % of position spread, cloud grains at least
80 ms; the sub two octaves down and mono -- a Binaural offset without Pulse becomes a Beat of
0.25 Hz in both ears, the offset under a Pulse is kept, since that is the isochronic design of the
Foundation and not a mixing mistake; and at most one LFO on a bar division, the others freed onto
their own rates. Counted over the packs afterwards: every window at 100 % except `depth` at or over
0.9 (the rest between 0.85 and 0.9 by the mapping), `purity` (85 %, the rest the 12-TET presets)
and the sub's mono (93 % of the sub presets; the rest carry a Pulse). The generator's own ranges in `styles.py` were left; the table is the gate.

**What was left as it was, and why.** Far Width was left as a knob but every preset now stands at 1
or wider -- the guide wants the far layer widest, and with Near Width the near plane is narrower than
the far one by default whatever Far Width says. The micro-motion amounts (5 cents of drift, 0.7 octaves of filter drift) are larger
than the guide's, deliberately: they are what the R-rounds measured as alive. Oversampling of the
nonlinear stages, a correlation meter in the panel, a mid high-pass on the returns and the spectral
ducking in six to eight bands are noted as open -- and were built the next night, below.

## The guide's second round, and the conductor against ambient harmony (25./26.09.2026)

Two documents of the same day: the production guide, whose first round is above, and a review of
the Cluster Brain against the harmony of the genre ("Review: Cluster Brain vs.
Ambient-Harmonielehre"). The first round had left four of the guide's points open and found two
more; the review found eight places where a model of tonal music psychology -- major and minor,
twelve equal steps, the octave, interval classes -- was set on modal, just, register-bound drone
music. Both rounds are in the engine, and the library was measured once for all of it.

**The rooms, serial** (`room_to_far`, Room, default 0.15). The guide's main room feeds its far room
at ten to twenty per cent, so the horizon sounds like the same place going on rather than a second,
foreign one -- the trick behind Lustmord's "endless" rooms. The convolution room is now processed
ahead of the far hall (its input was taken there already, so what it hears is unchanged) and its
return, before it is ducked, goes into the hall's input. Subtle by design: at 0.3 the far stem of
the default patch with the room at 0.6 is 0.19 dB louder, a continuation and not a second hall.

**The far return in mid and side** (`far_mid_lowcut`, Far Reverb, default 300 Hz). The guide lets
the far room's sides be as wide as they like and high-passes its middle from 300 Hz, so the centre
below that belongs to the near plane and the sub. A second-order high-pass on the mid of the far
bus after the hall; measured, the far return's mid at 110 Hz 17.4 dB down and its side untouched.

**Ducking in seven bands, on both rooms.** Unmask had three bands on one-pole crossovers; the guide
asks for six to eight. Now seven, an octave apart from 150 Hz to 4.8 kHz, on second-order
crossovers whose bands are differences of neighbouring low-passes -- so they still add back to the
input exactly -- a 20 ms attack and the guide's half second of return (the Return default, 1.2 s
before). The one-poles were too gentle for seven bands: a 2 kHz foreground ducked the background
2.4 dB at 2.1 kHz and 1.0 dB at 200 Hz; on the steeper crossovers it is 5.8 and 0.6 (Unmask 0.3,
foreground at -20 dBFS). The engine runs a second Unmask on the convolution room's return, on the
same knob -- the guide's "the near bus ducks the main room". The guide's depth is two to four
decibels, a tenth to a fifth of the knob; the library's 0.2 .. 0.44 was mapped onto 0.1 .. 0.2 with
its ordering kept (guide.py).

**The Foundation's ceiling** (`sub_ceiling`, Foundation, default -6 dBFS). A beating sub swings its
peaks by up to six decibels, and summed straight into the master it drove the soft clipper, which
pumped everything else with it. The sub now has its own limiter before the sum, its ceiling in
dBFS at the output (the master gain taken out of it), instant to hold a peak, 5 ms to act, half a
second to let go. Measured with Beat 0.5 Hz and the master at +6 dB: the sub peaks at -6.0 dBFS
under the default ceiling and at -2.8 with the ceiling at 0.

**Four times the rate on everything nonlinear.** Oversample.h: two linear-phase half-band stages
(51 and 19 taps, 80 dB, flat to 18 kHz, 29.5 samples of latency). On the filter's drive, the
wavefolder, the Patina's tape, the air ahead of the far hall and the feedback loop's saturation.
A clipped 9 kHz sine aliases 31.7 dB less; the driven filter's fold-back stands 55.5 dB under its
fundamental. Three are left at the base rate, each for a reason: the master's soft clip (its third
harmonic stands 40 dB under a peak at the library's levels, and 0.6 ms on every output would move
the output off its stems), the ladder's saturation (it is inside the filter's own feedback loop,
which would have to run at four times the rate as a whole), and the near radio (its input is
band-limited to 3 kHz before the cubic, which then makes nothing above 9 kHz). Where Filter
Parallel mixes the dry sum with the driven filter, the dry sum is delayed by the same 29.5 samples.
The Memory's loop and the Cloud already had antiderivative antialiasing.

**A correlation meter.** The loudness meter measures left against right over three seconds and
since its reset; the panel shows it beside the true peak and the crest and turns amber under
nought, and the renderer's loudness line carries the mean (`corr=`). The guide wants the master
between 0.3 and 0.7.

**The harmony review, in the engine.**

- F5 -- the consonance is continuous. intervalConsonance() was the Tenney height of the simplest
  ratio within ten cents, a step function: a tempered major third, fourteen cents from 5/4, fell
  from 0.19 to 0.11 and was judged as rough as a semitone. It is harmonic entropy now (Erlich; a
  17-cent Gaussian over the ratios with n x d <= 10000, weighted 1/sqrt(n d)), a table of one value
  per cent written by Tools/make_harmonic_entropy.py, mapped onto the old scale: the fifth 0.30,
  the fourth 0.21, 5/4 0.17, 9/8 0.13, 16/15 0.11 -- and the tempered third 0.15.
- F1 -- the key is the scale's own. The Krumhansl-Kessler profiles measure major and minor cadences
  and put the third above the fourth and the second; on a just scale the profile is now made of the
  scale -- each degree's consonance with the tonic, spread to the listeners' range (the fifth 0.8,
  the fourth 0.64, a third about 0.55, a bin the scale does not reach 0.35) -- and Thirds and
  Seconds tilt it. On Harmonic 8-16 the key is the overtone series (7/4 at 0.52, the minor third
  the scale lacks at 0.35); 12-TET keeps Krumhansl-Kessler. With Key up on the default JI 7-limit
  the key's confidence rises from 0.62 to 0.85 (a first mapping, 1 + 0.2 ln c, left the profile
  without shape: 0.68 against 0.66).
- F2a -- the prime limit. Harmonicity weights a harmonic holding the prime five by 1 + 0.8 Thirds
  under nought (a fifth at Thirds -1) and one holding seven by 1 + 0.5 Seventh: under Thirds -1 the
  just major triad keeps no more than its open fifth's share.
- F2b -- Utonal (`brain_utonal`): the mirror measure, every tone an undertone of a common top; with
  Utonal up Harmonic takes the larger of the two. A minor triad is 0.391 utonal, 0.377 harmonic.
- F3 -- adaptive intonation (`purity_adapt` 0.4 .. 0.8) wherever the root moves: a range, below.
- F4 -- distances, not interval classes. The tritone veto is for the close tritone (and the one an
  octave up in the bass); a second's colour fades by half with every octave between its notes, and
  sought it counts an octave apart in any register -- dissonance by separating registers; the
  ceiling of two notes an octave is a window of twelve semitones. An hour under Low Spacing 1:
  no close tritone below MIDI 55, 336 spread over an octave and more.
- F6 -- other tunings. On a scale that does not repeat at the octave, Key, Even and the
  constellation memory are off and no key is shown; on a keyboard that walks the scale degree by
  degree every rule reads its distances from the frequencies. Half an hour of Bohlen-Pierce: 341
  onsets, nothing closer than a minor third below MIDI 60.
- F7 -- the deep family's background register from MIDI 20 (26 Hz): a range, below.
- F8 -- Root Targets (`brain_root_targets`): Classic, the six intervals alike; Modal (the default:
  the fifth 40 %, the fourth 30 %, the whole tone up or down 25 %); Mediant; Phrygian (the falling
  semitone a fifth of the time). Four two-hour runs: whole-tone root steps 0 of 112 under Classic,
  25 of 112 under Modal.
- Section 4: Series (`brain_series`), the Harmonic Cloud -- candidates drawn to the harmonics of the
  root's fundamental under 40 Hz, 1/sqrt(h) above the eighth, the prime limit of Thirds and
  Seventh; on JI 7-limit 46 % of the notes land on harmonics 1..16 without it, 87 % with it. The
  difference tone: under Low Spacing the two lowest voices under MIDI 48 must make one of 25 Hz or
  more that stands on the scale (an hour: 0 of 92 pairs under 25 Hz). The tension arc: Arc Harmony
  by family, a range.

**The ranges** (Tools/library/review.py, applied to every preset the generator writes and by
retrofit_review.py to the 14591 that exist, each by its family): purity_adapt 0.4 .. 0.8 where the
root moves; the deep family's Brain 2 register 20 .. 24 to 36 .. 40 MIDI; Root Targets Modal, but
Mediant for luminous and Phrygian for ritual; Utonal 0.6 .. 0.9 deep, 0.4 .. 0.7 ritual, 0.3 .. 0.6
cold, a little elsewhere; Series 0.2 .. 0.5 for sleep and space; Arc Harmony 0.1 .. 0.5 by family.
Fuzzy: a value is drawn inside its window from a hash of the preset's name, and a value already
inside stays. conductor_ranges.md and .json carry the same rows.

**The library, measured once.** The loudness window is the guide's for dark ambient now, -20 ..
-16 LUFS (it was -24 .. -18 in the first round), a lift never goes past -1 dBTP, and two of the
guide's targets that are no parameter are pulled towards from the measurement (guide_fit.py): the
sub three to six decibels over 250 and 500 Hz, by Sub Level, and the correlation between 0.3 and
0.7, by the master's Width inside its cap of 1.3 -- sixty per cent of the way to the nearer edge,
jittered by the preset's name, so the library spreads through the windows instead of stacking on
their edges. Two passes, so every preset gets its gain once: measured without gain, fitted, and
the fitted ones measured again with the gain for all. The built-ins go through the same mill as a
pack (builtin_pack.py writes Presets.cpp's rows out and the measured gain, Sub Level and Width back
in -- NOT rebuild_all's step 5, whose staging packs of 13.09 would have undone the first round).

How it went, for the record. The first fit moved sixty per cent of the way to the nearer edge and
was undone before it was measured: it would have left the median correlation at 0.24 and the sub at
+0.6 dB, still outside. Moved into the windows instead, the second pass showed the fit's blind spot
-- it had lifted subs and widths at once, and a sub is mono, so 3162 presets it had brought into
the correlation's window came out over 0.7 -- and a second fit, from that measurement and with the
sub's share in the arithmetic, moved 5603 presets once more (a third pass, its gain corrected for
exactly those, so nothing was lifted twice). The lift the loudness window may make went from six
decibels to twelve: at the old gains the new engine plays the packs at a median of -28.1 LUFS,
about eight under where they were, and at six most of the library would have stayed out of reach.

```
                                   packs (14336)              built-ins (255)
                                   before       after         before      after
loudness -20 .. -16 LUFS           (-28.1 med)  12215         --          232
true peak <= -1 dBTP               14329        14335         255         255
correlation 0.3 .. 0.7             4817         12487         82          234
mono loss <= 3 dB                  11534        14334         190         255
crest >= 12 dB                     9494         10520         138         156
sub 3 .. 6 dB over 250-500 Hz      741 of 9043  5792 of 9043  5 of 137    85 of 137   (presets with a sub)
```

Under the window stay the 2118 quietest packs, which needed more than twelve decibels (the ones the
cap is there for); 1072 subs that stand under their window already play at Sub Level 1. The crest
moved least, as expected: it is a property of the material -- a bed without transients -- and no
knob of this round was aimed at it. The loudness range is under 8 LU for nearly every preset, which
over one measured minute of a drone says nothing: the guide's 8 to 20 LU are for a whole piece.

## Circuit filters from Ephemeris (26.09.2026)

Rene: "Kannst du dir vielleicht mal die Filtermodelle im BerlinSchool-Generator anschauen. Wäre das
vielleicht auch was für Noctuary?" Ephemeris had, the same day, got ten filter models that are the
circuits of classic instruments rather than textbook filters. Four of them suit a drone instrument
and came over; the Polivoks, the Wasp and the MS-20 are built to scream and stayed behind, and a comb
was there already.

**The five new models** (`filter_model`, appended after Formant so every existing preset keeps its
model): **Moog** (the transistor ladder, tanh in every stage, Huovilainen), **SEM** (Oberheim's
state-variable filter with saturating integrators, and a new knob, **Morph** -- `filter_morph`,
Filter, 0 .. 1 -- from low pass through notch to high pass), **Prophet** and **Juno** (the OTA
cascades of the SSM2040 and the IR3109, the Prophet's feedback saturating before its stages) and
**Diode** (the diode ladder of the EMS and the TB-303, its last capacitor half the size, after
Stinchcombe). `Core/include/ambient/CircuitFilter.h` holds them, as templates like in Ephemeris.

What makes them different from the old Ladder is not the curve but the loop. The old one breaks the
path through its resonance with a sample of delay, and that sample detunes it: its corner has to sit
1.55 times over Cutoff and its resonance drifts with the cutoff. The circuit models solve the loop
exactly every sample, three Newton-Raphson steps on the circuit's own Jacobian, so their resonance
rings on Cutoff -- after an impulse at 440 Hz and full Resonance all four ladders and cascades ring
within 1.4 per cent of it, and Prophet, Juno and Diode go on singing by themselves, within 2.3 per
cent (the Moog is voiced a hair under the threshold, as in Ephemeris, and rings out). With Key Track 1 the ringing
plays the note. Drive on these five is the level into the circuit, whose own stages saturate, not a
clip ahead of it.

**What they cost, and what was done about it.** As first ported -- scalar, one channel after the
other, at four times the rate as the guide asks of everything nonlinear -- a circuit model cost 2500
to 3000 cycles a stereo sample against 32 for LP 12, and "Consonant Expanse" went from 11 times
realtime to 2.4. Measured one by one:

* **Twice the rate is enough.** A circuit is not a bare curve: the stages after each saturation
  low-pass what it makes. A full-scale sine into a fully driven Moog at a 5 kHz cutoff aliases at
  -75 dB at the base rate and at -101 dB, the measurement's floor, at twice the rate; four times
  measured no better in any case tried (what is left at 12 and 18 kHz cutoffs is the same at two and
  four times: the half-band's transition above 19 kHz). Ephemeris runs them at twice the rate too.
  `StereoOversampler2` is the first half-band stage of Oversampler4 alone, 25 samples late.
* **Three Newton steps.** Warm-started from the last sample, two leave an error under -128 dB of the
  signal for quiet and pushed inputs, but -33 to -52 dB with the cutoff swept to 21 kHz at a
  resonance of 0.8; three leave -72 to -103 dB there.
* **Both channels in one register.** The Newton steps are chains of dependent divisions: a second
  lane rides along for nothing. The Jacobian's diagonal is inverted as soon as it is known, off the
  chain of the substitution.
* **The oversampler's sums as vector sums.** Its 26-tap sums were added one after the other. The
  first vector version came out twice as slow -- a wide load over a history whose newest sample has
  just been stored waits for that store -- so the histories are Lines now: the newest sample is
  multiplied on its own, the vector reads only older ones, and the line jumps back to the top of its
  buffer every 64 samples instead of wrapping. 197 cycles for a stereo round trip at four times
  became 96, which every Drive, fold, Patina, air and feedback stage in the instrument gets as well
  (LP 12 with Drive: 237 cycles a stereo sample before, 147 now).

Now: Moog 470 cycles a stereo sample, SEM 520, Prophet and Juno 600, Diode 730, and "Consonant
Expanse" plays at 13.1 times realtime on LP 12 and 5 to 7 times on the circuit models -- the dearest
filters in the instrument, about as dear as the rest of a voice together, and there only where a
preset asks for one. The old Ladder stays in the menu (45 cycles).

**The library moved** (Rene, after the A/B renders: "Ja, bitte umziehen. Bitte auch noch andere
Presets auf die anderen neuen Filter umstellen, auch aus den Packs (aber nur die, die bislang nicht
ausschließlich den z-Filter benutzen). Ich denke, das Neuvermessen können wir uns trotzdem sparen.")
`Tools/library/circuit.py` is the rule, applied like guide.py and review.py -- by make_presets.py to
every preset it generates, by make_layer_presets.py to the near bank, and by retrofit_circuit.py to
what exists: every Ladder to the Moog (at 1.55 times the Cutoff and the Resonance squared, where the
old ladder had its corner and its feedback), seven in ten LP 24 to a four-pole circuit by the family
(cold to the diode ladder, deep and ritual to the Moog, luminous and sleep to the Juno, the rest
between Prophet and Juno), six in ten Notch and one in two HP 12 to the SEM at Morph 0.5 and 1, one
in five LP 12 to the SEM -- never a preset whose voice filter is not heard (a Replace z-plane, a
parallel one at a Mix of 0.95 and more). The SEM takes the state-variable filter's Q; a four-pole
circuit is fitted to the LP 24's small-signal curve. 3583 presets moved: 3510 of the packs (950
Ladder, 521 LP 24, 198 Notch, 135 HP 12, 1706 LP 12), 63 built-ins, 10 of the near bank.

Not measured again, as asked -- but not unchecked either. A reckoning of what each circuit's pass
band gives up held for the SEM and the cascades and not for the diode ladder (-8 to +4 dB on a
sample), and the old Ladder turned out to have been measured wrongly: its sample of delay turns the
loop positive at Nyquist, and with its corner pushed near the top by the envelope or key tracking it
oscillated at 24 kHz at full scale. Inaudible, and in 15 presets the loudest thing in them: the
library's measurement had turned them down for it, "Envelope Hollow" by sixteen decibels (its
measured spectral centroid was 23.9 kHz). So every move but LP 12 to the SEM was rendered on both
filters, 30 seconds at the preset's old level (1832 presets, `Tools/library/circuit_measured.json`),
and each preset's master_gain corrected by what it measured, as far as its true peak allowed:
Ladder to Moog median +0.7 dB (-19.4 .. +8.9), LP 24 to the circuits -0.7 (-7.3 .. 0), HP 12 and
Notch to the SEM 0.0 (-0.4 .. +1.3). Against the library's own measurement, 96 moved presets
rendered exactly as it was made (the warm-up by the attack, then 60 s) came out at a median of
0.0 dB, every one within 2 dB (the farthest +1.6). A first check without the warm-up had put
"Silo Chamber" 5.6 dB under its measurement; measured as the library is measured it is at -20.05
LUFS, where the measurement has it -- a preset with a long attack is quieter while it arrives. The old
Ladder's loop is held under a gain of 0.8 at Nyquist now (its corner stops near 9 kHz on the knob
at the top of Resonance), and `testCircuitFilters` holds it there.

What the move leaves stale: the map, the groups and the CLAP embeddings are the last measurement's,
and for the fifteen Ladder presets that were a 24 kHz tone to it, their place on the map is that
tone's. The next measurement of the library puts them right.

The Quest gets the same code: F4 is NEON on arm64, and `ambient_filtertest` (Tests/, 26.09.2026)
holds every vector path to the plain float one -- built as the core is here (SSE lanes, AVX2 sums),
through the NEON shim on x86, scalar, and for arm64 in build-android, where it compiles with the
APK. The lanes differ from one float by 1e-6 of the peak with the desktop's FMA and not at all
through the shim; the Prophet sings at 432 Hz and the SEM's notch is -61 dB on every path.

The parallel filter route (`z_route`) delays its dry branch by the filter's latency, which the filter
now reports as a number (`VoiceFilter::latency()`: 29.5 samples with Drive, 25 for a circuit model,
0 otherwise). `testCircuitFilters` holds the ringing, the bounds when pushed (the cascades let a loud
input through at up to 3.3 times its peak at full Drive and Resonance, as their saturating feedback
gives the pass band back; Ephemeris tests 6.4), the SEM's notch (-61 dB on Cutoff at Morph 0.5) and
the latency; `testFilterModels` holds the small-signal curves the display draws.

## Roadmap

1. **Sound** — done since v0.2: spectral freeze (Nebula), head-shadow
   low-pass on the far ear (20 kHz → 3 kHz at full lateral position, scaled
   by *Time Width*), user preset files, a second delay in series, the
   granular cloud, independent Sound/Cosmos preset layers. Open: per-preset
   random seeds, a "morph" between two full presets over minutes, MIDI
   learn for the standalone.
2. **Performance** — SIMD across partials is done (`Core/include/ambient/Simd.h`,
   AVX2 with a scalar path): the median preset went from about 39× to 52×
   realtime on one core, the slowest from 17× to 17×.

   **Voice rendering in parallel was measured and dropped, deliberately.** The
   voices are rendered per control block of 64 samples with the conductor
   interleaved between blocks; the work per block is a few microseconds, which
   is the same order as the cost of synchronising two threads, so splitting the
   voices there would buy nothing. Making it worthwhile would mean rendering a
   whole chunk per voice group, which moves the conductor's decisions from a
   1.3 ms grid to a 10 ms one and changes every render. At 52× realtime in the
   median and 17× in the worst case, on one core, there is nothing to buy: the
   instrument is not short of time. If a future feature changes that (many more
   sources per voice, say), this is the note that says what to do and what it
   would cost.
3. **Quest** — CMake toolchain for the Android NDK (arm64-v8a), Oboe for
   low-latency audio, OpenXR for hands and head; the visual layer is a
   separate concern and can reuse the Kaleidoscope engine's ideas (calm
   motion, no camera shake). `Core/` is expected to compile unchanged; the
   `Engine::soundingNotes` / `noteDistance` / `arcValue` observers already
   exist for a visualisation to read, and the distance model maps directly
   onto placing sound sources in a 3D scene.
