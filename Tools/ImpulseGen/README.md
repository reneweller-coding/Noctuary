# ImpulseGen — impulse responses for the Room

The Room is Noctuary's third reverb: a convolution reverb on the far plane
that plays an impulse response (BACKGROUND → Room). Without a file it uses a
built-in dark hall; this tool makes files for it. Same Python environment as
TextureGen:

```
..\TextureGen\.venv\Scripts\python impulsegen.py
```

## Four ways to an impulse

* **Design** — a synthetic room from numbers: RT60 per band (low below
  300 Hz, mid, high above 3 kHz — Rich's rooms keep the low end ringing
  longest), *Size* (spacing of the early reflections), *Pre-delay*, *Width*,
  *Tone*, *Modulation* (a slow wobble in the tail, less static). Eight room
  presets: dark cathedral, stone chapel, deep cave, hangar, dark chamber,
  glass room, infinite plate, night field.
* **Recording** — any WAV becomes an impulse: onset detection, trim, cut
  where the tail falls below the floor, fades; *extend* continues a tail that
  the recording cut off, with the recording's own level as the starting point.
* **Prompt** — a TextureGen model (Stable Audio Open works best) renders
  "a single sharp hand clap in <your room>", and the result is cut like a
  recording. Text-to-audio models do not know what an impulse response is,
  but they know what a clap in a cathedral sounds like, and that is one.
* **Hybrid** — the average spectrum of a recording colours noise, the
  designed per-band decay shapes it: the character of a place with a decay
  you choose.

The view shows the impulse and its energy decay curve; *Play chord through
it* convolves a short dry chord for a quick impression. Export writes
`Impulses/<name>.wav` (stereo float) plus a `.txt` with the numbers. The synth
normalises every impulse's energy, so rooms do not change the level.

## Command line

```
..\TextureGen\.venv\Scripts\python impulsegen_cli.py presets
..\TextureGen\.venv\Scripts\python impulsegen_cli.py procedural --name cavern --seconds 7 --rt60 7 4 1 --size 2 --tone -0.7 --seeds 3
..\TextureGen\.venv\Scripts\python impulsegen_cli.py audio ..\..\Textures\*.wav --extend
..\TextureGen\.venv\Scripts\python impulsegen_cli.py prompt --prompts rooms.txt --model sao --seconds 20 --extend
..\TextureGen\.venv\Scripts\python impulsegen_cli.py hybrid recording.wav --rt60 6 3 1
```

`ambient_render --ir file.wav` loads an impulse offline; the Quest app picks
up `impulse.wav` from its data folder.

## RoomGen: the library's rooms (`roomgen.py`)

What real late reverberation is, measured across hundreds of rooms, is
Gaussian noise whose every frequency band decays exponentially at its own rate
(Polack 1993, Jot 1992, Traer and McDermott 2016). RoomGen makes exactly that,
at 48 kHz and up to a minute long, from one design per file:

* **decay** — T60 anchored at 500 Hz–1 kHz; 63, 125 and 250 Hz as ratios of
  it, never above 1.1 × (no mud); above 1 kHz the walls take more and the air
  takes what ISO 9613-1 says at the design's temperature and humidity.
* **colour** — three knobs (slope below 500 Hz, slope above 1 kHz, a sub
  shelf) solved by bounded least squares for three long-term targets measured
  rooms have: energy below 150 Hz, the share 150–500 Hz, the power centroid;
  the first 300 ms stay under 4 kHz. A target the knobs cannot reach is a
  redraw, never a silent clamp.
* **stereo** — the ears' coherence per frequency: half-coherent in the bass,
  independent above it.

| family | files | what | T60 mid |
|---|---|---|---|
| chamber | 90 | small room | 1.2–2.5 s |
| hall | 150 | concert hall | 2.5–6 s |
| cathedral | 140 | stone nave | 5–12 s |
| cavern | 80 | wet, cold, dark | 6–16 s |
| vast | 70 | tanks, silos, the impossible | 15–30 s |
| plate | 90 | dense, bright, flat bass | 1.8–5 s |
| bloom | 90 | a small room coupled to a huge one: dense start, long quiet hang | 1–3 s into 8–25 s |
| far | 60 | the source in the huge room: the sound fades in | 8–25 s |
| drift | 120 | a colour gliding down while the room dies | 4–12 s |
| swell | 110 | every band rising towards a release | 2–8 s |

Names ending in `a`/`b` are pairs on one seed and one noise: **grow** (b is the
same room larger) or **darken** (b the same room darker). The Room Morph is a
linear crossfade, so two independent noises would lose 3 dB half way; a pair
stays within 0.1 dB. Pack presets name the partner in field 9 (impulse B);
`make_presets.py` picks it.

Every file passes the design's acceptance tests before it is written, or it is
redrawn: octave T30 against the design (±5 %, ±10 % at 63–125 Hz), bass and
treble ratios, a straight decay, echo density, no fixed pitches (peakiness,
spectral lines, a note spread measured against what a pitchless decay of that
length gives by chance), colour, coherence, envelope against the design, a
clean end. The `.json` beside each WAV holds the design, the solved knobs, the
seed and every measurement.

```
..\TextureGen\.venv\Scripts\python roomgen.py generate --out DIR [--count 1000] [--families hall,vast] [--jobs 12]
..\TextureGen\.venv\Scripts\python roomgen.py check DIR\hall_012a.wav
..\TextureGen\.venv\Scripts\python roomgen.py default --wav built_in.wav   (ambient_render --write-default-ir built_in.wav)
..\TextureGen\.venv\Scripts\python roomgen.py selftest
```

Deterministic: the same `--tag` gives the same files. It never writes a name
the released packs used (`Tools/library/legacy_impulses.json`); those files stay
as they are, because a session keeps the path of its room. The built-in hall
(`Convolver::makeDefaultImpulse`) is a RoomGen design baked into octave values,
and `default` checks the engine's output against it.
