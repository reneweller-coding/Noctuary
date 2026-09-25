/**
 * @file Dsp.h
 * @brief DSP primitives: PRNG, sine table, slow modulators, envelope, filter.
 *
 * Header-only, allocation-free, no framework dependencies.
 *
 * Everything in the engine that needs a random number, a sine, a slow random curve, an envelope,
 * a state-variable filter or a smoothed parameter takes it from here, so the sources, the effects,
 * the cloud and the body all move the same way and a seed reproduces the same set on every
 * machine. Nothing here allocates or keeps global state except the one sine table, which is built
 * on first use; every struct is small enough to live inside its owner and to be reset by
 * assignment.
 */
#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace ambient {

constexpr float  kPi    = 3.14159265358979f;   ///< pi in single precision
constexpr float  kTwoPi = 6.28318530717959f;   ///< 2 pi in single precision: radians per cycle

/**
 * @brief Clamps a value into [lo, hi]; the only clamp the engine uses, so that ints and floats read alike.
 * @tparam T  any type with operator<
 * @param v   the value
 * @param lo  the lower bound
 * @param hi  the upper bound (lo when hi < lo: the first comparison wins)
 * @return    v held inside the bounds
 */
template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
/**
 * @brief Decibels to a linear amplitude gain.
 * @param db  the level in dB (0 is unity, -6 about a half)
 * @return    10^(db / 20)
 */
inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }

/**
 * @brief xorshift64 -- deterministic per seed, one instance per owner (never shared across threads).
 *
 * Every module that draws (the brain, the cloud, the body, the patina ...) carries its own Rng,
 * seeded from the engine's seed through fork(), so the set is reproducible and no two owners ever
 * read the same stream.
 */
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;   ///< the generator's state; never zero after seed()
    /**
     * @brief Sets the state from a seed and discards the first two outputs.
     * @param v  any 64-bit seed; the state is scrambled with two odd constants so nearby seeds diverge, and a zero state is nudged to 1
     */
    void seed(uint64_t v) { s = v * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull; if (s == 0) s = 1; next(); next(); }
    /**
     * @brief Advances the generator by one step.
     * @return  the new 64-bit state, all bits usable
     */
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    /**
     * @brief A uniform draw in [0,1), from the top 24 bits of the state.
     * @return  a float in [0,1), never 1
     */
    float uniform() { return static_cast<float>((next() >> 40) * (1.0 / 16777216.0)); }   // [0,1)
    /**
     * @brief A uniform draw in [-1,1).
     * @return  uniform() x 2 - 1
     */
    float bipolar() { return uniform() * 2.0f - 1.0f; }
    /**
     * @brief A uniform integer below a bound.
     * @param n  the exclusive upper bound; 0 or less gives 0
     * @return   an int in [0, n)
     */
    int   below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }
    /**
     * @brief A seed for a child generator, so an owner can hand each of its parts its own stream.
     * @return  the next 64-bit output
     */
    uint64_t fork() { return next(); }
};

/**
 * @brief Sine lookup on a [0,1) phase. 4096 entries + guard, linear interpolation.
 *
 * Built once by sineTable(); read through sin01() and phasorFrom().
 */
struct SineTable {
    static constexpr int N = 4096;   ///< entries per cycle
    /**
     * Two guard entries, not one: the interpolation reads v[i] and v[i + 1], and a phase of
     * exactly 1.0 -- which the chorus produces as 1.0 - phase when the phase is zero -- lands
     * on i == N and read v[N + 1], one past the end of the table.
     */
    float v[N + 2];
    /** @brief Fills the table with one cycle of the sine, guards included. */
    SineTable() { for (int i = 0; i <= N + 1; ++i) v[i] = std::sin(kTwoPi * static_cast<float>(i) / static_cast<float>(N)); }
};
/**
 * @brief The one sine table of the process, built on first use.
 * @return  a reference to the static table
 */
