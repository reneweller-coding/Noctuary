/**
 * @file EngineControl.cpp
 * @brief What the engine decides once per block: the modulation sources and the matrix,
 *        the clock, and readParams, which turns the parameter atomics into the structures the voices and
 *        the effects are given.
 *
 * Split out of Engine.cpp, which had grown past fourteen hundred lines.
 */
#include "ambient/Engine.h"
#include "ambient/PresetMap.h"
#include "ambient/PresetMeta.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
  /**
   * @brief 1 on x86: the SSE control register (MXCSR) exists, and Engine::process() in EngineRender.cpp
   *        sets its flush-to-zero and denormals-are-zero bits for the length of a block and restores
   *        it afterwards.
   */
  #define AMBIENT_HAS_MXCSR 1
#else
  /**
   * @brief 0 on every other architecture: there is no MXCSR to set, so the denormal control in
   *        Engine::process() is compiled out (on 64-bit ARM it is FPCR's, see AMBIENT_HAS_FPCR).
   */
  #define AMBIENT_HAS_MXCSR 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  /**
   * @brief 1 on 64-bit ARM: Engine::process() sets bit 24 of FPCR (flush-to-zero) for the length of a
   *        block, which is what keeps the Quest's decaying tails and envelopes out of denormals.
   */
  #define AMBIENT_HAS_FPCR 1
#else
  /** @brief 0 on every other architecture: there is no FPCR to set. */
  #define AMBIENT_HAS_FPCR 0
#endif

