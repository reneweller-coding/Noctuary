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
