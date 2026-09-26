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
enum class ParamId : int {
    /// Master
    MasterGain,   ///< master output gain in dB
    /// Source 1: Type chooses the additive strand bank (the classic Oscillator, default) or any of
    /// the slot types below; then the bank's own spectrum parameters
    Src1Type,   ///< which source type Source 1 is (SourceType); Additive is the strand bank
    OscLevel, Partials, Tilt, Brightness, OddEven, Inharmonic, Shimmer, ShimmerRate,   ///< the bank's level, partial count, spectral tilt, brightness, odd/even balance, inharmonic stretch, shimmer depth and rate (Hz)
    Unison, Detune, Drift, DriftRate, Spread, Bloom, BloomTime,   ///< strands, their detune (ct), pitch drift (ct) and its rate (Hz), stereo spread, and Bloom with its time (s)
    /// Stack: strands at pure ratios (one key = one just chord) instead of detuned copies;
    /// Rate Wander: every voice's drift and shimmer rates themselves wander (nested LFO)
    Stack, RateWander,   ///< Stack: a kStackNames choice; Rate Wander: 0 .. 1
    /// Source 1's slot fields, for the non-additive types (level is OscLevel, spectrum the eight above)
    Src1Octave, Src1Ratio, Src1Pan, Src1Table, Src1Position, Src1PosDrift,   ///< Source 1's slot fields: octave, just ratio, pan, table, position and its drift
    Src1FmRatio, Src1FmIndex, Src1Grain, Src1Density, Src1DensitySync, Src1Follow, Src1Grains, Src1Spread, Src1Noise, Src1NoiseQ, Src1Drift,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q, pitch drift (ct)
    /// Source 2 / Source 3: the same slot, laid out identically (the engine reads them by offset):
    /// 17 slot fields, then the seven of the slot's own additive bank
    Src2Type, Src2Level, Src2Octave, Src2Ratio, Src2Pan, Src2Table, Src2Position, Src2PosDrift,   ///< Source 2: type, level, octave, ratio, pan, table, position and its drift
    Src2FmRatio, Src2FmIndex, Src2Grain, Src2Density, Src2DensitySync, Src2Follow, Src2Grains, Src2Spread, Src2Noise, Src2NoiseQ,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q
    Src2Partials, Src2Tilt, Src2Bright, Src2OddEven, Src2Inharm, Src2Shimmer, Src2ShimmerRate, Src2Drift,   ///< Source 2's own additive bank: partials, tilt, brightness, odd/even, inharmonic, shimmer and its rate (Hz), pitch drift (ct)
    Src3Type, Src3Level, Src3Octave, Src3Ratio, Src3Pan, Src3Table, Src3Position, Src3PosDrift,   ///< Source 3: type, level, octave, ratio, pan, table, position and its drift
    Src3FmRatio, Src3FmIndex, Src3Grain, Src3Density, Src3DensitySync, Src3Follow, Src3Grains, Src3Spread, Src3Noise, Src3NoiseQ,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q
    Src3Partials, Src3Tilt, Src3Bright, Src3OddEven, Src3Inharm, Src3Shimmer, Src3ShimmerRate, Src3Drift,   ///< Source 3's own additive bank: partials, tilt, brightness, odd/even, inharmonic, shimmer and its rate (Hz), pitch drift (ct)
    /// Foundation: a sub voice that follows the brain's root or the ghost tone (difference
    /// tone of the two lowest sounding voices); Pad Low Cut keeps the pads out of its register
    SubLevel, SubOctave, SubGlide, SubBinaural, SubTone, SubSource, PadLowCut,   ///< the Foundation: level, octave (-1/-2), glide (s), binaural beat (Hz), tone, source (root, difference, lowest), and the pads' low cut (Hz)
    /// Strike: a short plucked or struck impulse (Karplus-Strong) at note-on, on the near plane
    StrikeLevel, StrikeType, StrikeDecay, StrikeDamp, StrikeWho,   ///< strike level, type (String, Wood, Metal), decay (s), damping, and who fires it (keys, or keys and brain)
    /// Air (filtered-noise breath layer per voice)
    Air, AirColor, AirQ,   ///< Air level, colour (a multiple of f0) and Q
    /// Amplitude envelope
    Attack, Decay, Sustain, Release,   ///< the amplitude envelope: attack, decay and release in seconds, sustain 0 .. 1
    /// Filter: one of nine models (Filter.h) behind the same knobs; Drive saturates ahead of it
    FilterOn, FilterModel, Cutoff, Resonance, FilterEnv, FilterDrift, KeyTrack, FilterDrive,   ///< filter switch, model, cutoff (Hz), resonance, envelope amount, drift, key tracking, drive
    /// Z-plane filter: four frames on a square, the point (X, Y) interpolates their poles and wanders
    /// Route: with both filters on, the z-plane follows the voice filter (Series) or both hear the
    /// dry signal and Mix balances them (Parallel); Replace is the z-plane alone
    /// ZZ is the cube's third axis (Transform); 0 is the filter as it was before it existed
    ZMode, ZRoute, ZShape, ZX, ZY, ZZ, ZRate, ZDepth, ZResonance, ZKeyTrack, ZMix,   ///< z-plane mode, route, shape, the point's X, Y and Transform, its wander rate (Hz) and depth, resonance, key tracking, mix
    ZDecay, ZDamp,          ///< Modal mode: how long the modes ring, and how much shorter the high ones
    /// Space: front-to-back planes, per-voice interaural time difference, hour-scale arc,
    /// presence bell for the near plane, slow breathing of every voice's distance
    Depth, KeysDepth, PanDrift, Itd, ArcAmount, ArcPeriod, ArcSync, Presence, Breath, BreathRate,   ///< the planes' depth, the keys' depth, pan drift, Time Width (the ITD), the arc's amount, period (min) and sync, presence (dB), breath and its rate (Hz)
    /// Phase Width: two all-pass pairs per voice, drifting in opposite directions on the two ears,
    /// so the room seems to change size rather than the sound to move; Doppler: the breathing
    /// distance bends the pitch a little as a voice approaches or recedes
    PhaseWidth, PhaseRate, Doppler, Externalise,   ///< phase width and its rate (Hz), doppler, externalisation
    /// Expression: what a key's pressure, its sideways slide and its own pitch bend do.
    /// With an MPE controller each finger has all three; a plain keyboard shares them.
    PressDistance, PressBright, PressLevel, SlideCutoff, SlideZ, BendRange, MpeOn,   ///< pressure to nearness, brightness and level; slide to the filter (oct) and the z-plane; bend range (st); the MPE switch
    /// Ensemble
    EnsembleMix, EnsembleDepth, EnsembleRate, EnsembleSync,   ///< ensemble mix, depth, rate (Hz) and sync
    /// Stereo delay (asymmetric L/R)
    DelayTimeL, DelayTimeR, DelaySyncL, DelaySyncR, DelayFeedback, DelayCross, DelayDamp, DelayAbsorb, DelayMix, DelayToFar, DelayDuck,   ///< delay 1: left and right times (s) and their syncs, feedback, cross, damping, absorb, mix, send to far, ducking
    /// Second stereo delay, in series after the first
    Delay2TimeL, Delay2TimeR, Delay2SyncL, Delay2SyncR, Delay2Feedback, Delay2Cross, Delay2Damp, Delay2Absorb, Delay2Mix, Delay2ToFar,   ///< delay 2: left and right times (s) and their syncs, feedback, cross, damping, absorb, mix, send to far
    /// Near reverb (foreground room)
    NearMix, NearDecay, NearDamp,   ///< near reverb mix, decay (s), damping
    /// Blur: a spectral smear on the near bus itself, so an attack is wiped into texture
    BlurMix, BlurSmear,   ///< blur amount and smear
    /// Far reverb (the infinite background)
    FarLevel, FarSize, FarDecay, FarDamp, FarPreDelay, FarAsym, FarHighcut, FarFreeze, FarRotate, FarUnmask,   ///< far reverb level, size, decay (s), damping, pre-delay (ms), asymmetry, tail cut (Hz), freeze, rotate, unmask
    /// Diffusion: modulated all-passes in front of the reverb, so the tail arrives instead
    /// of starting; and the sympathetic coupling of the voices through each other's filters
    FarDiffuse,   ///< the far reverb's diffusion
    /// Feedback: the mixed output (before the master) returns, low-passed and saturated,
    /// into the near bus before the filters and effects, and/or as phase modulation of every
    /// partial. Throttled by the output level so it hisses and holds instead of running away.
    FeedbackBus, FeedbackFm, FeedbackTone, FeedbackDrive,   ///< feedback into the bus and into the pitch (FM), its tone (Hz) and drive
    /// Room: convolution reverb with a loaded (or generated) impulse, in parallel on the far plane
    RoomLevel, RoomSource, RoomPreDelay, RoomHighcut, RoomMorph,   ///< room level, source (far or near), pre-delay (ms), tail cut (Hz), morph A/B
    /// Cosmos: science-fiction / deep-space path (send from the near bus)
    CosmosSend, CosmosShift, CosmosShiftDrift, CosmosRes, CosmosResPitch, CosmosResFeedback,   ///< Cosmos send, shift (Hz) and its drift, resonator amount, pitch (x root) and feedback
    CosmosVowel, CosmosVowelRate, CosmosNebula, CosmosSmear, CosmosShimmer, CosmosShimmerPitch,   ///< vowel and its rate (Hz), nebula, smear, shimmer and its pitch
    CosmosReturn, CosmosToFar,   ///< the Cosmos return level and its send to far
    /// Granular cloud on the far plane
    CloudSend, CloudDensity, CloudSync, CloudSize, CloudPitch, CloudSpray, CloudLevel,   ///< cloud send, density (/s) and sync, grain size (ms), pitch, spray (s), level
    /// Body: a bank of modes under everything -- the soundboard the pad sits on (Body.h)
    BodyLevel, BodyMaterial, BodyPitch, BodyDecay, BodyTone, BodySpread,   ///< body level, material, pitch (x root), decay (s), tone, spread
    /// Patina: the master's age -- tape wow, lost highs, a noise floor, gentle saturation
    PatinaAmount, PatinaWow, PatinaHiss, PatinaAge,   ///< patina amount, wow, hiss, age
    /// Master tilt: one broad see-saw around a pivot -- the move an ambient mix asks for
    /// more than any other, and the one thing the master stage did not have
    Tilt2, TiltPivot,   ///< the master tilt (dB) and its pivot (Hz)
    /// Mid/side master stage
    BassMono, SideAir, Width, MonoGuard,   ///< bass mono below (Hz), side air (dB), width, mono safe
    /// Cluster brain (generative sleep-concert mode)
    BrainOn, BrainDensity, BrainRate, BrainSync, BrainHoldMin, BrainHoldMax,   ///< brain on, density, event rate (s) and sync, hold min and max (s)
    BrainLow, BrainHigh, BrainConsonance, BrainWander, BrainQuantize,   ///< the brain's register (lowest, highest), consonance, wander, quantize
    /// Autoplay: instead of notes coming and going, the cluster stays full and one voice at a
    /// time is exchanged, so the chord travels. Step is a trigger, not a level.
    AutoMode, AutoRate, AutoSync, AutoLead, AutoTension, AutoRootMove, AutoStep,   ///< autoplay mode, every (s) and sync, voice leading (st), tension, root move, the step trigger
    /// A second conductor for the background: its own register, pace and plane, on the
    /// first one's root (plus an interval), so the two play a slow counterpoint
    Brain2On, Brain2Density, Brain2Rate, Brain2HoldMin, Brain2HoldMax,   ///< brain 2 on, density, event rate (s), hold min and max (s)
    Brain2Low, Brain2High, Brain2Depth, Brain2Interval, Brain2Consonance,   ///< brain 2's register (lowest, highest), plane, interval (st), consonance
    /// Tuning
    Scale, KeyMap, RootNote, RefPitch, Seed, Hold,   ///< scale (kScaleNames), key mapping, root note, A4 (Hz), seed, hold
    /// Purity blends every note between 12-TET (0) and the chosen scale (1) in the log domain; Drift
    /// lets that blend wander so the beating locks in and loosens over minutes; Freeze holds every
    /// voice's spectrum and pitch still (drifts, shimmer, bloom stop moving)
    TunePurity, TuneDrift, TuneDriftRate, Freeze,   ///< purity, its drift and the drift's rate (Hz), freeze
    /// Tide: the whole instrument's pitch leans by a few cents over many minutes
    Tide, TidePeriod,   ///< the tide's depth (ct) and period (min)
    /// Ghost: the Air noise through a bank of sharp resonators on the note's just harmonics
    AirMode,   ///< Air mode: Band or Ghost (kAirModeNames)
    /// Portamento for keys: a new key glides from the last one; Gravity slows the glide near
    /// consonant ratios to the root, so the slide "clicks into" harmonic nodes on the way
    Portamento, PortaGravity,   ///< portamento time (s) and gravity
    /// Tape in the feedback loop: asymmetric saturation, wow and flutter, level-dependent noise floor
    FeedbackTape,   ///< tape in the feedback loop
    /// Coherence: four slow Kuramoto oscillators, coupled by Coherence, modulating brightness,
    /// depth, pan drift and the z-plane point by Depth
    Coherence, CoherenceDepth, CoherenceRate, Sympathy,   ///< coherence coupling, depth, rate (x), sympathy
    /// Modulation: eight free LFOs and six multi-segment envelopes. Their shapes and the matrix
    /// rows are data, not parameters (see Modulation.h); what sits here is what a host automates.
    Lfo1Shape, Lfo1Rate, Lfo1Phase, Lfo1Depth, Lfo1Mode, Lfo1Table, Lfo1Sync,   ///< LFO 1: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo2Shape, Lfo2Rate, Lfo2Phase, Lfo2Depth, Lfo2Mode, Lfo2Table, Lfo2Sync,   ///< LFO 2: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo3Shape, Lfo3Rate, Lfo3Phase, Lfo3Depth, Lfo3Mode, Lfo3Table, Lfo3Sync,   ///< LFO 3: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo4Shape, Lfo4Rate, Lfo4Phase, Lfo4Depth, Lfo4Mode, Lfo4Table, Lfo4Sync,   ///< LFO 4: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo5Shape, Lfo5Rate, Lfo5Phase, Lfo5Depth, Lfo5Mode, Lfo5Table, Lfo5Sync,   ///< LFO 5: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo6Shape, Lfo6Rate, Lfo6Phase, Lfo6Depth, Lfo6Mode, Lfo6Table, Lfo6Sync,   ///< LFO 6: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo7Shape, Lfo7Rate, Lfo7Phase, Lfo7Depth, Lfo7Mode, Lfo7Table, Lfo7Sync,   ///< LFO 7: shape, rate (Hz), phase, depth, mode, table, sync
    Lfo8Shape, Lfo8Rate, Lfo8Phase, Lfo8Depth, Lfo8Mode, Lfo8Table, Lfo8Sync,   ///< LFO 8: shape, rate (Hz), phase, depth, mode, table, sync
    Env1Mode, Env1Time, Env1Depth, Env1Sync,   ///< Env 1: mode, time (x), depth, sync
    Env2Mode, Env2Time, Env2Depth, Env2Sync,   ///< Env 2: mode, time (x), depth, sync
    Env3Mode, Env3Time, Env3Depth, Env3Sync,   ///< Env 3: mode, time (x), depth, sync
    Env4Mode, Env4Time, Env4Depth, Env4Sync,   ///< Env 4: mode, time (x), depth, sync
    Env5Mode, Env5Time, Env5Depth, Env5Sync,   ///< Env 5: mode, time (x), depth, sync
    Env6Mode, Env6Time, Env6Depth, Env6Sync,   ///< Env 6: mode, time (x), depth, sync
    /// Morph between two stored full presets (A/B); never part of a preset itself
    MorphActive, MorphPos, MorphGlide,   ///< morph active, position, glide (s)
    /// Macros: eight performance controls routed through the gesture layer (Custom0..7);
    /// not part of presets. Named in Rich's vocabulary, not the engine's.
    MacroA, MacroB, MacroC, MacroD, MacroE, MacroF, MacroG, MacroH,   ///< the eight macros: Space, Alien, Motion, Bloom, Density, Distance, Evolution, Air
    /// Inertia: every float parameter glides to its value with this time constant (the analogue
    /// slew), so even a knob torn open arrives slowly; performance state, not in presets
    Inertia,   ///< the analogue slew, in seconds
    /// Preset map: a cursor in the plane of all presets blends its neighbours (PresetMap.h);
    /// performance state like the morph, never part of a preset
    MapActive, MapX, MapY, MapRadius,   ///< map active, the cursor's X and Y, the blend radius
    /// Route: the engine walks a route of waypoints over the map (Route.h); performance state
    RouteActive, RouteSpeed, RouteLoop,   ///< route play, speed (x), loop
    /// Clock (Clock.h): where the tempo comes from, the internal tempo, and whether the internal
    /// clock runs; performance state -- the tempo belongs to the session, not to a preset
    ClockSource, Tempo, ClockRun,   ///< clock source, tempo (bpm), run
    /// Appended, never inserted: a parameter's position in this enum is its automation slot in
    /// the host, and moving one would move somebody's automation lane with it.
    ///
    /// The other end of the filter funnel on the three reverb returns. A tail with its low mids
    /// still in it sits in front of the music instead of behind it.
    FarLowcut, NearLowcut, RoomLowcut,   ///< the low cuts (Hz) on the far, near and room returns
    /// A steep high-pass on the finished output for the energy below hearing: it does nothing for
    /// the music and everything for the amplifier. Off by default, because the Foundation reaches
    /// lower than the frequency a mastering engineer would cut at.
    Subsonic,   ///< the subsonic high-pass corner (Hz), off by default
    /// Vector: a point in a square whose corners are the three source slots and the three of them
    /// together, after the Prophet VS and the Wavestation. Amount 0 leaves every slot's level
    /// exactly as it is set.
    VecAmount, VecX, VecY, VecWander, VecRate,   ///< vector amount, X and Y, wander and its rate (Hz)
    /// Source 4: the same slot as 2 and 3, laid out identically, so the Vector's four corners are
    /// four sources rather than three and the three together. Appended for the same reason as
    /// everything above it.
    Src4Type, Src4Level, Src4Octave, Src4Ratio, Src4Pan, Src4Table, Src4Position, Src4PosDrift,   ///< Source 4: type, level, octave, ratio, pan, table, position and its drift
    Src4FmRatio, Src4FmIndex, Src4Grain, Src4Density, Src4DensitySync, Src4Follow, Src4Grains, Src4Spread, Src4Noise, Src4NoiseQ,   ///< FM ratio and index, grain length (ms), density (/s) and its sync, pitch follow, grain count, spread, noise colour and Q
    Src4Partials, Src4Tilt, Src4Bright, Src4OddEven, Src4Inharm, Src4Shimmer, Src4ShimmerRate, Src4Drift,   ///< Source 4's own additive bank: partials, tilt, brightness, odd/even, inharmonic, shimmer and its rate (Hz), pitch drift (ct)
    /// The Stretch type's two settings, one pair per slot, fields 26 and 27 of the slot.
    Src1Stretch, Src1Xfade, Src2Stretch, Src2Xfade, Src3Stretch, Src3Xfade, Src4Stretch, Src4Xfade,   ///< each slot's stretch factor (x) and loop fade
    /// Four things a dark-ambient mixing desk does that this instrument could not, each off at its
    /// default so nothing that exists sounds different: the background narrowed as it goes back,
    /// a static micro-detune instead of the chorus, a band delayed to one side, and a wavefolder.
    FarWidth, EnsMode, Haas, HaasTime, FilterFold,   ///< far reverb width, ensemble mode, Haas amount and time (ms), the wavefolder
    /// What the literature after the classics asks for: a stretched octave (Ward 1954, Terhardt),
    /// a reverb with scattering in its loop (Schlecht and Habets 2020), masking that spreads
    /// upward (Zwicker), and a binaural mode that follows the head. Neutral at every default.
    TuneStretch, FarMode, FarUnmaskSpread, Binaural,   ///< octave stretch (ct), far reverb mode, unmask spread, binaural mode
    /// The conductor judging intervals by the spectrum it actually plays (Sethares), not by
    /// the ratio alone. 0 = the ratio score it always had.
    BrainTimbre, BrainSpacing,   ///< the conductor's timbre weighting and its spacing
    /// The room's early reflections, from its geometry (see Effects.h): off at Level 0.
    EarlyLevel, EarlySize, EarlyAbsorb, EarlyWidth,   ///< early reflections: level, room size (m), absorption, width
    /// The Bow type's two: how hard the bow presses and how fast it travels. Fields 28 and 29
    /// of every slot.
    Src1BowForce, Src1BowSpeed, Src2BowForce, Src2BowSpeed, Src3BowForce, Src3BowSpeed, Src4BowForce, Src4BowSpeed,   ///< each slot's bow force and speed
    Src1SpecRate, Src1SpecBreath, Src2SpecRate, Src2SpecBreath, Src3SpecRate, Src3SpecBreath, Src4SpecRate, Src4SpecBreath,   ///< each slot's spectral read rate (x) and breath
    BrainHarmonic, BrainKey, BrainEven, BrainSmooth,   ///< conductor: harmonic weighting, key, even, smooth
    TuneGuard, TuneMatch, BrainBlend, ArcHarmony,   ///< purity guard, timbre match, brain blend, arc harmony
    ElevNear, ElevFar, DepthLaw, Envelop,   ///< the elevation of the near and far planes, depth law, envelop
    ArcClock,   ///< the arc on the clock
    BrainCascade, BrainSurprise, BrainHomeostat, TuneAdapt,   ///< conductor cascade, surprise, homeostat; adaptive purity
    FarComod, NearIld,   ///< far reverb comodulation; the near field's level difference
    Src1Transport, Src2Transport, Src3Transport, Src4Transport, SubPulse, FeedbackBias,   ///< each slot's Transport (the Harmonic morph by optimal transport); the sub's pulse; feedback bias
    LeniaRate, LeniaGrowth,   ///< Lenia rate (Hz) and growth (coherence)
    BrainDejaVu, BrainLoop, BrainSpread, BrainBias, ChaosPeriod, KeysFilter, Transpose, PartialSpread,   ///< conductor deja vu, loop, spread, bias; chaos period (s); MPE filter; transpose; Partial Spread
    StrikeChance, StrikeCluster, CosmosSwell,   ///< strike chance and cluster weighting; Cosmos swell
    /// Each slot's own entrance. Until these existed the four sources of a preset all started
    /// together on the one amplitude envelope in the Envelope section, so a preset with a
    /// wavetable and a texture was a single chord struck twice at once, however different the two
    /// materials were. Delay holds the slot silent after the note, Rise fades it in, and Env picks
    /// one of the six shapes as the slot's own contour instead. All three default to off, so a
    /// preset that says nothing about them sounds exactly as it did.
    Src1Delay, Src1Rise, Src1Env,   ///< Source 1's entrance: delay (s), rise (s), envelope choice (kSlotEnvNames)
    Src2Delay, Src2Rise, Src2Env,   ///< Source 2's entrance: delay (s), rise (s), envelope choice
    Src3Delay, Src3Rise, Src3Env,   ///< Source 3's entrance: delay (s), rise (s), envelope choice
    Src4Delay, Src4Rise, Src4Env,   ///< Source 4's entrance: delay (s), rise (s), envelope choice
    /// How a grain reads between two samples of its clip. Linear is what the instrument always
    /// did; Hermite costs two more reads a sample and is offered rather than imposed, because
    /// which of them is right is a matter of taste in an instrument built to be soft.
    Src1Interp, Src2Interp, Src3Interp, Src4Interp,   ///< each slot's interpolation: Linear or Hermite (kInterpNames)
    /// Unison in a slot: the main oscillator has had detuned strands with their own place in the
    /// field since the beginning, a slot had one mono voice. Defaults to one copy, which is that.
    /// Root: how much of a fundamental a wavetable is given when its own has none.
    Src1Root, Src2Root, Src3Root, Src4Root,   ///< each slot's fundamental share (Harmonic)
    Src1Unison, Src1UniDetune, Src1UniWidth,   ///< Source 1's unison copies, their detune (ct) and width
    Src2Unison, Src2UniDetune, Src2UniWidth,   ///< Source 2's unison copies, their detune (ct) and width
    Src3Unison, Src3UniDetune, Src3UniWidth,   ///< Source 3's unison copies, their detune (ct) and width
    Src4Unison, Src4UniDetune, Src4UniWidth,   ///< Source 4's unison copies, their detune (ct) and width
    /// The cloud as a granular feedback instrument (12.09.2026): its loop, the scatter over the
    /// scale's intervals, its flocks and its resonators on the scale's notes. All off by default,
    /// so a preset that says nothing about them has the cloud it always had.
    CloudFeedback, CloudTone, CloudTranspose, CloudScatter, CloudSwarm,   ///< cloud feedback, tone (Hz), transpose, scatter, swarm
    CloudResonance, CloudResMode, CloudResNotes, CloudResDecay,   ///< cloud resonance, its mode, its notes and its ring time (s)
    /// The Memory (12.09.2026): a drifting sound memory beside the Cosmos -- a send, two returns, the
    /// lines and their exchange, how long and how darkly it keeps, the recalled grains, and the tape.
    MemSend, MemReturn, MemToFar, MemLines, MemSize, MemBlur, MemDrift, MemHold, MemAge,   ///< Memory send, return, to far, lines, size (s), blur, drift, hold, age
    MemRenew, MemDrive, MemRecall, MemSeek, MemGrain, MemFreeze, MemReverse, MemHalf, MemErase,   ///< Memory renew, drive, recall, seek, grain (ms), freeze, reverse, half speed, erase
    /// The shifter in the spectrum (12.09.2026): which shifter the shimmer's loop uses, and the
    /// cloud's loop transposed on every pass.
    CosmosShimmerMode, CloudShift,   ///< the shimmer's shifter (kShimmerModeNames); the cloud loop's shift (kCloudShiftNames)
    /// Each source's own envelope (11.09.2026). A slot's Env could only borrow one of the six
    /// modulation envelopes, so every source that entered on a shape left one envelope fewer for
    /// modulating anything else. Env = Own reads the slot's own shape with these four, which do
    /// what an Env section's do: Mode, Time, Depth and Sync, four per slot in that order.
    Src1EnvMode, Src1EnvTime, Src1EnvDepth, Src1EnvSync,   ///< Source 1's own envelope: mode, time (x), depth, sync
    Src2EnvMode, Src2EnvTime, Src2EnvDepth, Src2EnvSync,   ///< Source 2's own envelope: mode, time (x), depth, sync
    Src3EnvMode, Src3EnvTime, Src3EnvDepth, Src3EnvSync,   ///< Source 3's own envelope: mode, time (x), depth, sync
    Src4EnvMode, Src4EnvTime, Src4EnvDepth, Src4EnvSync,   ///< Source 4's own envelope: mode, time (x), depth, sync
    /// What a generator of this music needs and the conductor had no way to say (12.09.2026, from
    /// Rene's rule book for a self-playing ambient generator): the register roles and the spacing
    /// that follows from them, the colour of the intervals, a clock that breathes and guards what
    /// it may not do, and the root's walk over the hour. Every one of them does nothing at its
    /// default, so every preset written before them plays exactly as it did.
    ///
    /// Layers and register: the pyramid (one voice at the bottom, most in the body, few on top),
    /// the bass held longer than what moves above it, the top softer, spacing sharpened with depth,
    /// the floor under which no third is chosen, and the leading note kept off the root.
    BrainLayers, BrainBassHold, BrainTopSoft, BrainLowSpacing, BrainThirdFloor, BrainLeading,   ///< layers, bass hold (x), top soft, low spacing, third floor, leading
    /// Interval colour: thirds sought or avoided, seconds likewise (above the third floor only),
    /// the seventh lifted -- as 7/4 in a just scale, the most characteristic interval this music
    /// has -- and the chance that a root change swaps one degree of the mode instead.
    BrainThirds, BrainSeconds, BrainSeventh, BrainDegreeSwap,   ///< thirds, seconds, seventh, degree swap
    /// Time: the mean event rate breathing on its own slow curve, the overlap that keeps a voice
    /// sounding while the one it replaces lets go, the thirty-millisecond rule as a guard, the gap
    /// between two note-offs, the silence a pitch must keep before it may sound again, density
    /// changes walked one voice at a time, and the planned silence at a section's border.
    BrainRateBreath, BrainBreathPeriod, BrainOverlap, BrainOnsetGuard, BrainReleaseGap,   ///< rate breath, breath period (min), overlap (s), onset guard, release gap (s)
    BrainRetrigger, BrainDensitySlew, BrainSilence, BrainSilenceLen,   ///< retrigger (s), density slew (min), silence chance and its length (s)
    /// Root and form: where a new root may come from, which way it leans, the pivot window in which
    /// old and new root sound together, the pull back to the root the night began on, and the
    /// memory that forbids a chord the hour has already had.
    BrainRootSteps, BrainRootDown, BrainPivot, BrainHome, BrainHomeTime, BrainMemory,   ///< root steps (kRootStepNames), root down, pivot (s), home, home time (min), memory (min)
    /// The background conductor on a clock that never lines up with the foreground's; the tuning
    /// holding what already sounds when the root moves; and a ceiling on how fast a beat may beat.
    /// (Brain2 Interval was asked for too, and already existed: brain2_interval, in semitones.)
    Brain2Golden, TuneHoldSounding, BeatCeiling,   ///< brain 2's golden clock; the tuning holding sounding notes; the beat ceiling (Hz)
    /// Three that are true outside the conductor: a quiet note entering slower than a loud one, the
    /// plane a voice stands in taken from its role rather than from the draw, and the strands of a
    /// low note detuned less than those of a high one, so warmth does not become wobble.
    EnvVelAttack, LayerDepth, StrandLowDetune,   ///< velocity to attack, layer depth, the strands' low detune
    /// How much of the granular cloud comes back on the NEAR plane instead of the far one. The cloud
    /// has always returned to the far bus alone -- a deliberate choice, written down in the concept
    /// paper: "it exists in the background and the far reverb smears it". At 0 that is exactly what
    /// still happens, so no preset changes; above it the cloud can be brought forward, which is what
    /// a listener expects of a send they have turned all the way up.
    CloudToNear,   ///< the cloud's return on the near plane
    /// The near plane (13.09.2026). A role per slot -- which of the voice's notes it sounds in:
    /// all, the lowest, an inner one, the highest -- which belongs to the sound preset. And the
    /// near layer, which does not: a source of its own (Near Source: a fifth slot with its own
    /// type, envelope, filter and strike, rendered only by the events) and the Near Events section
    /// (what the instrument plays close to the ear, on a clock of its own). The layer is a bank
    /// like the Cosmos, kept across sound presets, so a night's foreground can be chosen once and
    /// the backgrounds changed under it. Level 0 is off, and every preset ever written has it there.
    Src1Role, Src2Role, Src3Role, Src4Role,   ///< each slot's role (SlotRole)
    ForeType, ForeOctave, ForeRatio, ForePosition, ForePosDrift, ForeDensity, ForeFollow, ForeBright,   ///< the near source: type, octave, ratio, position and its drift, density (/s), pitch follow, brightness
    ForeForce, ForeSpeed, ForeNoise, ForeNoiseQ, ForeFmRatio, ForeFmIndex, ForePartials, ForeTilt,   ///< force, speed, noise colour and Q, FM ratio and index, partials, tilt
    ForeInharm, ForeDrift, ForeTable, ForeAttack, ForeDecay, ForeSustain, ForeRelease,   ///< inharmonic, drift (ct), table, attack, decay, sustain, release (s)
    ForeCutoff, ForeResonance, ForeFilterModel, ForeFilterEnv, ForeStrike, ForeStrikeType, ForeStrikeDecay, ForeStrikeDamp,   ///< cutoff (Hz), resonance, filter model and envelope amount, strike level, type, decay (s) and damp
    ForeLevel, ForeKind, ForeRate, ForeChance, ForeCluster, ForeLength,   ///< the near events: level, kind (kNearKindNames), every (s), chance, cluster weighting, length (s)
    ForePitch, ForeSpread, ForeApproach, ForeProximity, ForeHold, ForeGlide,   ///< pitch rule (kNearPitchNames), spread, approach, proximity, hold brain, glide (s)
    ForeSteps, ForeStep, ForeStepSync, ForeMutation, ForeScatter, ForeBloom,   ///< sequence steps, step time (s) and its sync, mutation, scatter, bloom
    /// Where an event sits between the planes (0 at the ear, 1 on the horizon; Approach arrives
    /// from the horizon to it, or, negative, leaves from it), and how much of it goes straight to
    /// the output past every reverb and delay: a rattle twenty centimetres from the nose, a click
    /// that no room may soften.
    ForeDistance, ForeDry,   ///< the event's plane and its dry share
    /// Whether a sound preset of a pack brings its own foreground (the artist's table, NearAuto.inc).
    ForeAuto,   ///< whether a pack's sound preset brings its own foreground
    /// How long the far reverb takes to come back after the foreground has ducked it. It came back
    /// in 1.2 seconds since the unmask was built, and that stays the default; a foreground that
    /// speaks and then lets the horizon return over five to ten seconds makes the return itself a
    /// gesture, which is what the near events want.
    FarUnmaskReturn,   ///< seconds the far reverb takes to come back after the foreground ducked it
    /// The foreground's own sends, per event: a share of the event's voice into the second delay
    /// and into the Cosmos, on top of what the near bus gives them. A send on a desk, not a
    /// routing switch -- the event still sounds where Distance and Dry put it, and this is what is
    /// thrown into the long chain or into the deep space beside it.
    ForeDelay2, ForeCosmos,   ///< the event's sends into the second delay and into the Cosmos
    /// The foreground's own gain, in dB, after its source and before everything it passes through
    /// (14.09.2026). Level is the source's level, and for several near sources it is not only a
    /// level -- a jet, a bow, a clip each read it their own way, one clamps it at two -- so it cannot
    /// carry a calibration of twenty decibels. This does, and the bank's presets carry a measured
    /// one each: Tools/library/near_loudness.py puts every preset's loudest moment at the same
    /// distance from the background, where they had lain eighteen decibels apart.
    ForeGain,   ///< the foreground's own gain in dB
    /// The production guide's depth model, built in (25.09.2026). Until then a note at the
    /// horizon was 6 dB quieter than one at the ear, its far send left at the same instant as its
    /// direct sound, and its strands fanned out as wide wherever it stood -- three cues that said
    /// "near" while the low-pass and the wet share said "far". Range is the level a source loses
    /// between the ear and the horizon, in dB; Gap the pre-delay a source at the ear gets before
    /// the far reverb, shrinking to nothing on the horizon; Near Width how wide the strands fan
    /// out at the ear as a share of their width on the horizon.
    DepthRange, DepthPreDelay, DepthWidth,   ///< level drop over the depth (dB); the near source's gap before the far reverb (ms); the strands' width at the ear (0 .. 1 of Spread)
    /// A high-pass in front of every reverb -- the near room, the far hall and the convolution
    /// room -- so that nothing under it is ever thrown into a tail (the guide's first rule of
    /// reverb: 150 to 300 Hz, before the send, not on the return). The Low Cut knobs of the three
    /// reverbs stay what they were, filters on the tail itself.
    SendLowcut,   ///< Hz, the second-order high-pass in front of every reverb's input
    /// The Foundation's harmonics and its beat. Harmonics adds the second and third partial of the
    /// sub at -24 and -27 dB at full, so the sub is still heard as a note on a small speaker where
    /// its fundamental is not reproduced (the residue pitch); Beat sums a second sine a fraction of
    /// a hertz above the first, in both ears alike, so the level breathes over two to ten seconds
    /// -- the slow swell of a Lustmord sub, which the Binaural offset between the ears is not.
    SubHarmonics, SubBeat,   ///< the sub's 2nd and 3rd harmonics (0 .. 1); a second sine this many Hz above the sub, beating in mono
    /// The rest of the production guide's rooms and low end (25.09.2026, the second round). To Far
    /// sends the convolution room's return into the far hall, the guide's serial rooms: the main
    /// room's output feeds the room behind it, so the horizon sounds like the same place going on
    /// rather than a second, foreign one. Mid Low Cut high-passes the middle of the far hall's
    /// return and leaves its sides, so the centre stays free for the near plane. Ceiling is the
    /// Foundation's own limiter, in dBFS at the output: a beating sub swings its peaks by up to
    /// six decibels, and without a limiter of its own it pushed the whole mix into the clipper.
    RoomToFar, FarMidLowcut, SubCeiling,   ///< the room's return into the far hall (0 .. 0.5); the far return's mid high-pass (Hz, 20 = off); the sub's ceiling (dBFS)
    /// The review of the conductor against ambient harmony (25.09.2026). Root Targets: which
    /// intervals the root's next step aims at, and how often (kRootTargetNames) -- the six the
    /// conductor always drew alike, or the modal weighting with the whole tone the genre moves by.
    /// Utonal: Harmonic also hears undertone sets, so a dark family can listen for rootedness
    /// without being pulled into major. Series: the Harmonic Cloud, candidates drawn to the
    /// harmonics of the root's fundamental rather than to the scale's degrees alone.
    BrainRootTargets, BrainUtonal, BrainSeries,   ///< root targets (kRootTargetNames); utonal (0 .. 1); series (0 .. 1)
    Count   ///< one past the last: kNumParams
};

