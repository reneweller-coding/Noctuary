// Noctuary -- parameter table (single source of truth).
// The core engine, the plugin host layer, the GUI and the render tool all read this table.
// No framework dependencies here: this header must compile on Quest/Android.
#pragma once
#include <array>
#include <cstddef>

namespace ambient {

enum class ParamId : int {
    // Master
    MasterGain,
    // Source 1: Type chooses the additive strand bank (the classic Oscillator, default) or any of
    // the slot types below; then the bank's own spectrum parameters
    Src1Type,
    OscLevel, Partials, Tilt, Brightness, OddEven, Inharmonic, Shimmer, ShimmerRate,
    Unison, Detune, Drift, DriftRate, Spread, Bloom, BloomTime,
    // Stack: strands at pure ratios (one key = one just chord) instead of detuned copies;
    // Rate Wander: every voice's drift and shimmer rates themselves wander (nested LFO)
    Stack, RateWander,
    // Source 1's slot fields, for the non-additive types (level is OscLevel, spectrum the eight above)
    Src1Octave, Src1Ratio, Src1Pan, Src1Table, Src1Position, Src1PosDrift,
    Src1FmRatio, Src1FmIndex, Src1Grain, Src1Density, Src1DensitySync, Src1Follow, Src1Grains, Src1Spread, Src1Noise, Src1NoiseQ, Src1Drift,
    // Source 2 / Source 3: the same slot, laid out identically (the engine reads them by offset):
    // 17 slot fields, then the seven of the slot's own additive bank
    Src2Type, Src2Level, Src2Octave, Src2Ratio, Src2Pan, Src2Table, Src2Position, Src2PosDrift,
    Src2FmRatio, Src2FmIndex, Src2Grain, Src2Density, Src2DensitySync, Src2Follow, Src2Grains, Src2Spread, Src2Noise, Src2NoiseQ,
    Src2Partials, Src2Tilt, Src2Bright, Src2OddEven, Src2Inharm, Src2Shimmer, Src2ShimmerRate, Src2Drift,
    Src3Type, Src3Level, Src3Octave, Src3Ratio, Src3Pan, Src3Table, Src3Position, Src3PosDrift,
    Src3FmRatio, Src3FmIndex, Src3Grain, Src3Density, Src3DensitySync, Src3Follow, Src3Grains, Src3Spread, Src3Noise, Src3NoiseQ,
    Src3Partials, Src3Tilt, Src3Bright, Src3OddEven, Src3Inharm, Src3Shimmer, Src3ShimmerRate, Src3Drift,
    // Foundation: a sub voice that follows the brain's root or the ghost tone (difference
    // tone of the two lowest sounding voices); Pad Low Cut keeps the pads out of its register
    SubLevel, SubOctave, SubGlide, SubBinaural, SubTone, SubSource, PadLowCut,
    // Strike: a short plucked or struck impulse (Karplus-Strong) at note-on, on the near plane
    StrikeLevel, StrikeType, StrikeDecay, StrikeDamp, StrikeWho,
    // Air (filtered-noise breath layer per voice)
    Air, AirColor, AirQ,
    // Amplitude envelope
    Attack, Decay, Sustain, Release,
    // Filter: one of nine models (Filter.h) behind the same knobs; Drive saturates ahead of it
    FilterOn, FilterModel, Cutoff, Resonance, FilterEnv, FilterDrift, KeyTrack, FilterDrive,
    // Z-plane filter: four frames on a square, the point (X, Y) interpolates their poles and wanders
    // Route: with both filters on, the z-plane follows the voice filter (Series) or both hear the
    // dry signal and Mix balances them (Parallel); Replace is the z-plane alone
    // ZZ is the cube's third axis (Transform); 0 is the filter as it was before it existed
    ZMode, ZRoute, ZShape, ZX, ZY, ZZ, ZRate, ZDepth, ZResonance, ZKeyTrack, ZMix,
    ZDecay, ZDamp,          // Modal mode: how long the modes ring, and how much shorter the high ones
    // Space: front-to-back planes, per-voice interaural time difference, hour-scale arc,
    // presence bell for the near plane, slow breathing of every voice's distance
    Depth, KeysDepth, PanDrift, Itd, ArcAmount, ArcPeriod, ArcSync, Presence, Breath, BreathRate,
    // Phase Width: two all-pass pairs per voice, drifting in opposite directions on the two ears,
    // so the room seems to change size rather than the sound to move; Doppler: the breathing
    // distance bends the pitch a little as a voice approaches or recedes
    PhaseWidth, PhaseRate, Doppler, Externalise,
    // Expression: what a key's pressure, its sideways slide and its own pitch bend do.
    // With an MPE controller each finger has all three; a plain keyboard shares them.
    PressDistance, PressBright, PressLevel, SlideCutoff, SlideZ, BendRange, MpeOn,
    // Ensemble
    EnsembleMix, EnsembleDepth, EnsembleRate, EnsembleSync,
    // Stereo delay (asymmetric L/R)
    DelayTimeL, DelayTimeR, DelaySyncL, DelaySyncR, DelayFeedback, DelayCross, DelayDamp, DelayAbsorb, DelayMix, DelayToFar, DelayDuck,
    // Second stereo delay, in series after the first
    Delay2TimeL, Delay2TimeR, Delay2SyncL, Delay2SyncR, Delay2Feedback, Delay2Cross, Delay2Damp, Delay2Absorb, Delay2Mix, Delay2ToFar,
    // Near reverb (foreground room)
    NearMix, NearDecay, NearDamp,
    // Blur: a spectral smear on the near bus itself, so an attack is wiped into texture
    BlurMix, BlurSmear,
    // Far reverb (the infinite background)
    FarLevel, FarSize, FarDecay, FarDamp, FarPreDelay, FarAsym, FarHighcut, FarFreeze, FarRotate, FarUnmask,
    // Diffusion: modulated all-passes in front of the reverb, so the tail arrives instead
    // of starting; and the sympathetic coupling of the voices through each other's filters
    FarDiffuse,
    // Feedback: the mixed output (before the master) returns, low-passed and saturated,
    // into the near bus before the filters and effects, and/or as phase modulation of every
    // partial. Throttled by the output level so it hisses and holds instead of running away.
    FeedbackBus, FeedbackFm, FeedbackTone, FeedbackDrive,
    // Room: convolution reverb with a loaded (or generated) impulse, in parallel on the far plane
    RoomLevel, RoomSource, RoomPreDelay, RoomHighcut, RoomMorph,
    // Cosmos: science-fiction / deep-space path (send from the near bus)
    CosmosSend, CosmosShift, CosmosShiftDrift, CosmosRes, CosmosResPitch, CosmosResFeedback,
    CosmosVowel, CosmosVowelRate, CosmosNebula, CosmosSmear, CosmosShimmer, CosmosShimmerPitch,
    CosmosReturn, CosmosToFar,
    // Granular cloud on the far plane
    CloudSend, CloudDensity, CloudSync, CloudSize, CloudPitch, CloudSpray, CloudLevel,
    // Body: a bank of modes under everything -- the soundboard the pad sits on (Body.h)
    BodyLevel, BodyMaterial, BodyPitch, BodyDecay, BodyTone, BodySpread,
    // Patina: the master's age -- tape wow, lost highs, a noise floor, gentle saturation
    PatinaAmount, PatinaWow, PatinaHiss, PatinaAge,
    // Master tilt: one broad see-saw around a pivot -- the move an ambient mix asks for
    // more than any other, and the one thing the master stage did not have
    Tilt2, TiltPivot,
    // Mid/side master stage
    BassMono, SideAir, Width, MonoGuard,
    // Cluster brain (generative sleep-concert mode)
    BrainOn, BrainDensity, BrainRate, BrainSync, BrainHoldMin, BrainHoldMax,
    BrainLow, BrainHigh, BrainConsonance, BrainWander, BrainQuantize,
    // Autoplay: instead of notes coming and going, the cluster stays full and one voice at a
    // time is exchanged, so the chord travels. Step is a trigger, not a level.
    AutoMode, AutoRate, AutoSync, AutoLead, AutoTension, AutoRootMove, AutoStep,
    // A second conductor for the background: its own register, pace and plane, on the
    // first one's root (plus an interval), so the two play a slow counterpoint
    Brain2On, Brain2Density, Brain2Rate, Brain2HoldMin, Brain2HoldMax,
    Brain2Low, Brain2High, Brain2Depth, Brain2Interval, Brain2Consonance,
    // Tuning
    Scale, KeyMap, RootNote, RefPitch, Seed, Hold,
    // Purity blends every note between 12-TET (0) and the chosen scale (1) in the log domain; Drift
    // lets that blend wander so the beating locks in and loosens over minutes; Freeze holds every
    // voice's spectrum and pitch still (drifts, shimmer, bloom stop moving)
    TunePurity, TuneDrift, TuneDriftRate, Freeze,
    // Tide: the whole instrument's pitch leans by a few cents over many minutes
    Tide, TidePeriod,
    // Ghost: the Air noise through a bank of sharp resonators on the note's just harmonics
    AirMode,
    // Portamento for keys: a new key glides from the last one; Gravity slows the glide near
    // consonant ratios to the root, so the slide "clicks into" harmonic nodes on the way
    Portamento, PortaGravity,
    // Tape in the feedback loop: asymmetric saturation, wow and flutter, level-dependent noise floor
    FeedbackTape,
    // Coherence: four slow Kuramoto oscillators, coupled by Coherence, modulating brightness,
    // depth, pan drift and the z-plane point by Depth
    Coherence, CoherenceDepth, CoherenceRate, Sympathy,
    // Modulation: eight free LFOs and six multi-segment envelopes. Their shapes and the matrix
    // rows are data, not parameters (see Modulation.h); what sits here is what a host automates.
    Lfo1Shape, Lfo1Rate, Lfo1Phase, Lfo1Depth, Lfo1Mode, Lfo1Table, Lfo1Sync,
    Lfo2Shape, Lfo2Rate, Lfo2Phase, Lfo2Depth, Lfo2Mode, Lfo2Table, Lfo2Sync,
    Lfo3Shape, Lfo3Rate, Lfo3Phase, Lfo3Depth, Lfo3Mode, Lfo3Table, Lfo3Sync,
    Lfo4Shape, Lfo4Rate, Lfo4Phase, Lfo4Depth, Lfo4Mode, Lfo4Table, Lfo4Sync,
    Lfo5Shape, Lfo5Rate, Lfo5Phase, Lfo5Depth, Lfo5Mode, Lfo5Table, Lfo5Sync,
    Lfo6Shape, Lfo6Rate, Lfo6Phase, Lfo6Depth, Lfo6Mode, Lfo6Table, Lfo6Sync,
    Lfo7Shape, Lfo7Rate, Lfo7Phase, Lfo7Depth, Lfo7Mode, Lfo7Table, Lfo7Sync,
    Lfo8Shape, Lfo8Rate, Lfo8Phase, Lfo8Depth, Lfo8Mode, Lfo8Table, Lfo8Sync,
    Env1Mode, Env1Time, Env1Depth, Env1Sync,
    Env2Mode, Env2Time, Env2Depth, Env2Sync,
    Env3Mode, Env3Time, Env3Depth, Env3Sync,
    Env4Mode, Env4Time, Env4Depth, Env4Sync,
    Env5Mode, Env5Time, Env5Depth, Env5Sync,
    Env6Mode, Env6Time, Env6Depth, Env6Sync,
    // Morph between two stored full presets (A/B); never part of a preset itself
    MorphActive, MorphPos, MorphGlide,
    // Macros: eight performance controls routed through the gesture layer (Custom0..7);
    // not part of presets. Named in Rich's vocabulary, not the engine's.
    MacroA, MacroB, MacroC, MacroD, MacroE, MacroF, MacroG, MacroH,
    // Inertia: every float parameter glides to its value with this time constant (the analogue
    // slew), so even a knob torn open arrives slowly; performance state, not in presets
    Inertia,
    // Preset map: a cursor in the plane of all presets blends its neighbours (PresetMap.h);
    // performance state like the morph, never part of a preset
    MapActive, MapX, MapY, MapRadius,
    // Route: the engine walks a route of waypoints over the map (Route.h); performance state
    RouteActive, RouteSpeed, RouteLoop,
    // Clock (Clock.h): where the tempo comes from, the internal tempo, and whether the internal
    // clock runs; performance state -- the tempo belongs to the session, not to a preset
    ClockSource, Tempo, ClockRun,
    // Appended, never inserted: a parameter's position in this enum is its automation slot in
    // the host, and moving one would move somebody's automation lane with it.
    //
    // The other end of the filter funnel on the three reverb returns. A tail with its low mids
    // still in it sits in front of the music instead of behind it.
    FarLowcut, NearLowcut, RoomLowcut,
    // A steep high-pass on the finished output for the energy below hearing: it does nothing for
    // the music and everything for the amplifier. Off by default, because the Foundation reaches
    // lower than the frequency a mastering engineer would cut at.
    Subsonic,
    // Vector: a point in a square whose corners are the three source slots and the three of them
    // together, after the Prophet VS and the Wavestation. Amount 0 leaves every slot's level
    // exactly as it is set.
    VecAmount, VecX, VecY, VecWander, VecRate,
    // Source 4: the same slot as 2 and 3, laid out identically, so the Vector's four corners are
    // four sources rather than three and the three together. Appended for the same reason as
    // everything above it.
    Src4Type, Src4Level, Src4Octave, Src4Ratio, Src4Pan, Src4Table, Src4Position, Src4PosDrift,
    Src4FmRatio, Src4FmIndex, Src4Grain, Src4Density, Src4DensitySync, Src4Follow, Src4Grains, Src4Spread, Src4Noise, Src4NoiseQ,
    Src4Partials, Src4Tilt, Src4Bright, Src4OddEven, Src4Inharm, Src4Shimmer, Src4ShimmerRate, Src4Drift,
    // The Stretch type's two settings, one pair per slot, fields 26 and 27 of the slot.
    Src1Stretch, Src1Xfade, Src2Stretch, Src2Xfade, Src3Stretch, Src3Xfade, Src4Stretch, Src4Xfade,
    // Four things a dark-ambient mixing desk does that this instrument could not, each off at its
    // default so nothing that exists sounds different: the background narrowed as it goes back,
    // a static micro-detune instead of the chorus, a band delayed to one side, and a wavefolder.
    FarWidth, EnsMode, Haas, HaasTime, FilterFold,
    // What the literature after the classics asks for: a stretched octave (Ward 1954, Terhardt),
    // a reverb with scattering in its loop (Schlecht and Habets 2020), masking that spreads
    // upward (Zwicker), and a binaural mode that follows the head. Neutral at every default.
    TuneStretch, FarMode, FarUnmaskSpread, Binaural,
    // The conductor judging intervals by the spectrum it actually plays (Sethares), not by
    // the ratio alone. 0 = the ratio score it always had.
    BrainTimbre, BrainSpacing,
    // The room's early reflections, from its geometry (see Effects.h): off at Level 0.
    EarlyLevel, EarlySize, EarlyAbsorb, EarlyWidth,
    // The Bow type's two: how hard the bow presses and how fast it travels. Fields 28 and 29
    // of every slot.
    Src1BowForce, Src1BowSpeed, Src2BowForce, Src2BowSpeed, Src3BowForce, Src3BowSpeed, Src4BowForce, Src4BowSpeed,
    Src1SpecRate, Src1SpecBreath, Src2SpecRate, Src2SpecBreath, Src3SpecRate, Src3SpecBreath, Src4SpecRate, Src4SpecBreath,
    BrainHarmonic, BrainKey, BrainEven, BrainSmooth,
    TuneGuard, TuneMatch, BrainBlend, ArcHarmony,
    ElevNear, ElevFar, DepthLaw, Envelop,
    ArcClock,
    BrainCascade, BrainSurprise, BrainHomeostat, TuneAdapt,
    FarComod, NearIld,
    Src1Transport, Src2Transport, Src3Transport, Src4Transport, SubPulse, FeedbackBias,
    LeniaRate, LeniaGrowth,
    BrainDejaVu, BrainLoop, BrainSpread, BrainBias, ChaosPeriod, KeysFilter, Transpose, PartialSpread,
    StrikeChance, StrikeCluster, CosmosSwell,
    // Each slot's own entrance. Until these existed the four sources of a preset all started
    // together on the one amplitude envelope in the Envelope section, so a preset with a
    // wavetable and a texture was a single chord struck twice at once, however different the two
    // materials were. Delay holds the slot silent after the note, Rise fades it in, and Env picks
    // one of the six shapes as the slot's own contour instead. All three default to off, so a
    // preset that says nothing about them sounds exactly as it did.
    Src1Delay, Src1Rise, Src1Env,
    Src2Delay, Src2Rise, Src2Env,
    Src3Delay, Src3Rise, Src3Env,
    Src4Delay, Src4Rise, Src4Env,
    // How a grain reads between two samples of its clip. Linear is what the instrument always
    // did; Hermite costs two more reads a sample and is offered rather than imposed, because
    // which of them is right is a matter of taste in an instrument built to be soft.
    Src1Interp, Src2Interp, Src3Interp, Src4Interp,
    // Unison in a slot: the main oscillator has had detuned strands with their own place in the
    // field since the beginning, a slot had one mono voice. Defaults to one copy, which is that.
    // Root: how much of a fundamental a wavetable is given when its own has none.
    Src1Root, Src2Root, Src3Root, Src4Root,
    Src1Unison, Src1UniDetune, Src1UniWidth,
    Src2Unison, Src2UniDetune, Src2UniWidth,
    Src3Unison, Src3UniDetune, Src3UniWidth,
    Src4Unison, Src4UniDetune, Src4UniWidth,
    // The cloud as a granular feedback instrument (12.09.2026): its loop, the scatter over the
    // scale's intervals, its flocks and its resonators on the scale's notes. All off by default,
    // so a preset that says nothing about them has the cloud it always had.
    CloudFeedback, CloudTone, CloudTranspose, CloudScatter, CloudSwarm,
    CloudResonance, CloudResMode, CloudResNotes, CloudResDecay,
    // The Memory (12.09.2026): a drifting sound memory beside the Cosmos -- a send, two returns, the
    // lines and their exchange, how long and how darkly it keeps, the recalled grains, and the tape.
    MemSend, MemReturn, MemToFar, MemLines, MemSize, MemBlur, MemDrift, MemHold, MemAge,
    MemRenew, MemDrive, MemRecall, MemSeek, MemGrain, MemFreeze, MemReverse, MemHalf, MemErase,
    // The shifter in the spectrum (12.09.2026): which shifter the shimmer's loop uses, and the
    // cloud's loop transposed on every pass.
    CosmosShimmerMode, CloudShift,
    // Each source's own envelope (11.09.2026). A slot's Env could only borrow one of the six
    // modulation envelopes, so every source that entered on a shape left one envelope fewer for
    // modulating anything else. Env = Own reads the slot's own shape with these four, which do
    // what an Env section's do: Mode, Time, Depth and Sync, four per slot in that order.
    Src1EnvMode, Src1EnvTime, Src1EnvDepth, Src1EnvSync,
    Src2EnvMode, Src2EnvTime, Src2EnvDepth, Src2EnvSync,
    Src3EnvMode, Src3EnvTime, Src3EnvDepth, Src3EnvSync,
    Src4EnvMode, Src4EnvTime, Src4EnvDepth, Src4EnvSync,
    // What a generator of this music needs and the conductor had no way to say (12.09.2026, from
    // Rene's rule book for a self-playing ambient generator): the register roles and the spacing
    // that follows from them, the colour of the intervals, a clock that breathes and guards what
    // it may not do, and the root's walk over the hour. Every one of them does nothing at its
    // default, so every preset written before them plays exactly as it did.
    //
    // Layers and register: the pyramid (one voice at the bottom, most in the body, few on top),
    // the bass held longer than what moves above it, the top softer, spacing sharpened with depth,
    // the floor under which no third is chosen, and the leading note kept off the root.
    BrainLayers, BrainBassHold, BrainTopSoft, BrainLowSpacing, BrainThirdFloor, BrainLeading,
    // Interval colour: thirds sought or avoided, seconds likewise (above the third floor only),
    // the seventh lifted -- as 7/4 in a just scale, the most characteristic interval this music
    // has -- and the chance that a root change swaps one degree of the mode instead.
    BrainThirds, BrainSeconds, BrainSeventh, BrainDegreeSwap,
    // Time: the mean event rate breathing on its own slow curve, the overlap that keeps a voice
    // sounding while the one it replaces lets go, the thirty-millisecond rule as a guard, the gap
    // between two note-offs, the silence a pitch must keep before it may sound again, density
    // changes walked one voice at a time, and the planned silence at a section's border.
    BrainRateBreath, BrainBreathPeriod, BrainOverlap, BrainOnsetGuard, BrainReleaseGap,
    BrainRetrigger, BrainDensitySlew, BrainSilence, BrainSilenceLen,
    // Root and form: where a new root may come from, which way it leans, the pivot window in which
    // old and new root sound together, the pull back to the root the night began on, and the
    // memory that forbids a chord the hour has already had.
    BrainRootSteps, BrainRootDown, BrainPivot, BrainHome, BrainHomeTime, BrainMemory,
    // The background conductor on a clock that never lines up with the foreground's; the tuning
    // holding what already sounds when the root moves; and a ceiling on how fast a beat may beat.
    // (Brain2 Interval was asked for too, and already existed: brain2_interval, in semitones.)
    Brain2Golden, TuneHoldSounding, BeatCeiling,
    // Three that are true outside the conductor: a quiet note entering slower than a loud one, the
    // plane a voice stands in taken from its role rather than from the draw, and the strands of a
    // low note detuned less than those of a high one, so warmth does not become wobble.
    EnvVelAttack, LayerDepth, StrandLowDetune,
    // How much of the granular cloud comes back on the NEAR plane instead of the far one. The cloud
    // has always returned to the far bus alone -- a deliberate choice, written down in the concept
    // paper: "it exists in the background and the far reverb smears it". At 0 that is exactly what
    // still happens, so no preset changes; above it the cloud can be brought forward, which is what
    // a listener expects of a send they have turned all the way up.
    CloudToNear,
    // The near plane (13.09.2026). A role per slot -- which of the voice's notes it sounds in:
    // all, the lowest, an inner one, the highest -- which belongs to the sound preset. And the
    // near layer, which does not: a source of its own (Near Source: a fifth slot with its own
    // type, envelope, filter and strike, rendered only by the events) and the Near Events section
    // (what the instrument plays close to the ear, on a clock of its own). The layer is a bank
    // like the Cosmos, kept across sound presets, so a night's foreground can be chosen once and
    // the backgrounds changed under it. Level 0 is off, and every preset ever written has it there.
    Src1Role, Src2Role, Src3Role, Src4Role,
    ForeType, ForeOctave, ForeRatio, ForePosition, ForePosDrift, ForeDensity, ForeFollow, ForeBright,
    ForeForce, ForeSpeed, ForeNoise, ForeNoiseQ, ForeFmRatio, ForeFmIndex, ForePartials, ForeTilt,
    ForeInharm, ForeDrift, ForeTable, ForeAttack, ForeDecay, ForeSustain, ForeRelease,
    ForeCutoff, ForeResonance, ForeFilterModel, ForeFilterEnv, ForeStrike, ForeStrikeType, ForeStrikeDecay, ForeStrikeDamp,
    ForeLevel, ForeKind, ForeRate, ForeChance, ForeCluster, ForeLength,
    ForePitch, ForeSpread, ForeApproach, ForeProximity, ForeHold, ForeGlide,
    ForeSteps, ForeStep, ForeStepSync, ForeMutation, ForeScatter, ForeBloom,
    // Where an event sits between the planes (0 at the ear, 1 on the horizon; Approach arrives
    // from the horizon to it, or, negative, leaves from it), and how much of it goes straight to
    // the output past every reverb and delay: a rattle twenty centimetres from the nose, a click
    // that no room may soften.
    ForeDistance, ForeDry,
    // Whether a sound preset of a pack brings its own foreground (the artist's table, NearAuto.inc).
    ForeAuto,
    // How long the far reverb takes to come back after the foreground has ducked it. It came back
    // in 1.2 seconds since the unmask was built, and that stays the default; a foreground that
    // speaks and then lets the horizon return over five to ten seconds makes the return itself a
    // gesture, which is what the near events want.
    FarUnmaskReturn,
    // The foreground's own sends, per event: a share of the event's voice into the second delay
    // and into the Cosmos, on top of what the near bus gives them. A send on a desk, not a
    // routing switch -- the event still sounds where Distance and Dry put it, and this is what is
    // thrown into the long chain or into the deep space beside it.
    ForeDelay2, ForeCosmos,
    Count
};

extern const char* const kNearKindNames[3];     // "Note", "Phrase", "Sequence"
extern const char* const kNearPitchNames[5];    // "Consonant", "Highest", "Lowest", "Root", "Cluster"

extern const char* const kRootStepNames[4];     // "Any", "Fifths", "Diatonic", "Falling"
extern const char* const kShimmerModeNames[2];  // "Spectral" (phase-locked peak shifting), "Grain" (the two-head shifter)
extern const char* const kCloudShiftNames[7];   // "Off", "+12", "+7", "+5", "-5", "-7", "-12"
extern const float kCloudShiftSemitones[7];

constexpr int kNumParams = static_cast<int>(ParamId::Count);

enum class ParamKind { Float, Int, Bool, Choice };

struct ParamDesc {
    ParamId     id;
    const char* key;      // stable identifier (automation / presets)
    const char* name;     // display name
    const char* section;  // GUI grouping
    ParamKind   kind;
    float       min;
    float       max;
    float       def;
    float       skew;     // 1 = linear, <1 = more resolution at the low end
    const char* unit;
    const char* const* choices; // ParamKind::Choice only
    int         numChoices;
};

// Ordered by ParamId. Verified by the self test.
const std::array<ParamDesc, kNumParams>& paramTable();
inline const ParamDesc& paramDesc(ParamId id) { return paramTable()[static_cast<size_t>(id)]; }
const ParamDesc* findParam(const char* key);   // nullptr if unknown

// Names used by the Scale choice parameter; index == built-in scale index,
// the last entry is the user slot filled by a loaded Scala file.
// The three source slots have the same twenty-six fields, but their parameter ids are not
// consecutive (Source 1's level and spectrum are the classic Oscillator parameters). The table
// that maps slot and field to an id used to be written out twice -- once in the engine, once in
// the editor -- and every field added since had to be added to both. It lives here now.
constexpr int kSourceSlots = 4;    // the self test checks this against kSlots in Sources.h
constexpr int kSlotFields  = 42;   // ... the last of them the slot's role (13.09.2026)
const ParamId* slotParamIds(int slot);   // kSlotFields entries, or nullptr for a slot that is not one

// What section a parameter belongs to, as something the compiler can check. The section string in
// the table stays (the editor shows it), but everything that asks a question about a parameter --
// is this performance state, is this part of the Cosmos layer -- asks it through this, so a
// mistyped section name is an Unknown the self test catches instead of a predicate that silently
// answers no forever.
enum class ParamSection : int {
    Master, Source1, Strands, Source2, Source3, Source4, Strike, Foundation, Air, Envelope, Filter, ZPlane,
    Expression, Space, Ensemble, Delay, Delay2, NearReverb, FarReverb, Blur, Feedback, Room, Body,
    Patina, Cosmos, Cloud, ClusterBrain, Brain2, Autoplay, Tuning, Coherence, Clock, Lfo, ModEnvelope, Morph,
    Macros, Map, Route, Vector, Memory, NearSource, NearEvents, Unknown
};
extern const char* const kMemLineNames[3];      // "2", "4", "8": how many lines the Memory's pool is divided into
ParamSection sectionOf(const char* sectionName);
// By id it is a lookup, not a search. It used to take the parameter's section NAME and compare it
// against a table of names, string by string -- and isPerformanceParam asks five of those
// questions, and the modulation matrix asks it of all two hundred and ninety-three parameters
// once per block. Measured: forty-four thousand string comparisons per block, 101 microseconds,
// three quarters of everything the engine did between one block and the next.
const ParamSection* sectionTable();   // kNumParams entries, built once
inline ParamSection sectionOf(ParamId id) { return sectionTable()[static_cast<size_t>(id)]; }

constexpr int kNumScaleChoices = 13;
// The last two are not tables. One is whatever Scala file was loaded; one is computed from
// the instrument's own spectrum while it plays. Named, because five places used to spell the
// user slot as "the last one" and appending anything after it would have quietly moved it.
constexpr int kUserScaleIndex   = 11;
constexpr int kTimbreScaleIndex = 12;
extern const char* const kScaleNames[kNumScaleChoices];
extern const char* const kRootNames[12];
extern const char* const kKeyMapNames[2];   // 0 = snap 12 keys/octave to nearest degree, 1 = consecutive degrees
extern const char* const kSubOctaveNames[2];   // "-1", "-2"
extern const char* const kSubSourceNames[3];   // "Root", "Difference" (ghost tone), "Lowest" (the lowest voice)
constexpr int kNumSlotEnvs = 8;                // "Off", the six shapes the preset carries, and "Own"
extern const char* const kSlotEnvNames[kNumSlotEnvs];
constexpr int kNumInterp = 2;                  // Linear, Hermite
extern const char* const kInterpNames[kNumInterp];
extern const char* const kRoomSourceNames[2];  // "Far", "Near": what the convolution room reverberates
extern const char* const kAirModeNames[2];
extern const char* const kEnsModeNames[3];      // "Chorus", "Microshift" (static detune), "Velvet" (sparse-noise decorrelation)
extern const char* const kKeysFilterNames[2];   // "Classic" (a fixed 30 ms), "One Euro" (cutoff follows the distance still to travel)
extern const char* const kTransposeNames[7];    // "None", "Fourth up", "Fifth up", "Octave up", "Fourth down", "Fifth down", "Octave down"
extern const char* const kCloudResModeNames[2]; // "Band" (Mathews-Smith phasor resonators), "Comb" (tuned feedback combs)
extern const char* const kCloudResNoteNames[4]; // which of the scale's notes the cloud's resonators sit on
extern const char* const kFarModeNames[4];      // "Classic", "Scattering" (all-passes in the loop), "Colourless" (also flat-searched lengths), "Rotating" (also a turning lossless matrix)
extern const char* const kBinauralNames[2];     // "Off" or "Headphones"
extern const char* const kStrikeTypeNames[3];   // String, Wood, Metal
extern const char* const kStrikeWhoNames[2];    // Keys, Keys + Brain     // "Band" (one band-pass) or "Ghost" (resonators on the just harmonics)
constexpr int kNumStacks = 8;
extern const char* const kStackNames[kNumStacks];   // Detune, Octaves, Fifths, Major, Minor, Seventh, Harmonics, Subharmonics
extern const double kStackRatios[kNumStacks][6];    // ratio of strand 0..5 to the note (Detune = all 1)
constexpr int kNumShimmerPitches = 6;
extern const char* const kShimmerPitchNames[kNumShimmerPitches];
extern const float kShimmerPitchSemitones[kNumShimmerPitches];

} // namespace ambient
