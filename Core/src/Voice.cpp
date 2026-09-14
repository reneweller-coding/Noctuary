#include "ambient/Voice.h"
#include "ambient/Simd.h"
#include "ambient/Params.h"   // kStackRatios
#include "ambient/Tuning.h"   // intervalConsonance for the portamento gravity
#include <cmath>
#include <cstring>

namespace ambient {

namespace {
inline void phasorFromPhase(double phase01, float& c, float& s) { phasorFrom(phase01, c, s); }

// log2 of the harmonic numbers, so the presence bell needs one log2 per strand, not per partial.
struct Log2Harmonics {
    float v[kMaxPartials + 1];
    Log2Harmonics() { v[0] = 0.0f; for (int h = 1; h <= kMaxPartials; ++h) v[h] = std::log2(static_cast<float>(h)); }
};
const Log2Harmonics kLog2H;
constexpr float kPresenceCentreLog2 = 11.64386f;   // log2(3200 Hz): the bell spans about 1.8-5.6 kHz
constexpr float kPresenceHalfWidth  = 0.8f;        // octaves to the bell's zero

// Harmonic numbers as floats: phase modulation of the waveform by θ moves partial h by h·θ.
struct HarmonicNumbers {
    float v[kMaxPartials];
    HarmonicNumbers() { for (int h = 0; h < kMaxPartials; ++h) v[h] = static_cast<float>(h + 1); }
};
const HarmonicNumbers kHf;
constexpr float kFmMaxStep = 0.4f;   // per-sample deviation clamp (tan of the angle): keeps the two-step normalisation exact
constexpr float kSqrt2 = 1.41421356237f;
constexpr float kSqrt2Half = 0.70710678119f;
}

void Voice::prepare(double sampleRate, uint64_t seed)
{
    sr_ = sampleRate;
    // The modal bank derives every mode's angle and radius from its own copy of the rate, and
    // nothing ever set it: at 96 kHz the modes rang an octave high and half as long.
    zModal_.prepare(static_cast<float>(sr_));
    ildCoefConst_ = 1.0f - std::exp(-kTwoPi * 1000.0f / static_cast<float>(sr_));
    skyProto_.setQ(8000.0f, 1.5f, static_cast<float>(sr_));   // Blauert's band: fixed frequency, fixed Q
    rng_.seed(seed);
    env_.setSampleRate(sr_);
    for (auto& s : strands_) {
        for (int h = 0; h < kMaxPartials; ++h) {
            s.shimmer[h].init(rng_);
            phasorFromPhase(rng_.uniform(), s.pc[h], s.ps[h]);
            s.rc[h] = 1.0f; s.rs[h] = 0.0f;
            s.amp[h] = 0.0f;
            s.ampStep[h] = 0.0f;
        }
        s.pitch.init(rng_);
        s.active = 0;
    }
    filterDrift_.init(rng_);
    airDrift_.init(rng_);
    panCenter_.init(rng_);
    breath_.init(rng_);
    rateWander_.init(rng_);
    zDriftX_.init(rng_); zDriftY_.init(rng_);
    for (auto& r : zbL_) r.reset(); for (auto& r : zbR_) r.reset();
    // Slot 0 (Source 1) came later than the other two: it takes a seed derived from the voice's
    // rather than a fork from the stream, so the stream -- and with it every preset's random
    // phases and drifts -- stayed exactly where it was before the slot existed.
    slots_[0].prepare(sr_, seed ^ 0x9E3779B97F4A7C15ull);
    {   // the phase field's drifter seeds from a side stream, so the voice's stream is untouched
        Rng aux; aux.seed(seed ^ 0x51ED270B9E3779B9ull);
        phaseDrift_.init(aux);
    }
    ksOn_ = false; ksLen_ = 0;
    std::memset(apX_, 0, sizeof(apX_)); std::memset(apY_, 0, sizeof(apY_));
    // Slots 2 and 3 fork the voice's stream, as they always did. Slot 4 came later and seeds from
    // a side stream instead: one more fork here would advance the voice's stream by a draw and
    // move every random decision after it, and the oracle would report all 39 presets changed --
    // which is exactly what it did before this line was split.
    for (int k = 1; k < 3; ++k) slots_[k].prepare(sr_, rng_.fork());
    for (int k = 3; k < kSlots; ++k) slots_[k].prepare(sr_, seed ^ (0xC2B2AE3D27D4EB4Full + static_cast<uint64_t>(k)));
    filt_.prepare(sr_);
    airL_.reset();  airR_.reset();
    std::memset(itdBufL_, 0, sizeof(itdBufL_));
    std::memset(itdBufR_, 0, sizeof(itdBufR_));
    itdW_ = 0;
    itdL_ = itdR_ = itdLTarget_ = itdRTarget_ = 0.0f;
    cachedTilt_ = -1.0f; cachedOddEven_ = -9.0f; cachedPartials_ = -1; cachedB_ = -1.0f;
    env_.kill();
    note_ = -1;
}

// The plane as the ear would rank it. Heard distance grows with physical distance as roughly a
// power of a half (Zahorik 2002, 2005: the pooled exponent is about 0.54), so a knob that is
// linear in the model's distance spends most of its perceptual travel in its near half. Depth
// Law bends it back: d^(1 + 0.85 law), which at 1 is d^1.85, the inverse of that exponent. The
// ends stay where they are -- 0 is still the ear and 1 is still the horizon.
static inline float perceivedDistance(float d, float law)
{
    return law > 0.0f ? std::pow(clampv(d, 0.0f, 1.0f), 1.0f + 0.85f * clampv(law, 0.0f, 1.0f)) : d;
}

void Voice::noteOn(int note, double freqHz, float velocity, int owner, float distance, const VoiceParams& p,
                   bool allowStrike, float ageSeconds)
{
    const bool wasSounding = env_.isActive();   // asked before anything below can change it
    note_ = note;
    freq_ = freqTarget_ = freqHz;
    rootComp_ = 1.0f;            // a note beginning now takes the ratios to the root as it is now
    velocity_ = 0.3f + 0.7f * clampv(velocity, 0.0f, 1.0f);
    owner_ = owner;
    distance_ = clampv(distance, 0.0f, 1.0f);
    distTarget_ = distance_; distSeconds_ = 0.0f;
    distEff_  = perceivedDistance(distance_, p.depthLaw);
    gNear_  = std::cos(distEff_ * 0.5f * kPi);
    gFar_   = std::sin(distEff_ * 0.5f * kPi);
    gLevel_ = 1.0f - 0.5f * distEff_;
    // Which of the five roles this note belongs to, for the matrix and for the plane it stands in.
    // The second conductor is the shadow whatever it plays; everything else is read off the
    // register, because in this music the register IS the role (Rene's table). A near event
    // (owner 3) is the colour, wherever it sits.
    layer_ = (owner == 2) ? 1.0f                        // 2 = the second conductor (Engine::OwnerBrain2)
           : (owner == 3) ? 0.5f
           : (note < 43 ? 0.0f : note < 60 ? 0.25f : note < 84 ? 0.5f : 0.75f);
    // A near event's shape was set just before this; any other note starts with none of it, or a
    // voice that played an event a minute ago would keep its gate and its lift.
    if (owner != 3) { releaseMul_ = 1.0f; cutoffMul_ = 1.0f; proxDb_ = 0.0f; panOffset_ = 0.0f; nearGain_ = 1.0f; }
    // The near field's lift is a band, 120 to 300 Hz, and not a shelf: below it stand the
    // Foundation and Bass Mono, and a shelf there muddied the sub, which is where the depth lives.
    proxAmt_ = proxDb_ > 0.0f ? dbToGain(proxDb_) - 1.0f : 0.0f;
    proxLoCoef_ = 1.0f - std::exp(-kTwoPi * 120.0f / static_cast<float>(sr_));
    proxHiCoef_ = 1.0f - std::exp(-kTwoPi * 300.0f / static_cast<float>(sr_));
    {
        const float angle = (panOffset_ + 1.0f) * 0.25f * kPi;
        // The event's Gain rides on the pan pair: one multiplication that is already there, after
        // the sources and the filter, so it changes how loud the event is and nothing about it.
        panL_ = nearGain_ * 1.41421356f * std::cos(angle); panR_ = nearGain_ * 1.41421356f * std::sin(angle);
    }
    // The slot roles start where the note stands; control() fades them as the cluster changes.
    for (int k = 0; k < kSlots; ++k) roleGain_[k] = roleTarget(p.slot[k].role, placeLowest_, placeHighest_);
    // Velocity as an attack time rather than as a level (R7.1): the air floats in, the foundation
    // simply stands. Positive lets a quiet note enter slower and a loud one faster; at the extreme
    // the slowest is four times the written attack and the fastest a quarter of it. The factor is
    // fixed here, at the note's own velocity, because control() rewrites the times every block.
    attackMul_ = p.velAttack != 0.0f
               ? std::pow(4.0f, p.velAttack * (0.5f - clampv(velocity, 0.0f, 1.0f)) * 2.0f) : 1.0f;
    env_.setTimes(p.attack * attackMul_, p.decay, p.sustain, p.release);
    for (auto& s : slots_) s.noteOn(!env_.isActive());
    if (!env_.isActive()) {
        // Fresh start: random phases (no two voices share a waveform), silent partials.
        for (auto& s : strands_) {
            for (int h = 0; h < kMaxPartials; ++h) {
                phasorFromPhase(rng_.uniform(), s.pc[h], s.ps[h]);
                s.amp[h] = 0.0f; s.ampStep[h] = 0.0f;
            }
            s.active = 0;
        }
        filt_.reset();
        airL_.reset();  airR_.reset();
        std::memset(itdBufL_, 0, sizeof(itdBufL_));
        std::memset(itdBufR_, 0, sizeof(itdBufR_));
        proxLoL_ = proxLoR_ = proxHiL_ = proxHiR_ = 0.0f;
        // An inherited note has been sounding: its Bloom is open and its sources have entered.
        bloomT_ = ageSeconds;
        slotT_  = ageSeconds;
        for (float& s : slotShift_) s = 0.0f;
        slotHeld_ = false;
        prevDist_ = distance_;
    }
    press_ = pressTarget_; slide_ = slideTarget_; bend_ = bendTarget_;   // a new note starts where its controller is
    // An inherited note enters at the level its age says it has reached; anything else climbs its
    // attack as it always did. Only a preset change passes an age at all.
    if (ageSeconds > 0.0f && !wasSounding) env_.noteOnAged(ageSeconds);
    else env_.noteOn();
    if (p.strikeLevel > 0.0f && allowStrike && (owner == 0 || p.strikeBrain)) { strikeStart(freqHz, p); ++strikes_; }
}

// The strike: a burst of noise into a delay loop with a damping low pass (Karplus-Strong). String
// rings at the note; Wood is two octaves up, short and dull; Metal puts an all-pass in the loop,
// which stretches the partials into something clangorous. It plays on the near plane whatever
// the voice's distance -- the intimate impulse that makes the background behind it vast.
void Voice::strikeStart(double hz, const VoiceParams& p)
{
    const int type = clampv(p.strikeType, 0, 2);
    double f = hz;
    if (type == 1) f = clampv(hz * 4.0, 200.0, 2500.0);
    ksLen_ = clampv(static_cast<int>(sr_ / std::max(f, 20.0)), 8, kStrikeMax - 1);
    std::memset(ks_, 0, sizeof(float) * static_cast<size_t>(ksLen_));
    ksPos_ = 0;
    ksLeft_ = ksLen_;
    float decay = std::max(p.strikeDecay, 0.02f);
    if (type == 1) decay *= 0.35f;
    ksG_ = std::pow(10.0f, -3.0f * static_cast<float>(ksLen_) / (decay * static_cast<float>(sr_)));
    ksLpC_ = 1.0f - 0.9f * clampv(p.strikeDamp, 0.0f, 1.0f);
    if (type == 1) ksLpC_ = std::min(ksLpC_, 0.25f);
    ksApK_ = type == 2 ? 0.55f : 0.0f;
    ksLp_ = ksApX_ = ksApY_ = 0.0f;
    ksAmp_ = 0.6f * p.strikeLevel * velocity_;
    ksT_ = 0;
    ksTotal_ = static_cast<int>(decay * 1.5f * static_cast<float>(sr_)) + ksLen_;
    const float angle = (clampv(centre_, -1.0f, 1.0f) + 1.0f) * 0.25f * kPi;
    ksGainL_ = std::cos(angle); ksGainR_ = std::sin(angle);
    ksOn_ = true;
}

inline float Voice::strikeTick()
{
    float in = 0.0f;
    if (ksLeft_ > 0) { in = ksAmp_ * rng_.bipolar(); --ksLeft_; }
    const float d = ks_[ksPos_];
    ksLp_ += ksLpC_ * (d - ksLp_);
    float v = ksLp_;
    if (ksApK_ != 0.0f) { const float y = -ksApK_ * v + ksApX_ + ksApK_ * ksApY_; ksApX_ = v; ksApY_ = y; v = y; }
    ks_[ksPos_] = in + ksG_ * v;
    if (++ksPos_ >= ksLen_) ksPos_ = 0;
    if (++ksT_ > ksTotal_) ksOn_ = false;
    return d;
}

void Voice::noteOff() { env_.noteOff(); }
// Silence at once, and that includes a strike still ringing on its own physics: the render loop
// now runs while EITHER the envelope or the strike is going, and a reset has to stop both.
void Voice::kill()    { env_.kill(); note_ = -1; portaLeft_ = 0.0f; ksOn_ = false; }

void Voice::glideFrom(double fromHz, float seconds, float gravity)
{
    if (fromHz <= 0.0 || seconds <= 0.0f) return;
    portaFrom_ = fromHz; freq_ = fromHz;
    portaSeconds_ = seconds; portaLeft_ = seconds; portaGravity_ = clampv(gravity, 0.0f, 1.0f);
}

void Voice::glideTo(int note, double hz, float seconds, float gravity)
{
    if (hz <= 0.0 || !env_.isActive()) return;
    note_ = note;
    portaFrom_ = freq_; freqTarget_ = hz;
    portaSeconds_ = std::max(seconds, 0.05f); portaLeft_ = portaSeconds_; portaGravity_ = clampv(gravity, 0.0f, 1.0f);
}

void Voice::control(int blockLen, const VoiceParams& p)
{
    const float dtReal = static_cast<float>(blockLen / sr_);
    // Freeze: the movement clock stops (spectrum, pitch drift, breath, bloom hold still); the
    // envelope, filter and effects keep their own time.
    const float dt = p.freeze ? 0.0f : dtReal;
    env_.setTimes(p.attack * attackMul_, p.decay, p.sustain, p.release * releaseMul_);
    // A plane on its way somewhere (moveTo): an exponential approach that arrives, near enough,
    // in the seconds it was given.
    if (distSeconds_ > 0.0f && dtReal > 0.0f) {
        distance_ += (distTarget_ - distance_) * (1.0f - std::exp(-3.0f * dtReal / distSeconds_));
        if (std::fabs(distance_ - distTarget_) < 1.0e-3f) { distance_ = distTarget_; distSeconds_ = 0.0f; }
    }

    // Portamento: slide in the log domain from portaFrom_ to the target; the speed drops near
    // consonant ratios to the root (gravity), so the slide dwells on the harmonic nodes and
    // hurries across the dissonant stretches. The nominal time is what an even slide takes.
    if (portaLeft_ > 0.0f && dtReal > 0.0f) {
        const double total = std::log(freqTarget_ / portaFrom_);
        if (std::fabs(total) < 1e-9) { portaLeft_ = 0.0f; freq_ = freqTarget_; }
        else {
            // Gravity as an attraction to the nearest node, not as a function of how consonant
            // the present ratio happens to be. The first version braked in proportion to
            // intervalConsonance(), which rises and falls smoothly across the whole slide, so the
            // pull was everywhere and nowhere -- more like wading than like a magnet. A magnet
            // has almost no reach and then all of it: 1/(1 + (d/d0)^2) with d the distance in
            // cents to the nearest just ratio. Thirty cents away it is at half strength; a
            // semitone away it is down to eight per cent and the slide runs free.
            const double cents = 1200.0 * std::log2(std::max(freq_, 1.0) / std::max(p.rootHz, 1.0));
            double fold = std::fmod(cents, 1200.0);
            if (fold < 0.0) fold += 1200.0;
            static const double kNodes[] = { 0.0, 203.91, 315.64, 386.31, 498.04, 701.96, 813.69, 884.36, 996.09, 1200.0 };
            double dist = 1200.0;
            for (double node : kNodes) dist = std::min(dist, std::fabs(fold - node));
            const double pull = 1.0 / (1.0 + (dist / 30.0) * (dist / 30.0));
            const double slow = 1.0 - 0.90 * portaGravity_ * pull;
            const double step = total / portaSeconds_ * dtReal * slow;
            double lf = std::log(freq_) + step;
            const double lt = std::log(freqTarget_);
            if ((total > 0 && lf >= lt) || (total < 0 && lf <= lt)) { lf = lt; portaLeft_ = 0.0f; }
            freq_ = std::exp(lf);
            portaLeft_ = portaLeft_ > 0.0f ? std::max(portaLeft_ - dtReal * static_cast<float>(slow), 1e-4f) : 0.0f;
        }
    }
    // Retune glide (tuning purity / drift): log-domain one-pole toward the target, ~1 s.
    else if (freqTarget_ != freq_) {
        const double c = 1.0 - std::exp(-static_cast<double>(dtReal) / 1.0);
        freq_ = std::exp(std::log(freq_) + (std::log(freqTarget_) - std::log(freq_)) * c);
        if (std::fabs(freq_ - freqTarget_) < 1e-5 * freqTarget_) freq_ = freqTarget_;
    }

    // Breath: the voice's plane itself wanders slowly (Rich's "the room breathes"): everything
    // that hangs on the distance -- dry/wet balance, level, air absorption, presence -- moves
    // with it, continuously (Drifter = smoothstep curves, no steps).
    // Rate wander (nested modulation): one very slow curve per voice scales every movement
    // rate by up to +-1 octave, so five minutes never look like the five before.
    const float rw = rateWander_.update(dt, 0.01f, rng_);
    const float rateMul = p.rateWander > 0.0f ? std::pow(2.0f, rw * p.rateWander) : 1.0f;
    rateMul_ = rateMul;
    // Partial Spread: each partial of the bank gets its own place between the ears, on a
    // pattern that turns once every fifty seconds (the golden angle between neighbours, so no
    // two harmonically related partials sit together). Equal power per partial: the pair of
    // weights squares to two, which is what the mono path's identical left and right add up to.
    spreadAmt_ = clampv(p.partialSpread, 0.0f, 1.0f);
    if (spreadAmt_ > 0.0f) {
        spreadPhase_ += static_cast<double>(dt) * 0.02;
        if (spreadPhase_ >= 1.0) spreadPhase_ -= 1.0;
        for (int h = 0; h < kMaxPartials; ++h) {
            const float w = spreadAmt_ * std::sin(static_cast<float>(h) * 2.39996f + static_cast<float>(spreadPhase_) * kTwoPi);
            const float g = 1.0f / std::sqrt(1.0f + w * w);
            const float wl = (1.0f + w) * g, wr = (1.0f - w) * g;   // equal power: wl^2 + wr^2 = 2
            // Kept as the pair's sum and difference, which is all a strand needs to carry its own
            // place into it -- they are the cosine and sine of the partial's angle less 45 degrees.
            spreadSum_[h] = wl + wr;
            spreadDif_[h] = wl - wr;
        }
    }
    const float driftRate = p.driftRate * rateMul, shimmerRate = p.shimmerRate * rateMul;

    // Expression, smoothed towards what the controller last said (about 30 ms).
    if (!p.oneEuro) {
        const float c = clampv(dtReal / 0.03f, 0.0f, 1.0f);
        press_ += (pressTarget_ - press_) * c;
        slide_ += (slideTarget_ - slide_) * c;
        bend_  += (bendTarget_ - bend_) * c;
    } else {
        // The one-euro filter (Casiez, Roussel and Vogel 2012): a one-pole whose cutoff rises
        // with the speed of what it is filtering, so a hand at rest is smoothed hard -- one
        // hertz or so, which takes the sensor's jitter out of a held note -- and a hand that moves
        // is followed at once. Controllers speak in steps, not streams, so the speed is taken
        // from how far the value still has to travel rather than from a derivative of the
        // input, which between two messages would be zero and leave a fast gesture crawling.
        auto euro = [dtReal](float& y, float x, float unitsPerSecond) {
            const float fc = 0.6f + 30.0f * std::fabs(x - y) / unitsPerSecond;
            const float a = 1.0f - std::exp(-kTwoPi * fc * dtReal);
            y += a * (x - y);
        };
        euro(press_, pressTarget_, 1.0f);
        euro(slide_, slideTarget_, 1.0f);
        euro(bend_, bendTarget_, 12.0f);
    }
    const float bd = breath_.update(dt, p.breathRate * rateMul, rng_);
    // Pressure pulls the note towards the listener: the plane already decides brightness, level,
    // dryness and presence, so one finger moves all of them the way leaning into a note does.
    const float pressPull = p.pressDistance * press_;
    distEff_ = perceivedDistance(clampv(distance_ * (1.0f - pressPull) + 0.35f * p.breath * bd, 0.0f, 1.0f), p.depthLaw);
    // Doppler: the breathing distance has a velocity; a voice coming closer rises a little, one
    // receding falls. One plane unit is taken as about twenty metres.
    if (p.doppler > 0.0f && dtReal > 0.0f) {
        const float vel = (distEff_ - prevDist_) / dtReal;
        dopplerMul_ = clampv(1.0 - 0.06 * static_cast<double>(p.doppler * vel), 0.97, 1.03);
    } else dopplerMul_ = 1.0;
    prevDist_ = distEff_;
    gNear_  = std::cos(distEff_ * 0.5f * kPi);
    gFar_   = std::sin(distEff_ * 0.5f * kPi);
    gLevel_ = 1.0f - 0.5f * distEff_;
    // Presence: a broad bell in the 2-5 kHz articulation band, only on the near plane. Applied
    // in the additive domain (per partial), so it costs nothing per sample.
    const float presLin = p.presence > 0.0f ? std::pow(10.0f, p.presence * (1.0f - distEff_) / 20.0f) - 1.0f : 0.0f;
    const float lowCut = p.lowCut;

    // Bloom: the spectrum opens over bloomTime seconds (smoothstep, so the start is gentle).
    bloomT_ += dt;
    const float bt = clampv(bloomT_ / std::max(p.bloomTime, 1.0f), 0.0f, 1.0f);
    const float bloomOpen = bt * bt * (3.0f - 2.0f * bt);

    // Each slot's own entrance, from this note. A preset that says nothing about it -- no delay
    // and no shape -- gets a gain of exactly one and is not touched, which is what every preset
    // written before these parameters existed relies on.
    //
    // On real time, not the movement clock: an entrance is an envelope, and Freeze holds the
    // spectrum and the pitch still, not the envelopes (Help says so, and the amp envelope above
    // keeps its time). Read from bloomT_, which Freeze stops, a slot with a delay or a shape of
    // its own never entered while Freeze was on -- and a preset that had Freeze on from the
    // start (the 2.0 library writes it into six per cent of them) with every slot delayed or
    // shaped was silent for good. Five of the fourteen thousand were, and four more had only
    // their undelayed slot left.
    slotT_ += dtReal;
    {
        const bool held = env_.isActive() && !env_.isReleasing();
        for (int k = 0; k < kSlots; ++k) {
            const SlotParams& sp = p.slot[k];
            slotEnvT_[k] = -1.0f;
            if (sp.delaySec <= 0.0f && sp.envIndex < 0 && !sp.ownEnv) { slotGain_[k] = 1.0f; continue; }
            const float t = slotT_ - sp.delaySec;
            if (t <= 0.0f) { slotGain_[k] = 0.0f; continue; }
            // A shape: the slot's own, or one of the six borrowed. The own one may be a single
            // point -- a level -- where a borrowed one needs two to be a contour at all.
            const bool borrowed = !sp.ownEnv && sp.envIndex >= 0 && sp.envIndex < kNumModEnvs;
            const ModEnv* shape = sp.ownEnv ? p.srcEnvShape[k] : (borrowed ? p.envShape[sp.envIndex] : nullptr);
            const ModEnvSpec* spec = sp.ownEnv ? p.srcEnvSpec[k] : (borrowed ? p.envSpec[sp.envIndex] : nullptr);
            if (shape != nullptr && shape->count() > (sp.ownEnv ? 0 : 1)) {
                const float scale = spec != nullptr ? std::max(spec->timeScale, 0.01f) : 1.0f;
                const EnvMode mode = spec != nullptr ? spec->mode : EnvMode::OneShot;
                // Sustain Loop, the moment the note is let go: carry on from where the shape was
                // being read -- its sustain point, or wherever in its loop it had got to. The
                // note's clock ran on while the shape held, and reading the release from that
                // clock snapped the level to wherever the clock had got to in the tail.
                if (mode == EnvMode::SustainLoop && slotHeld_ && !held)
                    slotShift_[k] = t - shape->readTime((t - slotShift_[k]) / scale, mode, true) * scale;
                const float u = (t - slotShift_[k]) / scale;
                const float v = shape->at(u, mode, held);
                slotEnvT_[k] = shape->readTime(u, mode, held);   // where in the shape, for the editor's playhead
                if (sp.ownEnv) {
                    // A slot's own shape is a level: 0 is silence, 1 the slot at its written
                    // level, and Depth 0 leaves it at that level whatever the shape says.
                    const float depth = spec != nullptr ? clampv(spec->depth, 0.0f, 1.0f) : 1.0f;
                    slotGain_[k] = 1.0f - depth * (1.0f - clampv(v, 0.0f, 1.0f));
                } else {
                    // The six are bipolar, as modulation shapes are; read as a level, -1 is
                    // silence and +1 is the slot at its written level.
                    slotGain_[k] = clampv(0.5f + 0.5f * v, 0.0f, 1.0f);
                }
            } else {
                const float r = clampv(t / std::max(sp.riseSec, 0.01f), 0.0f, 1.0f);
                slotGain_[k] = r * r * (3.0f - 2.0f * r);   // smoothstep, as Bloom above
            }
        }
        slotHeld_ = held;
    }
    {   // The slot roles: each slot's gain from where the note stands, faded over a second and a
        // half so a note that stops being the top hands its chime on instead of dropping it.
        const float c = 1.0f - std::exp(-dtReal / 1.5f);
        for (int k = 0; k < kSlots; ++k) {
            const float t = roleTarget(p.slot[k].role, placeLowest_, placeHighest_);
            roleGain_[k] += (t - roleGain_[k]) * c;
            if (std::fabs(roleGain_[k] - t) < 1.0e-4f) roleGain_[k] = t;
        }
    }
    const float brightness = clampv(p.brightness * (1.0f - p.bloom * (1.0f - bloomOpen)) + p.cohBrightness
                                    + p.pressBright * press_, 0.0f, 1.0f);

    // Base spectrum shared by all strands of this voice. The tilt/odd-even shape is
    // cached (pow is expensive); only the brightness window is applied per block.
    const int partials = clampv(p.partials, 1, kMaxPartials);
    if (p.tilt != cachedTilt_ || p.oddEven != cachedOddEven_ || partials != cachedPartials_) {
        for (int h = 1; h <= partials; ++h) {
            float a = std::pow(static_cast<float>(h), -p.tilt);
            if (p.oddEven > 0.0f && (h % 2) == 0) a *= 1.0f - p.oddEven;
            if (p.oddEven < 0.0f && (h % 2) == 1 && h > 1) a *= 1.0f + p.oddEven;
            tiltCache_[h] = a;
        }
        cachedTilt_ = p.tilt; cachedOddEven_ = p.oddEven; cachedPartials_ = partials;
    }
    const float hc = 1.0f + brightness * brightness * 31.0f;   // brightness -> last full-level harmonic
    float base[kMaxPartials + 1];
    for (int h = 1; h <= partials; ++h) {
        float a = tiltCache_[h];
        if (static_cast<float>(h) > hc) {
            const float x = std::min((static_cast<float>(h) - hc) / 6.0f, 1.0f);
            a *= 0.5f * (1.0f + std::cos(kPi * x));
        }
        base[h] = a;
    }
    const float B = p.inharmonic * p.inharmonic * 0.02f;
    if (B != cachedB_) {
        for (int h = 1; h <= kMaxPartials; ++h) stretchCache_[h - 1] = B > 0.0f ? std::sqrt(1.0 + B * static_cast<double>(h * h)) : 1.0;
        cachedB_ = B;
    }
    const int unison = clampv(p.unison, 1, kMaxStrands);
    const float norm = p.level * (1.0f + p.pressLevel * press_) / std::sqrt(static_cast<float>(unison));
    const double nyq = 0.45 * sr_;
    const float invLen = 1.0f / static_cast<float>(blockLen);

    // The voice's centre wanders slowly; strands fan out around it. Source 1's Pan shifts the
    // centre, its Octave and Ratio move the whole bank, so the three slots read alike.
    const SlotParams& s1 = p.slot[0];
    const float centre = clampv(panCenter_.update(dt, driftRate * 0.3f, rng_) * p.panDrift + p.cohPan + s1.pan + panOffset_, -1.0f, 1.0f);
    centre_ = centre;
    const double bankMul = kSlotRatios[clampv(s1.ratio, 0, kNumSlotRatios - 1)] * std::pow(2.0, clampv(s1.octave, -2, 2))
                         * (static_cast<double>(p.pitchMul) * dopplerMul_)   // the tide and the doppler, 1.0 exactly when off
                         * (bend_ != 0.0f ? std::pow(2.0, static_cast<double>(bend_) / 12.0) : 1.0);
    const int stack = clampv(p.stack, 0, kNumStacks - 1);

    for (int si = 0; si < unison; ++si) {
        Strand& s = strands_[si];
        const float pos = (unison == 1) ? 0.0f : (2.0f * static_cast<float>(si) / static_cast<float>(unison - 1) - 1.0f);
        float cents = pos * p.detune + s.pitch.update(dt, driftRate, rng_) * p.drift;
        // Two limits on how far a strand may stand from its note, and both are about the beat
        // rather than about the pitch. The beat between a strand and its note is f (2^(c/1200) - 1),
        // so the same cents beat faster the higher the note sits: Low Detune thins the detuning
        // out towards the bottom, where warmth turns into wobble, and Beat Ceiling is the flat
        // upper bound on the rate itself, halved under 150 Hz (R4.6).
        if (p.lowDetune > 0.0f || p.beatCeiling > 0.0f) {
            const double fHz = freq_ * bankMul;
            if (p.lowDetune > 0.0f) {
                const double t = clampv(std::log2(std::max(fHz, 20.0) / 150.0), 0.0, 1.0);   // 0 at 150 Hz, 1 at 300
                cents *= 1.0f - 0.5f * p.lowDetune * static_cast<float>(1.0 - t);
            }
            if (p.beatCeiling > 0.0f && fHz > 0.0) {
                const double lim = static_cast<double>(p.beatCeiling) * (fHz < 150.0 ? 0.5 : 1.0);
                const double maxCents = 1200.0 * std::log2(1.0 + lim / fHz);
                if (std::fabs(cents) > maxCents) cents = static_cast<float>(cents > 0.0f ? maxCents : -maxCents);
            }
        }
        // Stack: the strand sits at a pure ratio to the note (a just chord from one key);
        // detune and drift still apply on top, so Detune 0 makes it beat-free.
        const double f = freq_ * bankMul * kStackRatios[stack][si] * std::pow(2.0, cents / 1200.0);
        const float pan = clampv(centre + pos * p.spread, -1.0f, 1.0f);
        const float angle = (pan + 1.0f) * 0.25f * kPi;
        s.gainL = std::cos(angle) * norm;
        s.gainR = std::sin(angle) * norm;
        // Partial Spread and the strand's own place, composed instead of multiplied. The spread
        // gives partial h a pair of weights, which is an angle in the field; the strand's pan is
        // another; and what the ear should get is their SUM. Multiplying the pair by the strand's
        // two gains one side at a time is not that: it attenuates every partial by how far it
        // leans away from the pan -- eleven decibels at the outer strand of a six-strand fan, and
        // since the pattern turns once every fifty seconds, coming and going. Which is why the
        // test for this passed: it placed one strand in the middle, and in the middle the two
        // agree exactly.
        //   Composing two angles needs no trigonometry of its own: cos and sin of (angle + phi -
        // 45 degrees) come out of the pair's own sum and difference. Past an ear the sum would
        // carry on to the far side and arrive inverted, which is a partial cancelling itself in
        // mono, so it is stopped AT the ear -- and a stop is still a place, so every partial
        // keeps cos^2 + sin^2 = 1 of its power wherever the pan puts it. A centred strand comes
        // out exactly as it did; a hard-panned one squeezes its field onto that ear.
        if (spreadAmt_ > 0.0f) {
            const float ca = std::cos(angle), sa = std::sin(angle);
            for (int h = 0; h < kMaxPartials; ++h) {
                float l = kSqrt2Half * (ca * spreadSum_[h] + sa * spreadDif_[h]);
                float r = kSqrt2Half * (sa * spreadSum_[h] - ca * spreadDif_[h]);
                if (l < 0.0f)      { l = 0.0f;   r = kSqrt2; }
                else if (r < 0.0f) { l = kSqrt2; r = 0.0f;   }
                s.wL[h] = l; s.wR[h] = r;
            }
            s.spreadGain = norm * kSqrt2Half;
        }

        float target[kMaxPartials];
        float sumSq = 0.0f;
        int H = 0;
        const float lf0 = std::log2(static_cast<float>(f)) - kPresenceCentreLog2;
        for (int h = 1; h <= partials; ++h) {
            // The branch, not a blend: (f * h) * s and f * (h * s) are not the same double, and
            // the oracle would hear the last bit move on six thousand presets that never asked
            // for Match at all.
            const double fh = p.match > 0.0f ? f * static_cast<double>(p.partialRatio[h - 1]) : f * h * stretchCache_[h - 1];
            if (fh >= nyq) break;
            // Rotation per sample for this partial, and keep the phasor on the unit circle.
            const double inc = fh / sr_;
            phasorFromPhase(inc, s.rc[h - 1], s.rs[h - 1]);
            const float r2 = s.pc[h - 1] * s.pc[h - 1] + s.ps[h - 1] * s.ps[h - 1];
            const float fix = 1.5f - 0.5f * r2;
            s.pc[h - 1] *= fix; s.ps[h - 1] *= fix;
            const float d = s.shimmer[h - 1].update(dt, shimmerRate, rng_);
            float a = base[h] * (1.0f + 0.9f * p.shimmer * d);
            if (presLin > 0.0f) {
                const float x = (lf0 + kLog2H.v[h]) / kPresenceHalfWidth;
                if (x > -1.0f && x < 1.0f) a *= 1.0f + presLin * (1.0f - x * x);
            }
            if (lowCut > 0.0f && fh < lowCut) {
                const float r = static_cast<float>(fh) / lowCut;
                a *= r * r;   // 12 dB/oct below the cut
            }
            target[h - 1] = a;
            sumSq += a * a;
            H = h;
        }
        const float scale = sumSq > 0.0f ? 0.5f / std::sqrt(sumSq) : 0.0f;
        for (int h = 0; h < kMaxPartials; ++h) {
            const float tgt = (h < H) ? target[h] * scale : 0.0f;
            s.ampStep[h] = (tgt - s.amp[h]) * invLen;
        }
        int act = H;
        for (int h = H; h < s.active; ++h) if (std::fabs(s.amp[h]) > 1e-6f) act = h + 1;
        s.active = act;
    }
    if (unison < lastUnison_)
        for (int si = unison; si < lastUnison_; ++si)
            for (int h = 0; h < kMaxPartials; ++h) { strands_[si].amp[h] = 0.0f; strands_[si].ampStep[h] = 0.0f; }
    lastUnison_ = unison;

    // Interaural time difference from the centre pan: the far ear hears it later.
    float lateral = centre;          // -1..1: where the source is, left to right
    bool  behind = false;
    float shadowOct = 2.7f * p.itd;
    if (p.binaural) {
        // Headphones: the pan is an azimuth (90 degrees at full pan) and the head's yaw turns
        // the field the other way, so a source stays put in the room while the head moves.
        float az = centre * 90.0f - p.headYawDeg;
        while (az > 180.0f) az -= 360.0f;
        while (az < -180.0f) az += 360.0f;
        behind = std::fabs(az) > 90.0f;
        const float folded = behind ? (az > 0.0f ? 180.0f - az : -180.0f - az) : az;   // -90..90
        const float th = folded * (kPi / 180.0f);
        // Woodworth's spherical head: tau = (a/c)(theta + sin theta), a = 8.75 cm, which is
        // 0.656 ms at ninety degrees -- the figure the plain mode's 0.65 ms came from.
        const float tau = (0.0875f / 343.0f) * (std::fabs(th) + std::sin(std::fabs(th)));
        const float itdSamples = tau * static_cast<float>(sr_);
        itdLTarget_ = th > 0.0f ? itdSamples : 0.0f;
        itdRTarget_ = th < 0.0f ? itdSamples : 0.0f;
        lateral = std::sin(th);
        shadowOct = 2.7f;            // the head is the head, whatever Time Width says
    } else {
        const float maxItd = 0.00065f * static_cast<float>(sr_) * p.itd;
        itdLTarget_ = centre > 0.0f ?  centre * maxItd : 0.0f;
        itdRTarget_ = centre < 0.0f ? -centre * maxItd : 0.0f;
    }
    // Head shadow. In the plain mode a one-pole on the far ear, which is enough for a pan.
    // In the binaural mode the spherical-head filter of Brown and Duda (1998): a sphere of
    // radius a diffracts sound into a shadow whose transfer function is well approximated by
    //     H(s) = (1 + alpha beta s) / (1 + beta s),   beta = a / (2 c),
    // where alpha depends on the angle of incidence at that ear --
    //     alpha(t) = 1 + amin/2 + (1 - amin/2) cos(t / tmin * pi),  amin = 0.1, tmin = 150 deg --
    // so the ipsilateral ear is lifted (alpha near 2), the contralateral one shadowed (alpha
    // near 0.1) and the shadow is a shelf rather than a low pass: the far ear keeps a little of
    // its top, which is what a real head does and what a plain one-pole cannot.
    sphereShadow_ = p.binaural;
    if (sphereShadow_) {
        const float az = std::asin(clampv(lateral, -1.0f, 1.0f));       // radians, +right
        const float c = static_cast<float>(sr_) * (0.0875f / 343.0f);   // 1 / (w0 T), w0 = c/a
        for (int ch = 0; ch < 2; ++ch) {
            // Angle of incidence at this ear: 0 when the source is at the ear itself.
            const float earSign = (ch == 0) ? -1.0f : 1.0f;             // left ear points left
            float theta = std::fabs(az - earSign * 0.5f * kPi) * (180.0f / kPi);
            if (theta > 180.0f) theta = 360.0f - theta;
            const float amin = 0.1f, tmin = 150.0f;
            const float alpha = 1.0f + 0.5f * amin + (1.0f - 0.5f * amin) * std::cos(theta / tmin * kPi);
            const float den = 1.0f + c;
            sphB0_[ch] = (1.0f + alpha * c) / den;
            sphB1_[ch] = (1.0f - alpha * c) / den;
            sphA1_[ch] = (1.0f - c) / den;
        }
    }
    const float fcL = 20000.0f * std::pow(2.0f, -shadowOct * std::max(lateral, 0.0f));
    const float fcR = 20000.0f * std::pow(2.0f, -shadowOct * std::max(-lateral, 0.0f));
    shadowCoefL_ = fcL >= 19000.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * fcL / static_cast<float>(sr_));
    shadowCoefR_ = fcR >= 19000.0f ? 1.0f : 1.0f - std::exp(-kTwoPi * fcR / static_cast<float>(sr_));
    // The near field. The head model above is a far-field one: plane waves, and a level
    // difference that lives above a kilohertz or so where the head is big enough to shadow.
    // Within a metre the wavefront is curved, the two ears are at measurably different
    // distances from the source, and the level difference grows at EVERY frequency, most of
    // all at the low ones the far-field head does not touch -- twenty decibels and more at
    // two hundred hertz for a source near the ear (Brungart and Rabinowitz 1999). That growth
    // is the ear's own cue for "within reach", which no amount of presence or loudness gives
    // it. So: a shelf below a kilohertz, cut on the far ear by up to eighteen decibels and
    // lifted on the near one by up to four, scaled by how far to the side the source is and by
    // the square of its nearness, so it is gone by the middle of the room.
    ildAmt_ = clampv(p.nearIld, 0.0f, 1.0f);
    if (ildAmt_ > 0.0f) {
        const float prox = 1.0f - clampv(distEff_, 0.0f, 1.0f);
        const float lat = clampv(lateral, -1.0f, 1.0f);
        const float amount = ildAmt_ * prox * prox * std::fabs(lat);
        const float cut = 1.0f - std::pow(10.0f, -18.0f * amount / 20.0f);    // fraction of the lows taken from the far ear
        const float lift = std::pow(10.0f, 4.0f * amount / 20.0f) - 1.0f;     // fraction added to the near one
        ildL_ = lat > 0.0f ? cut : -lift;
        ildR_ = lat < 0.0f ? cut : -lift;
        ildCoef_ = ildCoefConst_;
    }
    // Externalisation: the pinna's notch sits near 7 kHz for a source in front and climbs towards
    // 9 kHz as it moves to the side; the shoulder reflection arrives about a quarter of a
    // millisecond later. Together they are most of what tells the ear a sound is outside the head.
    extAmt_ = clampv(p.externalise, 0.0f, 1.0f);
    // Height, from where the voice stands between the two planes. It is heard through the same
    // pinna notch as externalisation -- the notch IS the elevation cue -- so a raised voice brings
    // that path in on its own, shoulder or no shoulder.
    const float elev = clampv(p.elevNear + (p.elevFar - p.elevNear) * distEff_, -1.0f, 1.0f);
    pinnaAmt_ = std::max(extAmt_, std::fabs(elev));
    skyGain_ = 0.0f;
    if (pinnaAmt_ > 0.0f) {
        const float lat = std::fabs(lateral);
        // Behind the head the pinna's notch sits lower (Brown and Duda 1998), which is most of
        // what tells front from back with no visual cue; only the binaural mode knows about
        // behind, the plain mode's pan has no back. And it rises with height (Hebrank and
        // Wright 1974): about three kilohertz from the horizon to overhead.
        const float notch = 7000.0f + 2000.0f * lat - (behind ? 1800.0f : 0.0f) + 3000.0f * elev;
        pinnaL_.setQ(clampv(notch, 3000.0f, 0.45f * static_cast<float>(sr_)), 2.2f, static_cast<float>(sr_));
        pinnaR_.copyCoefficients(pinnaL_);
        shoulder_ = std::max(1, static_cast<int>(0.00026 * sr_));
        // Blauert's directional band for "above" sits near 8 kHz: lifted for a source overhead,
        // cut for one below. Guarded at zero so a flat field adds nothing, not even a zero.
        if (elev != 0.0f) {
            skyL_.copyCoefficients(skyProto_);
            skyR_.copyCoefficients(skyL_);
            skyGain_ = 0.6f * elev;
        }
    }

