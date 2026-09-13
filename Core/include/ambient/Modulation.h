// Noctuary -- the modulation section: free LFOs, multi-segment envelopes and a matrix that
// connects any source to any number of targets.
//
// Until now every modulator in this instrument was soldered to one destination and carried its
// own depth and rate parameters: the pitch drifter to pitch, the filter drifter to the cutoff,
// the Kuramoto ring to brightness, distance, pan and the z-plane point. That is why adding a
// source always meant adding another pair of knobs. This is the general form.
//
// What is a parameter and what is data
// ------------------------------------
// The LFOs and the envelope timings are parameters (Params.h), so a host can automate them and a
// preset carries them like everything else. The *shapes* -- the envelope breakpoints and the
// matrix rows -- are data, the way a Scala scale and the gesture mappings already are: a compact
// text form that travels in the preset string and in the plugin state. Thirty-two matrix rows as
// four parameters each would put a hundred and twenty entries into the automation list for very
// little gain.
//
// Rates
// -----
// A drone instrument needs modulators far below the usual LFO range: the slowest here is one
// cycle in twenty minutes. Everything runs at control rate (one step per 64 samples), which is
// 750 Hz at 48 kHz -- ample for a modulator whose fastest setting is 20 Hz.
#pragma once
#include "Params.h"
#include "Dsp.h"
#include <cstdint>

namespace ambient {

struct Wavetable;   // Sources.h -- a table frame can be an LFO shape

constexpr int kNumLfos      = 8;
constexpr int kNumModEnvs   = 6;
constexpr int kMaxEnvPoints = 16;
constexpr int kMaxModRoutes = 32;

// ---------------------------------------------------------------- LFO

enum class LfoShape : int {
    Sine, Triangle, RampUp, RampDown, Square, Random, StepRandom, Table, Count
};
constexpr int kNumLfoShapes = static_cast<int>(LfoShape::Count);
extern const char* const kLfoShapeNames[kNumLfoShapes];

// Global: one phase for the whole instrument, so every voice breathes together.
// Voice: each voice runs its own copy from wherever the shared phase was when it started.
// Retrigger: each voice restarts its copy from Phase.
enum class LfoMode : int { Global, Voice, Retrigger, Count };
constexpr int kNumLfoModes = static_cast<int>(LfoMode::Count);
extern const char* const kLfoModeNames[kNumLfoModes];

struct LfoSpec {
    LfoShape shape = LfoShape::Sine;
    float    rateHz = 0.05f;
    float    phase = 0.0f;      // 0..1, where the shape starts
    float    depth = 1.0f;      // scales the output; the matrix scales again per route
    LfoMode  mode = LfoMode::Global;
    int      table = 0;         // frame of the user wavetable when shape == Table
};

// A shape read at a phase in 0..1, returning -1..1. `Table` reads a frame of a wavetable, which
// is what makes any of the 608 generated tables -- and any curve drawn into one -- a usable LFO
// shape without a second mechanism for "drawable" modulators.
class Lfo {
public:
    void reset(uint64_t seed, float phase01);
    // Advances by dt seconds and returns the new value in -1..1. `table` may be null.
    float step(float dt, const LfoSpec& spec, const Wavetable* table);
    float value() const { return value_; }
    float phase() const { return static_cast<float>(phase_); }
    void  setPhase(float p) { phase_ = p - std::floor(p); }
    // The shape at a phase, without stepping: the editor draws a whole cycle with this.
    static float shapeAt(const LfoSpec& spec, float phase01, const Wavetable* table,
                         const Lfo* live = nullptr);

private:
    friend float lfoStepsAccess(const Lfo&);
    double  phase_ = 0.0;
    float   value_ = 0.0f;
    float   prev_ = 0.0f, next_ = 0.0f;   // Random: the two ends of this cycle
    float   steps_[5] = {};               // Steps: four holds a cycle, the fifth carries over the wrap
    Rng     rng_;
};

// ---------------------------------------------------------------- envelope

// A multi-segment envelope: up to sixteen breakpoints, a curve per segment, an optional sustain
// point the envelope holds at while the note is down, and an optional loop between two points so
// it can run as a slow shape generator rather than a one-shot.
struct EnvPoint {
    float time = 0.0f;    // seconds from the start of the envelope, non-decreasing
    float value = 0.0f;   // -1..1
    float curve = 0.0f;   // -1 (fast then slow) .. 0 (linear) .. 1 (slow then fast)
};

// Shapes to start an envelope from, in the text form below. They live here rather than in the
// editor so that the self test can check that every one of them parses -- a shape string with a
// typo in it does not fail loudly, it simply does nothing when the menu item is picked.
constexpr int kNumEnvShapePresets = 10;
extern const char* const kEnvShapePresetNames[kNumEnvShapePresets];
extern const char* const kEnvShapePresetTexts[kNumEnvShapePresets];

enum class EnvMode : int { OneShot, Loop, SustainLoop, Count };
constexpr int kNumEnvModes = static_cast<int>(EnvMode::Count);
extern const char* const kEnvModeNames[kNumEnvModes];

class ModEnv {
public:
    // Points must be in non-decreasing time order; returns false otherwise. Fewer than two points
    // is a constant.
    bool set(const EnvPoint* points, int count);
    int  count() const { return count_; }
    const EnvPoint& point(int i) const { return points_[i]; }
    void setSustain(int index) { sustain_ = index; }       // -1 = none
    int  sustain() const { return sustain_; }
    void setLoop(int from, int to) { loopFrom_ = from; loopTo_ = to; }
    int  loopFrom() const { return loopFrom_; }
    int  loopTo() const { return loopTo_; }
    float length() const { return count_ > 0 ? points_[count_ - 1].time : 0.0f; }

