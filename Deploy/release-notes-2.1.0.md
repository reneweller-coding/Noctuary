**Five analogue filters modelled on their circuits, a production guide built into the engine, and a
conductor held to the harmony of the genre -- and every preset measured again for it.**

## Five circuit filters

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

## The library moved to them

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

## The production guide, built in

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

## The conductor and the harmony of the genre

A review of the Cluster Brain against modal, just, register-bound drone music found eight places
where a model of major-minor tonality was doing the listening. All of them are changed:
consonance is **harmonic entropy** now; the key is judged by a profile made from the active
**scale** (12-TET keeps Krumhansl-Kessler); Thirds and Seventh set a **prime limit**; **Utonal**
lets undertone sets count as rooted; **Root Targets** chooses where the root steps (Modal by
default, with the whole tone the genre moves by; Mediant; Phrygian; Classic); **Series** draws
chords from the harmonics of one unsounded fundamental (the Harmonic Cloud); scales without an
octave get rules in cents. The packs got these ranges family by family.

## Measured again

Every preset of the library was measured again for the guide and the review: loudness in the
guide's window of -20 to -16 LUFS for 12215 of the pack presets, the correlation between 0.3 and
0.7 for 12487 (4817 before), mono loss of 3 dB or less for 14334 (11534), and the sub three to six
decibels over the low mids for 5792 of the 9043 presets that have one (741). The map, the groups and
the sound search were made again from
that measurement (the filter round above came after it and was not measured into the map).

## Checked

The self test, the host test and the race test in the release configuration (Intel oneAPI);
the new filter test on every vector path -- as the desktop is built, through the NEON path on the
x86 shim, and scalar -- and compiled for the Quest's arm64. The sample library is that of 2.0.0.