namespace ambient {



// ---------------------------------------------------------------- modulation

/**
 * @brief Takes the pending matrix and shapes for the message thread.
 *
 * Everything that writes into them
 * has to hold this, not merely announce afterwards that it has finished: the audio thread copies
 * the very bytes the parser writes, so an edit that begins while a copy is running is a race on
 * the matrix itself. That was the shape of it -- announcing was guarded, writing was not, and the
 * thread sanitizer found it in four seconds once there was a workload that did both at once.
 */
void Engine::lockModulation()
{
    // The message thread may wait; the copy it waits for is a few hundred bytes.
    while (modLock_.test_and_set(std::memory_order_acquire)) { }
}

void Engine::unlockModulation() { modLock_.clear(std::memory_order_release); }

/**
 * @brief Clears the pending matrix and shapes WITHOUT announcing them: the announcement is what makes
 *        the audio thread copy, and it must not do that while the rest is still being written.
 */
void Engine::clearPendingModulation()
{
    matrixPending_.clear();
    // A default shape every envelope starts from: up over a quarter of its length, down over the
    // rest. Time is scaled by the envelope's own Time parameter, so this is a shape, not a length.
    for (auto& e : envPending_) e.parse("0:0/1:1/4:0");
    // And every source's own: a rise to its level, which is what the plain Rise does as well.
    for (auto& e : srcEnvPending_) e.parse(kSrcEnvDefault);
}

void Engine::resetModulation()
{
    lockModulation();
    clearPendingModulation();
    publishModulation();
    unlockModulation();
}

/**
 * @brief Hand the finished matrix and shapes over: the audio thread copies them at the top of a block
 *        once it can take the lock.
 *
 * Called with the lock held.
 */
void Engine::publishModulation()
{
    modVersion_.fetch_add(1, std::memory_order_release);
}

bool Engine::applyPresetModulation(const Preset& p)
{
    // Cleared, then filled, then announced -- once, at the end. It used to announce the clear
    // and then parse into the same object, so the audio thread could be copying the matrix
    // while the message thread was still writing it.
    lockModulation();
    clearPendingModulation();
    bool ok = true;
    if (p.mod != nullptr && *p.mod) ok = matrixPending_.parse(p.mod) && ok;
    if (p.envs != nullptr && *p.envs) {
        // The six modulation envelopes, then the four sources' own: one list with '~' between
        // them, so a field written before the sources had shapes simply stops after the sixth.
        const char* s = p.envs;
        for (int i = 0; i < kNumModEnvs + kSlots && *s; ++i) {
            const char* end = s;
            while (*end && *end != '~') ++end;
            if (end > s) {
                // Sixteen points with a curve each run past 256 characters, and a buffer of that
                // size used to drop such a shape without a word. One too long now says so.
                char buf[1024];
                const size_t len = static_cast<size_t>(end - s);
                if (len < sizeof(buf)) {
                    std::memcpy(buf, s, len);
                    buf[len] = 0;
                    ModEnv& shape = i < kNumModEnvs ? envPending_[i] : srcEnvPending_[i - kNumModEnvs];
                    ok = shape.parse(buf) && ok;
                } else ok = false;
            }
            s = (*end == '~') ? end + 1 : end;
        }
    }
    publishModulation();
    unlockModulation();
    return ok;
}

bool Engine::setModMatrixText(const char* text)
{
    lockModulation();
    const bool ok = matrixPending_.parse(text);
    if (ok) publishModulation();
    unlockModulation();
    return ok;
}

bool Engine::setEnvShape(int index, const char* text)
{
    if (index < 0 || index >= kNumModEnvs) return false;
    lockModulation();
    const bool ok = envPending_[index].parse(text);
    if (ok) publishModulation();
    unlockModulation();
    return ok;
}

int Engine::writeEnvShape(int index, char* buf, size_t cap) const
{
    if (index < 0 || index >= kNumModEnvs) return 0;
    return envPending_[index].write(buf, cap);
}

bool Engine::setSrcEnvShape(int slot, const char* text)
{
    if (slot < 0 || slot >= kSlots) return false;
    lockModulation();
    const bool ok = srcEnvPending_[slot].parse(text);
    if (ok) publishModulation();
    unlockModulation();
    return ok;
}

int Engine::writeSrcEnvShape(int slot, char* buf, size_t cap) const
{
    if (slot < 0 || slot >= kSlots) return 0;
    return srcEnvPending_[slot].write(buf, cap);
}

// ---------------------------------------------------------------- the Beat source

/**
 * @brief The instrument listening to its own harmonic friction.
 *
 * The Foundation's ghost tone already takes the two lowest sounding voices and uses their
 * frequency difference as a bass note. That difference is only half the story: what the ear
 * actually reacts to in a sustained chord is not the combination tone itself but whether the
 * interval is IN TUNE -- two voices a fifth apart beat at |2*f2 - 3*f1|, which is silent when
 * the fifth is just and gets quicker the further it has drifted. That is the rate this source
 * runs at.
 *
 * So a chord sitting exactly on its just ratios makes this oscillator stand still, and as Purity
 * Drift loosens the tuning it starts to turn, in time with the roughness you can already hear.
 * Route it at a filter, at the Nebula's smear, at anything: the sound then breathes at the rate
 * of its own mistuning rather than at a rate somebody typed into an LFO.
 */
float Engine::updateBeat(float dt)
{
    // The two lowest distinct pitches, exactly as the ghost tone finds them.
    double f1 = 0.0, f2 = 0.0;
    for (const auto& v : voices_)
        if (v.isActive() && (f1 <= 0.0 || v.frequency() < f1)) f1 = v.frequency();
    if (f1 > 0.0)
        for (const auto& v : voices_)
            if (v.isActive() && v.frequency() / f1 > 1.003 && (f2 <= 0.0 || v.frequency() < f2)) f2 = v.frequency();

    float want = 0.0f;
    if (f1 > 0.0 && f2 > 0.0) {
        // The simplest ratio near the interval they make. Small numbers only: those are the ones
        // whose harmonics are close enough together to beat audibly, which is the whole point.
        static const int kRatios[][2] = { {1,1}, {6,5}, {5,4}, {4,3}, {3,2}, {8,5}, {5,3}, {7,4}, {2,1}, {5,2}, {3,1}, {4,1} };
        const double r = f2 / f1;
        double bestErr = 1e30;
        int bp = 1, bq = 1;
        for (const auto& pq : kRatios) {
            const double err = std::fabs(std::log(r / (static_cast<double>(pq[0]) / pq[1])));
            if (err < bestErr) { bestErr = err; bp = pq[0]; bq = pq[1]; }
        }
        // The harmonics that would coincide if the interval were just: q*f2 against p*f1. Their
        // difference IS the beat, and it is zero for a perfectly tuned interval. For a mistuned
        // unison (p = q = 1) this is |f2 - f1|, the difference tone itself.
        want = static_cast<float>(std::fabs(static_cast<double>(bq) * f2 - static_cast<double>(bp) * f1));
        if (!(want == want) || want > 30.0f) want = 30.0f;      // NaN guard and a ceiling
    }
    // Followed slowly. A voice arriving or leaving changes which pair is lowest, and the rate
    // would otherwise jump; over about two seconds it slides instead, which is also how long the
    // ear takes to decide that a chord has stopped beating.
    const float follow = 1.0f - std::exp(-dt / 2.0f);
    beatHz_ += follow * (want - beatHz_);
    // Turned into a modulation value. Below a fiftieth of a hertz the phase simply holds: a
    // chord that is in tune should leave whatever it is driving exactly where it is, not creep.
    if (beatHz_ > 0.02f) {
        beatPhase_ += kTwoPi * beatHz_ * dt;
        while (beatPhase_ > kTwoPi) beatPhase_ -= kTwoPi;
    }
    return std::sin(beatPhase_);
}

/**
 * @brief One step of every modulator, then the matrix summed into modOut_.
 *
 * Called once per block, before
 * readParams, so the values the parameters are read with already carry the modulation.
 */
void Engine::stepModulation(float dt)
{
    // Pick up matrix or shape edits made on the message thread (fixed-size objects, no allocation).
    const int mv = modVersion_.load(std::memory_order_acquire);
    // Tried, never waited for: the audio thread does not block on the message thread. A matrix
    // that is being edited right now simply arrives at the next block, a few milliseconds later,
    // which is sooner than a hand can move a mouse.
    if (mv != modSeen_ && !modLock_.test_and_set(std::memory_order_acquire)) {
        matrix_ = matrixPending_;
        for (int i = 0; i < kNumModEnvs; ++i) envShape_[i] = envPending_[i];
        for (int k = 0; k < kSlots; ++k) srcEnvShape_[k] = srcEnvPending_[k];
        modLock_.clear(std::memory_order_release);
        modSeen_ = mv;
        // Whether anything reads the Lenia field, so it is only computed when it is heard.
        leniaUsed_ = false;
        chaosUsed_ = false;
        for (int i = 0; i < matrix_.count(); ++i) {
            const ModRoute& r = matrix_.route(i);
            auto inRange = [](ModSource s, ModSource lo, ModSource hi) { return static_cast<int>(s) >= static_cast<int>(lo) && static_cast<int>(s) <= static_cast<int>(hi); };
            if (inRange(r.source, ModSource::Lenia1, ModSource::Lenia4) || inRange(r.via, ModSource::Lenia1, ModSource::Lenia4)) leniaUsed_ = true;
            if (inRange(r.source, ModSource::LorenzX, ModSource::RosslerZ) || inRange(r.via, ModSource::LorenzX, ModSource::RosslerZ)) chaosUsed_ = true;
        }
    }

    // A modulator's own settings, with the matrix's last word on them.
    //
    // These specs are read at the top of this function, before the matrix is summed at the bottom
    // of it, so a route pointed at an LFO's rate, an LFO's depth or an envelope's time used to
    // parse, sit in the matrix and do nothing whatsoever -- measured, the render hash did not
    // move by a bit. Eighty parameters behaved that way, and among them is the one figure the
    // ambient literature keeps coming back to: a filter sweep on a 13-second LFO whose rate is
    // itself moved by a 19-second one, so the pair only repeats after 13 x 19 = 247 seconds.
    //
    // It reads the PREVIOUS block's modOut_ (it is cleared further down, after these lines), and
    // that is not a compromise, it is what a feedback path in a modulation matrix is: one control
    // block of delay -- under a millisecond and a half -- which is the only way lfo2 can move
    // lfo1's rate without the two needing each other's answer before either has one.
    //
    // Only the continuous settings are taken from it. A choice -- a shape, a mode, a table, a
    // sync division -- would have to jump from one value to the next, and nothing in this
    // instrument is allowed to jump.
    auto modulated = [this](ParamId id) {
        const float m = modOut_[static_cast<int>(id)];
        const float v = effectiveParam(id);
        if (m == 0.0f) return v;
        const ParamDesc& d = paramDesc(id);
        return clampv(v + m, d.min, d.max);
    };

    for (int i = 0; i < kNumLfos; ++i) {
        const int base = static_cast<int>(ParamId::Lfo1Shape) + i * 7;
        LfoSpec& sp = lfoSpec_[i];
        sp.shape  = static_cast<LfoShape>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 0)))), 0, kNumLfoShapes - 1));
        sp.rateHz = modulated(static_cast<ParamId>(base + 1));
        sp.phase  = modulated(static_cast<ParamId>(base + 2));
        sp.depth  = modulated(static_cast<ParamId>(base + 3));
        sp.mode   = static_cast<LfoMode>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 4)))), 0, kNumLfoModes - 1));
        sp.table  = static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 5))));
        const int sync = clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 6)))), 0, kNumSyncDivs - 1);
        if (syncOn(sync)) {
            // One cycle per division, and the phase follows the clock: the step below lands
            // exactly on the beat's position, so a synced LFO stays on the grid however long
            // it runs and wherever the transport jumps.
            sp.rateHz = static_cast<float>(syncHz(sync, bpm_));
            if (running_) lfo_[i].setPhase(static_cast<float>(syncPhase(sync, beat_)) - sp.rateHz * dt);
        }
        // Retrigger: the phase goes back to where the knob says at the start of a phrase. Per Voice
        // cannot be honoured here and the help says so: this modulation is computed once a block
        // for the whole instrument, not once per voice, so there is no per-voice copy to give it.
        if (phraseStart_ && sp.mode == LfoMode::Retrigger) lfo_[i].setPhase(sp.phase);
        const Wavetable* table = userTable_.frames > 0 ? &userTable_ : nullptr;
        modSrc_[static_cast<int>(ModSource::Lfo1) + i] = lfo_[i].step(dt, sp, table);
    }
    phraseStart_ = false;

    const bool wasHeld = envHeld_;
    envHeld_ = false;
    for (const auto& v : voices_) if (v.isActive() && !v.isReleasing()) { envHeld_ = true; break; }
    for (int i = 0; i < kNumModEnvs; ++i) {
        envTime_[i] += dt;
        const int base = static_cast<int>(ParamId::Env1Mode) + i * 4;
        ModEnvSpec& sp = envSpec_[i];
        sp.mode      = static_cast<EnvMode>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 0)))), 0, kNumEnvModes - 1));
        sp.timeScale = std::max(0.01f, modulated(static_cast<ParamId>(base + 1)));
        const int sync = clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 3)))), 0, kNumSyncDivs - 1);
        if (syncOn(sync))   // synced: the whole shape spans one division
            sp.timeScale = static_cast<float>(std::max(0.01, syncSeconds(sync, bpm_) / std::max(static_cast<double>(envShape_[i].length()), 1e-3)));
        sp.depth     = modulated(static_cast<ParamId>(base + 2));
        // Sustain Loop, at the moment the last voice lets go: put the clock where the shape was
        // being read -- the sustain point, or wherever in its loop it had got to -- so the tail
        // plays from where the held part ended instead of from wherever the clock had got to.
        // Without this the value jumps on release: the shape holds while the note is down, and
        // the release finds the clock long past it. It used to be put on the sustain point
        // always, which still jumped for a note let go before reaching it and for a shape held
        // inside its loop. Only this mode is touched.
        if (wasHeld && !envHeld_ && sp.mode == EnvMode::SustainLoop)
            envTime_[i] = static_cast<double>(envShape_[i].readTime(static_cast<float>(envTime_[i]) / sp.timeScale, sp.mode, true)) * sp.timeScale;
        const float t = static_cast<float>(envTime_[i]) / sp.timeScale;
        modSrc_[static_cast<int>(ModSource::Env1) + i] = envShape_[i].at(t, sp.mode, envHeld_) * sp.depth;
    }
    // Each source's own envelope keeps only its settings here: its clock is every voice's own,
    // from its note (Voice.cpp), so there is no single value of it to hand to the matrix.
    for (int k = 0; k < kSlots; ++k) {
        const int base = static_cast<int>(ParamId::Src1EnvMode) + k * 4;
        ModEnvSpec& sp = srcEnvSpec_[k];
        sp.mode      = static_cast<EnvMode>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 0)))), 0, kNumEnvModes - 1));
        sp.timeScale = std::max(0.01f, modulated(static_cast<ParamId>(base + 1)));
        const int sync = clampv(static_cast<int>(std::lround(getParam(static_cast<ParamId>(base + 3)))), 0, kNumSyncDivs - 1);
        if (syncOn(sync))   // synced: the whole shape spans one division
            sp.timeScale = static_cast<float>(std::max(0.01, syncSeconds(sync, bpm_) / std::max(static_cast<double>(srcEnvShape_[k].length()), 1e-3)));
        sp.depth     = modulated(static_cast<ParamId>(base + 2));
    }

    // Everything else that can drive something. Sources are bipolar; the ones that are naturally
    // 0..1 are mapped to -1..1 here, and a route's "unipolar" flag maps them back.
    const Voice* loud = loudestVoice();
    auto uni = [](float v) { return 2.0f * clampv(v, 0.0f, 1.0f) - 1.0f; };
    modSrc_[static_cast<int>(ModSource::Amp)] = uni(loud ? loud->level() : 0.0f);
    for (int m = 0; m < 8; ++m)
        modSrc_[static_cast<int>(ModSource::MacroA) + m] = uni(getParam(static_cast<ParamId>(static_cast<int>(ParamId::MacroA) + m)));
    for (int k = 0; k < 4; ++k)
        modSrc_[static_cast<int>(ModSource::Kura1) + k] = std::sin(kuraPhase_[k]);
    if (leniaUsed_) stepLenia(dt);
    for (int k = 0; k < 4; ++k)
        modSrc_[static_cast<int>(ModSource::Lenia1) + k] = uni(leniaOut_[k]);
    if (chaosUsed_) stepChaos(dt);
    for (int k = 0; k < 6; ++k)
        modSrc_[static_cast<int>(ModSource::LorenzX) + k] = chaosOut_[k];
    modSrc_[static_cast<int>(ModSource::Note)] = uni(loud ? (loud->note() - 24) / 84.0f : 0.5f);
    modSrc_[static_cast<int>(ModSource::Velocity)] = uni(loud ? loud->level() : 0.0f);
    modSrc_[static_cast<int>(ModSource::Distance)] = uni(loud ? loud->distance() : 0.5f);
    modSrc_[static_cast<int>(ModSource::RandomPerNote)] = randomPerNote_;
    modSrc_[static_cast<int>(ModSource::Beat)] = updateBeat(dt);
    // Where the piece stands in itself. Root Age is measured against Home Time, so it reads the
    // same whether the night is an hour or three; Layer is the role of the voice being read, which
    // for a per-voice source is the loudest one; Section is the arc as a staircase of five, which
    // is what a supply or a register needs when it should change rather than glide.
    modSrc_[static_cast<int>(ModSource::RootAge)] =
        uni(static_cast<float>(clampv(brain_.rootAgeSeconds() / std::max(60.0, static_cast<double>(bp_.homeTime) * 60.0), 0.0, 1.0)));
    modSrc_[static_cast<int>(ModSource::Layer)] = uni(loud ? loud->layer() : 0.5f);
    modSrc_[static_cast<int>(ModSource::Section)] =
        uni(std::floor(clampv(0.5f + 0.5f * arcOut_, 0.0f, 0.9999f) * 5.0f) * 0.25f);
    // The cascade's excitation, smoothed: the Hawkes kick is a step, and a step on a send is a
    // click. One pole at two seconds turns it into a swell that still rises with the cluster.
    {
        const float target = static_cast<float>(brain_.excitation() / (1.0 + brain_.excitation()));
        const float a = 1.0f - std::exp(-dt / 2.0f);
        cascadeNow_ += a * (target - cascadeNow_);
        // Its own slow average, so anything reading it can ask "busier than usual?" rather than
        // "busy?" -- what keeps a piece without a cascade exactly where it was.
        cascadeAvg_ += (1.0f - std::exp(-dt / 90.0f)) * (cascadeNow_ - cascadeAvg_);
        modSrc_[static_cast<int>(ModSource::Cascade)] = uni(cascadeNow_);
    }
    // The hands. Pressure and slide are per note and read from the loudest voice -- it is the one
    // being leaned on; the wheel belongs to the instrument. All three rest at zero, which through
    // uni() is -1, so a route wanting "nothing until I move it" carries the 0..1 flag.
    modSrc_[static_cast<int>(ModSource::Pressure)] = uni(loud ? loud->pressure() : 0.0f);
    modSrc_[static_cast<int>(ModSource::Slide)]    = uni(loud ? loud->slide() : 0.0f);
    wheel_ += (1.0f - std::exp(-dt / 0.03f)) * (wheelTarget_ - wheel_);   // 30 ms: no step from a 7-bit controller
    modSrc_[static_cast<int>(ModSource::Wheel)]    = uni(wheel_);
    modSrc_[static_cast<int>(ModSource::None)] = 0.0f;

    std::memset(modOut_, 0, sizeof(modOut_));
    matrix_.apply(modSrc_, modOut_);
    // Performance state is never a target: modulating the morph position or the map cursor from
    // inside would fight the hand that is holding them.
    for (const ParamDesc& d : paramTable())
        if (isPerformanceParam(d.id)) modOut_[static_cast<int>(d.id)] = 0.0f;
}

// ---------------------------------------------------------------- Lenia

/**
 * @brief One block of the Lenia field: the rows due at Lenia Rate are stepped, a field that has died
 *        or filled up is seeded again, and the four readings glide to their new means.
 *
 * Lenia (Chan 2019) is Conway's Life taken to the continuum: cells hold a value in 0..1, each
 * looks at a ring-shaped neighbourhood -- a bell around half the radius, normalised -- and grows
 * or shrinks by a smooth growth function of what it sees, 2 exp(-(u - mu)^2 / 2 sigma^2) - 1, so
 * a cell in the right company grows and one in too little or too much decays. The field is
 * updated a tenth of the way per step. On a large grid this breeds the gliders and rotors the
 * literature shows; on thirty-two cells it breeds blobs that drift, pulse, split and sometimes
 * die, which is what is wanted from a modulator. Four readings, each the mean of a three-by-
 * three patch near a corner, glide to their new values with the step's own time constant.
 */