inline const SineTable& sineTable() { static const SineTable t; return t; }
/**
 * @brief Cosine/sine of a phase in [0,1), from the sine table. Used to seed and to step the rotating
 *        phasors that stand in for every per-sample std::sin/std\::cos in the engine.
 * @param phase01  the phase in cycles, [0,1) (a phase outside is wrapped by sin01)
 * @param c        receives cos(2 pi phase01)
 * @param s        receives sin(2 pi phase01)
 */
inline void phasorFrom(double phase01, float& c, float& s);

/**
 * @brief sin(2 pi phase) from the table, linearly interpolated.
 * @param phase01  the phase in cycles; [0,1) is the fast path, anything else is wrapped first
 * @return         the sine, within the table's interpolation error (about -80 dB)
 */
inline float sin01(double phase01)
{
    double x = phase01 * SineTable::N;
    int i = static_cast<int>(x);
    // One unsigned comparison catches a phase below zero and one above one at the same time.
    // Both were reading outside the table; a caller that hands over a phase it has not
    // wrapped gets the wrapped answer rather than whatever was in memory.
    if (static_cast<unsigned>(i) > static_cast<unsigned>(SineTable::N)) {
        const double p = phase01 - std::floor(phase01);
        x = p * SineTable::N;
        i = static_cast<int>(x);
        if (static_cast<unsigned>(i) > static_cast<unsigned>(SineTable::N)) return 0.0f;
    }
    const float f = static_cast<float>(x - i);
    const float* t = sineTable().v;
    return t[i] + f * (t[i + 1] - t[i]);
}

inline void phasorFrom(double phase01, float& c, float& s)
{
    s = sin01(phase01);
    double q = phase01 + 0.25; if (q >= 1.0) q -= 1.0;
    c = sin01(q);
}

/**
 * @brief Smooth bounded random curve in [-1,1]: new random target every 1/rate seconds
 *        (period jittered +-30 %), smoothstep interpolation -> continuous with zero
 *        slope at the knots.
 *
 * This is the only kind of "randomness" allowed to reach a
 * pitch or amplitude: never a step.
 *
 * The owner calls init() once with its generator and update() at whatever interval it likes
 * (per sample, per block, per 64 samples), handing over the elapsed time each call.
 */
class Drifter {
public:
    /**
     * @brief Draws the first two knots and a random starting point between them.
     * @param r  the owner's generator
     */
    void init(Rng& r)
    {
        prev_ = r.bipolar(); next_ = r.bipolar();
        t_ = r.uniform(); scale_ = 0.7f + 0.6f * r.uniform();
    }
    /**
     * @brief Advances the curve by a stretch of time and returns its value.
     * @param dtSeconds  seconds since the last call
     * @param rateHz     knots per second on average; the actual period is jittered by the scale drawn at each knot
     * @param r          the owner's generator, for the next knot when one is passed
     * @return           the current value in [-1,1]
     */
    float update(float dtSeconds, float rateHz, Rng& r)
    {
        t_ += dtSeconds * rateHz * scale_;
        if (t_ >= 1.0f) {
            t_ -= std::floor(t_);
            prev_ = next_; next_ = r.bipolar();
            scale_ = 0.7f + 0.6f * r.uniform();
        }
        const float c = t_ * t_ * (3.0f - 2.0f * t_);
        value_ = prev_ + (next_ - prev_) * c;
        return value_;
    }
    /**
     * @brief The value of the last update(), without advancing.
     * @return  the current value in [-1,1]
     */
    float value() const { return value_; }
private:
    float prev_ = 0,    ///< the knot being left
          next_ = 0,    ///< the knot being approached
          t_ = 0,       ///< position between them, 0 .. 1
          scale_ = 1,   ///< this segment's period jitter, 0.7 .. 1.3
          value_ = 0;   ///< the last value returned
};

/**
 * @brief ADSR with exponential segments.
 *
 * Attack/decay/release may be minutes long.
 *
 * The attack is a one-pole towards 1.3 that ends when the level reaches 1, so it has a knee rather
 * than an asymptote; decay and release are one-poles towards the sustain and towards zero. All
 * times are in seconds and are turned into coefficients against the sample rate given to
 * setSampleRate(). process() runs once a sample and returns the level.
 */