    // Filter: cutoff follows key, envelope, a slow drift, and distance (air absorption).
    const float fd = filterDrift_.update(dt, driftRate * 0.5f, rng_);
    // note_ is given back the moment the envelope is done, and a strike may still be ringing out
    // after that: no key tracking in that case rather than tracking the filter down to note -1.
    const float octaves = p.keyTrack * static_cast<float>((note_ >= 0 ? note_ : 60) - 60) / 12.0f
                        + p.filterEnv * 4.0f * env_.level()
                        + p.filterDrift * 2.0f * fd
                        + p.slideCutoff * slide_
                        - 2.5f * distEff_;
    const float cut = p.cutoff * cutoffMul_ * std::pow(2.0f, octaves);
    filt_.set(static_cast<FilterModel>(clampv(p.filterModel, 0, kNumFilterModels - 1)), cut, p.resonance, p.filterDrive);
    // Binaural phase field: the two ears' all-pass corners drift apart and back on one slow curve;
    // the phase relation changes, not the level, so the room seems to breathe in size.
    phaseOn_ = p.phaseWidth > 0.0f;
    if (phaseOn_) {
        const float d = phaseDrift_.update(dt, p.phaseRate * rateMul, rng_) * p.phaseWidth * 1.5f;   // octaves apart
        const float sr = static_cast<float>(sr_);
        for (int ch = 0; ch < 2; ++ch) {
            const float sgn = ch == 0 ? 1.0f : -1.0f;
            const float f1 = clampv(300.0f * std::pow(2.0f, sgn * d), 40.0f, 0.4f * sr), f2 = clampv(1500.0f * std::pow(2.0f, sgn * d), 40.0f, 0.4f * sr);
            const float t1 = std::tan(kPi * f1 / sr), t2 = std::tan(kPi * f2 / sr);
            apC_[ch][0] = (t1 - 1.0f) / (t1 + 1.0f);
            apC_[ch][1] = (t2 - 1.0f) / (t2 + 1.0f);
        }
    }
    if (p.filterOn != lastFilterOn_) { if (p.filterOn) filt_.reset(); lastFilterOn_ = p.filterOn; }   // switched back in: from rest

