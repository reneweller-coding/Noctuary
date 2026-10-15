/**
 * @file Params.h
 * @brief Parameter table (single source of truth).
 *
 * The core engine, the plugin host layer, the GUI and the render tool all read this table.
 * No framework dependencies here: this header must compile on Quest/Android.
 *
 * A parameter is an entry of ParamId and a row of paramTable(): its stable key (what presets,
 * automation, OSC and scores name it by), its display name and section, its kind, range, default
 * and skew (the knob's curve, which is also the domain the morph, the map blend and the score
 * ramp in), its unit and, for a choice, its names. The enum is append-only: a parameter's position
 * is its automation slot in the host, so nothing is ever inserted or moved. The choice-name
 * tables declared at the bottom are defined in Params.cpp beside the table itself, and the
 * ParamSection enum answers, at compile time, the question every layer keeps asking -- which
 * section is this, is it performance state, does a preset carry it.
 * @author Rene Weller
 */
#pragma once
#include <array>
#include <cstddef>

namespace ambient {

/**
 * @brief Every parameter of the instrument, in automation-slot order.
 *
 * Appended, never inserted: a parameter's position in this enum is its automation slot in the
 * host, and moving one would move somebody's automation lane with it. The group comments say
 * what each run of values is; paramTable() holds their keys, names, ranges and defaults.
 */
enum class ParamIdE : int {
    /// Master
    MASTER_GAIN,   ///< master output gain in dB
    /// Source 1: Type chooses the additive strand bank (the classic Oscillator, default) or any of
    /// the slot types below; then the bank's own spectrum parameters
    SRC1_TYPE,   ///< which source type Source 1 is (SourceType); Additive is the strand bank
    OSC_LEVEL, PARTIALS, TILT, BRIGHTNESS, ODD_EVEN, INHARMONIC, SHIMMER, SHIMMER_RATE,   ///< the bank's level, partial count, spectral tilt, brightness, odd/even balance, inharmonic stretch, shimmer depth and rate (Hz)
    UNISON, DETUNE, DRIFT, DRIFT_RATE, SPREAD, BLOOM, BLOOM_TIME,   ///< strands, their detune (ct), pitch drift (ct) and its rate (Hz), stereo spread, and Bloom with its time (s)
    /// Stack: strands at pure ratios (one key = one just chord) instead of detuned copies;
    /// Rate Wander: every voice's drift and shimmer rates themselves wander (nested LFO)
    STACK, RATE_WANDER,   ///< Stack: a StackNames choice; Rate Wander: 0 .. 1
    /// Source 1's slot fields, for the non-additive types (level is OscLevel, spectrum the eight above)
    SRC1_OCTAVE, SRC1_RATIO, SRC1_PAN, SRC1_TABLE, SRC1_POSITION, SRC1_POS_DRIFT,   ///< Source 1's slot fields: octave, just ratio, pan, table, position and its drift
    SRC1_FM_RATIO, SRC1_FM_INDEX, SRC1_GRAIN, SRC1_DENSITY, SRC1_DENSITY_SYNC, SRC1_FOLLOW, SRC1_GRAINS, SRC1_SPREAD, SRC1_NOISE, SRC1_NOISE_Q, SRC1_DRIFT,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q, pitch drift (ct)
    /// Source 2 / Source 3: the same slot, laid out identically (the engine reads them by offset):
    /// 17 slot fields, then the seven of the slot's own additive bank
    SRC2_TYPE, SRC2_LEVEL, SRC2_OCTAVE, SRC2_RATIO, SRC2_PAN, SRC2_TABLE, SRC2_POSITION, SRC2_POS_DRIFT,   ///< Source 2: type, level, octave, ratio, pan, table, position and its drift
    SRC2_FM_RATIO, SRC2_FM_INDEX, SRC2_GRAIN, SRC2_DENSITY, SRC2_DENSITY_SYNC, SRC2_FOLLOW, SRC2_GRAINS, SRC2_SPREAD, SRC2_NOISE, SRC2_NOISE_Q,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q
    SRC2_PARTIALS, SRC2_TILT, SRC2_BRIGHT, SRC2_ODD_EVEN, SRC2_INHARM, SRC2_SHIMMER, SRC2_SHIMMER_RATE, SRC2_DRIFT,   ///< Source 2's own additive bank: partials, tilt, brightness, odd/even, inharmonic, shimmer and its rate (Hz), pitch drift (ct)
    SRC3_TYPE, SRC3_LEVEL, SRC3_OCTAVE, SRC3_RATIO, SRC3_PAN, SRC3_TABLE, SRC3_POSITION, SRC3_POS_DRIFT,   ///< Source 3: type, level, octave, ratio, pan, table, position and its drift
    SRC3_FM_RATIO, SRC3_FM_INDEX, SRC3_GRAIN, SRC3_DENSITY, SRC3_DENSITY_SYNC, SRC3_FOLLOW, SRC3_GRAINS, SRC3_SPREAD, SRC3_NOISE, SRC3_NOISE_Q,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q
    SRC3_PARTIALS, SRC3_TILT, SRC3_BRIGHT, SRC3_ODD_EVEN, SRC3_INHARM, SRC3_SHIMMER, SRC3_SHIMMER_RATE, SRC3_DRIFT,   ///< Source 3's own additive bank: partials, tilt, brightness, odd/even, inharmonic, shimmer and its rate (Hz), pitch drift (ct)
    /// Foundation: a sub voice that follows the brain's root or the ghost tone (difference
    /// tone of the two lowest sounding voices); Pad Low Cut keeps the pads out of its register
    SUB_LEVEL, SUB_OCTAVE, SUB_GLIDE, SUB_BINAURAL, SUB_TONE, SUB_SOURCE, PAD_LOW_CUT,   ///< the Foundation: level, octave (-1/-2), glide (s), binaural beat (Hz), tone, source (root, difference, lowest), and the pads' low cut (Hz)
    /// Strike: a short plucked or struck impulse (Karplus-Strong) at note-on, on the near plane
    STRIKE_LEVEL, STRIKE_TYPE, STRIKE_DECAY, STRIKE_DAMP, STRIKE_WHO,   ///< strike level, type (String, Wood, Metal), decay (s), damping, and who fires it (keys, or keys and brain)
    /// Air (filtered-noise breath layer per voice)
    AIR, AIR_COLOR, AIR_Q,   ///< Air level, colour (a multiple of f0) and Q
    /// Amplitude envelope
    ATTACK, DECAY, SUSTAIN, RELEASE,   ///< the amplitude envelope: attack, decay and release in seconds, sustain 0 .. 1
    /// Filter: one of nine models (Filter.h) behind the same knobs; Drive saturates ahead of it
    FILTER_ON, FILTER_MODEL, CUTOFF, RESONANCE, FILTER_ENV, FILTER_DRIFT, KEY_TRACK, FILTER_DRIVE,   ///< filter switch, model, cutoff (Hz), resonance, envelope amount, drift, key tracking, drive
    /// Z-plane filter: four frames on a square, the point (X, Y) interpolates their poles and wanders
    /// Route: with both filters on, the z-plane follows the voice filter (Series) or both hear the
    /// dry signal and Mix balances them (Parallel); Replace is the z-plane alone
    /// ZZ is the cube's third axis (Transform); 0 is the filter as it was before it existed
    Z_MODE, Z_ROUTE, Z_SHAPE, ZX, ZY, ZZ, Z_RATE, Z_DEPTH, Z_RESONANCE, Z_KEY_TRACK, Z_MIX,   ///< z-plane mode, route, shape, the point's X, Y and Transform, its wander rate (Hz) and depth, resonance, key tracking, mix
    Z_DECAY, Z_DAMP,          ///< Modal mode: how long the modes ring, and how much shorter the high ones
    /// Space: front-to-back planes, per-voice interaural time difference, hour-scale arc,
    /// presence bell for the near plane, slow breathing of every voice's distance
    DEPTH, KEYS_DEPTH, PAN_DRIFT, ITD, ARC_AMOUNT, ARC_PERIOD, ARC_SYNC, PRESENCE, BREATH, BREATH_RATE,   ///< the planes' depth, the keys' depth, pan drift, Time Width (the ITD), the arc's amount, period (min) and sync, presence (dB), breath and its rate (Hz)
    /// Phase Width: two all-pass pairs per voice, drifting in opposite directions on the two ears,
    /// so the room seems to change size rather than the sound to move; Doppler: the breathing
    /// distance bends the pitch a little as a voice approaches or recedes
    PHASE_WIDTH, PHASE_RATE, DOPPLER, EXTERNALISE,   ///< phase width and its rate (Hz), doppler, externalisation
    /// Expression: what a key's pressure, its sideways slide and its own pitch bend do.
    /// With an MPE controller each finger has all three; a plain keyboard shares them.
    PRESS_DISTANCE, PRESS_BRIGHT, PRESS_LEVEL, SLIDE_CUTOFF, SLIDE_Z, BEND_RANGE, MPE_ON,   ///< pressure to nearness, brightness and level; slide to the filter (oct) and the z-plane; bend range (st); the MPE switch
    /// Ensemble
    ENSEMBLE_MIX, ENSEMBLE_DEPTH, ENSEMBLE_RATE, ENSEMBLE_SYNC,   ///< ensemble mix, depth, rate (Hz) and sync
    /// Stereo delay (asymmetric L/R)
    DELAY_TIME_L, DELAY_TIME_R, DELAY_SYNC_L, DELAY_SYNC_R, DELAY_FEEDBACK, DELAY_CROSS, DELAY_DAMP, DELAY_ABSORB, DELAY_MIX, DELAY_TO_FAR, DELAY_DUCK,   ///< delay 1: left and right times (s) and their syncs, feedback, cross, damping, absorb, mix, send to far, ducking
    /// Second stereo delay, in series after the first
    DELAY2_TIME_L, DELAY2_TIME_R, DELAY2_SYNC_L, DELAY2_SYNC_R, DELAY2_FEEDBACK, DELAY2_CROSS, DELAY2_DAMP, DELAY2_ABSORB, DELAY2_MIX, DELAY2_TO_FAR,   ///< delay 2: left and right times (s) and their syncs, feedback, cross, damping, absorb, mix, send to far
    /// Near reverb (foreground room)
    NEAR_MIX, NEAR_DECAY, NEAR_DAMP,   ///< near reverb mix, decay (s), damping
    /// Blur: a spectral smear on the near bus itself, so an attack is wiped into texture
    BLUR_MIX, BLUR_SMEAR,   ///< blur amount and smear
    /// Far reverb (the infinite background)
    FAR_LEVEL, FAR_SIZE, FAR_DECAY, FAR_DAMP, FAR_PRE_DELAY, FAR_ASYM, FAR_HIGHCUT, FAR_FREEZE, FAR_ROTATE, FAR_UNMASK,   ///< far reverb level, size, decay (s), damping, pre-delay (ms), asymmetry, tail cut (Hz), freeze, rotate, unmask
    /// Diffusion: modulated all-passes in front of the reverb, so the tail arrives instead
    /// of starting; and the sympathetic coupling of the voices through each other's filters
    FAR_DIFFUSE,   ///< the far reverb's diffusion
    /// Feedback: the mixed output (before the master) returns, low-passed and saturated,
    /// into the near bus before the filters and effects, and/or as phase modulation of every
    /// partial. Throttled by the output level so it hisses and holds instead of running away.
    FEEDBACK_BUS, FEEDBACK_FM, FEEDBACK_TONE, FEEDBACK_DRIVE,   ///< feedback into the bus and into the pitch (FM), its tone (Hz) and drive
    /// Room: convolution reverb with a loaded (or generated) impulse, in parallel on the far plane
    ROOM_LEVEL, ROOM_SOURCE, ROOM_PRE_DELAY, ROOM_HIGHCUT, ROOM_MORPH,   ///< room level, source (far or near), pre-delay (ms), tail cut (Hz), morph A/B
    /// Cosmos: science-fiction / deep-space path (send from the near bus)
    COSMOS_SEND, COSMOS_SHIFT, COSMOS_SHIFT_DRIFT, COSMOS_RES, COSMOS_RES_PITCH, COSMOS_RES_FEEDBACK,   ///< Cosmos send, shift (Hz) and its drift, resonator amount, pitch (x root) and feedback
    COSMOS_VOWEL, COSMOS_VOWEL_RATE, COSMOS_NEBULA, COSMOS_SMEAR, COSMOS_SHIMMER, COSMOS_SHIMMER_PITCH,   ///< vowel and its rate (Hz), nebula, smear, shimmer and its pitch
    COSMOS_RETURN, COSMOS_TO_FAR,   ///< the Cosmos return level and its send to far
    /// Granular cloud on the far plane
    CLOUD_SEND, CLOUD_DENSITY, CLOUD_SYNC, CLOUD_SIZE, CLOUD_PITCH, CLOUD_SPRAY, CLOUD_LEVEL,   ///< cloud send, density (/s) and sync, grain size (ms), pitch, spray (s), level
    /// Body: a bank of modes under everything -- the soundboard the pad sits on (Body.h)
    BODY_LEVEL, BODY_MATERIAL, BODY_PITCH, BODY_DECAY, BODY_TONE, BODY_SPREAD,   ///< body level, material, pitch (x root), decay (s), tone, spread
    /// Patina: the master's age -- tape wow, lost highs, a noise floor, gentle saturation
    PATINA_AMOUNT, PATINA_WOW, PATINA_HISS, PATINA_AGE,   ///< patina amount, wow, hiss, age
    /// Master tilt: one broad see-saw around a pivot -- the move an ambient mix asks for
    /// more than any other, and the one thing the master stage did not have
    TILT2, TILT_PIVOT,   ///< the master tilt (dB) and its pivot (Hz)
    /// Mid/side master stage
    BASS_MONO, SIDE_AIR, WIDTH, MONO_GUARD,   ///< bass mono below (Hz), side air (dB), width, mono safe
    /// Cluster brain (generative sleep-concert mode)
    BRAIN_ON, BRAIN_DENSITY, BRAIN_RATE, BRAIN_SYNC, BRAIN_HOLD_MIN, BRAIN_HOLD_MAX,   ///< brain on, density, event rate (s) and sync, hold min and max (s)
    BRAIN_LOW, BRAIN_HIGH, BRAIN_CONSONANCE, BRAIN_WANDER, BRAIN_QUANTIZE,   ///< the brain's register (lowest, highest), consonance, wander, quantize
    /// Autoplay: instead of notes coming and going, the cluster stays full and one voice at a
    /// time is exchanged, so the chord travels. Step is a trigger, not a level.
    AUTO_MODE, AUTO_RATE, AUTO_SYNC, AUTO_LEAD, AUTO_TENSION, AUTO_ROOT_MOVE, AUTO_STEP,   ///< autoplay mode, every (s) and sync, voice leading (st), tension, root move, the step trigger
    /// A second conductor for the background: its own register, pace and plane, on the
    /// first one's root (plus an interval), so the two play a slow counterpoint
    BRAIN2_ON, BRAIN2_DENSITY, BRAIN2_RATE, BRAIN2_HOLD_MIN, BRAIN2_HOLD_MAX,   ///< brain 2 on, density, event rate (s), hold min and max (s)
    BRAIN2_LOW, BRAIN2_HIGH, BRAIN2_DEPTH, BRAIN2_INTERVAL, BRAIN2_CONSONANCE,   ///< brain 2's register (lowest, highest), plane, interval (st), consonance
    /// Tuning
    SCALE, KEY_MAP, ROOT_NOTE, REF_PITCH, SEED, HOLD,   ///< scale (ScaleNames), key mapping, root note, A4 (Hz), seed, hold
    /// Purity blends every note between 12-TET (0) and the chosen scale (1) in the log domain; Drift
    /// lets that blend wander so the beating locks in and loosens over minutes; Freeze holds every
    /// voice's spectrum and pitch still (drifts, shimmer, bloom stop moving)
    TUNE_PURITY, TUNE_DRIFT, TUNE_DRIFT_RATE, FREEZE,   ///< purity, its drift and the drift's rate (Hz), freeze
    /// Tide: the whole instrument's pitch leans by a few cents over many minutes
    TIDE, TIDE_PERIOD,   ///< the tide's depth (ct) and period (min)
    /// Ghost: the Air noise through a bank of sharp resonators on the note's just harmonics
    AIR_MODE,   ///< Air mode: Band or Ghost (AirModeNames)
    /// Portamento for keys: a new key glides from the last one; Gravity slows the glide near
    /// consonant ratios to the root, so the slide "clicks into" harmonic nodes on the way
    PORTAMENTO, PORTA_GRAVITY,   ///< portamento time (s) and gravity
    /// Tape in the feedback loop: asymmetric saturation, wow and flutter, level-dependent noise floor
    FEEDBACK_TAPE,   ///< tape in the feedback loop
    /// Coherence: four slow Kuramoto oscillators, coupled by Coherence, modulating brightness,
    /// depth, pan drift and the z-plane point by Depth
    COHERENCE, COHERENCE_DEPTH, COHERENCE_RATE, SYMPATHY,   ///< coherence coupling, depth, rate (x), sympathy
    /// Modulation: eight free LFOs and six multi-segment envelopes. Their shapes and the matrix
    /// rows are data, not parameters (see Modulation.h); what sits here is what a host automates.
    LFO1_SHAPE, LFO1_RATE, LFO1_PHASE, LFO1_DEPTH, LFO1_MODE, LFO1_TABLE, LFO1_SYNC,   ///< LFO 1: shape, rate (Hz), phase, depth, mode, table, sync
    LFO2_SHAPE, LFO2_RATE, LFO2_PHASE, LFO2_DEPTH, LFO2_MODE, LFO2_TABLE, LFO2_SYNC,   ///< LFO 2: shape, rate (Hz), phase, depth, mode, table, sync
    LFO3_SHAPE, LFO3_RATE, LFO3_PHASE, LFO3_DEPTH, LFO3_MODE, LFO3_TABLE, LFO3_SYNC,   ///< LFO 3: shape, rate (Hz), phase, depth, mode, table, sync
    LFO4_SHAPE, LFO4_RATE, LFO4_PHASE, LFO4_DEPTH, LFO4_MODE, LFO4_TABLE, LFO4_SYNC,   ///< LFO 4: shape, rate (Hz), phase, depth, mode, table, sync
    LFO5_SHAPE, LFO5_RATE, LFO5_PHASE, LFO5_DEPTH, LFO5_MODE, LFO5_TABLE, LFO5_SYNC,   ///< LFO 5: shape, rate (Hz), phase, depth, mode, table, sync
    LFO6_SHAPE, LFO6_RATE, LFO6_PHASE, LFO6_DEPTH, LFO6_MODE, LFO6_TABLE, LFO6_SYNC,   ///< LFO 6: shape, rate (Hz), phase, depth, mode, table, sync
    LFO7_SHAPE, LFO7_RATE, LFO7_PHASE, LFO7_DEPTH, LFO7_MODE, LFO7_TABLE, LFO7_SYNC,   ///< LFO 7: shape, rate (Hz), phase, depth, mode, table, sync
    LFO8_SHAPE, LFO8_RATE, LFO8_PHASE, LFO8_DEPTH, LFO8_MODE, LFO8_TABLE, LFO8_SYNC,   ///< LFO 8: shape, rate (Hz), phase, depth, mode, table, sync
    ENV1_MODE, ENV1_TIME, ENV1_DEPTH, ENV1_SYNC,   ///< Env 1: mode, time (x), depth, sync
    ENV2_MODE, ENV2_TIME, ENV2_DEPTH, ENV2_SYNC,   ///< Env 2: mode, time (x), depth, sync
    ENV3_MODE, ENV3_TIME, ENV3_DEPTH, ENV3_SYNC,   ///< Env 3: mode, time (x), depth, sync
    ENV4_MODE, ENV4_TIME, ENV4_DEPTH, ENV4_SYNC,   ///< Env 4: mode, time (x), depth, sync
    ENV5_MODE, ENV5_TIME, ENV5_DEPTH, ENV5_SYNC,   ///< Env 5: mode, time (x), depth, sync
    ENV6_MODE, ENV6_TIME, ENV6_DEPTH, ENV6_SYNC,   ///< Env 6: mode, time (x), depth, sync
    /// Morph between two stored full presets (A/B); never part of a preset itself
    MORPH_ACTIVE, MORPH_POS, MORPH_GLIDE,   ///< morph active, position, glide (s)
    /// Macros: eight performance controls routed through the gesture layer (Custom0..7);
    /// not part of presets. Named in Rich's vocabulary, not the engine's.
    MACRO_A, MACRO_B, MACRO_C, MACRO_D, MACRO_E, MACRO_F, MACRO_G, MACRO_H,   ///< the eight macros: Space, Alien, Motion, Bloom, Density, Distance, Evolution, Air
    /// Inertia: every float parameter glides to its value with this time constant (the analogue
    /// slew), so even a knob torn open arrives slowly; performance state, not in presets
    INERTIA,   ///< the analogue slew, in seconds
    /// Preset map: a cursor in the plane of all presets blends its neighbours (PresetMap.h);
    /// performance state like the morph, never part of a preset
    MAP_ACTIVE, MAP_X, MAP_Y, MAP_RADIUS,   ///< map active, the cursor's X and Y, the blend radius
    /// Route: the engine walks a route of waypoints over the map (Route.h); performance state
    ROUTE_ACTIVE, ROUTE_SPEED, ROUTE_LOOP,   ///< route play, speed (x), loop
    /// Clock (Clock.h): where the tempo comes from, the internal tempo, and whether the internal
    /// clock runs; performance state -- the tempo belongs to the session, not to a preset
    CLOCK_SOURCE, TEMPO, CLOCK_RUN,   ///< clock source, tempo (bpm), run
    /// Appended, never inserted: a parameter's position in this enum is its automation slot in
    /// the host, and moving one would move somebody's automation lane with it.
    ///
    /// The other end of the filter funnel on the three reverb returns. A tail with its low mids
    /// still in it sits in front of the music instead of behind it.
    FAR_LOWCUT, NEAR_LOWCUT, ROOM_LOWCUT,   ///< the low cuts (Hz) on the far, near and room returns
    /// A steep high-pass on the finished output for the energy below hearing: it does nothing for
    /// the music and everything for the amplifier. Off by default, because the Foundation reaches
    /// lower than the frequency a mastering engineer would cut at.
    SUBSONIC,   ///< the subsonic high-pass corner (Hz), off by default
    /// Vector: a point in a square whose corners are the three source slots and the three of them
    /// together, after the Prophet VS and the Wavestation. Amount 0 leaves every slot's level
    /// exactly as it is set.
    VEC_AMOUNT, VEC_X, VEC_Y, VEC_WANDER, VEC_RATE,   ///< vector amount, X and Y, wander and its rate (Hz)
    /// Source 4: the same slot as 2 and 3, laid out identically, so the Vector's four corners are
    /// four sources rather than three and the three together. Appended for the same reason as
    /// everything above it.
    SRC4_TYPE, SRC4_LEVEL, SRC4_OCTAVE, SRC4_RATIO, SRC4_PAN, SRC4_TABLE, SRC4_POSITION, SRC4_POS_DRIFT,   ///< Source 4: type, level, octave, ratio, pan, table, position and its drift
    SRC4_FM_RATIO, SRC4_FM_INDEX, SRC4_GRAIN, SRC4_DENSITY, SRC4_DENSITY_SYNC, SRC4_FOLLOW, SRC4_GRAINS, SRC4_SPREAD, SRC4_NOISE, SRC4_NOISE_Q,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q
    SRC4_PARTIALS, SRC4_TILT, SRC4_BRIGHT, SRC4_ODD_EVEN, SRC4_INHARM, SRC4_SHIMMER, SRC4_SHIMMER_RATE, SRC4_DRIFT,   ///< Source 4's own additive bank: partials, tilt, brightness, odd/even, inharmonic, shimmer and its rate (Hz), pitch drift (ct)
    /// The Stretch type's two settings, one pair per slot, fields 26 and 27 of the slot.
    SRC1_STRETCH, SRC1_XFADE, SRC2_STRETCH, SRC2_XFADE, SRC3_STRETCH, SRC3_XFADE, SRC4_STRETCH, SRC4_XFADE,   ///< each slot's stretch factor (x) and loop fade
    /// Four things a dark-ambient mixing desk does that this instrument could not, each off at its
    /// default so nothing that exists sounds different: the background narrowed as it goes back,
    /// a static micro-detune instead of the chorus, a band delayed to one side, and a wavefolder.
    FAR_WIDTH, ENS_MODE, HAAS, HAAS_TIME, FILTER_FOLD,   ///< far reverb width, ensemble mode, Haas amount and time (ms), the wavefolder
    /// What the literature after the classics asks for: a stretched octave (Ward 1954, Terhardt),
    /// a reverb with scattering in its loop (Schlecht and Habets 2020), masking that spreads
    /// upward (Zwicker), and a binaural mode that follows the head. Neutral at every default.
    TUNE_STRETCH, FAR_MODE, FAR_UNMASK_SPREAD, BINAURAL,   ///< octave stretch (ct), far reverb mode, unmask spread, binaural mode
    /// The conductor judging intervals by the spectrum it actually plays (Sethares), not by
    /// the ratio alone. 0 = the ratio score it always had.
    BRAIN_TIMBRE, BRAIN_SPACING,   ///< the conductor's timbre weighting and its spacing
    /// The room's early reflections, from its geometry (see Effects.h): off at Level 0.
    EARLY_LEVEL, EARLY_SIZE, EARLY_ABSORB, EARLY_WIDTH,   ///< early reflections: level, room size (m), absorption, width
    /// The Bow type's two: how hard the bow presses and how fast it travels. Fields 28 and 29
    /// of every slot.
    SRC1_BOW_FORCE, SRC1_BOW_SPEED, SRC2_BOW_FORCE, SRC2_BOW_SPEED, SRC3_BOW_FORCE, SRC3_BOW_SPEED, SRC4_BOW_FORCE, SRC4_BOW_SPEED,   ///< each slot's bow force and speed
    SRC1_SPEC_RATE, SRC1_SPEC_BREATH, SRC2_SPEC_RATE, SRC2_SPEC_BREATH, SRC3_SPEC_RATE, SRC3_SPEC_BREATH, SRC4_SPEC_RATE, SRC4_SPEC_BREATH,   ///< each slot's spectral read rate (x) and breath
    BRAIN_HARMONIC, BRAIN_KEY, BRAIN_EVEN, BRAIN_SMOOTH,   ///< conductor: harmonic weighting, key, even, smooth
    TUNE_GUARD, TUNE_MATCH, BRAIN_BLEND, ARC_HARMONY,   ///< purity guard, timbre match, brain blend, arc harmony
    ELEV_NEAR, ELEV_FAR, DEPTH_LAW, ENVELOP,   ///< the elevation of the near and far planes, depth law, envelop
    ARC_CLOCK,   ///< the arc on the clock
    BRAIN_CASCADE, BRAIN_SURPRISE, BRAIN_HOMEOSTAT, TUNE_ADAPT,   ///< conductor cascade, surprise, homeostat; adaptive purity
    FAR_COMOD, NEAR_ILD,   ///< far reverb comodulation; the near field's level difference
    SRC1_TRANSPORT, SRC2_TRANSPORT, SRC3_TRANSPORT, SRC4_TRANSPORT, SUB_PULSE, FEEDBACK_BIAS,   ///< each slot's Transport (the Harmonic morph by optimal transport); the sub's pulse; feedback bias
    LENIA_RATE, LENIA_GROWTH,   ///< Lenia rate (Hz) and growth (coherence)
    BRAIN_DEJA_VU, BRAIN_LOOP, BRAIN_SPREAD, BRAIN_BIAS, CHAOS_PERIOD, KEYS_FILTER, TRANSPOSE, PARTIAL_SPREAD,   ///< conductor deja vu, loop, spread, bias; chaos period (s); MPE filter; transpose; Partial Spread
    STRIKE_CHANCE, STRIKE_CLUSTER, COSMOS_SWELL,   ///< strike chance and cluster weighting; Cosmos swell
    /// Each slot's own entrance. Until these existed the four sources of a preset all started
    /// together on the one amplitude envelope in the Envelope section, so a preset with a
    /// wavetable and a texture was a single chord struck twice at once, however different the two
    /// materials were. Delay holds the slot silent after the note, Rise fades it in, and Env picks
    /// one of the six shapes as the slot's own contour instead. All three default to off, so a
    /// preset that says nothing about them sounds exactly as it did.
    SRC1_DELAY, SRC1_RISE, SRC1_ENV,   ///< Source 1's entrance: delay (s), rise (s), envelope choice (SlotEnvNames)
    SRC2_DELAY, SRC2_RISE, SRC2_ENV,   ///< Source 2's entrance: delay (s), rise (s), envelope choice
    SRC3_DELAY, SRC3_RISE, SRC3_ENV,   ///< Source 3's entrance: delay (s), rise (s), envelope choice
    SRC4_DELAY, SRC4_RISE, SRC4_ENV,   ///< Source 4's entrance: delay (s), rise (s), envelope choice
    /// How a grain reads between two samples of its clip. Linear is what the instrument always
    /// did; Hermite costs two more reads a sample and is offered rather than imposed, because
    /// which of them is right is a matter of taste in an instrument built to be soft.
    SRC1_INTERP, SRC2_INTERP, SRC3_INTERP, SRC4_INTERP,   ///< each slot's interpolation: Linear or Hermite (InterpNames)
    /// Unison in a slot: the main oscillator has had detuned strands with their own place in the
    /// field since the beginning, a slot had one mono voice. Defaults to one copy, which is that.
    /// Root: how much of a fundamental a wavetable is given when its own has none.
    SRC1_ROOT, SRC2_ROOT, SRC3_ROOT, SRC4_ROOT,   ///< each slot's fundamental share (Harmonic)
    SRC1_UNISON, SRC1_UNI_DETUNE, SRC1_UNI_WIDTH,   ///< Source 1's unison copies, their detune (ct) and width
    SRC2_UNISON, SRC2_UNI_DETUNE, SRC2_UNI_WIDTH,   ///< Source 2's unison copies, their detune (ct) and width
    SRC3_UNISON, SRC3_UNI_DETUNE, SRC3_UNI_WIDTH,   ///< Source 3's unison copies, their detune (ct) and width
    SRC4_UNISON, SRC4_UNI_DETUNE, SRC4_UNI_WIDTH,   ///< Source 4's unison copies, their detune (ct) and width
    /// The cloud as a granular feedback instrument (12.09.2026): its loop, the scatter over the
    /// scale's intervals, its flocks and its resonators on the scale's notes. All off by default,
    /// so a preset that says nothing about them has the cloud it always had.
    CLOUD_FEEDBACK, CLOUD_TONE, CLOUD_TRANSPOSE, CLOUD_SCATTER, CLOUD_SWARM,   ///< cloud feedback, tone (Hz), transpose, scatter, swarm
    CLOUD_RESONANCE, CLOUD_RES_MODE, CLOUD_RES_NOTES, CLOUD_RES_DECAY,   ///< cloud resonance, its mode, its notes and its ring time (s)
    /// The Memory (12.09.2026): a drifting sound memory beside the Cosmos -- a send, two returns, the
    /// lines and their exchange, how long and how darkly it keeps, the recalled grains, and the tape.
    MEM_SEND, MEM_RETURN, MEM_TO_FAR, MEM_LINES, MEM_SIZE, MEM_BLUR, MEM_DRIFT, MEM_HOLD, MEM_AGE,   ///< Memory send, return, to far, lines, size (s), blur, drift, hold, age
    MEM_RENEW, MEM_DRIVE, MEM_RECALL, MEM_SEEK, MEM_GRAIN, MEM_FREEZE, MEM_REVERSE, MEM_HALF, MEM_ERASE,   ///< Memory renew, drive, recall, seek, grain (ms), freeze, reverse, half speed, erase
    /// The shifter in the spectrum (12.09.2026): which shifter the shimmer's loop uses, and the
    /// cloud's loop transposed on every pass.
    COSMOS_SHIMMER_MODE, CLOUD_SHIFT,   ///< the shimmer's shifter (ShimmerModeNames); the cloud loop's shift (CloudShiftNames)
    /// Each source's own envelope (11.09.2026). A slot's Env could only borrow one of the six
    /// modulation envelopes, so every source that entered on a shape left one envelope fewer for
    /// modulating anything else. Env = Own reads the slot's own shape with these four, which do
    /// what an Env section's do: Mode, Time, Depth and Sync, four per slot in that order.
    SRC1_ENV_MODE, SRC1_ENV_TIME, SRC1_ENV_DEPTH, SRC1_ENV_SYNC,   ///< Source 1's own envelope: mode, time (x), depth, sync
    SRC2_ENV_MODE, SRC2_ENV_TIME, SRC2_ENV_DEPTH, SRC2_ENV_SYNC,   ///< Source 2's own envelope: mode, time (x), depth, sync
    SRC3_ENV_MODE, SRC3_ENV_TIME, SRC3_ENV_DEPTH, SRC3_ENV_SYNC,   ///< Source 3's own envelope: mode, time (x), depth, sync
    SRC4_ENV_MODE, SRC4_ENV_TIME, SRC4_ENV_DEPTH, SRC4_ENV_SYNC,   ///< Source 4's own envelope: mode, time (x), depth, sync
    /// What a generator of this music needs and the conductor had no way to say (12.09.2026, from
    /// Rene's rule book for a self-playing ambient generator): the register roles and the spacing
    /// that follows from them, the colour of the intervals, a clock that breathes and guards what
    /// it may not do, and the root's walk over the hour. Every one of them does nothing at its
    /// default, so every preset written before them plays exactly as it did.
    ///
    /// Layers and register: the pyramid (one voice at the bottom, most in the body, few on top),
    /// the bass held longer than what moves above it, the top softer, spacing sharpened with depth,
    /// the floor under which no third is chosen, and the leading note kept off the root.
    BRAIN_LAYERS, BRAIN_BASS_HOLD, BRAIN_TOP_SOFT, BRAIN_LOW_SPACING, BRAIN_THIRD_FLOOR, BRAIN_LEADING,   ///< layers, bass hold (x), top soft, low spacing, third floor, leading
    /// Interval colour: thirds sought or avoided, seconds likewise (above the third floor only),
    /// the seventh lifted -- as 7/4 in a just scale, the most characteristic interval this music
    /// has -- and the chance that a root change swaps one degree of the mode instead.
    BRAIN_THIRDS, BRAIN_SECONDS, BRAIN_SEVENTH, BRAIN_DEGREE_SWAP,   ///< thirds, seconds, seventh, degree swap
    /// Time: the mean event rate breathing on its own slow curve, the overlap that keeps a voice
    /// sounding while the one it replaces lets go, the thirty-millisecond rule as a guard, the gap
    /// between two note-offs, the silence a pitch must keep before it may sound again, density
    /// changes walked one voice at a time, and the planned silence at a section's border.
    BRAIN_RATE_BREATH, BRAIN_BREATH_PERIOD, BRAIN_OVERLAP, BRAIN_ONSET_GUARD, BRAIN_RELEASE_GAP,   ///< rate breath, breath period (min), overlap (s), onset guard, release gap (s)
    BRAIN_RETRIGGER, BRAIN_DENSITY_SLEW, BRAIN_SILENCE, BRAIN_SILENCE_LEN,   ///< retrigger (s), density slew (min), silence chance and its length (s)
    /// Root and form: where a new root may come from, which way it leans, the pivot window in which
    /// old and new root sound together, the pull back to the root the night began on, and the
    /// memory that forbids a chord the hour has already had.
    BRAIN_ROOT_STEPS, BRAIN_ROOT_DOWN, BRAIN_PIVOT, BRAIN_HOME, BRAIN_HOME_TIME, BRAIN_MEMORY,   ///< root steps (RootStepNames), root down, pivot (s), home, home time (min), memory (min)
    /// The background conductor on a clock that never lines up with the foreground's; the tuning
    /// holding what already sounds when the root moves; and a ceiling on how fast a beat may beat.
    /// (Brain2 Interval was asked for too, and already existed: brain2_interval, in semitones.)
    BRAIN2_GOLDEN, TUNE_HOLD_SOUNDING, BEAT_CEILING,   ///< brain 2's golden clock; the tuning holding sounding notes; the beat ceiling (Hz)
    /// Three that are true outside the conductor: a quiet note entering slower than a loud one, the
    /// plane a voice stands in taken from its role rather than from the draw, and the strands of a
    /// low note detuned less than those of a high one, so warmth does not become wobble.
    ENV_VEL_ATTACK, LAYER_DEPTH, STRAND_LOW_DETUNE,   ///< velocity to attack, layer depth, the strands' low detune
    /// How much of the granular cloud comes back on the NEAR plane instead of the far one. The cloud
    /// has always returned to the far bus alone -- a deliberate choice, written down in the concept
    /// paper: "it exists in the background and the far reverb smears it". At 0 that is exactly what
    /// still happens, so no preset changes; above it the cloud can be brought forward, which is what
    /// a listener expects of a send they have turned all the way up.
    CLOUD_TO_NEAR,   ///< the cloud's return on the near plane
    /// The near plane (13.09.2026). A role per slot -- which of the voice's notes it sounds in:
    /// all, the lowest, an inner one, the highest -- which belongs to the sound preset. And the
    /// near layer, which does not: a source of its own (Near Source: a fifth slot with its own
    /// type, envelope, filter and strike, rendered only by the events) and the Near Events section
    /// (what the instrument plays close to the ear, on a clock of its own). The layer is a bank
    /// like the Cosmos, kept across sound presets, so a night's foreground can be chosen once and
    /// the backgrounds changed under it. Level 0 is off, and every preset ever written has it there.
    SRC1_ROLE, SRC2_ROLE, SRC3_ROLE, SRC4_ROLE,   ///< each slot's role (SlotRole)
    FORE_TYPE, FORE_OCTAVE, FORE_RATIO, FORE_POSITION, FORE_POS_DRIFT, FORE_DENSITY, FORE_FOLLOW, FORE_BRIGHT,   ///< the near source: type, octave, ratio, position and its drift, density (/s), pitch follow, brightness
    FORE_FORCE, FORE_SPEED, FORE_NOISE, FORE_NOISE_Q, FORE_FM_RATIO, FORE_FM_INDEX, FORE_PARTIALS, FORE_TILT,   ///< force, speed, noise colour and Q, FM ratio and index, partials, tilt
    FORE_INHARM, FORE_DRIFT, FORE_TABLE, FORE_ATTACK, FORE_DECAY, FORE_SUSTAIN, FORE_RELEASE,   ///< inharmonic, drift (ct), table, attack, decay, sustain, release (s)
    FORE_CUTOFF, FORE_RESONANCE, FORE_FILTER_MODEL, FORE_FILTER_ENV, FORE_STRIKE, FORE_STRIKE_TYPE, FORE_STRIKE_DECAY, FORE_STRIKE_DAMP,   ///< cutoff (Hz), resonance, filter model and envelope amount, strike level, type, decay (s) and damp
    FORE_LEVEL, FORE_KIND, FORE_RATE, FORE_CHANCE, FORE_CLUSTER, FORE_LENGTH,   ///< the near events: level, kind (NearKindNames), every (s), chance, cluster weighting, length (s)
    FORE_PITCH, FORE_SPREAD, FORE_APPROACH, FORE_PROXIMITY, FORE_HOLD, FORE_GLIDE,   ///< pitch rule (NearPitchNames), spread, approach, proximity, hold brain, glide (s)
    FORE_STEPS, FORE_STEP, FORE_STEP_SYNC, FORE_MUTATION, FORE_SCATTER, FORE_BLOOM,   ///< sequence steps, step time (s) and its sync, mutation, scatter, bloom
    /// Where an event sits between the planes (0 at the ear, 1 on the horizon; Approach arrives
    /// from the horizon to it, or, negative, leaves from it), and how much of it goes straight to
    /// the output past every reverb and delay: a rattle twenty centimetres from the nose, a click
    /// that no room may soften.
    FORE_DISTANCE, FORE_DRY,   ///< the event's plane and its dry share
    /// Whether a sound preset of a pack brings its own foreground (the artist's table, NearAuto.inc).
    FORE_AUTO,   ///< whether a pack's sound preset brings its own foreground
    /// How long the far reverb takes to come back after the foreground has ducked it. It came back
    /// in 1.2 seconds since the unmask was built, and that stays the default; a foreground that
    /// speaks and then lets the horizon return over five to ten seconds makes the return itself a
    /// gesture, which is what the near events want.
    FAR_UNMASK_RETURN,   ///< seconds the far reverb takes to come back after the foreground ducked it
    /// The foreground's own sends, per event: a share of the event's voice into the second delay
    /// and into the Cosmos, on top of what the near bus gives them. A send on a desk, not a
    /// routing switch -- the event still sounds where Distance and Dry put it, and this is what is
    /// thrown into the long chain or into the deep space beside it.
    FORE_DELAY2, FORE_COSMOS,   ///< the event's sends into the second delay and into the Cosmos
    /// The foreground's own gain, in dB, after its source and before everything it passes through
    /// (14.09.2026). Level is the source's level, and for several near sources it is not only a
    /// level -- a jet, a bow, a clip each read it their own way, one clamps it at two -- so it cannot
    /// carry a calibration of twenty decibels. This does, and the bank's presets carry a measured
    /// one each: Tools/library/near_loudness.py puts every preset's loudest moment at the same
    /// distance from the background, where they had lain eighteen decibels apart.
    FORE_GAIN,   ///< the foreground's own gain in dB
    /// The production guide's depth model, built in (25.09.2026). Until then a note at the
    /// horizon was 6 dB quieter than one at the ear, its far send left at the same instant as its
    /// direct sound, and its strands fanned out as wide wherever it stood -- three cues that said
    /// "near" while the low-pass and the wet share said "far". Range is the level a source loses
    /// between the ear and the horizon, in dB; Gap the pre-delay a source at the ear gets before
    /// the far reverb, shrinking to nothing on the horizon; Near Width how wide the strands fan
    /// out at the ear as a share of their width on the horizon.
    DEPTH_RANGE, DEPTH_PRE_DELAY, DEPTH_WIDTH,   ///< level drop over the depth (dB); the near source's gap before the far reverb (ms); the strands' width at the ear (0 .. 1 of Spread)
    /// A high-pass in front of every reverb -- the near room, the far hall and the convolution
    /// room -- so that nothing under it is ever thrown into a tail (the guide's first rule of
    /// reverb: 150 to 300 Hz, before the send, not on the return). The Low Cut knobs of the three
    /// reverbs stay what they were, filters on the tail itself.
    SEND_LOWCUT,   ///< Hz, the second-order high-pass in front of every reverb's input
    /// The Foundation's harmonics and its beat. Harmonics adds the second and third partial of the
    /// sub at -24 and -27 dB at full, so the sub is still heard as a note on a small speaker where
    /// its fundamental is not reproduced (the residue pitch); Beat sums a second sine a fraction of
    /// a hertz above the first, in both ears alike, so the level breathes over two to ten seconds
    /// -- the slow swell of a Lustmord sub, which the Binaural offset between the ears is not.
    SUB_HARMONICS, SUB_BEAT,   ///< the sub's 2nd and 3rd harmonics (0 .. 1); a second sine this many Hz above the sub, beating in mono
    /// The rest of the production guide's rooms and low end (25.09.2026, the second round). To Far
    /// sends the convolution room's return into the far hall, the guide's serial rooms: the main
    /// room's output feeds the room behind it, so the horizon sounds like the same place going on
    /// rather than a second, foreign one. Mid Low Cut high-passes the middle of the far hall's
    /// return and leaves its sides, so the centre stays free for the near plane. Ceiling is the
    /// Foundation's own limiter, in dBFS at the output: a beating sub swings its peaks by up to
    /// six decibels, and without a limiter of its own it pushed the whole mix into the clipper.
    ROOM_TO_FAR, FAR_MID_LOWCUT, SUB_CEILING,   ///< the room's return into the far hall (0 .. 0.5); the far return's mid high-pass (Hz, 20 = off); the sub's ceiling (dBFS)
    /// The review of the conductor against ambient harmony (25.09.2026). Root Targets: which
    /// intervals the root's next step aims at, and how often (RootTargetNames) -- the six the
    /// conductor always drew alike, or the modal weighting with the whole tone the genre moves by.
    /// Utonal: Harmonic also hears undertone sets, so a dark family can listen for rootedness
    /// without being pulled into major. Series: the Harmonic Cloud, candidates drawn to the
    /// harmonics of the root's fundamental rather than to the scale's degrees alone.
    BRAIN_ROOT_TARGETS, BRAIN_UTONAL, BRAIN_SERIES,   ///< root targets (RootTargetNames); utonal (0 .. 1); series (0 .. 1)
    /// The circuit models of the voice filter (26.09.2026, from Ephemeris): the SEM's response,
    /// from low pass through notch to high pass.
    FILTER_MORPH,   ///< 0 .. 1: low pass, notch at 0.5, high pass (the SEM; the other models ignore it)
    COUNT   ///< one past the last: NumParams
};

extern const char* const NearKindNames[3];     ///< "Note", "Phrase", "Sequence"
extern const char* const NearPitchNames[5];    ///< "Consonant", "Highest", "Lowest", "Root", "Cluster"

extern const char* const RootStepNames[4];     ///< "Any", "Fifths", "Diatonic", "Falling"
extern const char* const RootTargetNames[4];   ///< "Classic", "Modal", "Mediant", "Phrygian": the weightings of BrainParams::rootTargets
extern const char* const ShimmerModeNames[2];  ///< "Spectral" (phase-locked peak shifting), "Grain" (the two-head shifter)
extern const char* const CloudShiftNames[7];   ///< "Off", "+12", "+7", "+5", "-5", "-7", "-12"
extern const float CloudShiftSemitones[7];     ///< the semitones behind CloudShiftNames: 0, 12, 7, 5, -5, -7, -12

constexpr int NumParams = static_cast<int>(ParamIdE::COUNT);   ///< how many parameters there are; the size of every per-parameter table

/** @brief How a parameter's value is to be read: a continuous float, a stepped integer, a switch, or one of a list of names. */
enum class ParamKindE {
    FLOAT,   ///< @brief continuous, with a skew
    INT,     ///< @brief whole numbers, linear
    BOOL,    ///< @brief 0 or 1
    CHOICE   ///< @brief an index into `choices`
};

/** @brief One row of the parameter table: everything a host, an editor or a preset needs to know about a parameter. */
struct ParamDesc {
    ParamIdE     m_id;       ///< the parameter; rows are ordered by it, so paramTable()[id] is this row
    const char* m_key;      ///< stable identifier (automation / presets)
    const char* m_name;     ///< display name
    const char* m_section;  ///< GUI grouping
    ParamKindE   m_kind;     ///< how the value is read
    float       m_min;      ///< the lowest value, in the parameter's unit
    float       m_max;      ///< the highest value
    float       m_def;      ///< the default: what a fresh instance has, and what an unmentioning preset sets
    float       m_skew;     ///< 1 = linear, <1 = more resolution at the low end
    const char* m_unit;     ///< the unit shown after the value ("Hz", "s", "dB", "ct", ""), for the editor and the help
    const char* const* m_choices; ///< ParamKind::Choice only
    int         m_numChoices;     ///< how many names `choices` holds (0 for anything but a choice)
};

/**
 * @brief Ordered by ParamId. Verified by the self test.
 * @return the table, NumParams rows, row i describing ParamId i
 */
const std::array<ParamDesc, NumParams>& paramTable();
/**
 * @brief The row of one parameter.
 * @param[in] id  the parameter
 * @return its description
 */
inline const ParamDesc& paramDesc(ParamIdE id) { return paramTable()[static_cast<size_t>(id)]; }
/**
 * @brief Looks a parameter up by its key.
 *
 * A binary search over a sorted index built on first use (Params.cpp): everything that reads a
 * preset goes through here, and a linear scan over the whole table cost 1.75 seconds every time
 * the map was warmed up for the library.
 * @param[in] key  the stable identifier, as presets and OSC spell it
 * @return the row, or nullptr if unknown
 */
const ParamDesc* findParam(const char* key);   ///< nullptr if unknown

constexpr int SourceSlots = 4;    ///< the self test checks this against Slots in Sources.h
constexpr int SlotFields  = 42;   ///< ... the last of them the slot's role (13.09.2026)
/**
 * @brief The parameter ids of one source slot, field by field.
 *
 * The three source slots have the same twenty-six fields, but their parameter ids are not
 * consecutive (Source 1's level and spectrum are the classic Oscillator parameters). The table
 * that maps slot and field to an id used to be written out twice -- once in the engine, once in
 * the editor -- and every field added since had to be added to both. It lives here now.
 * @param[in] slot  0 .. SourceSlots-1
 * @return SlotFields entries, or nullptr for a slot that is not one
 */
const ParamIdE* slotParamIds(int slot);   ///< SlotFields entries, or nullptr for a slot that is not one

/**
 * @brief What section a parameter belongs to, as something the compiler can check.
 *
 * The section string in
 * the table stays (the editor shows it), but everything that asks a question about a parameter --
 * is this performance state, is this part of the Cosmos layer -- asks it through this, so a
 * mistyped section name is an Unknown the self test catches instead of a predicate that silently
 * answers no forever.
 */
enum class ParamSectionE : int {
    MASTER, SOURCE1, STRANDS, SOURCE2, SOURCE3, SOURCE4, STRIKE, FOUNDATION, AIR, ENVELOPE, FILTER, Z_PLANE,   ///< the master stage, the four sources and the strands, the strike, the sub, the air, the amplitude envelope, the two filters
    EXPRESSION, SPACE, ENSEMBLE, DELAY, DELAY2, NEAR_REVERB, FAR_REVERB, BLUR, FEEDBACK, ROOM, BODY,   ///< the expression, the planes, and the effects chain from the ensemble to the body
    PATINA, COSMOS, CLOUD, CLUSTER_BRAIN, BRAIN2, AUTOPLAY, TUNING, COHERENCE, CLOCK, LFO, MOD_ENVELOPE, MORPH,   ///< the patina, the Cosmos and cloud, the two conductors and autoplay, tuning, coherence, the clock, every LFO, every envelope (the sources' own included), the morph
    MACROS, MAP, ROUTE, VECTOR, MEMORY, NEAR_SOURCE, NEAR_EVENTS, UNKNOWN   ///< the macros, the map, the route, the vector, the Memory, the near layer's two sections, and a name the table does not know
};
extern const char* const MemLineNames[3];      ///< "2", "4", "8": how many lines the Memory's pool is divided into
/**
 * @brief The section behind a section name from the table ("LFO 3", "Env 5" and "Src Env 2" fold into Lfo and
 *        ModEnvelope).
 * @param[in] sectionName  the string in ParamDesc::section; nullptr gives Unknown
 * @return the section, Unknown for a name the table of names does not hold
 */
ParamSectionE sectionOf(const char* sectionName);
/**
 * @brief By id it is a lookup, not a search.
 *
 * It used to take the parameter's section NAME and compare it
 * against a table of names, string by string -- and isPerformanceParam asks five of those
 * questions, and the modulation matrix asks it of all two hundred and ninety-three parameters
 * once per block. Measured: forty-four thousand string comparisons per block, 101 microseconds,
 * three quarters of everything the engine did between one block and the next.
 *
 * @return NumParams entries, built once, indexed by ParamId
 */
const ParamSectionE* sectionTable();   ///< NumParams entries, built once
/**
 * @brief The section of a parameter, by id (a table lookup, see sectionTable()).
 * @param[in] id  the parameter
 * @return its section
 */
inline ParamSectionE sectionOf(ParamIdE id) { return sectionTable()[static_cast<size_t>(id)]; }

constexpr int NumScaleChoices = 13;   ///< entries of ScaleNames: eleven tables, the user slot and the timbre scale
/**
 * @brief The last two are not tables.
 *
 * One is whatever Scala file was loaded; one is computed from
 * the instrument's own spectrum while it plays. Named, because five places used to spell the
 * user slot as "the last one" and appending anything after it would have quietly moved it.
 */
constexpr int UserScaleIndex   = 11;
constexpr int TimbreScaleIndex = 12;   ///< the scale computed from the spectrum (Sethares), the other one that is not a table
/**
 * @brief Names used by the Scale choice parameter; index == built-in scale index,
 *        the last entry is the user slot filled by a loaded Scala file.
 */
extern const char* const ScaleNames[NumScaleChoices];
extern const char* const RootNames[12];    ///< "C" .. "B", the twelve pitch classes for the Root choice
extern const char* const KeyMapNames[2];   ///< 0 = snap 12 keys/octave to nearest degree, 1 = consecutive degrees
extern const char* const SubOctaveNames[2];   ///< "-1", "-2"
extern const char* const SubSourceNames[3];   ///< "Root", "Difference" (ghost tone), "Lowest" (the lowest voice)
constexpr int NumSlotEnvs = 8;                ///< "Off", the six shapes the preset carries, and "Own"
extern const char* const SlotEnvNames[NumSlotEnvs];   ///< the names behind a slot's Env choice, NumSlotEnvs of them
constexpr int NumInterp = 2;                  ///< Linear, Hermite
extern const char* const InterpNames[NumInterp];      ///< "Linear", "Hermite": how a grain reads between two samples
extern const char* const RoomSourceNames[2];  ///< "Far", "Near": what the convolution room reverberates
extern const char* const AirModeNames[2];      ///< "Band" (one band-pass) or "Ghost" (resonators on the just harmonics); see also the line at StrikeWhoNames
extern const char* const EnsModeNames[3];      ///< "Chorus", "Microshift" (static detune), "Velvet" (sparse-noise decorrelation)
extern const char* const KeysFilterNames[2];   ///< "Classic" (a fixed 30 ms), "One Euro" (cutoff follows the distance still to travel)
extern const char* const TransposeNames[7];    ///< "None", "Fourth up", "Fifth up", "Octave up", "Fourth down", "Fifth down", "Octave down"
extern const char* const CloudResModeNames[2]; ///< "Band" (Mathews-Smith phasor resonators), "Comb" (tuned feedback combs)
extern const char* const CloudResNoteNames[4]; ///< which of the scale's notes the cloud's resonators sit on
extern const char* const FarModeNames[4];      ///< "Classic", "Scattering" (all-passes in the loop), "Colourless" (also flat-searched lengths), "Rotating" (also a turning lossless matrix)
extern const char* const BinauralNames[2];     ///< "Off" or "Headphones"
extern const char* const StrikeTypeNames[3];   ///< String, Wood, Metal
extern const char* const StrikeWhoNames[2];    ///< Keys, Keys + Brain     // "Band" (one band-pass) or "Ghost" (resonators on the just harmonics)
constexpr int NumStacks = 8;   ///< entries of StackNames and rows of StackRatios
extern const char* const StackNames[NumStacks];   ///< Detune, Octaves, Fifths, Major, Minor, Seventh, Harmonics, Subharmonics
extern const double StackRatios[NumStacks][6];    ///< ratio of strand 0..5 to the note (Detune = all 1)
constexpr int NumShimmerPitches = 6;   ///< entries of ShimmerPitchNames and ShimmerPitchSemitones
extern const char* const ShimmerPitchNames[NumShimmerPitches];    ///< "+12", "+7", "+5", "+19", "-12", "+24": the Cosmos shimmer's intervals
extern const float ShimmerPitchSemitones[NumShimmerPitches];      ///< the semitones behind ShimmerPitchNames

} // namespace ambient