extern const char* const kNearKindNames[3];     ///< "Note", "Phrase", "Sequence"
extern const char* const kNearPitchNames[5];    ///< "Consonant", "Highest", "Lowest", "Root", "Cluster"

extern const char* const kRootStepNames[4];     ///< "Any", "Fifths", "Diatonic", "Falling"
extern const char* const kRootTargetNames[4];   ///< "Classic", "Modal", "Mediant", "Phrygian": the weightings of BrainParams::rootTargets
extern const char* const kShimmerModeNames[2];  ///< "Spectral" (phase-locked peak shifting), "Grain" (the two-head shifter)
extern const char* const kCloudShiftNames[7];   ///< "Off", "+12", "+7", "+5", "-5", "-7", "-12"
extern const float kCloudShiftSemitones[7];     ///< the semitones behind kCloudShiftNames: 0, 12, 7, 5, -5, -7, -12

constexpr int kNumParams = static_cast<int>(ParamId::Count);   ///< how many parameters there are; the size of every per-parameter table

/** @brief How a parameter's value is to be read: a continuous float, a stepped integer, a switch, or one of a list of names. */
enum class ParamKind {
    Float,   ///< @brief continuous, with a skew
    Int,     ///< @brief whole numbers, linear
    Bool,    ///< @brief 0 or 1
    Choice   ///< @brief an index into `choices`
};