    // Z-plane: the point wanders around (X, Y) on two Drifters, the frame is interpolated from
    // the shape's corners, resonance narrows the bandwidths, key tracking moves the frame with
    // the note. Mode changes ramp the wet/dry gains so switching never clicks.
    zModeCur_ = clampv(p.zMode, 0, 3);   // 3 = Modal
    if (zModeCur_ != 0) {
        const float dx = zDriftX_.update(dt, p.zRate * rateMul, rng_), dy = zDriftY_.update(dt, p.zRate * 0.77f * rateMul, rng_);
        const float x = clampv(p.zX + 0.5f * p.zDepth * dx + p.cohZ + p.slideZ * slide_, 0.0f, 1.0f), y = clampv(p.zY + 0.5f * p.zDepth * dy - p.cohZ, 0.0f, 1.0f);
        const ZFrame f = zInterpolate(p.zShape, x, y, p.zZ);
        const float track = std::pow(static_cast<float>(freq_) / 261.6256f, p.zKeyTrack);
        const float bwScale = std::pow(2.0f, 2.0f * (0.5f - p.zRes));   // resonance 1 -> quarter bandwidth, 0 -> double
        const float sr = static_cast<float>(sr_);
        ZFrame scaled = f;   // key tracking and resonance move the whole frame
        for (int i = 0; i < scaled.used; ++i) {
            ZSection& s = scaled.s[i];
            s.poleHz *= track; s.poleBw = std::max(s.poleBw * bwScale * track, 0.5f);
            if (s.zeroHz > 0.0f) { s.zeroHz *= track; s.zeroBw = std::max(s.zeroBw * track, 0.5f); }
        }
        if (zModeCur_ == 3) {
            // Modal: the frame's frequencies are modes of a ringing object, not the corners of a
            // filter. Nothing is cascaded -- the resonators sit in parallel and add.
            zModal_.set(scaled, p.zDecay, p.zDamp);
            zUsed_ = zModal_.used();
            zNorm_ = 1.0f;                      // the bank normalises itself on expected power
        } else {
            zUsed_ = scaled.used;
            zNorm_ = zBuildCascade(scaled, zbL_, sr);
            for (int i = 0; i < zUsed_; ++i) zbR_[i].copyCoefficients(zbL_[i]);
        }
        zWet_ = p.zMix; zDry_ = 1.0f - p.zMix;
    } else {
        zWet_ = 0.0f; zDry_ = 1.0f; zUsed_ = 0;
    }