    // Value at a time, following the mode. `held` is whether the note is still down.
    float at(float seconds, EnvMode mode, bool held) const;
    // The time `at` reads the shape at: folded into the loop or held at the sustain point, as the
    // mode says, and no later than the end. A release continues from here, so the value does not
    // jump from where the shape was being held to where the clock has run on to meanwhile.
    float readTime(float seconds, EnvMode mode, bool held) const;

    // Text form: "t:v:c/t:v:c/...", optionally followed by "!s<index>" for the sustain point and
    // "!l<from>-<to>" for the loop. Times in seconds. The marker is '!' and not '|' because a
    // pack line splits its fields on '|' -- an envelope with a loop used to tear the line apart.
    bool parse(const char* text);
    int  write(char* buf, size_t cap) const;

private:
    EnvPoint points_[kMaxEnvPoints];
    int count_ = 0;
    int sustain_ = -1, loopFrom_ = -1, loopTo_ = -1;
};

// One envelope's per-preset settings that are parameters rather than shape.
struct ModEnvSpec {
    EnvMode mode = EnvMode::OneShot;
    float   timeScale = 1.0f;   // stretches the whole shape, 0.05 .. 20
    float   depth = 1.0f;
};

// ---------------------------------------------------------------- matrix

// Everything that can drive a target. The order is the text form's order, so it must only ever
// grow at the end.
enum class ModSource : int {
    None,
    Lfo1, Lfo2, Lfo3, Lfo4, Lfo5, Lfo6, Lfo7, Lfo8,
    Env1, Env2, Env3, Env4, Env5, Env6,
    Amp,                       // the voice's own ADSR
    MacroA, MacroB, MacroC, MacroD, MacroE, MacroF, MacroG, MacroH,
    Kura1, Kura2, Kura3, Kura4,   // the coherence ring, now addressable
    Note, Velocity, Distance,     // per voice: pitch 0..1 over the keyboard, velocity, plane
    RandomPerNote,
    // The instrument listening to its own harmonic friction. The two lowest sounding voices are
    // compared with the simplest just ratio near the interval they make, and the rate of this
    // oscillator is the beat between them: silent when the chord is in tune, and quicker the
    // further the tuning has drifted from it. Route it at anything and the sound breathes in
    // time with how far out of tune it currently is.
    Beat,
    // What the hands are doing, as three ordinary sources. Pressure and Slide already reach the
    // sound through fixed routes (Expression), but only there; a player who wants aftertouch on
    // the filter's resonance, the wavetable position and the reverb at once needs them here. All
    // three rest at 0, so route them with the 0..1 flag and a patch at rest sounds untouched.
    Pressure, Wheel, Slide,
    // A Lenia field (Chan 2019): a continuous cellular automaton -- Conway's Life with real-valued
    // cells, a ring-shaped neighbourhood and a smooth growth rule -- run on a small torus in the
    // background, read at four fixed points. Its blobs drift, pulse, split and die; the four
    // readings are what they do near each corner. Off the clock and off the random stream:
    // motion that is caused by its own neighbours, which is what makes it read as alive.
    Lenia1, Lenia2, Lenia3, Lenia4,
    // Two strange attractors, integrated in the background on a time scale of minutes: the
    // Lorenz system (two lobes, switched between at irregular moments) and the Roessler system
    // (a slow spiral with a sudden climb). Deterministic chaos: never the same path twice,
    // never a cycle, and never a step -- what an LFO cannot be and filtered noise cannot be
    // either, which is the reason for having them. Three coordinates each.
    LorenzX, LorenzY, LorenzZ, RosslerX, RosslerY, RosslerZ,
    // The conductor's own excitement. With Cascade up its clock is a Hawkes process -- an event
    // breeds events -- and this is that excitation, as exc / (1 + exc), so it rests at 0 between
    // the clusters and climbs towards 1 inside one. It is the only source that comes from what
    // the piece is doing rather than from a clock, a shape or a field of its own, so a route
    // from it makes the instrument swell where it is busy and rest where it is not.
    Cascade,
    // Three that come from the piece's own shape rather than from a clock (12.09.2026).
    // Root Age: how long the root has stood, over Home Time -- 0 the moment it moves, climbing
    // towards 1 the longer it holds, so a sound can open after a change instead of being there
    // already. Layer: the role of the voice being read (foundation 0, body .25, colour .5, air
    // .75, shadow 1), which gives every register its own brightness, its own air, its own plane
    // without the conductor needing to know. Section: the hour's arc as five steps rather than a
    // glide, for the things that should switch rather than slide.
    RootAge, Layer, Section,
    Count
};
constexpr int kNumModSources = static_cast<int>(ModSource::Count);
const char* modSourceName(ModSource);
bool modSourceFromName(const char* name, ModSource& out);

struct ModRoute {
    ModSource source = ModSource::None;
    ParamId   target = ParamId::Cutoff;
    float     depth = 0.0f;       // -1 .. 1 of the target's own range
    ModSource via = ModSource::None;   // scales the depth (a second source as an amount)
    bool      unipolar = false;   // treat the source as 0..1 instead of -1..1
};

// Up to thirty-two rows. One source may appear in as many rows as it likes -- that is the whole
// point of a matrix, and what the soldered drifters could never do.
class ModMatrix {
public:
    int  count() const { return count_; }
    const ModRoute& route(int i) const { return routes_[i]; }
    void clear() { count_ = 0; }
    bool add(const ModRoute& r) { if (count_ >= kMaxModRoutes) return false; routes_[count_++] = r; return true; }
    bool remove(int i);

    // Text form, one row per ';': "<source>><target>:<depth>[:<via>][:u]"
    //   "lfo1>cutoff:0.4;env2>z_x:-0.25:macro_a;lfo3>shimmer:0.6:none:u"
    bool parse(const char* text);
    int  write(char* buf, size_t cap) const;

    // Sums every route into `out` (one entry per parameter, in the target's own units), given the
    // current value of each source. Targets outside the sound scope are ignored.
    void apply(const float* sourceValues, float* out) const;

private:
    ModRoute routes_[kMaxModRoutes];
    int count_ = 0;
};

} // namespace ambient