void Engine::stepLenia(float dt)
{
    constexpr int S = kLeniaSize, R = kLeniaRadius, K = 2 * R + 1;
    auto bell = [](float x, float m, float s) { const float d = (x - m) / s; return std::exp(-0.5f * d * d); };
    if (!leniaInit_) {
        leniaInit_ = true;
        leniaRng_.seed(0x1E51Aull);
        float sum = 0.0f;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                const float r = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / static_cast<float>(R);
                const float k = r <= 1.0f ? bell(r, 0.5f, 0.15f) : 0.0f;
                leniaKernel_[(dy + R) * K + (dx + R)] = k;
                sum += k;
            }
        for (float& k : leniaKernel_) k /= sum;
        seedLenia();
    }
    // How many rows this block: one full field per 1/rate seconds, the remainder carried over.
    leniaRowAcc_ += static_cast<float>(S) * dt * std::max(leniaRate_, 0.01f);
    int rows = static_cast<int>(leniaRowAcc_);
    leniaRowAcc_ -= static_cast<float>(rows);
    rows = clampv(rows, 0, S);
    const float sigma = 0.02f;
    for (int pass = 0; pass < rows; ++pass) {
        const int y = leniaRow_;
        for (int x = 0; x < S; ++x) {
            float u = 0.0f;
            for (int dy = -R; dy <= R; ++dy) {
                const float* row = &lenia_[((y + dy + S) % S) * S];
                const float* krow = &leniaKernel_[(dy + R) * K];
                for (int dx = -R; dx <= R; ++dx) u += krow[dx + R] * row[(x + dx + S) % S];
            }
            const float g = 2.0f * bell(u, leniaMu_, sigma) - 1.0f;
            leniaNext_[y * S + x] = clampv(lenia_[y * S + x] + 0.1f * g, 0.0f, 1.0f);
        }
        if (++leniaRow_ >= S) {
            leniaRow_ = 0;
            std::memcpy(lenia_, leniaNext_, sizeof(lenia_));
            ++leniaSteps_;
            float mass = 0.0f;
            for (float v : lenia_) mass += v;
            if (mass < 3.0f || mass > 0.85f * static_cast<float>(S * S)) { seedLenia(); ++leniaReseeds_; }
            static const int px[4] = { 8, 24, 8, 24 }, py[4] = { 8, 8, 24, 24 };
            for (int k = 0; k < 4; ++k) {
                float s = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) s += lenia_[((py[k] + dy) % S) * S + (px[k] + dx) % S];
                leniaTarget_[k] = s / 9.0f;
            }
        }
    }
    const float c = 1.0f - std::exp(-dt * std::max(leniaRate_, 0.01f));
    for (int k = 0; k < 4; ++k) leniaOut_[k] += c * (leniaTarget_[k] - leniaOut_[k]);
}

// ---------------------------------------------------------------- the attractors

/**
 * @brief One block of the two attractors, integrated in their own time and read into chaosOut_ as
 *        six bipolar values; a state that has left the finite world is seeded again.
 *
 * Lorenz (sigma 10, rho 28, beta 8/3) and Roessler (a = b = 0.2, c = 5.7), stepped by fourth-order
 * Runge-Kutta in their own time, which is scaled so that Chaos Period is about the time between
 * the Lorenz system's lobe changes and about one turn of the Roessler spiral. The readings are
 * the coordinates scaled by the attractors' known extents and clamped -- the Roessler z climbs
 * higher now and then than the scale allows, and that is what its z is for. The time step in
 * natural units is kept below a hundredth by substepping, so the integration is the same
 * whatever the block size.
 */
void Engine::stepChaos(float dt)
{
    auto rk4 = [](double* s, double h, auto&& f) {
        double k1[3], k2[3], k3[3], k4[3], t[3];
        f(s, k1);
        for (int i = 0; i < 3; ++i) t[i] = s[i] + 0.5 * h * k1[i];
        f(t, k2);
        for (int i = 0; i < 3; ++i) t[i] = s[i] + 0.5 * h * k2[i];
        f(t, k3);
        for (int i = 0; i < 3; ++i) t[i] = s[i] + h * k3[i];
        f(t, k4);
        for (int i = 0; i < 3; ++i) s[i] += h / 6.0 * (k1[i] + 2.0 * k2[i] + 2.0 * k3[i] + k4[i]);
    };
    auto lorenz = [](const double* s, double* d) { d[0] = 10.0 * (s[1] - s[0]); d[1] = s[0] * (28.0 - s[2]) - s[1]; d[2] = s[0] * s[1] - (8.0 / 3.0) * s[2]; };
    auto rossler = [](const double* s, double* d) { d[0] = -s[1] - s[2]; d[1] = s[0] + 0.2 * s[1]; d[2] = 0.2 + s[2] * (s[0] - 5.7); };
    const double period = std::max(static_cast<double>(chaosPeriod_), 1.0);
    const double hL = static_cast<double>(dt) * 1.5 / period;    // lobe changes a few natural units apart
    const double hR = static_cast<double>(dt) * 6.0 / period;    // one turn of the spiral is about six
    for (double left = hL; left > 0.0; left -= 0.01) rk4(lorenz_, std::min(left, 0.01), lorenz);
    for (double left = hR; left > 0.0; left -= 0.01) rk4(rossler_, std::min(left, 0.01), rossler);
    // An attractor that has left the finite world never comes back on its own, and clampv
    // does not stop it: every comparison against a NaN is false, so clampv hands the NaN
    // straight on -- into the matrix, into filters and gains, and the instrument is silent
    // until the plugin is reloaded. If either state is no longer finite it is seeded again.
    auto finite3 = [](const double* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); };
    if (!finite3(lorenz_))  { lorenz_[0] = 0.1; lorenz_[1] = 0.0; lorenz_[2] = 20.0; }
    if (!finite3(rossler_)) { rossler_[0] = 1.0; rossler_[1] = 1.0; rossler_[2] = 1.0; }
    chaosOut_[0] = clampv(static_cast<float>(lorenz_[0] / 20.0), -1.0f, 1.0f);
    chaosOut_[1] = clampv(static_cast<float>(lorenz_[1] / 27.0), -1.0f, 1.0f);
    chaosOut_[2] = clampv(static_cast<float>((lorenz_[2] - 25.0) / 22.0), -1.0f, 1.0f);
    chaosOut_[3] = clampv(static_cast<float>(rossler_[0] / 12.0), -1.0f, 1.0f);
    chaosOut_[4] = clampv(static_cast<float>(rossler_[1] / 12.0), -1.0f, 1.0f);
    chaosOut_[5] = clampv(static_cast<float>((rossler_[2] - 8.0) / 12.0), -1.0f, 1.0f);
    ++chaosSteps_;
}

/** @brief A few soft blobs on an empty torus. From the field's own random stream, so the sound's is not touched. */
void Engine::seedLenia()
{
    constexpr int S = kLeniaSize;
    for (float& v : lenia_) v = 0.0f;
    for (int b = 0; b < 3; ++b) {
        const float cx = leniaRng_.uniform() * S, cy = leniaRng_.uniform() * S;
        const float rad = 2.5f + 2.5f * leniaRng_.uniform();
        const float peak = 0.6f + 0.4f * leniaRng_.uniform();
        for (int y = 0; y < S; ++y)
            for (int x = 0; x < S; ++x) {
                float dx = std::fabs(static_cast<float>(x) - cx), dy = std::fabs(static_cast<float>(y) - cy);
                dx = std::min(dx, static_cast<float>(S) - dx); dy = std::min(dy, static_cast<float>(S) - dy);
                lenia_[y * S + x] = std::min(1.0f, lenia_[y * S + x] + peak * std::exp(-(dx * dx + dy * dy) / (2.0f * rad * rad)));
            }
    }
    std::memcpy(leniaNext_, lenia_, sizeof(lenia_));
    leniaRow_ = 0;
}

// ---------------------------------------------------------------- clock

void Engine::stepClock(double dt)
{
    const int src = clampv(static_cast<int>(std::lround(getParam(ParamId::ClockSource))), 0, kNumClockSources - 1);
    const double internal = clampv(getParam(ParamId::Tempo), 20.0f, 300.0f);
    const bool run = getParam(ParamId::ClockRun) >= 0.5f;
    midiSilence_ += dt;
    const bool useHost = src == static_cast<int>(ClockSource::Host) && hostSeen_ && hostBpm_ > 1.0;
    const bool useMidi = src == static_cast<int>(ClockSource::Midi) && midiTicks_ >= 24 && midiBpm_ > 1.0 && midiSilence_ < 2.0;
    if (useHost)      { bpm_ = hostBpm_; beat_ = hostBeat_; running_ = hostPlaying_; }
    else if (useMidi) { bpm_ = midiBpm_; beat_ = midiBeat_; running_ = midiRunning_; }
    else {
        // The internal clock: the Tempo knob, counting beats while Run is on. Also what Host and
        // MIDI fall back to when nothing arrives -- the standalone has no play head.
        bpm_ = internal;
        running_ = run;
        if (run) intBeat_ += bpm_ / 60.0 * dt;
        beat_ = intBeat_;
    }
    tempoOut_.store(bpm_, std::memory_order_relaxed);
    beatOut_.store(beat_, std::memory_order_relaxed);
    runningOut_.store(running_, std::memory_order_relaxed);
}

void Engine::midiClockTick(double interval)
{
    // 24 ticks a quarter. The tempo settles over a beat's worth of ticks, so jitter in the
    // interface does not wobble every synced LFO.
    if (interval > 0.002 && interval < 2.0) {
        const double bpm = 60.0 / (24.0 * interval);
        midiBpm_ = midiTicks_ < 24 || midiBpm_ <= 1.0 ? bpm : midiBpm_ + (bpm - midiBpm_) * 0.08;
    }
    if (midiRunning_) midiBeat_ += 1.0 / 24.0;
    ++midiTicks_;
    midiSilence_ = 0.0;
}

void Engine::midiClockStart()    { midiBeat_ = 0.0; midiRunning_ = true; }
void Engine::midiClockContinue() { midiRunning_ = true; }
void Engine::midiClockStop()     { midiRunning_ = false; }