    // Air: band-passed noise around a drifting multiple of the fundamental -- or, in Ghost mode,
    // noise through six sharp resonators on the note's harmonics 1 2 3 5 7 9: the harmony is
    // filtered out of the chaos (Rich's string and pipe resonances). Q from Air Q, x8.
    ghostGain_ = 0.0f;
    if (p.air > 0.0f && p.airGhost) {
        static const float hs[6] = { 1.0f, 2.0f, 3.0f, 5.0f, 7.0f, 9.0f };
        const float q = clampv(p.airQ, 1.0f, 40.0f) * 8.0f;
        float expected = 0.0f;
        for (int i = 0; i < 6; ++i) {
            const float fc = clampv(static_cast<float>(freq_) * hs[i], 30.0f, static_cast<float>(nyq));
            const float bw = fc / q, gain = 1.0f / std::sqrt(hs[i]);
            ghostL_[i].set(fc, bw, gain, static_cast<float>(sr_));
            ghostR_[i].b0 = ghostL_[i].b0; ghostR_[i].a1 = ghostL_[i].a1; ghostR_[i].a2 = ghostL_[i].a2;
            expected += gain * gain * kPi * bw / (2.0f * static_cast<float>(sr_));   // noise power through a resonator of bandwidth bw
        }
        const float expectedRms = std::sqrt(expected / 3.0f);   // bipolar noise has power 1/3
        ghostGain_ = p.air * std::min(0.05f / std::max(expectedRms, 1e-4f), 60.0f);
        airGain_ = 0.0f;
    } else if (p.air > 0.0f) {
        const float ad = airDrift_.update(dt, driftRate * 0.7f, rng_);
        const float fc = clampv(static_cast<float>(freq_) * p.airColor * std::pow(2.0f, 0.5f * ad), 40.0f, static_cast<float>(nyq));
        const float q = clampv(p.airQ, 1.0f, 40.0f);
        airL_.setQ(fc, q, static_cast<float>(sr_));
        airR_.copyCoefficients(airL_);
        // Normalise the expected band-passed noise level so `air` reads as a level.
        const float expectedRms = std::sqrt((1.0f / 3.0f) * kPi * fc / (q * static_cast<float>(sr_)));
        airGain_ = p.air * std::min(0.05f / std::max(expectedRms, 1e-4f), 40.0f);
    } else {
        airGain_ = 0.0f;
    }
}

