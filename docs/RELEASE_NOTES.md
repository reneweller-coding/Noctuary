# Noctuary release notes

## 2.6.0 (16.10.2026, not released)

**Inside: one conductor for the family** (20.11.2026). The conductor, `ClusterBrain.h`, lived three times -- in
Noctuary, Empyrean and Zodiacal -- and every fix of 19. and 20.11. had to be carried into the other copies by hand,
with six port scripts; Zodiacal still has none of them. There is now one canonical file, `Tools/family/ClusterBrain.h.in`
in this repository, and `Tools/family/sync_brain.py` writes each instrument's copy from it (only the namespace
differs). What one instrument needs and the others do not stays in the one file as a knob the others leave at rest:
Empyrean's Follow Light (`BrainParams::m_light`) and its countdown (`untilNext()`) are now in Noctuary's copy too,
unused. Noctuary's conductor plays exactly as before: the music audit's 200 presets give the same 10852 onsets, 10066
notes, 87 short notes and 3 strikes again, preset by preset. A new ctest, `brain_in_sync`, fails when the checked-in
copy is not what the sync writes.

**The near bank learns your taste: like and never again** (20.11.2026). On 19.11. Rene rated fifteen near presets
by ear; the thirteen he liked became a list in the generator's source. Now the instrument asks: beside the near
preset box (Near Events) a heart (like) and a struck circle (never again), for the near preset that is playing, with
their meaning in the help line. Never again takes effect at once: Auto no longer draws that preset (and if Auto is
playing it, draws another straight away); it stays in the box, dimmed and marked "(never again)", to be chosen by
hand and taken back with the same button. A liked preset is marked "(liked)" and Auto draws it twice as often --
twice, because an artist's list holds about 130 entries in groups Rene weighted, so a handful of likes leans inside
his table instead of replacing it (ten likes of average weight go from about 8 % to about 14 % of the draws). The
verdicts are kept by name in `Documents/Noctuary/near_taste.txt` beside the favourites, one `like <name>` or
`never <name>` a line, written atomically when a verdict changes and never at load; a missing or garbled file is
harmless. `Tools/make_layer_presets.py --taste` and `Tools/library/near_table.py --taste` read the same file: a liked
preset becomes a parent of variants (a liked variant counts for its parent), a turned-down one and its variants are
left out, and the bank stays at 512. Without `--taste` both make today's banks and table byte for byte, which a new
ctest, `layer_presets` (`make_layer_presets.py --check`), now keeps. The bank was not regenerated. Selftest over 20000
preset names of one pack: the most drawn near preset (Flute Phrase, 1588 times) is drawn 0 times once turned down,
every name still gets a foreground, and a liked one (Shakuhachi) goes from 1483 to 2746, x1.85 against the x1.86
the arithmetic expects.