void Engine::readParams()
{
    // Inertia: every float parameter glides to its effective value with the Inertia time
    // constant (skew domain), the analogue slew that keeps even a torn-open knob slow.
    const float inertia = getParam(ParamId::Inertia);
    const float inertiaCoef = inertia > 0.005f ? 1.0f - std::exp(-lastBlockSeconds_ / inertia) : 1.0f;
    auto g = [this, inertiaCoef](ParamId id) {
        const float target = effectiveParam(id);
        const int i = static_cast<int>(id);
        const ParamDesc& d = paramDesc(id);
        // Modulation is added after the inertia glide: a modulator moves at its own rate, it is
        // not slewed by the setting that exists to slow down the performer's hand.
        const float mod = modOut_[i];
        if (inertiaCoef >= 1.0f || d.kind != ParamKind::Float || isPerformanceParam(id)) {
            inertiaCur_[i] = target;
            return mod != 0.0f ? clampv(target + mod, d.min, d.max) : target;
        }
        const float span = std::max(d.max - d.min, 1e-9f);
        // A knob that is not moving is not glided. The three powers below were computed for every
        // float parameter of every block whether or not anything had changed -- and a panel that
        // is being listened to rather than turned is the normal case. The threshold also lets the
        // glide arrive: without it the value approaches its target for ever and never reaches it.
        if (std::fabs(target - inertiaCur_[i]) <= 1.0e-7f * span) {
            inertiaCur_[i] = target;
            return mod != 0.0f ? clampv(target + mod, d.min, d.max) : target;
        }
        const float pc = std::pow(clampv((inertiaCur_[i] - d.min) / span, 0.0f, 1.0f), d.skew);
        const float pt = std::pow(clampv((target - d.min) / span, 0.0f, 1.0f), d.skew);
        inertiaCur_[i] = d.min + span * std::pow(pc + (pt - pc) * inertiaCoef, 1.0f / d.skew);
        return mod != 0.0f ? clampv(inertiaCur_[i] + mod, d.min, d.max) : inertiaCur_[i];
    };
    vp_.level       = g(ParamId::OscLevel);
    vp_.partials    = static_cast<int>(std::lround(g(ParamId::Partials)));
    vp_.tilt        = g(ParamId::Tilt);
    vp_.brightness  = g(ParamId::Brightness);
    vp_.oddEven     = g(ParamId::OddEven);
    vp_.inharmonic  = g(ParamId::Inharmonic);
    // Match: the partials placed on the current scale instead of on the harmonic series. Sethares'
    // other direction -- the timbre scale takes a spectrum and finds its scale; this takes a scale
    // and bends the spectrum until that scale is the smooth one. Milne, Sethares and Plamondon
    // (2009) call the pair dynamic tonality. Computed here once per block for the voice and for
    // the conductor's ear alike, so both hear the same partials.
    vp_.match = g(ParamId::TuneMatch);
    // Thirty-two partials, each two logarithms and an exponential, plus a walk over the
    // scale's degrees inside matchedPartialRatio -- a hundred transcendentals, recomputed on
    // every block for an answer that changes only when the knob, the spectrum's stiffness or
    // the scale changes. None of those move at audio rate.
    if (vp_.match > 0.0f) {
        const float B = vp_.inharmonic * vp_.inharmonic * 0.02f;
        const double m = static_cast<double>(vp_.match);
        const int scaleId = static_cast<int>(std::lround(getParam(ParamId::Scale)));
        const bool same = matchHave_ && vp_.match == matchLast_ && B == matchB_
                       && scaleId == matchScale_ && rootNote_ == matchRoot_;
        if (same) {
            for (int h = 0; h < kMaxPartials; ++h) vp_.partialRatio[h] = matchRatio_[h];
        } else {
        matchHave_ = true; matchLast_ = vp_.match; matchB_ = B; matchScale_ = scaleId; matchRoot_ = rootNote_;
        for (int h = 1; h <= kMaxPartials; ++h) {
            const double natural = h * (B > 0.0f ? std::sqrt(1.0 + B * static_cast<double>(h * h)) : 1.0);
            const double matched = matchedPartialRatio(h);
            vp_.partialRatio[h - 1] = static_cast<float>(std::exp(std::log(natural) + m * (std::log(matched) - std::log(natural))));
            matchRatio_[h - 1] = vp_.partialRatio[h - 1];
        }
        }
    }
    vp_.shimmer     = g(ParamId::Shimmer);
    vp_.shimmerRate = g(ParamId::ShimmerRate);
    vp_.unison      = static_cast<int>(std::lround(g(ParamId::Unison)));
    vp_.detune      = g(ParamId::Detune);
    vp_.drift       = g(ParamId::Drift);
    vp_.lowDetune   = g(ParamId::StrandLowDetune);
    vp_.beatCeiling = g(ParamId::BeatCeiling);
    vp_.velAttack   = g(ParamId::EnvVelAttack);
    layerDepth_     = g(ParamId::LayerDepth);
    vp_.driftRate   = g(ParamId::DriftRate);
    vp_.spread      = g(ParamId::Spread);
    vp_.bloom       = g(ParamId::Bloom);
    vp_.bloomTime   = g(ParamId::BloomTime);
    vp_.stack       = static_cast<int>(std::lround(g(ParamId::Stack)));
    vp_.rateWander  = g(ParamId::RateWander);
    {   // Source slots: 24 fields each. Source 2 and 3 are laid out consecutively from their Type;
        // Source 1's fields are scattered (its level and spectrum are the classic Oscillator
        // parameters, read above), so every slot goes through one table of ids.
        // The ids come from Params.h; they used to be written out here and again in the editor.
        for (int k = 0; k < kSlots; ++k) {
            // Source 1's level and spectrum were read into vp_ above; reading them again would step
            // their inertia twice per block, so they are copied instead.
            const ParamId* ids = slotParamIds(k);
            auto at = [&](int off) { return g(ids[off]); };
            SlotParams& s = vp_.slot[k];
            if (k == 0) {
                s.level = vp_.level; s.partials = vp_.partials; s.tilt = vp_.tilt; s.bright = vp_.brightness;
                s.oddEven = vp_.oddEven; s.inharm = vp_.inharmonic; s.shimmer = vp_.shimmer; s.shimmerRate = vp_.shimmerRate;
            } else {
                s.level       = at(1);
                s.partials    = static_cast<int>(std::lround(at(17)));
                s.tilt        = at(18);
                s.bright      = at(19);
                s.oddEven     = at(20);
                s.inharm      = at(21);
                s.shimmer     = at(22);
                s.shimmerRate = at(23);
            }
            s.type          = static_cast<SourceType>(clampv(static_cast<int>(std::lround(at(0))), 0, kNumSourceTypes - 1));
            s.drift         = at(25);
            s.octave        = static_cast<int>(std::lround(at(2)));
            s.ratio         = static_cast<int>(std::lround(at(3)));
            s.pan           = at(4);
            s.table         = static_cast<int>(std::lround(at(5)));
            s.position      = at(6);
            s.positionDrift = at(7);
            s.fmRatio       = at(8);
            s.fmIndex       = at(9);
            s.grainMs       = at(10);
            s.density       = at(11);
            {   // Sync: a grain (or a crackle) per division instead of per second
                const int dsync = clampv(static_cast<int>(std::lround(getParam(ids[24]))), 0, kNumSyncDivs - 1);
                if (syncOn(dsync)) s.density = static_cast<float>(syncHz(dsync, bpm_));
            }
            s.follow        = at(12) >= 0.5f;
            s.grains        = static_cast<int>(std::lround(at(13)));
            s.spread        = at(14);
            s.noise         = static_cast<NoiseKind>(clampv(static_cast<int>(std::lround(at(15))), 0, kNumNoiseKinds - 1));
            s.noiseQ        = at(16);
            s.stretch       = at(26);
            s.xfade         = at(27);
            s.bowForce      = at(28);
            s.bowSpeed      = at(29);
            s.specRate      = at(30);
            s.specBreath    = at(31);
            s.transport     = at(32);
            s.interp        = static_cast<int>(std::lround(at(36)));
            s.unison        = static_cast<int>(std::lround(at(37)));
            s.uniDetune     = at(38);
            s.uniWidth      = at(39);
            s.root          = at(40);
            s.delaySec      = at(33);
            s.riseSec       = at(34);
            s.role          = static_cast<SlotRole>(clampv(static_cast<int>(std::lround(at(41))), 0, kNumSlotRoles - 1));
            {   // The choice is "Off" first, then the six shapes -- one off the shape's index --
                // and "Own" last, the slot's own shape.
                const int env = static_cast<int>(std::lround(at(35)));
                s.envIndex = (env >= 1 && env <= kNumModEnvs) ? env - 1 : -1;
                s.ownEnv   = env == kNumModEnvs + 1;
            }
        }
        // The shapes a slot may borrow for its entrance. Pointers into the engine, valid for the
        // block like every other pointer in VoiceParams.
        for (int i = 0; i < kNumModEnvs; ++i) { vp_.envShape[i] = &envShape_[i]; vp_.envSpec[i] = &envSpec_[i]; }
        for (int k = 0; k < kSlots; ++k) { vp_.srcEnvShape[k] = &srcEnvShape_[k]; vp_.srcEnvSpec[k] = &srcEnvSpec_[k]; }
        // ---- the Vector
        //
        // A point in a square, after the Prophet VS and the Wavestation: the four corners are the
        // four source slots, and the point's bilinear weights become their levels. Written as a
        // factor on the levels each slot already has, so the slot knobs stay what they were -- a
        // trim under the vector rather than a thing the vector overwrites.
        //
        // The weights are scaled by four, which makes the CENTRE of the square neutral: at
        // (0.5, 0.5) each factor is exactly 1 and turning Amount up changes nothing at all. It
        // also keeps the four factors summing to four wherever the point is, so travelling to a
        // corner moves the timbre without moving the level.
        {
            const float amount = g(ParamId::VecAmount);
            if (amount > 0.0005f) {
                const float wander = g(ParamId::VecWander);
                float vx = g(ParamId::VecX), vy = g(ParamId::VecY);
                if (wander > 0.0005f) {
                    // Two drifts whose rates share no simple ratio, so the point never traces the
                    // same path twice -- the same reason the LFOs sit on a golden ladder.
                    const float rate = g(ParamId::VecRate);
                    vx += 0.5f * wander * vecDriftX_.update(lastBlockSeconds_, rate, auxRng_);
                    vy += 0.5f * wander * vecDriftY_.update(lastBlockSeconds_, rate * 1.6180339887f, auxRng_);
                }
                vx = clampv(vx, 0.0f, 1.0f);
                vy = clampv(vy, 0.0f, 1.0f);
                const float f[kSlots] = { (1.0f - vx) * (1.0f - vy),   // Source 1, bottom left
                                          vx * (1.0f - vy),            // Source 2, bottom right
                                          (1.0f - vx) * vy,            // Source 3, top left
                                          vx * vy };                   // Source 4, top right
                for (int k = 0; k < kSlots; ++k) {
                    const float factor = 1.0f + amount * (4.0f * f[k] - 1.0f);
                    vp_.slot[k].level = clampv(vp_.slot[k].level * factor, 0.0f, 1.0f);
                }
                vp_.level = vp_.slot[0].level;
            }
        }
        vp_.userTable = userTable_.frames > 0 ? &userTable_ : nullptr;
        {
            const int a = cyclesActive_.load(std::memory_order_acquire);
            vp_.userCycles = (a >= 0 && !userCycles_[a].empty()) ? &userCycles_[a] : nullptr;
        }
        for (int k = 0; k < kSlots; ++k) {
            const int a = textureActive_[k].load(std::memory_order_acquire);
            vp_.texture[k] = (a >= 0 && !textures_[k][a].empty()) ? &textures_[k][a] : nullptr;
        }
    }
    masterGain_     = g(ParamId::MasterGain);   // through g(), so the matrix can reach it
    subLevel_       = g(ParamId::SubLevel);
    subOctave_      = std::lround(g(ParamId::SubOctave)) == 0 ? 1 : 2;
    subGlide_       = g(ParamId::SubGlide);
    subBinaural_    = g(ParamId::SubBinaural);
    subPulse_       = g(ParamId::SubPulse);
    subTone_        = g(ParamId::SubTone);
    subHarmonics_   = g(ParamId::SubHarmonics);
    subBeat_        = g(ParamId::SubBeat);
    subSource_      = clampv(static_cast<int>(std::lround(g(ParamId::SubSource))), 0, 2);
    vp_.lowCut      = g(ParamId::PadLowCut);
    const bool hold = g(ParamId::Hold) >= 0.5f;
    if (hold_ && !hold) { for (int i = 0; i < 128; ++i) if (midiHeld_[i]) { midiHeld_[i] = false; stopNote(i, OwnerMidi); } }
    hold_ = hold;
    vp_.air         = g(ParamId::Air);
    vp_.airColor    = g(ParamId::AirColor);
    vp_.airQ        = g(ParamId::AirQ);
    vp_.attack      = g(ParamId::Attack);
    vp_.decay       = g(ParamId::Decay);
    vp_.sustain     = g(ParamId::Sustain);
    vp_.release     = g(ParamId::Release);
    vp_.filterOn    = g(ParamId::FilterOn) >= 0.5f;
    vp_.filterParallel = std::lround(g(ParamId::ZRoute)) == 1;
    vp_.filterModel = static_cast<int>(std::lround(g(ParamId::FilterModel)));
    vp_.filterDrive = g(ParamId::FilterDrive);
    vp_.fold = g(ParamId::FilterFold);
    vp_.binaural = std::lround(getParam(ParamId::Binaural)) == 1;
    vp_.headYawDeg = headYawDeg_.load(std::memory_order_relaxed);
    vp_.cutoff      = g(ParamId::Cutoff);
    vp_.resonance   = g(ParamId::Resonance);
    vp_.filterEnv   = g(ParamId::FilterEnv);
    vp_.filterDrift = g(ParamId::FilterDrift);
    vp_.keyTrack    = g(ParamId::KeyTrack);
    vp_.zMode       = static_cast<int>(std::lround(g(ParamId::ZMode)));
    vp_.zShape      = static_cast<int>(std::lround(g(ParamId::ZShape)));
    vp_.zX          = g(ParamId::ZX);
    vp_.zY          = g(ParamId::ZY);
    vp_.zZ          = g(ParamId::ZZ);
    vp_.zDecay      = g(ParamId::ZDecay);
    vp_.zDamp       = g(ParamId::ZDamp);
    vp_.zRate       = g(ParamId::ZRate);
    vp_.zDepth      = g(ParamId::ZDepth);
    vp_.zRes        = g(ParamId::ZResonance);
    vp_.zKeyTrack   = g(ParamId::ZKeyTrack);
    vp_.zMix        = g(ParamId::ZMix);
    vp_.panDrift    = g(ParamId::PanDrift);
    vp_.itd         = g(ParamId::Itd);
    vp_.presence    = g(ParamId::Presence);
    vp_.depthRange    = g(ParamId::DepthRange);      // the guide's depth model (25.09.2026): level, gap and width on the one distance
    vp_.depthPreDelay = g(ParamId::DepthPreDelay);
    vp_.depthWidth    = g(ParamId::DepthWidth);
    vp_.breath      = g(ParamId::Breath);
    vp_.breathRate  = g(ParamId::BreathRate);
    vp_.phaseWidth  = g(ParamId::PhaseWidth);
    vp_.phaseRate   = g(ParamId::PhaseRate);
    vp_.doppler     = g(ParamId::Doppler);
    vp_.externalise = g(ParamId::Externalise);
    vp_.elevNear = g(ParamId::ElevNear);
    vp_.elevFar = g(ParamId::ElevFar);
    vp_.depthLaw = g(ParamId::DepthLaw);
    vp_.nearIld = g(ParamId::NearIld);
    vp_.oneEuro = std::lround(g(ParamId::KeysFilter)) == 1;
    vp_.partialSpread = g(ParamId::PartialSpread);
    vp_.strikeLevel = g(ParamId::StrikeLevel);
    vp_.strikeType  = static_cast<int>(std::lround(g(ParamId::StrikeType)));
    vp_.strikeDecay = g(ParamId::StrikeDecay);
    vp_.strikeDamp  = g(ParamId::StrikeDamp);
    vp_.strikeBrain = std::lround(g(ParamId::StrikeWho)) == 1;
    strikeChance_   = g(ParamId::StrikeChance);
    strikeCluster_  = g(ParamId::StrikeCluster);
    cosmosSwell_    = g(ParamId::CosmosSwell);
    tide_       = g(ParamId::Tide);
    tidePeriod_ = g(ParamId::TidePeriod);
    vp_.pitchMul = tide_ > 0.0f ? std::pow(2.0f, tide_ * tideDrift_.value() / 1200.0f) : 1.0f;
    blurMix_    = g(ParamId::BlurMix);
    blur_.set(g(ParamId::BlurSmear));
    farRotate_  = g(ParamId::FarRotate);
    farWidth_   = g(ParamId::FarWidth);
    // Envelopment: the far bus's side channel lifted in the band that carries it. The band
    // starts where Bass Mono ends, because everything under that corner is folded to mono at
    // the master and lifting it here would be lifting something the guard then removes.
    envelop_ = g(ParamId::Envelop);
    comod_ = g(ParamId::FarComod);
    leniaRate_ = g(ParamId::LeniaRate);
    leniaMu_ = g(ParamId::LeniaGrowth);
    chaosPeriod_ = g(ParamId::ChaosPeriod);
    {
        // A bell between the two corners, centred on their geometric mean, as wide as they are
        // apart (Q from the bandwidth in octaves). Coefficients only: the filter's state stays.
        const float lo = std::max(125.0f, getParam(ParamId::BassMono));
        const float ratio = 500.0f / lo;
        envBell_.setQ(std::sqrt(lo * 500.0f), std::sqrt(ratio) / std::max(ratio - 1.0f, 0.05f), static_cast<float>(sr_));
    }
    farReverb_.setMode(clampv(static_cast<int>(std::lround(getParam(ParamId::FarMode))), 0, 3));

    depth_       = g(ParamId::Depth);
    keysDepth_   = g(ParamId::KeysDepth);
    arcAmount_   = g(ParamId::ArcAmount);
    arcClock_    = g(ParamId::ArcClock) >= 0.5f;
    arcPeriodMin_ = g(ParamId::ArcPeriod);
    // Sync choices: when set, the division at the current tempo replaces the free knob.
    auto syncedSeconds = [this](ParamId sync, float free) {
        const int d = clampv(static_cast<int>(std::lround(getParam(sync))), 0, kNumSyncDivs - 1);
        return syncOn(d) ? static_cast<float>(syncSeconds(d, bpm_)) : free;
    };
    auto syncedHz = [this](ParamId sync, float free) {
        const int d = clampv(static_cast<int>(std::lround(getParam(sync))), 0, kNumSyncDivs - 1);
        return syncOn(d) ? static_cast<float>(syncHz(d, bpm_)) : free;
    };
    arcPeriodMin_ = syncedSeconds(ParamId::ArcSync, arcPeriodMin_ * 60.0f) / 60.0f;

    bp_.on          = g(ParamId::BrainOn) >= 0.5f;
    bp_.density     = static_cast<int>(std::lround(g(ParamId::BrainDensity)));
    bp_.rateSeconds = syncedSeconds(ParamId::BrainSync, g(ParamId::BrainRate));
    bp_.holdMin     = g(ParamId::BrainHoldMin);
    bp_.holdMax     = g(ParamId::BrainHoldMax);
    bp_.low         = static_cast<int>(std::lround(g(ParamId::BrainLow)));
    bp_.high        = static_cast<int>(std::lround(g(ParamId::BrainHigh)));
    bp_.consonance  = g(ParamId::BrainConsonance);
    bp_.wander      = g(ParamId::BrainWander);
    // The spectrum the conductor judges by: the bank's own template, as the voice renders it.
    bp_.timbre = g(ParamId::BrainTimbre);
    bp_.spacing = g(ParamId::BrainSpacing);
    bp2_.spacing = bp_.spacing;
    bp_.harmonic = g(ParamId::BrainHarmonic);
    bp2_.harmonic = bp_.harmonic;
    bp_.key = g(ParamId::BrainKey);
    bp2_.key = bp_.key;
    bp_.even = g(ParamId::BrainEven);
    bp2_.even = bp_.even;
    bp_.smooth = g(ParamId::BrainSmooth);
    bp2_.smooth = bp_.smooth;
    // Blend was declared, documented and tested in the third research round -- and never read
    // into the conductor. The test drove the conductor directly, so it passed; through the
    // engine the knob did nothing. Found while adding the lines below it.
    bp_.blend = g(ParamId::BrainBlend);
    bp2_.blend = bp_.blend;
    bp_.cascade = g(ParamId::BrainCascade);
    bp2_.cascade = bp_.cascade;
    bp_.surprise = g(ParamId::BrainSurprise);
    bp2_.surprise = bp_.surprise;
    bp_.homeostat = g(ParamId::BrainHomeostat);
    bp2_.homeostat = bp_.homeostat;
    bp_.dejavu = g(ParamId::BrainDejaVu);
    bp2_.dejavu = bp_.dejavu;
    // The register roles and the intervals that belong to them. Both conductors get them: the
    // background plane is the one that most needs to be told not to put a third in the bass.
    bp_.layers      = g(ParamId::BrainLayers);       bp2_.layers      = bp_.layers;
    bp_.bassHold    = g(ParamId::BrainBassHold);     bp2_.bassHold    = bp_.bassHold;
    bp_.topSoft     = g(ParamId::BrainTopSoft);      bp2_.topSoft     = bp_.topSoft;
    bp_.lowSpacing  = g(ParamId::BrainLowSpacing);   bp2_.lowSpacing  = bp_.lowSpacing;
    bp_.thirdFloor  = static_cast<int>(std::lround(g(ParamId::BrainThirdFloor)));
    bp2_.thirdFloor = bp_.thirdFloor;
    bp_.leading     = g(ParamId::BrainLeading);      bp2_.leading     = bp_.leading;
    bp_.thirds      = g(ParamId::BrainThirds);       bp2_.thirds      = bp_.thirds;
    bp_.seconds     = g(ParamId::BrainSeconds);      bp2_.seconds     = bp_.seconds;
    bp_.seventh     = g(ParamId::BrainSeventh);      bp2_.seventh     = bp_.seventh;
    // What the clock may and may not do.
    bp_.rateBreath   = g(ParamId::BrainRateBreath);    bp2_.rateBreath   = bp_.rateBreath;
    bp_.breathPeriod = g(ParamId::BrainBreathPeriod);  bp2_.breathPeriod = bp_.breathPeriod;
    bp_.overlap      = g(ParamId::BrainOverlap);       bp2_.overlap      = bp_.overlap;
    bp_.onsetGuard   = g(ParamId::BrainOnsetGuard) >= 0.5f;
    bp2_.onsetGuard  = bp_.onsetGuard;
    bp_.releaseGap   = g(ParamId::BrainReleaseGap);    bp2_.releaseGap   = bp_.releaseGap;
    bp_.retrigger    = g(ParamId::BrainRetrigger);     bp2_.retrigger    = bp_.retrigger;
    bp_.densitySlew  = g(ParamId::BrainDensitySlew);   bp2_.densitySlew  = bp_.densitySlew;
    bp_.silence      = g(ParamId::BrainSilence);       bp2_.silence      = bp_.silence;
    bp_.silenceLen   = g(ParamId::BrainSilenceLen);    bp2_.silenceLen   = bp_.silenceLen;
    // Root and long form.
    bp_.rootSteps  = static_cast<int>(std::lround(g(ParamId::BrainRootSteps)));  bp2_.rootSteps  = bp_.rootSteps;
    bp_.rootDown   = g(ParamId::BrainRootDown);        bp2_.rootDown   = bp_.rootDown;
    bp_.pivot      = g(ParamId::BrainPivot);           bp2_.pivot      = bp_.pivot;
    bp_.home       = g(ParamId::BrainHome);            bp2_.home       = bp_.home;
    bp_.homeTime   = g(ParamId::BrainHomeTime);        bp2_.homeTime   = bp_.homeTime;
    bp_.memory     = g(ParamId::BrainMemory);          bp2_.memory     = bp_.memory;
    bp_.degreeSwap = g(ParamId::BrainDegreeSwap);      bp2_.degreeSwap = bp_.degreeSwap;
    // The review against ambient harmony (25.09.2026): the root's targets, the undertone sets and
    // the Harmonic Cloud. The facts about the tuning the review's fixes read are set with the tuning below.
    bp_.rootTargets = clampv(static_cast<int>(std::lround(g(ParamId::BrainRootTargets))), 0, 3);   bp2_.rootTargets = bp_.rootTargets;
    bp_.utonal     = g(ParamId::BrainUtonal);          bp2_.utonal     = bp_.utonal;
    bp_.series     = g(ParamId::BrainSeries);          bp2_.series     = bp_.series;
    bp_.loop = static_cast<int>(std::lround(g(ParamId::BrainLoop)));
    bp2_.loop = bp_.loop;
    bp_.spread = g(ParamId::BrainSpread);
    bp2_.spread = bp_.spread;
    bp_.bias = g(ParamId::BrainBias);
    bp2_.bias = bp_.bias;
    // The spectrum is needed by the conductor's Timbre ear and by the timbre scale, and the
    // scale is read further down, so the question is asked here from the raw value.
    const bool wantTimbreScale = std::lround(getParam(ParamId::Scale)) == kTimbreScaleIndex;
    {   // Computed every block now, not only when Timbre or the timbre scale asks: the panel's
        // Tuning display draws the roughness curve from it. A dozen pows; nothing downstream
        // reads it unless Timbre is up, so the sound is untouched.
        (void)wantTimbreScale;
        const int count = std::min(BrainSpectrum::kMax, std::max(1, vp_.partials));
        const float hc = 1.0f + vp_.brightness * vp_.brightness * 31.0f;
        const double B = static_cast<double>(vp_.inharmonic) * vp_.inharmonic * 0.02;
        brainSpec_.count = count;
        for (int h = 1; h <= count; ++h) {
            double a = std::pow(static_cast<double>(h), -static_cast<double>(vp_.tilt));
            if (vp_.oddEven > 0.0f && (h % 2) == 0) a *= 1.0 - vp_.oddEven;
            if (vp_.oddEven < 0.0f && (h % 2) == 1 && h > 1) a *= 1.0 + vp_.oddEven;
            if (static_cast<float>(h) > hc) { const float x = std::min((static_cast<float>(h) - hc) / 6.0f, 1.0f); a *= 0.5 * (1.0 + std::cos(kPi * x)); }
            brainSpec_.amp[h - 1] = a;
            brainSpec_.ratio[h - 1] = vp_.match > 0.0f ? static_cast<double>(vp_.partialRatio[h - 1])
                                                       : h * (B > 0.0 ? std::sqrt(1.0 + B * h * h) : 1.0);
        }
    }
    bp_.spectrum = &brainSpec_;
    bp2_.timbre = bp_.timbre;
    bp2_.spectrum = &brainSpec_;
    brainQuant_     = clampv(static_cast<int>(std::lround(g(ParamId::BrainQuantize))), 0, kNumSyncDivs - 1);
    // Autoplay. In Chords the conductor keeps the cluster full and exchanges one voice at a time;
    // the rate can come from the clock instead of the seconds knob.
    bp_.mode         = static_cast<BrainMode>(clampv(static_cast<int>(std::lround(g(ParamId::AutoMode))), 0, kNumBrainModes - 1));
    bp_.voiceLead    = g(ParamId::AutoLead);
    bp_.chordTension = g(ParamId::AutoTension);
    bp_.rootMove     = g(ParamId::AutoRootMove);
    if (bp_.mode == BrainMode::Chords) bp_.rateSeconds = syncedSeconds(ParamId::AutoSync, g(ParamId::AutoRate));
    {   // Step is a trigger: it fires on the rising edge and the host's switch is left alone.
        const bool now = g(ParamId::AutoStep) >= 0.5f;
        if (now && !autoStepWas_) brain_.requestStep();
        autoStepWas_ = now;
        if (autoStepAsked_.exchange(false, std::memory_order_relaxed)) brain_.requestStep();
    }
    vp_.pressDistance = g(ParamId::PressDistance);
    vp_.pressBright   = g(ParamId::PressBright);
    vp_.pressLevel    = g(ParamId::PressLevel);
    vp_.slideCutoff   = g(ParamId::SlideCutoff);
    vp_.slideZ        = g(ParamId::SlideZ);
    bendRange_        = g(ParamId::BendRange);
    // The second conductor. Its root follows the first one's plus an interval, so the two stay
    // in one harmony however far the first one's root wanders.
    brain2On_        = g(ParamId::Brain2On) >= 0.5f;
    bp2_.on          = brain2On_;
    bp2_.density     = static_cast<int>(std::lround(g(ParamId::Brain2Density)));
    bp2_.rateSeconds = g(ParamId::Brain2Rate);
    bp2_.holdMin     = g(ParamId::Brain2HoldMin);
    bp2_.holdMax     = g(ParamId::Brain2HoldMax);
    bp2_.low         = static_cast<int>(std::lround(g(ParamId::Brain2Low)));
    bp2_.high        = static_cast<int>(std::lround(g(ParamId::Brain2High)));
    bp2_.consonance  = g(ParamId::Brain2Consonance);
    bp2_.wander      = 0.0f;   // it follows the first conductor's root instead of wandering itself
    brain2Depth_     = g(ParamId::Brain2Depth);
    brain2Interval_  = static_cast<int>(std::lround(g(ParamId::Brain2Interval)));
    // Golden: the background layer's clock is stretched against the foreground's by the one ratio
    // that has no good rational approximation, so the two never fall into a simple relation and
    // are never heard as one (Anti 15). It overrides Brain 2's own rate and holds, which is the
    // point -- a ratio cannot be guaranteed while both ends are set by hand.
    if (g(ParamId::Brain2Golden) >= 0.5f) {
        constexpr double phi = 1.6180339887498949;
        bp2_.rateSeconds = static_cast<float>(bp_.rateSeconds * phi);
        bp2_.holdMin     = static_cast<float>(bp_.holdMin * phi);
        bp2_.holdMax     = static_cast<float>(bp_.holdMax * phi);
    }

    // Hour-scale arc: a very slow drift that leans on density, brightness and depth.
    const float a = arcOut_ * arcAmount_;
    bp_.density = clampv(bp_.density + static_cast<int>(std::lround(a * 2.0f)), 1, ClusterBrain::kSlots);
    // And on the harmony, if asked. Lerdahl and Krumhansl (2007) modelled tonal tension and tested
    // it against listeners: tension rises with distance from the tonic in pitch space and with
    // surface dissonance, and falls back as the music returns. This instrument's proxies for those
    // are the three judgements the conductor already makes -- rootedness (Harmonic), stability in
    // the key (Key), and consonance -- so the arc's rise loosens all three and its fall tightens
    // them. The same lean, in the same direction, as the arc gives density and brightness: the
    // climax of the night is denser, brighter, and further from home.
    arcHarmony_ = g(ParamId::ArcHarmony);
    arcLean_ = arcOut_ * arcHarmony_;
    if (arcHarmony_ > 0.0f) {
        const float lean = arcLean_;
        auto leaned = [lean](float base) {
            // Tense: down towards a fraction of itself. Relaxed: up towards one. Continuous
            // through zero, where nothing happens.
            return lean >= 0.0f ? base * (1.0f - 0.8f * lean) : base + (1.0f - base) * (0.6f * -lean);
        };
        bp_.harmonic = clampv(leaned(bp_.harmonic), 0.0f, 1.0f);
        bp_.key = clampv(leaned(bp_.key), 0.0f, 1.0f);
        bp_.consonance = clampv(leaned(bp_.consonance), 0.0f, 1.0f);
        bp2_.harmonic = bp_.harmonic; bp2_.key = bp_.key;
    }
    vp_.brightness = clampv(vp_.brightness * (1.0f + 0.25f * a), 0.0f, 1.0f);
    depth_ = clampv(depth_ * (1.0f + 0.3f * a), 0.0f, 1.0f);

    ensemble_.set(g(ParamId::EnsembleMix), g(ParamId::EnsembleDepth), syncedHz(ParamId::EnsembleSync, g(ParamId::EnsembleRate)));
    ensemble_.setMode(clampv(static_cast<int>(std::lround(getParam(ParamId::EnsMode))), 0, 2));
    haas_.set(g(ParamId::Haas), g(ParamId::HaasTime));
    early_.setRoom(g(ParamId::EarlySize), g(ParamId::EarlyAbsorb), g(ParamId::EarlyWidth));
    early_.setLevel(g(ParamId::EarlyLevel));
    {   // Where the room thinks the sound is: the level-weighted mean of the sounding voices'
        // pan and distance. One position for the whole near bus, which is the simplification the
        // class documents; it follows the music, which is what the early pattern is for.
        float wsum = 0.0f, pan = 0.0f, dist = 0.0f;
        for (const auto& v : voices_) {
            if (!v.isActive()) continue;
            const float wgt = v.level();
            wsum += wgt; pan += wgt * v.pan(); dist += wgt * v.distance();
        }
        if (wsum > 1.0e-6f) early_.setSource(pan / wsum, dist / wsum);
    }
    delay_.set(syncedSeconds(ParamId::DelaySyncL, g(ParamId::DelayTimeL)), syncedSeconds(ParamId::DelaySyncR, g(ParamId::DelayTimeR)),
               g(ParamId::DelayFeedback), g(ParamId::DelayCross), g(ParamId::DelayDamp), g(ParamId::DelayAbsorb));
    delay_.setDuck(g(ParamId::DelayDuck));
    delayMix_   = g(ParamId::DelayMix);
    delayToFar_ = g(ParamId::DelayToFar);
    delay2_.set(syncedSeconds(ParamId::Delay2SyncL, g(ParamId::Delay2TimeL)), syncedSeconds(ParamId::Delay2SyncR, g(ParamId::Delay2TimeR)),
                g(ParamId::Delay2Feedback), g(ParamId::Delay2Cross), g(ParamId::Delay2Damp), g(ParamId::Delay2Absorb));
    delay2Mix_   = g(ParamId::Delay2Mix);
    delay2ToFar_ = g(ParamId::Delay2ToFar);
    cloud_.set(syncedHz(ParamId::CloudSync, g(ParamId::CloudDensity)), g(ParamId::CloudSize), g(ParamId::CloudPitch), g(ParamId::CloudSpray), g(ParamId::CloudLevel));
    cloud_.setLoop(g(ParamId::CloudFeedback), g(ParamId::CloudTone));
    cloud_.setScatter(g(ParamId::CloudTranspose), g(ParamId::CloudScatter));
    cloud_.setSwarm(g(ParamId::CloudSwarm));
    cloud_.setShift(kCloudShiftSemitones[clampv(static_cast<int>(std::lround(g(ParamId::CloudShift))), 0, 6)]);
    cloud_.setResonators(g(ParamId::CloudResonance), static_cast<int>(std::lround(g(ParamId::CloudResMode))),
                         static_cast<int>(std::lround(g(ParamId::CloudResNotes))), g(ParamId::CloudResDecay));
    cloudSend_ = g(ParamId::CloudSend);
    cloudToNear_ = g(ParamId::CloudToNear);
    // Send Low Cut: one high-pass in front of all three reverbs' inputs (25.09.2026).
    sendLowcut_ = g(ParamId::SendLowcut);
    nearReverb_.setSendLowcut(sendLowcut_);
    farReverb_.setSendLowcut(sendLowcut_);
    if (sendLowcut_ > 21.0f) { roomSendHpL_.setQ(clampv(sendLowcut_, 20.0f, 1000.0f), 0.7071f, static_cast<float>(sr_)); roomSendHpR_.copyCoefficients(roomSendHpL_); }
    nearReverb_.setSpace(0.3f, 20000.0f, g(ParamId::NearLowcut));
    nearReverb_.set(0.6f, g(ParamId::NearDecay), g(ParamId::NearDamp), 5.0f, false, g(ParamId::NearMix));
    unmask_.set(g(ParamId::FarUnmask), g(ParamId::FarUnmaskSpread), g(ParamId::FarUnmaskReturn));
    // The guide's second round (25.09.2026): the main room ducks under the near bus on the same
    // knob as the far hall, feeds the far hall (To Far), the far return loses the low middle of its
    // centre (Mid Low Cut), and the Foundation has a ceiling of its own.
    roomUnmask_.set(g(ParamId::FarUnmask), g(ParamId::FarUnmaskSpread), g(ParamId::FarUnmaskReturn));
    roomToFar_ = g(ParamId::RoomToFar);
    farMidLowcut_ = g(ParamId::FarMidLowcut);
    if (farMidLowcut_ > 21.0f) farMidHp_.setQ(clampv(farMidLowcut_, 20.0f, 1000.0f), 0.7071f, static_cast<float>(sr_));
    subCeiling_ = g(ParamId::SubCeiling);
    bodyLevel_ = g(ParamId::BodyLevel);
    bodyPitch_ = g(ParamId::BodyPitch);
    body_.set(static_cast<BodyMaterial>(clampv(static_cast<int>(std::lround(g(ParamId::BodyMaterial))), 0, kNumBodyMaterials - 1)),
              static_cast<float>(frequencyOf(brain_.root())) * bodyPitch_ * vp_.pitchMul,
              g(ParamId::BodyDecay), g(ParamId::BodyTone), g(ParamId::BodySpread));
    patina_.set(g(ParamId::PatinaAmount), g(ParamId::PatinaWow), g(ParamId::PatinaHiss), g(ParamId::PatinaAge));
    farReverb_.setSpace(g(ParamId::FarAsym), g(ParamId::FarHighcut), g(ParamId::FarLowcut));
    farReverb_.set(g(ParamId::FarSize), g(ParamId::FarDecay), g(ParamId::FarDamp), g(ParamId::FarPreDelay), g(ParamId::FarFreeze) >= 0.5f, 1.0f);
    farLevel_ = g(ParamId::FarLevel);
    midSide_.set(g(ParamId::BassMono), g(ParamId::SideAir), g(ParamId::Width));
    midSide_.setMonoGuard(g(ParamId::MonoGuard) >= 0.5f);
    midSide_.setTilt(g(ParamId::Tilt2), g(ParamId::TiltPivot));
    diffuser_.set(g(ParamId::FarDiffuse));
    farDiffuse_ = g(ParamId::FarDiffuse);   // also drives the air saturation ahead of the reverb
    sympathy_ = g(ParamId::Sympathy);
    vp_.sympathy = sympathy_;
    roomLevel_    = g(ParamId::RoomLevel);
    roomSource_   = static_cast<int>(std::lround(g(ParamId::RoomSource)));
    roomPreDelay_ = static_cast<int>(g(ParamId::RoomPreDelay) * 0.001f * static_cast<float>(sr_));
    roomHighcut_  = g(ParamId::RoomHighcut);
    roomLowcut_   = g(ParamId::RoomLowcut);
    subsonicHz_   = g(ParamId::Subsonic);
    roomMorph_    = hasImpulseB_ ? g(ParamId::RoomMorph) : 0.0f;
    fbBus_   = g(ParamId::FeedbackBus);
    fbFm_    = g(ParamId::FeedbackFm);
    fbTone_  = g(ParamId::FeedbackTone);
    fbDrive_ = g(ParamId::FeedbackDrive);
    fbBias_  = g(ParamId::FeedbackBias);
    vp_.fmAmount = fbFm_;

    // Cosmos
    cosmosSend_   = g(ParamId::CosmosSend);
    cosmosReturn_ = g(ParamId::CosmosReturn);
    cosmosToFar_  = g(ParamId::CosmosToFar);
    cosmosNebula_ = g(ParamId::CosmosNebula);
    const float shiftHz = g(ParamId::CosmosShift) * (1.0f + 0.5f * g(ParamId::CosmosShiftDrift) * shiftDrift_.value());
    shifter_.set(shiftHz, shiftHz == 0.0f ? 0.0f : 1.0f);
    const float rootHz = static_cast<float>(scaleFrequency(*scale_, brain_.root(), 60 + clampv(static_cast<int>(std::lround(g(ParamId::RootNote))), 0, 11), g(ParamId::RefPitch), snapKeys_));
    resonator_.set(rootHz * g(ParamId::CosmosResPitch), g(ParamId::CosmosResFeedback), g(ParamId::CosmosRes));
    {   // The cloud's scatter and resonators hear the same root, placed in the scale against the key's
        // tonic; the Memory's Seek listens for the scale from that tonic.
        const int keyRoot = 60 + clampv(static_cast<int>(std::lround(g(ParamId::RootNote))), 0, 11);
        const double tonicHz = scaleFrequency(*scale_, keyRoot, keyRoot, g(ParamId::RefPitch), snapKeys_);
        cloud_.setHarmony(*scale_, tonicHz, rootHz);
        memory_.setHarmony(*scale_, tonicHz);
    }

    // Memory
    memSend_   = g(ParamId::MemSend);
    memReturn_ = g(ParamId::MemReturn);
    memToFar_  = g(ParamId::MemToFar);
    memory_.setShape(2 << clampv(static_cast<int>(std::lround(g(ParamId::MemLines))), 0, 2),
                     g(ParamId::MemSize), g(ParamId::MemBlur), g(ParamId::MemDrift));
    memory_.setDecay(g(ParamId::MemHold), g(ParamId::MemAge), g(ParamId::MemRenew), g(ParamId::MemDrive));
    memory_.setRecall(g(ParamId::MemRecall), g(ParamId::MemSeek), g(ParamId::MemGrain));
    memory_.setTape(g(ParamId::MemReverse) >= 0.5f, g(ParamId::MemHalf) >= 0.5f,
                    g(ParamId::MemFreeze) >= 0.5f, g(ParamId::MemErase) >= 0.5f);
    vowel_.set(g(ParamId::CosmosVowel), g(ParamId::CosmosVowelRate));
    nebula_.set(g(ParamId::CosmosSmear));
    cosmosShimmer_ = g(ParamId::CosmosShimmer);
    const int sp = clampv(static_cast<int>(std::lround(g(ParamId::CosmosShimmerPitch))), 0, kNumShimmerPitches - 1);
    shimmerL_.setSemitones(kShimmerPitchSemitones[sp]);
    shimmerR_.setSemitones(kShimmerPitchSemitones[sp]);
    shimmerSpec_.setSemitones(kShimmerPitchSemitones[sp]);
    shimmerMode_ = clampv(static_cast<int>(std::lround(g(ParamId::CosmosShimmerMode))), 0, 1);

    // Tuning
    const int scaleIdx = clampv(static_cast<int>(std::lround(g(ParamId::Scale))), 0, kNumScaleChoices - 1);
    // The scale that is computed rather than tabulated. It is rebuilt only when the spectrum it
    // is made of has actually moved, and at most a few times a second: sweeping the dissonance
    // curve is a third of a millisecond, which is nothing now and then and too much every block.
    // A knob turn therefore retunes the instrument in small steps rather than continuously, which
    // is also kinder to a chord that is already sounding.
    bool scaleRebuilt = false;
    if (scaleIdx == kTimbreScaleIndex && brainSpec_.count > 0) {
        double sig = brainSpec_.count;
        for (int i = 0; i < brainSpec_.count; ++i) sig += brainSpec_.ratio[i] * 7.0 + brainSpec_.amp[i] * 131.0;
        if (++timbreScaleWait_ >= 96 && std::fabs(sig - timbreScaleSig_) > 1.0e-9) {
            timbreScaleWait_ = 0;
            timbreScaleSig_ = sig;
            FixedScale built;
            if (makeTimbreScale(brainSpec_, built)) { scales_[kTimbreScaleIndex] = built; scaleRebuilt = true; }
        }
    }
    scale_ = &scales_[scaleIdx];
    refPitch_ = g(ParamId::RefPitch);
    {   // The stretched octave. A change while notes are held retunes them like a purity change.
        const float st = g(ParamId::TuneStretch);
        if (st != stretchCents_) { stretchCents_ = st; stretchChanged_ = true; }
    }
    snapKeys_ = std::lround(g(ParamId::KeyMap)) == 0;
    const int rootPc = clampv(static_cast<int>(std::lround(g(ParamId::RootNote))), 0, 11);
    {
        // R4.7: when the root moves, the ground does not move under what is already sounding. Each
        // voice keeps the frequency it was given, by carrying the ratio the move would have made;
        // notes that begin afterwards take the new root's ratios, so the harmony changes by what
        // enters rather than by everything sliding at once. Off, everything glides as before.
        const int newRoot = 60 + rootPc;
        if (newRoot != rootNote_ && g(ParamId::TuneHoldSounding) >= 0.5f) {
            double before[kMaxVoices];
            int i = 0;
            for (const auto& v : voices_) { before[i] = v.isActive() ? frequencyOf(v.note()) : 0.0; ++i; }
            rootNote_ = newRoot;
            i = 0;
            for (auto& v : voices_) {
                const double b = before[i++];
                if (b > 0.0) { const double a2 = frequencyOf(v.note()); if (a2 > 0.0) v.setRootComp(v.rootComp() * static_cast<float>(b / a2)); }
            }
        } else rootNote_ = newRoot;
    }
    {   // What the conductors need to know about the tuning (the review's F1 and F6): the scale, for
        // a key profile of its own and for placing a difference tone; whether it repeats at the
        // octave; and whether a MIDI step is a semitone at all.
        const bool octave = std::fabs(scale_->period - 2.0) < 1.0e-9;
        const double firstHz = scaleFrequency(*scale_, rootNote_, rootNote_, refPitch_, snapKeys_);
        for (BrainParams* b : { &bp_, &bp2_ }) {
            b->scale = scale_;
            b->octavePeriodic = octave;
            b->consecutive = !snapKeys_ || !octave;   // a non-octave scale is walked degree by degree whatever Key Map says
            b->scaleRootHz = firstHz;
        }
    }
    {
        const float purity = g(ParamId::TunePurity), drift = g(ParamId::TuneDrift);
        float wander = drift > 0.0f ? 0.5f * drift * purityDrift_.value() : 0.0f;   // drifter is advanced in process()
        // The fluctuation guard. Fastl and Zwicker: the sensation of fluctuation peaks at a
        // modulation rate of 4 Hz and is gone by about 20; roughness takes over at around 70.
        // Between two and eight hertz a beating chord is heard as wobble, which in a sleep concert
        // is the one thing it must not be. Purity Drift makes beats without knowing where they
        // land; the BEAT source already measures where they landed. Above zero the guard reins
        // the drift in while the beat sits in that band, so the beat slows out of it; below zero
        // it does the opposite and seeks the wobble out. Zero leaves the drift exactly alone.
        guardFactor_ = 1.0f;
        {
            const float guard = g(ParamId::TuneGuard);
            if (guard != 0.0f && drift > 0.0f && beatHz_ > 0.05f) {
                const float x = std::log2(beatHz_ / 4.0f);                 // 0 at 4 Hz, +-1 at 2 and 8
                const float inBand = std::exp(-x * x * 1.25f);              // 1 at 4 Hz, 0.29 at the edges
                if (guard > 0.0f) guardFactor_ = 1.0f - guard * inBand;
                else guardFactor_ = 1.0f + (-guard) * (1.0f - inBand) * (beatHz_ < 4.0f ? 1.0f : -1.0f);
                guardFactor_ = clampv(guardFactor_, 0.0f, 2.0f);
                wander *= guardFactor_;
            }
        }
        purityCur_ = clampv(purity + wander, 0.0f, 1.0f);
        adaptAmt_ = g(ParamId::TuneAdapt);
        {
            static const double kTranspose[7] = { 0.0, 0.41503749927884381, 0.58496250072115619, 1.0, -0.41503749927884381, -0.58496250072115619, -1.0 };
            transposeTarget_ = kTranspose[clampv(static_cast<int>(std::lround(g(ParamId::Transpose))), 0, 6)];
        }
        retune_ = purityCur_ < 0.9999 || drift > 0.0f || stretchChanged_ || scaleRebuilt || adaptAmt_ > 0.0f || commaCents_ != 0.0
               || transposeCur_ != transposeTarget_ || transposeCur_ != 0.0;
        stretchChanged_ = false;
    }
    vp_.freeze = g(ParamId::Freeze) >= 0.5f;
    vp_.airGhost = std::lround(g(ParamId::AirMode)) == 1;
    vp_.rootHz = frequencyOf(brain_.root());
    portamento_ = g(ParamId::Portamento);
    portaGravity_ = g(ParamId::PortaGravity);
    fbTape_ = g(ParamId::FeedbackTape);
    {   // Coherence: four Kuramoto oscillators, coupled by K = Coherence; their sines become
        // offsets on brightness, depth, pan and the z-plane point, scaled by Depth.
        const float depth = g(ParamId::CoherenceDepth);
        vp_.cohBrightness = 0.15f * depth * std::sin(kuraPhase_[0]);
        depth_ = clampv(depth_ + 0.15f * depth * std::sin(kuraPhase_[1]), 0.0f, 1.0f);
        vp_.cohPan = 0.4f * depth * std::sin(kuraPhase_[2]);
        vp_.cohZ = 0.25f * depth * std::sin(kuraPhase_[3]);
    }
    if (rootPc != lastRootPc_) {
        lastRootPc_ = rootPc;
        brain_.setRoot(48 + rootPc);
    }
    // ---- the near layer (13.09.2026)
    rolesUsed_ = false;
    for (int k = 0; k < kSlots; ++k) if (vp_.slot[k].role != SlotRole::All) rolesUsed_ = true;
    np_.level     = g(ParamId::ForeLevel);
    np_.kind      = clampv(static_cast<int>(std::lround(g(ParamId::ForeKind))), 0, 2);
    np_.rate      = g(ParamId::ForeRate);
    np_.chance    = g(ParamId::ForeChance);
    np_.cluster   = g(ParamId::ForeCluster);
    np_.length    = g(ParamId::ForeLength);
    np_.pitch     = clampv(static_cast<int>(std::lround(g(ParamId::ForePitch))), 0, 4);
    np_.spread    = g(ParamId::ForeSpread);
    np_.approach  = g(ParamId::ForeApproach);
    np_.distance  = g(ParamId::ForeDistance);
    np_.dry       = g(ParamId::ForeDry);
    np_.toDelay2  = g(ParamId::ForeDelay2);
    np_.toCosmos  = g(ParamId::ForeCosmos);
    np_.proximity = g(ParamId::ForeProximity);
    np_.hold      = g(ParamId::ForeHold) >= 0.5f;
    np_.glide     = g(ParamId::ForeGlide);
    np_.steps     = static_cast<int>(std::lround(g(ParamId::ForeSteps)));
    np_.stepSeconds = g(ParamId::ForeStep);
    np_.stepSync  = clampv(static_cast<int>(std::lround(g(ParamId::ForeStepSync))), 0, kNumSyncDivs - 1);
    np_.mutation  = g(ParamId::ForeMutation);
    np_.scatter   = g(ParamId::ForeScatter);
    np_.bloom     = g(ParamId::ForeBloom);
    np_.attack    = g(ParamId::ForeAttack);
    np_.release   = g(ParamId::ForeRelease);
    nearGain_     = dbToGain(g(ParamId::ForeGain));
    {
        // What a near event's voice renders with: the sound preset's voice with its four slots
        // put out and the Near Source in the last one, its own envelope, its own filter, its own
        // strike; no z-plane, no air, no bloom, no breath -- the event is placed by the scheduler
        // and shaped by the section, and owes the background nothing but the room around it.
        vpNear_ = vp_;
        for (int k = 0; k < kSlots; ++k) { vpNear_.slot[k].type = SourceType::Off; vpNear_.slot[k].role = SlotRole::All; }
        SlotParams& s = vpNear_.slot[kSlots - 1];
        s = SlotParams{};
        s.type          = static_cast<SourceType>(clampv(static_cast<int>(std::lround(g(ParamId::ForeType))), 0, kNumSourceTypes - 1));
        s.level         = np_.level;
        s.octave        = static_cast<int>(std::lround(g(ParamId::ForeOctave)));
        s.ratio         = static_cast<int>(std::lround(g(ParamId::ForeRatio)));
        s.position      = g(ParamId::ForePosition);
        s.positionDrift = g(ParamId::ForePosDrift);
        s.density       = g(ParamId::ForeDensity);
        s.follow        = g(ParamId::ForeFollow) >= 0.5f;
        s.bright        = g(ParamId::ForeBright);
        s.bowForce      = g(ParamId::ForeForce);
        s.bowSpeed      = g(ParamId::ForeSpeed);
        s.noise         = static_cast<NoiseKind>(clampv(static_cast<int>(std::lround(g(ParamId::ForeNoise))), 0, kNumNoiseKinds - 1));
        s.noiseQ        = g(ParamId::ForeNoiseQ);
        s.fmRatio       = g(ParamId::ForeFmRatio);
        s.fmIndex       = g(ParamId::ForeFmIndex);
        s.partials      = static_cast<int>(std::lround(g(ParamId::ForePartials)));
        s.tilt          = g(ParamId::ForeTilt);
        s.inharm        = g(ParamId::ForeInharm);
        s.drift         = g(ParamId::ForeDrift);
        s.table         = static_cast<int>(std::lround(g(ParamId::ForeTable)));
        vpNear_.attack      = np_.attack;
        vpNear_.decay       = g(ParamId::ForeDecay);
        vpNear_.sustain     = g(ParamId::ForeSustain);
        vpNear_.release     = np_.release;
        vpNear_.cutoff      = g(ParamId::ForeCutoff);
        vpNear_.resonance   = g(ParamId::ForeResonance);
        vpNear_.filterModel = clampv(static_cast<int>(std::lround(g(ParamId::ForeFilterModel))), 0, kNumFilterModels - 1);
        vpNear_.filterEnv   = g(ParamId::ForeFilterEnv);
        vpNear_.filterOn    = true;
        vpNear_.filterParallel = false;
        vpNear_.filterDrive = 0.0f;
        vpNear_.fold        = 0.0f;
        vpNear_.zMode       = 0;
        vpNear_.air         = 0.0f;
        vpNear_.strikeLevel = g(ParamId::ForeStrike);
        vpNear_.strikeType  = static_cast<int>(std::lround(g(ParamId::ForeStrikeType)));
        vpNear_.strikeDecay = g(ParamId::ForeStrikeDecay);
        vpNear_.strikeDamp  = g(ParamId::ForeStrikeDamp);
        vpNear_.strikeBrain = true;
        vpNear_.velAttack   = 0.0f;
        vpNear_.bloom       = 0.0f;
        vpNear_.breath      = 0.0f;
        vpNear_.freeze      = false;
        vpNear_.sympathy    = 0.0f;
        vpNear_.fmAmount    = 0.0f;
        vpNear_.partialSpread = 0.0f;
        vpNear_.lowCut      = 0.0f;
        {   // The near source's own clip in its slot -- the event's pick of the pool -- or Source 4's where it has none.
            const int a = nearPoolActive_.load(std::memory_order_acquire);
            if (a >= 0 && !nearPools_[a].empty()) {
                const std::vector<Texture>& pool = nearPools_[a];
                vpNear_.texture[kSlots - 1] = &pool[std::min(static_cast<size_t>(std::max(0, nearPick_)), pool.size() - 1)];
            }
        }
    }
    const int seed = static_cast<int>(std::lround(g(ParamId::Seed)));
    if (seed != seed_) {
        seed_ = seed;
        rng_.seed(static_cast<uint64_t>(seed_) + 1);
        brain_.reset(rng_.fork(), 48 + rootPc);
        brain2_.reset(0x2B2A1Full + static_cast<uint64_t>(seed_), 48 + rootPc);
        arc_.init(rng_);
    }
}

} // namespace ambient