class Envelope {
public:
    /** @brief Where the envelope is. */
    enum class Stage {
        Idle,      ///< silent, level 0; process() does nothing
        Attack,    ///< rising towards 1
        Decay,     ///< falling towards the sustain
        Sustain,   ///< holding the sustain until noteOff()
        Release    ///< falling towards silence
    };
    /**
     * @brief Sets the rate the coefficients are computed against; call before setTimes().
     * @param sr  the engine's rate in Hz
     */
    void setSampleRate(double sr) { sr_ = sr; }
    /**
     * @brief Sets the four segments.
     * @param attackS   attack time in seconds: how long the level takes to reach 1 (at least 0.5 ms)
     * @param decayS    decay time in seconds: the one-pole towards the sustain, three time constants
     * @param sustain   the sustain level, 0 .. 1; under 1e-4 the envelope goes Idle after the decay
     * @param releaseS  release time in seconds, five time constants
     */
    void setTimes(float attackS, float decayS, float sustain, float releaseS)
    {
        aCoef_ = coef(attackS, 1.466f);   // one-pole toward 1.3 reaches 1.0 after `attackS`
        dCoef_ = coef(decayS, 3.0f);
        rCoef_ = coef(releaseS, 5.0f);    // -43 dB after `releaseS`, silent (-80 dB) at ~1.8x
        sus_   = sustain;
    }
    /** @brief Starts the attack; continues from the current level, so a retriggered note does not dip. */
    void noteOn()  { stage_ = Stage::Attack; }
    /**
     * @brief Starts a note that has been sounding for a while already, at the level it would have.
     *
     * A note that has ALREADY been sounding for `seconds`, handed over rather than begun: it enters
     * at the level it would have reached by now instead of climbing its attack from silence. The
     * conductor of an arriving preset adopts the cluster of the one that is leaving -- those notes
     * are sounding, and starting them at zero is why a preset with a twelve-second attack dipped
     * four to five dB after a two-second crossfade. The attack is one pole toward 1.3, so its level
     * after n samples is 1.3 (1 - (1 - aCoef)^n) in closed form; past the attack it lands where the
     * decay would have left it, at the sustain.
     * @param seconds  how long the note has been sounding; 0 or less is a plain noteOn()
     */
    void noteOnAged(float seconds)
    {
        stage_ = Stage::Attack;
        if (!(seconds > 0.0f)) return;
        const double n = static_cast<double>(seconds) * sr_;
        const double reached = 1.3 * (1.0 - std::pow(1.0 - static_cast<double>(aCoef_), n));
        if (reached >= 1.0) { level_ = sus_; stage_ = sus_ > 1e-4f ? Stage::Sustain : Stage::Idle; if (sus_ <= 1e-4f) level_ = 0.0f; }
        else level_ = std::max(level_, static_cast<float>(reached));
    }
    /** @brief Enters the release from wherever the envelope is; nothing happens when it is Idle. */
    void noteOff() { if (stage_ != Stage::Idle) stage_ = Stage::Release; }
    /** @brief Silences the envelope at once: Idle, level 0. */
    void kill()    { stage_ = Stage::Idle; level_ = 0.0f; }
    /**
     * @brief Whether the envelope is in any stage but Idle.
     * @return  true while the note sounds, release included
     */
    bool isActive() const    { return stage_ != Stage::Idle; }
    /**
     * @brief Whether the envelope is in its release.
     * @return  true after noteOff() until the level has died
     */
    bool isReleasing() const { return stage_ == Stage::Release; }
    /**
     * @brief The current level without advancing.
     * @return  0 .. 1
     */
    float level() const      { return level_; }
    /**
     * @brief The current stage.
     * @return  the Stage
     */
    Stage stage() const      { return stage_; }

