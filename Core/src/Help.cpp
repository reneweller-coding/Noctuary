/**
 * @file Help.cpp
 * @brief The help texts: the parameter table, the manual by topic and the tab paragraphs.
 *
 * Help.h declares five lookups; this file is the text behind them, and very little else. Three tables
 * of string pairs in an anonymous namespace: kHelp, one or two sentences per parameter key, in the
 * order of the panel; kTopics, the chapters of the manual as raw string literals, with the
 * bibliography last; and kTabHelp, a paragraph per block of the panel. The one piece of mechanism is
 * the family lookup: the four source slots, the eight LFOs, the six envelopes and the two delays share
 * one text each, so a key is normalised to its family template (familyKey()) and the answer for every
 * ParamId is resolved once, on first use, into HelpCache. The lookups are then an index into that
 * cache or a linear search over a short table, so any shell -- tooltip, the header line, the Help
 * page, the render tool's --list -- may call them freely on the message thread.
 *
 * Writing a text: a key that appears nowhere in kHelp gives "" and nothing complains here; the self
 * test (Tests/selftest.cpp) walks paramTable() and is where a missing text shows up. The section
 * comments inside the tables follow the panel, and the dates in them say when a family of parameters
 * arrived.
 */
#include "ambient/Help.h"
#include <cstring>
#include <string>