/** @brief One row of the parameter table: everything a host, an editor or a preset needs to know about a parameter. */
struct ParamDesc {
    ParamId     id;       ///< the parameter; rows are ordered by it, so paramTable()[id] is this row
    const char* key;      ///< stable identifier (automation / presets)
    const char* name;     ///< display name
    const char* section;  ///< GUI grouping
    ParamKind   kind;     ///< how the value is read
    float       min;      ///< the lowest value, in the parameter's unit
    float       max;      ///< the highest value
    float       def;      ///< the default: what a fresh instance has, and what an unmentioning preset sets
    float       skew;     ///< 1 = linear, <1 = more resolution at the low end
    const char* unit;     ///< the unit shown after the value ("Hz", "s", "dB", "ct", ""), for the editor and the help
    const char* const* choices; ///< ParamKind::Choice only
    int         numChoices;     ///< how many names `choices` holds (0 for anything but a choice)
};

/**
 * @brief Ordered by ParamId. Verified by the self test.
 * @return the table, kNumParams rows, row i describing ParamId i
 */
const std::array<ParamDesc, kNumParams>& paramTable();
/**
 * @brief The row of one parameter.
 * @param id  the parameter
 * @return    its description
 */
inline const ParamDesc& paramDesc(ParamId id) { return paramTable()[static_cast<size_t>(id)]; }
/**
 * @brief Looks a parameter up by its key.
 *
 * A binary search over a sorted index built on first use (Params.cpp): everything that reads a
 * preset goes through here, and a linear scan over the whole table cost 1.75 seconds every time
 * the map was warmed up for the library.
 *
 * @param key  the stable identifier, as presets and OSC spell it
 * @return     the row, or nullptr if unknown
 */
