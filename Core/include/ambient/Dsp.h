// Noctuary -- DSP primitives: PRNG, sine table, slow modulators, envelope, filter.
// Header-only, allocation-free, no framework dependencies.
#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace ambient {

constexpr float  kPi    = 3.14159265358979f;
constexpr float  kTwoPi = 6.28318530717959f;

template <class T> inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }

// xorshift64 -- deterministic per seed, one instance per owner (never shared across threads).
struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    void seed(uint64_t v) { s = v * 0x9E3779B97F4A7C15ull + 0xD1B54A32D192ED03ull; if (s == 0) s = 1; next(); next(); }
    uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    float uniform() { return static_cast<float>((next() >> 40) * (1.0 / 16777216.0)); }   // [0,1)
    float bipolar() { return uniform() * 2.0f - 1.0f; }
    int   below(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint64_t>(n)); }
    uint64_t fork() { return next(); }
};

// Sine lookup on a [0,1) phase. 4096 entries + guard, linear interpolation.
struct SineTable {
    static constexpr int N = 4096;
    // Two guard entries, not one: the interpolation reads v[i] and v[i + 1], and a phase of
    // exactly 1.0 -- which the chorus produces as 1.0 - phase when the phase is zero -- lands
    // on i == N and read v[N + 1], one past the end of the table.
    float v[N + 2];
    SineTable() { for (int i = 0; i <= N + 1; ++i) v[i] = std::sin(kTwoPi * static_cast<float>(i) / static_cast<float>(N)); }
};
inline const SineTable& sineTable() { static const SineTable t; return t; }
// Cosine/sine of a phase in [0,1), from the sine table. Used to seed and to step the rotating
// phasors that stand in for every per-sample std::sin/std::cos in the engine.
inline void phasorFrom(double phase01, float& c, float& s);

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

// Smooth bounded random curve in [-1,1]: new random target every 1/rate seconds
// (period jittered +-30 %), smoothstep interpolation -> continuous with zero
// slope at the knots. This is the only kind of "randomness" allowed to reach a
// pitch or amplitude: never a step.
class Drifter {
public:
    void init(Rng& r)
    {
        prev_ = r.bipolar(); next_ = r.bipolar();
        t_ = r.uniform(); scale_ = 0.7f + 0.6f * r.uniform();
    }
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
    float value() const { return value_; }
private:
    float prev_ = 0, next_ = 0, t_ = 0, scale_ = 1, value_ = 0;
};

// ADSR with exponential segments. Attack/decay/release may be minutes long.
class Envelope {
public:
    enum class Stage { Idle, Attack, Decay, Sustain, Release };
    void setSampleRate(double sr) { sr_ = sr; }
    void setTimes(float attackS, float decayS, float sustain, float releaseS)
    {
        aCoef_ = coef(attackS, 1.466f);   // one-pole toward 1.3 reaches 1.0 after `attackS`
        dCoef_ = coef(decayS, 3.0f);
        rCoef_ = coef(releaseS, 5.0f);    // -43 dB after `releaseS`, silent (-80 dB) at ~1.8x
        sus_   = sustain;
    }
    void noteOn()  { stage_ = Stage::Attack; }            // continues from the current level
    // A note that has ALREADY been sounding for `seconds`, handed over rather than begun: it enters
    // at the level it would have reached by now instead of climbing its attack from silence. The
    // conductor of an arriving preset adopts the cluster of the one that is leaving -- those notes
    // are sounding, and starting them at zero is why a preset with a twelve-second attack dipped
    // four to five dB after a two-second crossfade. The attack is one pole toward 1.3, so its level
    // after n samples is 1.3 (1 - (1 - aCoef)^n) in closed form; past the attack it lands where the
    // decay would have left it, at the sustain.
    void noteOnAged(float seconds)
    {
        stage_ = Stage::Attack;
        if (!(seconds > 0.0f)) return;
        const double n = static_cast<double>(seconds) * sr_;
        const double reached = 1.3 * (1.0 - std::pow(1.0 - static_cast<double>(aCoef_), n));
        if (reached >= 1.0) { level_ = sus_; stage_ = sus_ > 1e-4f ? Stage::Sustain : Stage::Idle; if (sus_ <= 1e-4f) level_ = 0.0f; }
        else level_ = std::max(level_, static_cast<float>(reached));
    }
    void noteOff() { if (stage_ != Stage::Idle) stage_ = Stage::Release; }
    void kill()    { stage_ = Stage::Idle; level_ = 0.0f; }
    bool isActive() const    { return stage_ != Stage::Idle; }
    bool isReleasing() const { return stage_ == Stage::Release; }
    float level() const      { return level_; }
    Stage stage() const      { return stage_; }

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
    float coef(float seconds, float k) const
    {
        const float tau = std::max(seconds, 0.0005f) / k;
        return 1.0f - std::exp(-1.0f / (tau * static_cast<float>(sr_)));
    }
    double sr_ = 48000.0;
    float aCoef_ = 0.01f, dCoef_ = 0.001f, rCoef_ = 0.0005f, sus_ = 1.0f;
    float level_ = 0.0f;
    Stage stage_ = Stage::Idle;
};