namespace ambient {

namespace {

/**
 * @brief One entry per parameter key.
 *
 * The slot, LFO, envelope and delay families share their texts: a
 * key like "src2_pos" is looked up as "srcN_pos", "lfo3_rate" as "lfoN_rate", "dly2_mix" as
 * "dly_mix" -- so the table stays readable and the three slots can never drift apart.
 */
struct HelpEntry { const char* key; const char* text; };
/**
 * @var const char* HelpEntry::key
 * @brief The parameter key as Params.h spells it, or a family template with an N in place of the slot digit.
 */
/**
 * @var const char* HelpEntry::text
 * @brief The one or two sentences shown for it: a string literal, never null.
 */

/**
 * @brief The parameter texts, in the order of the panel; the section comments name the block each run
 * belongs to. Searched once per parameter when HelpCache is built.
 */
const HelpEntry kHelp[] = {
    { "master_gain", "Output level after the mid/side stage and before the soft clipper. There is no compressor anywhere in this instrument: what you hear is the dynamics of the drone." },

    // ---- sources (shared by the four slots)
    { "srcN_type", "What this slot is. Additive: a bank of partials shaped by tilt, brightness, odd/even and shimmer (in Source 1 the strand bank with unison, detune and stacks). Harmonic: a table of spectra, morphed by Position. Wavetable: single cycles played as samples, as Serum, Vital and Hive play them. FM: a two-operator pair. Texture: a granular player over a loaded clip. Stretch: the same clip as a continuum, spectrally stretched up to a thousand times. Noise: ten colours. Off: silent." },
    { "osc_level", "Level of Source 1 (the strand bank when Additive, otherwise the slot). Levels of the three sources mix before the filter." },
    { "srcN_level", "Level of this source. All three sources are normalised so the same Level means about the same loudness, whatever the type." },
    { "srcN_delay", "Seconds this source stays silent after the note before it enters, up to thirty. The instrument's Envelope section is one envelope for the whole voice, so without this all four sources start together and a preset of a wavetable and a texture is one chord struck twice at once. Give the texture twenty seconds and it is a second instrument arriving under a note that is already sounding. 0 -- the default -- is what every preset did before this existed." },
    { "srcN_rise", "Seconds the source takes to fade in once its Delay has passed (a smooth curve, not a ramp). Ignored while Delay is 0 and while Env names a shape, which carries its own rise." },
    { "srcN_env", "A level contour for this source instead of the plain Delay-and-Rise entrance. Own gives it a shape of its own -- drawn on the SOURCES page of the ENV tab, read from 0 (silent) to 1 (the source at its written Level), with its own Mode, Time, Depth and Sync -- and takes none of the six modulation envelopes. Env 1 to 6 borrow one of those instead: the same shape the matrix reads, bipolar (-1 silence, +1 full), with that envelope's Mode and Time. Either way the shape runs from the note, not from the phrase, so every note gets it. Off by default." },
    { "srcN_env_mode", "How this source's own envelope runs: One Shot plays it once per note, Loop repeats it between its loop points, Sustain Loop holds at its sustain point (or loops) while the note is down and plays the rest from there when it is let go. Heard while the source's Env is Own." },
    { "srcN_env_time", "Stretches this source's own envelope: 0.05 is twenty times faster, 20 twenty times slower. Ignored while Sync is set." },
    { "srcN_env_depth", "How much of this source's own envelope is heard: 1 the whole contour, 0 the source at its written Level whatever the shape says." },
    { "srcN_env_sync", "This source's own envelope spans one note value at the current tempo." },
    { "partials", "How many harmonics the bank generates, 1 to 32. Partials above Nyquist are simply not made, so nothing aliases at any pitch." },
    { "tilt", "Spectral tilt: partial h has amplitude h to the power of minus Tilt. 1 is a saw-like slope, 2 a triangle-like one, 0.3 nearly flat." },
    { "brightness", "A window that fades the upper partials out: at 1 all 32 sound, at 0 only the fundamental. This is what Bloom opens over time." },
    { "odd_even", "Weight between odd and even harmonics. Negative thins the odd ones, positive the even ones; +1 is a hollow, square-like spectrum." },
    { "inharmonic", "Stretches the partials away from whole-number ratios like a stiff string or a bell; 0 is exactly harmonic." },
    { "shimmer", "Each partial has its own slow random drift of amplitude. Shimmer is how deep; this is the breathing of a Rich drone." },
    { "shimmer_rate", "How fast the partials' amplitudes drift. Slow means a spectrum that changes over a minute, fast a flickering one." },
    { "srcN_partials", "How many harmonics this slot's additive bank generates, 1 to 32." },
    { "srcN_tilt", "Spectral tilt of this slot's additive bank: partial h has amplitude h to the power of minus Tilt." },
    { "srcN_bright", "Brightness window of this slot's additive bank: fades the upper partials out." },
    { "srcN_odd_even", "Odd/even weight of this slot's additive bank; +1 hollow, -1 without the odd harmonics above the fundamental." },
    { "srcN_inharmonic", "Inharmonic stretch of this slot's additive bank, 0 = harmonic." },
    { "srcN_shimmer", "Depth of the per-partial amplitude drift in this slot's additive bank." },
    { "srcN_shimmer_rate", "Rate of the per-partial amplitude drift in this slot's additive bank." },
    { "srcN_octave", "Transposes the source by octaves. In Source 1 it moves the whole strand bank too." },
    { "srcN_ratio", "A just ratio to the note (1/1 .. 2/1): 3/2 puts the source a pure fifth up, 7/4 a harmonic seventh. Stays in tune with the scale because it is a ratio, not semitones." },
    { "srcN_pan", "Where the source sits, left to right. For Source 1 in Additive it shifts the strand bank's wandering centre." },
    { "srcN_table", "Which table the Harmonic and Wavetable types read: five built in, or User for a file loaded with the button -- a WAV from Serum, Vital or Hive (the frame length is read from the file, 2048 samples otherwise), a Surge .wt, or a single cycle of any length. The Harmonic type hears the file's spectra, the Wavetable type its waveforms." },
    { "srcN_pos", "Harmonic and Wavetable: the frame position, morphing between frames. Texture: where in the clip the grains start. Noise: the band centre or the colour's character." },
    { "srcN_pos_drift", "How far Position wanders on its own slow random curve. For FM it wanders the index instead." },
    { "srcN_fm_ratio", "FM: the modulator's frequency as a multiple of the carrier. Whole numbers are harmonic, fractions clangorous." },
    { "srcN_fm_index", "FM: how deep the modulator bends the carrier. Automatically reduced on high notes so nothing aliases." },
    { "srcN_root", "Harmonic: a floor under the fundamental. A table here is a spectrum, and a spectrum cut from a recording keeps what that recording had -- which for a bell, a bowed harmonic or overtone singing is very little at the bottom. Measured over 1200 frames of the library the energy sits at partial 5.8 on average, the fundamental holds a quarter of it, and a quarter of all frames have less than a tenth of their energy there. Played high that is the material speaking; played low it is why such a source sounds thin and seems to work only up top -- what is heard is the sixth partial of a note whose own pitch is not in the sound. Turned up, the fundamental is brought to half the frame's energy and the rest is pulled down to make room, so this moves weight about rather than adding level. At 0 the frame is exactly what was analysed." },
    { "srcN_unison", "Detuned copies of this source, spread across the field. The main oscillator has had up to six strands with their own detune, drift and place since the instrument began; a slot had one, in mono, and against the bank it sounded exactly as small as that -- which is most of what \"the wavetable sources are thin\" was. The copies are not a second oscillator: the partial bank is one flat list, so a second copy is thirty-two more entries in it at a slightly different pitch, and the same loop steps them all. Two is a body, four is a choir. Energy is divided among them, so this thickens rather than raises the level. One copy is what every preset written before this had." },
    { "srcN_uni_detune", "Cents between the outermost copies of this source. The copies sit symmetrically about the written pitch, so the sound stays where it was; what changes is that they beat against one another. Ten cents is an ensemble, forty is a chorus that has stopped agreeing." },
    { "srcN_uni_width", "How far apart the copies are placed across the stereo field. At 0 they sit on top of each other and only beat; at 1 the outermost are hard left and right. Pan then moves the whole group." },
    { "srcN_interp", "Texture: how the clip is read between two of its samples. Linear draws a straight line, which is what the instrument has always done and what every preset was voiced on. Hermite lays a curve through four samples instead (Catmull-Rom): played above the pitch it was recorded at, the straight line folds what it cannot represent back down as a metallic edge, and played below it, it acts as a gentle treble roll-off -- three to four decibels near the top of the range. The curve does less of both, at two more reads a sample. Which one is right is a matter of taste, not of correctness: a field recording through a granulator is often the better for the softer reading, so Linear stays the default and nothing changes unless this is turned." },
    { "srcN_density", "Texture: how many grains a second the slot starts, up to two hundred. What decides whether a cloud is heard as a cloud is not this number but Density times Grain -- the overlap, how many grains sound at once. Below about two the ear counts them and the sound is a rattle; from eight it is air. The ceiling used to be sixty a second, which put short grains out of reach of any real density: at sixty milliseconds it allowed an overlap of three and a half however far the knob was turned." },
    { "srcN_grain", "Texture: the length of one grain in milliseconds. Short grains smear the clip into a texture, long ones keep its identity. Stretch: the spectral window -- short is grainy and quick to follow the clip, long (the top third of the range) is the smooth, frozen continuum of a Paulstretch." },
    { "srcN_transport", "Harmonic: how the frames morph. At 0 the amplitudes are blended, which is what every wavetable does and why a morph between two formants sounds hollow halfway -- the old peak fades out, the new one fades in, and between them the energy dips. At 1 the morph is the displacement interpolation of optimal transport: each frame's spectrum is read as a distribution of mass over the partials and the mass is carried from where it sits in one frame to where it sits in the next, so a peak slides through the partials between rather than fading across them, and the energy halfway is what it should be (Roma, Green and Tremblay, audio morphing by optimal transport; along one axis the Wasserstein barycentre is exact and cheap). Position Drift and everything else read the table as before; only what lies between two frames changes." },
    { "srcN_spec_rate", "Spectral: how fast the model of the clip is read, as a multiple of the speed it was recorded at. This is the point of the type: the note sets the pitch and Rate sets the speed, and neither touches the other. At 1 the recording runs at its own pace, at 4 it hurries, and at 0 the read head stands still and the clip becomes a single held chord -- one moment of a recording, sustained for as long as the note lasts and transposed wherever it is played." },
    { "srcN_spec_breath", "Spectral: which half of the model is favoured. The analysis split every band into a partial and a band of noise by measuring how far its content stood above the local noise floor. At the negative end only the partials are rebuilt, and a rain recording turns into a chord of the frequencies hiding in it; at the positive end only the noise is, and a struck bell turns into the wind that has its shape. In the middle the clip is put back together as it was measured." },
    { "srcN_bow_force", "Force. Bow: how hard the bow presses on the string -- light and the string slips almost all the time, which is the airy, breathy end of a bowed note; heavy and it sticks for most of each period and releases suddenly, the Helmholtz motion and the full tone. Flute: the breath pressure; a light breath does not speak, a heavy one drives the jet towards its limit. Bowl and Ice: how hard the stick presses. Murmur: the effort of the voice -- level, an open mouth, a harder tone." },
    { "srcN_bow_speed", "Speed. Bow: how fast the bow travels; with Force it decides whether the string sticks or slips, and the pair is the bowing gesture. Flute: the noise the air carries, part of it into the pipe and part straight out -- the breathiness. Bowl and Ice: how fast the stick rubs, and below a twentieth it is lifted and the body only rings. Murmur: how fast the syllables come, three to six a second." },
    { "srcN_role", "Which of the voice's notes this slot sounds in. All: every note, as it always did. Lowest, Inner, Highest: the note's place in what its owner is sounding right now -- the register is the role in this music, and the cello under the chord need not also be the chime on top of it. The place is re-read as the cluster changes, and the slot fades over a second and a half, so a note that stops being the top hands the chime on rather than keeping it. A preset that says nothing here has every slot on All." },
    { "srcN_stretch", "Stretch: how many times slower than life the clip is read. 1 is the recording as it is; 40 turns twenty seconds into a quarter of an hour; 1000 turns them into a night. Pitch is unaffected -- it is set by Follow and the octave and ratio, before the stretch." },
    { "srcN_xfade", "Stretch: the crossfade at the loop's seam, as a fraction of the clip, so the end runs into the start without a bump. Ignored for a clip whose file name carries _loop: that one is seamless already and wraps straight round." },
    { "srcN_density", "Texture: how many grains start per second. Noise Crackle: how many crackles." },
    { "srcN_density_sync", "Ties the grain (or crackle) rate to the tempo: one per chosen note value instead of Density per second." },
    { "srcN_follow", "Texture and Stretch: Note pitches the clip to the played note (the clip is assumed recorded at its named pitch, the _A3 in its name); Free plays it at its own speed, with octave and ratio as a multiplier. For Stretch the pitch is applied before the stretch, so a chromatic sample plays across the keyboard without the high notes getting shorter. Noise Band/Wind: the band follows the note." },
    { "srcN_grains", "Texture: how many grains may sound at once, up to 64. More grains, denser and smoother; the level is normalised for the overlap." },
    { "srcN_spread", "Texture: scatters each grain's start point around Position, as a fraction of the clip. At 1 a grain may come from anywhere." },
    { "srcN_noise", "The noise colour: White, Pink (-3 dB/oct), Brown (-6), Blue (+3), Violet (+6), Grey (flat to the ear), Band (a resonant band at Position), Wind (a wandering band), Crackle (sparse impulses), Digital (sample-and-hold)." },
    { "srcN_noise_q", "Width of the Band and Wind colours: 0 wide open, 1 a whistle." },
    { "srcN_drift", "A slow, independent pitch drift of this source in cents. Three sources on just ratios each drifting on their own curve beat like real instruments in a changing room, never symmetrically." },

    // ---- strands (Source 1 additive only)
    { "strands", "How many detuned or stacked copies of the bank a voice plays, 1 to 6. Each has its own pitch drift and pan." },
    { "detune", "Spread of the strands in cents around the note. 0 with Stack Detune is a single beat-free bank." },
    { "drift", "Depth of each strand's slow random pitch drift in cents -- the tape-like wobble of an old analogue pad, but never a step." },
    { "drift_rate", "How fast the pitch drift moves. Very slow is a drone that leans; fast is vibrato-like." },
    { "spread", "Stereo spread of the strands around the voice's wandering centre." },
    { "bloom", "How much of the brightness is held back at the start of a note and opened over Bloom Time -- a spectrum that blossoms." },
    { "bloom_time", "Seconds the bloom takes to open fully, with a gentle start." },
    { "stack", "Places the strands on pure ratios instead of detuning them: octaves, fifths, a just major or minor chord, seventh, harmonics or subharmonics -- one key becomes a just chord." },
    { "rate_wander", "Every voice's drift, shimmer and breath rates themselves wander by up to an octave on a slow curve, so five minutes never look like the five before." },
    { "freeze", "Holds the spectrum and pitch still: shimmer, pitch drift, breath and bloom stop moving. The envelopes and effects keep their own time." },

    // ---- foundation
    { "sub_level", "Level of the sub voice, a sine (with Tone, a little more) one or two octaves under the brain's root, mono, injected after the mid/side stage so Bass Mono cannot thin it." },
    { "sub_octave", "How far under the root the sub sits: one octave (65-123 Hz under a C3-B3 root) or two (33-62 Hz). Two since 25.09.2026, which is the register a dark-ambient sub lives in -- 25 to 70 Hz, under the drone's own fundamental rather than inside it; one octave puts it where the pads' low end already is." },
    { "sub_glide", "Seconds the sub takes to slide to a new root, in the log domain." },
    { "sub_binaural", "Offsets the sub's left and right frequencies by this many hertz -- a binaural beat inside the bass." },
    { "sub_tone", "Adds a little second and third harmonic to the sub so it is audible on small speakers." },
    { "sub_source", "Root follows the brain's root. Difference follows the combination tone of the two lowest sounding voices (their frequency difference), folded into the sub's octave -- the ghost bass of a just chord." },
    { "pad_low_cut", "Partials below this frequency fall away at 12 dB per octave, leaving the bottom to the sub and keeping wide pads out of the sub's register." },

    // ---- air
    { "air", "Level of the breath layer: filtered noise inside every voice, following its pitch and its distance." },
    { "air_color", "Centre of the air band as a multiple of the note's fundamental." },
    { "air_q", "Narrowness of the air band. High Q is a whistling resonance, low a broad hiss." },
    { "air_mode", "Band: one wandering band pass. Ghost: the noise through six sharp resonators on the note's harmonics 1 2 3 5 7 9, so the harmony is filtered out of the chaos." },

    // ---- envelope
    { "attack", "Seconds the voice takes to swell in. Minutes are allowed." },
    { "decay", "Seconds from the peak down to the sustain level." },
    { "sustain", "Level held while the note is down (the brain holds its notes for Hold Min .. Hold Max)." },
    { "release", "Seconds the voice takes to fade after the note ends -- up to two minutes." },

    // ---- filter
    { "filter_on", "Switches the voice filter in or out. Off leaves the sources to the z-plane alone (or dry, if that is off too)." },
    { "filter_model", "The filter's character behind the same knobs: LP 6/12/24 low passes, HP 12, BP 12, Notch, Peak (a bell), Ladder (four-pole with saturating feedback), Comb (tuned to Cutoff, a resonating body)." },
    { "cutoff", "The filter's frequency. Key Track, Env Amount, Drift and the voice's distance move it from here." },
    { "resonance", "Emphasis at the cutoff. On the Ladder it self-oscillates near the top; on the Comb it deepens the dips between the peaks." },
    { "filter_env", "How far the amplitude envelope opens the filter, in octaves times four. Negative closes it as the note swells." },
    { "filter_drift", "Depth of the cutoff's slow random wander, in octaves times two." },
    { "keytrack", "How much the cutoff follows the note: 1 keeps the same partials in the passband on every key." },
    { "filter_drive", "Soft saturation ahead of the filter, level-compensated: adds harmonics, not loudness." },
    { "filter_fold", "A wavefolder after both filters. Drive flattens what will not fit; a folder turns it back on itself, and a wave mirrored at the fold grows a family of high partials that no saturation makes -- the metallic edge of an industrial record. The positive half folds a third sooner than the negative one, which is where the even harmonics and the body come from. Off at 0." },

    // ---- z-plane
    { "z_mode", "Off, or the z-plane filter in Series (it hears the voice filter, Mix is its dry/wet) or Replace (it is the only filter)." },
    { "z_route", "With both filters on: Series puts the z-plane after the voice filter; Parallel feeds both the dry sum and Mix balances them." },
    { "z_shape", "One of sixteen frame sets, four frames on the corners of a square: vowel morphs, bell clusters, resonator banks, the sweeps." },
    { "z_z", "The third axis of the cube. X and Y move the point around a square of four filters; Transform lifts it out of that square towards a fourth of its own -- usually the same shape far more resonant, sometimes its peaks turned into notches, sometimes an octave up. What that is depends on the shape, and it is written down for each of them in Tools/make_zplane_bank.py. At 0 the filter is exactly the square it always was, which is why every preset made before this knob existed still sounds the way it did." },
    { "z_decay", "Modal only: how long the lowest mode rings, from a tap to forty seconds. This is a T60 -- the time the mode takes to fall by 60 dB -- so 8 s means a struck bell that is still audibly there after eight. Every resonator is normalised to unity gain at its own frequency, so a long decay makes the instrument ring, not clip." },
    { "z_damp", "Modal only: how much shorter the higher modes ring than the lowest. At 0 every mode holds for the same time, which no real object does and which is exactly why it sounds unreal in a useful way. At 1 the decay time is inversely proportional to frequency, which is roughly what wood, metal and skin do. Between them is where most objects live." },
    { "z_x", "The point's horizontal position in the frame square; the filter interpolates the four corners' poles and zeros." },
    { "z_y", "The point's vertical position in the frame square." },
    { "z_rate", "How fast the point wanders around (X, Y) on two slow random curves." },
    { "z_depth", "How far the point wanders." },
    { "z_res", "Narrows every section's bandwidth: 1 is a quarter of the frame's, 0 double." },
    { "z_keytrack", "Moves the whole frame with the note's pitch (1 = fully)." },
    { "z_mix", "Dry/wet of the z-plane stage -- or, in Parallel, the balance between the voice filter and the z-plane." },

    // ---- space
    { "depth", "How deep the brain places its notes: 40 % land close, 60 % deep into the background, scaled by this. Per unit distance a voice loses 2.5 octaves of cutoff and 6 dB, and is heard only through the far reverb." },
    { "keys_depth", "The plane MIDI keys are played on (0 = at the ear, 1 = infinite background). The brain has Depth." },
    { "pan_drift", "How far each voice's centre wanders left and right on its own slow curve." },
    { "itd", "Time Width: the interaural time difference, up to 0.65 ms, applied to the far ear from the voice's pan -- width from time, not from level." },
    { "arc", "The hour-scale arc: one very slow drift that leans on density, brightness and depth, so the whole night has a shape." },
    { "arc_period", "Minutes the arc takes for one swing." },
    { "brain_dejavu", "Deja vu, after Mutable Instruments' Marbles. The conductor keeps a ring of Loop places holding the last notes it chose; each choice moves one place round the ring, and with this probability the note already there is played again instead of the fresh one -- which then stays. At 1 the loop plays round and round; below it, fresh notes seep into the loop at the rate the knob leaves them, so a figure returns and slowly mutates -- the middle ground between a loop and a random walk that generative music lives on. A note the ring offers that is still sounding is passed over. 0 draws nothing and changes nothing." },
    { "brain_loop", "How many places the deja-vu ring has: the length of the figure that can come back. Short loops are riffs, long ones are memories." },
    { "brain_spread", "The shape of the conductor's random draws -- how loud a note is, how long it holds. At 0.5 the draw is uniform, as it always was. Towards 0 it gathers round the middle: every note about as loud and as long as the last. Towards 1 it is pushed to the extremes: soft or loud, short or long, and seldom in between, which is Marbles' bimodal setting and a surprisingly musical one for a drone -- a few long anchors and short passing notes rather than a grey average." },
    { "brain_bias", "Moves the centre of the conductor's random draws: positive and the notes come louder and longer, negative and they come softer and shorter. 0 leaves the draw where it was." },
    { "chaos_period", "The time scale of the two strange attractors the matrix can read as lorenz_x/y/z and rossler_x/y/z: about the time between the Lorenz system's lobe changes, and about one turn of the Roessler spiral. Deterministic chaos -- never the same path twice, never a cycle, never a step -- which is what an LFO cannot give and filtered noise cannot either: the motion has a shape and a memory without ever repeating. The Lorenz coordinates swing between two centres and cross at irregular moments; the Roessler x and y circle slowly and almost regularly -- measured, its best self-match at any lag is 0.95, so it is nearly a cycle -- and its z stays low and then climbs suddenly, once a turn or so, by an amount that is not the same twice; routed at a filter or a distance that climb is an event that was caused rather than scheduled. The Lorenz system, by contrast, never comes back at all (0.40 at its best lag). Computed only while a route reads one of them." },
    { "keys_filter", "How pressure, slide and bend are smoothed between the controller's messages. Classic is a fixed thirty milliseconds. One Euro is the filter of Casiez, Roussel and Vogel (2012): a cutoff that follows the speed of the movement, about half a hertz for a hand at rest -- which takes the sensor's jitter out of a held note, where a fixed thirty milliseconds lets most of it through -- and twenty and more for a hand that moves, so a fast gesture is followed at once. The speed is read from how far the value still has to travel, because a controller speaks in steps and a derivative between two messages is nothing." },
    { "transpose", "A pure interval on everything that sounds: a fourth (4:3), a fifth (3:2) or an octave, up or down, the interval keys of SOMA's Terra. It glides there at an octave in two seconds and stops exactly on the ratio, so a press is a slide and every interval inside the chord is kept -- all the voices move together, and the Foundation with them. Map it to a switch or a footswitch and a drone can be lifted a fifth for a passage and set down again." },
    { "partial_spread", "The partials of the additive bank spread across the stereo field one by one, each at its own place, the pattern turning once every fifty seconds (Madrona Labs' Sumu spreads its partials this way). The fundamental and its neighbours are not favoured: neighbours in the series sit far apart, by the golden angle, so nothing harmonically related clumps to one side. Equal power per partial, so the mono sum keeps its level. The whole voice is still placed by its distance and pan as before; this is width inside the note rather than around it, and the two are composed rather than multiplied: a strand panned hard carries its field onto that ear, keeping every partial, instead of thinning out the ones that lean the other way. 0 is the bank as it always was, summed once." },
    { "lenia_rate", "How many steps a second the Lenia field takes. Lenia (Chan 2019) is Conway's Life taken to the continuum: real-valued cells on a small torus, a ring-shaped neighbourhood and a smooth growth rule, so blobs drift, pulse, split and die by their own neighbours' doing rather than by a clock or a random stream. The matrix reads it at four points as the sources lenia1 to lenia4, one near each corner, and the field is only computed while a route uses one of them -- a patch that does not costs nothing for it. Slow rates make it a tide, fast ones a weather." },
    { "lenia_growth", "The centre of the Lenia growth rule: how much company a cell wants around it before it grows rather than decays. The classic setting is 0.15; lower makes a sparser, wandering field, higher a denser, pulsing one, and at the ends of the range the field tends to die or to fill up, at which point it is reseeded with a few soft blobs and goes on." },
    { "sub_pulse", "The Foundation pulses at the Binaural rate: a raised cosine on its level, full at the top of every cycle and down by this amount at the bottom, with no corner anywhere. Binaural alone makes a beat only in the brainstem, where the two ears' phases are compared; an amplitude modulation is a beat on the basilar membrane itself, and it drives the auditory steady-state response of the cortex several times harder -- which is why the entrainment literature has moved to this hybrid of the two. Whether a few hertz of pulse entrain anything worth the name is a less settled question than its advocates say (the systematic reviews call the evidence inconsistent); what this builds is the modulation, not the claim. Needs a Binaural rate to pulse at; with Binaural at 0 it stands still and does nothing." },
    { "fb_bias", "The feedback shaper's operating point follows the bass. A static curve makes the same harmonics whatever came before it; a transformer or a capacitor-coupled tube stage does not -- low-frequency energy charges the coupling and shifts where on the curve the signal is being bent, so a bass swell changes how the highs distort, which is much of what an analogue stage's warmth is (the reactive nonlinearities of the wave-digital literature, Chowdhury among others). This is the cheapest honest form of that: the loop's content below sixty hertz, rectified and followed at five hertz, pushes the shaper's input off centre as the bass rises -- in the curve's own units after Drive, by up to 1.2, most of it there once the bass is a hundredth of full scale -- the loop throttles itself to small levels, so an offset proportional to them would never be heard, and one that rode on Drive would push the signal off the end of the curve -- so second harmonics come and go with the bass. Rides on Drive; 0 is the shaper as it always was." },
    { "far_comod", "The whole background breathes to one slow random envelope -- a new target about nine times a second, a smooth curve between -- so that every band of it rises and falls together. Hall, Haggard and Fernandes (1984) found that a tone in a noise whose bands move together is heard ten to fifteen decibels further down than in a noise whose bands move independently: the ear groups what moves together into one object and listens past it. Nothing in the foreground follows the envelope, which is what makes the foreground the thing heard past it. Full is a hundred per cent depth, which is the condition the effect was measured under and more than a background usually wants; 0 is the background as it always was." },
    { "near_ild", "The near field. The head model is a far-field one: it knows the shadow a head casts above a kilohertz, and nothing about a source within reach. Within a metre the wavefront is curved and the two ears are at measurably different distances from it, so the level difference grows at every frequency and most of all at the low ones -- twenty decibels and more at two hundred hertz for a source by the ear (Brungart and Rabinowitz 1999). That growth is the ear's own cue for closeness, which no presence filter gives it. A shelf below a kilohertz: cut on the far ear by up to eighteen decibels, lifted on the near one by up to four, scaled by how far to the side the voice sits and by the square of its nearness (Depth 0 is by the ear, and it is gone by the middle of the room). A voice in the centre gets nothing from it, as it should: both ears are the same distance from what is straight ahead." },
    { "brain_cascade", "Events that cause events. The conductor's clock draws every gap afresh and remembers nothing, which is the most even a random clock can be -- and nothing in nature is that even: a gust brings gusts, one crack in cooling wood brings the next. This makes the clock a Hawkes process: each event lifts the rate by a jump that decays over half a mean gap, so notes arrive in caused clusters with quiet between them rather than at their steady average. At full, one event breeds about two-thirds of another and the conductor fires roughly three times as often as Rate alone would have it. 0 is the clock as it always was." },
    { "brain_surprise", "How unpredictable the conductor aims to be, on the entropy of the intervals it has recently chosen: 0 is a machine repeating one figure, 1 is as unpredictable as twelve pitch classes allow. This is the target; Homeostat is how hard the conductor steers towards it. Predictive-coding accounts of listening put attention between two failures -- nothing surprising and the ear stops attending, everything surprising and it gives up -- and the middle of this knob is the middle of that." },
    { "brain_homeostat", "How hard the conductor holds its interval entropy at Surprise. When the recent choices have become more predictable than asked, the draw is flattened so the unlikely notes get their turn; when they have become more random than asked, it is sharpened towards the best-fitting notes. In Chords mode the same lean widens or narrows the little noise on the choice. Needs a few events of history before it says anything; at 0 nothing leans and the choice is as it always was." },
    { "purity_adapt", "Adaptive intonation. A note that starts is tuned pure against the notes already sounding -- nearest small-integer ratio to each of them, weighted by simplicity -- rather than against the root, and keeps that offset for as long as it sounds; capped at thirty cents. Such offsets accumulate: a progression through pure fifths and thirds walks away by a syntonic comma (21.5 cents) per cycle, which is why fixed just intonation either drifts or sours. So a common offset shared by every voice pays the drift back at three cents a minute, below the ear's threshold for a moving pitch, and because all the voices move together every interval stays exactly as pure as it was. Works on top of Purity and the scale; 0 is the tuning as it always was." },
    { "arc_clock", "The arc follows the clock instead of its own slow drift. A piece that runs all night can know what time it is -- the idea of the generative apps, Eno's Reflection and Endel among them -- so that its dawn and the real one coincide: the night's bottom is at four in the morning, its top at four in the afternoon, a cosine between, and Arc and Arc Harmony still decide how far that leans on the sound. Arc Period is ignored while this is on. Switching either way glides over twenty seconds rather than jumping. Off, the arc is exactly the drift it always was." },
    { "arc_sync", "Ties the arc's period to the tempo, in bars, instead of minutes." },
    { "presence", "A broad bell at 2-5 kHz on the near plane only (up to 6 dB), gone on the far plane -- foreground articulation the way Rich carves it." },
    { "breath", "Every voice's distance itself wanders by up to this much of the plane (0.35 at 1): the room breathes." },
    { "breath_rate", "How fast the distances breathe. 0.03 Hz is half a minute per swing." },
    { "phase_width", "Two all-pass stages per ear whose corners drift in opposite directions: the phase between left and right changes slowly and the room seems to change size rather than the sound to move. Off at 0." },
    { "phase_rate", "How fast the phase field drifts. Keep it slow: the effect is space, not tremolo." },
    { "haas", "The Haas trick, done to one band only. Delaying a whole channel by ten to thirty milliseconds widens it and destroys it in mono. Between about 1.2 and 4 kHz, where the ear takes its direction from level rather than from time, each side is given the other side's delayed band six decibels down: the edges open and the bass and the top stay exactly where they were. Off at 0." },
    { "haas_time", "How far that band is delayed. Twelve to eighteen milliseconds is the studio figure: long enough to be a separate arrival, short enough that the ear fuses it with the original instead of hearing an echo." },
    { "binaural", "Headphones turns the stereo picture into a binaural one: the pan becomes an angle round the head, the interaural delay follows Woodworth's head model (0.65 ms at ninety degrees), the head shadow is at full strength whatever Time Width says, and a source behind the head gets the lower pinna notch that tells back from front. With a headset or an OSC head tracker sending /ambient/head, the whole field turns against the head, so a voice stays where it is in the room while you look round. On speakers leave it off." },
    { "externalise", "The two cues a headphone image needs to sit outside the head: the notch the pinna cuts into what arrives from the side, and the reflection off the shoulder a quarter of a millisecond later. Both follow each voice's own position. On speakers leave it off." },
    { "doppler", "As a voice breathes closer or further away its pitch bends a little, the way a moving source does. A few cents at most; the ear reads approach and retreat from it." },

    // ---- ensemble
    { "ens_mix", "Amount of the ensemble (a slow stereo chorus) on the near bus." },
    { "ens_depth", "Modulation depth of the ensemble's delay lines." },
    { "ens_rate", "Speed of the ensemble's modulation." },
    { "ens_mode", "Velvet is the decorrelator the literature settled on: each channel is convolved with its own sparse random sequence of plus and minus one impulses -- velvet noise -- which is spectrally flat, so the two channels come apart without the sound being coloured and without anything being modulated. Chorus is the three modulated taps. Microshift is the studio's other way of widening: the two channels detuned a few cents in opposite directions and delayed by different amounts, with nothing moving. It survives a mono sum, which a deep chorus at 13 to 22 ms does not -- that is a comb filter waiting to be summed. In this mode Depth is the detune (up to 12 cents) and Rate a very slow wander of it, so the two sides never settle into a fixed phase." },
    { "ensemble_sync", "Ties the ensemble's rate to the tempo." },

    // ---- delays
    { "dly_time_l", "Left delay time in seconds. The two sides are independent: asymmetry is what makes the space wide." },
    { "dly_time_r", "Right delay time in seconds." },
    { "dly_sync_l", "Ties the left delay time to a note value at the current tempo; the knob is then ignored." },
    { "dly_sync_r", "Ties the right delay time to a note value at the current tempo." },
    { "dly_feedback", "How much of the delay returns into itself. Near 1 the echoes last for minutes." },
    { "dly_cross", "How much the left echo feeds the right and vice versa -- ping-pong at 1." },
    { "dly_damp", "Low-pass in the feedback path: each repeat darker than the last." },
    { "dly_duck", "The echoes make room. While the input is loud the high cut inside the feedback loop drops, so a fresh attack does not have to fight the brightness of the last one's tail; as the note settles the loop opens again over about a second. It is the same idea as Unmask in the far reverb -- get out of the way of what is being played -- applied to the delay. At 0 the loop behaves exactly as it always did." },
    { "dly_absorb", "Absorption: with Absorb up, the loop also loses its low end and its high cut moves down as Feedback rises, so long echoes drown into a warm fog instead of merely getting quieter." },
    { "dly_mix", "Level of the echoes on the near (dry) bus." },
    { "dly_to_far", "Level of the echoes sent into the far reverb instead: echoes that recede into the background." },

    // ---- reverbs
    { "near_mix", "Amount of the small foreground room around the dry voices." },
    { "near_decay", "Decay of the near room in seconds." },
    { "near_damp", "High-frequency damping of the near room." },
    { "far_level", "Level of the far reverb, the infinite background that every distant voice is heard through." },
    { "far_size", "Size of the far reverb's space (its delay lines)." },
    { "far_decay", "Decay time of the far reverb in seconds -- tens of seconds are the point." },
    { "far_damp", "High-frequency damping inside the far reverb: darker with every reflection." },
    { "far_predelay", "Milliseconds before the far reverb starts, for everything that reaches it. Since 25.09.2026 the gap that says \"in front of the room\" belongs to the source and follows its distance (Space: Gap), so this is only the hall's own offset and stands near zero by default: a source on the horizon has no gap between its sound and its room, and a pre-delay here would give it one." },
    { "depth_range", "How many decibels a source loses between the ear and the horizon. Six was the old law, and six decibels is a step, not a distance: the guide's three planes are 0 to -6, -10 to -18 and -20 to -36 dB under the foreground, and the ear reads depth as the difference between them. Exponential in the distance, so every step of the plane costs the same. At 20 a note at Depth 0.7 stands 14 dB under one at the ear and one on the horizon 20 under it; 30 and more is Lustmord's abyss, where the background is felt rather than heard." },
    { "depth_predelay", "The gap a source at the ear leaves between its direct sound and its far reverb, in milliseconds; a source on the horizon leaves none. Pre-delay is the distance knob a mixer reaches for before the reverb amount: fifty milliseconds at thirty per cent reads as nearer than none at fifteen. Here it is not a knob on the hall but a cue of the source, per voice, shrinking with the distance -- so an approaching event closes its gap as it comes." },
    { "depth_width", "How wide a source's strands and partials fan out at the ear, as a share of Spread; on the horizon they fan out fully. Width by plane: the guide wants the near layer placeable (20 to 40 per cent, a correlation of 0.5 to 0.9) and the far layer a surround (90 to 100 per cent), and nothing in the instrument told the width where a note stood until 25.09.2026. Far Width on the far reverb still narrows the background's tail if a preset asks for that funnel." },
    { "send_lowcut", "A high-pass in front of every reverb -- the near room, the far hall and the convolution room -- so that nothing under it is ever thrown into a tail. The first rule of reverb in the production guide: filter the send, 150 to 300 Hz, before the room and not on its return. Fundamentals fed into a hall come back as a low-frequency mass that no Low Cut on the tail removes, because by then they have recirculated for the length of the decay. The three Low Cut knobs stay what they were, filters on the tails; this is the one ahead of them. Second order, -3 dB at the number; 20 Hz switches it off." },
    { "sub_harmonics", "The residue of the Foundation: its second and third harmonic, at -24 and -27 dB under the fundamental at full, made from the sub's own phase so they are exact and alias nothing. A small speaker cannot move at forty hertz, and a sub of one sine is simply absent on it; the ear rebuilds the missing fundamental from its harmonics (the residue pitch, Schouten 1940), so with them the note is still heard where it cannot be felt. 0.6 by default, which is -28 and -31 dB." },
    { "sub_beat", "A second sine this many hertz above the Foundation, in both ears alike, so the pair's level swells and fades once every 1/Beat seconds: 0.1 to 0.4 is the slow breath of a Lustmord sub, two and a half to ten seconds a cycle, felt as the room itself breathing. This is a beat on the basilar membrane -- both sines reach each ear -- where Binaural is a beat made between the ears; Binaural changes nothing when the two channels are summed, this survives mono. The two share the level, so the peak stays where the sub alone peaked; the trough is silence." },
    { "far_asym", "Stretches the right half of the reverb's lines and delays its output slightly, so the two ears hear different reflections." },
    { "far_highcut", "Low-pass on the far reverb's tail." },
    { "far_lowcut", "High-pass on the far reverb's tail, 12 dB/oct, off at 20 Hz. The other end of the funnel a mixing engineer puts on a reverb return: dense tails and synthetic textures pile up between 200 and 450 Hz, and that is exactly where a background stops being behind the music and starts covering it. Somewhere between 300 and 500 Hz the tail loses its weight and lays itself behind the notes instead of over them." },
    { "near_lowcut", "The same for the near room: the small reverb's own low end, taken out so the foreground keeps its body." },
    { "room_lowcut", "The same for the convolution room. Real impulse responses of large spaces carry a lot of low-mid energy, which is what makes them sound real and what makes them muddy in a mix." },
    { "subsonic", "A steep high-pass (24 dB/oct) on the finished output, off at 0. Below about 20 Hz there is nothing to hear, but there is plenty to move: it takes headroom, it drives amplifiers and speaker cones for nothing, and it makes mastering processors distort early. Off by default and adjustable rather than fixed, because the Foundation two octaves under a low root reaches about 16 Hz -- a mastering engineer's 20 Hz cut would take this instrument's deepest tone with it. Set it under the lowest note you actually want." },
    { "vec_amount", "How much of the Vector: at 0 each source slot plays at the level it is set to and nothing here does anything. Turned up, the levels are taken over by a point in a square (X, Y) whose corners are the four source slots -- the Prophet VS and Wavestation idea, where the timbre is a place rather than a setting." },
    { "vec_x", "Left to right in the square: Source 1 at the left edge, Source 2 at the right." },
    { "vec_y", "Bottom to top: Source 3 at the top left, Source 4 at the top right." },
    { "vec_wander", "The point drifts on its own by this much, on two slow curves that share no ratio, so it never traces the same path twice." },
    { "vec_rate", "How fast it drifts. Drone rates: a whole cycle takes minutes at the low end." },
    { "far_freeze", "Holds the far reverb's tail forever: an instant infinite pad of whatever was in it." },
    { "far_unmask", "The background steps aside for the foreground, band by band: while a voice sounds, the far reverb loses that part of the spectrum and lets it back in over a second when the voice goes. A dense pad keeps its own notes audible instead of swallowing them." },
    { "body_level", "Level of the resonating body: twelve modes tuned to the root, fed from the finished mix and returned to it. Not a reverb -- a reverb is a statistical tail, this is a handful of pitched resonances, which is the difference the ear hears between a room and an instrument." },
    { "body_material", "Which set of mode ratios: Wood (a soundboard's irregular low modes), Plate (the stretched series of flat metal), Bell (hum, prime, tierce, quint, nominal), String (harmonic with a little stiffness)." },
    { "body_pitch", "What the body is tuned to, as a multiple of the brain's root. 1 is the root itself; 0.5 puts the body an octave below the music, which is what a large soundboard does." },
    { "body_decay", "How long the lowest mode rings. The higher modes die away faster, at a rate that belongs to the material." },
    { "body_tone", "Tilts the modes: at 0 only the low ones speak (dark and wooden), at 1 the high ones are as loud (bright and metallic)." },
    { "body_spread", "How far the modes are scattered across the stereo field. A body is not a point source; this is what makes it read as width rather than as movement." },
    { "patina", "The master's age: tape wow, the highs a worn machine no longer carries, a noise floor under the music, gentle saturation. Off at 0, and then not computed at all. Most of what separates a recording from a render." },
    { "patina_wow", "Depth of the wow and flutter: a wavering pitch, slow and irregular with a little 6 Hz on top." },
    { "patina_hiss", "The noise floor, a touch louder when the tape is carrying more (that is modulation noise, and it is what makes a floor sound like tape rather than like dither)." },
    { "patina_age", "How much top end the machine has lost: from untouched down to about 4 kHz." },
    { "far_diffuse", "Modulated all-passes in front of the far reverb: the tail arrives instead of starting. At zero the reverb answers immediately, as it always has; turned up, the first reflections smear into a slow swell that takes a second to become a room." },
    { "far_rotate", "The whole background slowly turns: the far field's left and right rotate into each other on a minute-scale curve. Depth of the turn." },
    { "far_mode", "Rotating is Colourless with the feedback changed: the line lengths stop wobbling and the feedback matrix turns instead -- eight rotations on pairs of lines, their angles advancing at a few hundredths of a hertz, in front of the reflection the network always had. Rotations and reflections are lossless whatever their angles, so the loop stays exactly energy-preserving while its modes are continually re-mixed rather than re-tuned: no pitch modulation anywhere in the tail, and a band pattern that drifts from moment to moment instead of ringing in one place (after Schlecht and Habets, time-varying feedback matrices). Colourless is Scattering with the eight line lengths replaced by a set searched offline for the flattest response, so the tail rings less at the lengths' own frequencies. Classic is the network as it always was: eight delay lines behind four all-passes. Scattering puts a short all-pass inside every line's loop, so each pass round the network scatters every echo into many: the echo density grows much faster (a quarter more of the tail is dense after 50 ms, measured) and the late tail is at least as smooth as before. Same decay, same level, a denser texture of tail -- after Schlecht and Habets." },
    { "far_unmask_return", "Seconds the far reverb takes to come back after the foreground has ducked it (the duck itself takes fifty milliseconds). 1.2 is what it always was; at five to ten the horizon's return after a near event is a gesture of its own." },
    { "far_unmask_spread", "How far a loud low band of the foreground also ducks the far reverb's bands above it. Masking in the ear is asymmetric: a low tone masks the frequencies above it far more than those below (the upward spread of masking), so a bass note in front should thin the background's middle as well as its bottom. At 0 the three bands are independent, as they always were." },
    { "far_width", "The width of the background alone, before it is added to the foreground. A mix in which everything is spread as far as it will go has no depth left -- it is a flat wall. Pulling the far plane in towards the centre while the foreground stays wide is the funnel that reads as distance: the ear is drawn into the middle of the horizon. 1 is the reverb as it made itself, 0 a mono background, and above 1 wider." },
    { "blur_mix", "A spectral smear on the near bus itself, ahead of the effects: every attack is wiped into texture, notes flow into each other. Mix of the blurred signal (latency 43 ms on the blurred part)." },
    { "blur_smear", "How much the blur smears: 0 follows the input closely, 1 is a spectral freeze that only lets new energy in slowly." },

    // ---- feedback
    { "fb_bus", "The mixed output returns, low-passed and saturated, into the near bus before the filters and effects -- throttled by the output level so it hisses and holds instead of running away." },
    { "fb_fm", "The returned output phase-modulates every partial of every voice (To Pitch): the sound bends itself." },
    { "fb_tone", "Low-pass on the feedback path." },
    { "fb_drive", "Saturation in the feedback path." },
    { "fb_tape", "Tape in the loop: asymmetric saturation, wow and flutter, a level-dependent noise floor." },

    // ---- room
    { "early_level", "The room's early reflections, from its geometry rather than from a reverb's statistics: the six surfaces of a shoebox, each with its own delay and direction, added to the foreground. What the ear takes the size of a room and the distance of a source from is not the tail but the first arrivals -- when they come and from where -- and unlike the far reverb this part moves when the sound does: the source position follows the sounding voices' own pan and distance. Off at 0, and then not computed." },
    { "early_size", "The room's longest dimension in metres. The shoebox is that by four fifths by nine twentieths, proportions with no simple ratio between them so the room's own modes do not pile up; the first reflections arrive at the times those dimensions imply, from about ten milliseconds in a small room to over a hundred in a hall." },
    { "early_absorb", "How much each surface takes out of a reflection, and how dark what comes back is. At 0 the walls are stone and the reflections go on bouncing; at 1 they are cloth and the room answers once and stops." },
    { "early_width", "How far apart the six surfaces are placed in the stereo picture. 1 puts each where its own direction says; below that the room narrows towards the centre, above it opens past the speakers." },
    { "room_level", "Level of the convolution room, an extra reverb from a loaded impulse response (or the built-in dark hall), in parallel on the far plane." },
    { "room_source", "What the room reverberates: the far sends (before the far reverb) or the finished near bus." },
    { "room_predelay", "Milliseconds before the room's response starts." },
    { "room_highcut", "Low-pass on the room's tail." },
    { "room_morph", "Crossfades between the two loaded impulses, A and B: one room becomes another over as long as you like. The two are blended inside the one convolution, so a morph costs what one room costs. A pack preset brings its B with it; one that names its room but no B plays A alone, whatever B was loaded before." },

    // ---- cosmos
    { "cosmos_send", "How much of the near bus goes into the Cosmos path (frequency shifter, resonator, vowel, nebula). The dry signal is untouched; Cosmos is additive." },
    { "cosmos_shift", "Frequency shift in hertz: every partial moves by the same amount, so the harmonic series becomes inharmonic. The right channel shifts 3 % less." },
    { "cosmos_shift_drift", "Lets the shift wander slowly around its value." },
    { "cosmos_res", "Level of the comb resonator tuned to the brain's root." },
    { "cosmos_res_pitch", "The resonator's pitch as a multiple of the root." },
    { "cosmos_res_fb", "Resonator feedback: how long it rings." },
    { "cosmos_vowel", "The vowel filter's position, a-e-i-o-u." },
    { "cosmos_vowel_rate", "How fast the vowel wanders." },
    { "cosmos_nebula", "Mix of the Nebula: a spectral smear that scatters the phases of the spectrum, a frozen cloud at full Smear." },
    { "cosmos_smear", "How much the Nebula smears; 1 is a spectral freeze." },
    { "cosmos_shimmer", "Feeds the far reverb's pitch-shifted previous block back into its input: the rising cloud. Regulated by the reverb level so it cannot run into the clipper." },
    { "cosmos_shimmer_pitch", "The shimmer's transposition: an octave up, a fifth, a fourth, an octave and a fifth, an octave down, two up." },
    { "cosmos_return", "How much of the Cosmos path returns to the near plane." },
    { "cosmos_to_far", "How much of the Cosmos path goes into the far reverb." },

    // ---- cloud
    { "cloud_send", "How much of the recent foreground the granular cloud takes. What comes back lands on the far plane, behind everything, unless To Near brings some of it forward." },
    { "cloud_to_near", "How much of the cloud comes back in front instead of behind. The cloud has always returned to the far bus alone -- it was built as a background that the far reverb smears, and at 0 that is exactly what it still is. Turned up, the same grains are shared between the two planes at equal power, so the cloud moves forward without there being more of it. If you have ever turned the Send all the way up and wondered where it went, it went behind you." },
    { "cloud_density", "Grains per second in the cloud." },
    { "cloud_sync", "Ties the cloud's grain rate to the tempo." },
    { "cloud_size", "Length of the cloud's grains in milliseconds." },
    { "cloud_pitch", "Transposition of the cloud's grains: octaves and fifths, the cloud a register above the voices." },
    { "cloud_spray", "How far back in time (seconds) the grains are taken from." },
    { "cloud_level", "Level of the cloud, dropped into the far reverb." },
    { "cloud_feedback", "How much of the cloud goes back into its own history: grains of grains, transpositions that stack. Saturated without aliasing and held at a ceiling, so 1 sustains instead of running away." },
    { "cloud_tone", "Low-pass in the cloud's feedback loop: how much brightness each pass keeps." },
    { "cloud_transpose", "Transposes every grain of the cloud, in semitones." },
    { "cloud_scatter", "Spreads the grains over the intervals of the scale that is playing, measured from the conductor's root: octaves first, then fifths and fourths, then the rest -- the whole scale at a half. Beyond a half the grains begin to leave the scale; at 1 they are free." },
    { "cloud_swarm", "Grains in flocks: every grain makes the next more likely for a moment (a Hawkes process), at the same mean density." },
    { "cloud_resonance", "Mix of the resonators on the scale's notes. Each grain excites one of them, and it rings on after its grain has ended." },
    { "cloud_res_mode", "Band: one resonance per note, which glides when the root moves. Comb: a tuned comb, every harmonic of the note ringing." },
    { "cloud_res_notes", "Which notes the resonators sit on, in the two octaves above C3: the whole scale, the chord on the conductor's root, its fifths, or its octaves." },
    { "cloud_res_decay", "How long the cloud's resonators ring, in seconds to -60 dB." },
    { "cloud_shift", "Transposes the cloud's feedback loop on every pass -- an octave, a fifth or a fourth, up or down -- in the spectrum, so a spiral of grains climbs or sinks cleanly. Needs Feedback." },
    { "cosmos_shimmer_mode", "Which shifter the shimmer loop uses. Spectral moves every partial in the spectrum and stays clean however often the loop comes round; Grain is the two-head shifter the shimmer always had, with its flutter." },
    // ---- memory
    { "mem_send", "How much of the foreground goes into the Memory, a long drifting sound memory beside the Cosmos." },
    { "mem_return", "Level of the Memory back on the near plane." },
    { "mem_to_far", "Level of the Memory into the background, where the far reverb takes it." },
    { "mem_lines", "How many delay lines the memory is divided into: two long ones, four, or eight shorter ones. Changing it re-divides what the memory holds." },
    { "mem_size", "How long the longest line repeats, in seconds; the others step down from it to six tenths, every length a prime number of samples." },
    { "mem_blur", "Exchange between the lines: at 0 every line is its own echo, towards 1 every echo runs through all of them, and beyond a half the echoes scatter into a space. It never changes the level." },
    { "mem_drift", "The lines slide against each other and wander across the stereo field, bent by a slow chaotic signal -- at most 15 cents of pitch, so the echoes move and the notes do not warble." },
    { "mem_hold", "How long a memory lasts: 4 seconds to -60 dB at 0, many minutes near 1, for ever at 1." },
    { "mem_age", "How much darker a memory grows as it fades: the highs go faster than the lows, at the same rate in every line." },
    { "mem_renew", "What arrives pushes out what the memory holds, the harder the louder it is." },
    { "mem_drive", "Saturation in the loop, without aliasing: a hot memory holds at its limit and grows a little darker on every pass, as tape does." },
    { "mem_recall", "Grains played out of the whole memory instead of its echoes." },
    { "mem_seek", "How much the recalled grains prefer stretches whose notes fit the scale and what came in during the last seconds, against chance." },
    { "mem_grain", "Length of the recalled grains, in milliseconds." },
    { "mem_freeze", "Holds what the memory has, exactly, and takes nothing new in." },
    { "mem_reverse", "Runs the memory's tape backwards: what was recorded plays backwards, and what is recorded now comes back as it went in." },
    { "mem_half", "Runs the memory's tape at half speed: what was recorded plays an octave down and twice as long." },
    { "mem_erase", "Empties the memory, and keeps it empty while on." },

    // ---- master
    { "master_tilt", "One broad see-saw around the pivot: turn it down and the whole instrument leans dark, up and it leans open. A single tilt does more for an ambient mix than any equaliser with more knobs, because it never carves a hole." },
    { "tilt_pivot", "The frequency the tilt turns around: everything below moves one way, everything above the other." },
    { "bass_mono", "Below this frequency the side channel is removed: a mono low end, the foundation of a wide picture." },
    { "side_air", "A broad bell at 3 kHz on the side channel, up to +6 dB: air in the width." },
    { "mono_guard", "A safety net for mono. Everything in this instrument is built to widen -- all-pass phase width, asymmetric delays, a reverb whose two sides are deliberately different -- and a drone that sounds gigantic in stereo can lose most of itself when a phone, a club system or a radio sums it to mono. With this on, the side channel is measured against the mid over about a second and a half, and if the side really is the louder of the two the width is eased back, by at most a quarter and at about two per cent a second. It never touches the middle of the mix, only how far the sides may go, and on anything that is already mono-safe it does nothing at all. Off if you would rather have the width and check the mono sum yourself." },
    { "width", "Stereo width: 1 as recorded, above widens, 0 mono." },

    // ---- cluster brain
    { "brain_on", "The conductor: chooses notes from the scale, places them on the planes, holds them for minutes and lets them go. Off, only your keys play." },
    { "brain_density", "How many notes the brain keeps sounding at once." },
    { "brain_rate", "Seconds between the brain's decisions (a new note or a release), on average." },
    { "brain_sync", "Ties the brain's decision rate to the tempo -- a decision every so many bars." },
    { "brain_hold_min", "Shortest time the brain holds a note, in seconds." },
    { "brain_hold_max", "Longest time the brain holds a note, in seconds." },
    { "brain_low", "Lowest MIDI note the brain may choose." },
    { "brain_high", "Highest MIDI note the brain may choose." },
    { "brain_consonance", "How strongly the brain prefers consonant intervals to the notes already sounding. 1 is pure, 0 anything goes." },
    { "press_distance", "How far a key's pressure pulls its voice towards the listener. Press harder and the note steps out of the background into the foreground -- brighter, louder, drier, all at once, because the plane decides all of that." },
    { "press_bright", "How much pressure opens (or, negative, closes) the voice's brightness on top of the plane change." },
    { "press_level", "How much pressure raises the voice's level." },
    { "slide_cutoff", "How far a sideways slide (CC 74 on an MPE controller) moves that voice's filter, in octaves." },
    { "slide_z", "How far the slide moves that voice's point in the z-plane filter." },
    { "bend_range", "Pitch bend range in semitones. With MPE every finger bends on its own channel; without it the wheel bends everything." },
    { "mpe", "MPE: channels 2 to 16 each carry one note with its own bend, pressure and slide, the way a Seaboard or Linnstrument plays. Off, pressure and the wheel apply to every sounding voice." },
    { "brain_quantize", "Holds the conductor's decisions until the next note value of the clock: the notes land on the grid instead of wherever the dice fell. Free is how it has always worked." },
    { "auto_mode", "Free is the conductor as it has always been: notes start and stop on their own timers, so the cluster breathes but never really moves. Chords keeps it full and exchanges one voice at a time -- the chord travels instead of churning. Everything else in this section only matters in Chords." },
    { "auto_rate", "Seconds between exchanges. Long is the point: at a minute apart a listener hears a harmony that is going somewhere without ever catching it move." },
    { "auto_sync", "Puts the exchanges on the clock instead of the seconds knob: one every so many bars." },
    { "auto_lead", "How far the exchanged voice may travel, in semitones. Small is voice leading -- the note that leaves is replaced by one near it, and the ear hears the chord shift rather than one note being cut and another started. Large lets the harmony jump." },
    { "auto_tension", "How strictly the arriving note has to fit the ones that stay. At 0 only notes that sit well against the whole chord are considered; turned up, the progression is allowed to lean." },
    { "auto_root_move", "How often an exchange also moves the root. Without it the harmony circles one centre for ever; with it the piece travels -- and the Foundation travels with it, since the sub stands on the root unless its Source says otherwise. It applies in both conductor modes: in Chords it is the whole of the chance, in Free it is added to the smaller one Wander already carries." },
    { "auto_step", "Exchange a voice now, whatever the timer says. It is a trigger, not a setting: switch it on and it fires and switches itself back off, so a controller, a macro or the button on the panel can drive the progression by hand." },
    { "brain2_on", "A second conductor, normally for the background: its own register, pace, density and plane, on the first one's root plus an interval. Two of them make a slow counterpoint that neither would play alone." },
    { "brain2_density", "How many notes the second conductor keeps sounding." },
    { "brain2_rate", "Seconds between the second conductor's decisions." },
    { "brain2_hold_min", "Shortest time the second conductor holds a note." },
    { "brain2_hold_max", "Longest time the second conductor holds a note." },
    { "brain2_low", "Lowest note the second conductor may choose." },
    { "brain2_high", "Highest note the second conductor may choose." },
    { "brain2_depth", "The plane the second conductor plays on: 1 puts it deep in the background behind the first." },
    { "brain2_interval", "Semitones between the first conductor's root and the second's. 7 makes it answer a fifth up, -12 an octave down." },
    { "brain2_consonance", "How strongly the second conductor prefers consonant intervals to its own root." },
    { "brain_even", "How strongly the conductor prefers chords whose notes are spread evenly round the octave. Tymoczko showed that the chords which can be joined to their neighbours by small movements are the nearly even ones, and that those are the chords Western music actually uses -- evenness is not a taste but the property that lets a chord MOVE, where a cluster can only leap. Set this against Harmonic, which pulls the other way, towards low harmonics and octave doublings; the two together are a spread-against-rooted control that neither has on its own." },
    { "brain_smooth", "Which voice moves. Normally the conductor retires whichever voice has been sounding longest -- a rule about time that knows nothing about where the chord would land -- and then looks for a replacement. With this up it tries every voice and keeps the exchange that moves the chord the shortest distance for what it gains. That distance is the voice-leading distance of Tymoczko's geometry: the smallest total travel over all ways of pairing the old chord with the new one. In the free mode -- the one nearly every preset uses -- it reads as the step size of a wandering voice instead: a candidate is weighted by how far it stands from the note chosen before it, so the conductor steps oftener than it leaps and a leap stays possible. At 0 neither applies and the draw is exactly what it always was." },
    { "brain_blend", "How a chord arrives. Rasch measured the onset spread of real ensembles at thirty to fifty milliseconds, and Bregman's rule is that tones starting together are heard as one object while tones starting apart are heard as separate voices. The conductor normally brings its voices in one at a time, minutes apart, so every voice is its own object. Turned up, the notes that fill an empty chord arrive together -- inside thirty milliseconds at the top -- and fuse into one sound. At 0 nothing changes." },
    { "purity_guard", "The fluctuation guard. Fastl and Zwicker: the sensation of fluctuation peaks at four hertz and is gone by about twenty; roughness takes over near seventy. Between two and eight hertz a beating chord is heard as wobble, which in a sleep concert is the one thing it must not be. Purity Drift makes beats without knowing where they land; the BEAT source already measures where they landed. Above zero the guard reins the drift in while the beat sits in that band, so it slows back out of it; below zero it does the opposite and seeks the wobble out. It works on the drift's excursion, not on Purity itself: a Purity that already puts the beat in the band is left where you put it. At 0 the drift is exactly what it was." },
    { "match", "Partials placed on the degrees of the current scale instead of on the harmonic series -- Sethares' other direction. The Timbre scale takes a spectrum and finds its scale; this takes a scale and bends the spectrum until that scale is the smooth one: each partial moves to the nearest degree, counted in periods from the fundamental. For 12-TET the seventh partial moves from 3369 cents to 3400 and the fifth from 2786 to 2800, small moves, and after them a tempered chord stops beating; for Bohlen-Pierce it makes the spectrum the tritave scale was always waiting for. Works on Source 1's additive bank and on the conductor's Timbre ear alike. Has no effect with the Timbre scale, which is itself computed from the spectrum -- the two would chase each other in a circle." },
    { "arc_harmony", "How far the hour-scale arc leans on the harmony as well as on density, brightness and depth. Lerdahl and Krumhansl modelled tonal tension -- distance from home and surface dissonance -- and tested it against listeners; this instrument's proxies for those are the conductor's Harmonic, Key and Consonance, so the arc's rise loosens all three and its fall tightens them. The climax of the night is then denser, brighter and further from home, and the return is a return. At 0 the arc leaves the harmony alone." },
    { "elev_near", "Height of the foreground, from a little below the ear to overhead. The ear hears up and down at the pinna: a notch between about six and ten kilohertz whose frequency rises with elevation (Hebrank and Wright 1974), and Blauert's directional band near 8 kHz that says \"above\". Both are small filters, not a measured head. A voice takes its height from where it stands between the two planes, so this and Elev Far together tilt the whole landscape. Heard through the same notch as Externalise, and it brings that path in on its own; 0 is the flat field the instrument always had." },
    { "elev_far", "Height of the background. Set it above the foreground and the drones are the sky; below, and they are the ground the foreground stands on. See Elev Near for what the ear is being told." },
    { "depth_law", "Bends the depth axis so that the knob is linear in HEARD distance rather than in the model's. Perceived distance grows with physical distance as roughly a power of a half (Zahorik: the pooled exponent is about 0.54), so a plane that is linear in level and reverb spends most of its perceptual travel in its near half. At 1 the plane is d^1.85, the inverse of that exponent: the far half of the knob then sounds as far again as the near half. The ends do not move -- 0 is still the ear, 1 the horizon." },
    { "far_envelop", "The sense of being inside the room, as distinct from how wide the source is. Bradley and Soulodre found it in late lateral energy at LOW frequencies, under about 500 Hz -- exactly where this instrument's background has the least side, because the funnel narrows it and Bass Mono folds it. This lifts the far bus's side channel in the band between Bass Mono's corner and 500 Hz -- up to six decibels on the band, three measured on the finished output, where the reverb's own low end was never wide to begin with -- and leaves the mid alone. It starts where Bass Mono ends on purpose: anything lower would be lifted here and removed again at the master." },
    { "brain_key", "How strongly the conductor prefers the stable degrees of the key it finds itself in. Nothing sets that key: it is measured from what has actually been sounding and for how long, and on an instrument whose notes last minutes the duration is the only weighting that means anything. The stabilities are the ones Krumhansl and Kessler measured from listeners in 1982 -- the tonic highest, then the fifth, then the third, then the rest of the scale, then the notes outside it -- and the key itself is found by correlating what is sounding against all twenty-four of those profiles. The correlation is the confidence, and it does the weighting: a passage with no key in it pulls at nothing. Turned up, the music acquires a home it keeps returning to, and the root wanders to stable degrees rather than away from them, which is what a modulation is. At 0 the conductor hears intervals and no key at all, exactly as it always did." },
    { "brain_harmonic", "How much the conductor judges a chord by whether the WHOLE of it fits one harmonic series, rather than only by how its pairs sound. The two are not the same question, and for more than two voices they can disagree flatly: scored pairwise, a stack of fifths beats a just major triad, and a plain segment of the harmonic series comes last of all, because neighbouring members of one series make complicated ratios two at a time however perfectly the set fits together. Turned up, the conductor looks for the chord with one root -- which is what listeners respond to at least as strongly as they respond to the absence of beating. 0 is the pairwise judgement it always had." },
    { "brain_spacing", "How the conductor treats two notes that fall inside one critical band. Above zero it avoids them: tones closer than about an equivalent rectangular bandwidth excite overlapping places on the basilar membrane, and the ear fuses them into one rough sound rather than hearing two, so a cluster spread wider than a critical band stays audible as separate voices. Below zero it seeks them out, which is what a cluster is for. 0 leaves the choice as it always was." },
    { "brain_timbre", "How much the conductor judges an interval by the spectrum it actually plays rather than by the ratio alone. The ratio score says a fifth is consonant because 3:2 is simple. The spectrum score (after Sethares) sums the roughness of every pair of partials the two tones would make -- the bank's own tilt, brightness, odd/even weight and inharmonic stretch -- so with an inharmonic timbre the consonant intervals move, as they do on a bell or a stretched string, and the conductor moves with them. 0 is the ratio score it always had." },
    { "brain_wander", "How readily the brain's root moves to a new centre over time." },

    // ---- tuning
    { "scale", "The tuning: just scales, 12-TET, Bohlen-Pierce, User for a loaded Scala file, or Timbre. Timbre is not a table: it is the scale THIS sound asks for, computed while it plays. The instrument's own roughness curve is swept across the octave -- how rough the timbre is against a transposed copy of itself -- and wherever the curve dips, a degree goes. For a harmonic spectrum those dips are just intonation, to within a couple of cents, which is the reason just intonation exists at all; turn Inharmonic up and they move somewhere else entirely, because a stiff string wants a different scale and this gives it one. Change Tilt, Brightness or Inharmonic and the tuning follows the sound. Every note the brain or the keys play comes from here." },
    { "keymap", "Snap: the 12 keys of an octave snap to the nearest scale degree. Consecutive: each key is the next degree, whatever the scale's step count." },
    { "root", "The key's root note. The brain's root lives an octave below it." },
    { "ref_pitch", "Reference pitch of A4 in hertz." },
    { "seed", "Seed of the brain's randomness: the same seed replays the same decisions." },
    { "hold", "Keys latch: a played key stays until Hold is switched off." },
    { "purity", "Blends every note between 12-TET (0) and the chosen scale (1) in the log domain -- the beating locks in as you turn it up." },
    { "purity_drift", "Lets the purity wander, so the tuning locks in and loosens over minutes." },
    { "purity_rate", "How fast the purity wanders." },
    { "tide", "The whole instrument's pitch leans by up to this many cents on a very slow curve, like a tape machine over an evening. Sub and voices move together, so the harmony stays." },
    { "tide_period", "Minutes for one swing of the tide." },
    { "strike_level", "A short plucked or struck impulse at note-on on the near plane, whatever the voice's distance: the intimate contrast that makes the background vast. Level; 0 is off." },
    { "strike_type", "String: a plucked string at the note (Karplus-Strong). Wood: a short, dull knock two octaves up. Metal: the string with an all-pass in its loop, stretched and clangorous." },
    { "strike_decay", "Seconds the strike rings." },
    { "strike_damp", "Brightness loss per round of the string: 0 bright and long, 1 dull and short." },
    { "strike_who", "Keys: only your notes strike. Keys + Brain: the conductor's notes too." },
    // ---- the near layer (13.09.2026)
    { "fore_type", "The near source: a fifth slot of its own, rendered only by the near events. Any of the source types, with the near ones made for it -- Flute, Murmur, Bowl, Ice, Drops, Clip (a recording straight through, once) -- and the signals: Whistler (a lightning stroke's whistle falling through the magnetosphere; Bright how high it begins, Speed how fast it falls), Shaker (a seed pod: Density the shakes a second, Force how long the beans keep moving, Position the shell's pitch, Noise Q its ring), Chime (struck bronze with the beating of its doublets: Force how long it rings, Pos Drift the beating, Tilt the hum under it, Bright the high partial), Geiger (clicks on a Poisson clock at Density a second, Force the chance of a cluster, Position the tube's pitch, Noise Q its damping), Tube (a fluorescent tube: the starter's clicks, the choke's hum at 100 or 120 Hz by Position, the plasma's hiss by Bright), Krell (FM steered by a Roessler attractor: Speed its pace, Position past the middle snaps the carrier to semitones, FM Ratio and FM Index as in FM), Beacon (a chirp and eight bits of frequency-shift keying, a packet every 1/Density seconds), Morse (five-figure groups at Speed words a minute, Position past the middle letters instead of figures, Bright the ionosphere's flutter), Dial (a shortwave dial turned: heterodyne whistles wandering, Position how many, Bright the band's noise). The clip types (Clip, Texture, Stretch, Spectral) read the near source's own clip -- the one a near preset names from the library's archive, or the one opened with the button -- and Source 4's where it has none." },
    { "fore_octave", "The near source's octave against the note the event chose." },
    { "fore_ratio", "A just ratio on top of that, as a slot has." },
    { "fore_pos", "Position, as the type reads it: the embouchure of the Flute (0 the fundamental, 1 the pipe overblown to its octave), where the stick sits on the Bowl (rim to belly), the medium of the Murmur (0 a voice in the room, 1 a radio with its band, its hiss, its squelch and the Quindar tones), the vessel the Drops fall into (a cup to a cistern), or what it means for any other type." },
    { "fore_pos_drift", "Pos Drift: the Flute's vibrato, the Bowl's beating (how far apart the two halves of every mode sit), the Murmur's intonation, the Drops' spread of sizes." },
    { "fore_density", "Drops a second, or grains, or crackles, for the types that count." },
    { "fore_follow", "Pitch = Note puts the Drops' bubbles on the note's own partials -- the wet marimba of a cave -- and does for the clip and noise types what it does in a slot." },
    { "fore_bright", "Bright: the pipe's end filter, the bowl's upper modes, the voice's clarity, the bubbles' size." },
    { "fore_force", "Force: the Flute's breath, the stick's pressing, the Murmur's effort, the Bow's hand." },
    { "fore_speed", "Speed: the Flute's air noise, the stick's rubbing, the Murmur's syllable rate, the Bow's travel." },
    { "fore_noise", "The colour of a Noise near source, Cicada among them: a chorus of insects, two per ear, singing in bursts and resting between." },
    { "fore_noise_q", "The band's width for the Noise colours that have one." },
    { "fore_fm_ratio", "FM: modulator over carrier." },
    { "fore_fm_index", "FM: how deep." },
    { "fore_partials", "Additive: how many partials the near bank has." },
    { "fore_tilt", "Additive: partial h at h to the minus Tilt." },
    { "fore_inharmonic", "Additive: the series stretched like a stiff string -- the gamelan and the glass." },
    { "fore_drift", "Cents of slow, independent pitch drift on the near source." },
    { "fore_table", "The table for a Harmonic or Wavetable near source." },
    { "fore_attack", "The near event's own envelope: seconds to swell in. A pluck is five milliseconds, a flute a third of a second, a bowl two." },
    { "fore_decay", "Seconds from the peak down to Sustain." },
    { "fore_sustain", "Where the event holds while it lasts; 0 with a short Decay is a pluck." },
    { "fore_release", "Seconds the event takes to go once its Length is over, or the gate of a sequence's step." },
    { "fore_cutoff", "The near voice's own filter, so the foreground's brightness does not depend on the background's. A sequence's Bloom breathes around this." },
    { "fore_resonance", "Its resonance." },
    { "fore_filter", "Its model -- the Ladder for the Berlin pluck, a formant for a voice, the plain 12 dB for everything that should simply be near." },
    { "fore_filter_env", "How far the event's envelope opens the filter: a pluck that closes as it dies." },
    { "fore_strike", "A Karplus-Strong strike at every near note, with the three settings beside it. The koto, the harp, the kalimba: with the near type Off the strike is the whole sound." },
    { "fore_strike_type", "String, Wood or Metal, as in the Strike section." },
    { "fore_strike_decay", "Seconds the strike rings." },
    { "fore_strike_damp", "Its brightness loss per round." },
    { "fore_level", "The foreground's level; 0 is off, and it is off in every preset that does not ask for it. The near events play the Near Source, close to the ear, on a clock of their own -- a flute blown once, water in a bowl, a voice on a radio, a rubbed rim, or a Berlin-school line -- and the far reverb ducks under them where Far Unmask is up." },
    { "fore_kind", "Note: one tone, held for Length. Phrase: two to four tones over the Length, each a slide to another consonant degree on the portamento and its gravity, the last one home to the first, and at the end the flute's meri -- a quarter tone down and back. Sequence: a ring of Steps notes on a clock of its own that breathes (the tempo on a drifter, every step a few milliseconds early or late), transposed with the conductor's root, mutating one step at a time -- its degree, its octave, its gate, its velocity or its accent -- thinning and thickening on a slow gate, arriving out of the far plane and leaving into it." },
    { "fore_chance", "The coin at each due moment: 1 plays every one, 0.3 about one in three." },
    { "fore_cluster", "The coin weighted by the conductor's cascade, as the Strike's is: the events then come where the music is excited and stay away from its stillest stretches." },
    { "fore_length", "Seconds an event lasts -- a note's hold, a phrase's whole arc, a sequence's whole run (minutes)." },
    { "fore_pitch", "Consonant: a degree of the tuning consonant with the root AND with everything sounding -- the fifth, the ninth, the pure third first -- in the soloist's register: at least a fifth above the highest body voice, or in a gap of a third or more, and never within a critical band of a sounding voice, where the drone would mask it. Highest, Lowest, Root: that note of the cluster. Cluster: any note of it." },
    { "fore_spread", "How far from the centre an event may sit; each is placed anew." },
    { "fore_approach", "The share of the event spent arriving out of the far plane to its Distance and, at the end, leaving into the far again. A sequence at 0.25 spends its first quarter as a pulse on the horizon and its last quarter dissolving back into it. Negative: the event begins at its Distance and leaves from the first moment -- a whistler that starts at the ear and falls away over the horizon." },
    { "fore_distance", "Where the event sits between the planes: 0 at the ear, 1 on the horizon, and the near field's lift, the width and the far reverb's share follow it. A foghorn at 0.85 stands deep in the far field; a rattle at 0.05 stands twenty centimetres from the nose. Approach arrives here from the horizon." },
    { "fore_auto", "A sound preset from a pack brings its own foreground: each artist has a table (which near presets, how often, and what share of the artist's presets get one at all), and the preset's name decides, the same way every time, so a preset always brings the same one. Nine in ten presets or more bring one; the few that do not switch the foreground off while they play. Choosing a near preset by hand switches Auto off and keeps yours; switching it on again draws for the preset that is playing." },
    { "fore_dry", "The share of the event's voice that goes straight to the output, past the near reverb, the far reverb and every delay. At 1 a Geiger click is a needle and nothing softens it; at 0 the event takes the same way through the room as everything else." },
    { "fore_delay2", "An extra share of every event into the second delay's input -- a send, added to what that delay already hears, so the event keeps its place and the long chain gets more of it. A beacon that answers itself across a minute while the bed stays dry." },
    { "fore_gain", "The foreground's gain in decibels, after its source and before the room it plays into. Level is the source's own level, and for a blown jet, a bow or a clip it shapes the sound as much as its loudness; Gain only makes it louder or quieter. The presets of the near bank carry a measured Gain that brings their loudest moment to the loudness of the background they play over, so the bank's foregrounds arrive about equally present." },
    { "fore_cosmos", "An extra share of every event into the Cosmos, on top of the section's own Send. With the section's Send at zero this puts the foreground alone into the deep space: the events are shifted, resonated and smeared while the bed is left where it is. The Cosmos' Return decides how much of it comes back." },
    { "fore_proximity", "The near field's lift on the event's voice, up to six decibels between 120 and 300 Hz -- a band, not a shelf, so the Foundation and Bass Mono below it are left alone -- and a function of the event's plane: full when it stands at the ear, gone when it has moved to the horizon. The proximity a source within reach has, which is what makes a breath sound at the lips rather than in the room." },
    { "fore_hold", "While a Note or a Phrase sounds, and for ten seconds after it, the conductor begins no new note: its holds run on, its releases happen, but the background stops moving under the soloist. A sequence asks something else of it: the root stays where it is and the events come half as often, so the line is never thrown across a changeover." },
    { "fore_rate", "Mean seconds from the end of one event to the start of the next, drawn as the conductor draws its gaps: exponential, shifted, so that there is never a pulse and never two at once. And into stillness: when an event is due it waits, up to half the rate, for four seconds without an onset or a release of the conductor's -- so it answers the background rather than interrupting it." },
    { "fore_glide", "Phrase: seconds of the slide to the second degree; 0 takes two fifths of the Length." },
    { "fore_steps", "Sequence: the ring's length. Seven and eleven never sound like four-four." },
    { "fore_step", "Sequence: seconds per step, when Step Sync is Free -- the eighths of a slow Berlin line are a third to half a second." },
    { "fore_step_sync", "Sequence: the step as a note value of the clock instead." },
    { "fore_mutation", "Sequence: the chance, once per cycle, that one step changes -- a new degree or an octave flipped. The shift register's one bit: at 0 a loop, at 0.15 a line that is a different theme five minutes on and was never heard to change." },
    { "fore_scatter", "Sequence: ghost notes between the steps at a third of the level, and the odd accent sent to the far plane, where it leaves a trail through the delays and the reverb while the line taps on dry in front." },
    { "fore_bloom", "Sequence: the filter breathing over the run -- two octaves under Cutoff as it begins, an octave over at its height, and back down as it goes." },
    { "cosmos_swell", "Lets the Cosmos breathe with the conductor's cascade: where its excitation is above what this piece has been running at, the send opens; in the long gaps between clusters it closes again, so the parallel world comes and goes instead of humming at one level all night. On at a half by default, and it does nothing at all until the Cluster Brain's Cascade is up -- without a cascade there is no excitation. Smoothed over two seconds, so it swells and never steps." },
    { "strike_chance","How many of the conductor's notes strike: 1 every one of them, 0.2 about one in five, 0 none. Your own keys always strike, whatever this says. It does nothing while Fires is on Keys." },
    { "strike_cluster", "Lets the strikes follow the conductor's cascade instead of falling independently: where its excitation is up, the chance is lifted with it, so the strikes arrive in handfuls with long silences between. 0 is an even coin at Chance; at 1 a cascade at full excitation makes a strike near certain. Needs the Cluster Brain's Cascade to be up -- without it there is no excitation to follow." },
    { "stretch", "The stretched octave, in cents per octave away from the reference pitch. Listeners prefer octaves a little wider than 2:1 -- ten to twenty cents at the extremes of the range -- and a piano is tuned that way; here every octave above A4 is that much wider and every octave below that much narrower, the reference itself staying put. 0 is the exact 2:1 of every preset that was ever saved." },
    { "portamento", "Seconds a new key glides from the last one." },
    { "porta_gravity", "Slows the glide near consonant ratios to the root, so a slide clicks into the harmonic nodes on the way." },

    // ---- coherence
    { "coherence", "Coupling of four slow Kuramoto oscillators: at 0 they run free, near 1 they fall into step. Their sines are the KURA modulation sources." },
    { "coherence_depth", "How much the ring moves brightness, depth, pan and the z-plane point on its own." },
    { "sympathy", "The voices hear each other: the previous block's foreground is fed back into every voice at low level, through that voice's own filter. Strings on one soundboard do this, and with the Comb or Formant model it is unmistakable -- each voice rings at what it is tuned to when another plays. Kept small on purpose; it is a loop." },
    { "coherence_rate", "Base speed of the ring." },

    // ---- LFOs (shared)
    { "lfoN_shape", "The waveform: Sine, Triangle, Ramp Up, Ramp Down, a soft Square, Random (smooth), Steps (held random), or Table -- a frame of the user wavetable as a shape, which makes any drawn curve an LFO." },
    { "lfoN_rate", "Cycles per second, from one in twenty minutes to 20 Hz. Ignored while Sync is set." },
    { "lfoN_phase", "Where in the cycle the shape starts (0..1)." },
    { "lfoN_depth", "Scales the LFO's output; every matrix route scales it again." },
    { "lfoN_mode", "Global: one phase for the whole instrument, every voice breathes together. Voice: each voice runs its own copy. Retrigger: each voice restarts from Phase." },
    { "lfoN_table", "Which frame of the user wavetable the Table shape reads." },
    { "lfoN_sync", "One cycle per note value at the current tempo; the phase follows the beat position, so it stays on the grid wherever the transport jumps." },

    // ---- envelopes (shared)
    { "envN_shape", "The curve itself is edited on it: drag a breakpoint to move it in time and level, double-click the line to add one or a point to remove it, right-click for the sustain point, the loop, the curvature of a segment, and ten shapes to start from -- ADSR among them. Up to sixteen points, each with its own curve, which is a good deal more than an ADSR when you want it and exactly an ADSR when you do not. The first point stays at the start; use the matrix or a delay if you want it to begin late." },
    { "envN_mode", "One Shot plays the shape once per note. Loop repeats it between its loop points. Sustain Loop loops while the note is held, then finishes." },
    { "envN_time", "Stretches the whole shape: 0.05 is twenty times faster, 20 twenty times slower. Ignored while Sync is set." },
    { "envN_depth", "Scales the envelope's output before the matrix." },
    { "envN_sync", "The whole shape spans one note value at the current tempo." },

    // ---- morph, macros, map, route
    { "morph_active", "Switches the morph on: the whole instrument is the blend of snapshots A and B at Position. An end nobody has chosen plays as the knobs stand, so this changes nothing until A or B is set." },
    { "morph", "Where between A (0) and B (1) the instrument is. Continuous parameters interpolate in their own curve, choices flip halfway." },
    { "morph_glide", "Seconds the instrument takes to follow a new position -- up to fifteen minutes, so one gesture can carry a piece." },
    { "macro_a", "Macro A -- Space: one knob, several parameters, mapped in the gesture table (Gestures...)." },
    { "macro_b", "Macro B -- Alien." },
    { "macro_c", "Macro C -- Motion." },
    { "macro_d", "Macro D -- Bloom." },
    { "macro_e", "Macro E -- Density." },
    { "macro_f", "Macro F -- Distance." },
    { "macro_g", "Macro G -- Evolution." },
    { "macro_h", "Macro H -- Air." },
    { "inertia", "Every knob glides to its value with this time constant, the analogue slew: even a knob torn open arrives slowly. Modulation is not slewed." },
    { "map_active", "Plays the blend of the presets around the map cursor (Browse > Map) instead of the live parameters." },
    { "map_x", "The map cursor's horizontal position." },
    { "map_y", "The map cursor's vertical position." },
    { "map_radius", "How far around the cursor presets contribute to the blend." },
    { "route_active", "Walks the route of waypoints over the map (Browse > Map)." },
    { "route_speed", "Speed of the route, 1 = as written." },
    { "route_loop", "Starts the route again when it ends." },

    // ---- clock
    { "clock_source", "Where the tempo comes from: Internal (Tempo and Run here), Host (the DAW's play head, if there is one), or MIDI clock at the input. Host and MIDI fall back to Internal when nothing arrives." },
    { "tempo", "The internal clock's tempo in beats per minute. Every Sync choice in the instrument reads it (or the host's / MIDI's tempo instead)." },
    { "clock_run", "Runs the internal clock. Off, synced LFOs hold their phase." },
    { "lfoN_mode", "Global: one phase for the whole instrument, and every voice breathes together. Retrigger: the phase goes back to where Phase says whenever a note arrives into silence -- the start of a phrase, not of every note in a cluster. Per Voice is not honoured on this instrument and is kept only so that presets naming it still load: the modulation matrix is computed once a block for the instrument, not once per voice, so there is no per-voice copy to hand it. What per-voice movement there is comes from the places that have it -- the strands' own drift, the shimmer, Rate Wander." },

    // ---- the voicing rules (12.09.2026). Every one of these does nothing at its default, so a
    // preset written before them plays exactly as it did. They come from Rene's rule book for a
    // self-playing generator, and the paragraph numbers in it are given where a rule is quoted.
    { "brain_layers", "Register as a role rather than as a range. A conductor that draws from one weighted list treats the bottom of its register like the top, and the ear does not: two tones a third apart are a chord at C4 and mud at C2, because the critical band is a fixed fraction of the frequency and therefore an enormous interval down there. Turned up, the draw becomes a pyramid -- one voice in the foundation, most in the body, few in the colour, the air rarest of all -- and at 1 no octave holds more than two notes, the octave below MIDI 36 only one. The roles also keep their own times: under it a colour note holds half of what Hold Min to Hold Max drew, an air note a quarter, a second foundation voice four times as long (the lowest voice is Bass Hold's), and the octave and the unison, which the conductor's own taste keeps rare, are as welcome as the rule book's table has them." },
    { "brain_bass_hold", "Multiplies the hold time of whichever voice is currently lowest. The foundation lies still while what stands on it moves: at 4 a bass note outlasts four turns of the voices above it, which is the difference between a drone with a floor and a cluster that happens to reach low." },
    { "brain_top_soft", "Velocity falls with height. In this music velocity is not loudness but nearness -- the foundation is near and steady, the air far and quiet -- and at 1 the top of the register arrives at forty per cent of the bottom's velocity, which is the rule book's table: 70 to 100 for the foundation, 20 to 45 for the air. It also narrows the draw, so that within one role velocity varies by twelve at most; at 0 the draw is the 0.5 to 0.9 it always was." },
    { "brain_low_spacing", "Sharpens Spacing as the register falls. Two tones closer than a critical bandwidth fuse into one rough sound, and that bandwidth in semitones is huge in the bass and small in the treble. At 1 the floor is an octave below MIDI 36, a fifth to 47, a minor third to 59, a second to 71, and above 72 nothing changes -- clusters up there are the point." },
    { "brain_third_floor", "The note below which no third is chosen, major or minor. A third in the bass is the single most reliable way to make a drone muddy; 48 is the usual place to put the floor. 0 is off." },
    { "brain_leading", "Penalises the semitone under a sounding root. A leading note pulls somewhere, and this music has nowhere to go: it promises a resolution that will never arrive. At 1 it is excluded outright, except inside a Pivot window, where it is exactly the right tone." },
    { "brain_thirds", "Moves the weight of thirds, major and minor together. Negative avoids them -- the empty fifth-and-octave sound of the dark profiles -- and positive seeks them out, which is what makes a consonant, lit harmony. Judged against every sounding note, not against the root alone." },
    { "brain_seconds", "The same for seconds and ninths, and the reason it is a separate knob: a second is a colour above MIDI 60 and mud below it. Seeking them counts only above the third floor; avoiding them counts everywhere -- and avoiding takes the minor second and the major seventh, which is a minor second turned upside down, at their full weight, the major second at a third of it, because that is the proportion the rule book's own table of intervals has between them." },
    { "brain_seventh", "Lifts the minor seventh. In a just scale that is 7/4, thirty-one cents below the tempered one -- the one interval in this music that fuses with the root instead of pressing against it, and the most characteristic sound it has. In 12-TET it is the tempered seventh, and the lift still helps." },
    { "brain_degree_swap", "The chance that a root change also exchanges a single degree of the supply -- a major sixth for a minor one, a major third for a minor. The mode wanders instead of being switched, which is the only way this music changes colour without announcing it." },
    { "brain_rate_breath", "Lets the mean gap between events breathe. A constant mean is the one thing a Poisson clock cannot hide: the density is the same at minute three and at minute fifty, and the piece has no shape. At 1 the mean swings between half and double, on a curve made of two waves whose periods stand in the golden ratio, so the tide never repeats and never becomes a pulse." },
    { "brain_breath_period", "How long one breath takes. Without Rate Breath it does nothing." },
    { "brain_overlap", "Nothing begins and nothing ends in the void. A voice may not be let go until the one replacing it has been sounding for this long, so a change is always heard inside a chord that is still there. It only postpones -- the note goes as soon as the condition is met. Ten seconds is the rule of thumb; with long releases, less." },
    { "brain_onset_guard", "The thirty-millisecond rule. Two tones either begin within thirty milliseconds, in which case the ear fuses them into one sound, or more than three seconds apart, in which case they are two voices. In between they are heard as one chord played inaccurately -- a machine, not an instrument. An event falling in that gap waits for the far side of it. The entrance after a preset change is exempt, or a cluster walking in would never assemble." },
    { "brain_release_gap", "The smallest time between two note-offs. Without it a cluster whose voices were started together can collapse together, which is the one ending this music must not have." },
    { "brain_retrigger", "How long a pitch must rest after it ends before it may be chosen again. A pitch that has only just been let go is not a new note, it is the same note again -- the one repetition this music notices. Thirty seconds is the rule." },
    { "brain_density_slew", "How long a change of Density takes. A change -- by hand, by a macro, by the Arc -- is then carried out one voice at a time over these minutes instead of at once: five voices appearing together is an edit, not a piece of music. 0 changes immediately, as it always did." },
    { "brain_silence", "The chance that a root change is also a pause: every voice is let go and the room stays empty for a while. Complete silence belongs at a section's border and nowhere else -- once or twice an hour is the rule, and the guard is that this can only happen where the root moves." },
    { "brain_silence_len", "How long that pause lasts, give or take a third. When it ends the cluster walks back in at the entrance pace rather than waiting out a draw made before it." },
    { "brain_root_steps", "Where a new root may come from. Any is the draw the conductor always made. Fifths keeps it to the fourth and the fifth, the two moves that change everything and disturb nothing. Diatonic adds the minor third and the major second, and the descending semitone, which is how this music actually travels. Falling allows only downward steps. All but Any forbid the ascending semitone outright: it is heard as a lift, and there is nothing here to lift towards." },
    { "brain_root_down", "Which way the root leans. A fifth down and a fifth up are one interval to the scoring and are not one move: downwards the music settles, upwards it climbs." },
    { "brain_pivot", "The changeover window. Without it the root simply moves. With it the new root is announced rather than declared: a tone belonging to both roots begins at once, the root itself follows halfway through the window, and the voice on the old root is let go only at its end. Twenty to sixty seconds. A listener hears the harmony turn instead of switch, which is the whole of the difference." },
    { "brain_home", "The pull back to the root the night began on. It grows with the hours and with how far the root has travelled, so a piece left running comes home without ever being told to -- and a piece switched off after ten minutes never notices it was there. That return is the only form this music needs." },
    { "brain_home_time", "When that pull is at its strongest. It is also what Root Age in the matrix is measured against, so an hour-long night and a three-hour one read the same." },
    { "brain_memory", "How long a chord may not return. The conductor remembers each constellation it has made -- the pitch classes that sounded and the octave the bottom of it sat in -- and will not build the same one again inside this time. Single pitches may come back as often as they like; that is Deja Vu's business. Constellations may not: a repeated chord is the moment a listener starts to hear a loop." },
    { "brain2_golden", "Stretches the second conductor's rate and hold times against the first by the golden ratio -- the one number with no good rational approximation, so the two clocks can never fall into a simple relation and be heard as one layer. It overrides Brain 2's own Rate and Hold, which is the point: a ratio cannot be guaranteed while both ends are set by hand." },
    { "tuning_hold_sounding", "When the root moves, what is already sounding keeps its frequency; only notes beginning afterwards take the ratios to the new root. Off, every voice glides -- and the whole harmony slides at once, which takes the floor out from under the piece. The harmony should change by what enters, not by everything moving together." },
    { "beat_ceiling", "The fastest beat the strands may make. Two tones a few cents apart beat at the difference of their frequencies, so the same detuning beats faster the higher the note sits: warmth at 300 Hz is a wobble at 1 kHz. This is the flat upper bound on the rate, and under 150 Hz it is halved again, because down there anything above half a hertz reads as movement rather than as depth. 0 is off." },
    { "env_vel_attack", "Velocity as an attack time rather than as a level. Positive lets a quiet note enter slower and a loud one faster -- the air floats in, the foundation simply stands -- and negative does the opposite. With Layers and Top Soft, which make the high voices the quiet ones, this is what gives each register its own way of arriving." },
    { "layer_depth", "Takes a note's plane away from the dice and gives it to its role: foundation and body near, colour in the middle, air and shadow far back. At 0 the conductor places as it always did, four notes in ten intimately close and the rest deep in the background." },
    { "strand_low_detune", "Thins the strand detuning out towards the bottom of the range. A beat of a hertz is warmth at 300 Hz and a wobble at 100, because the same cents make a slower beat the lower the note: at 1 a note under 150 Hz keeps half the detuning of one above 300. It works on Detune and Drift together, and leaves the upper range exactly as it was." },
};

/**
 * @brief Normalises a key to its family template: src2_pos -> srcN_pos, lfo3_rate -> lfoN_rate,
 * env5_depth -> envN_depth, dly2_mix -> dly_mix.
 *
 * Returns whether anything changed.
 * @param key  a parameter key from paramTable()
 * @return     the family template, or the key itself when it belongs to no family
 */
std::string familyKey(const char* key)
{
    std::string k(key);
    auto digitAt = [&](size_t i) { return i < k.size() && k[i] >= '1' && k[i] <= '9'; };
    if (k.rfind("src", 0) == 0 && digitAt(3) && k[4] == '_') { k[3] = 'N'; return k; }
    if (k.rfind("lfo", 0) == 0 && digitAt(3) && k[4] == '_') { k[3] = 'N'; return k; }
    if (k.rfind("env", 0) == 0 && digitAt(3) && k[4] == '_') { k[3] = 'N'; return k; }
    if (k.rfind("dly2_", 0) == 0) return "dly" + k.substr(4);
    return k;
}

/**
 * @brief Every parameter's help text, resolved once: the answer for each ParamId by its own key or by
 * its family template, so paramHelp() is an index and not a search.
 */
struct HelpCache {
    const char* text[kNumParams];   ///< indexed by ParamId; "" where kHelp has nothing for the key or its family
    /** @brief Walks paramTable() once and fills text[]; the first entry of kHelp that matches, by key or by family, wins. */
    HelpCache()
    {
        for (const ParamDesc& d : paramTable()) {
            const char* found = "";
            const std::string fam = familyKey(d.key);
            for (const HelpEntry& e : kHelp)
                if (std::strcmp(e.key, d.key) == 0 || fam == e.key) { found = e.text; break; }
            text[static_cast<int>(d.id)] = found;
        }
    }
};

/**
 * @brief The one HelpCache, built on first use; the static's own initialisation makes that thread-safe.
 * @return the cache, which never changes afterwards
 */
const HelpCache& cache() { static const HelpCache c; return c; }

// ---------------------------------------------------------------- the manual

/** @brief One chapter of the manual: its title and its text, as the Help page prints them. */
struct Topic { const char* title; const char* text; };
/**
 * @var const char* Topic::title
 * @brief The chapter heading, which is also its entry in the Help page's list.
 */
/**
 * @var const char* Topic::text
 * @brief The chapter as a raw string literal, its paragraphs separated by blank lines.
 */

/**
 * @brief The manual, chapter by chapter, in the order the Help page lists them: overview and signal
 * flow first, the reading list last. numHelpTopics() counts this table.
 */
const Topic kTopics[] = {
    { "Overview and signal flow",
R"(Noctuary is a drone instrument for slowly breathing clusters: just intonation, additive banks whose partials live their own lives, envelopes measured in minutes, a conductor (the Cluster Brain) that can play a whole night by itself, and a spatial model that treats depth as a landscape rather than an effect.

SIGNAL FLOW

  Cluster Brain / MIDI keys
        each note gets a DISTANCE: 0 at the ear, 1 the infinite background
  Voice (x16)
        Source 1 + Source 2 + Source 3 + Source 4   (four equal slots: additive bank,
              harmonic table, wavetable, FM, texture grains, spectral stretch, noise)
        the Vector reads the four as the corners of one square; Strike adds a struck body
        + Air (filtered noise on the note)
        -> Filter (ten models, wavefolder) and/or Z-plane filter, in series or parallel
        -> Envelope, x (1 - distance/2)
        -> interaural time difference from the pan (the far ear hears later)
        -> NEAR bus by cos(distance), FAR bus by sin(distance)
  Near layer (its own bank of 108 presets, beside the sound presets)
        Near Events: a second, small conductor -- it waits, picks a degree consonant with
              what sounds, plays ONE thing close to the ear, and waits again
        Near Source: 25 types (flute, bowl, ice, drops, voice, the nine signals, a
              Berlin-school sequence) or a recording played straight through
        -> the same two buses, at a Distance of its own (Approach walks it in or out)
        -> Dry: a share straight to the output, past every reverb and delay
        -> To Delay 2 and To Cosmos: a share of every event added to what those hear
  NEAR:  Ensemble (chorus or microshift) -> Delay -> Delay 2 -> (+ Near reverb + Haas band)
         "to far" from both delays and the Cloud send go into the background
         Cosmos (send / return): shifter, resonator, vowel, nebula -- added, never replacing
  FAR:   Far reverb (dark, wide, asymmetric, minutes long, with its own width)
         + Room (convolution) + Shimmer loop, unmasked band by band under the foreground
  Body (twelve tuned modes) -> mid/side (bass mono, side air, width) -> + Foundation sub
         -> Patina -> subsonic -> Master -> soft clip.   No compressor anywhere.
  Feedback: the finished mix can return into the near bus and/or bend every partial's phase.

A voice's plane decides everything at once: how bright it is (2.5 octaves of cutoff per unit of distance), how loud (-6 dB), how dry (the far plane is heard only through the reverb), and how present (the presence bell lives on the near plane). The brain places 40 % of its notes close and 60 % deep; your keys sit at Keys Depth.

THE PAGE

Everything is on one page and nothing scrolls; drag the window corner to zoom. Rows whose sections are of a kind page through tabs: SOURCE 1 / SOURCE 2 / SOURCE 3 / SOURCE 4 / VECTOR, FILTER / Z-PLANE / AMP ENV / EXPRESSION, the effect pairs, COSMOS / STRIKE, BRAIN / AUTOPLAY / BRAIN 2 / TUNING / COHERENCE / CLOCK, MORPH / MACROS. The strand bank has no tab of its own: it belongs to Source 1's additive type alone and sits under that page's display. The header carries the pages -- Main, Perform, Browse, VR (calibration and gestures) and Help at the end. The room a row's knobs leave is a live display drawn from the engine's own numbers. The strip along the bottom holds the modulators. Point at any control and this header line tells you what it does.)" },

    { "Sources",
R"(Every voice has four equal source slots; their levels mix before the filter. Each slot has its own clip for the Texture type, so four slots can play four different recordings. Each slot has a Type:

ADDITIVE  A bank of up to 32 partials. Partial h has amplitude h^-Tilt, the Brightness window fades the upper ones out, Odd/Even weights the two families, Inharmonic stretches the series like a stiff string, and Shimmer lets every partial drift in level on its own slow curve -- the breathing. Partials above Nyquist are not generated, so nothing aliases. In Source 1, Additive is the STRAND BANK: up to six copies of the bank, detuned (Detune, Drift) or placed on pure ratios (Stack: octaves, fifths, a just major or minor, seventh, harmonics, subharmonics -- one key becomes a just chord), fanned out in stereo (Spread), with Bloom opening the brightness over Bloom Time and Rate Wander slowly varying every movement rate. In Source 2 and 3, Additive is a single bank with its own Partials, Tilt, Bright, Odd/Even, Inharmonic and Shimmer.

HARMONIC  A table not of samples but of SPECTRA: 32 partial amplitudes per frame, up to 64 frames; Position morphs between frames and Pos Drift wanders it. Five built-in tables (Classic: sine to pulse; Organ; Vocal a-e-i-o-u; Glass; Metal) and User, loaded from a WAV in the Serum/Vital layout (2048-sample frames) with the button. Alias-free like the bank, and the same partials-based tricks (presence, low cut, feedback FM) apply. Transport decides how the frames morph: at 0 the amplitudes are blended, which is what every wavetable does and why a morph between two formants sounds hollow half way; turned up, the partials WALK along the frequency axis instead, so a formant travels rather than fading out while another fades in. Root puts a floor under the fundamental, for a table cut from a recording that had none. (This type was called Wavetable until the classic one below was built, and a preset written then still reads correctly: the name moved, the sound did not.)

WAVETABLE  The classic kind, and the other half of the pair: single cycles read as SAMPLES rather than rebuilt from partials, eight times oversampled and mip-mapped per octave so nothing aliases however high it is played. The same tables and the same User slot as the Harmonic type, the same Position and Pos Drift; what differs is that a cycle keeps its phase relationships, which is what gives the hard, resinous character a spectrum cannot -- and that it has no partials to reach into, so the tricks that work on the bank do not apply here. Unison detunes copies of it across the field, as it does for the Harmonic type.

SPECTRAL  A recording played back from a model of its bands instead of from its samples. Two numbers a sampler has to share are separate here: the note scales every band's frequency, so it decides the PITCH, and Rate scales how fast the model is read, so it decides the SPEED -- and neither touches the other. At Rate 1 the clip runs at its own pace; at 0 the read head stands still and the clip becomes one endless chord, a frozen moment of a recording held at whatever pitch is played. Each band is rebuilt from two things: an oscillator at the band's strongest partial, weighted by how tonal that band measured, and a band of noise at the same place, weighted by the rest. Breath tilts the balance -- all the way to the partials, and a rain recording turns into the chord hiding inside it; all the way to the noise, and a struck bell turns into the wind that has its shape.

FM  A two-operator pair: carrier at the slot pitch, modulator at FM Ratio, FM Index up to 8, reduced automatically on high notes. Pos Drift wanders the index.

TEXTURE  A granular player over a loaded clip (Texture... button, or the preset's own sample): up to 64 grains (Grains), Grain length, Density per second (or per note value with Sync), starting around Position with Spread, pitched to the note (Pitch = Note; the clip's pitch comes from its file name, e.g. "_A3") or played free. The display shows the grains reading the clip.

STRETCH  The same clip read as a continuum instead of as grains: a spectral time stretch, after Paulstretch -- a window (Grain) of the clip is transformed, its magnitudes kept, its phases drawn afresh and the result overlap-added, while the read position crawls through the recording at one Stretch-th of its speed. No grain rhythm, no transient left standing: a field recording becomes weather. Position is where it reads (Pos Drift wanders it), Pitch = Note or Free is applied by resampling BEFORE the stretch so a note played higher does not get shorter, and the loop's seam is crossfaded by Loop Fade unless the clip's name carries _loop, which marks it seamless.

BOW  A bowed string, continuously excited rather than struck: a waveguide of one period with a
two-point loop filter, and at one point on it the friction of a bow -- Force against Speed,
through the Stribeck curve that makes a string stick while the relative velocity is small and slip
once it is not. That stick-slip alternation is the Helmholtz motion of a real bowed string, and it
is why the note sustains for as long as the bow moves instead of decaying like the Strike. Position
is where the bow sits along the string (away from the ends, and never quite the middle, where the
even partials would be lost); Bright is how much top the string keeps as it goes round its loop.

NOISE  Eleven colours: White, Pink, Brown, Blue, Violet, Grey, Band (a resonant band at Position, Q from Noise Q, tracking the note with Pitch = Note), Wind (a wandering band), Crackle (sparse impulses at Density), Digital (sample-and-hold at a rate from Position), Cicada (a chorus of insects, two per ear, each singing in bursts of pulses and resting between, the band from Position and its Q the insect's resonator). Levels are matched so a colour change does not change the loudness.

THE NEAR SOURCES (13.09.2026) are what the instrument plays close to the ear -- built for the Near Events, but any slot may hold them:

FLUTE  A blown pipe, after the jet-drive waveguide of Cook: a loop of one period with a loss filter at the far end (Bright), and at the embouchure an air jet that is deflected by the wave in the pipe, travels to the edge, and is switched in or out by a soft cubic. The jet's travel time sets the register -- half a period speaks the fundamental, a quarter the octave -- and Position is the embouchure between them, so the same pipe overblows when it shortens. Force is the breath pressure; Speed the noise the air carries; Pos Drift a player's vibrato. Measured in tune across the register like the Bow.

MURMUR  A voice that never says anything: a glottal pulse at the pitch, jittered as a voice is, through three formants that walk between the vowels at a syllable's pace (Speed), some syllables fricatives, some begun with a stop, in phrases of a few seconds with pauses between. The pitch falls over a phrase and rises on the stressed syllables (Pos Drift is the range). Force is the effort. Position is the medium: at 0 a voice in the room, close; towards 1 a radio -- a band from 300 to 3000 Hz, a saturation, the hiss of the carrier, the squelch that closes after every phrase, and from 0.7 up the Quindar tones Apollo keyed its air with.

BOWL  A singing bowl under a stick: six modes in the ladder every thin-walled bowl has (1 : 2.7 : 5 : 7.8 ...), rubbed by the Bow's own friction curve, so the tone builds over seconds and sustains for as long as the stick moves. Every mode is two resonators a hair apart -- the doublet an asymmetric bowl always has, and the beating that comes with it (Pos Drift is how far apart). Force is the pressing, Speed the rubbing (below a twentieth the stick is lifted and the bowl rings out), Position where the stick sits, rim to belly; Bright how long the upper modes last.

ICE  The same friction on a low, dense, short-lived set of modes, with a slip clock in front of it: under a slow load the stick does not glide, it creeps -- holds, gives, holds -- and every give is a pulse into the modes. Ice, or old wood, stretching rather than struck. Speed is how fast it creeps.

CLIP  The recording as it is: from Position, once, at its own speed (Pitch = Free, the octave and ratio as a speed) or pitched to the note, the last twenty milliseconds faded, silence after -- or wrapping, for a file whose name says _loop. Every other way of playing a clip here takes it apart; this one keeps the sentence. Made for the near layer's archive (Library/Archive: NASA's radio loops, launches, Mars), where a near event plays "Houston, we've had a problem" once and lets the horizon come back.

DROPS  Water falling into a vessel, after van den Doel: a drop is a bubble, a sine whose pitch rises as it decays with the damping the physics gives it, and the click of the impact rings the vessel (two resonators, their pitch from Position -- a cup at the top, a cistern at the bottom). Density is drops a second on a clock that never repeats; Bright is the bubbles' size; Pitch = Note puts them on the note's partials.

THE SIGNALS (13.09.2026) are the second nine, built for a foreground that is not a note at all -- a thing heard rather than played. Each is a physical model or a piece of radio, and each reads the near source's controls its own way:

WHISTLER  A lightning stroke's pulse travelling along a field line through the magnetosphere's plasma arrives dispersed: the high frequencies first, the low ones later, and a VLF receiver hears a whistle falling. Eckersley's law gives the delay as D / sqrt(f), so the frequency falls as one over the square of time -- f(t) = fEnd + (fStart - fEnd) / (1 + t/tau)^2. The note is where the whistle ENDS; Bright is how far above it begins (two to six octaves), Speed the tau (0.2 to 1.7 s, the slow ones the long field lines). Under the tone a thread of noise in a narrow band follows it down at -32 dB: the carrier's own fluctuation.

SHAKER  Cook's PhISEM (Physically Informed Stochastic Event Modeling, 1997), which models a shaker not as a sound but as a system energy: a shake tops the energy up, it decays, and while it lasts the beans hit the shell at random, every hit a grain of noise at the energy's level, the shell a resonance over them. Density is the shakes a second while the note is held (the first at the note), Force how long the energy lasts -- a bean pod at 0, a big gourd at 1 -- Position the shell's pitch (1.5 to 5 kHz, or the note itself with Pitch = Note), Noise Q how much the shell rings.

CHIME  Struck bronze: a ting-sha, a ship's bell, a church bell far off. Five modes, and what makes cast bronze beat is that its modes come in doublets a hair apart, an asymmetry of the casting -- the prime's twin sits at 1 + split, and the two together beat the way a pair of cymbals does (3.5 Hz at 2.4 kHz). Under the prime the hum an octave down (Tilt is how much of it: a church bell has one, a cymbal none), the tierce a minor third over, a high partial at 2.5 to 3.5 times (Bright). Force is how long it rings, two to eighteen seconds for the prime and a fifth of that for the high one; Pos Drift is the split. One hit of 0.8 ms sets every mode going at once.

GEIGER  A Geiger-Mueller tube: discharges on a Poisson clock at Density a second, and now and then a cluster -- the rate leaping to 45 a second for 120 ms and falling back over 60, which is the burst a particle shower makes. Force is the chance of a cluster (per second, up to two thirds). Every discharge is a Dirac step into the counter's piezo, a heavily damped resonance (Position: 800 to 4000 Hz, the classic 1850 near the middle; Noise Q: 0.7 to 2.2) that makes the click last about two milliseconds and no longer.

TUBE  A fluorescent tube in a bunker corridor, in three stages. The bimetal starter's two or three clicks, 150 to 350 ms apart, each a Dirac through a low-pass at 380 Hz -- the thunk of the switch. Then the choke's hum: the mains rectified to twice its frequency, 100 Hz on 50 and 120 on 60 (Position chooses), harmonics falling as k^-1.6, coloured by the coil's resonance at 850 Hz. And the plasma's hiss, noise between 3.5 and 6.5 kHz chopped by the same half-waves (Bright is how much of it). Hum and hiss come up over 400 ms after the last click, as the tube strikes.

KRELL  The Krell's machines (Forbidden Planet, 1956; Louis and Bebe Barron's circuits): FM whose carrier and index are steered by a Roessler attractor -- the one strange attractor with a single fold, so the pitch wanders through its band, never repeats and never quite loses the thread. dx = -y - z, dy = x + ay, dz = b + z(x - c), with a = b = 0.2 and c = 5.7. Its x steers the carrier over 2.6 octaves about the note (Position past the middle snaps that to semitones), its y the index (FM Index is the ceiling), Speed the attractor's pace, and FM Ratio is the modulator's.

BEACON  A deep-space beacon's packet: a preamble chirp falling from 2.17 to 1.5 times the note over 35 ms, then eight bits of frequency-shift keying at 25 ms each -- space at the note, mark a major third over it (1200 and 1500 Hz on a note of 1200) -- every bit windowed with 5 ms Tukey edges so the keying does not click. The bits are drawn anew for every packet; a packet every 1/Density seconds while the note lasts, the first at once.

MORSE  A number station: five-figure groups (Position past the middle: letters instead) in Morse at Speed words a minute, 8 to 30 -- a dit is 1.2/wpm seconds, a dah three of them, the gaps one, three and seven. The tone is at the note, keyed with 5 ms edges, and over it the ionosphere's flutter: a slow random tremolo, Bright its depth, as the signal comes and goes over the horizon.

DIAL  A shortwave set with its dial turned: heterodyne whistles -- a carrier beating against the local oscillator -- sliding as the tuning moves. One to three of them (Position), each drifting to a new pitch every second or two between 300 Hz and 3 kHz with the note as the centre, and under them the band's own noise (Bright), through the same 350-3200 Hz window the Murmur's radio has, with a squelch burst now and then as a carrier drops.

EACH SLOT ENTERS ON ITS OWN CLOCK. The Envelope section is one envelope for the whole voice, so until these existed all four sources started on the same note at the same instant, and a preset of a wavetable against a texture was one chord struck twice at once however different the two materials were. DELAY holds a slot silent for up to thirty seconds after the note; RISE fades it in afterwards on a smooth curve; ENV gives it a level contour instead -- Own, a shape of its own drawn on the SOURCES page of the ENV tab with its own Mode, Time, Depth and Sync, read from 0 (silent) to 1 (the slot at its written Level); or one of the six modulation envelopes, borrowed, read bipolar (-1 silence, +1 full). The shape runs from the NOTE rather than from the phrase clock, so every note gets it and not only the first after a silence. All three default to off, so a preset written before them sounds exactly as it did. Give a texture twenty seconds and it is a second instrument arriving under a note that is already sounding.

ROLE is the other half of that: which of the voice's notes a slot sounds in at all. All is every note, as it always did; Lowest, Inner and Highest give the slot the note's PLACE in what its owner is sounding right now -- the register is the role in this music, and the cello under the chord need not also be the chime on top of it. The place is re-read as the cluster changes and the slot fades over a second and a half, so a note that stops being the top hands its chime on instead of dropping it.

Every slot also has Octave, a just Ratio to the note (3/2 a fifth up, 7/4 a harmonic seventh -- ratios, so the scale stays pure) and Pan. In Source 1 these move the strand bank as well.

THE VECTOR is the four slots read as a place rather than as four levels, after the Prophet VS and the Korg Wavestation: a point in a square whose corners are the four sources. Amount is how much of it there is, and at 0 nothing here does anything -- each slot plays at the level it is set to. Drag the point, or turn X and Y. The centre of the square is neutral by construction: at (0.5, 0.5) every factor is exactly 1, so turning Amount up on a patch you like changes nothing until you move. Wander lets the point drift on its own, on two curves whose rates share no simple ratio, so it never traces the same path twice; Rate is how fast. The bars beside the square say what the point is doing to each slot.)" },