    /**
     * @brief Advances one sample.
     *
     * Audio thread, once a sample. The attack ends when the level reaches 1; the decay ends within
     * 1e-4 of the sustain; a sustain under 1e-4 goes Idle; the release ends under 1e-4.
     * @return  the level after the step, 0 .. 1
     */
    float process()
    {
        switch (stage_) {
        case Stage::Attack:
            level_ += (1.3f - level_) * aCoef_;
            if (level_ >= 1.0f) { level_ = 1.0f; stage_ = Stage::Decay; }
            break;
        case Stage::Decay:
            level_ += (sus_ - level_) * dCoef_;
            if (std::fabs(level_ - sus_) < 1e-4f) { level_ = sus_; stage_ = Stage::Sustain; }
            if (sus_ < 1e-4f && level_ < 1e-4f) { level_ = 0.0f; stage_ = Stage::Idle; }
            break;
        case Stage::Sustain:
            level_ = sus_;
            if (sus_ < 1e-4f) { level_ = 0.0f; stage_ = Stage::Idle; }
            break;
        case Stage::Release:
            level_ -= level_ * rCoef_;
            if (level_ < 1e-4f) { level_ = 0.0f; stage_ = Stage::Idle; }
            break;
        default: break;
        }
        return level_;
    }
private:
    /**
     * @brief One-pole coefficient for a segment.
     * @param seconds  the segment's nominal length (floored at 0.5 ms)
     * @param k        how many time constants the segment is taken to span
     * @return         1 - exp(-1 / (tau sr)) with tau = seconds / k
     */
    float coef(float seconds, float k) const
    {
        const float tau = std::max(seconds, 0.0005f) / k;
        return 1.0f - std::exp(-1.0f / (tau * static_cast<float>(sr_)));
    }
    double sr_ = 48000.0;   ///< sample rate the coefficients were computed for
    float aCoef_ = 0.01f,     ///< attack one-pole coefficient
          dCoef_ = 0.001f,    ///< decay one-pole coefficient
          rCoef_ = 0.0005f,   ///< release one-pole coefficient
          sus_ = 1.0f;        ///< the sustain level
    float level_ = 0.0f;   ///< the current level, 0 .. 1
    Stage stage_ = Stage::Idle;   ///< the current stage
};

/**
 * @brief TPT state-variable filter (Simper), low-pass output.
 *
 * Coefficients and state in one struct: an owner that runs several identical filters sets one and
 * copies the coefficients to the others (copyCoefficients). tick() gives all three outputs, lp()
 * the low pass alone a little cheaper.
 */