// TPT state-variable filter (Simper), low-pass output.
struct Svf {
    float ic1 = 0, ic2 = 0;
    float a1 = 0, a2 = 0, a3 = 0, k = 1.0f;
    void set(float cutoffHz, float resonance, float sr)
    {
        setK(cutoffHz, 2.0f - 1.9f * clampv(resonance, 0.0f, 1.0f), sr);
    }
    void setQ(float cutoffHz, float q, float sr) { setK(cutoffHz, 1.0f / std::max(q, 0.05f), sr); }
    void copyCoefficients(const Svf& o) { a1 = o.a1; a2 = o.a2; a3 = o.a3; k = o.k; }
    void setK(float cutoffHz, float damping, float sr)
    {
        const float fc = clampv(cutoffHz, 10.0f, sr * 0.45f);
        const float g = std::tan(kPi * fc / sr);
        k = damping;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    // All three outputs at once (band-pass has unity gain at the cutoff).
    inline void tick(float in, float& lp, float& bp, float& hp)
    {
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2; bp = v1; hp = in - k * v1 - v2;
    }
    inline float lp(float in)
    {
        const float v3 = in - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return v2;
    }
    void reset() { ic1 = ic2 = 0.0f; }
};

// Exponential parameter smoother.
struct Smoother {
    float value = 0, coef = 0.001f;
    void setTime(float seconds, double sr) { coef = 1.0f - std::exp(-1.0f / (std::max(seconds, 1e-4f) * static_cast<float>(sr))); }
    inline float next(float target) { value += (target - value) * coef; return value; }
    void snap(float v) { value = v; }
};

// A wavefolder. Where a clipper flattens what will not fit, a folder reflects it: the transfer
// curve turns round and comes back, so the waveform is mirrored at the fold and the spectrum
// fills with high partials that no amount of saturation produces. sin() is the smooth version of
// that curve -- infinitely differentiable, so there is no corner anywhere to alias off -- and
// dividing by the same gain keeps small signals untouched (sin(u)/u -> 1), which is what makes
// the knob continuous from nothing.
//
// The positive half is driven a third harder than the negative one. A symmetric folder makes odd
// harmonics only, and the sound stays hollow; the asymmetry adds the even ones, and with them the
// second and fourth that the ear hears as body.
//
// The amount both drives the folder and mixes it in, so the knob leaves the identity continuously
// -- at 0 this returns x itself, which is what lets it be added to an instrument full of finished
// presets. The makeup gain is measured, not guessed: with 2.2, a 0.3-amplitude sine holds its RMS
// to within 1.3 dB across the whole knob. A much louder input does lose level (about 9 dB at 0.8),
// and that is not a fault to compensate: past the first fold the fundamental itself is being
// folded away, which is exactly the effect being asked for.
inline float wavefold(float x, float amount)
{
    if (amount <= 0.0f) return x;
    const float g = 1.0f + 9.0f * amount;
    const float a = x >= 0.0f ? g * 1.33f : g;
    const float y = std::sin(a * x) / a * (1.0f + 2.2f * amount);
    return x + amount * (y - x);
}

inline float softClip(float x)
{
    if (x > 1.5f) return 1.0f;
    if (x < -1.5f) return -1.0f;
    return x - (4.0f / 27.0f) * x * x * x;
}

} // namespace ambient