    { "Filters and Z-plane",
R"(Two filters, each with its own switch, in series or in parallel.

THE VOICE FILTER (Filter section) has ten models behind the same knobs:
  LP 6      one pole, warm and gentle
  LP 12     the state-variable low pass -- the default, the one the instrument always had
  LP 24     two stages, steep
  HP 12     the opposite slope
  BP 12     a band, unity at the cutoff
  Notch     a hole swept through the harmonics
  Peak      a bell of up to +14 dB, narrower with Resonance
  Ladder    four one-poles with saturating feedback, self-oscillating near full Resonance
  Comb      a feedback comb tuned to Cutoff; Resonance deepens the dips -- on a cluster, a second resonating body
  Formant  three tracked bands: the filter sings a vowel
FOLD is a wavefolder after both filters. Drive flattens what will not fit through the filter; a folder turns it back on itself instead, and a wave mirrored at the fold grows a family of high partials that no saturation makes -- the metallic edge of an industrial record. Its positive half folds a third sooner than the negative one, so the even harmonics are there too. Off at 0.

Cutoff is moved by Key Track (1 keeps the same partials in the passband on every key), Env Amount (the amplitude envelope, negative closes), Drift (a slow wander) and by the voice's distance (2.5 octaves darker on the far plane). Drive saturates ahead of the filter. On switches it out.

THE Z-PLANE FILTER (after the E-mu Morpheus idea): four filter frames sit on the corners of a square and a point (X, Y) inside it is a filter interpolated from all four -- on the pole and zero parameters, so every point is stable. 155 Shapes in twelve families (vowel morphs, bell clusters, resonator banks, sweeps, and the acoustic ratio families generated for the bank); the point wanders at Rate by Depth; the third axis Z is the cube's own depth, so a shape is a volume rather than a square; Resonance narrows every section; Key Track moves the frame with the note. Mode: Off, Series or Replace (the z-plane alone). Route: with both filters on, Series puts the z-plane after the voice filter (Mix is its dry/wet), Parallel feeds both the dry sum and Mix balances them.

The FILTER RESPONSE display draws the voice filter (in the voice colour), the z-plane (in the accent) and what a note actually meets after both, from the same maths the audio path uses.

The three reverb returns each have a LOW CUT beside their high cut now. Together they are the filter funnel a mixing engineer puts on a return: dense tails pile up between 200 and 450 Hz, which is exactly where a background stops sitting behind the music and starts covering it, and taking that out is what lets a 40-second tail be enormous and transparent at the same time.

The OUTPUT SPECTRUM along the bottom of the left column is the other half of that picture: what is actually coming out, with the same filter curve laid over it on the same decibel scale. Its window is 16384 samples -- 2.9 Hz at 48 kHz -- which is long enough to show the partials of a low drone as separate lines rather than one hump, so a fifth sitting exactly on the third partial is something you can see, and see come apart as Purity Drift loosens it. The bars are the moment; the faint line above them is the loudest each band has been in the last few seconds; the ticks along the bottom edge are the fundamentals of the notes sounding now. Point at it to read a frequency, its nearest note and that band's level.)" },

    { "Space, Air, Foundation",
R"(SPACE is the spatial model. Depth scales how deep the brain places its notes (40 % close, 60 % deep); Keys Depth is the plane your keys play on. Pan Drift wanders each voice's centre; Time Width is the interaural time difference (up to 0.65 ms, the far ear hears later) -- width from time, not level. Presence is a bell at 2-5 kHz on the near plane only, gone on the far plane. Breath lets every voice's distance itself wander (the room breathes) at Breath Rate. Arc is one very slow drift over Arc Period minutes that leans on density, brightness and depth, so a whole night has a shape; Arc Sync ties it to bars.

The STAGE display shows every sounding voice as a dot: left-right by pan, near-far by plane, size by envelope, brain notes in the voice colour, your keys in orange.

AIR is filtered noise inside each voice, at a multiple (Color) of the note's fundamental, with Q; in Ghost mode the noise runs through six sharp resonators on the harmonics 1 2 3 5 7 9, so the harmony is filtered out of the chaos.

FOUNDATION is a mono sub voice one or two octaves under the brain's root (or, with Source = Difference, on the combination tone of the two lowest voices -- the ghost bass of a just chord), gliding in the log domain over Glide seconds, with Binaural offsetting left and right by a few hertz and Tone adding a little harmonic content. It is injected after the mid/side stage so Bass Mono cannot thin it. Pad Low Cut takes the pads out of its register (12 dB/oct below the cut).)" },

    { "Effects: foreground and background",
R"(The near bus (the dry plane) runs through ENSEMBLE (Mix, Depth, Rate or Sync, and a Mode: Chorus is three modulated taps, Microshift detunes the two channels a few cents in opposite directions with nothing moving -- the studio's way of widening a drone that survives a mono sum, where a deep chorus at 13 to 22 ms is a comb filter waiting to be summed), DELAY and DELAY 2 in series (independent left and right times or note values, Feedback, Cross for ping-pong, Damping in the loop, Mix onto the near bus and To Far into the background: echoes that recede), and the NEAR REVERB (a small room: Mix, Decay, Damping, Low Cut).

The HAAS band (in Space) widens the foreground where the ear takes its direction from level rather than from time. Delaying a whole channel by ten to thirty milliseconds widens it and destroys it in mono; done to the band between about 1.2 and 4 kHz, and put into the side channel so that what is added on one side comes off the other, the edges open, the bass and the top stay where they were, and a mono sum is exactly the picture it was. Haas Time is how far that band is delayed. Off at 0.

The far bus is the background: FAR REVERB is an eight-line feedback network, 100 % wet, dark and wide -- Size, Decay (tens of seconds), Damping, Pre-Delay, Asymmetry (the right half stretched and delayed so the two ears hear different reflections), Tail Cut, Low Cut, Freeze for an instant infinite pad, and WIDTH, which is the background's own stereo width before it is added to the foreground. MODE gives the network four characters. Classic is the network as it always was. Scattering puts a Schroeder all-pass inside every delay line's loop (Schlecht and Habets 2020), so each pass round the network scatters every echo into many and the echo density grows far faster -- measured, 0.60 to 0.76 of Gaussian after 50 ms -- while the late tail stays as smooth. Colourless is that with the eight line lengths searched offline for the flattest response rather than hand-picked primes (after Dal Santo, Prawda, Schlecht and Valimaki): a tail is a sum of modes at the lines' own frequencies, and where those pile up it rings -- 0.31 dB of third-octave spread against the classic set's 0.47. Rotating is Colourless with the feedback turning: the line lengths stop wobbling and eight rotations on pairs of lines advance at hundredths of a hertz in front of the reflection the network always had. Rotations and reflections are lossless at any angle, so the loop stays exactly energy-preserving while its modes are re-mixed rather than re-tuned -- no pitch modulation anywhere in the tail, and a band pattern that drifts instead of ringing in one place. Four more shape the background as a place rather than as a device: DIFFUSE puts modulated all-passes in front of it, so the tail arrives instead of starting; ROTATE turns the whole field on a minute-scale curve; ENVELOP lifts the far bus's side channel between Bass Mono's corner and 500 Hz, which is where Bradley and Soulodre found the sense of being INSIDE a room rather than in front of it, and exactly where the funnel and Bass Mono had taken it away; and CO-MOD breathes the whole background to one slow random envelope so every band of it rises and falls together -- Hall, Haggard and Fernandes (1984) measured a tone in such a noise as audible ten to fifteen decibels further down, because the ear groups what moves together into one object and listens past it. Nothing in the foreground follows that envelope, which is what makes the foreground the thing heard past it. That last one is the funnel: a mix in which everything is spread as far as it will go is a flat wall, and pulling the far plane in towards the centre while the foreground stays wide is what the ear reads as distance. 1 is the reverb as it made itself. CLOUD takes grains of the recent foreground (Send, Density or Sync, Size, Spray back in time), transposes them -- Pitch for leaps of octaves and fifths, Transpose for all of them, Scatter over the intervals of the scale that is playing -- and drops them into the far reverb. Feedback lets it hear itself, grains of grains, through Tone, and FB Shift transposes that loop on every pass, in the spectrum; Resonance rings resonators on the scale's notes (Res Mode, Res Notes, Ring), each grain exciting one of them; Swarm makes the grains arrive in flocks -- the onsets as a self-exciting Hawkes process, every grain raising the chance of the next for a quarter of a second, at the same mean rate. Those five together are what Absynth's Aetherizer is known for -- grains that hear their own echoes, tuned resonance, grains in groups -- built from the parts the literature does better where it has them: the loop saturates with antiderivative antialiasing so a stacked transposition does not fold back on every pass, Scatter follows the instrument's own tuning (just intonation and Scala files included) where the Aetherizer rasters to thirteen fixed twelve-tone menus, and a resonator rings ON after its grain has ended, where the Aetherizer's filter lives and dies with its grain. TO NEAR shares the same grains between the two planes at equal power: the cloud has always returned to the far bus alone, and if you have ever turned the Send up and wondered where it went, it went behind you. ROOM is a convolution reverb from a loaded impulse response (Impulse... button, or the preset's own) or the built-in dark hall, with Pre-Delay, Tail Cut and Low Cut. SOURCE chooses what it reverberates: the far sends, before the far reverb, or the finished near bus. It takes TWO impulses, A and B, and MORPH crossfades between them -- one room becoming another over as long as you like, blended inside the one convolution, so a morph costs what a single room costs. A pack preset brings its B with it; one that names its room but no B plays A alone, whatever B was loaded before. The library ships 1100 impulses, 421 of them a/b pairs generated for exactly that morph, beside the measured rooms. The convolution itself is partitioned in three sizes with the work of the long partitions spread across the blocks between their deadlines, which is how sixty seconds of hall costs about 1.4 per cent of a core instead of arriving as a spike every few blocks.

FEEDBACK returns the finished mix: To Bus into the near bus before the filters and effects (throttled by the output level so it hisses and holds instead of running away), To Pitch as phase modulation of every partial (the sound bends itself), through Tone and Drive; Tape adds asymmetric saturation, wow and flutter and a level-dependent noise floor.

MASTER: Bass Mono removes the side channel below a frequency (a mono low end under a wide picture), Side Air lifts the side at 3 kHz, Width scales the stereo image, Subsonic is a steep high-pass on the finished output; then the master gain and a soft clipper. There is no compressor.

The LOUDNESS METER under the master reads the finished output to BS.1770: I is the gated integrated value, S the short term, LRA the range, TP the true peak between samples, and crest the peak-to-RMS distance. The band on the bar is -24 to -16 LUFS, where a dark ambient master is asked to land, and the line at -14 is where the streaming services normalise: a master louder than that is turned down again and arrives flat rather than loud. Click the meter to start it again.)" },

    { "Cosmos",
R"(Cosmos is a parallel path off the near bus -- Send in, Return to the near plane and To Far into the background -- that adds to the sound and never replaces it. In order:

FREQUENCY SHIFTER  Every partial moves by the same number of hertz (Shift), so a harmonic series becomes inharmonic; the right channel shifts 3 % less, which spreads the picture. Shift Drift wanders it.
RESONATOR  A comb tuned to the brain's root (Res Pitch as a multiple of it), ringing with Res Feedback, at level Resonator.
VOWEL  An a-e-i-o-u formant filter at Vowel, wandering at Vowel Rate.
NEBULA  A spectral smear: the spectrum's phases are scattered by Smear (1 is a spectral freeze); Nebula is its mix.

SHIMMER sits around the far reverb rather than in the Cosmos path: the reverb's previous block, pitch-shifted (Shimmer Pitch: an octave, a fifth, a fourth, an octave and a fifth, an octave down, two up), is fed back into its input -- the rising cloud. It is regulated by the reverb's level so it cannot run into the clipper. Shimmer Mode chooses the shifter: Spectral moves every partial in the spectrum, phase-locked (after Laroche and Dolson), and stays clean however often the loop comes round; Grain is the two-head shifter it always had, with its flutter.

MEMORY, the third tab, is a second parallel world: a long, drifting sound memory in the spirit of SOMA's Cosmos. Send takes the foreground into a pool of ninety seconds divided between Lines (two, four or eight) of prime lengths, the longest Size seconds; Return and To Far bring it back. Blur exchanges the lines' content through a lossless rotation -- at 0 every line is its own echo, towards 1 every echo runs through all of them, and beyond a half the echoes scatter into a space -- without ever changing the level. Drift slides the lines against each other and wanders them across the field, bent by a slow chaotic signal. Hold is how long a memory lasts (for ever at 1), Age how much darker it grows on the way, Renew lets new sound push the old out, Drive saturates the loop without aliasing. Recall plays grains out of the whole memory, and Seek makes them prefer the stretches whose notes fit the scale and the last seconds of input. Freeze holds, Erase empties, and Reverse and Half Speed move the tape itself: what was recorded plays backwards or an octave down, what is recorded now comes back as it went in.

The COSMOS RETURN display shows the spectrum of what the path hands back (nothing while Send is 0).)" },

    { "Conductor: brain, tuning, coherence, clock",
R"(CLUSTER BRAIN is the conductor. While Active it chooses notes from the scale between Lowest and Highest, prefers consonant intervals to what is sounding (Consonance), places each on a plane, keeps Density notes sounding, decides every Event Rate seconds (or every so many bars with Sync), holds each note between Hold Min and Hold Max, and lets its root Wander over time. Seed (in Tuning) makes a night reproducible. The lowest held MIDI key becomes the brain's root. The NOTES display is a roll of what it has played.

TUNING: Scale (just scales, 12-TET, Bohlen-Pierce, or a loaded Scala file), Key Map (snap the twelve keys to the nearest degrees, or walk the degrees consecutively), Root, A4 reference, Hold (keys latch). Purity blends every note between 12-TET and the scale in the log domain, and Purity Drift lets that blend wander at Purity Rate, so the beating locks in and loosens over minutes. Freeze holds every voice's spectrum and pitch still. Portamento glides a new key from the last one, and Gravity slows the glide near consonant ratios so it clicks into the harmonic nodes on the way.

COHERENCE: four slow Kuramoto oscillators coupled by Coherence (free at 0, in step near 1), moving brightness, depth, pan and the z-plane point by Depth at Rate. Their sines are also the KURA 1-4 modulation sources.

CLOCK: where the tempo comes from -- Internal (Tempo, Run), Host (the DAW's play head) or MIDI (MIDI clock at the input). See the topic "Clock and sync".)" },

    { "Modulation: LFOs, envelopes, matrix",
R"(THE SOURCES a route can be driven by: the eight LFOs, the six envelopes, the voice's own amplitude, the eight macros, the four Kuramoto oscillators of the Coherence ring, the note, its velocity and its distance, one random number per note, the Beat (the instrument listening to how far out of tune it currently is), and the hands: PRESSURE (channel or polyphonic aftertouch), WHEEL (CC 1) and SLIDE (CC 74). Those last three rest at zero, so give them the 0..1 flag and a patch at rest sounds exactly as it did until you move them. Aftertouch on the filter's resonance, the wavetable position and the reverb at once is one gesture with three routes.

Three more come from the piece's own shape rather than from a clock or a field. ROOT AGE is how long the root has stood, measured against Home Time: 0 the moment it moves and climbing towards 1 the longer it holds, so a sound can open after a change instead of having been open all along. LAYER is the role of the voice being read -- foundation 0, body 0.25, colour 0.5, air 0.75, and the second conductor's shadow 1 -- which gives every register its own brightness, its own air, its own plane without the conductor needing to know anything about it. SECTION is the hour's Arc as a staircase of five steps instead of a glide, for the things that should change rather than slide: a register, a supply, a room.

The strip along the bottom holds every modulation source as a card: LFO 1-8, ENV 1-6, MACRO A-H, KURA 1-4, AMP (the voice's own envelope), NOTE, VELO, DIST (the voice's plane), RAND (a random value per note) and BEAT. Its tabs edit the sources:

LFO  Eight free LFOs with Shape (Sine, Triangle, Ramp Up/Down, soft Square, Random, Steps, or Table -- a frame of the user wavetable as a shape, so any drawn curve is an LFO), Rate from one cycle in twenty minutes to 20 Hz or a note value (Sync), Phase, Depth, and Mode: Global (one phase for the instrument, every voice breathes together), Voice (each voice its own copy), Retrigger (each voice restarts from Phase). The editors show the shape with a running dot.

ENVELOPES  Six multi-segment envelopes: up to sixteen breakpoints, a curve on every segment, an optional sustain point and an optional loop. Edit the curve on the curve: drag a breakpoint to move it in time and level, double-click the line to add a point or a point to remove it, right-click for the sustain point, the loop, the curvature of a segment, and ten shapes to start from -- ADSR, AD, AR, ramps, a pulse, a slow swell, two peaks, stepped, bipolar. The sustain point wears a ring, the loop points a vertical line. Mode One Shot / Loop / Sustain Loop, Time stretches the shape or Sync spans it over one note value, Depth scales it. (The text form "t:v:c/t:v:c/...!s2!l1-3" is still what a preset stores.)

The voice's own AMP ENVELOPE is a plain ADSR -- Attack, Decay, Sustain, Release -- on the AMP ENV tab, with times in seconds up to a minute for the attack and two for the release. These six are for everything else.

MATRIX  Up to 32 routes "source > target : depth [: via] [: u]". Depth is a fraction of the target's range (-1..1); via scales the depth by a second source (a macro, typically); u treats a bipolar source as 0..1. One source may drive as many targets as it likes.

BEAT  The instrument listening to its own tuning. It takes the two lowest sounding voices, finds the simplest just ratio near the interval they make, and turns at the beat between the harmonics that would coincide if that interval were exact -- which is zero when the chord is in tune and quicker the further it has drifted. An exact octave leaves it standing still; a fifth in equal temperament, two cents narrow, turns it about half a hertz. Route it at a filter, at the Nebula's smear, at anything, and the sound breathes in time with its own harmonic friction rather than at a rate somebody typed into an LFO. Purity Drift is what sets it moving.

ROUTING WITHOUT TYPING  Drag a card from the strip onto any knob: a route at a quarter of the range is added. Right-click a card to see and remove its routes; right-click a knob to see what drives it. A modulated knob wears a thin ring in its source's colour (LFOs turquoise, envelopes green, macros pink, coherence blue) and a second arc from its value to where the modulation is pushing it right now. Performance state (morph, macros, map, route, clock) can never be a target.)" },

    { "Morph, macros, perform, gestures",
R"(MORPH holds two full snapshots, A and B: pick a preset for each or capture the current state with A <- now / B <- now. Switch Active on and Position blends the whole instrument between the two worlds; Glide sets how long it takes to follow a new position -- up to fifteen minutes, so one gesture can carry a piece across a quarter of an hour. Continuous parameters interpolate in their own perceptual curve, integers round, choices flip halfway. Morph settings are never part of a preset. While Morph is on, the knobs reach the sound only through an end that has not been chosen: a snapshot is a snapshot, and turning a knob does not change it. So a slot nobody has chosen does not hold anything -- it reads "as played" and plays as the knobs stand, and switching Morph on before choosing A or B changes nothing you hear. Choose only B and Position runs from what is playing now to B. The snapshots stay with the instrument across a preset change and a journey step, and come back with a saved state.

JOURNEYS (in the Morph section) are presets in a row for an evening that plays itself: each preset is held for a while drawn from a range, then crossfaded into the next over a fade drawn from another, and round again when the journey is cyclic. Pick one in the Journey box and it starts; the label beside it says where it is and how long the step has left; Stop leaves you on the preset you are on. Starting a journey switches Morph and the map blend off, because both hold the instrument at a blend of their own and would overrule every preset the journey brings. The volume knob reaches both presets while one fades into the next, so it works through a step as it does between them. 119 come with the library -- for every pack a Journey of twelve presets spread across the pack's space and a Night of its twelve stillest, and for every family of artists a Crossing from pack to pack -- and your own are read from Documents\Noctuary\Journeys. To write one while playing: + now adds the preset that is playing as a step (with the default five to ten minutes and a fade of half a minute to a minute and a half), Save... names the journey and puts it in the box. A journey is a plain text file, one preset a line: <preset> | <dwell> | <fade> [| near=<near preset, auto or keep>], times as m:ss, h:mm:ss or seconds, a range as two times with a dash, and "cyclic on" or "cyclic off" on a line of its own. The near field says what the foreground does at that step: a name sets that near preset, auto lets the sound preset bring its own (see Auto in Near Events), keep leaves whatever is playing, and no field at all does what Auto would.

MACROS A-H (Space, Alien, Motion, Bloom, Density, Distance, Evolution, Air) are one knob for several parameters each; the mapping table (Gestures...) says which, with range and smoothing. They are also modulation sources. Inertia is the analogue slew: every knob glides to its value with this time constant.

PERFORM (header button) shows only the eight macros and the morph, large, for playing a set; Record set / Play set log and replay everything you do, with time.

GESTURES: the same layer that will drive the Quest version listens to OSC (/ambient/hand/L|R, /head, /param, /gesture, /note, /preset, /morph, port 9000): hand height, distance, pinch. Calibrate learns your range (hands together and apart, low and high, near and far, for six seconds). The right pinch is the clutch: mappings act only while it is engaged.)" },

    { "Presets, packs, browser, map, routes, sets",
R"(Presets come in two independent layers: the Sound box (voices, space, effects, brain, tuning -- 256 built in) and the Cosmos box (32 presets for the Cosmos section only), with the z-plane and the Strike layers beside them. Loading one never touches the other; in a DAW the full presets are the programs. Save... / Load... store the whole state as an .noctuary file.

THE BUILT-IN PRESETS

The 256 compiled-in presets are the instrument playing itself with nothing installed: not one of them names a sample, a wavetable or an impulse response, so they sound the same on a machine that never saw the library. They were generated from the same ranges as the packs (Tools/library/artists.py), balanced against their own noise bed, rendered and gain-matched, and they are grouped into sixteen families of sixteen, by what part of the instrument they are about:

Just Drones: the additive bank on pure ratios, the instrument's oldest sound. Glass and Bells: inharmonic and bright, struck and left to ring. Choirs and Vowels: the vocal tables and the formant filter. Deep and Sub: the low register, almost no treble, the Foundation carrying the weight. Bowed Strings: the waveguide string under a real bow. Organs and Reeds: the organ and reed tables, chapel registers. Played Keys: the conductor off, made to be played from a keyboard. Generative Chords: autoplay in Chords, one voice exchanged at a time. Cosmos: the frequency shifter, the resonator and the nebula as the subject. Clouds and Memory: the granular cloud and the long Memory beside it. Weather and Noise: the Air section as an instrument, noise shaped into wind. Metal and Feedback: the feedback bus driven, metal tables, the fold. Space and Motion: rotation, elevation, the Vector and the binaural field. Slow Worlds: the hour-scale Arc, the tide, Lenia and the attractors. Microtonal: Bohlen-Pierce, Slendro, otonality, the stretched octave. Strike and Modal: the Strike as an exciter into the z-plane read as a resonator bank.

PACKS

Plain text files (*.ambientpack, one preset per line) that may name a sample, a wavetable, an impulse response, a modulation matrix and envelope shapes of their own. Put them in Documents/Noctuary/Packs or point AMBIENT_PACKS at a folder (the installer's own folders are read too, and a pack found in two of them loads once); they appear everywhere the built-in presets do, each pack as a family. The library that ships alongside has 14336 presets in 56 packs, 8686 samples (5748 textures and 2938 field recordings), 2191 wavetables on three shelves and 1100 impulse responses.

Every pack is one corner of the drone repertoire, written in the spirit of an artist who works there -- nothing is sampled from or affiliated with any of them; the packs are ranges over this synth's own parameters, chosen by ear, then rendered, measured and gain-matched. Two hundred and fifty-six presets each, spread over eight shades from deep to lit, still to astir, sparse to massed. Every clip in the library was generated for a named artist, and a pack draws the clips written for its own and for the neighbours it borrows from, so the material and the settings agree; a preset takes its key from the clip its voice is playing. A pack is a family in the browser, and its name is the first thing to search for.

SLEEP CONCERT (in the spirit of Robert Rich). The all-night concert: just intonation, a binaural sub a few hertz apart between the ears, holds measured in minutes, the brain placing most notes deep. Attacks of ten to thirty seconds, the Bloom opening the spectrum over a minute or two, the Arc leaning on the whole night. Flutes, steel guitar, singing bowls and rainforest, in cathedrals and blooming rooms.

DEEP EARTH (Lustmord). Subterranean: the low register, almost no treble, the far reverb long and dark, the Foundation carrying most of the weight. Gongs, chant, caves and seismic recordings read as continua by the Stretch type, in caverns and vast spaces. Presets that are felt in the floor before they are heard.

DESERT EMBER (Steve Roach). Warm and organic: the ladder filter and its drive, slow pulses from the delays, the second delay in series, flutes and analogue pads over desert and fire recordings. The analogue end of the library, in halls and drifting rooms.

RITUAL MACHINE (Deutsch Nepal). Saturated feedback loops: the feedback bus and its tape, the drive, the patina, long delays with high feedback that absorb into fog. Metal creak, factories and bowed metal, the Strike on metal, the z-plane on steel plates and tam-tams. Grime as a material.

ARCTIC LOOP (Biosphere). Cold and repeating: the Memory holding what has passed, the delays in sync, digital bells and ice, sea and polar stations. Brightness middling, the far plane wide, everything absorbing rather than ringing.

FAR RELAY (Martin Stuertzer). Berlin-school space: sequences that pulse, the Cosmos wide, the wavetables classic and analogue, radio-space recordings underneath. Vast reverbs, slow filter movement, nothing acoustic in it.

PLANETARY (Michael Stearns). The harmonic series as the subject: stacks on harmonics and subharmonics, overtone voices and long strings, wide spreads, shimmer, the far reverb enormous and bright. The widest presets in the library.

GENTLE SYSTEMS (Brian Eno). Quiet systems: few voices, mild consonance, mallets and reed organ and piano, two delays out of phase with each other, the Memory recalling a phrase the way a tape loop would. The room is a hall and the pieces are patient.

VAST CHORD (Mathias Grassow). Dense just-intoned chord walls: six-strand stacks on pure ratios, high brain density, purity near one, the sub on the difference tone, overtone voices and tanpura-like drones. A single key is a chord; the brain adds four more.

RITUAL STONE (Raison d'Etre). Ritual in a stone room: chant, choir and bells through the convolution Room on cathedrals and caverns, the Room Morph moving between two of them, the Strike on metal, the z-plane as church bells and organ pipes.

FROZEN GONG (Thomas Koener). Filtered noise fields, nearly motionless: the Spectral type mostly with its read head stopped, gongs and ice, brightness under a third, the far plane cold and enormous. What changes, changes over minutes.

HARBOUR RAIN (Loscil). Dub-tinged and tidal: electric pianos and mallets in the Texture type, delays in sync that absorb, the Memory, rain on roofs and harbours underneath. Warm, repeating, always a little wet.

HULL RUMBLE (Sleep Research Facility). Machine hum, static and depth: brown and grey noise, the Foundation, engine hulls and ventilation stretched to a standstill, the z-plane as concrete pipe and tunnel. The engine room of a ship at night.

FOREST RITE (Ulf Soederberg). Nordic ritual: bowed and struck wood, caves and night country, the Room on caverns, the Strike on wood, the sub heavy. Slow ceremonies in the open air.

WEATHER STATION (Hazard). Wind and electricity: the Cloud with its feedback and swarm, wind and electrical hum, pure tones under noise, the far plane distant. Field recording as a drone instrument.

PAINTED FIELD (Andrew Chalk). Blurred warm washes: the Blur high, the Memory long, tape loops and reed organ and piano, the Ensemble as a microshift. Presets like a colour rather than a note.

MILLSTONE (Jonathan Coleclough). Acoustic and mechanical: bells, glass and blown vessels through the Cloud and its resonators, the Strike on metal and wood, the Body's modes, the Room on small designed spaces. The sound of things turning.

GLASS VITRINE (Mirror). Ghostly harmonium: the Glass, Organ and Reed tables, inharmonicity, spectral z-plane shapes, the nebula's smear. Thin, high and see-through.

SLOW CAROUSEL (Mimir). Warped loops under tape hiss: the Patina high, the feedback's tape, wow on everything, tape keyboards and fairground pianos, the Memory reversing now and then. Old and slightly wrong on purpose.

CHAMBER GREY (In Camera). Small dim rooms: the near reverb and the Early Room doing the work, the far plane quiet, short delays, close and dark. The intimate end of the spatial model.

LOOP STUDIO (Colin Potter). Long tape delays and processed loops: two delays in series at seconds with feedback near the top, absorption, the tape in the loop, the Memory at its longest. Every note keeps arriving for minutes.

STILL MEADOW (Darren Tate). English open air: one or two voices, purity drifting, grain textures over wind and night country, the far reverb modest. The quietest pack that still has a pulse in it.

WATER HYMN (ora). Organ and water: pipe organ and bowed metal over rivers and rain, the Spectral type rebuilding them, drifting rooms, everything sustained. A hymn played by a place.

VEGETAL DRONE (Monos). Slow growth: coherence between the voices, sympathetic resonance, the purity drifting in and out of just, organ and long strings. A drone that breathes rather than holds.

SUSTAIN (Paul Bradley). One long tone, minimal change: one voice, one key, purity near one, drift near zero, the far reverb enormous, the background frozen. The stillest presets in the library, and a test of every reverb.

STREET RESONANCE (BJ Nilsen). The city as a resonator: contact recordings, singing wires and traffic, the Vector moving between four of them, the Early Room close, the sub low. Listening rather than playing.

BOWL TEMPLE (Klaus Wiese). Singing bowls and tanpura: the Cloud with its resonators tuned to the scale, overtone tables, the Strike on metal, cathedrals and halls. Ceremonial and warm.

TEMPLE OF AIR (Ooephoi). Pure and extremely slow: sine-like banks with few partials, minute-long attacks and releases, a high consonance, rainforest and insects far behind. Nothing here happens quickly, and nothing has an edge.

STAR RITE (Inade). Ritual metal: the Cosmos heavy in every preset -- the resonator on the root, the shifter drifting, the nebula smearing -- with the modal z-plane as struck metal underneath, caves and ritual objects around it. Ceremonial, slow, with a pulse from the delays.

DREAM DEPTH (Troum). Bowed and blurred: the feedback bus, the Blur, the Memory, reed organ and murmur and abandoned places, the Ensemble wide. A dream with the edges rubbed off.

LONG TRANSIT (S.E.T.I.). Deep space telemetry: radio-space and geothermal recordings stretched to a standstill, pure tones, the binaural field, the Arc on the hour. The longest holds in the library.

SIGNAL ALGORITHM (Bad Sector). Machine signal: the wavetables digital and the FM pair against them, quantized decisions, the Lenia field and the attractors on the matrix, the filter models at their sharpest. Cold, precise, slightly alive.

COSMIC PROCESSION (Phelios). Cinematic ritual: bowed strings and brass through vast reverbs, the Strike on wood as a procession, the Cosmos and the rotation, the sub carrying it. Slow ceremony on a large scale.

PULSAR FIELD (Arecibo). Radio astronomy: noise as the voice, the pulse from the delays, the Foundation at its lowest, radio-space and seismic recordings. Almost nothing is pitched, and what is, is barely.

SLOW UNFOLDING (Moljebka Pvlse). One gesture over an hour: sparse slots, the Blur, the feedback low, metal creak and abandoned places stretched. The pack that asks the most patience.

FIELD ABSENCE (Francisco Lopez). Granular field recordings, quiet, atonal: the Stretch type over rooms and weather, no tonal anchor at all, levels low enough that the room is the instrument. The Vector moves between four places.

MACHINE DEPTHS (Tho-So-Aa). Industrial depth: noise and stretched factories, tunnels and mud, the z-plane on pipes, the sub at its heaviest. Dark, mechanical and entirely unhurried.

FIELD RECORDINGS (Chris Watson). Places, not instruments: seamless recordings -- rain on twelve kinds of roof, caves, harbours in fog, power stations through a wall -- read as a continuum by the Stretch type, up to four of them on the Vector's corners, a quiet additive centre underneath.

SHAMAN OBJECTS (Voice of Eye). Ritual objects: frame drums, gourds and waterphones in the Cloud, the z-plane as handpan and tabla, the Strike as the exciter, caves and fire around them. Ceremony with things rather than notes.

GHOST SIGNAL (Bass Communion). Granular, wide, processed: shellac crackle and old pianos through the Cloud and the grain shimmer, the Patina high, the Memory ageing what it holds. A recording of a recording.

ATOM SPACE (Atomine Elektrine). Analogue sequences in space: the classic wavetables, the filter models with envelope and drift, delays with cross-feed, the Cosmos wide. The brightest of the cosmic packs.

HYBRID MELANCHOLY (Polygon). Half acoustic, half synthetic: morphing tables against string ensembles and pianos, the z-plane sweeping, the Blur, cold rooms. Melancholy with a hard edge to it.

CORRIDOR (Kammarheit). Dark reverberant rooms, sparse: few notes, long holds, the Room convolution on bunkers and caverns, the near plane almost empty. Space with very little in it.

NORTHERN DARK (Gustaf Hildebrand). Cinematic: sub-bass, wide stereo, the far reverb with rotation, slow z-plane sweeps, a presence lift on what is close. Presets for a film that has not been made.

VOID STATION (Tholen). Cold science fiction: the z-plane as the subject, the Cosmos shifter, digital noise, the feedback frequency-shifted. Nothing organic in it.

STRINGS AT REST (Stars of the Lid). Consonant bowed swells: the Bow type and string ensembles, every layer entering on a contour of its own, long attacks, a just major, the hall as a string section's room.

TAPE SATURATION (Tim Hecker). Bright, distorted, damaged: the feedback bus driven, the fold, pipe organ through it all, the Patina's age high, the grain shimmer. The loudest and most broken presets in the library.

DRUM PROCESSION (Apoptose). Ritual drums: the Strike on wood, the cascade of decisions, chant and bells in cathedrals, the Body's modes on frame drums and timpani. A procession heard from inside the crowd.

SHORTWAVE DARK (Land:Fire). Shortwave at night: radio recordings and carrier tones, noise with the band filter, the Cosmos, the z-plane as comb and tunnel. Signals that almost say something.

MODULAR NOCTURNE (Ian Boddy). Resonant filter movement and echoing sequences: the filter drift and envelope high, autoplay stepping in Chords, delays with cross-feed, the classic wavetables. Presets that move like a patch on a modular.

FOG CITY (Jeff Greinke). Harbour weather: the Blur and the diffuse background, tape keyboards, ships and city recordings, the Room far away. Everything seen through haze.

RIVER LOOPS (Vidna Obmana). Flutes and water: melodic Texture slots over rivers and rain, the Memory looping them, the delays in sync, halls that bloom. The gentlest of the ethnic-ambient packs.

LOW BRASS SWELL (Tom Heasley). Tuba and didgeridoo: brass and overtone horns in the Texture type, every voice swelling in on its own contour, the Memory holding what was played, the formant filter. Breath at the bottom of the register.

GUITAR TWILIGHT (Jeff Pearce). EBowed guitar: guitar and steel guitar swelling in, long delays, the Memory, halls that bloom. Melodic without ever playing a melody.

SILK ROAD SPACE (Amir Baghiri). Ethnic ambient: zithers, bowed folk instruments and frame drums in the Cloud, the z-plane as tabla and handpan, desert recordings behind them. Warm, with a pulse.

WATER TOWER VOICES (Jim Cole). Overtone singing in a cistern: vowel and overtone tables in the Harmonic type, the formant filter, cathedrals and vast rooms, the tuning adaptive so the voices stay pure against each other.

BROWSE (header button): The map zooms about the mouse with the wheel and pans by dragging (any button on empty plane, or the right button anywhere); a double-click puts it back. Zoomed in past four, the presets in the filter show their names, as many as fit. "More like this" narrows the list to the six presets that measured nearest the selected one and zooms the map onto them. Columns narrows the list by Family, Character (dark, bright, tonal, noisy, wide, bass), Motion (calm, moving, dense, sparse) and Features, with search, sort and favourites -- every preset was measured by rendering it, not tagged by hand. FAVOURITES: the star on a row (or Favourite under the list) marks a preset; "only favourites" narrows the list to the starred ones, "favourites first" puts them at the top whatever the sort, and the map rings them in gold. They are kept by name in Documents\Noctuary\favourites.txt, one a line, so they are the same in the standalone and in every DAW and survive a library made anew. Map shows all presets as points clustered by what they sound like; click one to load it, or switch on Map blend and drag the cursor: the synth glides to the blend of the presets around it, so the space between two presets is playable. A ROUTE is a list of waypoints (presets or map positions with travel and hold times) the synth walks by itself: twelve route presets of 20-40 minutes, or your own from the cursor; Speed and Loop as you like.

SETS: Record set on the Perform page logs every knob, macro, route step and note with its time into an .ambientset file; Play set replays it.)" },

    { "Clock and sync",
R"(Nothing in this instrument needs a clock to make sound, but the moment it plays with other machines every rate wants to sit on the grid. Every rate that can has a SYNC choice next to its free knob: the eight LFOs (one cycle per note value, the phase following the beat position so it stays on the grid wherever the transport jumps), the six envelopes (the whole shape spans one value), both delays' left and right times, the ensemble rate, the cloud's grain rate, the brain's decision rate, the arc period, and each source's grain density. Free means the knob rules. The values run from 64 bars to 1/32, with dotted (D) and triplet (T) values.

WHERE THE TEMPO COMES FROM (Conductor > CLOCK > Source):
  Internal  The Tempo knob, counting beats while Run is on. This is the standalone's own clock.
  Host      The DAW's play head: its tempo, position and transport. In the standalone there is none, and the engine falls back to Internal.
  MIDI      MIDI clock at the MIDI input (24 ticks a quarter, Start / Continue / Stop). The tempo settles over one beat's worth of ticks; two seconds without a tick and the engine falls back.
The header shows the tempo and the bar the engine is following. The clock's settings are performance state like the morph: no preset changes your tempo.)" },

    { "MIDI, OSC, files",
R"(MIDI: notes play voices on the Keys Depth plane; the lowest held key becomes the brain's root. Right-click any control for MIDI Learn, then move a controller; right-click again to clear. The mapping is saved with the state. MIDI clock and Start/Stop/Continue drive the clock when its Source is MIDI. In the standalone, pick the MIDI input in Options > Audio/MIDI settings.

OSC on UDP port 9000: /ambient/param/<key> <value>, /ambient/paramn/<key> <0..1>, /ambient/note <n> <vel>, /ambient/preset <index>, /ambient/sound and /ambient/cosmos <index>, /ambient/morph <0..1>, /ambient/hand/L and /R <x y z pinch>, /ambient/head, /ambient/gesture, /ambient/calibrate. A second instance simply reports the port as taken.

FILES: .noctuary (whole state), .ambientpack (preset packs), .ambientset (recorded sets), Scala .scl (Tuning > Load Scala...), wavetables (WAV from Serum, Vital or Hive -- the frame length is read from the file, 2048 samples otherwise -- Surge .wt, or a single cycle), WAV textures (a trailing note name such as "_A3" gives the clip's pitch), WAV impulse responses (mono or stereo, up to a minute). Rec in the header records the output to a 32-bit WAV.

MEASURING: ambient_render renders any preset offline, deterministically, and prints level, spectral centroid, flatness, width, clicks; Tools/preset_check.py runs the sound test over a library. The same descriptors place the presets on the map.)" },

    { "Shortcuts and tips",
R"(F1 or Help          this manual; Escape closes it
Right-click a knob  MIDI Learn / clear, and the modulation routes that drive it
Right-click a card  the routes this source drives, each removable
Drag a card         onto a knob: a new route at a quarter of the target's range
Window corner       zooms the whole page; the arrangement never reflows
Options             (standalone) audio device, sample rate, MIDI input
Perform / Browse    the two other pages; the header stays

TIPS
- Turn Depth up and let the brain run for ten minutes before judging a patch: the arc, the breath and the bloom need time.
- A drone that clicks is a bug, not a feature: every movement in here is continuous by design. If you hear a step, it is worth reporting.
- Source 1 on Texture with a long clip, Source 2 Additive an octave down, Source 3 Noise Wind at low level: three sources, one instrument.
- The Comb filter on a stacked just chord, Resonance 0.7, Key Track 1: the filter becomes a second body that rings in tune.
- For a DAW session set Clock Source to Host and put LFO 1 on 4 bars: the slow breathing lands on the downbeats.)" },
    { "Design I: what the instrument is for, and the rules that follow",
R"(Noctuary was built for one kind of music: the slowly breathing clusters Robert Rich played at his sleep concerts -- dense chords in just intonation, no rhythm, changes that take minutes, a piece that can run all night without repeating and without anyone touching it. Almost every design decision in the instrument follows from taking that brief literally. This chapter and the six after it say which decision follows from what, give the mathematics where there is mathematics, and point at the literature the decisions rest on; the references are collected in the last chapter, cited here by author and year. Where a number is quoted it was measured on the instrument, by the self test or by the offline renderer, and is quoted as a measurement rather than repeated as a claim.

THE TIME SCALE

A drone is heard over minutes, not over the half second a piano note occupies. What changes over those minutes is less the ear than the listener: loudness adaptation for a steady tone at a moderate level is small (Scharf 1983 -- it is mainly at low sensation levels and high frequencies that a tone fades), but attention habituates, a steady spectrum stops being attended to, and slow changes go unnoticed altogether (the change deafness of Eramudugolla et al. 2005). A change that would be a gesture in a song is the whole event in a drone. So the instrument's rates are drone rates. Attacks reach a minute and releases two; the LFOs run from twenty hertz down to one cycle in twenty minutes; the Arc leans on the whole night on a period in minutes; a wandering pitch drifts over a hundred seconds. This is the range in which an ear that has adapted to what is there can still hear that something is moving.

The same time scale is why the instrument has a conductor rather than a sequencer. A sequencer repeats, and repetition is exactly what adaptation punishes. The Cluster Brain draws hold times from a range, intervals between its events from an exponential distribution, notes from a weighted choice; it forgets nothing because it keeps nothing, and two hours of it never recur. The exponential distribution is chosen because it is the one with no rhythm in it: for a Poisson process the waiting time to the next event has the density

    p(t) = lambda * exp(-lambda * t)        mean 1/lambda = the Event Rate

and, crucially, no memory -- the time already waited says nothing about the time still to wait, so the ear can find no pulse to lock on to. The same distribution spawns the grains of the granular sources and of the Cloud, for the same reason.

EVERY MOVEMENT IS CONTINUOUS

A click is the loudest thing a drone can do. A step of height D in a signal has the spectrum of a step: D / (2 pi f), falling only six decibels an octave, so it is broadband and arrives everywhere at once. In a mix with a drum kit it hides behind a hundred transients; in a bed of held tones it is the only transient there is, and the auditory system, which segregates and attends by onsets (Bregman 1990), turns to it at once. So the standing rule of the whole instrument is that nothing steps.

The mechanism that carries most of that rule is the Drifter, the instrument's random modulator. It moves between random targets a and b on a smoothstep curve,

    c(t) = 3 t^2 - 2 t^3,   c(0) = 0,  c(1) = 1,  c'(0) = c'(1) = 0
    x(t) = a + (b - a) * c(t)

so the value is continuous, its first derivative is continuous and zero at every knot, and the direction of change never reverses abruptly. Each segment's duration is scaled by a random factor between 0.7 and 1.3, so the knots themselves fall on no grid. Envelopes are exponential; partial levels ramp across a control block of 64 samples; filter cutoffs, delay lengths and reverb line lengths glide; where a switch cannot be avoided -- a source type, a filter model -- the gains cross-fade over a control block and the new state starts from rest. A controller sending seven-bit steps is smoothed inside the voice before it reaches anything. The self test measures the largest sample-to-sample step of every render it makes; the standing figure on the default patch is 0.06, and the largest step a 440 Hz sine at amplitude 0.5 makes on its own is 0.029.

MEASURE, DO NOT ONLY LISTEN

The instrument renders deterministically offline and prints its own descriptors: RMS level, spectral centroid and spectral flatness, spectral flux, stereo width, energy below 150 Hz, voice count, the largest sample step, the mono loss, and a hash of the samples themselves, quantised to about minus 120 decibels so that a change in what is played trips it while the last bit of a floating-point sum does not. The descriptors are the usual ones:

    centroid  = sum_k f_k |X_k| / sum_k |X_k|
    flatness  = exp(mean_k ln |X_k|) / mean_k |X_k|        1 for white noise, small for a line spectrum
    width     = RMS(side) / RMS(mid),   mid = (L+R)/2,  side = (L-R)/2
    mono loss = 10 log10( RMS(mid)^2 / mean(RMS(L)^2, RMS(R)^2) )

and the self test checks the tuning arithmetic to a billionth and the effects by impulse and sine measurements. The reason for all of it is that ambient sound design is full of confident wrong answers -- a widening that widens nothing, a ducking that measures brighter because it measured a ratio, a resonator normalised at its peak that passes almost nothing of a broadband signal, an envelope that does nothing because the noise band on top of it pins the spectrum. Every one of those happened while building this instrument, every one sounded plausible for an afternoon, and every one was caught by a number.

ONE TABLE

Every parameter -- name, key, range, default, skew, unit, section -- lives in one table, and the engine, the plugin's automation list, the panel, the presets, the packs, the render tool and this manual all read it. That is why a parameter cannot exist in the panel and not in the presets, or in the presets and not in the help. It is also why new parameters are appended and never inserted, and new choices appended to their lists: a host stores automation by slot, a preset stores a choice by its index, and six thousand finished presets must go on meaning what they meant.

WHY ADDITIVE

The voice is a bank of up to thirty-two partials, each a rotating phasor, rather than a stored waveform read through a filter. Each partial h at frequency f_h is a unit vector (c, s) turned once per sample by its own rotation,

    (c, s) <- (c cos w - s sin w,  c sin w + s cos w),      w = 2 pi f_h / f_s

and renormalised once per control block with a single Newton step for 1/sqrt(r^2), fix = 1.5 - 0.5 r^2, so it stays on the unit circle without a square root in the inner loop. There is no table lookup and no phase accumulator, the partials are independent of one another, and the loop vectorises; the whole instrument, effects and conductor included, renders at about forty times real time on one core.

Three consequences of the additive choice run through the instrument. Partials above Nyquist are simply not generated (the bank stops at 0.45 f_s), so there is no aliasing at any pitch. Every partial can move independently -- its level drifting on its own slow curve (Shimmer), its frequency stretched (Inharmonic), its weight set against its neighbours (Odd/Even) -- which is the internal life a Rich drone has and a sample of one does not. And brightness, tilt and inharmonicity become continuous parameters rather than a choice between waveforms, which matters in an instrument where everything is meant to glide. The additive domain is also where the spatial model lives for free: presence, the pad's low cut and the distance-dependent darkening multiply into the partial targets at control rate and cost nothing per sample.

SPACE IS A LANDSCAPE, NOT AN EFFECT)"
R"(

The last of the guiding decisions is the one the third chapter spends its whole length on. In most synthesisers space is a reverb at the end of the chain. Here every note has a distance from the ear, and that single number decides its brightness, its level, how dry it is and how present -- before any effect. Depth then comes from the contrast between a foreground plane and a background plane, width from time differences rather than from level, and focus from a mono low end. Nothing is compressed anywhere, because the distance between the quietest texture and the loudest swell is what the ear reads as the size of the room.)" },
    { "Design II: the voice and its sources",
R"(This chapter is about what a voice is made of and why: the additive bank and its strands, the four source slots and their types, the two filters and the fold, and the struck layer on top.

THE SPECTRUM OF THE BANK

The amplitude of partial h before anything moves is

    a_h = h^(-Tilt) * e_h * w(h)

where e_h is the odd/even weight (with Odd/Even = v > 0 the even partials are scaled by 1 - v, with v < 0 the odd ones above the fundamental by 1 + v), and w(h) is the brightness window: unity up to the last full-level harmonic

    hc = 1 + 31 * Brightness^2

and a raised cosine over the six harmonics after it,

    w(h) = 0.5 (1 + cos(pi * min((h - hc)/6, 1)))     for h > hc.

The tilt is the spectral slope in the sense of the classic waveforms -- a slope of one is a sawtooth's 1/h, two a triangle's 1/h^2 -- but continuous, and the window closes the spectrum from the top the way a low-pass would, without a filter's resonance. Inharmonic stretches the series the way stiffness stretches a real string (Fletcher and Rossing 1998, chapter 2):

    f_h = h * f0 * sqrt(1 + B h^2),       B = 0.02 * Inharmonic^2

which is exactly the stiff-string formula with B the inharmonicity coefficient; at the knob's top the thirty-second partial sits a quarter tone sharp of harmonic. Shimmer multiplies each partial by its own Drifter,

    a_h(t) = a_h * (1 + 0.9 * Shimmer * d_h(t)),      d_h in [-1, 1]

so no two partials breathe alike, and the sum never sounds like a stored waveform.

STRANDS, DETUNE AND BEATING

A single bank is a clean tone; a drone wants a chorus of them, and the strand bank is up to six copies of the bank, each with its own pitch offset, its own slow pitch drift and its own place in the stereo field, summed with a level of 1/sqrt(N) so that N strands are as loud as one. The psychoacoustics is the psychoacoustics of beating. Two tones at f and f + df sum to

    sin(2 pi f t) + sin(2 pi (f+df) t) = 2 cos(pi df t) sin(2 pi (f + df/2) t)

a tone at the mean frequency whose amplitude beats at df. At eight cents around 220 Hz that is about one beat a second, which the ear hears as movement and warmth; Plomp and Levelt (1965) put the transition to roughness at a difference of roughly a quarter of a critical bandwidth -- twenty to thirty hertz in the midrange -- and the strand bank stays well below it. Detune sets the spread; the per-strand Drift, on its own curve, keeps the beats from settling into a fixed pattern; Spread fans the strands out so the beats happen between the ears as well as in time.

Stack replaces the detuned copies with pure ratios -- octaves (1 2 1/2 4 1/4), fifths (1 3/2 2 3 1/2 9/4), a just major (1 3/2 5/4 2 5/2 1/2) or minor, sevenths, the harmonic and the subharmonic series -- ordered so that two strands already make root and fifth and three a triad. With Detune at zero the chord is beat-free by arithmetic: the fifth's second partial at 2 * (3/2) f = 3 f is the root's third partial exactly, and every coincidence of that kind locks. One key becomes a just chord. Detune and Drift still apply on top, so the same chord can be set slowly beating, which is where much of the instrument's character lives: harmony that breathes at the rate of its own mistuning.

Bloom holds the brightness back at the start of a note and opens it over a time T,

    Brightness(t) = Brightness * (1 - Bloom * (1 - c(t/T)))

with c the smoothstep above, because a drone's arrival is heard as a spectrum opening rather than as a level rising. Rate Wander scales every slow rate in the voice -- pitch drift, pan, filter, air, shimmer, breath -- by 2^(w * d(t)) with d a hundred-second Drifter, the nested modulation that makes five minutes never resemble the five before.

FOUR EQUAL SLOTS AND THE VECTOR

Every voice has four source slots, equal on purpose. Three slots of the same kind a fifth and an octave apart are a chord out of one key; three of different kinds are an instrument; and the fourth exists so that the Vector has four sources at the corners of its square and not three plus their sum. Each slot has a type, a level, an octave, a just ratio to the note and a pan, and goes through the voice's filter, envelope, distance and interaural delay like the bank.

The Vector is the idea of the Sequential Prophet VS (1986) and the Korg Wavestation (1990): the timbre as a place. A point (x, y) in the unit square gives the four corners bilinear weights

    w1 = (1-x)(1-y),  w2 = x(1-y),  w3 = (1-x) y,  w4 = x y,      sum = 1

and each slot's level is multiplied by

    g_k = 1 + Amount * (4 w_k - 1).

At the centre every w_k is 1/4 and every g_k is exactly 1, so turning Amount up on a patch you like changes nothing until the point moves; at a corner the corner's slot is at 1 + 3 Amount and the others at 1 - Amount. That neutral centre is an instance of a rule the whole instrument follows for every feature added after the first presets existed: neutral at the default, so nothing that was finished sounds different.

WAVETABLE AS SPECTRA

The Harmonic type is a table of spectra, not of samples: up to 64 frames of 32 partial amplitudes, rendered by the same phasor bank as the main oscillator, with Position interpolating linearly between neighbouring frames. Interpolating spectra is what a wavetable morph sounds like it is doing and, in a sample-based table, is not: there the morph cross-fades waveforms whose partials may stand at opposite phases and cancel on the way. Here nothing cancels and nothing aliases, and every trick that works on partials -- the presence bell, the low cut, the feedback's phase modulation -- works on the wavetable as it does on the bank. A user table is analysed from its file, one transform per frame, bins 1 to 32, and becomes spectra too. (Until the classic kind below arrived, this type was called Wavetable.)

The Wavetable type is the other kind, the one Serum, Vital and Hive play: up to 256 single cycles of 2048 samples, read as samples with a four-point interpolation, Position blending one frame into the next. The shape of the wave is kept -- its phases, and every harmonic up to the 512th -- so a saw is a saw and a table drawn for a formant sweep sweeps the way it was drawn. What keeps a high note from aliasing is a stack of copies of every frame, each an octave poorer than the last, from which a note reads the richest whose top harmonic still lies below Nyquist; when a gliding note crosses from one copy to the next, the two are crossfaded over one control block. One file loads into both types: a WAV from Serum or Vital names its frame length in a chunk and that length is used, a Surge .wt gives it in its header, and a file that says nothing is read as 2048-sample frames -- or recognised as a bank of shorter cycles laid end to end, or as a single cycle of its own length. Unison, detune and width spread the cycles the way they spread the Harmonic type's copies, and the two types of one table enter the mix at the same level.

FM

The FM pair is Chowning's (1973): a carrier at the slot's pitch phase-modulated by a modulator at Ratio times that pitch,

    y(t) = sin(2 pi f_c t + I sin(2 pi f_m t)) = sum_k J_k(I) sin(2 pi (f_c + k f_m) t)

whose sidebands at f_c + k f_m carry the Bessel weights J_k(I). Integer ratios give harmonic spectra, bells and electric pianos; a ratio a little off an integer gives a bell that beats. Above three kilohertz the index I is reduced, because the significant sidebands extend to about k = I + 1 on either side and a high note with a large index would put them past Nyquist; the reduction is what keeps the top of the keyboard from turning to aliased noise.

TEXTURE

The granular type follows Roads (2001): grains of a loaded clip, each a Hann window over Grain milliseconds, spawned at exponentially distributed intervals around Density, starting at Position scattered by Spread, each with its own pan, overlapping freely up to Grains at once. Two of its constants were measured rather than assumed. A clip enters at its own level while the wavetable and FM types normalise to unity, so a reading gain is set from the clip's RMS; and a Hann-windowed stream at overlap N has an RMS of

    RMS = sqrt(N) * 0.612 * RMS(source)

so the normalisation that returns the source's level is 1 / 0.612 = 1.63, not the 0.7 / sqrt(N) the first version used. Together those two were twenty-two decibels, and the manual's promise that every source type lands within a decibel of the others at the same Level rests on them.

STRETCH

Stretch reads the same clip as a continuum: Paulstretch (Nasca 2006) inside a voice. A window of N samples of the recording (Grain, up to 16384 samples) is transformed, its magnitudes |X_k| kept, its phases replaced by fresh uniform random phases,

    Y_k = |X_k| * exp(i phi_k),     phi_k ~ U(0, 2 pi))"
R"(

and the inverse transform is windowed and overlap-added at a hop of N/4 with a gain of 1.3 to restore the level, while the analysis position advances by hop / Stretch per frame. Every output frame is a plausible slice of the clip's spectrum with no memory of where its transients were -- the random phases destroy the temporal fine structure and keep the spectral envelope -- so a recording read forty times slower has no grain rhythm and no attack left standing. Pitch is applied when the window is read, as a resampling step through the clip, and the stretch to how far the read moves between frames; the two do not know about each other, which is what lets a chromatic sample play across the keyboard without a high note ending sooner than a low one. Measured: the pitch comes out identical to the granular player's, 110.7 Hz free and 220.3 Hz at A3.

BOW

The one source in the instrument that is a physical model rather than a description of a spectrum. The string is two digital waveguides meeting at the bow (Smith 2010): from the bow to the nut and back is 2a samples, to the bridge and back 2b, and a + b is half a period. The nut reflects and inverts; the bridge reflects, inverts and loses the highs through a one-pole whose damping is Bright, which is why the upper partials die first as they do on a real string. At the bow the relative velocity between hair and string decides how much force is transmitted, through the friction characteristic of McIntyre, Schumacher and Woodhouse (1983):

    dv = v_bow - (v_left + v_right)
    rho = min(1, (|(dv - 0.001) * slope| + 0.75)^-4),    slope = 5 - 4 * Force
    f = dv * rho

and that force is added into both outgoing waves. Everything about this sound is in the shape of rho. It has to FALL as the slipping gets faster -- more slip, less force -- because that negative resistance is what feeds the oscillation. The first version of this model used a curve that merely saturated, rho = (F/|dv|)^0.8 clipped at one, and it has a stable fixed point: the string sticks to the bow and stays there. Measured in the engine it was a constant with no pitch at all, RMS 0.0000. The corrected curve gives the Helmholtz motion, and the model then measures 222 Hz for an A3, 110 Hz an octave down, at the level of a wavetable slot to a tenth of a decibel.

A bow that stops moving does not simply stop driving: the hair is still on the string, and hair that does not move absorbs. Without that the junction is a lossless termination and a stopped note rings on for seconds, which is also what the test found. A small contact loss, confined to the bottom sixth of the Speed range so that nothing being played is touched by it, takes a stopped note down by seventy-three decibels in a tenth of a second.

SPECTRAL

Every other type that reads a clip still plays samples: granular cuts it into pieces, Stretch smears its spectrum, and in both, pitch and speed are entangled in the one number that says how fast the recording is read. Spectral does not play samples at all. When a clip is loaded it is measured once into thirty-two bands one equal step of the ERB rate apart (Glasberg and Moore 1990, the same scale the loudness meter uses), and what is kept per frame is three numbers per band: how loud it is, where in it the strongest partial sits, and how tonal that content is. Tonality is measured as how far the content stands above the LOCAL noise floor -- the geometric mean of the magnitudes over twenty-one bins either side -- which is the one decision that makes the type work on anything. It needs no fundamental, so nothing has to be found that could be found wrong: a bell measures as nearly all partial, rain as nearly all noise, a voice as both.

Playback rebuilds each band from an oscillator at that band's partial and a band of noise at the same place, weighted by the tonality and its complement. This is the deterministic-plus-stochastic decomposition of Serra and Smith (1990) taken band by band instead of partial by partial. Because nothing is a sample any more, the note sets the pitch and Rate sets the speed and neither knows about the other; at Rate 0 the read head stands still and one moment of a recording becomes a chord held for as long as the note lasts, transposed wherever it is played. Breath tilts the balance to the partials alone or to the noise alone -- a rain recording turned into the chord hiding inside it, or a struck bell turned into the wind that has its shape.

Measured on a test clip that is two seconds of tone followed by two of noise: the tonal half reads 1.00 and the noisy half 0.20; a 300 Hz partial is placed at 299.9 Hz rather than at its band's centre; an octave up is an octave up to within half a per cent; and the level lands within a decibel of a wavetable at the same setting.

NOISE

The ten colours are the textbook slopes plus four shaped ones. White is flat; pink falls 3 dB an octave (equal energy per octave, 1/f); brown falls 6 (1/f^2, integrated white); blue and violet rise by the same amounts. The pink filter is the full seven-term form of Kellet's approximation, because the common three-pole short form is 1.7 dB an octave too steep; measured slopes are white -0.1, pink -3.1, brown -6.0, blue +2.8, violet +5.5 dB per octave. Grey is white noise weighted by the inverse of an equal-loudness contour (ISO 226), so that it sounds flat rather than measuring flat -- the one colour defined by psychoacoustics rather than by physics. Every colour is level-matched to a wavetable slot at the same Level; before that, violet sat eleven decibels above pink at the same setting.

AIR

Air is filtered noise on the note: a state-variable band-pass around a chosen harmonic of the fundamental with a Q from the knob, or -- in Ghost mode -- six sharp resonators on the harmonics 1 2 3 5 7 9, the harmony filtered out of chaos. It exists because a real sustained tone is never only its partials: a bowed string, a blown pipe, a voice all carry broadband noise shaped by the same resonances as the tone, and the ear uses that noise as the cue that the source is physical. It is also, in this instrument, the cause of more measurement traps than any other section: a broadband band on top of the spectrum pins the centroid, so a filter sweep or an envelope measured with Air on appears to do almost nothing. Anyone measuring the instrument learns to switch it off first.

THE VOICE FILTER

Nine of the ten models are built on the topology-preserving transform of Zavalishin (2018): the analogue prototype's integrators are replaced by trapezoidal ones and the zero-delay feedback is solved algebraically rather than broken with a unit delay. For the state-variable filter that gives

    g = tan(pi f_c / f_s),     k = 2 - 1.9 R          (R the Resonance knob, k the damping)
    hp = (x - (k + g) s1 - s2) / (1 + g (g + k))
    bp = g hp + s1,   s1 <- g hp + bp
    lp = g bp + s2,   s2 <- g bp + lp

and the reason it matters is that a filter built this way can be swept quickly without detuning or clicking, and a swept filter is the normal state of a filter in this instrument. The one exception is deliberate: the four-pole Ladder keeps a sample of delay in its feedback path, because that delay is part of what the model sounds like and every preset that uses it was voiced with it; its corner sits 1.55 times above the knob so the minus-3-dB point lands near Cutoff, and its resonance is squared because the interesting range is at the top. The Comb is tuned to Cutoff with a low pass in the loop and its output scaled by 1 - feedback so the peaks stay at unity and Resonance deepens the dips instead of raising the level:

    y[n] = x[n] + fb * LP(y[n - D]),   D = f_s / f_c,   out = (1 - fb) y

-- on a sustained cluster less a filter than a second resonating body. The formant model is three band-passes on the formant frequencies of the sung vowels u-o-a-e-i (the classic tables of Peterson and Barney 1952), morphed by Cutoff. Every model reports its own magnitude response from the same arithmetic as the audio path, which is what the filter display draws.

THE Z-PLANE

The z-plane filter is Rossum's idea from the E-mu Morpheus (US patent 5,170,369, 1992): filter frames on the corners of a cube, a point inside it a filter interpolated between them, the whole resonant structure gliding as the point moves. A frame is up to six cascaded second-order sections, each a pole pair and a zero pair,

    H(z) = prod_i (1 - 2 r_zi cos t_zi z^-1 + r_zi^2 z^-2) / (1 - 2 r_pi cos t_pi z^-1 + r_pi^2 z^-2)

with the angle t = 2 pi f / f_s and the radius from the bandwidth, r = exp(-pi B / f_s). The interpolation is trilinear on the parameters -- log frequency and log bandwidth of every pole and zero -- and never on the coefficients. That is the whole reason the design is stable: a positive bandwidth interpolated in the log domain stays positive, so r stays below one and every point inside the cube is a stable filter by construction, where an interpolation of coefficients can pass through unstable filters between two stable corners. The 155 shapes are generated from published acoustics rather than typed: the mode series of bars, bells and membranes from Fletcher and Rossing, the formants of the vowels, the modes of a room from c / 2L. Sections come in two kinds, and the distinction matters: a bell (its zero on its pole, wider) boosts its frequency and leaves the rest at unity, so six in series shape a spectrum; a resonator (no zero, or a zero elsewhere) passes only its band, so a few in series take the sound over completely, and six narrow ones cancel each other to nothing -- the self test found a first draft of the cluster shapes at minus 120 dBFS. The bank is normalised twice per control block from a probe grid that includes every pole and zero angle, because a fixed grid alone walks past a needle-sharp resonance, so a shape can be extremely resonant without being loud.

Modal mode reads the same data as what it is. Six sections in series take away what is not wanted and stop when the input stops; six two-pole resonators in parallel, each with its own decay, keep ringing after the input has gone, which is what a bar, a bell, a membrane or a room does. That is modal synthesis (Smith 2010; Bilbao 2009). A resonator with the decay time T60 has the pole radius

    r = 10^(-3 / (T60 f_s))

and Damping sets how the higher modes die: the k-th mode's decay is T60 * (f_1 / f_k)^Damping, from every mode holding equally at 0 -- which no real object does -- to decay inversely proportional to frequency at 1, which is roughly what wood, metal and skin do. Each resonator is normalised to unity at its own frequency, and the bank on its expected power rather than on the sum of its peaks, because the modes are at different frequencies and almost never in phase.

THE FOLD

The wavefolder exists because saturation cannot make a certain sound. A clipper flattens what will not fit, and a flattened wave gains shoulders: odd harmonics falling quickly, the sound of warmth or of overdrive. A folder reflects what will not fit back into the range, and a wave mirrored at the fold gains a family of high partials whose strength grows with the drive -- the metallic edge the dark-ambient literature reaches for. The curve here is a sine divided by its own gain, the smooth version of the triangular fold, with the positive half driven a third harder than the negative:

    g = 1 + 9 A,    a = g * 1.33 for x >= 0, g otherwise
    y = sin(a x) / a * (1 + 2.2 A),      out = x + A (y - x)

For a sine input x = sin(wt) the Jacobi-Anger expansion gives

    sin(a sin wt) = 2 sum_k J_{2k+1}(a) sin((2k+1) wt))"
R"(

odd harmonics only, with the Bessel weights J_{2k+1}(a) spreading further up the series as a grows -- this is the waveshaping synthesis of Le Brun (1979). The asymmetry breaks the odd symmetry and adds the even harmonics, which is where the body of the sound is. Because sin is entire, there is no corner anywhere to alias off, and because sin(u)/u tends to one, small signals pass unchanged; the amount both drives and mixes, so the knob leaves the identity continuously. The makeup gain was measured: 2.2 holds a 0.3-amplitude sine to within 1.3 dB across the knob, and a loud input loses about nine decibels at the top, which is not a fault -- past the first fold the fundamental itself is being folded away, and J_1(a) is already falling.

THE STRIKE

Strike is the plucked string of Karplus and Strong (1983), with the extensions of Jaffe and Smith (1983): a delay line of length L = f_s / f_0 fed back through a two-point average,

    y[n] = x[n] + 0.5 (y[n - L] + y[n - L - 1])

excited by a noise burst at note-on, whose loop filter makes the high partials decay faster than the low ones exactly as a real string does. Wood is the same loop two octaves up with heavy damping and a short decay; Metal has an all-pass in the loop, which stretches the partials inharmonically. It is placed on the near plane whatever the voice's distance, because the ear takes proximity from onsets: a sharp attack, close and dry in the centre, reads as near, and a foreground that has one makes the background behind it read as vast.

Who strikes, and how often. For a long time the answer was all or nothing: with Fires on Keys only your own notes struck, so an instrument left to play by itself never struck at all, and with Fires on Keys + Brain every note the conductor placed was struck, which is a plucked instrument rather than a room in which something is occasionally touched. Chance is the missing middle -- the conductor's notes strike with that probability, one in five at 0.2, your own keys always -- and Cluster decides how those survivors are distributed. Falling independently, they are a thinned version of the note stream, evenly scattered; but the conductor's own clock is a Hawkes cascade, whose excitation rises when an event breeds events, and Cluster weighs the coin by that excitation against the average it has been running at:

    p = clamp(chance * (1 + 2 * cluster * (excitation - mean excitation)), 0, 1)

so a note arriving in a cluster of events is likelier to be struck and one in the quiet stretches is likelier not to be, while the count over an hour still follows Chance. The first draft weighed the coin by the excitation itself rather than by its distance from the average, which saturated: at any excitation worth having the probability clamped to one and every note struck again. Measured over five minutes of a conductor at two-second events (self test): 123 notes, Chance 1 strikes all 123, Chance 0.33 strikes 42, Chance 0 none; at Chance 0.3 with a cascade running, an even coin strikes 51 times with a coefficient of variation of 0.85 between the gaps, and Cluster at 1 strikes 57 times with 1.41 -- the same number of strikes, arriving in handfuls. The coin is drawn from a stream of its own and only when the chance is below one, so every preset written before there was a chance renders bit for bit as it did.

The Cosmos was asked the same question and answered differently. It is a send and not an event: everything on the near bus goes into it, so it sounds whenever anything sounds, and the measurement said so plainly -- over five minutes with the conductor alone the Cosmos bus sat at -34 dBFS with one second in three hundred below a fiftieth of its peak. Nothing was ever triggered because nothing needed triggering; what it never did was rest. Swell gives it that. The send follows the conductor's excitation relative to the average that piece has been running at, so it opens inside a cluster of events and closes in the long gaps, and because it is measured against the average and not against an absolute number, a piece with no cascade at all is left exactly where it was -- which is why it can be on by default. The excitation is smoothed over two seconds first: the cascade's kick is a step, and a step on a send is a click. The same excitation is now a modulation source too, so anything else can be made to breathe with the piece's own busyness rather than with a clock.)" },
    { "Design III: space, depth and width",
R"(This is the chapter the instrument is built around. The spatial model comes from Robert Rich's practice as much as from the literature, and the literature it draws on is the psychoacoustics of localisation and distance -- Blauert (1997) for spatial hearing as a whole, Zahorik (2005) for distance, Rayleigh (1907) for the duplex theory, Brown and Duda (1998) for the structural model of the head.

ONE NUMBER PER NOTE

Every note has a distance d between 0, at the ear, and 1, the infinite background. The brain places its notes bimodally -- forty per cent close, at Depth * 0.15 u, sixty per cent deep, at Depth * (0.55 + 0.45 u), with u uniform -- and the keys sit at Keys Depth. From that one number the voice derives four things, and they are the four cues the ear uses to judge distance (Zahorik 2005 reviews them: intensity, direct-to-reverberant ratio, spectrum, and for very near sources the binaural cues):

    cutoff   f_c(d) = f_c * 2^(-2.5 d)                 air absorption
    level    A(d)   = 10^(-6 d / 20)                    the inverse-distance law, softened
    dryness  near gain cos(d pi/2),  far gain sin(d pi/2)      cos^2 + sin^2 = 1
    presence P(f, d) = G (1 - d) * max(0, 1 - ((log2 f - log2 3200) / 0.8)^2)   in decibels

Air absorbs high frequencies far more than low ones -- the molecular relaxation of oxygen and nitrogen, tabulated in ISO 9613-1 -- but honestly, not by much: at 20 degrees and 50 % humidity the loss at 4 kHz is about 0.02 dB per metre, under half a decibel over twenty metres. The two and a half octaves per plane unit are therefore not atmospheric physics. They are the recording engineer's convention that far is dark, which in a real room comes from absorption at the walls, from sources heard off their axis and above all from the direct-to-reverberant ratio, and it is tuned by ear against Rich's recordings. The literature agrees that the spectral cue to distance is weak on its own (Zahorik 2005) and that the ratio of direct to reverberant sound is the strong one (Bronkhorst and Houtgast 1999), which is what the cosine-sine split below implements; the darkening is the stylisation on top. Level alone is a weak cue -- a quiet close sound and a loud distant one are told apart by their spectra and their reverberation, not by their level (Zahorik 2005) -- so the level falls by only six decibels per unit. The cosine-sine split keeps the total power constant while the direct-to-reverberant ratio, which is the strongest distance cue in a room, falls from all direct to all reverberant. And the presence bell -- a parabola in log frequency centred on 3.2 kHz, zero at plus or minus 0.8 octave, so about 1.8 to 5.6 kHz -- is the proximity of a close microphone and the reason a foreground voice sounds articulate rather than merely loud; multiplied by 1 - d it is gone on the far plane.

Because the four cues move together from one number, a voice that moves in depth moves believably. Breath lets every voice's distance wander by up to 0.35 on its own Drifter, and everything hanging on the distance moves with it: the room breathes. Doppler adds the last cue -- the breathing distance has a velocity, and the pitch follows it,

    f' = f * (1 - v / c),    one plane unit taken as about twenty metres,  up to 3 %

which the ear reads as approach and retreat.

WIDTH FROM TIME, NOT FROM LEVEL

Stereo width here is built on Rayleigh's duplex theory (1907): below roughly 1.5 kHz the ear takes direction from the interaural time difference, above it from the interaural level difference, because the head shadows wavelengths shorter than its own diameter and not longer ones; Wightman and Kistler (1992) showed that when the two cues conflict, the low-frequency time difference dominates. A pan pot makes only level differences, the wrong cue for the part of the spectrum a drone lives in. So every voice is rendered with a true interaural time difference: a fractional delay on the far ear's channel of

    tau = 0.65 ms * TimeWidth * |pan|

gliding, never jumping. The 0.65 ms is where Woodworth's formula puts the maximum for a human head: tau = (a / c)(theta + sin theta) with the head radius a about 8.75 cm gives 0.65 ms at ninety degrees (Blauert 1997). Width from time survives a mono sum in a way width from level does not, and the default patch's stereo correlation fell from 0.36 to 0.11 when the model went in.

Phase Width goes further in the same direction. Two first-order all-passes per ear, with corners at 300 and 1500 Hz, drift apart and back on one slow curve -- the left ear's up, the right ear's down, by up to 1.5 octaves. A first-order all-pass

    H(z) = (a + z^-1) / (1 + a z^-1),   a = (tan(pi f_c / f_s) - 1) / (tan(pi f_c / f_s) + 1)

has unity magnitude at every frequency and a phase that turns through 180 degrees around f_c; sweeping f_c differently in the two ears changes the interaural phase without changing the level, and the ear reads a room changing size rather than a sound moving. Externalise adds the two cues a headphone image needs to leave the head, taken from Brown and Duda's structural model (1998), the parts that need no measured head: the notch the pinna cuts into what arrives from the side, whose frequency moves with the angle of the source, and the reflection off the shoulder about a quarter of a millisecond later. On speakers the room supplies those cues and the switch is left off.

The Haas effect (Haas 1951; the precedence effect of Wallach, Newman and Rosenzweig 1949) -- one channel delayed by ten to thirty milliseconds -- is the classic widener and the classic mistake. Summed to mono the two channels comb: a signal and its copy delayed by D have the magnitude

    |1 + exp(-i 2 pi f D)| = 2 |cos(pi f D)|

with notches at f = (2k+1) / (2D), the first at 33 Hz for D = 15 ms, in the bass. Here the effect is done to one band only, between about 1.2 and 4 kHz where the ear takes its direction from level and is least troubled by a delayed copy, and the delayed band is put into the side channel -- added on the left, taken off on the right --

    L' = L + g b(t - D),   R' = R - g b(t - D),      (L' + R') / 2 = (L + R) / 2

so a mono sum is exactly the picture it was, to the sample. The edges open; the bass and the top stay where they were.

The Headphones binaural mode takes the same model one step further. With it on, the pan becomes an azimuth round the head (ninety degrees at full pan), the interaural delay follows Woodworth's formula exactly rather than a straight line, the head shadow is at full strength whatever Time Width says, and a source that ends up behind the head gets a lower pinna notch, which is most of what tells back from front with no visual cue (Brown and Duda 1998). And it listens to the head: a headset, or an OSC head tracker sending /ambient/head, gives the engine the head's yaw, and the whole field is turned the other way,

    azimuth = pan * 90 deg - yaw,   tau = (a / c)(|theta| + sin |theta|)

so a voice stays where it is in the room while the listener looks round. That dynamic cue is, by the current research, the strongest single contributor to externalisation on headphones -- stronger than the pinna's spectral detail (Best et al. 2020; Hendrickx et al. 2017), which is why the mode exists at all rather than another filter. Measured: with the head turned ninety degrees a centred voice arrives at the ears with the same interaural delay as a hard-panned voice with the head straight, and that delay is Woodworth's 31.5 samples at 48 kHz plus the two or three the far ear's shadow filter adds as group delay -- which a real head adds too. Off, it is bit-identical to before.

The binaural mode also carries the head shadow itself, which the ordinary pan does not. A sphere the size of a head attenuates and delays what arrives at the far ear by an amount that depends on frequency and angle, and Brown and Duda give it as a one-pole, one-zero filter that needs no measured head:

    H(s) = (1 + alpha s / (2 w0)) / (1 + s / (2 w0)),     w0 = c / a
    alpha(theta) = 1 + a_min/2 + (1 - a_min/2) cos(theta / 150 deg * pi),   a_min = 0.1

with a the head radius and c the speed of sound. At ninety degrees away the far ear loses the top end; at the front nothing happens; and behind the head the filter opens again, which is why the shadow alone cannot tell front from back and the pinna notch has to. It runs only in the binaural mode: on speakers the listener's own head does this, and doing it twice would be wrong.)"
R"(

The Ensemble's Microshift is the same argument applied to detuning. A chorus at thirteen to twenty-two milliseconds is a comb filter waiting to be summed; two channels detuned by c cents in opposite directions, at ratios r = 2^(c/1200) and 2^(-c/1200), and at different base delays, are never at a fixed phase difference, so there is no comb to cancel into. A pitch shift by a delay line is a delay that changes at a constant rate, d(t) = d_0 + (1 - r) t, and since it cannot change for ever it is wrapped: a ramp of 200 ms of travel and a 25 ms equal-power hand-over to a second tap one ramp behind. The textbook construction -- two taps half a cycle apart under a Hann pair -- was measured and rejected, because both taps are audible all the time at a fixed delay difference, which on a sustained tone is the comb above; it lost a fifth of the signal. With the long ramp the two taps overlap for a thousandth of the cycle. Measured by counting zero crossings of a 440 Hz sine: 443.06 Hz left, 436.96 Hz right, twelve cents each way to within half a hertz.

The third mode, Velvet, drops the pitch shift entirely and decorrelates the two channels with velvet noise: a sparse sequence of impulses of plus or minus one over the square root of their number, one impulse placed at random inside each equal interval of a thirty-millisecond span, convolved with the signal, a different sequence per channel. Sparse enough and the ear hears no echo; random enough in sign and position and the two channels share no comb. The claim is not that it decorrelates better -- measured on a sustained tone the chorus and the velvet mode decorrelate about equally, 0.05 either way -- but that it does so without colouring: 1.32 dB of spectral colouration against the chorus's 5.41. That is what the literature on velvet noise says it is for, and it is the reason to have a third mode at all: the same width with none of the comb.

THREE TIERS, AND THE FUNNEL

There are three reverbs because a room has three kinds of reflection. The near reverb is the small room around the dry voices: short, bright, what makes a foreground sound placed rather than pasted. The far reverb is the infinite background: a feedback delay network in the sense of Jot and Chaigne (1991) and, before them, Schroeder (1962) -- eight delay lines behind four input all-passes, per-line damping, a slow modulation of the line lengths so no mode ever stands still -- heard by the far plane alone, 100 % wet, with a decay in tens of seconds. For a decay time T60 the gain of a line of length L_i samples is

    g_i = 10^(-3 L_i / (T60 f_s))

The far reverb has a second mode, Scattering, after Schlecht and Habets (2020): a short Schroeder all-pass inside every delay line's loop, with mutually prime lengths between 1.9 and 7.1 ms, so that each pass round the network scatters every echo into many. The echo density -- measured as the fraction of samples above the local RMS, which for Gaussian noise is 0.317 (Abel and Huang 2006) -- reaches 0.76 of Gaussian after fifty milliseconds against 0.60 for the classic network, with the same decay time and a late tail at least as smooth. The classic mode is the default and unchanged, because the library was voiced with it; the difference is a texture of tail, denser and more diffuse, not a different room.

There is a third mode, Colourless, and it changes nothing in the structure: only the eight line lengths. A feedback delay network's tail is not flat -- its modes pile up wherever the line lengths share arithmetic -- and how flat it comes out is decided by those eight numbers alone. So they were searched for rather than chosen. Tools/optimise_fdn.py renders the network's impulse response, measures the spread of its late spectrum in decibels, and hill-climbs the lengths under the usual constraint that they stay mutually prime. Sixty iterations from three seeds gave

    29.7  31.4  39.3  46.6  55.1  58.6  68.4  89.0 ms

which measures 0.314 dB of spread against the classic set's 0.474 -- a third flatter, at the same decay time. This is the direction the recent literature on colourless reverberation points (Schlecht and Habets), reached by measurement rather than by design: the search does not know why the answer is better, and neither, honestly, does this chapter. It knows that it is, by a number.

THE EARLY ROOM

The three tiers above are all TAIL. What tells a listener the size of a room and where in it a source stands is not the tail at all but the first few reflections: their delays give the dimensions, their directions give the geometry, and the way both move when the source moves is what makes the room a place rather than a wash (Blauert 1997 for the direction, Bronkhorst and Houtgast 1999 for the distance). A reverb whose early part does not move when the source moves is a room the source is not in.

So there is a fourth stage, off by default, which is a scattering delay network in the sense of De Sena, Hacihabiboglu and Cvetkovic (2015). A node sits at the centre of each of the six walls of a shoebox whose proportions have no simple ratio between them. A delay carries the source's sound to each node, and another carries each node's pressure to the listener, both at the real distances and with spherical spreading over the whole path. At each node a one-pole takes the wall's absorption out, and the incoming waves are scattered to all the other nodes by the isotropic matrix

    p(k to m) = (2/(N-1)) * sum over j of p(j to k)  -  p(m to k),      N - 1 = 5

which is its own inverse, so it moves energy between the surfaces without creating or losing any; a single gain below one decides how long the early field runs before the far reverb takes over. Each wall's contribution is panned by its own direction, so the left wall answers a source on the left first.

The first version of this had the nodes scattering into one another after a delay of two samples rather than after the distance between them, and it is worth saying why that is not a small error. Two samples is a scattering loop at 24 kHz: within a few samples all six nodes carry the same signal, every direction is gone, and what is left is a diffuser. Measured, moving the source from one side of a ten-metre room to the other changed the balance between the ears by 0.02 dB -- which is to say, by nothing, in a stage whose entire purpose is that it should change. With the real inter-node distances in place the same test measures 1.60 dB, the first reflection of a four-metre room arrives at 6.8 ms and of a twenty-four-metre room at 35.5, and the two are the right way round.

The measurement had a trap of its own. Taken over the whole quarter-second the side balance reads 0.04 dB even with the network correct, because after a few passes of the scattering the energy has been round every surface and points nowhere. Only the first thirty milliseconds carry direction, and that is where the test looks.

The convolution room is a measured or designed space in parallel on the far plane, by partitioned convolution (Gardner 1995) with partitions that grow along the impulse (Battenberg and Avizienis 2011, Wefers 2015): 256 samples over the first 85 ms, 2048 up to 0.68 s, 16384 for the rest. Each stage's input blocks enter a frequency-domain delay line, every output block is the sum over partitions of input spectrum times impulse-partition spectrum, one inverse transform per block, overlap-added -- exact, with 5 ms of latency that the pre-delay absorbs. A late partition's answer is due long after its input arrives, so that work is spread over the time in between, and the long partitions of the tail are cheap: an impulse of a minute costs less than eight seconds did with 512-sample partitions throughout, and no single callback carries the load. Only the spectrum bins that matter are stored per partition, a mono impulse is read once for both ears, and Room Morph blends the two impulses inside the one convolution. Pre-delay on each reverb is the gap between the direct sound and the first reflection, which the ear reads not as the room's size but as where the source stands in it (Blauert 1997): a long pre-delay puts the source close and the wall far, a short one merges it with the room.

The far reverb's Asymmetry stretches the right-hand lines by 1 + 0.08 a and delays the right output by up to 10 ms, so the two ears hear different reflections -- decorrelation, which is what the ear needs from a reverb to hear it as space rather than as a wash (an interaural cross-correlation below about 0.3 is the classic figure for spaciousness). Its Width is the funnel: the background's own stereo width, pulled in towards the centre while the foreground stays wide,

    M = (F_L + F_R)/2,  S = (F_L - F_R)/2 * Width,     F_L' = M + S,  F_R' = M - S

applied to the far bus alone before it joins the near bus. A mix in which everything is spread as far as it will go is a flat wall, because width is a relative cue and a wall of equal width has no depth in it; a narrower background behind a wider foreground reads as distance. Measured: width 0 leaves no side at all, 1 leaves the reverb's own, 1.5 is wider, and the mid never moves.

Every reverb has a low cut beside its high cut -- two one-poles, 12 dB an octave, their common corner set 1.5538 times below the knob so the minus-3-dB point lands where the knob says -- which is the filter funnel a mixing engineer puts on a return: dense tails pile up between 200 and 450 Hz, and that is where a background stops sitting behind the music and starts covering it. Unmask lets the background step aside for the foreground band by band: three bands split at 300 Hz and 2.5 kHz, the near bus as the side chain, an attack of 50 ms and a return of 1.2 s. It is the frequency masking of the ear (Zwicker and Fastl 1999) turned into a control -- a loud component masks quieter ones in the same critical band -- and a pad that does not duck its own reverb swallows its own notes. Masking in the ear is not symmetric: a tone masks the frequencies above it far more than those below, the upward spread of masking, with a masking pattern that falls steeply towards lower frequencies and shallowly, and more shallowly the louder the masker, towards higher ones. Unmask Spread puts that asymmetry into the control: a band is also masked by the bands below it,

    e_b = env_b + spread * (0.5 env_{b-1} + 0.25 env_{b-2} + 0.1 env_{b+1})

so a bass note in the foreground thins the background's middle as well as its bottom. At 0 the three bands are independent, as they always were. Measured: with the spread on, a bass note in front ducks the far reverb's middle to less than half of what it did without, and the top less than the middle.

HEIGHT

The landscape had two axes, depth and width, and no up. The ear hears up and down at the pinna,
and the two cues are simple enough to need no measured head. Hebrank and Wright (1974) found the
elevation cue in a notch between about six and ten kilohertz whose frequency rises as the source
rises; Blauert's directional bands add a lift near 8 kHz that says "above". Elev Near and Elev Far
set the height of the two planes, and a voice takes its own from where it stands between them, so
the background can be the sky and the foreground the ground. The notch is the same one
externalisation uses -- the notch IS the elevation cue -- so a raised voice brings that path in on
its own:

    f_notch = 7000 + 2000 |lat| - 1800 [behind] + 3000 elev   Hz,     g_sky = 0.6 elev at 8 kHz

Measured on a bright thirty-two-partial note, as the energy near ten kilohertz against the energy
near seven: 0.509 with the source below the ear, 0.227 flat, 0.028 overhead. The notch has moved
from four kilohertz to ten, which is what the ear reads as a source climbing.

ENVELOPMENT

Spaciousness is two things, and a reverb's width is only one of them: the apparent width of the
source. The other is the sense of being inside the room, and Bradley and Soulodre (1995) found it
in late lateral energy at low frequencies, under about 500 Hz -- exactly where this instrument's
background has the least side, because the funnel narrows it and Bass Mono folds it. Envelop lifts
the far bus's side channel in the band between Bass Mono's corner and 500 Hz and leaves the mid
alone:

    S' = S + Envelop (LP_500 - LP_lo) S,    lo = max(125 Hz, Bass Mono)

It starts where Bass Mono ends on purpose. Anything lower would be lifted here and removed again at
the master, and a control that fights another control is two controls too many. Measured on the
finished output with a note placed wholly on the far plane: the side energy between 150 and 500 Hz
rises by 3.0 dB and the mid by 0.00.

THE DEPTH LAW

Heard distance grows with physical distance as roughly a power of a half -- Zahorik's pooled
exponent across the studies he reviewed is about 0.54 -- and the direct-to-reverberant ratio is the
cue the ear trusts for it, not the level. A plane that is linear in level and reverb therefore
spends most of its perceptual travel in its near half. Depth Law bends it back,

    d' = d^(1 + 0.85 Law)

which at 1 is d^1.85, the inverse of that exponent: the far half of the knob then sounds as far
again as the near half. The ends do not move. Measured: half depth is heard at 0.500 flat and at
0.277 under the law, and full depth at 1.000 either way.

THE MASTER, AND WHY THERE IS NO COMPRESSOR

Bass Mono high-passes the side channel with a second-order filter at about 150 Hz. This is a production rule rather than a perceptual limit, and the manual says so: in an anechoic room listeners localise low tones quite well by their interaural time differences, which work down to well under 100 Hz. In a listening room, where the wavelengths are longer than the room's dimensions and standing waves dominate, that resolution is gone, and a low end that differs between the channels buys almost no image while it cancels on a mono system and overloads a vinyl cutter. A mono low end under a wide picture is also what makes the picture feel anchored. Side Air lifts the side channel with a broad bell at 3 kHz (Q 0.6, up to +6 dB), the region where interaural level differences are largest and the directional bands of the pinna begin, so the width is heard rather than merely present. The mono guard measures side power against mid power over about a second and a half and eases the width back only if the side is half again the mid's power, by a quarter at most, at two per cent a second. That threshold cost a measurement: at side-louder-than-mid the guard engaged on thirty of thirty-seven reference presets, minutely, and a safety net that is always slightly on is a change to the sound.

There is no compressor anywhere, and that is a decision. The impression of an enormous room comes from the distance between the quietest texture and the loudest swell -- the loudness range -- and a limiter takes the finest amplitude movement out of a reverb tail and leaves it flat. Instead the instrument measures itself to ITU-R BS.1770-4 (2015), the standard EBU R 128 is built on. The signal is K-weighted -- a second-order high-shelf of about +4 dB above 1.5 kHz modelling the head's acoustic effect, then a second-order high pass near 38 Hz, the revised low-frequency B-weighting -- and the loudness of a block is

    L_K = -0.691 + 10 log10( sum_channels G_i * z_i )      z_i the mean square of the weighted channel)"
R"(

with 400 ms blocks overlapping by 75 %. The integrated value is gated twice, first absolutely at -70 LUFS and then relatively at 10 LU below the ungated mean, so that a piece that is mostly silence does not measure as mostly silence; the loudness range is the spread between the 10th and 95th percentiles of the short-term values above the gate; the true peak is estimated between the samples, and the crest factor is the peak above the RMS. The K-weighting is implemented from the analogue prototype rather than from the RBJ cookbook, because the cookbook shelf comes out about two per cent away from the coefficients the standard prints -- close enough to look right -- and the whole meter was checked against an independent implementation over the same render: -23.62 against -23.62 integrated. The window a dark-ambient master is asked to land in, -24 to -16 LUFS, is marked on the meter, with the -14 LUFS line the streaming services normalise to drawn across it.)" },
    { "Design IV: tuning, harmony and the conductor",
R"(Just intonation is not a period flavour in this instrument; it is the reason the voices can be as many and as slow as they are. This chapter is about why, and about the conductor that plays them.

WHY JUST INTONATION

Two tones in a simple ratio share partials: for the fifth 3:2, the upper tone's second harmonic 2 * (3/2) f = 3 f is the lower's third; for the major third 5:4, the upper's fourth is the lower's fifth. In equal temperament those partials are a few cents apart and beat -- the tempered fifth is 700 cents against the just 701.955, so the coinciding partials of a fifth on A3 beat at about three quarters of a hertz, and every other pair of partials beats at its own rate -- and a chord of six voices with thirty-two partials each is a field of slow beats that never resolves. In just intonation the shared partials coincide exactly and the beating vanishes, and the chord locks into a single complex tone the ear can rest inside for an hour. That is the roughness theory of consonance: Helmholtz (1877) proposed that dissonance is the roughness of beating partials, and Plomp and Levelt (1965) measured it -- two pure tones are most dissonant when they are about a quarter of a critical bandwidth apart, consonant when they coincide or are more than a critical bandwidth apart, and the dissonance of complex tones is the sum over all their pairs of partials. Sethares (2005) makes the same account the basis of matching a timbre to a tuning.

The research has moved on from there, and the manual should say how. Roughness turns out to be only part of what listeners call consonant: preferences track harmonicity -- how well a chord fits a single harmonic series, Terhardt's virtual pitch -- at least as strongly as they track the absence of beating (McDermott, Lehr and Oxenham 2010), the two are combined with familiarity in the current models (Harrison and Pearce 2020), and the preference itself is partly cultural: listeners with no exposure to Western harmony show no preference for consonant over dissonant chords at all (McDermott et al. 2016). For this instrument the practical consequence is small, because a just chord maximises harmonicity and minimises roughness at once -- the partials that coincide are the partials of one series. The consequence for the design was worth acting on: the conductor's consonance score is a number about the ratio, not about the spectrum actually sounding, and for a strongly inharmonic timbre Sethares shows that the consonant intervals move. So the conductor has a second ear, Timbre, which judges an interval by the roughness of the partials the two tones would actually make. It is Plomp and Levelt's curve for a pair of pure tones, summed over every pair of partials of the two tones (Sethares 1993):

    D(f1, f2) = sum_i sum_j a_i a_j ( exp(-3.5 s x) - exp(-5.75 s x) ),   x = |f2 r_j - f1 r_i|
    s = 0.24 / (0.021 min(f1 r_i, f2 r_j) + 19)

where r_h and a_h are the bank's own partial template -- its tilt, odd/even weight, brightness window and inharmonic stretch, the same numbers the voice renders with -- and the scaling s puts the roughness peak at about a quarter of a critical bandwidth in every register. The roughness less the tone's own is turned into a consonance on the ratio score's scale, exp(-2.3 D / D_semitone), so that a tone against itself is 1 and a semitone about a tenth, and Timbre blends it into the note weights; at 0 the conductor hears exactly as it always did. Measured: for a harmonic template the most consonant interval near the fifth is 3:2 to within a third of a per cent, and for the template of a stiff string (Inharmonic at 1) it is wider than 3:2 -- which is Sethares' result, and which means that with Timbre up the conductor of an inharmonic patch plays the intervals that patch is actually consonant at. The built-in scales are the just ones -- Ptolemy's major, a just minor, a seven-limit scale, the Pythagorean, a just pentatonic, the harmonic series 8 to 16 and the subharmonic 16 to 8, slendro, Bohlen-Pierce, an otonality 1-3-5-7-9-11 -- with 12-TET for comparison and any Scala file.

Purity is the blend between the two worlds, per note, in the log domain,

    f = f_ET^(1 - P) * f_JI^P        P the Purity knob

so at one the partials lock, at zero they beat like a piano, and in between the beats slow as the intervals close in on their ratios: E4 over a C root is 327.03 Hz pure and 329.63 Hz tempered, and 0.5 gives their geometric mean. Purity Drift lets P wander on a Drifter at a rate of one swing in about a hundred seconds, so the lock-in comes and goes -- harmony that breathes in and out of tune -- and sounding voices follow with a one-second glide, never a retrigger.

Stretch widens the octave. Listeners judge an octave as in tune when it is a little wider than 2:1 -- ten to twenty cents at the extremes of the range, the octave enlargement measured by Ward (1954) and explained by Terhardt from the pitch shifts of the partials -- and a piano is tuned that way, the Railsback curve (1938) being the measured result on real instruments, where the inharmonicity of the strings adds its own reason. The instrument does it as a slope about the reference pitch,

    log2 f' = log2 A4 + (1 + s / 1200) (log2 f - log2 A4)

so every octave above A4 is s cents wider and every octave below s cents narrower, the reference itself not moving, and the same slope in both directions keeps every interval within an octave nearly as it was -- a fifth a note above A4 is stretched by seven twelfths of s. It is applied after Purity and before the per-voice drift, so it composes with the just ratios rather than replacing them; a held note follows the change through the same glide the purity drift uses. At 0 it is the exact 2:1 of every preset that was ever saved. Measured: at twelve cents an octave up from A4 is 1212.00 cents, an octave down 1212.00, two octaves 2424. Tide leans the whole instrument's pitch by up to thirty cents on a minute-scale curve, and the sub follows, so the harmony stays while the pitch centre drifts the way an organ's does with the temperature of the room.

THE SCALE A TIMBRE ASKS FOR

The argument above runs one way: a harmonic spectrum makes simple ratios sound smooth. Sethares
(Tuning, Timbre, Spectrum, Scale, 2005) runs it the other. To a spectrum -- any spectrum -- there
belongs a set of intervals at which that spectrum, sounded against a transposed copy of itself, is
least rough. Play those and the partials line up; play others and they beat. Just intonation is
what that answer happens to be FOR A HARMONIC SPECTRUM. It is not a fact about numbers, and for an
inharmonic timbre it is as arbitrary as a gamelan's tuning would be on a piano.

An instrument that already computes its own spectrum and already has the roughness formula can
therefore compute its own scale, and the thirteenth tuning does. The roughness of the timbre against
itself is swept across the octave at three cents a step; where the curve dips, a parabola through
the three points around the dip gives its bottom to well under a cent; the least rough dips, up to
twelve of them, become the degrees. There is no table anywhere in that path.

Measured on the instrument's own spectrum at its plain harmonic setting, the degrees land on 5/4,
4/3, 3/2, 8/5, 5/3 and 7/4 within 1.7 cents, with 7/5 and 10/7 in the tritone. Turn Inharmonic to
the top and four of those six no longer have a degree within fifteen cents: the stiff string asks
for a different scale, and gets one. Change Tilt or Brightness or Inharmonic while it plays and the
tuning follows the sound, a few times a second -- sweeping the curve costs a third of a millisecond,
which is nothing now and then and far too much every block.

A spectrum with few partials, or only odd ones, has few minima and therefore few degrees. That is
the model's answer rather than a failure of it, and it is left standing.

CONSONANCE AS A NUMBER

The conductor needs consonance as a number, and the instrument's is

    C(p/q) = 1 / (1 + log2(p q))

for the simplest ratio p/q within ten cents of the interval (octave-reduced, q at most 32), so that the unison scores 1, the fifth 0.28, the major third 0.19, a semitone 0.11 and anything unrecognised 0.05. The quantity log2(p q) is Tenney's harmonic distance (1983), the height of a ratio in the harmonic lattice, and its reciprocal is the same ordering Euler's gradus suavitatis gives: the simpler the ratio, the more of its partials coincide, the more consonant the interval.

THE INSTRUMENT LISTENING TO ITSELF

Two mechanisms use the physics of intervals as a source of control. The Foundation's Difference mode plays the sub on the combination tone of the two lowest voices: two tones at f1 and f2 produce, by the quadratic nonlinearity of the ear, a tone at f2 - f1 (the difference tone Tartini described in 1754; Plomp 1965 and Moore 2012 for the modern account), and in just intonation that tone is itself harmonic -- a fifth gives f/2, a fourth f/3, a major third f/4. Rich reports these tones between 55 and 440 Hz carrying so much energy in his concerts that he tames them in mastering. Here the instrument plays the one the ear would make, folded into the sub's register so the register never changes, gliding as the sub always does. Measured: A3 and D4 (220 and 293.3 Hz, a fourth) put the sub at 146.7 Hz.

BEAT is a modulation source whose rate is the interval's mistuning. Two voices a fifth apart beat at the difference between the harmonics that would coincide if the fifth were just,

    beat = | q f2 - p f1 |      for the nearest simple ratio p/q)"
R"(

which is silent when the interval is in tune and quicker the further it has drifted; for a mistuned unison it is |f2 - f1|, the difference tone itself. BEAT finds the simplest ratio near the interval the two lowest voices make, from a short list of small-number ratios -- small numbers only, because those are the ones whose harmonics are close enough together to beat audibly -- and runs at exactly that rate, followed over about two seconds so that a voice arriving or leaving slides the rate rather than jumping it; below a fiftieth of a hertz it holds still, because a chord in tune should leave whatever it drives exactly where it is. Routed at a filter or at the Nebula's smear, the sound breathes at the rate of its own mistuning, and Purity Drift is what sets it moving. The test compares an octave (2:1 in every temperament there is: 0.000 Hz) with a tempered fifth (0.469 Hz at the test's pitch, two cents narrow), because the first version compared two tunings and measured the tuning system instead of the source.

THE CONDUCTOR

The Cluster Brain chooses notes from the scale, places them on the planes, holds them for minutes and lets them go. Its events come at exponentially distributed intervals around Event Rate, clamped between half a second and four times the rate. When a slot's hold time -- uniform between Hold Min and Hold Max -- expires, the note is released and the voice's long release does the fade; when Density is reached, an event either retires the note ending soonest or does nothing. Its choices are weighted, not random: every note n in the register not already sounding gets the weight

    w(n) = C(n / root)^(3 Consonance) * bell(register)

with octave doublings of sounding notes at 0.15, the root's pitch class at 3 when nothing sounds it and 0.25 when something does, and exact duplicate pitches excluded. At Consonance 1 the brain plays only fifths and octaves; at 0 the exponent vanishes and it plays clusters. Its root wanders, with probability 0.35 * Wander per event, to the note whose ratio to the old root is closest to 3/2, 4/3, 5/4, 6/5, 5/3 or 8/5 -- a modulation by consonant step, the way a slow improviser moves -- unless a held key pins it.

Autoplay is the conductor in another mode: it keeps the cluster full and exchanges one voice at a time, in free steps or in chords drawn from the scale, with a Tension that bounds how far a step may go. One voice at a time is the point, and the reason is auditory scene analysis (Bregman 1990): the ear follows a chord as a stream, and a stream survives one of its members changing while the others hold; change them all at once and the stream breaks and a new one begins, which is a cut rather than a movement. Brain 2 is a second conductor for the background alone, with its own register, pace, density and plane, on the first one's root plus an interval and with its own random stream, so the two planes stop moving in step and the picture gains a second layer of time.

Portamento with Gravity is the microtonal glide of a lap steel. A new key slides in from the last in the log domain over a chosen time, and the slide slows near the consonant ratios to the root with a strength

    s(d) = 1 / (1 + (d / 30)^2)        d the distance in cents to the nearest just ratio

half at thirty cents, eight per cent at a semitone, nothing between the nodes -- a Lorentzian, chosen because a magnet has almost no reach and then all of it. The first version braked in proportion to the consonance of whatever ratio the slide was passing through, a quantity that rises and falls smoothly across the whole glide, so the pull was everywhere and nowhere.

WHAT THE PAIRS CANNOT HEAR

The conductor's judgement was, for a long time, a mean over pairs: every candidate against the root
and against each sounding voice, averaged. For two tones that is the whole question. For five it is
not, and the difference is not academic. Measured on the instrument's own intervalConsonance, the
mean over pairs prefers a stack of fifths (4:6:9, 0.233) to a just major triad (4:5:6, 0.212), and
puts a plain segment of the harmonic series (8:9:10:11:12, 0.165) last of all -- because
neighbouring members of one series make complicated ratios two at a time (9/8, 11/10) however
perfectly the set fits together as a whole.

Harmonic is the second opinion. It asks how strongly the WHOLE sounding set implies one virtual
root -- Terhardt's virtual pitch, Parncutt's root support, and the property that listeners' stated
preferences track at least as strongly as they track the absence of beating (McDermott, Lehr and
Oxenham 2010). Every fundamental that could hold the set is tried, from the lowest tone divided by
one to sixteen; each tone is assigned to its nearest harmonic; and the score is how cleanly it sits
there, weighted so that a tone on a low harmonic supports the root far more than one high up:

    H = max over f0 of (1/N) sum over i of exp(-(cents_i / 25)^2) / log2(1 + n_i)

Two rules keep the trivial answers out, both of them put there because the first version handed
them over. Two tones may not share a harmonic number -- several tones crammed onto one harmonic is
a cluster, not a fit. And a root supported by fewer than two tones does not count at all, because
every tone is the first harmonic of itself; without that, a semitone cluster scored as high as a
just major triad and a bare tritone scored higher than both. With them: just triad 0.391, stacked
fifths 0.377, tempered triad 0.376, cluster 0.235, diminished seventh 0.208.

Getting that into the music took three changes rather than one, and the reasons are worth naming.
The free mode -- which is what nearly every preset uses -- weighs a candidate against the ROOT
alone, not against the chord, so without the term there the parameter did exactly nothing. A
weighted random draw is not an argmax, so the exponent had to be higher to shift the odds at all.
And which voice LEAVES is half the question: retiring by the clock gives back, one voice at a time,
whatever the arrivals gained. There was also a rule pulling openly against it. "An octave doubling
is not a new colour" is a matter of taste, while harmonicity says an octave is the strongest
relation two tones can have -- harmonics one and two of the same series. Harmonic settles that
argument now: at zero the old taste rule stands untouched, and as it rises the veto softens into a
preference. Measured over four chord sizes, the harmonicity of the chords the conductor actually
builds goes from 0.317 to 0.493, and at six voices three pitch classes remain: a rooted sonority
with octave doublings, which is a colour and not a general improvement.

A KEY, FOUND RATHER THAN SET

The conductor knew nothing of degrees. It asked how a candidate sounded against what was sounding,
which is a question about intervals, and never how it sat in a key. Krumhansl and Kessler (1982)
measured how stable each degree of a key feels -- a listener hears a context, then a probe tone,
and rates the fit -- and their two profiles, major and minor, are what Key weighs a candidate by.

Nothing sets the key. It is found, by the method of Krumhansl and Schmuckler: correlate the
distribution of what is sounding against all twenty-four rotated profiles and take the best. The
correlation is the confidence, and it does the weighting as well, so a passage with no key in it
pulls at nothing and an honest "this is barely a key" is available as an answer. The distribution
is weighted by HOW LONG each pitch class has been sounding, which on an instrument whose notes last
minutes is the only weighting that means anything, and it fades over about three minutes so the key
can travel when the music does.

The pitch class is taken from the frequency, not from the MIDI number, because a scale here need
not have twelve degrees: with consecutive degrees on a nine-tone scale, note modulo twelve means
nothing at all, while cents always mean cents.

Measured, on distributions whose answer is known: a C major scale comes back as C major (r 0.76),
the same seven notes with A and E held long come back as A minor (r 0.80), a full chromatic in
balance comes back as no key at all, and a five-semitone cluster as a weak one (r 0.37). On the
conductor itself, over three minutes, the confidence of the key it is in rises from 0.73 to 0.90
with nine pitch classes still in play -- a centre, not a restriction.

EVENNESS, AND THE DISTANCE A CHORD TRAVELS

Tymoczko (Science, 2006) showed that the chords which can be joined to their neighbours by small
voice movements are the nearly even ones, and that those are the chords Western music actually
uses. Evenness is therefore not a taste but the property that makes a chord able to MOVE; a cluster
can only leap. Even measures it as the spread of the gaps between the pitch classes: one for a
chord that divides the octave equally, zero for one whose notes are all in the same place. The
augmented triad and the diminished seventh measure 1.000, a major triad 0.875 -- nearly even, not
quite, which is exactly Tymoczko's point -- a cluster 0.250, three octaves of one note 0.000. On
the chords the conductor builds, Even takes the mean from 0.664 to 0.768; Harmonic takes it the
other way, to 0.445. The two are meant to be set against each other.

His other measure needed no building, and finding that out was worth more than building it.
Voice-leading distance is the smallest total movement over all ways of pairing two chords. When one
note is exchanged that sum is exactly the leap the one voice makes -- the displacements of the
others telescope away -- so the number that had been in the conductor since the beginning was
already the right one. Checked over four thousand random exchanges: agreement to the last bit. The
selftest keeps that on the record so that nobody improves it.

What was missing is which voice moves. The conductor retired whichever had been sounding longest, a
rule about time that knows nothing about where the chord would land. Smooth tries them all and
keeps the exchange that moves the chord least for what it gains: measured over a hundred and
forty-five exchanges, 3.12 semitones against 2.68.

In the free mode the same knob reads as something else, because there is no exchange to weigh: it
becomes the step size of a wandering voice. A candidate is weighted by how far it stands from the
note chosen before it, so the conductor steps oftener than it leaps -- at a half a neighbour is
about ten times as likely as a note an octave away -- while a leap stays possible, which is what
keeps a line from becoming a scale. At 0 neither meaning applies and the draw is what it always
was. Free mode is the one nearly every preset uses, and for four rounds this knob did nothing
there at all.

THE SPECTRUM A SCALE ASKS FOR

The timbre scale runs from spectrum to scale. Match runs the other way, and the pair is what
Milne, Sethares and Plamondon (2009) call dynamic tonality: choose a scale, and bend the spectrum
until that scale is the smooth one. Each partial is moved to the nearest degree, counted in periods
of the scale from the fundamental,

    n = floor(log_P h),   r_h = P^n * r_d,   d = argmin |log(r_d) - log(P^(log_P h - n))|

and Match blends the natural position and the matched one in the log domain. For twelve-tone equal
temperament the moves are small -- the third partial from 1902 cents to 1900, the fifth from 2786 to
2800, the seventh from 3369 to 3400 -- and after them a tempered chord no longer beats: the mean
roughness of the eleven tempered intervals falls from 0.1263 to 0.1191. For a just scale the third
and fifth partials are already on degrees and do not move at all, which is the check that the
direction is right. For Bohlen-Pierce, whose period is the tritave, the octave partial itself moves
onto the scale, to 1.960; that is the spectrum the tritave was always waiting for. The voice's
additive bank and the conductor's Timbre ear read the same table, so what the ear judges is what
the bank plays. With the Timbre scale selected Match is inert, and has to be: a scale computed from
the partials and partials computed from the scale would chase each other round a circle.

THE ARC, LEANING ON THE HARMONY

The hour-scale arc leaned on density, brightness and depth and left the harmony where the knobs
put it. Lerdahl and Krumhansl (2007) modelled tonal tension -- distance from the tonic in pitch
space plus surface dissonance -- and tested the model against listeners' ratings across whole
pieces. This instrument's proxies for those two quantities are the judgements the conductor already
makes, Harmonic and Key for the distance from home and Consonance for the dissonance, so Arc
Harmony lets the arc loosen all three on its rise and tighten them on its fall:

    tense (lean > 0):    x' = x (1 - 0.8 lean)
    relaxed (lean < 0):  x' = x + (1 - x)(0.6 |lean|)

continuous through zero. The climax of the night is then denser, brighter and further from home, and
the return is heard as a return. Measured over forty seconds at the shortest arc, the lean reaches
0.113; at zero it is exactly nothing.

HOW A CHORD ARRIVES

Rasch (1979) measured the onset asynchrony of performing ensembles at thirty to fifty milliseconds,
and showed that a little of it helps the ear tell the voices apart; Bregman's rule is the converse,
that tones starting together are heard as one object. The conductor brings its voices in one at a
time, minutes apart, so each is its own object by construction. Blend fills an empty chord with all
its notes at once, each chosen against the ones already committed and released inside a window of
thirty milliseconds at the top -- inside the fusion limit -- and fifty a little lower down. Measured
by driving the conductor directly: the first five onsets span 6.29 seconds one by one and 0.020 with
Blend.

THE FLUCTUATION GUARD

Fastl and Zwicker's two curves say where a beat is heard as what. The sensation of fluctuation
strength peaks at a modulation rate of 4 Hz and is gone by about 20; roughness takes over near 70.
Between two and eight hertz a beating chord is wobble, and in a sleep concert wobble is the one
thing it must not be. Purity Drift makes beats without knowing where they land, and the BEAT source
already measures where they landed, so the guard closes the loop:

    x = log2(f_beat / 4),   b = exp(-1.25 x^2),   drift' = drift (1 - Guard b)

reining the drift in while the beat sits in the band so that it slows back out, and below zero
doing the reverse and seeking the wobble out. It acts on the drift's excursion and not on Purity
itself: a Purity that already puts the beat in the band is left where it was put. Measured with a
tempered major third under a full drift: the beat spends 22 per cent of two minutes in the band
without the guard, 18 with it, 27 with it reversed. A modest effect, and the number is stated so
that nobody expects a larger one.

COHERENCE

Four slow oscillators coupled after Kuramoto (1984; Strogatz 2000 for the review) move brightness, depth, pan and the z-plane point. The Kuramoto model is the mathematics of fireflies falling into step, of pacemaker cells, of any population of rhythms that pull on each other:

    d theta_i / dt = omega_i + (K / N) sum_j w_ij sin(theta_j - theta_i)

with natural periods of 23, 31, 41 and 53 seconds over Rate -- primes, so their cycles share no common period -- and a coupling K of up to 0.6 rad/s times Rate, which is above the critical coupling for a spread of natural frequencies of 0.15 rad/s, so at full Coherence the bank locks. The coupling here is deliberately asymmetric,

    w_ij = 1 + 0.22 sin(2 pi (j - i) / 4)

each oscillator pulled a little harder by the one behind it in the ring than by the one in front. With symmetric coupling a high Coherence settled into exact synchrony -- all four phases equal, the order parameter

    r exp(i psi) = (1/N) sum_j exp(i theta_j)

at one -- and stayed there, four oscillators behaving as one, which is the opposite of what the section is for. An antisymmetric perturbation has no synchronous fixed point, so the bank locks in frequency and keeps a slowly turning spread of phase, which is what a ring of coupled biological oscillators does and why they never look identical. Measured with the order parameter after twenty minutes: above 0.9 when coupled, below without. Sympathy is coherence of another kind: a little of the whole foreground fed back into every voice through its own filter, one control block late, so the voices hear each other.)" },
    { "Design V: movement, modulation and the loop",
R"(A drone that does not move is a test tone. This chapter is about how the instrument keeps moving without ever repeating, and about the paths by which the sound feeds itself.

RATES THAT NEVER LINE UP

Two modulations whose periods share a simple ratio repeat their combination quickly: two LFOs at ten and twenty seconds recur every twenty. Two whose periods are incommensurable never do, and the least commensurable ratio there is -- the one whose continued fraction converges most slowly -- is the golden ratio phi = (1 + sqrt 5) / 2. So the eight LFOs default to rates on a golden ladder,

    f_n = 0.03 Hz * phi^n

the coherence ring's periods are primes, and the library generator draws its LFO rates on a golden ladder anchored on each preset's own base period, so no two presets share a set of rates either. The rates themselves are drone rates: log-uniform between one cycle in eight seconds and one in forty minutes in the library, so the median route takes a bit over two minutes to come round -- which means a short render shows little, and the instrument is built for half-hour pieces.

The Drifter, described in the first chapter, is the instrument's random LFO, and it is the reason random never means stepped here. Rate Wander applies a Drifter to the rates themselves, Breath to every voice's distance, Purity Drift to the tuning, Pos Drift to a wavetable's frame or a clip's read position, Shimmer to every partial's level; and every one of them runs on its own random stream, seeded from the voice's or from a side stream, so switching one on never moves another's dice.

ENVELOPES IN MINUTES

The voice's amplitude envelope is a plain ADSR with an attack of up to a minute and a release of up to two, exponential in every segment. The six modulation envelopes are drawn by hand, up to sixteen breakpoints with a curve on every segment, an optional sustain point and an optional loop. Their clock is a phrase clock: it restarts when a note arrives into silence, not on every note of a cluster, because a shape spanning a minute retriggered by every one of the brain's notes would never get anywhere. Each envelope has its own clock, and a Sustain Loop envelope's clock is placed exactly on the sustain point when the last voice lets go, so the tail plays from the value the hold ended on; the first version snapped there, by half the shape, which is the one thing this instrument is not allowed to do, and the self test now fails if the value moves by more than 0.02 across the release.

THE MATRIX

Every modulator drives any knob through one matrix. A route adds to its target

    target += v * depth * (max - min),     v in [-1, 1],   or  (v + 1) / 2 with the 0..1 flag

with an optional second source scaling the depth as an amount in 0..1, so a depth is a fraction of the target's own range and the same number means the same thing on a cutoff in hertz and on a mix in nought to one. The routes are data, not parameters: they travel in the preset as text, the way a Scala scale does, because thirty-two rows as four parameters each would put a hundred and twenty entries into a host's automation list for very little gain. Modulation is added after the Inertia glide -- a modulator moves at its own rate and is not slewed by the setting that exists to slow the performer's hand -- and a modulator's own rate or depth can itself be a target, read from the previous control block. That one block of delay, 64 samples or 1.3 ms at 48 kHz, is not a compromise; it is what a feedback path in a modulation matrix is, and the only way an LFO can move another LFO's rate without either needing the other's answer first. The nesting is the figure the ambient literature keeps coming back to: a thirteen-second sweep whose rate is itself moved by a nineteen-second one repeats only after 13 * 19 = 247 seconds.

The hands are sources too. Pressure and slide are read from the loudest voice, the wheel from the instrument, smoothed with a 30 ms time constant,

    w <- w + (1 - exp(-dt / 0.03)) (w_target - w)

so a seven-bit controller never steps a cutoff. All three rest at zero, and through the matrix's bipolar mapping zero is minus one, so a route wanting nothing until I move it carries the 0..1 flag -- which is what made it safe to put such routes into every preset in the library. The four Kuramoto oscillators, the note, its velocity and its distance, a random number drawn once per note, and BEAT complete the list.

THE CIRCLE

Rich's drones are not a chain but a circle: what comes out of the reverb goes back in front of the filter or into the oscillators. The instrument keeps the previous block's output mix, low-passed at Tone and driven through a gain-compensated saturation, and reads it back two ways. To Bus adds it to the near bus ahead of every effect; this path adds energy, so it is throttled by the mix's own mean level -- the feedback gain goes to zero as the 50 ms mean level reaches 0.1, about -20 dBFS -- so the loop hisses and holds instead of running away. The ceiling was measured: at 0.25 a preset climbed ten decibels and collapsed to a stereo correlation of 0.6, because a loop near unity gain circulates through the delay's cross-feed until both ears carry the same thing. To Pitch phase-modulates every partial of every voice by

    theta_h = h * theta,   theta = 3 * amount * fb(t) radians

which in the phasor bank is one extra small-angle rotation per partial, and this path is not throttled, because phase modulation redistributes energy among partials (the Bessel weights again) without adding any: a loud drone keeps its modulation. Tape in the loop adds an asymmetric saturation whose even-order term the DC blocker cleans up on the next pass, wow as an irregular Drifter of up to 3 ms and flutter at 6 Hz of 0.3 ms as a fractional read position, and a noise floor that rises with the loop's level -- modulation noise, which is what makes a floor sound like tape rather than like dither -- so every generation through the loop goes a little softer and less stable, like a forty-year-old tape.

The Cosmos is a second circle, in parallel, added and never replacing. Its frequency shifter is a single-sideband modulator: the input is split into a quadrature pair by an eight-section all-pass Hilbert approximation (Niemitalo 2003), multiplied by a quadrature oscillator at the shift frequency, and one sideband is kept, with 44 dB of rejection of the other; the right channel is shifted three per cent less than the left, so a 200 Hz shift beats at six hertz between the ears -- alien, and wide. Its resonators are two combs tuned to the brain's root times Res Pitch, the right 0.3 % longer, with feedback up to 0.97 and the output normalised by 1 - feedback. Its vowel filter is three band-passes on the formant tables, morphed on a Drifter. Its Nebula is a short-time Fourier transform (2048 points, hop 512, Hann) whose magnitudes are smoothed over time with the coefficient

    alpha = (1 - Smear)^2

and whose phases are random per frame -- Paulstretch pointed at the mix -- so at full Smear the spectrum freezes. And its Shimmer pitch-shifts the far reverb's output an interval up and feeds it back into the reverb's input, throttled by the reverb's own level so it blooms to a ceiling and holds there instead of running into the clipper. Blur is the Nebula pointed at the foreground: every attack wiped into texture, notes flowing into one another, the blurred part 43 ms late and the dry part not.

THE ROOM AS A THING)"
R"(

Four stages are about the room the sound is in rather than the sound. The Body is a soundboard: twelve two-pole resonators tuned to the brain's root in wood, plate, bell or string ratios, fed from the whole mix and returned into it, the lowest mode ringing for Decay and the higher ones dying faster at a rate belonging to the material. Two things had to be measured. A resonator normalised to unity at its peak passes almost nothing of a broadband signal -- the Body knob moved the mix by two hundredths of a decibel -- so the modes are normalised on their expected power, the way the Air band is; and modes scattered at random around the stereo centre are all driven by the same signal and so correlate the two channels (the width descriptor collapsed from 0.69 to 0.11), so neighbouring modes sit on opposite sides. Q is capped at 300, because a mode narrower than that is never excited by a drone that drifts. The Patina is the master's age -- wow and flutter as a moving read point, the top end a worn machine has lost, the modulation-noise floor and a gentle saturation -- because every one of those is a defect, and together they are most of what separates a recording from a render. The DC blocker, a first-order high pass at 4 Hz that costs 0.17 dB at 20 Hz, exists because several paths can leave an offset behind -- FM at an integer ratio, the tape stage's asymmetric term, a granular window over a clip that carries one -- and an offset costs headroom in the soft clipper without ever being heard; the blockers inside the FM slot and inside the feedback loop stay, because a loop can lock onto a DC operating point that a filter at the end cannot undo.)" },
    { "Design VI: the library, and how it was measured",
R"(Fourteen and a half thousand presets is not a number anyone can audition, and the design of the library follows from that: nothing in it is described, everything is measured.

STYLES AS RANGES

Each pack is a style: a set of ranges over the instrument's own parameters -- uniform, log-uniform, or a choice -- written in the spirit of an artist who works in one corner of the drone repertoire (nothing sampled from or affiliated with any of them), plus a list of which of the instrument's modules the style reaches for and how often. A preset is drawn from those ranges by a generator that also chooses its sources, its clip, its impulse, its matrix and its envelope shapes; half of each pack is drawn still, half astir, the astir half with more routes at 2.6 times the rate. The generator is deterministic on its seed, keyed on the style's index, so the same library can always be made again with the same names, which is what lets a pack name a sample that has not been rendered yet.

MEASURED, NOT DESCRIBED

Every preset, built in or generated, is rendered offline -- twelve seconds, a held chord and a busy brain -- and its descriptors are taken from the last eight seconds of the render: spectral centroid and flatness, spectral flux, stereo width, energy below 150 Hz, the mean voice count. Ranked across the library they become the browser's brightness, motion, width, noisiness, bass and density, the tag bits the columns filter on (quantiles of the ranks plus flags read from the parameters), and the position on the preset map: the first two principal components of the standardised descriptors, with the module flags weighted in so the Cosmos, the feedback and the source presets form clusters of their own, followed by a short repulsion pass so that no two points overlap. A point on the map is where a preset sounds, not where somebody put it. Since 1.11.0 it is a cloud rather than a field, and it is measured for drones: the twelve seconds became a minute, and three descriptors were added that say what a drone is like rather than what a note is like -- how far the sound travels over that minute (the spread of its per-second colour and level, which flux cannot see, since a fast tremolo has flux and goes nowhere), how rough its own partials are against each other on the Plomp-Levelt curve, and how much of what you hear comes back from the far planes rather than standing in the near one. The nine of them are laid out by springs on a nearest-neighbour graph instead of by rank, so presets that measure alike now actually gather and the library shows its own thin places; the cloud is then turned until dark is left and evolving is up. The whole library is embedded at once -- the built-ins and the packs used to be two projections sharing a square. Groups fall out of the same nine numbers by k-means, each named after what makes it itself, and the map is coloured by them unless you ask for the packs again. Four sliders narrow the cloud to a range of brightness, evolution, roughness or distance, and the view closes in on whatever survived. The same pass corrects every preset's master gain towards a common loudness window, which is what keeps a library of thousands from having a few dozen presets that jump out, and the browser's level matching trims the gain by at most 12 dB towards a common target when a preset is loaded, so that auditioning a hundred of them is not a ride on the volume knob.

The map's empty space is playable because the blend between presets is defined in the parameter domain. The six nearest points to the cursor get Gaussian weights of their distance,

    w_i = exp(-d_i^2 / (2 sigma^2)),   sigma = Radius, default 0.08 of the plane

floats are mixed in the skew domain of their knob, integers rounded, choices taken from the strongest neighbour, and every parameter glides towards the blend with the morph's time constant, 95 % after Glide seconds. On a point the blend is that preset to 0.1 %; between points it is a sound nobody saved.

NEUTRAL AT THE DEFAULT

A rule that shaped the last several rounds of the instrument's growth: every feature added after the first presets existed is neutral at its default, so nothing that was finished sounds different. The Vector's centre is exactly one; the far reverb's width is one; the fold, the Haas band, the unmask, the body, the patina, the tide and the rotation are zero; a new source type's index is appended so the indices stored by thousands of presets do not move; a new random source seeds from a side stream so switching it on never moves the brain's dice or a preset's random phases. A sound oracle -- forty presets rendered and hashed before and after every change to the core -- enforces it, and has caught the cases where it was violated by accident: a fourth random fork in the voice that advanced every stream by one draw and moved every random decision after it, and a sampling of the packs that shifted when a pack was added. Where the library was then changed on purpose -- the retrofit that gave every preset the mixing desk's features that suited it -- it was done from each preset's own settings, deterministic on the preset's name, rendered before and after on a sample (level median 0.00 dB, worst +3.4 dB, no clipping, no click), and re-measured afterwards.

THE SOURCE MATERIAL

The clips, wavetables and impulses the library plays are generated, and generated with the same discipline. The 5748 textures and the 2938 field recordings come from text-to-audio models pointed at instruments and at places; every field recording is made seamless by construction -- the last 1.5 s faded into the first with an equal-power crossfade, the overlap trimmed, the seam measured as the largest sample step across the wrap against the largest inside the clip -- and marked in its file name, which is what the Stretch type reads to skip its own crossfade. The wavetables are spectra generated from recipes or analysed from sound. The impulses are not only rooms: tuned partial banks that ring in key, inharmonic modal metal, reversed swells, combs and pipes, spectral bands with different decay times -- because a convolution reverb fed a drone is a resonator you can shape -- and forty struck objects cut from the field recordings. For those the sharpest event in a recording is found as the largest rise of the log energy envelope over four 5 ms hops, cut from 4 ms before it, and shaped by an exponential that reaches -60 dB at the end,

    h(t) = x(t0 + t) * exp(-6.9078 t / (0.9 T)),      T between 0.12 and 0.45 s

with a second channel a few milliseconds later. Convolution is multiplication of spectra, Y = X H, so it transfers the resonance of that object -- its poles and zeros, as measured by the world -- onto the synthetic wave, which no designed filter can fake; measured on the room stem, a pad through a struck object is five to fourteen decibels RMS different from the same pad through a hall across the third-octave bands, with peaks of up to 27 dB where the object rings.

WHAT THE LITERATURE SAID, AND WHAT WAS DONE

Two production papers on ambient and dark-ambient sound design were checked against the instrument late in its development, and most of what they ask for was already present: bass mono below the region of poor directional resolution, a side lift where directional hearing is sharpest, three reverb tiers with pre-delay and low cuts, air absorption with distance, free-running modulators on incommensurable rates, comb filters modulated by slow random curves, Paulstretch, a loudness meter and no limiter. Five things were not, and became parameters that are neutral at their default: the hands as modulation sources, the background's own width, the microshift, the band-limited Haas effect and the wavefolder. Two of the five were wrong in their first version and were caught by measurement rather than by ear -- the textbook microshift that was a comb filter on a sustained tone, and a Haas cross-feed that, fed a mono signal, produced no width at all -- and both are described honestly in the chapters above, because the instrument's manual is also its record.

WHERE THE RESEARCH HAS MOVED ON)"
R"(

The design leans on classic accounts, and the field has moved past several of them. Rather than list the gaps, the instrument was taken through them one at a time. Nine things were found; eight were built; each is neutral at its default, so the six thousand presets voiced before them sound to the bit exactly as they did -- the whole library was re-rendered and hashed after every one.

Consonance is harmonicity and culture as much as roughness. The conductor's judgement was a number about the ratio alone, and it now has a second ear that hears the spectrum as well (Timbre, in the previous chapter), plus a third that hears how crowded a critical band is getting (Spacing).

Octaves are not 2:1. Listeners set them wide, and pianos are tuned wider still (Ward 1954; Railsback 1938). Stretch does that to the whole scale, in cents per octave, compounding: f = f_ref * (f/f_ref)^(1 + s/1200).

Externalisation on headphones is driven first by reverberation and by the dynamic cue of head movement, not by static spectral detail (Best et al. 2020; Hendrickx et al. 2017). The head-tracked binaural mode answers that, and it now carries Brown and Duda's spherical head shadow as well.

Masking is not symmetric: a loud band masks upwards much further than downwards. The far reverb's Unmask now spreads that way, taking a band's own envelope plus half the band below, a quarter of the one below that, and a tenth of the one above.

Decorrelation by chorus is decorrelation by comb filter. Velvet noise gives the same width without the colouration, and the two were measured side by side to say so.

A feedback delay network's colouration is decided by its line lengths. Those were searched rather than chosen, and the Colourless mode is a third flatter than the classic set.

Early reflections are a separate problem from the tail, and the current answer is the scattering delay network. That is now the Early Room, and it is the one stage in the instrument whose whole purpose is that the sound moves when the source does.

Sound texture is recognised from time-averaged statistics of the auditory periphery (McDermott and Simoncelli 2011). That belongs offline rather than in the audio thread, and it is a tool in the library: measure a recording's statistics, impose them on noise, get an endless bed with the character of the original and none of its repetition.

Loudness is not energy. The meter now says sones beside LUFS, and on this instrument's own presets the two disagree by a quarter at the same LUFS.

Then the same was done for harmony, where the gaps turned out to be older and closer to home.

A chord is not its pairs. The conductor averaged consonance over every pair of voices, which for two tones is the whole question and for five inverts the ranking: measured on its own function, a stack of fifths beat a just major triad and a segment of the harmonic series came last. Harmonic asks instead how strongly the whole set implies one root.

A key is a thing a listener hears, and it can be measured rather than declared. Krumhansl and Kessler's profiles, correlated against what has actually been sounding and weighted by how long, tell the conductor which key it has drifted into; Key then decides how much it cares.

Chords that move smoothly are the nearly even ones (Tymoczko 2006), and that is now a control. His other measure -- the distance between two chords -- needed no work at all: for an exchange of one voice it is exactly the leap that voice makes, which the conductor had been using since the beginning without anybody knowing it was the right number.

And a scale is not a thing to choose but a thing a timbre asks for (Sethares). The thirteenth tuning computes the instrument's own dissonance curve while it plays and puts its minima where the degrees go: just intonation for a harmonic spectrum, to within two cents, and something quite else for a stiff string.

A third pass took four smaller ones. A scale can ask for a spectrum as well as the other way round (Match); the hour-scale arc can lean on the harmony as it already leans on density (Arc Harmony, after Lerdahl and Krumhansl's tension model); a chord can arrive as one object rather than as voices (Blend, after Rasch and Bregman); and a beat can be kept out of the band where it is heard as wobble (the guard, after Fastl and Zwicker). After those, the research that can make this instrument audibly better while staying readable is, in the author's judgement, largely used up, and the chapters say so rather than promising a fourth pass.

A last look at the acoustics found three more that were worth it: a vertical axis, from the pinna's elevation notch (Height); envelopment as distinct from width, from low-frequency lateral energy (Envelop); and a depth axis linear in what is heard rather than in what is computed (Depth Law, after Zahorik). Each is in Design III, with its numbers.

A sixth round came from outside: a survey of the state of the art, written against this manual, that proposed eleven things. Nine were built, each measured. The conductor's clock became a Hawkes process (Cascade: events that breed events, the gaps' coefficient of variation 0.91 to 1.41), it was given a surprisal budget (Surprise and Homeostat: the entropy of its interval choices held to a target, 3.52 bits down to 2.92 -- and the untouched conductor turned out to sit within two per cent of the maximum twelve pitch classes allow, so there is almost nowhere to go upwards), and its intonation became adaptive (Adaptive: a note tuned pure against what is sounding, a third at exactly 5:4, and a shared offset paying the comma back at three cents a minute so that every interval stays pure while the ensemble's centre returns home). The background learned to breathe as one (Comodulate: Hall, Haggard and Fernandes' masking release; the low and high bands' envelopes go from a correlation of 0.41 to 0.98), the far reverb gained a turning lossless matrix (Rotating: Schlecht and Habets' time-varying feedback, eight Givens rotations, every delay line standing still), and the head model a near field (Near Field: Brungart and Rabinowitz's low-frequency level difference, -5 to +10 dB below 400 Hz for a voice by the ear, the high shadow untouched). A Harmonic table morphs by optimal transport (Transport: the halfway energy 0.50 to 1.00, the spread five partials to none), the Foundation pulses at its binaural rate (Pulse), the feedback shaper's operating point follows the bass (Bias), and a Lenia field runs in the background as four sources (599 steps in thirty seconds, one reseed, no move larger than a per cent per block). And a knob from the third round, Blend, was found never to have reached the conductor: its test had driven the conductor directly and passed. It is wired now, and tested through the engine.

An eighth round came from a third report, on the interface, and its diagnosis was mostly right: eleven rows of tabs on one page split attention; the stage was a picture nobody could touch; a swing of four hours is invisible on a knob; the loop that feeds the sources closed out of sight; and the one-strand banks in Sources 2 to 4 were greyed rather than explained. Its therapy -- a rebuilt interface in three zones, an isometric stage with voices to grab, a three-dimensional filter cube, a second preset explorer -- was declined: weeks of work and every working thing at risk, and two of the four wrong for a generative instrument, whose voices are where the conductor put them. Eight smaller things were built instead. A knob now shows everything that moves it: the matrix as before, the morph or the map's blend as a neutral arc to the live value, and the arc and the tide as a dot on the outer ring saying where in their slow swing they stand. Three layouts under one button: Normal, Compact, and Expanded, where every page of every tab row lies under the next with a title in the tab's place, for a tall screen or for reading a preset through. The Tuning page draws the timbre's own roughness curve across the octave with the scale's degrees on it, the key the conductor has found, the comma and the tide; the Coherence page draws the Kuramoto ring, the Lenia field and the attractors. The stage's three planes can be dragged. A glyph marks where the feedback loop closes, a sentence says why the second bank is one strand, and the figures are fixed-width, so a moving value does not shiver. The reply to that reply added four: the header names the conductor's key and glows with the cascade's excitation; the notes roll shows the deja-vu ring, the cascade's glow and the homeostat's needle; the attractors draw their orbits; and with Arc Clock on the Arc knob wears a dial of the day, marks at four, ten, sixteen and twenty-two. What it asked for beyond that was declined again, with a reason each: tying the expanded layout to a pixel height would reflow the page while the corner is only zooming it, and the three-zone dock, drawer and soundstage are the modulation strip, its tabs and the Perform page under other names.

A last critique caught what the expanded layout got wrong, and the answer to it is the rule the panel now follows: the layout is a function of the parameters, never of the window. Expanded had been laid out tall, four to five, and a page that scales as a whole lands at forty-one per cent on a screen of a thousand and eighty lines. The height was the greyed knobs: every source strip was the union of every type's cells, and slots on Off, filters on Off, morphs and conductors not active stood fully open. So a strip now holds only the cells its type uses, a section whose switch is off closes to its title and the switch, and Expanded is three columns -- the voice; the room, the effects and the spectrum; morph and the conductor. Measured: Expanded at four fifths on that screen, from two fifths; Normal at eighty-six per cent. Then Expanded was given a page of its own, no tabs, the shaping sections in the middle column and the closed sections side by side in one row that wraps only when they open, and it came out at 2.4 to 1 -- larger than life on that screen; and once the rows without a display had learned to flow into one another, so that two Off slots share a line and the closed sections stand beside the open ones, at 2.5 to 1, where the screen's width and not its height is what limits it. Choose a type or throw a switch and the page rearranges, on that action and no other; the manual's pictures are taken with every section open.

The tabbed page had the same disease from the other side, and a picture of it said so: a row with tabs was as tall as its tallest page and as wide as its widest, so three clock cells stood in a box three rows high, a closed Cosmos in one two rows high, the macros in a band with nothing beside them, and the strand bank under Source 1's picture left a row of nothing under Source 1's knobs. The tabbed page is now fitted. Every page gets the fewest rows at which its sections, each as narrow as that allows, stand side by side in the column's width, so they come out the same height and nothing is under a short one; what the row has over goes to the display, or, where there is none, a cell at a time to the sections that can take it without losing a row, because a last row that is not full reads as a section and a band at the end of the row reads as a hole. A row is as tall as the page that is open on it, not as its tallest page, while the window keeps the shape of the tallest pages, and the last group of each column grows into the difference with its page refitted taller. And the pages were cut so that each fills its row and every page that can have a picture has one: the strand bank beside Source 1, the two filter pages sharing the filter's response, the envelope and the expression sharing the envelope's picture, morph, macros and the vector in one row with the vector's square, the coherence and the clock on one page; a closed page keeps its picture, which says why it is empty. Compact is no longer a rule about wrapping but a width -- the columns at eighty-eight per cent of Normal's, the fit doing the rest. Normal came out at 1.7 to 1 and the same size on a 1080-line screen as before; Compact at 1.3 to 1, from 1.5. And where a row still had room left, the answer was not to leave it empty: a section closes to save the page room, so where the room is there anyway it is laid out open instead -- a title beside an empty band is worse than the section it is hiding. Each closed section is opened in turn if the page still fits its column at the same number of rows, or, in the expanded layout, if it fits on the line as it stands. The switch is untouched; what you see is the section a switch away from sounding.

A seventh round came from a second survey, this one of the instruments rather than the literature: Osmose and the EaganMatrix, SOMA's Terra, Waldorf's Iridium, Madrona's Sumu, Borderlands, Surge, Mutable's Marbles, RAVE and FluCoMa, Ambisonics. Six things were taken from it, each measured. The conductor got Marbles' deja-vu ring (a figure of four comes round again 100 per cent of the time at full, 29 without) and its Spread and Bias on the shape of its draws (at full Spread most velocities sit at the extremes; Bias moves the mean from 0.70 to 0.80). Two strange attractors run in the background as six sources, on a time scale of minutes: the Lorenz system, whose best self-match at any lag over two minutes is 0.40, and the Roessler system, whose is 0.95 -- nearly a cycle with a chaotic climb, and the help says so. Pressure, slide and bend can be smoothed by the one-euro filter of Casiez, Roussel and Vogel, whose cutoff follows the speed, so a held note loses the sensor's jitter that a fixed thirty milliseconds lets through, and a press is still followed at once. A pure fourth, fifth or octave can be put on everything that sounds (Terra's interval keys), gliding at an octave in two seconds and landing exactly on the ratio. And the additive bank's partials can be spread across the field one by one (Sumu), the pattern turning once a minute, at equal power, so that a single strand decorrelates between the ears with its energy unchanged.

Several things that survey listed as missing were already here under other names: bandwidth-enhanced partials (the Spectral source's bands are sine plus noise, which is what Loris' are), MPE, Scala and keyboard mappings, gesture recording (the Sets), a two-dimensional playing surface (the map), per-voice microfluctuation, inertia on modulation. And several were declined: a scripting language in the audio thread, a sixteen-channel Ambisonics bus for an instrument that is heard on headphones, corpus navigation as a project of its own, and the same neural and port-Hamiltonian proposals as before.

Two of the eleven were not built. The port-Hamiltonian reformulation of the physical models is a formalism for guaranteeing passivity, not a sound: the bow and the waveguides are passive by construction, and rewriting them would change every sample for no measurable gain. And a biosignal loop -- the pulse rate following the listener's heart -- needs hardware this instrument does not have.

The ninth was a neural sound model in the audio path -- an autoencoder trained on a recording, played as an instrument, in the manner of RAVE. It was considered and deliberately not built: it would put a hundred megabytes of weights and a hard real-time constraint into a synthesiser whose core is framework-free and meant to run on a headset, and it would make the instrument's sound something nobody could read. Every other decision in these chapters can be checked by reading a formula. That one could not.

WHAT IS NOT CLAIMED

Some things this instrument does not do, and does not pretend to. It has no compressor, no limiter and no dithering, and does not intend to; mastering is a separate craft with its own tools. The four-pole ladder filter is not zero-delay and is not going to become so in place. The Quest application builds against the same core and has not yet been run on a headset. The loudness in sones is Zwicker's model built from third-octave levels, not a certified ISO 532-1 implementation, and its absolute scale is pinned at the definition of the unit rather than derived; what is not pinned, and what should be checked against, is that sixty decibels comes out at 4.21 sones where the standard says four. The Early Room places the whole near bus at one position, the level-weighted mean of where its voices are, not each voice at its own: six delay lines per voice would cost more than the rest of the instrument. The Spectral source's model is thirty-two bands and one partial per band, so a dense chord in one band comes back as its loudest member plus noise. The key profiles of Krumhansl and Kessler were measured on listeners raised on Western tonal music, and are quoted as what they are: a measurement of those listeners, not a fact about music -- the same caution the consonance section carries. Smooth means two different things in the two modes -- the voice-leading distance of an exchange in Chords, the step size of a wandering voice in Free -- and in Free mode it still says nothing about WHICH voice leaves: that is decided by the hold times and by nothing else. And the timbre scale is computed from the spectrum of the additive bank alone, so a patch whose sound comes mostly from a sample or a bowed string is being tuned for a timbre it is not really playing. Match, likewise, moves the partials of Source 1's additive bank and nothing else. Arc Harmony's mapping from tension to three knobs is a proxy for Lerdahl and Krumhansl's model, not the model. And the fluctuation guard's measured effect is four percentage points of time, which is honest and small. Height is two filters, not a head: it tells the ear up from down and nothing finer, and it is heard through headphones or a close pair of speakers, not through a room. Envelop's measured lift on the output is three decibels, not the six the band is given, because the far reverb's low end had little side to lift. The psychoacoustics quoted in these chapters is the standard account -- the duplex theory, roughness as the basis of consonance, the structural model of the head, auditory scene analysis -- rather than the frontier of the field, and it is quoted because it is what the design used, not as a claim to have tested it. The seventh round's: the Roessler attractor is nearly periodic and is offered as such; the one-euro filter reads its speed from the distance left to travel rather than from the input's derivative, which is a variant and not the paper's filter, because controllers send steps; the deja-vu ring holds notes, not the timing between them, so it brings a figure back and not its rhythm; and Partial Spread applies to the additive bank alone and only when feedback FM is off, which is the path that was vectorised and left alone. The sixth round's own caveats: the rotating reverb matrix does not beat the classic network's wobbling lines at re-mixing the modes -- it matches them (0.51 against 0.55 second to second) with every line still, and that is the whole of what it can claim; the Lenia field is thirty-two cells wide and breeds blobs, not the gliders of the literature; Pulse builds the modulation that drives the auditory steady-state response, not the entrainment its advocates claim for it, on which the systematic reviews are unpersuaded; Bias is the cheapest form of a reactive nonlinearity and not a wave-digital circuit; Adaptive intonation matches to a list of twelve ratios and caps at thirty cents, which is a rule and not a graph optimisation; and the Homeostat's Surprise scale is set so that the untouched conductor reads near its top, because it was measured there. The numbers that are claimed are the ones the instrument measured on itself.)" },
    { "Design VII: references",
R"(The works the design chapters rest on, by area. Where a page or a chapter is named it is the part that was used.

PSYCHOACOUSTICS AND SPATIAL HEARING

Rayleigh, Lord (J. W. Strutt): On our perception of sound direction. Philosophical Magazine 13, 214-232, 1907. The duplex theory: time differences at low frequencies, level differences at high.

Blauert, J.: Spatial Hearing. The Psychophysics of Human Sound Localization. Revised edition, MIT Press, 1997. Interaural time and level differences, Woodworth's head model, the directional bands, the precedence effect, spaciousness and interaural cross-correlation.

Wightman, F. L. and Kistler, D. J.: The dominant role of low-frequency interaural time differences in sound localization. Journal of the Acoustical Society of America 91, 1648-1661, 1992.

Brown, C. P. and Duda, R. O.: A structural model for binaural sound synthesis. IEEE Transactions on Speech and Audio Processing 6 (5), 476-488, 1998. The head shadow, the pinna notches and the shoulder reflection as simple filters -- the source of the Externalise stage.

Wallach, H., Newman, E. B. and Rosenzweig, M. R.: The precedence effect in sound localization. American Journal of Psychology 62, 315-336, 1949.

Haas, H.: Ueber den Einfluss eines Einfachechos auf die Hoersamkeit von Sprache. Acustica 1, 49-58, 1951 (English: The influence of a single echo on the audibility of speech, Journal of the Audio Engineering Society 20, 146-159, 1972).

Bronkhorst, A. W. and Houtgast, T.: Auditory distance perception in rooms. Nature 397, 517-520, 1999. The direct-to-reverberant ratio as the distance cue.

Macpherson, E. A. and Middlebrooks, J. C.: Listener weighting of cues for lateral angle: the duplex theory of sound localization revisited. Journal of the Acoustical Society of America 111, 2219-2236, 2002.

Best, V., Baumgartner, R., Lavandier, M., Majdak, P. and Kopco, N.: Sound externalization: a review of recent research. Trends in Hearing 24, 2020. Reverberation, spectral detail and head movement as the cues that put a headphone image outside the head.

Hendrickx, E., Stitt, P., Messonnier, J.-C., Lyzwa, J.-M., Katz, B. F. G. and de Boishéraud, C.: Influence of head tracking on the externalization of speech stimuli for non-individualized binaural synthesis. Journal of the Acoustical Society of America 141, 2011-2023, 2017.

Scharf, B.: Loudness adaptation. In Tobias, J. V. and Schubert, E. D. (eds.): Hearing Research and Theory, volume 2, Academic Press, 1983. Adaptation is small for steady tones at moderate levels.

Eramudugolla, R., Irvine, D. R. F., McAnally, K. I., Martin, R. L. and Mattingley, J. B.: Directed attention eliminates change deafness in complex auditory scenes. Current Biology 15, 1108-1113, 2005.

Zahorik, P., Brungart, D. S. and Bronkhorst, A. W.: Auditory distance perception in humans: a summary of past and present research. Acta Acustica united with Acustica 91, 409-420, 2005. Intensity, direct-to-reverberant ratio and spectrum as distance cues; the compressive power law between physical and perceived distance, exponent about 0.54, that Depth Law inverts.

Hebrank, J. and Wright, D.: Spectral cues used in the localization of sound sources on the median plane. Journal of the Acoustical Society of America 56, 1829-1834, 1974. The pinna notch that rises with elevation: the height of the two planes.

Bradley, J. S. and Soulodre, G. A.: Objective measures of listener envelopment. Journal of the Acoustical Society of America 98, 2590-2597, 1995. Envelopment as late lateral energy at low frequencies, distinct from apparent source width: Envelop.

Hall, J. W., Haggard, M. P. and Fernandes, M. A.: Detection in noise by spectro-temporal pattern analysis. Journal of the Acoustical Society of America 76, 50-56, 1984. Comodulation masking release: a signal in a noise whose bands share one envelope is detected ten to fifteen decibels further down. Comodulate.

Brungart, D. S. and Rabinowitz, W. M.: Auditory localization of nearby sources. Head-related transfer functions. Journal of the Acoustical Society of America 106, 1465-1479, 1999. Within a metre the interaural level difference grows at all frequencies and most at low ones, to twenty decibels and more. Near Field.

Picton, T. W., John, M. S., Dimitrijevic, A. and Purcell, D.: Human auditory steady-state responses. International Journal of Audiology 42, 177-219, 2003. The cortical response to amplitude modulation, which is what Pulse drives.

Ingendoh, R. M., Posny, E. S. and Heine, A.: Binaural beats to entrain the brain? A systematic review of the effects of binaural beat stimulation on brain oscillations. PLoS ONE 18, e0286023, 2023. The evidence for entrainment by binaural beats is inconsistent; quoted as the reason the manual claims the modulation and not the effect.

Casiez, G., Roussel, N. and Vogel, D.: 1 euro filter: a simple speed-based low-pass filter for noisy input in interactive systems. Proceedings of CHI 2012, 2527-2530. The adaptive smoothing of pressure, slide and bend (MPE Filter: One Euro).

Zwicker, E. and Fastl, H.: Psychoacoustics. Facts and Models. Second edition, Springer, 1999. Masking, critical bands, loudness and its adaptation; the specific-loudness law and the slopes of the excitation pattern that the sone meter is built from.

ISO 532-1:2017: Acoustics -- Methods for calculating loudness -- Part 1: Zwicker method. The standardised form of that model.

Glasberg, B. R. and Moore, B. C. J.: Derivation of auditory filter shapes from notched-noise data. Hearing Research 47, 103-138, 1990. The equivalent rectangular bandwidth: ERB(f) = 24.7 (0.00437 f + 1), the scale the Spectral source's bands, the conductor's Spacing and the texture tool all sit on.

Terhardt, E.: Calculating virtual pitch. Hearing Research 1, 155-182, 1979. Also the approximation of the absolute threshold of hearing, and the level-dependent upper slope of a masking pattern -- 22 + min(230/f, 10) - 0.2 L dB per Bark, with f in hertz.

McDermott, J. H. and Simoncelli, E. P.: Sound texture perception via statistics of the auditory periphery: evidence from sound synthesis. Neuron 71, 926-940, 2011. Texture is recognised from time-averaged statistics of subband envelopes; the basis of Tools/library/texture_statistics.py.

McDermott, J. H., Schemitsch, M. and Simoncelli, E. P.: Summary statistics in auditory perception. Nature Neuroscience 16, 493-498, 2013.

Moore, B. C. J.: An Introduction to the Psychology of Hearing. Sixth edition, Brill, 2012. Combination tones, pitch and the general account.

Bregman, A. S.: Auditory Scene Analysis. The Perceptual Organization of Sound. MIT Press, 1990. Streams, onsets and why one voice changes at a time.

ISO 226:2003: Acoustics -- Normal equal-loudness-level contours. The grey noise colour.

ISO 9613-1:1993: Acoustics -- Attenuation of sound during propagation outdoors -- Part 1: Calculation of the absorption of sound by the atmosphere. Air absorption as a function of frequency.

CONSONANCE AND TUNING

Sethares, W. A.: Adaptive tunings for musical scales. Journal of the Acoustical Society of America 96, 10-18, 1994. Retuning each note as it arrives to minimise the dissonance of the sounding set, with the drift that follows and the ways of holding it: Adaptive.

Helmholtz, H. von: On the Sensations of Tone as a Physiological Basis for the Theory of Music. Fourth German edition 1877, translated by A. J. Ellis, 1885 (Dover reprint 1954). Beating partials as the basis of dissonance; combination tones.

Plomp, R. and Levelt, W. J. M.: Tonal consonance and critical bandwidth. Journal of the Acoustical Society of America 38, 548-560, 1965. Roughness as a function of frequency difference in critical bandwidths; consonance of complex tones as a sum over partial pairs.

Plomp, R.: Detectability threshold for combination tones. Journal of the Acoustical Society of America 37, 1110-1123, 1965.

Sethares, W. A.: Local consonance and the relationship between timbre and scale. Journal of the Acoustical Society of America 94, 1218-1228, 1993. The roughness of two complex tones as a sum over their partial pairs, with the parametrised Plomp-Levelt curve the conductor's Timbre uses.

Sethares, W. A.: Tuning, Timbre, Spectrum, Scale. Second edition, Springer, 2005. The roughness account applied to scales and timbres; just intonation and the partials that coincide.

Terhardt, E.: Pitch, consonance, and harmony. Journal of the Acoustical Society of America 55, 1061-1069, 1974; and Calculating virtual pitch. Hearing Research 1, 155-182, 1979. Harmonicity and virtual pitch as a basis of consonance; the octave enlargement.

Ward, W. D.: Subjective musical pitch. Journal of the Acoustical Society of America 26, 369-380, 1954. The stretched octave: listeners set an octave a little wider than 2:1.

Railsback, O. L.: Scale temperament as applied to piano tuning. Journal of the Acoustical Society of America 9, 274, 1938. The measured stretch of tuned pianos.

McDermott, J. H., Lehr, A. J. and Oxenham, A. J.: Individual differences reveal the basis of consonance. Current Biology 20, 1035-1041, 2010. Consonance preference tracks harmonicity rather than the absence of beating.

McDermott, J. H., Schultz, A. F., Undurraga, E. A. and Godoy, R. A.: Indifference to dissonance in native Amazonians reveals cultural variation in music perception. Nature 535, 547-550, 2016.

Harrison, P. M. C. and Pearce, M. T.: Simultaneous consonance in music perception and composition. Psychological Review 127, 216-244, 2020. A composite model: harmonicity, interference and familiarity.

Tenney, J.: John Cage and the Theory of Harmony. 1983 (in Soundings 13, 1984). Harmonic distance log2(p q), the measure the consonance score is built on.

Partch, H.: Genesis of a Music. Second edition, Da Capo, 1974. Otonality and utonality, the ratios of the built-in scales.

Bohlen, H.: 13 Tonstufen in der Duodezime. Acustica 39, 76-86, 1978; Mathews, M. V., Pierce, J. R., Reeves, A. and Roberts, L. A.: Theoretical and experimental explorations of the Bohlen-Pierce scale. Journal of the Acoustical Society of America 84, 1214-1222, 1988.

Krumhansl, C. L. and Kessler, E. J.: Tracing the dynamic changes in perceived tonal organization in a spatial representation of musical keys. Psychological Review 89, 334-368, 1982. The probe-tone profiles: how stable each degree of a key is measured to feel, and the basis of the key-finding correlation of Krumhansl and Schmuckler.

Krumhansl, C. L.: Cognitive Foundations of Musical Pitch. Oxford University Press, 1990. The tonal hierarchy, and the key-finding algorithm in full.

Parncutt, R.: Harmony: A Psychoacoustical Approach. Springer, 1989. Root support and pitch salience: how strongly a set of tones implies one root.

Tymoczko, D.: The geometry of musical chords. Science 313, 72-74, 2006; and A Geometry of Music. Oxford University Press, 2011. Voice-leading distance as a distance in an orbifold, and the result that the chords which move smoothly are the nearly even ones.

Lerdahl, F. and Krumhansl, C. L.: Modeling tonal tension. Music Perception 24, 329-366, 2007. Tension as distance in tonal pitch space plus surface dissonance, tested against listeners' continuous ratings; the basis of Arc Harmony.

Milne, A., Sethares, W. A. and Plamondon, J.: Tuning continua and keyboard layouts. Journal of Mathematics and Music 2, 1-19, 2008; and Sethares, W. A., Milne, A., Tiedje, S., Prechtl, A. and Plamondon, J.: Spectral tools for dynamic tonality and audio morphing. Computer Music Journal 33, 71-84, 2009. Matching a spectrum to a scale as well as a scale to a spectrum: Match.

Rasch, R. A.: Synchronization in performed ensemble music. Acustica 43, 121-131, 1979. Onset asynchrony of thirty to fifty milliseconds in real ensembles, and its effect on hearing the voices apart: Blend.

Fastl, H. and Zwicker, E.: Psychoacoustics. Facts and Models. Third edition, Springer, 2007, chapters 10 and 11. Fluctuation strength peaking at 4 Hz and roughness near 70: the fluctuation guard.

Kuramoto, Y.: Chemical Oscillations, Waves, and Turbulence. Springer, 1984. The coupled-oscillator model behind the Coherence section.

Strogatz, S. H.: From Kuramoto to Crawford: exploring the onset of synchronization in populations of coupled oscillators. Physica D 143, 1-20, 2000. The order parameter and the critical coupling.
)"
R"(
SYNTHESIS AND SIGNAL PROCESSING

Schlecht, S. J. and Habets, E. A. P.: Time-varying feedback matrices in feedback delay networks and their application in artificial reverberation. Journal of the Acoustical Society of America 138, 1389-1398, 2015. Orthogonal matrices parametrised by rotations whose angles move slowly: the loop stays lossless while its modes are re-mixed, with no delay line modulated. The Rotating mode.

Roma, G., Green, O. and Tremblay, P. A.: Audio morphing using matrix decomposition and optimal transport. Proceedings of DAFx 2020. Spectral morphing as mass transport along the frequency axis rather than as crossfading of bins: Transport.

Chowdhury, J.: Wave digital filter circuit models with R-type adaptors (Medium, 2020) and the chowdsp_wdf library. The reactive nonlinearities whose operating-point drift Bias imitates in its cheapest form.

Lorenz, E. N.: Deterministic nonperiodic flow. Journal of the Atmospheric Sciences 20, 130-141, 1963. And Roessler, O. E.: An equation for continuous chaos. Physics Letters A 57, 397-398, 1976. The two attractors the matrix reads as lorenz_x/y/z and rossler_x/y/z.

Fitz, K. and Haken, L.: On the use of time-frequency reassignment in additive sound modeling. Journal of the Audio Engineering Society 50, 879-893, 2002. Bandwidth-enhanced partials (Loris): the sine-plus-noise partial the Spectral source's bands already are, and the additive model Madrona Labs' Sumu builds on. Sumu's per-partial placement is the model for Partial Spread.

Gillet, E.: Mutable Instruments Marbles, user manual, 2018. The deja-vu loop and the shaping of a random draw by spread and bias: Deja Vu, Loop, Spread and Bias in the conductor.

Chan, B. W.-C.: Lenia -- biology of artificial life. Complex Systems 28, 251-286, 2019. Continuous cellular automata with ring kernels and smooth growth: the Lenia field.

Fletcher, N. H. and Rossing, T. D.: The Physics of Musical Instruments. Second edition, Springer, 1998. The stiff string's inharmonicity, the mode series of bars, membranes, plates and bells that the z-plane shapes and the Body are built from.

Chowning, J. M.: The synthesis of complex audio spectra by means of frequency modulation. Journal of the Audio Engineering Society 21 (7), 526-534, 1973.

Le Brun, M.: Digital waveshaping synthesis. Journal of the Audio Engineering Society 27 (4), 250-266, 1979. Waveshaping and its harmonic series -- the wavefolder.

Karplus, K. and Strong, A.: Digital synthesis of plucked-string and drum timbres. Computer Music Journal 7 (2), 43-55, 1983; Jaffe, D. A. and Smith, J. O.: Extensions of the Karplus-Strong plucked-string algorithm. Computer Music Journal 7 (2), 56-69, 1983. The Strike.

Roads, C.: Microsound. MIT Press, 2001. Granular synthesis.

McIntyre, M. E., Schumacher, R. T. and Woodhouse, J.: On the oscillations of musical instruments. Journal of the Acoustical Society of America 74, 1325-1345, 1983. The nonlinear friction characteristic of the bow, and why it has to fall with slip velocity for the string to oscillate at all.

Serra, X. and Smith, J. O.: Spectral modeling synthesis: a sound analysis/synthesis system based on a deterministic plus stochastic decomposition. Computer Music Journal 14 (4), 12-24, 1990. Sound as partials plus a residual noise -- the Spectral source, taken band by band rather than partial by partial.

Heeger, D. J. and Bergen, J. R.: Pyramid-based texture analysis/synthesis. SIGGRAPH 1995, 229-238; Portilla, J. and Simoncelli, E. P.: A parametric texture model based on joint statistics of complex wavelet coefficients. International Journal of Computer Vision 40, 49-71, 2000. Synthesis by alternating projection onto a set of statistics -- the loop the texture tool runs.

Karjalainen, M. and Jaerveläinen, H.: Reverberation modeling using velvet noise. AES 30th International Conference, 2007; Vaelimaeki, V., Parker, J. D., Savioja, L., Smith, J. O. and Abel, J. S.: Fifty years of artificial reverberation. IEEE Transactions on Audio, Speech, and Language Processing 20, 1421-1448, 2012. Sparse signed impulses that decorrelate without colouring: the Ensemble's Velvet mode.

De Sena, E., Hacihabiboglu, H., Cvetkovic, Z. and Smith, J. O.: Efficient synthesis of room acoustics via scattering delay networks. IEEE/ACM Transactions on Audio, Speech, and Language Processing 23, 1478-1492, 2015. One node per wall, real distances between them, isotropic scattering: the Early Room.

Schlecht, S. J. and Habets, E. A. P.: On lossless feedback delay networks. IEEE Transactions on Signal Processing 65, 1554-1564, 2017. Where the colouration of a delay network comes from, and why its line lengths decide it.

Engel, J., Hantrakul, L., Gu, C. and Roberts, A.: DDSP: differentiable digital signal processing. International Conference on Learning Representations, 2020; Caillon, A. and Esling, P.: RAVE: a variational autoencoder for fast and high-quality neural audio synthesis. arXiv:2111.05011, 2021. Read and deliberately not built -- see the end of the previous chapter.)"
R"(

Nasca, P. (Nasca Octavian Paul): Paul's Extreme Sound Stretch (Paulstretch), 2006, with the algorithm description published alongside the program. The Stretch type and the Nebula.

Zavalishin, V.: The Art of VA Filter Design. Revision 2.1.0, Native Instruments, 2018. The topology-preserving transform and the zero-delay-feedback state-variable filter.

Rossum, D.: Dynamic digital IIR audio filter and method which provides dynamic digital filtering for audio signals. United States patent 5,170,369, 1992. The z-plane filter of the E-mu Morpheus: interpolating pole and zero parameters between frames.

Smith, J. O.: Physical Audio Signal Processing for Virtual Musical Instruments and Audio Effects. W3K Publishing, 2010 (online). Modal synthesis, digital waveguides, the Karplus-Strong loop filter.

Bilbao, S.: Numerical Sound Synthesis. Finite Difference Schemes and Simulation in Musical Acoustics. Wiley, 2009. Modal synthesis and the stability of resonator banks.

Peterson, G. E. and Barney, H. L.: Control methods used in a study of the vowels. Journal of the Acoustical Society of America 24, 175-184, 1952. The formant tables of the vowel filters.

Schroeder, M. R.: Natural sounding artificial reverberation. Journal of the Audio Engineering Society 10 (3), 219-223, 1962; Jot, J.-M. and Chaigne, A.: Digital delay networks for designing artificial reverberators. 90th AES Convention, preprint 3030, 1991. The feedback delay network of the far reverb.

Schlecht, S. J. and Habets, E. A. P.: Scattering in feedback delay networks. IEEE/ACM Transactions on Audio, Speech, and Language Processing 28, 1915-1924, 2020. All-passes inside the loop of a delay network: the Scattering mode.

Abel, J. S. and Huang, P.: A simple, robust measure of reverberation echo density. 121st AES Convention, paper 6985, 2006. The normalised echo density the reverb test measures.

Gardner, W. G.: Efficient convolution without input-output delay. Journal of the Audio Engineering Society 43 (3), 127-136, 1995. Partitioned convolution for the Room.

Battenberg, E. and Avizienis, R.: Implementing real-time partitioned convolution algorithms on conventional operating systems. Proceedings of the 14th International Conference on Digital Audio Effects (DAFx-11), Paris, 2011; Wefers, F.: Partitioned convolution algorithms for real-time auralization. Dissertation, RWTH Aachen University, 2015. Partitions that grow along the impulse, the late ones' work spread over the time until it is due: the Room's minute.

Dattorro, J.: Effect design, parts 1 and 2. Journal of the Audio Engineering Society 45 (9) and (10), 1997. Delay-line modulation, chorus and pitch shifting by a moving read pointer.

Niemitalo, O.: Hilbert transform approximation by a pair of all-pass filter chains, 2003 (published as a note on the music-dsp list and on the author's site). The quadrature pair of the Cosmos frequency shifter.

Kellet, P.: Pink noise filter, music-dsp source code archive, 1999. The seven-term approximation used by the noise type.

Voss, R. F. and Clarke, J.: 1/f noise in music and speech. Nature 258, 317-318, 1975. Why pink noise, and why a drone's slow movements should be 1/f-like.

LOUDNESS AND MASTERING

ITU-R BS.1770-4: Algorithms to measure audio programme loudness and true-peak audio level. International Telecommunication Union, 2015. K-weighting, gating, true peak.

EBU R 128: Loudness normalisation and permitted maximum level of audio signals. European Broadcasting Union, 2020 edition; and EBU Tech 3342: Loudness Range, a measure to supplement loudness normalisation. The loudness range and the gating practice.

VECTOR SYNTHESIS

Sequential Circuits: Prophet VS operation manual, 1986; Korg: Wavestation owner's manual, 1990. The joystick between four sources that the Vector follows.

THE MUSIC

Hawkes, A. G.: Spectra of some self-exciting and mutually exciting point processes. Biometrika 58, 83-90, 1971. The point process whose rate is raised by its own events: Cascade.

Koelsch, S., Vuust, P. and Friston, K.: Predictive processes and the peculiar case of music. Trends in Cognitive Sciences 23, 63-77, 2019. Listening as prediction; attention held between the too predictable and the too surprising: Surprise and Homeostat.

Rich, R.: The sleep concerts, from 1982; the recordings Somnium (2001) and Perpetual (2013), and the composer's own accounts of the concerts, of difference tones in just intonation and of stretched field material, are the practice this instrument was built after. The packs of the library name the artists whose corner of the repertoire each was written in the spirit of; nothing is sampled from or affiliated with any of them.)" },
};

// ---------------------------------------------------------------- the blocks, one by one

/**
 * @brief One block of the panel, by the name on its tab.
 *
 * What each tab of the panel IS, in a paragraph: the manual prints it under the tab's picture,
 * over the list of that tab's parameters. The parameter texts say what a knob does; these say
 * what the thing the knobs belong to is for, which is the question a reader has first.
 */
struct TabHelp { const char* name; const char* text; };
/**
 * @var const char* TabHelp::name
 * @brief The name as the tab spells it ("SOURCE 2", "MATRIX"), a section without a tab ("SPACE") or
 * a source type ("TYPE Stretch"); tabHelp() compares it exactly.
 */
/**
 * @var const char* TabHelp::text
 * @brief The paragraph, a string literal.
 */
/** @brief The tab paragraphs, in the order of the panel; the section comments name each row. */
const TabHelp kTabHelp[] = {
    // ---- the source row
    { "SOURCE 1", "The first of four equal source slots, and the one with a history: set to Additive it is the strand bank -- up to six copies of a partial bank, detuned or placed on pure ratios, fanned across the stereo field -- and the Strands section under its display belongs to it alone. Set to any other type it renders exactly like the other three. Every slot has a Type, a Level, an Octave, a just Ratio to the note and a Pan; the rest of its knobs light up according to the type." },
    { "SOURCE 2", "The second slot. Where Source 1 carries the melody of a patch, the second is most often its body or its shadow: an octave down at a fraction of the level, a wavetable with a slow position drift under an additive bank, a noise floor. Its own Partials, Tilt, Brightness, Odd/Even, Inharmonic and Shimmer apply when it is Additive; Table and Position when it is Harmonic or a Wavetable; FM Ratio and Index for FM; Grain, Density, Pitch and Grains for the Texture and Stretch types; Noise colour and Q for the noise." },
    { "SOURCE 3", "The third slot, with the same controls as the second. Three sources of the same kind a fifth and an octave apart are a chord out of one key; three of different kinds are an instrument. The Texture... button loads a clip into this slot alone, so it can play a recording the other slots do not." },
    { "SOURCE 4", "The fourth slot, added with the Vector so the four corners of its square are four sources. Off by default -- a preset that did not know about it sounds as it did -- and otherwise identical to slots 2 and 3, with its own clip." },
    { "VECTOR", "The four slots read as a place rather than as four levels, after the Prophet VS and the Korg Wavestation: a point in a square whose corners are the four sources. Amount is how much of the picture the point paints -- at 0 every slot plays at its own Level and nothing here does anything; the centre of the square is neutral by construction, so turning Amount up changes nothing until the point moves. X and Y place it, Wander lets it drift on two curves whose rates share no simple ratio, Rate is how fast. Route the point from an LFO, a macro or the wheel and one gesture moves through four landscapes." },
    // ---- the voice's second row
    { "FILTER", "The voice filter, one per voice, ten models behind one set of knobs: one- to four-pole low passes, a high pass, a band pass, a notch, a peak, the saturating ladder, a tuned comb and a formant. Cutoff follows the key (Key Track), the amplitude envelope (Env Amount), a slow wander (Drift) and the voice's distance -- a far voice is two and a half octaves darker per unit of depth. Drive saturates ahead of the filter; Fold is the wavefolder after it. The Air section beside it is the noise on the note: a band around a harmonic, or six resonators on the just harmonics." },
    { "Z-PLANE", "The second filter, after the E-mu Morpheus: filter frames sit on the corners of a cube, and a point inside it is a filter interpolated from all of them, poles and zeros alike, so every point is stable. 155 shapes in twelve families. The point wanders (Rate, Depth) around X and Y; Z is the cube's third axis; Resonance narrows every section; Key Track moves the frame with the note. Mode puts it after the voice filter, in its place, or -- Modal -- turns the frame into a bank of ringing resonators struck by the voice. Route decides whether the two filters run in series or side by side." },
    { "AMP ENV", "The amplitude envelope of every voice, in the time scale of this instrument: an attack of up to a minute, a release of two. There is no click anywhere in it by design -- a step in level is a bug here, not an effect. The six modulation envelopes on the strip along the bottom are separate and shaped by hand; this one is the four numbers everybody looks for first." },
    { "EXPRESSION", "What a hand on the keyboard can do beyond playing the note: pressure (channel or polyphonic aftertouch) pulls the voice out of the background towards the ear and lifts its level; slide (CC 74) moves the z-plane point; the bend range is here too, and the MPE switch that gives every finger its own channel. All of it is smoothed inside the voice, so a controller sending steps never steps the sound. The same three -- pressure, wheel, slide -- are also sources in the modulation matrix, for anything these fixed routes do not cover." },
    // ---- morph
    { "MORPH", "Two complete snapshots of every parameter, A and B, and a position between them. While Morph is active the instrument plays the interpolation, gliding to the position at the Glide rate -- one continuous gesture, made for a hand in VR, that moves the whole instrument from one world to another without a jump anywhere." },
    { "MACROS", "Eight knobs that mean nothing by themselves and anything through the matrix: route a macro at three targets and one hand turns three knobs at once, in the proportions you chose. They are what the OSC hands, the gestures and a controller's faders land on. Inertia is the slew every parameter passes through -- the analogue slowness that keeps even a torn-open knob from clicking." },
    // ---- foreground
    { "ENSEMBLE + DELAY", "The first two stations of the foreground bus. The Ensemble widens: as a Chorus, three modulated taps; as a Microshift, the two channels detuned a few cents against each other with nothing moving -- the version that survives a mono sum. The Delay is a stereo delay with independent left and right times (or note values, with Sync), feedback, cross-feed for ping-pong, damping and absorption in the loop, and two outputs: Mix onto the foreground, To Far into the background, so echoes recede. Duck pulls the loop's brightness down while the input is loud, so a fresh attack does not fight its own last echo." },
    { "DELAY 2 + NEAR REVERB + BLUR", "The rest of the foreground. Delay 2 is a second stereo delay in series after the first, so echoes of echoes form chains that never fall on a grid. The Near Reverb is the small room around the dry voices -- Mix, Decay, Damping and a Low Cut -- what makes a foreground sound placed rather than pasted. Blur is a spectral smear on the near bus ahead of all of it: every attack is wiped into texture, notes flow into one another, and at full Smear the spectrum freezes and only lets new energy in slowly." },
    // ---- background
    { "CLOUD + FAR REVERB", "The background. The Cloud takes grains of the recent foreground -- Send how much, Density how many a second (or a note value), Size how long, Spray how far back in time it reaches -- transposes them by octaves and fifths and drops them into the far reverb, so the past of the music keeps arriving from behind. The Far Reverb is the infinite background itself: an eight-line feedback network, dark and wide, with a decay measured in tens of seconds, Pre-Delay, Asymmetry so the two ears hear different reflections, a high cut and a low cut on the tail, Freeze, Rotate (the whole field slowly turning), Unmask (it steps aside for the foreground band by band), Diffuse (the tail arrives instead of starting) and its own Width, the funnel that reads as distance." },
    { "FEEDBACK + ROOM", "Two ways of making the instrument hear itself. Feedback returns the finished mix: To Bus into the near bus ahead of the filters and effects, throttled by the output level so it hisses and holds instead of running away; To Pitch as phase modulation of every partial, so the sound bends itself; through Tone, Drive and Tape, which adds the asymmetry, the wow and the noise floor of a machine. The Room is the convolution reverb, on the far plane in parallel: an impulse response loaded with the Impulse... button or named by the preset -- a hall, a plate, a tuned chord, a struck object -- with Pre-Delay, a high cut, a low cut, and Morph between two impulses." },
    { "BODY + PATINA", "The last two stages before the master. The Body is not a reverb but an instrument: twelve tuned modes -- wood, plate, bell or string -- fed from the whole mix and returned into it, tuned to the brain's root at a chosen multiple, ringing for as long as Decay says. The Patina is the master's age: tape wow, the highs a worn machine has lost, a noise floor that lives under the music, a gentle saturation. Every one of them is a defect, and together they are most of what separates a recording from a render." },
    // ---- the conductor
    { "BRAIN", "The conductor: chooses notes from the scale, places them on the planes between the ear and the background, holds them for minutes and lets them go, and can play a whole night by itself. Density is how many it keeps sounding, Rate how often it changes its mind, the Hold range how long a note lives, Register where it plays, Consonance how simple the ratios to the root have to be (1 is only fifths and octaves, 0 is clusters), Wander how far the root drifts. Off, only your keys play. Timbre gives it a second ear: instead of judging an interval by its ratio alone it can weigh the roughness the two tones' actual partials would make (after Sethares), so an inharmonic patch is conducted in the intervals it is consonant at. Its display is the stage: every sounding voice as a dot at its distance." },
    { "AUTOPLAY", "The brain's other mode: instead of holding a cluster it exchanges one voice at a time, in Free steps or in Chords drawn from the scale, at a Rate or on the clock, with a Tension that decides how far each step may go and a Step button to make it move now. The way a patient improviser plays a chord instrument: nothing ever changes all at once." },
    { "BRAIN 2", "A second conductor for the background alone. With it on, the far plane gets its own slow life -- its own hold range, its own Depth -- independent of the foreground's, so the two planes stop moving in step and the picture gains a second layer of time." },
    { "TUNING", "What a note means. Scale chooses the tuning -- eleven just and historical tables, a Scala file of your own, and Timbre, which is no table at all but the instrument's own dissonance curve read while it plays -- Root its centre, Ref Pitch its A. Purity is how close the instrument sits to the pure ratios, Purity Drift how far it lets them slip and at what rate, so a chord breathes in and out of tune, and Guard keeps the beats that makes out of the band where they read as wobble; Match bends the partials onto the scale's own degrees, so a tempered chord stops beating; Tide leans the whole pitch over minutes; Portamento glides between notes, slowing near consonant ratios by Gravity. Hold latches the keys." },
    { "COHERENCE", "Four slow oscillators coupled after the Kuramoto model of fireflies falling into step. At Coherence 0 they run free on their own periods; turned up they lock into one pulse and move brightness, depth, pan and the z-plane point together. The four are also sources in the matrix, so anything can be pulled into that shared breath. Sympathy is a different kind of coherence: the voices hear each other, a little of the whole foreground fed back into every voice through its own filter." },
    { "CLOCK", "Where the tempo comes from -- the internal Tempo and Run, the host, or MIDI clock -- and the beat every Sync choice in the instrument is measured against. Nothing here has to be used: the instrument's own rates are in seconds and minutes, and a synced LFO is a choice, not the default." },
    // ---- cosmos and strike
    { "COSMOS", "A parallel path, send and return, added and never replacing: a frequency shifter (Shift, with a Drift so the shift never sits still), tuned comb resonators that follow the brain's root (Res, Res Pitch), a vowel filter morphing through a-e-i-o-u (Vowel, Vowel Rate), a Nebula that smears the spectrum with random phases until at full Smear it freezes, and a Shimmer loop around the far reverb, self-regulating so it blooms and holds. Return puts the result into the foreground, To Far into the background. Thirty-two presets of its own live in the header." },
    { "STRIKE", "A struck layer on top of the voice: a Karplus-Strong string, a wooden or a metal body, excited at note-on and left to ring. Level, Type, Decay and Damp; Fires decides whether only the keys strike or the conductor's notes as well. Chance thins the conductor's strikes -- 0.2 and about one note in five is struck, which is the difference between a plucked instrument and a room where something is occasionally touched -- and Cluster lets those strikes follow the conductor's cascade, so they arrive in handfuls with long silences between instead of falling evenly. Your own keys always strike. It is the attack this instrument otherwise never has, and at a low level it is what makes a pad sound touched." },
    { "NEAR SOURCE", "A fifth source slot that belongs to the foreground alone: its type (the near types Flute, Murmur, Bowl, Ice and Drops were made for it, but any type will do), its own envelope, its own filter and its own strike, so what is played close to the ear owes the background nothing. Rendered only by the Near Events. With the Near Events it is a layer like the Cosmos -- the Preset box beside them holds a bank of foregrounds, and a chosen one stays while sound presets change under it." },
    { "NEAR EVENTS", "The foreground's own conductor. Every so often -- Every, drawn as the conductor draws its gaps -- it plays one thing on the Near Source, close, for Length: a Note, a Phrase that slides to a second consonant degree, or a Sequence, a Berlin-school ring of Steps notes on its own clock, transposed with the root, mutating one step at a time (Mutation), with ghost notes and far accents scattered through it and its filter breathing open and shut over the run (Bloom), arriving out of the far plane and leaving into it (Approach). The pitch is chosen against the harmony (Pitch: Consonant), never in the conductor's planned silences, never just after a root change, never two at once; while a Note or a Phrase sounds the conductor may hold its decisions (Hold Brain). Proximity is the near field's low lift on the event's voice. Level 0 is off." },
    // ---- sections without a tab of their own
    { "STRANDS", "The strand bank of Source 1: up to six copies of the partial bank, detuned against each other (Detune) or placed on pure ratios (Stack: octaves, fifths, a just major or minor, sevenths, harmonics, subharmonics -- one key becomes a just chord), each drifting in pitch on its own curve (Drift, Drift Rate), fanned across the stereo field (Spread). Bloom opens the brightness over Bloom Time from a duller start; Rate Wander lets every slow rate in the voice vary by up to an octave on a hundred-second curve, so nothing repeats; Freeze holds the spectrum still." },
    { "SPACE", "The spatial model, after Robert Rich: every note has a distance between the ear and the infinite background, and that one number decides its brightness, its level, how dry it is and how present. Depth is how deep the brain places its notes, Keys Depth the plane of the keys, Pan Drift the wandering of each voice's centre, Time Width the interaural time difference the far ear hears later. Arc is the hour-scale drift of the whole night. Presence is the 2-5 kHz lift on the near plane only; Breath lets every distance wander; Phase Width and its rate drift the phase between the ears so the room seems to change size; Doppler bends the pitch of a voice as it breathes closer; Externalise adds the pinna notch and the shoulder reflection headphones need to put the image outside the head. Haas and Haas Time are the band-limited widening of the foreground." },
    { "FOUNDATION", "The sub: one dry sine or triangle on the brain's root, one or two octaves down, gliding between roots, mono, added after the mid/side stage so Bass Mono leaves it alone. Binaural runs the two ears a few hertz apart. Source can be the root itself or the Difference tone of the two lowest sounding voices -- the tone the ear makes by itself in just intonation. Pad Low Cut keeps the voices out of the sub's register." },
    { "MASTER", "The end of the chain, in the header: Tilt, a see-saw of the whole spectrum around Pivot; Bass Mono, the side channel high-passed so the low end stays centred; Side Air, a lift on the sides at 3 kHz; Width; Subsonic, a steep high pass on the finished output; then the master gain and a soft clipper. No compressor anywhere. The loudness meter beside it reads the output to BS.1770." },
    // ---- the strip
    { "LFO", "Eight low-frequency oscillators, each with a shape (sine, triangle, ramps, square, a smoothed random, stepped random, or a wavetable), a rate in hertz or a note value, a phase, a depth and a mode: global, or retriggered at the start of a phrase (Per Voice is kept so older presets load, and behaves as Global: the matrix runs once a block for the instrument). Their cards are dragged onto knobs; right-click a knob to see what drives it. The rates worth using here are drone rates -- one cycle in ten seconds to one in forty minutes -- and two rates that share no simple ratio never repeat their combination." },
    { "ENV", "Six modulation envelopes, drawn by hand as points on a curve: any number of segments, a sustain point, and three modes -- one shot, loop, or a loop that holds at the sustain point until the key is released. Time scales the whole shape (or a note value spans it); Depth is how much. A shape that rises over four minutes and falls over eight is an envelope in this instrument's sense of the word. The SOURCES page beside them holds the four sources' own envelopes: level contours from silence to the source's written level, heard where a source's Env is set to Own -- so a preset can give every source its own entrance and still keep all six for modulation." },
    { "MATRIX", "Every route, one row each: a source, a target, a depth as a fraction of the target's own range, an optional second source that scales it (Via), and whether the source is read as 0..1 or -1..1. Sources are the LFOs, the envelopes, the voice's own amplitude, the macros, the Kuramoto ring, the note, its velocity and its distance, a random number per note, the Beat, and the hands -- aftertouch, wheel and slide. A modulator's own rate or depth can be a target as well: an LFO whose rate another LFO moves." },
    // ---- the pages
    { "PERFORM", "The page for playing rather than patching: the macros large, the morph, the map cursor, the note roll and the stage, and the set recorder -- Record set logs every knob, macro, route step and note with its time into a file, Play set replays it." },
    { "BROWSE", "Every preset the instrument knows, built in and from the packs, in one list: narrowed by Family, Character, Motion and Features, searched, sorted, starred. Every descriptor was measured by rendering the preset, not tagged by hand. The Map shows the same presets as points clustered by what they sound like; click one, or switch on Map blend and drag the cursor to play the blend of the presets around it. A Route walks the map by itself." },
    // ---- the source types, for the gallery
    { "TYPE Additive", "A bank of up to 32 partials with lives of their own. Partial h has amplitude h to the power of minus Tilt; Brightness fades the upper ones out; Odd/Even weights the two families; Inharmonic stretches the series like a stiff string; Shimmer lets every partial drift in level on its own slow curve. Partials above Nyquist are never generated, so nothing aliases. In Source 1 this is the strand bank." },
    { "TYPE Harmonic", "Not a table of samples but a table of spectra: 32 partial amplitudes per frame, up to 64 frames, and Position morphs between them while Pos Drift wanders it. Five tables are built in and User loads a wavetable file, which is heard here as its spectra. Alias-free like the bank, and every trick that works on partials -- presence, low cut, the feedback's phase modulation -- works here. Until the classic Wavetable type arrived, this one carried its name. The display stands the whole table in depth, every frame a line and the frame at Position lit as it moves; a click shows that one frame flat." },
    { "TYPE Wavetable", "The classic wavetable, as Serum, Vital and Hive play it: up to 256 single cycles of 2048 samples read as samples, so the shape of the wave and every harmonic up to the 512th are kept, and Position blends one frame into the next while Pos Drift wanders it. A high note reads a copy of the table an octave poorer for every octave it climbs, so nothing aliases. The five built-in tables have the Harmonic type's names -- Classic is a real sine, triangle, saw, square and pulse -- and User plays the loaded file: a WAV from Serum, Vital or Hive, a Surge .wt, a WaveEdit bank or a single cycle. Unison spreads the cycles across the field. The display stands the whole table in depth, every frame a line and the frame at Position lit as it moves; a click shows that one frame flat." },
    { "TYPE FM", "A two-operator pair: the carrier at the slot's pitch, the modulator at FM Ratio, the index up to 8 and reduced automatically on high notes so the top of the keyboard does not turn to noise. Pos Drift wanders the index. Integer ratios are bells and electric pianos; a ratio a little off an integer is a bell that beats." },
    { "TYPE Texture", "A granular player over a loaded clip: up to 64 grains (Grains) of Grain length, Density a second or per note value, starting around Position with Spread, pitched to the note (Pitch = Note; the clip's own pitch comes from its file name) or played as it is. The display shows the grains reading the clip. The Texture... button loads a clip into this slot; a pack preset names its own." },
    { "TYPE Stretch", "The same clip read as a continuum instead of as grains -- a spectral time stretch after Paulstretch. A window (Grain) of the clip is transformed, its magnitudes kept, its phases drawn afresh and the result overlap-added, while the read position crawls through the recording at one Stretch-th of its speed. No grain rhythm, no transient left standing: a field recording becomes weather. Pitch is applied before the stretch, so a note played higher does not get shorter, and Loop Fade crossfades the loop's seam unless the clip's name says _loop." },
    { "TYPE Spectral", "The clip, rebuilt rather than replayed. When it was loaded it was measured into thirty-two bands on the ear's own frequency scale, and what was kept per frame is how loud each band is, where in it the strongest partial sits, and how far above the local noise floor that content stands. Playing it back builds the sound again from an oscillator and a band of noise per band, which is the deterministic-plus-stochastic decomposition of Serra and Smith taken band by band. Nothing is a sample any more, so the note sets the pitch and Rate sets the speed and the two are finally independent -- and at Rate 0 the read head stands still and one moment of the recording is held for as long as the note is. Breath decides how much of the partials and how much of the noise comes back; Position picks the moment, Drift wanders around it, and Bright tilts the whole thing around a kilohertz. A clip too short to measure leaves the slot silent." },
    { "TYPE Bow", "A bowed string: a delay line of one period with a loop filter, and a bow pressing on it at Position. Every sample the relative velocity between bow and string decides whether the two are stuck together or slipping, through the friction curve of McIntyre, Schumacher and Woodhouse; the alternation is the Helmholtz motion, and it sustains for as long as Bow Speed is above zero. Force against Speed is the whole gesture: light and fast is breath, heavy and slow is tone, heavy and fast is the scratch of a beginner." },
    { "TYPE Noise", "Ten colours: White, Pink, Brown, Blue, Violet, Grey, a resonant Band at Position with Q from Noise Q that tracks the note, Wind (a wandering band), Crackle (sparse impulses at Density) and Digital (sample-and-hold at a rate from Position). The levels are matched, so changing the colour does not change the loudness." },
};


} // namespace

const char* tabHelp(const char* name)
{
    if (name == nullptr) return "";
    for (const TabHelp& t : kTabHelp) if (std::strcmp(t.name, name) == 0) return t.text;
    return "";
}

const char* paramHelp(ParamId id)
{
    const int i = static_cast<int>(id);
    return (i >= 0 && i < kNumParams) ? cache().text[i] : "";
}

int numHelpTopics() { return static_cast<int>(sizeof(kTopics) / sizeof(kTopics[0])); }
const char* helpTopicTitle(int index) { return (index >= 0 && index < numHelpTopics()) ? kTopics[index].title : ""; }
const char* helpTopicText(int index)  { return (index >= 0 && index < numHelpTopics()) ? kTopics[index].text : ""; }

} // namespace ambient