const ParamDesc* findParam(const char* key);   ///< nullptr if unknown

constexpr int kSourceSlots = 4;    ///< the self test checks this against kSlots in Sources.h
constexpr int kSlotFields  = 42;   ///< ... the last of them the slot's role (13.09.2026)
/**
 * @brief The parameter ids of one source slot, field by field.
 *
 * The three source slots have the same twenty-six fields, but their parameter ids are not
 * consecutive (Source 1's level and spectrum are the classic Oscillator parameters). The table
 * that maps slot and field to an id used to be written out twice -- once in the engine, once in
 * the editor -- and every field added since had to be added to both. It lives here now.
 *
 * @param slot  0 .. kSourceSlots-1
 * @return      kSlotFields entries, or nullptr for a slot that is not one
 */
const ParamId* slotParamIds(int slot);   ///< kSlotFields entries, or nullptr for a slot that is not one

/**
 * @brief What section a parameter belongs to, as something the compiler can check.
 *
 * The section string in
 * the table stays (the editor shows it), but everything that asks a question about a parameter --
 * is this performance state, is this part of the Cosmos layer -- asks it through this, so a
 * mistyped section name is an Unknown the self test catches instead of a predicate that silently
 * answers no forever.
 */
enum class ParamSection : int {
    Master, Source1, Strands, Source2, Source3, Source4, Strike, Foundation, Air, Envelope, Filter, ZPlane,   ///< the master stage, the four sources and the strands, the strike, the sub, the air, the amplitude envelope, the two filters
    Expression, Space, Ensemble, Delay, Delay2, NearReverb, FarReverb, Blur, Feedback, Room, Body,   ///< the expression, the planes, and the effects chain from the ensemble to the body
    Patina, Cosmos, Cloud, ClusterBrain, Brain2, Autoplay, Tuning, Coherence, Clock, Lfo, ModEnvelope, Morph,   ///< the patina, the Cosmos and cloud, the two conductors and autoplay, tuning, coherence, the clock, every LFO, every envelope (the sources' own included), the morph
    Macros, Map, Route, Vector, Memory, NearSource, NearEvents, Unknown   ///< the macros, the map, the route, the vector, the Memory, the near layer's two sections, and a name the table does not know
};
extern const char* const kMemLineNames[3];      ///< "2", "4", "8": how many lines the Memory's pool is divided into
/**
 * @brief The section behind a section name from the table ("LFO 3", "Env 5" and "Src Env 2" fold into Lfo and ModEnvelope).
 * @param sectionName  the string in ParamDesc::section; nullptr gives Unknown
 * @return             the section, Unknown for a name the table of names does not hold
 */