struct Svf {
    float ic1 = 0,   ///< the first integrator's state
          ic2 = 0;   ///< the second integrator's state
    float a1 = 0,     ///< 1 / (1 + g (g + k))
          a2 = 0,     ///< g a1
          a3 = 0,     ///< g a2
          k = 1.0f;   ///< the damping, 1 / Q; the band pass times k has unity gain at the cutoff
    /**
     * @brief Coefficients from a cutoff and a 0 .. 1 resonance.
     * @param cutoffHz   the cutoff in Hz, clamped between 10 Hz and 0.45 of the rate
     * @param resonance  0 .. 1, mapped to a damping of 2 (Q 0.5) down to 0.1 (Q 10)
     * @param sr         the sample rate in Hz
     */
    void set(float cutoffHz, float resonance, float sr)
    {
        setK(cutoffHz, 2.0f - 1.9f * clampv(resonance, 0.0f, 1.0f), sr);
    }
    /**
     * @brief Coefficients from a cutoff and a Q.
     * @param cutoffHz  the cutoff in Hz
     * @param q         the quality factor, floored at 0.05
     * @param sr        the sample rate in Hz
     */
    void setQ(float cutoffHz, float q, float sr) { setK(cutoffHz, 1.0f / std::max(q, 0.05f), sr); }
    /**
     * @brief Takes another filter's coefficients, leaving this one's state alone.
     * @param o  the filter to copy a1, a2, a3 and k from
     */
    void copyCoefficients(const Svf& o) { a1 = o.a1; a2 = o.a2; a3 = o.a3; k = o.k; }
    /**
     * @brief Coefficients from a cutoff and the damping itself.
     * @param cutoffHz  the cutoff in Hz, clamped between 10 Hz and 0.45 of the rate
     * @param damping   k = 1 / Q; 2 is critically damped
     * @param sr        the sample rate in Hz
     */
    void setK(float cutoffHz, float damping, float sr)
    {
        const float fc = clampv(cutoffHz, 10.0f, sr * 0.45f);
        const float g = std::tan(kPi * fc / sr);
        k = damping;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    /**
     * @brief All three outputs at once (band-pass has unity gain at the cutoff).
     * @param in  the input sample
     * @param lp  receives the low pass
     * @param bp  receives the band pass (multiply by k for unity at the cutoff)
     * @param hp  receives the high pass
     */
    inline void tick(float in, float& lp, float& bp, float& hp)
    {
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = in - k * v1 - v2;
    }
    /**
     * @brief The low pass alone.
     * @param in  the input sample
     * @return    the low-pass output
     */
    inline float lp(float in)
    {
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }
    /** @brief Clears the two integrators; the coefficients stay. */
    void reset() { ic1 = ic2 = 0.0f; }
};

/**
 * @brief Exponential parameter smoother.
 *
 * A one-pole towards the target, stepped once per call: what every knob goes through before it
 * reaches a coefficient, so a jump on the panel is a glide in the sound.
 */
struct Smoother {
    float value = 0,       ///< the current smoothed value
          coef = 0.001f;   ///< the one-pole coefficient, from setTime()
    /**
     * @brief Sets the time constant.
     * @param seconds  the time constant (floored at 0.1 ms): about 63 % of a step in this time, 99 % in five times it
     * @param sr       how many times a second next() will be called
     */
    void setTime(float seconds, double sr) { coef = 1.0f - std::exp(-1.0f / (std::max(seconds, 1e-4f) * static_cast<float>(sr))); }
    /**
     * @brief One step towards the target.
     * @param target  where the value is heading
     * @return        the value after the step
     */
    inline float next(float target) { value += (target - value) * coef; return value; }
    /**
     * @brief Jumps to a value without smoothing, as at prepare or a preset load.
     * @param v  the new value
     */
    void snap(float v) { value = v; }
};

/**
 * @brief A wavefolder.
 *
 * Where a clipper flattens what will not fit, a folder reflects it: the transfer
 * curve turns round and comes back, so the waveform is mirrored at the fold and the spectrum
 * fills with high partials that no amount of saturation produces. sin() is the smooth version of
 * that curve -- infinitely differentiable, so there is no corner anywhere to alias off -- and
 * dividing by the same gain keeps small signals untouched (sin(u)/u -> 1), which is what makes
 * the knob continuous from nothing.
 *
 * The positive half is driven a third harder than the negative one. A symmetric folder makes odd
 * harmonics only, and the sound stays hollow; the asymmetry adds the even ones, and with them the
 * second and fourth that the ear hears as body.
 *
 * The amount both drives the folder and mixes it in, so the knob leaves the identity continuously
 * -- at 0 this returns x itself, which is what lets it be added to an instrument full of finished
 * presets. The makeup gain is measured, not guessed: with 2.2, a 0.3-amplitude sine holds its RMS
 * to within 1.3 dB across the whole knob. A much louder input does lose level (about 9 dB at 0.8),
 * and that is not a fault to compensate: past the first fold the fundamental itself is being
 * folded away, which is exactly the effect being asked for.
 * @param x       the input sample
 * @param amount  the Fold knob, 0 .. 1: drive (1 + 9 amount, a third more on the positive half) and mix at once
 * @return        x itself at 0; otherwise x moved towards the folded, made-up copy by amount
 */
inline float wavefold(float x, float amount)
{
    if (amount <= 0.0f) return x;
    const float g = 1.0f + 9.0f * amount;
    const float a = x >= 0.0f ? g * 1.33f : g;
    const float y = std::sin(a * x) / a * (1.0f + 2.2f * amount);
    return x + amount * (y - x);
}

/**
 * @brief A cubic soft clip: x - 4/27 x^3, which reaches +-1 with zero slope at +-1.5 and is held there.
 *
 * The saturation of the voice filter's drive, the ladder's feedback and the patina: odd harmonics
 * that grow gently with the level, no corner until the hard limit.
 * @param x  the input sample
 * @return   the clipped sample, always within [-1, 1]
 */
inline float softClip(float x)
{
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - (4.0f / 27.0f) * x * x * x;
}

} // namespace ambient