**Who plays: the stage names the source of every note** (20.11.2026). Rene could not tell which part of the
instrument had played a note he did not like. The stage now colours its dots by who plays them -- keys orange, the
conductor blue, the second conductor violet, the near layer green (before, the second conductor and the near layer
were drawn in the keys' orange) -- with a legend along its top, and lists beside the dots the last notes begun, newest
on top: the time on the instrument's clock, the source in its colour and the note, a conductor's exchange as
"D3 -> F3", the near layer with its kind (note, phrase, sequence step, ghost), a phrase's slide as a move too. A
column at the stage's right in Normal, Compact and Pages, a box in its corner in Expanded, where the stage is narrow;
no page grew. The engine writes each note into a ring of 32 atomics where notes begin and end, the same places that
feed MIDI out: no lock, no allocation. The plane labels "conductor" and "conductor 2" no longer print over each
other when the two planes stand at one depth. The selftest renders three minutes of both conductors and a near
sequence with ghosts and compares the ticker with the note log: 546 of 546 notes the same in order, source, note and
time (24 conductor, 13 Brain 2, 236 near onsets: 160 steps, 76 ghosts). No sound changed.

**A fourth layout, Pages, after Empyrean's** (20.11.2026). "Could we consider an arrangement of the elements like
Empyrean's?" Settings > Layout > Pages: a strip under the header names seventeen pages under five headings -- Sound,
Space, Worlds, Conductor, Movement -- with a dot beside each that is lit while something on the page is on, and one
page is open at a time: its sections fitted to two thirds of the width, and beside them, the height of the page, the
picture of what they change (beside rather than above: above made the page taller and, on a 32:9 screen, everything a
fifth smaller). Every section of the tabbed page
stands on exactly one page; hosttest checks every parameter of the table against the pages (the header, the
modulation strip and the browser's map are shared by all layouts). Maximised on a 32:9 screen: labels 14 to 15
pixels high and knobs 120 to 130 across, against 9 and 70 before (Empyrean: 12 and 99 at its default zoom). Normal stays the
default; Pages is kept under a key of its own in the session, so a build without it opens Normal (hosttest checks
that too). A standalone window that comes back taller than its screen is made smaller to fit. `AMBIENT_LAYOUT=3` and `AMBIENT_PAGE=<n>` open it on a page for a picture.

**Maximised, the page fits the screen, on a wide one in three columns** (20.11.2026). "Extremely small and hard to
read when I start it maximised." On a 32:9 screen (7680 x 2160 at 150 %) the page, 1.7 to 1 and with its ratio fixed,
could only be as tall as the screen: it stood in 45 % of the width, 1.2 screen pixels a design pixel, labels 9 pixels
high and knobs 70 across. The maximise button made it worse: the window took the width, the fixed ratio made it 4627
pixels tall on a 2160-pixel screen, and the lower 53 % of the panel -- the morph, the conductor's lower half, the
modulation strip -- was below the edge. Maximised (or in full screen) the window now keeps the screen's shape and the
page fits into it whole, centred; and where the screen is much wider than the page, the tabbed page (Normal and
Compact) stands in three columns -- the voice; the effects, the cosmos and the spectrum; the morph and the conductor --
at about 3 to 1. On that screen: 1.5 screen pixels a design pixel, labels 11 pixels, knobs 88, 82 % of the width in
use, nothing cut off. Expanded fits whole too (it is a tall page and stays small on a wide screen). Back in a window
the page is the one it was, with its ratio; the corner still only zooms. No parameter, preset or sound changed.
`Tools/shoot_gui.ps1` starts the instrument muted and can maximise it (`-Maximize`, `-Restore`), choose the layout
(`-Layout`) and put the user's settings back byte for byte (`-KeepSettings`).

**Pitches, not keys: the rest of it** (20.11.2026). A test now checks the conductors' invariants in Hz over all thirteen
tunings, Free and Chords, two conductors on one root, Blend, Deja Vu, Overlap and a rest after each release: no voice
within 25 cents of one the same conductor sounds, none back inside its rest, no exchange onto the note it lets go, none
on a pitch the other conductor sounds, and no conductor kept to three pitches. It found one more: a voice sounding out
an exchange's Overlap in Chords mode had left its slot, and the other conductor no longer heard it and took its very
pitch (28 of 3549 onsets); the other conductor now hears those voices too. Over the music audit's 200 presets the
onsets on a pitch the other sounded fell from 5 to none and the time with a semitone or tritone across the two from
1.20 % to 1.03 %. Two more places compared keys: at the end of a changeover Free mode let go the voices whose key
stood a multiple of twelve from the old root, which in Bohlen-Pierce (no octave, walked degree by degree) are other
pitches (34 such in 69 changeovers, now 4, those the conductor's own retirements); it now lets go the old root's
pitch class as the tuning plays it. And the rules asked of a deja vu note and a pivot tone read "the root sounds" by key,
where the draws read it by pitch class; now by pitch class too. The foreground's figures (a Sequence's steps, a
Phrase's notes) took a neighbouring key within five cents of the note before as no step; in a just scale the neighbour
mostly stands up to ten cents away, and 122 of 2052 figure notes struck the note before again. Now within 25 cents,
none; ghosts still repeat a step, as they are meant to.

**No second voice on a pitch the conductor holds** (20.11.2026). A conductor barred a candidate by its key and a
second key on the very same pitch within one cent. In a scale whose keys snap the neighbouring key mostly stands 2 to
10 cents beside the degree, and both tests let it through: a new voice ten cents from one still sounding, the two
beating -- the same note struck again (Gate Basin, JI Minor: 295.37 Hz beside 293.67). Now no voice begins within 25
cents of one the same conductor sounds, whatever the key -- the slots, a Blend note about to begin, a voice sounding
out its Overlap -- in Free mode's draw, in Chords mode's choice, from the deja vu ring and for the pivot tone; an octave
stays allowed. Over the music audit's 200 presets, an hour each: 845 such onsets of 10982, now none of 10833 (the
onsets that change the pitch rose from 10137 to 10833); over 120 presets in the scales with the fewest pitches 563 of
6544, now none of 6524; over 120 Chords-mode presets 381 of 6181, now none of 6289. No conductor fell silent or onto
one pitch. Where a conductor is set for clusters (Consonance under a half) the next choice can be the minor second the
setting allows instead of the beating unison: the time with a minor second inside a conductor went from 0.48 % to 0.64 %
over the 200, most of it in such conductors (the second conductors of Chantry Rite, Resin Threshold and Driftwood
Veil, Consonance 0.24 to 0.42).

**An exchange moves to another pitch** (20.11.2026). Chords mode replaces one voice by the note that sits best within
Voice Lead of it, and barred the note being replaced by its key alone. In a scale whose keys snap -- the just scales
play twelve keys on five or seven degrees -- the neighbouring key often plays the same pitch or one 2 to 10 cents from
it, and being the smallest move it won: the old voice sounded out its Overlap while the same note began again beside it
(Gate Basin, JI Minor: keys 74 and 75, ten cents apart, a dozen times an hour). Now no key the tuning plays within 25
cents of the note replaced may replace it, neither in the choice nor from the deja vu ring, as in Empyrean. JI 7-limit
on four roots, the chord moving every eight seconds: 103 of 447 exchanges landed on the pitch let go, now none of 428.
With the exchanges real, one more rub came out: a note a semitone from a voice still sounding out its Overlap -- not the
one it replaced, the one before -- stood beside it for its whole Overlap; such a voice now goes within two seconds too,
as the replaced one does and as Free mode always had it. Over 120 Chords-mode presets of the library, an hour each, the
onsets within 25 cents of a pitch the same conductor still held fell from 1107 to 381, and the onsets that change the
pitch rose from 5220 to 5800 (the time with a semitone or tritone across the two conductors from 1.0 % to 0.5 %); over
the music audit's 200, from 1468 to 845, with 1 % fewer onsets in all (11097 to 10982) and no conductor left silent or
on one pitch. Over 120 presets in the scales with the fewest pitches (Harmonic, Subharmonic, JI Pentatonic, Slendro):
876 to 563, onsets 6653 to 6544. Sounding Dub (JI Pentatonic, Voice Lead 1) changes pitch 26 times an hour instead of
37: within one key of a pentatonic note there was only the same pitch, and its exchanges had walked through it.

**The program says when its library is older than itself** (20.11.2026). Noctuary.exe started by a double-click in
the source tree read the installed packs out of C:\ProgramData\Noctuary\Packs, not the tree's: the program built on
13.11. played the packs of 14.09. for weeks, and nothing anywhere said so. A library now carries a stamp,
`library.stamp` beside the packs -- when they last changed and a hash of them -- which the build keeps current in the
tree and compiles into the program, and the installer copies along. When the loaded library has no stamp or an older
one, the status line under the header begins with OLD LIBRARY in orange, names the folder and both dates, and the
browser's count and About say the same; nothing blocks and nothing sounds. A program started from a source tree's
`bin` folder now reads that tree's Library/Packs, as Tools/play.ps1 had it do; AMBIENT_PACKS, when set, still wins.

**The music audit** (20.11.2026). The faults heard this week -- rubs inside a conductor and between the two, one
doubling the other's note, a pitch struck again soon after it was let go, notes shorter than their own attack -- are
counted by `Tools/library/music_audit.py` over an hour of both conductors per preset, without audio and in cents of
what the tuning plays, and the ctest `music_audit` checks 200 library presets against stored bands. With the rule
against the other conductor's notes switched off for a trial, doublings rose from 7 to 501 and the time with a
semitone or tritone between the two from 1.05 % to 8.97 %, and the test failed. The self test takes `--list` and
`--only` and prints each test's time.

**The foreground's lines keep to the chord** (19.11.2026). A near Sequence drew each of its steps on its own, over two
and a half octaves, against the first conductor's notes only. Under Steel Span with Bowed Line, 62 % of the line's notes
stood a second, a seventh or a tritone from a note the conductors were holding. A Sequence and a Phrase are now figures,
after Empyrean's: they begin and end on a chord tone, move by step more often than by leap, and take no note that rubs
against a sounding one; when the chord moves under a running line, a step that no longer fits goes to the nearest note
that does. The second conductor's notes are part of the chord the near layer listens to. Under Steel Span: 0 % rub, 97 %
are chord tones. The lines keep their sound: ghosts, accents, gates and tempo are as they were.

**The two conductors hear each other** (19.11.2026). They shared a root and nothing else: each chose its notes against
the root and its own voices, and the second's could stand a semitone or a tritone from the first's -- in Gannet Colony
each was clean by itself while 32 % of their pairs were a semitone apart and 30 % a tritone. Each now weighs a
candidate against the other's sounding notes as it weighs the root, to the power its Consonance asks, and a conductor
whose Consonance is at least a half does not take a note a semitone, a major seventh or a tritone (within two octaves)
from one of the other's -- the weight alone was too gentle, since harmonic entropy puts a semitone at 0.10 and a major
third at 0.17, and where the rules left a single candidate it won however it scored (Pedal Line held C# against the
second's D for twenty minutes). Over 200 presets of the library, an hour each, the time with such a pair across the two
fell from 3.6 % (semitones) and 1.3 % (tritones) to 0.8 % and 0.1 %, the presets where it stood more than a tenth of
the time from 28 to none, with as many notes as before and no conductor left silent. A preset without the second
conductor plays exactly as before. `ambient_brain_audit` prints each note's frequency as an eighth field.

**A conductor hears its own notes** (19.11.2026). Free mode, which nearly every preset uses, weighed a candidate against
the root alone; a conductor set strictly consonant still put a note a semitone beside one of its own, and one leading a
single voice by step let the old note sound half a minute beside a successor a semitone away (Leaf Depth, Consonance
1.0: every change of the hour). Now a conductor whose Consonance is at least a half takes no note a minor second or a
minor ninth from a voice that stays -- in Free mode's draw, in Chords mode's choice, against the deja vu ring and for
the pivot tone of a changeover. A major seventh and a tritone stay colours, weighed as before. A step of a minor second
from the voice that leaves is a step of a line and stays allowed, but the old voice no longer waits the whole Overlap
beside it: two seconds at most (from Consonance 0.3, since the night's arc lowers it towards its climax). Over 150
presets without and 200 with a second conductor, an hour each, the time with a minor second or ninth inside a conductor
fell from 3.9 % to 0.3 % and from 3.3 % to 1.2 %, the presets where it stood more than a twentieth of the time from 52
to 4, with as many changes as before (46 to 48 an hour) and no conductor left silent; across the two conductors the
semitones fell further, from 0.8 % to 0.5 %, the tritones from 0.15 % to 0.06 %.

**No conductor doubles the other's note** (20.11.2026). The second conductor plays its own voices, so a pitch the first
holds and the second takes too was a second voice on the same note, its attack swelling over the first -- heard as the
note struck again, which under a pad's attack says nothing. Hearing each other had made it likelier, a unison being the
most consonant choice there is. Now neither takes a pitch the other sounds (within 25 cents, at any Consonance; an
octave stays). Over 300 presets of the library, an hour each: 641 of 16902 onsets (3.8 %) doubled the other's note,
now 8 of 17130; on one root and one register, a third of the onsets did, now none. The soundcheck's Harmonic Hours was
recorded again: its second conductor's first note is the E an octave under the first's instead of the F a major seventh
under it (its draw lost the first's own E), which moves its far plane 5 LU against the mix.

**A pitch rests, not a key** (20.11.2026). Retrigger kept the rest after a release per key. In a just scale whose keys
snap -- JI Pentatonic plays three neighbouring keys as one pitch -- the neighbour of the key just let go is the same
note, and it was free at once (in Empyrean, H-Beta Bell let 66 go and struck 68, both 392 Hz, seven seconds later). The
rest now holds for every key the tuning plays within 25 cents of the one let go, in Free mode, Chords mode and for the
deja vu ring and the pivot. Over 300 presets of the library: a conductor striking a pitch it let go less than thirty
seconds before, 35 times in 16902 onsets before the two changes of today, 13 now, with as many onsets (17068).

**The long loop in the library** (19.11.2026). Robert Rich's sleep-concert loop (Loop, on the Foreground page since
20.10.2026) was in no preset. 3014 of the library's have it now (Tools/library/retrofit_loop.py): three in five in the
sleep concert and the loop and tape packs and the slow unfolding ones, two in five in the slow chords, strings, hymns
and night pieces, one in four in the dark and far packs, none in the processions, drums, machines, rituals, signals,
weather, glass or field recordings; not where the conductor hurries (a chord sooner than every twenty seconds, an
event sooner than every six) or the delays already carry much. Each was played for a minute with its loop and
without: the loop adds 0.12 dB at the median (2.9 at the most, none over the 3 dB at which its Mix would have come
down) and sits 5 to 14 dB under the whole in the probes; master_gain came down by exactly what it adds.

**The near bank: 512 presets.** Built on what Rene heard and liked -- Bowed Line, Bunker Tube, Krell Circuit, Deep
Space Beacon, Number Station, Shortwave Dial, Whistler, Tuned Noise, Drop Sequence, Heartbeat, Machine Pulse, Wind
Chimes, Radio Cut-Up -- and longer rather than shorter. Thirty-four new ideas cross them: the signal sources as lines
(a new family, Signal Lines: Tube, Krell, Beacon, Morse, Dial and Whistler Line, Krell and Tube Pulse), long swelling
notes on the tube, the Krell circuit, the beacon, the dial and the bow, relatives of the heartbeat and the machine
pulse, chime lines and phrases, and cut-ups like Radio Cut-Up from the NASA and Library of Congress folders. Then
variants of all of them, each moving two things the ear tells apart -- register, tempo, place between the planes,
colour, onset, length, density -- and named by them ("Krell Line Slow Cosmic"); Quick only for lines, no single note
under six seconds. Every new preset joins the Near Auto group of the liked preset it comes from, so the auto foreground
draws them where it drew their relative, as often as before in all. Each was measured against four backgrounds
(near_loudness.py, which now also reports how long an event is heard) and set as loud as its liked relative stands
today -- a variant on its parent's gain had lain anywhere from 15 dB under it to 15 dB over; the 26 earlier variants
were measured the same way. Four whistler variants that stayed loud for under two seconds were left out for four more
lines.


**The near bank: two out, twenty-six in.** Seed Pod (the shaker, heard as a noise burst in the foreground) and Geiger
Counter left the bank. Two variants of each of thirteen presets joined it -- Bowed Line, Bunker Tube, Krell Circuit,
Deep Space Beacon, Number Station, Shortwave Dial, Whistler, Tuned Noise, Drop Sequence, Heartbeat, Machine Pulse, Wind
Chimes, Radio Cut-Up -- each with its register, tempo, ring, place or colour moved, and each in its parent's group in
Near Auto, so the packs that drew the parent now draw among three. 132 presets. The variants carry their parent's
measured Gain until they are measured themselves.

**Journeys with stays of their own.** Every step of a journey had the same range, five to ten minutes (eight to fifteen
at night). Now a step stays as long as its preset carries: the four of the twelve that move least by themselves a
minute and a half to three minutes, the four that move most six to ten, the others three to six, never two long or two
short in a row, the fade growing with the stay (at night three to fifteen minutes). A step lasts 4.8 minutes on average
instead of 7.5, 8.3 instead of 11.5 at night. The presets and their order are unchanged.

**MIDI out**: the near events' channel now gets every note's start as well as its end.

**Panic** (11.11.2026). A button in the header (Shift+Esc, and in the search): everything is silent at once -- every
voice, echo, grain, room and hall, a frozen one too -- and stays silent; a journey or a set's playback stops. The
same button, a key played or a preset chosen wakes the instrument: the conductor's chord at once, the piece from its
beginning. The sound's settings, its samples and rooms stay as they are.

**Noctuary Journeys.** A small program beside the instrument (Start menu, NoctuaryJourneys.exe) that renders journeys
into files: the templates beside the packs and your own (Documents/Noctuary/Journeys), each listed with its steps, its
shortest and longest pass and the length of its rendering -- the dwells and fades are drawn with the dice before it
begins, so the length is exact. Rendered as the instrument plays a journey: every step's preset with its clips, its
wavetable and its rooms, brought up on a fresh engine that takes over what the one leaving holds (its clips, its
wavetable, the near layer's clips, its room) and crossfaded in once it is audible, the conductor's chord and the near
layer handed over, the foreground as Auto (or the step) asks; the presets whose arc follows the clock hear the hours of
the rendering, from now or from 23:00; the passes of a cyclic journey, the last twenty seconds faded. Several chosen are rendered one after another, with progress, speed and time left. WAV or FLAC at 24
bits, Ogg Vorbis or MP3 at 320 kbit/s (MP3 through Windows' own encoder). The family's render app (Tools/renderapp,
the same in all six instruments); `--list`, `--render`, and a self-test of every format and the queue (ctest
`renderapp`).

**The tonal balance, corrected** (11.11.2026). The master's Balance had aimed at ranges worked out from the reference
recordings' third-octave curves read two thirds of an octave too high: a sub far under the recordings' (-34 to -20 dB of
the whole where 383 ambient recordings keep -11 to -5), the presence and the air 2 to 4 dB over them. The table now
comes from the tool that measures the recordings (Family/tools/mix_audit.py --balance-ranges). The Floor followed the
same reading: it held the sub 27 dB under the whole, and where a preset carried more sub than that of its own, the
floor's voice in the bass swelled to drown it -- up to 10 dB of bass over everything else. Its aims are the recordings'
now: the bass at about a third of the whole, the sub 6 dB under the bass. As the instrument plays, a preset's sub stands
at a median of -10 dB of the whole (-31 dB before, measured on 40 presets), and the presets the floor had swollen are 2
to 8 dB quieter and no longer boom. The library was measured again with it: 4113 of the 14336 presets in the packs and
53 of the 255 built in got a new gain (0.4 dB more at the median, 22 of them more than 3 dB either way), and the map was
laid out again.

## 2.5.0 (15.10.2026): the ways around the panel

**The ways around the panel** (15.10.2026, the family's Frame.h, from Empyrean): the search (Ctrl+K or the magnifier in
the header) finds any knob -- its tab opens and it lights up; a number after its name sets it --, any preset, tab or
action; the first steps on the first start; the night mode (the moon: the window dim and warm); the split square held
for the preset as it was loaded, and a star beside the Sound box while the knobs have moved from it; the journeys over time (Journey in
the header: the one under way step by step, a click goes there; the one being written, its steps' times and order
changed by hand); a knob's right-click menu with each route's depth to drag and every source to add; the standalone's
window back where it was left (place, size, maximised); ASIO in the standalone's audio settings (Windows).

**The code** (15.10.2026) follows the programming guideline of the CGVR group of the Universität Bremen: its names
(`m_` for members, constants without `k`, enum types with `E`), its comment template for Doxygen (every parameter with
its direction, every return value; the test `doccheck` holds it) and its layout, kept by clang-format; docs/STYLE.md
says where the code deviates and why. Nothing of it changes what is heard: parameter keys, saved sets and presets are
as they were.

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