ParamSection sectionOf(const char* sectionName);
/**
 * @brief By id it is a lookup, not a search.
 *
 * It used to take the parameter's section NAME and compare it
 * against a table of names, string by string -- and isPerformanceParam asks five of those
 * questions, and the modulation matrix asks it of all two hundred and ninety-three parameters
 * once per block. Measured: forty-four thousand string comparisons per block, 101 microseconds,
 * three quarters of everything the engine did between one block and the next.
 *
 * @return kNumParams entries, built once, indexed by ParamId
 */
const ParamSection* sectionTable();   ///< kNumParams entries, built once
/**
 * @brief The section of a parameter, by id (a table lookup, see sectionTable()).
 * @param id  the parameter
 * @return    its section
 */
inline ParamSection sectionOf(ParamId id) { return sectionTable()[static_cast<size_t>(id)]; }

constexpr int kNumScaleChoices = 13;   ///< entries of kScaleNames: eleven tables, the user slot and the timbre scale
/**
 * @brief The last two are not tables.
 *
 * One is whatever Scala file was loaded; one is computed from
 * the instrument's own spectrum while it plays. Named, because five places used to spell the
 * user slot as "the last one" and appending anything after it would have quietly moved it.
 */
constexpr int kUserScaleIndex   = 11;
constexpr int kTimbreScaleIndex = 12;   ///< the scale computed from the spectrum (Sethares), the other one that is not a table
/**
 * @brief Names used by the Scale choice parameter; index == built-in scale index,
 *        the last entry is the user slot filled by a loaded Scala file.
 */