void Voice::render(float* nearL, float* nearR, float* farL, float* farR, int n, const VoiceParams& p,
                   const float* fm, const float* couple)
{
    // The coupling is capped low and enters before the filter, so what comes back is not the
    // other voices but this voice's answer to them.
    const float sympathy = couple != nullptr ? clampv(p.sympathy, 0.0f, 1.0f) * 0.12f : 0.0f;
    const bool doFm = fm != nullptr && p.fmAmount > 0.0f;
    const float fmScale = p.fmAmount * 3.0f;   // radians at the fundamental per unit of feedback signal
    fmHpCoef_ = 1.0f - kTwoPi * 10.0f / static_cast<float>(sr_);
    if (!doFm) { fmHpXL_ = fmHpXR_ = fmHpYL_ = fmHpYR_ = 0.0f; }
    int pos = 0;
    while (pos < n && (env_.isActive() || ksOn_)) {
        const int len = std::min(kControlBlock, n - pos);
        control(len, p);
        // Source 1 is the strand bank only while its type is Additive; any other type renders in
        // the slot below and the bank stays silent.
        const bool bank = p.slot[0].type == SourceType::Additive;
        const int unison = bank ? clampv(p.unison, 1, kMaxStrands) : 0;
        // The two filters: each on or off, and with both on either in series (the z-plane hears
        // the filter) or in parallel (both hear the dry sum, Mix balances them).
        const bool fOn = p.filterOn && zModeCur_ != 2;
        const bool zOn = zModeCur_ != 0;
        const bool parallel = p.filterParallel;
        const bool air = airGain_ > 0.0f;
        // The wavefolder: not a saturation but a mirror. Past its threshold the wave is turned
        // back on itself, so a sine grows a whole family of upper partials instead of shoulders,
        // which is the metallic, industrial edge the Drive knob could never reach. It sits after
        // both filters, where the material it is folding already has a shape.
        const float foldAmt = clampv(p.fold, 0.0f, 1.0f);
        const bool ghost = ghostGain_ > 0.0f;
        // Extra sources render block-wise into their own buffers, then join the strands
        // before the filter (they share filter, envelope, distance and ITD with the bank).
        float slotL[kControlBlock], slotR[kControlBlock];
        bool anySlot = false;
        for (int k = 0; k < kSlots; ++k) {
            const SlotParams& sp = p.slot[k];
            // A slot whose role has faded out is left idle like an Off one, not rendered at nothing.
            if (sp.type == SourceType::Off || (k == 0 && bank) || roleGain_[k] < 1.0e-3f) { slots_[k].render(nullptr, nullptr, 0, freq_, sp, nullptr, nullptr, 0.0f); continue; }
            if (!anySlot) { std::memset(slotL, 0, sizeof(float) * static_cast<size_t>(len)); std::memset(slotR, 0, sizeof(float) * static_cast<size_t>(len)); anySlot = true; }
            const Wavetable* table = sp.table >= kNumTables - 1 ? p.userTable : &builtinTable(sp.table);
            const CycleTable* cycles = sp.type != SourceType::Wavetable ? nullptr
                                     : sp.table >= kNumTables - 1 ? p.userCycles : &builtinCycleTable(sp.table);
            // The slot's own entrance scales its level. A copy rather than a gain on the output,
            // because a grain that has already been given its gain keeps it until it dies: fading
            // the buffer would fade grains that are half over, fading the level lets the ones
            // already sounding finish and starts the new ones quieter, which is how an entrance
            // is heard. Costs one struct copy per slot per control block.
            SlotParams entered = sp;
            entered.level *= slotGain_[k] * roleGain_[k];
            slots_[k].render(slotL, slotR, len, freq_ * (static_cast<double>(p.pitchMul) * dopplerMul_), entered, table, p.texture[k], p.driftRate * rateMul_, cycles);
        }
        for (int i = 0; i < len; ++i) {
            const float e = env_.process();
            const float th = doFm ? fm[pos + i] * fmScale : 0.0f;
            float accL = anySlot ? slotL[i] : 0.0f, accR = anySlot ? slotR[i] : 0.0f;
            if (sympathy > 0.0f) { const float c = couple[pos + i] * sympathy * e; accL += c; accR += c; }
            for (int si = 0; si < unison; ++si) {
                Strand& s = strands_[si];
                float sum = 0.0f;
                const int act = s.active;
                float* pc = s.pc; float* ps = s.ps; const float* rc = s.rc; const float* rs = s.rs;
                float* amp = s.amp; const float* step = s.ampStep;
                if (doFm) {
                    // Rotate by the partial's own step, then by the modulation angle h·θ (small-angle
                    // rotation by tan = t, clamped, followed by two Newton steps of 1/sqrt so the
                    // phasor stays on the unit circle within 1e-4 per sample). The clamp makes deep
                    // modulation saturate softly on the high partials instead of tearing.
                    // Written out in Simd.h since 13.09.2026: it was the dearest of the bank's
                    // three loops and the only one still scalar on every platform.
                    sum = phasorBankStepFm(pc, ps, rc, rs, amp, step, act, kHf.v, th, kFmMaxStep);
                } else if (spreadAmt_ > 0.0f) {
                    float sumL, sumR;
                    phasorBankStepStereo(pc, ps, rc, rs, amp, step, act, s.wL, s.wR, sumL, sumR);
                    accL += sumL * s.spreadGain;
                    accR += sumR * s.spreadGain;
                    continue;
                } else {
                    sum = phasorBankStep(pc, ps, rc, rs, amp, step, act);
                }
                accL += sum * s.gainL;
                accR += sum * s.gainR;
            }
            float outL, outR;
            if (fOn && zOn) {
                filt_.tick(accL, accR, outL, outR);
                float zl = parallel ? accL : outL, zr = parallel ? accR : outR;
                zRun(zl, zr);
                outL = outL * zDry_ + zl * zNorm_ * zWet_;
                outR = outR * zDry_ + zr * zNorm_ * zWet_;
            } else if (fOn) {
                filt_.tick(accL, accR, outL, outR);
            } else if (zOn) {   // the z-plane alone (Replace, or the filter switched off)
                float zl = accL, zr = accR;
                zRun(zl, zr);
                outL = accL * zDry_ + zl * zNorm_ * zWet_;
                outR = accR * zDry_ + zr * zNorm_ * zWet_;
            } else {
                outL = accL; outR = accR;
            }
            if (foldAmt > 0.0f) {
                outL = wavefold(outL, foldAmt);
                outR = wavefold(outR, foldAmt);
            }
            if (air) {
                float lp, bp, hp;
                airL_.tick(rng_.bipolar(), lp, bp, hp); outL += airGain_ * bp;
                airR_.tick(rng_.bipolar(), lp, bp, hp); outR += airGain_ * bp;
            } else if (ghost) {
                const float nl2 = rng_.bipolar(), nr2 = rng_.bipolar();
                float gl = 0.0f, gr = 0.0f;
                for (int k = 0; k < 6; ++k) { gl += ghostL_[k].tick(nl2); gr += ghostR_[k].tick(nr2); }
                outL += ghostGain_ * gl; outR += ghostGain_ * gr;
            }
            const float g = e * velocity_ * gLevel_;
            outL *= g * panL_;   // a near event's place in the field; one and one for every other note
            outR *= g * panR_;
            if (doFm) {   // self-modulation of a partial by its own output carries a DC term (J1 of the index): block it
                const float yl = outL - fmHpXL_ + fmHpCoef_ * fmHpYL_; fmHpXL_ = outL; fmHpYL_ = yl; outL = yl;
                const float yr = outR - fmHpXR_ + fmHpCoef_ * fmHpYR_; fmHpXR_ = outR; fmHpYR_ = yr; outR = yr;
            }
            // Interaural time difference.
            itdL_ += (itdLTarget_ - itdL_) * 0.002f;
            itdR_ += (itdRTarget_ - itdR_) * 0.002f;
            itdBufL_[itdW_ & (kItdBuffer - 1)] = outL;
            itdBufR_[itdW_ & (kItdBuffer - 1)] = outR;
            {
                const int di = static_cast<int>(itdL_); const float f = itdL_ - static_cast<float>(di);
                const float a = itdBufL_[(itdW_ - di) & (kItdBuffer - 1)], b = itdBufL_[(itdW_ - di - 1) & (kItdBuffer - 1)];
                outL = a + f * (b - a);
            }
            {
                const int di = static_cast<int>(itdR_); const float f = itdR_ - static_cast<float>(di);
                const float a = itdBufR_[(itdW_ - di) & (kItdBuffer - 1)], b = itdBufR_[(itdW_ - di - 1) & (kItdBuffer - 1)];
                outR = a + f * (b - a);
            }
            ++itdW_;
            if (sphereShadow_) {
                const float yl = sphB0_[0] * outL + sphB1_[0] * sphX1_[0] - sphA1_[0] * sphY1_[0];
                sphX1_[0] = outL; sphY1_[0] = yl; outL = yl;
                const float yr = sphB0_[1] * outR + sphB1_[1] * sphX1_[1] - sphA1_[1] * sphY1_[1];
                sphX1_[1] = outR; sphY1_[1] = yr; outR = yr;
            } else {
                shadowL_ += shadowCoefL_ * (outL - shadowL_); outL = shadowL_;
                shadowR_ += shadowCoefR_ * (outR - shadowR_); outR = shadowR_;
            }
            if (ildAmt_ > 0.0f) {   // the near field's low shelf, per ear
                ildLpL_ += ildCoef_ * (outL - ildLpL_); outL -= ildL_ * ildLpL_;
                ildLpR_ += ildCoef_ * (outR - ildLpR_); outR -= ildR_ * ildLpR_;
            }
            if (proxAmt_ != 0.0f) {   // a near event's lift between 120 and 300 Hz, a function of its plane: gone on the far one
                proxLoL_ += proxLoCoef_ * (outL - proxLoL_); proxHiL_ += proxHiCoef_ * (outL - proxHiL_);
                proxLoR_ += proxLoCoef_ * (outR - proxLoR_); proxHiR_ += proxHiCoef_ * (outR - proxHiR_);
                const float near = proxAmt_ * (1.0f - distEff_);
                outL += near * (proxHiL_ - proxLoL_);
                outR += near * (proxHiR_ - proxLoR_);
            }
            if (pinnaAmt_ > 0.0f) {
                // The shoulder's copy comes out of the same ring the interaural delay reads, one
                // more tap further back; the pinna's notch is the state-variable filter's low plus
                // high output, mixed in by the amount. The shoulder belongs to externalisation
                // alone; the notch is shared with height.
                if (extAmt_ > 0.0f) {
                    const int sL = static_cast<int>(itdL_) + shoulder_, sR = static_cast<int>(itdR_) + shoulder_;
                    outL += 0.32f * extAmt_ * itdBufL_[(itdW_ - sL) & (kItdBuffer - 1)];
                    outR += 0.32f * extAmt_ * itdBufR_[(itdW_ - sR) & (kItdBuffer - 1)];
                }
                float lp, bp, hp;
                pinnaL_.tick(outL, lp, bp, hp); outL += pinnaAmt_ * ((lp + hp) - outL) * 0.8f;
                pinnaR_.tick(outR, lp, bp, hp); outR += pinnaAmt_ * ((lp + hp) - outR) * 0.8f;
                if (skyGain_ != 0.0f) {
                    skyL_.tick(outL, lp, bp, hp); outL += skyGain_ * bp;
                    skyR_.tick(outR, lp, bp, hp); outR += skyGain_ * bp;
                }
            }
            if (phaseOn_) {   // first-order all-passes: y = c x + x1 - c y1, two per ear
                float* c = apC_[0]; float* x1 = apX_[0]; float* y1 = apY_[0];
                for (int st = 0; st < 2; ++st) { const float y = c[st] * outL + x1[st] - c[st] * y1[st]; x1[st] = outL; y1[st] = y; outL = y; }
                c = apC_[1]; x1 = apX_[1]; y1 = apY_[1];
                for (int st = 0; st < 2; ++st) { const float y = c[st] * outR + x1[st] - c[st] * y1[st]; x1[st] = outR; y1[st] = y; outR = y; }
            }
            if (ksOn_) { const float sk = strikeTick(); nearL[pos + i] += sk * ksGainL_; nearR[pos + i] += sk * ksGainR_; }
            nearL[pos + i] += outL * gNear_;
            nearR[pos + i] += outR * gNear_;
            farL[pos + i]  += outL * gFar_;
            farR[pos + i]  += outR * gFar_;
        }
        pos += len;
    }
    // The Z-plane filter is the one thing here whose poles sit close enough to the circle that
    // sweeping it -- which is what an LFO on Z X or Z Y does, and what half the library does --
    // can put more energy in than comes out. See ZBiquad::guard.
    for (int k = 0; k < zUsed_; ++k) { zbL_[k].guard(); zbR_[k].guard(); }
    zModal_.guard();
    if (!env_.isActive()) note_ = -1;
}

} // namespace ambient