extern const char* const kScaleNames[kNumScaleChoices];
extern const char* const kRootNames[12];    ///< "C" .. "B", the twelve pitch classes for the Root choice
extern const char* const kKeyMapNames[2];   ///< 0 = snap 12 keys/octave to nearest degree, 1 = consecutive degrees
extern const char* const kSubOctaveNames[2];   ///< "-1", "-2"
extern const char* const kSubSourceNames[3];   ///< "Root", "Difference" (ghost tone), "Lowest" (the lowest voice)
constexpr int kNumSlotEnvs = 8;                ///< "Off", the six shapes the preset carries, and "Own"
extern const char* const kSlotEnvNames[kNumSlotEnvs];   ///< the names behind a slot's Env choice, kNumSlotEnvs of them
constexpr int kNumInterp = 2;                  ///< Linear, Hermite
extern const char* const kInterpNames[kNumInterp];      ///< "Linear", "Hermite": how a grain reads between two samples
extern const char* const kRoomSourceNames[2];  ///< "Far", "Near": what the convolution room reverberates
extern const char* const kAirModeNames[2];      ///< "Band" (one band-pass) or "Ghost" (resonators on the just harmonics); see also the line at kStrikeWhoNames
extern const char* const kEnsModeNames[3];      ///< "Chorus", "Microshift" (static detune), "Velvet" (sparse-noise decorrelation)
extern const char* const kKeysFilterNames[2];   ///< "Classic" (a fixed 30 ms), "One Euro" (cutoff follows the distance still to travel)
extern const char* const kTransposeNames[7];    ///< "None", "Fourth up", "Fifth up", "Octave up", "Fourth down", "Fifth down", "Octave down"
extern const char* const kCloudResModeNames[2]; ///< "Band" (Mathews-Smith phasor resonators), "Comb" (tuned feedback combs)
extern const char* const kCloudResNoteNames[4]; ///< which of the scale's notes the cloud's resonators sit on
extern const char* const kFarModeNames[4];      ///< "Classic", "Scattering" (all-passes in the loop), "Colourless" (also flat-searched lengths), "Rotating" (also a turning lossless matrix)
extern const char* const kBinauralNames[2];     ///< "Off" or "Headphones"
extern const char* const kStrikeTypeNames[3];   ///< String, Wood, Metal
extern const char* const kStrikeWhoNames[2];    ///< Keys, Keys + Brain     // "Band" (one band-pass) or "Ghost" (resonators on the just harmonics)
constexpr int kNumStacks = 8;   ///< entries of kStackNames and rows of kStackRatios
extern const char* const kStackNames[kNumStacks];   ///< Detune, Octaves, Fifths, Major, Minor, Seventh, Harmonics, Subharmonics
extern const double kStackRatios[kNumStacks][6];    ///< ratio of strand 0..5 to the note (Detune = all 1)
constexpr int kNumShimmerPitches = 6;   ///< entries of kShimmerPitchNames and kShimmerPitchSemitones
extern const char* const kShimmerPitchNames[kNumShimmerPitches];    ///< "+12", "+7", "+5", "+19", "-12", "+24": the Cosmos shimmer's intervals
extern const float kShimmerPitchSemitones[kNumShimmerPitches];      ///< the semitones behind kShimmerPitchNames

} // namespace ambient
