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
    while (m_modLock.test_and_set(std::memory_order_acquire)) { }
}

void Engine::unlockModulation() { m_modLock.clear(std::memory_order_release); }

/**
 * @brief Clears the pending matrix and shapes WITHOUT announcing them: the announcement is what makes
 *        the audio thread copy, and it must not do that while the rest is still being written.
 */
void Engine::clearPendingModulation()
{
    m_matrixPending.clear();
    // A default shape every envelope starts from: up over a quarter of its length, down over the
    // rest. Time is scaled by the envelope's own Time parameter, so this is a shape, not a length.
    for (auto& e : m_envPending) e.parse("0:0/1:1/4:0");
    // And every source's own: a rise to its level, which is what the plain Rise does as well.
    for (auto& e : m_srcEnvPending) e.parse(SrcEnvDefault);
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
    m_modVersion.fetch_add(1, std::memory_order_release);
}

bool Engine::applyPresetModulation(const Preset& p)
{
    // Cleared, then filled, then announced -- once, at the end. It used to announce the clear
    // and then parse into the same object, so the audio thread could be copying the matrix
    // while the message thread was still writing it.
    lockModulation();
    clearPendingModulation();
    bool ok = true;
    if (p.m_mod != nullptr && *p.m_mod) ok = m_matrixPending.parse(p.m_mod) && ok;
    if (p.m_envs != nullptr && *p.m_envs) {
        // The six modulation envelopes, then the four sources' own: one list with '~' between
        // them, so a field written before the sources had shapes simply stops after the sixth.
        const char* s = p.m_envs;
        for (int i = 0; i < NumModEnvs + Slots && *s; ++i) {
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
                    ModEnv& shape = i < NumModEnvs ? m_envPending[i] : m_srcEnvPending[i - NumModEnvs];
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
    const bool ok = m_matrixPending.parse(text);
    if (ok) publishModulation();
    unlockModulation();
    return ok;
}

bool Engine::setEnvShape(int index, const char* text)
{
    if (index < 0 || index >= NumModEnvs) return false;
    lockModulation();
    const bool ok = m_envPending[index].parse(text);
    if (ok) publishModulation();
    unlockModulation();
    return ok;
}

int Engine::writeEnvShape(int index, char* buf, size_t cap) const
{
    if (index < 0 || index >= NumModEnvs) return 0;
    return m_envPending[index].write(buf, cap);
}

bool Engine::setSrcEnvShape(int slot, const char* text)
{
    if (slot < 0 || slot >= Slots) return false;
    lockModulation();
    const bool ok = m_srcEnvPending[slot].parse(text);
    if (ok) publishModulation();
    unlockModulation();
    return ok;
}

int Engine::writeSrcEnvShape(int slot, char* buf, size_t cap) const
{
    if (slot < 0 || slot >= Slots) return 0;
    return m_srcEnvPending[slot].write(buf, cap);
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
    for (const auto& v : m_voices)
        if (v.isActive() && (f1 <= 0.0 || v.frequency() < f1)) f1 = v.frequency();
    if (f1 > 0.0)
        for (const auto& v : m_voices)
            if (v.isActive() && v.frequency() / f1 > 1.003 && (f2 <= 0.0 || v.frequency() < f2)) f2 = v.frequency();

    float want = 0.0f;
    if (f1 > 0.0 && f2 > 0.0) {
        // The simplest ratio near the interval they make. Small numbers only: those are the ones
        // whose harmonics are close enough together to beat audibly, which is the whole point.
        static const int Ratios[][2] = { {1,1}, {6,5}, {5,4}, {4,3}, {3,2}, {8,5}, {5,3}, {7,4}, {2,1}, {5,2}, {3,1}, {4,1} };
        const double r = f2 / f1;
        double bestErr = 1e30;
        int bp = 1, bq = 1;
        for (const auto& pq : Ratios) {
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
    m_beatHz += follow * (want - m_beatHz);
    // Turned into a modulation value. Below a fiftieth of a hertz the phase simply holds: a
    // chord that is in tune should leave whatever it is driving exactly where it is, not creep.
    if (m_beatHz > 0.02f) {
        m_beatPhase += TwoPi * m_beatHz * dt;
        while (m_beatPhase > TwoPi) m_beatPhase -= TwoPi;
    }
    return std::sin(m_beatPhase);
}

/**
 * @brief One step of every modulator, then the matrix summed into m_modOut.
 *
 * Called once per block, before
 * readParams, so the values the parameters are read with already carry the modulation.
 */
void Engine::stepModulation(float dt)
{
    // Pick up matrix or shape edits made on the message thread (fixed-size objects, no allocation).
    const int mv = m_modVersion.load(std::memory_order_acquire);
    // Tried, never waited for: the audio thread does not block on the message thread. A matrix
    // that is being edited right now simply arrives at the next block, a few milliseconds later,
    // which is sooner than a hand can move a mouse.
    if (mv != m_modSeen && !m_modLock.test_and_set(std::memory_order_acquire)) {
        m_matrix = m_matrixPending;
        for (int i = 0; i < NumModEnvs; ++i) m_envShape[i] = m_envPending[i];
        for (int k = 0; k < Slots; ++k) m_srcEnvShape[k] = m_srcEnvPending[k];
        m_modLock.clear(std::memory_order_release);
        m_modSeen = mv;
        // Whether anything reads the Lenia field, so it is only computed when it is heard.
        m_leniaUsed = false;
        m_chaosUsed = false;
        for (int i = 0; i < m_matrix.count(); ++i) {
            const ModRoute& r = m_matrix.route(i);
            auto inRange = [](ModSourceE s, ModSourceE lo, ModSourceE hi) { return static_cast<int>(s) >= static_cast<int>(lo) && static_cast<int>(s) <= static_cast<int>(hi); };
            if (inRange(r.m_source, ModSourceE::LENIA1, ModSourceE::LENIA4) || inRange(r.m_via, ModSourceE::LENIA1, ModSourceE::LENIA4)) m_leniaUsed = true;
            if (inRange(r.m_source, ModSourceE::LORENZ_X, ModSourceE::ROSSLER_Z) || inRange(r.m_via, ModSourceE::LORENZ_X, ModSourceE::ROSSLER_Z)) m_chaosUsed = true;
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
    // It reads the PREVIOUS block's m_modOut (it is cleared further down, after these lines), and
    // that is not a compromise, it is what a feedback path in a modulation matrix is: one control
    // block of delay -- under a millisecond and a half -- which is the only way lfo2 can move
    // lfo1's rate without the two needing each other's answer before either has one.
    //
    // Only the continuous settings are taken from it. A choice -- a shape, a mode, a table, a
    // sync division -- would have to jump from one value to the next, and nothing in this
    // instrument is allowed to jump.
    auto modulated = [this](ParamIdE id) {
        const float m = m_modOut[static_cast<int>(id)];
        const float v = effectiveParam(id);
        if (m == 0.0f) return v;
        const ParamDesc& d = paramDesc(id);
        return clampv(v + m, d.m_min, d.m_max);
    };

    for (int i = 0; i < NumLfos; ++i) {
        const int base = static_cast<int>(ParamIdE::LFO1_SHAPE) + i * 7;
        LfoSpec& sp = m_lfoSpec[i];
        sp.m_shape  = static_cast<LfoShapeE>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 0)))), 0, NumLfoShapes - 1));
        sp.m_rateHz = modulated(static_cast<ParamIdE>(base + 1));
        sp.m_phase  = modulated(static_cast<ParamIdE>(base + 2));
        sp.m_depth  = modulated(static_cast<ParamIdE>(base + 3));
        sp.m_mode   = static_cast<LfoModeE>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 4)))), 0, NumLfoModes - 1));
        sp.m_table  = static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 5))));
        const int sync = clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 6)))), 0, NumSyncDivs - 1);
        if (syncOn(sync)) {
            // One cycle per division, and the phase follows the clock: the step below lands
            // exactly on the beat's position, so a synced LFO stays on the grid however long
            // it runs and wherever the transport jumps.
            sp.m_rateHz = static_cast<float>(syncHz(sync, m_bpm));
            if (m_running) m_lfo[i].setPhase(static_cast<float>(syncPhase(sync, m_beat)) - sp.m_rateHz * dt);
        }
        // Retrigger: the phase goes back to where the knob says at the start of a phrase. Per Voice
        // cannot be honoured here and the help says so: this modulation is computed once a block
        // for the whole instrument, not once per voice, so there is no per-voice copy to give it.
        if (m_phraseStart && sp.m_mode == LfoModeE::RETRIGGER) m_lfo[i].setPhase(sp.m_phase);
        const Wavetable* table = m_userTable.m_frames > 0 ? &m_userTable : nullptr;
        m_modSrc[static_cast<int>(ModSourceE::LFO1) + i] = m_lfo[i].step(dt, sp, table);
    }
    m_phraseStart = false;

    const bool wasHeld = m_envHeld;
    m_envHeld = false;
    for (const auto& v : m_voices) if (v.isActive() && !v.isReleasing()) { m_envHeld = true; break; }
    for (int i = 0; i < NumModEnvs; ++i) {
        m_envTime[i] += dt;
        const int base = static_cast<int>(ParamIdE::ENV1_MODE) + i * 4;
        ModEnvSpec& sp = m_envSpec[i];
        sp.m_mode      = static_cast<EnvModeE>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 0)))), 0, NumEnvModes - 1));
        sp.m_timeScale = std::max(0.01f, modulated(static_cast<ParamIdE>(base + 1)));
        const int sync = clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 3)))), 0, NumSyncDivs - 1);
        if (syncOn(sync))   // synced: the whole shape spans one division
            sp.m_timeScale = static_cast<float>(std::max(0.01, syncSeconds(sync, m_bpm) / std::max(static_cast<double>(m_envShape[i].length()), 1e-3)));
        sp.m_depth     = modulated(static_cast<ParamIdE>(base + 2));
        // Sustain Loop, at the moment the last voice lets go: put the clock where the shape was
        // being read -- the sustain point, or wherever in its loop it had got to -- so the tail
        // plays from where the held part ended instead of from wherever the clock had got to.
        // Without this the value jumps on release: the shape holds while the note is down, and
        // the release finds the clock long past it. It used to be put on the sustain point
        // always, which still jumped for a note let go before reaching it and for a shape held
        // inside its loop. Only this mode is touched.
        if (wasHeld && !m_envHeld && sp.m_mode == EnvModeE::SUSTAIN_LOOP)
            m_envTime[i] = static_cast<double>(m_envShape[i].readTime(static_cast<float>(m_envTime[i]) / sp.m_timeScale, sp.m_mode, true)) * sp.m_timeScale;
        const float t = static_cast<float>(m_envTime[i]) / sp.m_timeScale;
        m_modSrc[static_cast<int>(ModSourceE::ENV1) + i] = m_envShape[i].at(t, sp.m_mode, m_envHeld) * sp.m_depth;
    }
    // Each source's own envelope keeps only its settings here: its clock is every voice's own,
    // from its note (Voice.cpp), so there is no single value of it to hand to the matrix.
    for (int k = 0; k < Slots; ++k) {
        const int base = static_cast<int>(ParamIdE::SRC1_ENV_MODE) + k * 4;
        ModEnvSpec& sp = m_srcEnvSpec[k];
        sp.m_mode      = static_cast<EnvModeE>(clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 0)))), 0, NumEnvModes - 1));
        sp.m_timeScale = std::max(0.01f, modulated(static_cast<ParamIdE>(base + 1)));
        const int sync = clampv(static_cast<int>(std::lround(getParam(static_cast<ParamIdE>(base + 3)))), 0, NumSyncDivs - 1);
        if (syncOn(sync))   // synced: the whole shape spans one division
            sp.m_timeScale = static_cast<float>(std::max(0.01, syncSeconds(sync, m_bpm) / std::max(static_cast<double>(m_srcEnvShape[k].length()), 1e-3)));
        sp.m_depth     = modulated(static_cast<ParamIdE>(base + 2));
    }

    // Everything else that can drive something. Sources are bipolar; the ones that are naturally
    // 0..1 are mapped to -1..1 here, and a route's "unipolar" flag maps them back.
    const Voice* loud = loudestVoice();
    auto uni = [](float v) { return 2.0f * clampv(v, 0.0f, 1.0f) - 1.0f; };
    m_modSrc[static_cast<int>(ModSourceE::AMP)] = uni(loud ? loud->level() : 0.0f);
    for (int m = 0; m < 8; ++m)
        m_modSrc[static_cast<int>(ModSourceE::MACRO_A) + m] = uni(getParam(static_cast<ParamIdE>(static_cast<int>(ParamIdE::MACRO_A) + m)));
    for (int k = 0; k < 4; ++k)
        m_modSrc[static_cast<int>(ModSourceE::KURA1) + k] = std::sin(m_kuraPhase[k]);
    if (m_leniaUsed) stepLenia(dt);
    for (int k = 0; k < 4; ++k)
        m_modSrc[static_cast<int>(ModSourceE::LENIA1) + k] = uni(m_leniaOut[k]);
    if (m_chaosUsed) stepChaos(dt);
    for (int k = 0; k < 6; ++k)
        m_modSrc[static_cast<int>(ModSourceE::LORENZ_X) + k] = m_chaosOut[k];
    m_modSrc[static_cast<int>(ModSourceE::NOTE)] = uni(loud ? (loud->note() - 24) / 84.0f : 0.5f);
    m_modSrc[static_cast<int>(ModSourceE::VELOCITY)] = uni(loud ? loud->level() : 0.0f);
    m_modSrc[static_cast<int>(ModSourceE::DISTANCE)] = uni(loud ? loud->distance() : 0.5f);
    m_modSrc[static_cast<int>(ModSourceE::RANDOM_PER_NOTE)] = m_randomPerNote;
    m_modSrc[static_cast<int>(ModSourceE::BEAT)] = updateBeat(dt);
    // Where the piece stands in itself. Root Age is measured against Home Time, so it reads the
    // same whether the night is an hour or three; Layer is the role of the voice being read, which
    // for a per-voice source is the loudest one; Section is the arc as a staircase of five, which
    // is what a supply or a register needs when it should change rather than glide.
    m_modSrc[static_cast<int>(ModSourceE::ROOT_AGE)] =
        uni(static_cast<float>(clampv(m_brain.rootAgeSeconds() / std::max(60.0, static_cast<double>(m_bp.m_homeTime) * 60.0), 0.0, 1.0)));
    m_modSrc[static_cast<int>(ModSourceE::LAYER)] = uni(loud ? loud->layer() : 0.5f);
    m_modSrc[static_cast<int>(ModSourceE::SECTION)] =
        uni(std::floor(clampv(0.5f + 0.5f * m_arcOut, 0.0f, 0.9999f) * 5.0f) * 0.25f);
    // The cascade's excitation, smoothed: the Hawkes kick is a step, and a step on a send is a
    // click. One pole at two seconds turns it into a swell that still rises with the cluster.
    {
        const float target = static_cast<float>(m_brain.excitation() / (1.0 + m_brain.excitation()));
        const float a = 1.0f - std::exp(-dt / 2.0f);
        m_cascadeNow += a * (target - m_cascadeNow);
        // Its own slow average, so anything reading it can ask "busier than usual?" rather than
        // "busy?" -- what keeps a piece without a cascade exactly where it was.
        m_cascadeAvg += (1.0f - std::exp(-dt / 90.0f)) * (m_cascadeNow - m_cascadeAvg);
        m_modSrc[static_cast<int>(ModSourceE::CASCADE)] = uni(m_cascadeNow);
    }
    // The hands. Pressure and slide are per note and read from the loudest voice -- it is the one
    // being leaned on; the wheel belongs to the instrument. All three rest at zero, which through
    // uni() is -1, so a route wanting "nothing until I move it" carries the 0..1 flag.
    m_modSrc[static_cast<int>(ModSourceE::PRESSURE)] = uni(loud ? loud->pressure() : 0.0f);
    m_modSrc[static_cast<int>(ModSourceE::SLIDE)]    = uni(loud ? loud->slide() : 0.0f);
    m_wheel += (1.0f - std::exp(-dt / 0.03f)) * (m_wheelTarget - m_wheel);   // 30 ms: no step from a 7-bit controller
    m_modSrc[static_cast<int>(ModSourceE::WHEEL)]    = uni(m_wheel);
    m_modSrc[static_cast<int>(ModSourceE::NONE)] = 0.0f;

    std::memset(m_modOut, 0, sizeof(m_modOut));
    m_matrix.apply(m_modSrc, m_modOut);
    // Performance state is never a target: modulating the morph position or the map cursor from
    // inside would fight the hand that is holding them.
    for (const ParamDesc& d : paramTable())
        if (isPerformanceParam(d.m_id)) m_modOut[static_cast<int>(d.m_id)] = 0.0f;
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
    constexpr int S = LeniaSize, R = LeniaRadius, K = 2 * R + 1;
    auto bell = [](float x, float m, float s) { const float d = (x - m) / s; return std::exp(-0.5f * d * d); };
    if (!m_leniaInit) {
        m_leniaInit = true;
        m_leniaRng.seed(0x1E51Aull);
        float sum = 0.0f;
        for (int dy = -R; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                const float r = std::sqrt(static_cast<float>(dx * dx + dy * dy)) / static_cast<float>(R);
                const float k = r <= 1.0f ? bell(r, 0.5f, 0.15f) : 0.0f;
                m_leniaKernel[(dy + R) * K + (dx + R)] = k;
                sum += k;
            }
        for (float& k : m_leniaKernel) k /= sum;
        seedLenia();
    }
    // How many rows this block: one full field per 1/rate seconds, the remainder carried over.
    m_leniaRowAcc += static_cast<float>(S) * dt * std::max(m_leniaRate, 0.01f);
    int rows = static_cast<int>(m_leniaRowAcc);
    m_leniaRowAcc -= static_cast<float>(rows);
    rows = clampv(rows, 0, S);
    const float sigma = 0.02f;
    for (int pass = 0; pass < rows; ++pass) {
        const int y = m_leniaRow;
        for (int x = 0; x < S; ++x) {
            float u = 0.0f;
            for (int dy = -R; dy <= R; ++dy) {
                const float* row = &m_lenia[((y + dy + S) % S) * S];
                const float* krow = &m_leniaKernel[(dy + R) * K];
                for (int dx = -R; dx <= R; ++dx) u += krow[dx + R] * row[(x + dx + S) % S];
            }
            const float g = 2.0f * bell(u, m_leniaMu, sigma) - 1.0f;
            m_leniaNext[y * S + x] = clampv(m_lenia[y * S + x] + 0.1f * g, 0.0f, 1.0f);
        }
        if (++m_leniaRow >= S) {
            m_leniaRow = 0;
            std::memcpy(m_lenia, m_leniaNext, sizeof(m_lenia));
            ++m_leniaSteps;
            float mass = 0.0f;
            for (float v : m_lenia) mass += v;
            if (mass < 3.0f || mass > 0.85f * static_cast<float>(S * S)) { seedLenia(); ++m_leniaReseeds; }
            static const int px[4] = { 8, 24, 8, 24 }, py[4] = { 8, 8, 24, 24 };
            for (int k = 0; k < 4; ++k) {
                float s = 0.0f;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) s += m_lenia[((py[k] + dy) % S) * S + (px[k] + dx) % S];
                m_leniaTarget[k] = s / 9.0f;
            }
        }
    }
    const float c = 1.0f - std::exp(-dt * std::max(m_leniaRate, 0.01f));
    for (int k = 0; k < 4; ++k) m_leniaOut[k] += c * (m_leniaTarget[k] - m_leniaOut[k]);
}

// ---------------------------------------------------------------- the attractors

/**
 * @brief One block of the two attractors, integrated in their own time and read into m_chaosOut as
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
    const double period = std::max(static_cast<double>(m_chaosPeriod), 1.0);
    const double hL = static_cast<double>(dt) * 1.5 / period;    // lobe changes a few natural units apart
    const double hR = static_cast<double>(dt) * 6.0 / period;    // one turn of the spiral is about six
    for (double left = hL; left > 0.0; left -= 0.01) rk4(m_lorenz, std::min(left, 0.01), lorenz);
    for (double left = hR; left > 0.0; left -= 0.01) rk4(m_rossler, std::min(left, 0.01), rossler);
    // An attractor that has left the finite world never comes back on its own, and clampv
    // does not stop it: every comparison against a NaN is false, so clampv hands the NaN
    // straight on -- into the matrix, into filters and gains, and the instrument is silent
    // until the plugin is reloaded. If either state is no longer finite it is seeded again.
    auto finite3 = [](const double* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); };
    if (!finite3(m_lorenz))  { m_lorenz[0] = 0.1; m_lorenz[1] = 0.0; m_lorenz[2] = 20.0; }
    if (!finite3(m_rossler)) { m_rossler[0] = 1.0; m_rossler[1] = 1.0; m_rossler[2] = 1.0; }
    m_chaosOut[0] = clampv(static_cast<float>(m_lorenz[0] / 20.0), -1.0f, 1.0f);
    m_chaosOut[1] = clampv(static_cast<float>(m_lorenz[1] / 27.0), -1.0f, 1.0f);
    m_chaosOut[2] = clampv(static_cast<float>((m_lorenz[2] - 25.0) / 22.0), -1.0f, 1.0f);
    m_chaosOut[3] = clampv(static_cast<float>(m_rossler[0] / 12.0), -1.0f, 1.0f);
    m_chaosOut[4] = clampv(static_cast<float>(m_rossler[1] / 12.0), -1.0f, 1.0f);
    m_chaosOut[5] = clampv(static_cast<float>((m_rossler[2] - 8.0) / 12.0), -1.0f, 1.0f);
    ++m_chaosSteps;
}

/** @brief A few soft blobs on an empty torus. From the field's own random stream, so the sound's is not touched. */
void Engine::seedLenia()
{
    constexpr int S = LeniaSize;
    for (float& v : m_lenia) v = 0.0f;
    for (int b = 0; b < 3; ++b) {
        const float cx = m_leniaRng.uniform() * S, cy = m_leniaRng.uniform() * S;
        const float rad = 2.5f + 2.5f * m_leniaRng.uniform();
        const float peak = 0.6f + 0.4f * m_leniaRng.uniform();
        for (int y = 0; y < S; ++y)
            for (int x = 0; x < S; ++x) {
                float dx = std::fabs(static_cast<float>(x) - cx), dy = std::fabs(static_cast<float>(y) - cy);
                dx = std::min(dx, static_cast<float>(S) - dx); dy = std::min(dy, static_cast<float>(S) - dy);
                m_lenia[y * S + x] = std::min(1.0f, m_lenia[y * S + x] + peak * std::exp(-(dx * dx + dy * dy) / (2.0f * rad * rad)));
            }
    }
    std::memcpy(m_leniaNext, m_lenia, sizeof(m_lenia));
    m_leniaRow = 0;
}

// ---------------------------------------------------------------- clock

void Engine::stepClock(double dt)
{
    const int src = clampv(static_cast<int>(std::lround(getParam(ParamIdE::CLOCK_SOURCE))), 0, NumClockSources - 1);
    const double internal = clampv(getParam(ParamIdE::TEMPO), 20.0f, 300.0f);
    const bool run = getParam(ParamIdE::CLOCK_RUN) >= 0.5f;
    m_midiSilence += dt;
    const bool useHost = src == static_cast<int>(ClockSourceE::HOST) && m_hostSeen && m_hostBpm > 1.0;
    const bool useMidi = src == static_cast<int>(ClockSourceE::MIDI) && m_midiTicks >= 24 && m_midiBpm > 1.0 && m_midiSilence < 2.0;
    if (useHost)      { m_bpm = m_hostBpm; m_beat = m_hostBeat; m_running = m_hostPlaying; }
    else if (useMidi) { m_bpm = m_midiBpm; m_beat = m_midiBeat; m_running = m_midiRunning; }
    else {
        // The internal clock: the Tempo knob, counting beats while Run is on. Also what Host and
        // MIDI fall back to when nothing arrives -- the standalone has no play head.
        m_bpm = internal;
        m_running = run;
        if (run) m_intBeat += m_bpm / 60.0 * dt;
        m_beat = m_intBeat;
    }
    m_tempoOut.store(m_bpm, std::memory_order_relaxed);
    m_beatOut.store(m_beat, std::memory_order_relaxed);
    m_runningOut.store(m_running, std::memory_order_relaxed);
}

void Engine::midiClockTick(double interval)
{
    // 24 ticks a quarter. The tempo settles over a beat's worth of ticks, so jitter in the
    // interface does not wobble every synced LFO.
    if (interval > 0.002 && interval < 2.0) {
        const double bpm = 60.0 / (24.0 * interval);
        m_midiBpm = m_midiTicks < 24 || m_midiBpm <= 1.0 ? bpm : m_midiBpm + (bpm - m_midiBpm) * 0.08;
    }
    if (m_midiRunning) m_midiBeat += 1.0 / 24.0;
    ++m_midiTicks;
    m_midiSilence = 0.0;
}

void Engine::midiClockStart()    { m_midiBeat = 0.0; m_midiRunning = true; }
void Engine::midiClockContinue() { m_midiRunning = true; }
void Engine::midiClockStop()     { m_midiRunning = false; }

void Engine::readParams()
{
    // Inertia: every float parameter glides to its effective value with the Inertia time
    // constant (skew domain), the analogue slew that keeps even a torn-open knob slow.
    const float inertia = getParam(ParamIdE::INERTIA);
    const float inertiaCoef = inertia > 0.005f ? 1.0f - std::exp(-m_lastBlockSeconds / inertia) : 1.0f;
    auto g = [this, inertiaCoef](ParamIdE id) {
        const float target = effectiveParam(id);
        const int i = static_cast<int>(id);
        const ParamDesc& d = paramDesc(id);
        // Modulation is added after the inertia glide: a modulator moves at its own rate, it is
        // not slewed by the setting that exists to slow down the performer's hand.
        const float mod = m_modOut[i];
        if (inertiaCoef >= 1.0f || d.m_kind != ParamKindE::FLOAT || isPerformanceParam(id)) {
            m_inertiaCur[i] = target;
            return mod != 0.0f ? clampv(target + mod, d.m_min, d.m_max) : target;
        }
        const float span = std::max(d.m_max - d.m_min, 1e-9f);
        // A knob that is not moving is not glided. The three powers below were computed for every
        // float parameter of every block whether or not anything had changed -- and a panel that
        // is being listened to rather than turned is the normal case. The threshold also lets the
        // glide arrive: without it the value approaches its target for ever and never reaches it.
        if (std::fabs(target - m_inertiaCur[i]) <= 1.0e-7f * span) {
            m_inertiaCur[i] = target;
            return mod != 0.0f ? clampv(target + mod, d.m_min, d.m_max) : target;
        }
        const float pc = std::pow(clampv((m_inertiaCur[i] - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
        const float pt = std::pow(clampv((target - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
        m_inertiaCur[i] = d.m_min + span * std::pow(pc + (pt - pc) * inertiaCoef, 1.0f / d.m_skew);
        return mod != 0.0f ? clampv(m_inertiaCur[i] + mod, d.m_min, d.m_max) : m_inertiaCur[i];
    };
    m_vp.m_level       = g(ParamIdE::OSC_LEVEL);
    m_vp.m_partials    = static_cast<int>(std::lround(g(ParamIdE::PARTIALS)));
    m_vp.m_tilt        = g(ParamIdE::TILT);
    m_vp.m_brightness  = g(ParamIdE::BRIGHTNESS);
    m_vp.m_oddEven     = g(ParamIdE::ODD_EVEN);
    m_vp.m_inharmonic  = g(ParamIdE::INHARMONIC);
    // Match: the partials placed on the current scale instead of on the harmonic series. Sethares'
    // other direction -- the timbre scale takes a spectrum and finds its scale; this takes a scale
    // and bends the spectrum until that scale is the smooth one. Milne, Sethares and Plamondon
    // (2009) call the pair dynamic tonality. Computed here once per block for the voice and for
    // the conductor's ear alike, so both hear the same partials.
    m_vp.m_match = g(ParamIdE::TUNE_MATCH);
    // Thirty-two partials, each two logarithms and an exponential, plus a walk over the
    // scale's degrees inside matchedPartialRatio -- a hundred transcendentals, recomputed on
    // every block for an answer that changes only when the knob, the spectrum's stiffness or
    // the scale changes. None of those move at audio rate.
    if (m_vp.m_match > 0.0f) {
        const float B = m_vp.m_inharmonic * m_vp.m_inharmonic * 0.02f;
        const double m = static_cast<double>(m_vp.m_match);
        const int scaleId = static_cast<int>(std::lround(getParam(ParamIdE::SCALE)));
        const bool same = m_matchHave && m_vp.m_match == m_matchLast && B == m_matchB
                       && scaleId == m_matchScale && m_rootNote == m_matchRoot;
        if (same) {
            for (int h = 0; h < MaxPartials; ++h) m_vp.m_partialRatio[h] = m_matchRatio[h];
        } else {
        m_matchHave = true; m_matchLast = m_vp.m_match; m_matchB = B; m_matchScale = scaleId; m_matchRoot = m_rootNote;
        for (int h = 1; h <= MaxPartials; ++h) {
            const double natural = h * (B > 0.0f ? std::sqrt(1.0 + B * static_cast<double>(h * h)) : 1.0);
            const double matched = matchedPartialRatio(h);
            m_vp.m_partialRatio[h - 1] = static_cast<float>(std::exp(std::log(natural) + m * (std::log(matched) - std::log(natural))));
            m_matchRatio[h - 1] = m_vp.m_partialRatio[h - 1];
        }
        }
    }
    m_vp.m_shimmer     = g(ParamIdE::SHIMMER);
    m_vp.m_shimmerRate = g(ParamIdE::SHIMMER_RATE);
    m_vp.m_unison      = static_cast<int>(std::lround(g(ParamIdE::UNISON)));
    m_vp.m_detune      = g(ParamIdE::DETUNE);
    m_vp.m_drift       = g(ParamIdE::DRIFT);
    m_vp.m_lowDetune   = g(ParamIdE::STRAND_LOW_DETUNE);
    m_vp.m_beatCeiling = g(ParamIdE::BEAT_CEILING);
    m_vp.m_velAttack   = g(ParamIdE::ENV_VEL_ATTACK);
    m_layerDepth     = g(ParamIdE::LAYER_DEPTH);
    m_vp.m_driftRate   = g(ParamIdE::DRIFT_RATE);
    m_vp.m_spread      = g(ParamIdE::SPREAD);
    m_vp.m_bloom       = g(ParamIdE::BLOOM);
    m_vp.m_bloomTime   = g(ParamIdE::BLOOM_TIME);
    m_vp.m_stack       = static_cast<int>(std::lround(g(ParamIdE::STACK)));
    m_vp.m_rateWander  = g(ParamIdE::RATE_WANDER);
    {   // Source slots: 24 fields each. Source 2 and 3 are laid out consecutively from their Type;
        // Source 1's fields are scattered (its level and spectrum are the classic Oscillator
        // parameters, read above), so every slot goes through one table of ids.
        // The ids come from Params.h; they used to be written out here and again in the editor.
        for (int k = 0; k < Slots; ++k) {
            // Source 1's level and spectrum were read into m_vp above; reading them again would step
            // their inertia twice per block, so they are copied instead.
            const ParamIdE* ids = slotParamIds(k);
            auto at = [&](int off) { return g(ids[off]); };
            SlotParams& s = m_vp.m_slot[k];
            if (k == 0) {
                s.m_level = m_vp.m_level; s.m_partials = m_vp.m_partials; s.m_tilt = m_vp.m_tilt; s.m_bright = m_vp.m_brightness;
                s.m_oddEven = m_vp.m_oddEven; s.m_inharm = m_vp.m_inharmonic; s.m_shimmer = m_vp.m_shimmer; s.m_shimmerRate = m_vp.m_shimmerRate;
            } else {
                s.m_level       = at(1);
                s.m_partials    = static_cast<int>(std::lround(at(17)));
                s.m_tilt        = at(18);
                s.m_bright      = at(19);
                s.m_oddEven     = at(20);
                s.m_inharm      = at(21);
                s.m_shimmer     = at(22);
                s.m_shimmerRate = at(23);
            }
            s.m_type          = static_cast<SourceTypeE>(clampv(static_cast<int>(std::lround(at(0))), 0, NumSourceTypes - 1));
            s.m_drift         = at(25);
            s.m_octave        = static_cast<int>(std::lround(at(2)));
            s.m_ratio         = static_cast<int>(std::lround(at(3)));
            s.m_pan           = at(4);
            s.m_table         = static_cast<int>(std::lround(at(5)));
            s.m_position      = at(6);
            s.m_positionDrift = at(7);
            s.m_fmRatio       = at(8);
            s.m_fmIndex       = at(9);
            s.m_grainMs       = at(10);
            s.m_density       = at(11);
            {   // Sync: a grain (or a crackle) per division instead of per second
                const int dsync = clampv(static_cast<int>(std::lround(getParam(ids[24]))), 0, NumSyncDivs - 1);
                if (syncOn(dsync)) s.m_density = static_cast<float>(syncHz(dsync, m_bpm));
            }
            s.m_follow        = at(12) >= 0.5f;
            s.m_grains        = static_cast<int>(std::lround(at(13)));
            s.m_spread        = at(14);
            s.m_noise         = static_cast<NoiseKindE>(clampv(static_cast<int>(std::lround(at(15))), 0, NumNoiseKinds - 1));
            s.m_noiseQ        = at(16);
            s.m_stretch       = at(26);
            s.m_xfade         = at(27);
            s.m_bowForce      = at(28);
            s.m_bowSpeed      = at(29);
            s.m_specRate      = at(30);
            s.m_specBreath    = at(31);
            s.m_transport     = at(32);
            s.m_interp        = static_cast<int>(std::lround(at(36)));
            s.m_unison        = static_cast<int>(std::lround(at(37)));
            s.m_uniDetune     = at(38);
            s.m_uniWidth      = at(39);
            s.m_root          = at(40);
            s.m_delaySec      = at(33);
            s.m_riseSec       = at(34);
            s.m_role          = static_cast<SlotRoleE>(clampv(static_cast<int>(std::lround(at(41))), 0, NumSlotRoles - 1));
            {   // The choice is "Off" first, then the six shapes -- one off the shape's index --
                // and "Own" last, the slot's own shape.
                const int env = static_cast<int>(std::lround(at(35)));
                s.m_envIndex = (env >= 1 && env <= NumModEnvs) ? env - 1 : -1;
                s.m_ownEnv   = env == NumModEnvs + 1;
            }
        }
        // The shapes a slot may borrow for its entrance. Pointers into the engine, valid for the
        // block like every other pointer in VoiceParams.
        for (int i = 0; i < NumModEnvs; ++i) { m_vp.m_envShape[i] = &m_envShape[i]; m_vp.m_envSpec[i] = &m_envSpec[i]; }
        for (int k = 0; k < Slots; ++k) { m_vp.m_srcEnvShape[k] = &m_srcEnvShape[k]; m_vp.m_srcEnvSpec[k] = &m_srcEnvSpec[k]; }
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
            const float amount = g(ParamIdE::VEC_AMOUNT);
            if (amount > 0.0005f) {
                const float wander = g(ParamIdE::VEC_WANDER);
                float vx = g(ParamIdE::VEC_X), vy = g(ParamIdE::VEC_Y);
                if (wander > 0.0005f) {
                    // Two drifts whose rates share no simple ratio, so the point never traces the
                    // same path twice -- the same reason the LFOs sit on a golden ladder.
                    const float rate = g(ParamIdE::VEC_RATE);
                    vx += 0.5f * wander * m_vecDriftX.update(m_lastBlockSeconds, rate, m_auxRng);
                    vy += 0.5f * wander * m_vecDriftY.update(m_lastBlockSeconds, rate * 1.6180339887f, m_auxRng);
                }
                vx = clampv(vx, 0.0f, 1.0f);
                vy = clampv(vy, 0.0f, 1.0f);
                const float f[Slots] = { (1.0f - vx) * (1.0f - vy),   // Source 1, bottom left
                                          vx * (1.0f - vy),            // Source 2, bottom right
                                          (1.0f - vx) * vy,            // Source 3, top left
                                          vx * vy };                   // Source 4, top right
                for (int k = 0; k < Slots; ++k) {
                    const float factor = 1.0f + amount * (4.0f * f[k] - 1.0f);
                    m_vp.m_slot[k].m_level = clampv(m_vp.m_slot[k].m_level * factor, 0.0f, 1.0f);
                }
                m_vp.m_level = m_vp.m_slot[0].m_level;
            }
        }
        m_vp.m_userTable = m_userTable.m_frames > 0 ? &m_userTable : nullptr;
        {
            const int a = m_cyclesActive.load(std::memory_order_acquire);
            m_vp.m_userCycles = (a >= 0 && !m_userCycles[a].empty()) ? &m_userCycles[a] : nullptr;
        }
        for (int k = 0; k < Slots; ++k) {
            const int a = m_textureActive[k].load(std::memory_order_acquire);
            m_vp.m_texture[k] = (a >= 0 && !m_textures[k][a].empty()) ? &m_textures[k][a] : nullptr;
        }
    }
    m_masterGain     = g(ParamIdE::MASTER_GAIN);   // through g(), so the matrix can reach it
    m_subLevel       = g(ParamIdE::SUB_LEVEL);
    m_subOctave      = std::lround(g(ParamIdE::SUB_OCTAVE)) == 0 ? 1 : 2;
    m_subGlide       = g(ParamIdE::SUB_GLIDE);
    m_subBinaural    = g(ParamIdE::SUB_BINAURAL);
    m_subPulse       = g(ParamIdE::SUB_PULSE);
    m_subTone        = g(ParamIdE::SUB_TONE);
    m_subHarmonics   = g(ParamIdE::SUB_HARMONICS);
    m_subBeat        = g(ParamIdE::SUB_BEAT);
    m_subSource      = clampv(static_cast<int>(std::lround(g(ParamIdE::SUB_SOURCE))), 0, 2);
    m_vp.m_lowCut      = g(ParamIdE::PAD_LOW_CUT);
    const bool hold = g(ParamIdE::HOLD) >= 0.5f;
    if (m_hold && !hold) { for (int i = 0; i < 128; ++i) if (m_midiHeld[i]) { m_midiHeld[i] = false; stopNote(i, OWNER_MIDI); } }
    m_hold = hold;
    m_vp.m_air         = g(ParamIdE::AIR);
    m_vp.m_airColor    = g(ParamIdE::AIR_COLOR);
    m_vp.m_airQ        = g(ParamIdE::AIR_Q);
    m_vp.m_attack      = g(ParamIdE::ATTACK);
    m_vp.m_decay       = g(ParamIdE::DECAY);
    m_vp.m_sustain     = g(ParamIdE::SUSTAIN);
    m_vp.m_release     = g(ParamIdE::RELEASE);
    m_vp.m_filterOn    = g(ParamIdE::FILTER_ON) >= 0.5f;
    m_vp.m_filterParallel = std::lround(g(ParamIdE::Z_ROUTE)) == 1;
    m_vp.m_filterModel = static_cast<int>(std::lround(g(ParamIdE::FILTER_MODEL)));
    m_vp.m_filterDrive = g(ParamIdE::FILTER_DRIVE);
    m_vp.m_filterMorph = g(ParamIdE::FILTER_MORPH);
    m_vp.m_fold = g(ParamIdE::FILTER_FOLD);
    m_vp.m_binaural = std::lround(getParam(ParamIdE::BINAURAL)) == 1;
    m_vp.m_headYawDeg = m_headYawDeg.load(std::memory_order_relaxed);
    // The family jam (02.10.2026): the leader's energy on the voices' filters -- half the cutoff at an energy of 0.3, as
    // written at full energy --, gliding block by block.
    {
        const float target = m_jamEnergy >= 0.0f ? std::exp2((std::clamp(m_jamEnergy, 0.0f, 1.0f) - 1.0f) * 1.5f) : 1.0f;
        m_jamCutoff += (target - m_jamCutoff) * 0.05f;
    }
    m_vp.m_cutoff      = g(ParamIdE::CUTOFF) * m_jamCutoff;
    m_vp.m_resonance   = g(ParamIdE::RESONANCE);
    m_vp.m_filterEnv   = g(ParamIdE::FILTER_ENV);
    m_vp.m_filterDrift = g(ParamIdE::FILTER_DRIFT);
    m_vp.m_keyTrack    = g(ParamIdE::KEY_TRACK);
    m_vp.m_zMode       = static_cast<int>(std::lround(g(ParamIdE::Z_MODE)));
    m_vp.m_zShape      = static_cast<int>(std::lround(g(ParamIdE::Z_SHAPE)));
    m_vp.m_zX          = g(ParamIdE::ZX);
    m_vp.m_zY          = g(ParamIdE::ZY);
    m_vp.m_zZ          = g(ParamIdE::ZZ);
    m_vp.m_zDecay      = g(ParamIdE::Z_DECAY);
    m_vp.m_zDamp       = g(ParamIdE::Z_DAMP);
    m_vp.m_zRate       = g(ParamIdE::Z_RATE);
    m_vp.m_zDepth      = g(ParamIdE::Z_DEPTH);
    m_vp.m_zRes        = g(ParamIdE::Z_RESONANCE);
    m_vp.m_zKeyTrack   = g(ParamIdE::Z_KEY_TRACK);
    m_vp.m_zMix        = g(ParamIdE::Z_MIX);
    m_vp.m_panDrift    = g(ParamIdE::PAN_DRIFT);
    m_vp.m_itd         = g(ParamIdE::ITD);
    m_vp.m_presence    = g(ParamIdE::PRESENCE);
    m_vp.m_depthRange    = g(ParamIdE::DEPTH_RANGE);      // the guide's depth model (25.09.2026): level, gap and width on the one distance
    m_vp.m_depthPreDelay = g(ParamIdE::DEPTH_PRE_DELAY);
    m_vp.m_depthWidth    = g(ParamIdE::DEPTH_WIDTH);
    m_vp.m_breath      = g(ParamIdE::BREATH);
    m_vp.m_breathRate  = g(ParamIdE::BREATH_RATE);
    m_vp.m_phaseWidth  = g(ParamIdE::PHASE_WIDTH);
    m_vp.m_phaseRate   = g(ParamIdE::PHASE_RATE);
    m_vp.m_doppler     = g(ParamIdE::DOPPLER);
    m_vp.m_externalise = g(ParamIdE::EXTERNALISE);
    m_vp.m_elevNear = g(ParamIdE::ELEV_NEAR);
    m_vp.m_elevFar = g(ParamIdE::ELEV_FAR);
    m_vp.m_depthLaw = g(ParamIdE::DEPTH_LAW);
    m_vp.m_nearIld = g(ParamIdE::NEAR_ILD);
    m_vp.m_oneEuro = std::lround(g(ParamIdE::KEYS_FILTER)) == 1;
    m_vp.m_partialSpread = g(ParamIdE::PARTIAL_SPREAD);
    m_vp.m_strikeLevel = g(ParamIdE::STRIKE_LEVEL);
    m_vp.m_strikeType  = static_cast<int>(std::lround(g(ParamIdE::STRIKE_TYPE)));
    m_vp.m_strikeDecay = g(ParamIdE::STRIKE_DECAY);
    m_vp.m_strikeDamp  = g(ParamIdE::STRIKE_DAMP);
    m_vp.m_strikeBrain = std::lround(g(ParamIdE::STRIKE_WHO)) == 1;
    m_strikeChance   = g(ParamIdE::STRIKE_CHANCE);
    m_strikeCluster  = g(ParamIdE::STRIKE_CLUSTER);
    m_cosmosSwell    = g(ParamIdE::COSMOS_SWELL);
    m_tide       = g(ParamIdE::TIDE);
    m_tidePeriod = g(ParamIdE::TIDE_PERIOD);
    m_vp.m_pitchMul = m_tide > 0.0f ? std::pow(2.0f, m_tide * m_tideDrift.value() / 1200.0f) : 1.0f;
    m_blurMix    = g(ParamIdE::BLUR_MIX);
    m_blur.set(g(ParamIdE::BLUR_SMEAR));
    m_farRotate  = g(ParamIdE::FAR_ROTATE);
    m_farWidth   = g(ParamIdE::FAR_WIDTH);
    // Envelopment: the far bus's side channel lifted in the band that carries it. The band
    // starts where Bass Mono ends, because everything under that corner is folded to mono at
    // the master and lifting it here would be lifting something the guard then removes.
    m_envelop = g(ParamIdE::ENVELOP);
    m_comod = g(ParamIdE::FAR_COMOD);
    m_leniaRate = g(ParamIdE::LENIA_RATE);
    m_leniaMu = g(ParamIdE::LENIA_GROWTH);
    m_chaosPeriod = g(ParamIdE::CHAOS_PERIOD);
    {
        // A bell between the two corners, centred on their geometric mean, as wide as they are
        // apart (Q from the bandwidth in octaves). Coefficients only: the filter's state stays.
        const float lo = std::max(125.0f, getParam(ParamIdE::BASS_MONO));
        const float ratio = 500.0f / lo;
        m_envBell.setQ(std::sqrt(lo * 500.0f), std::sqrt(ratio) / std::max(ratio - 1.0f, 0.05f), static_cast<float>(m_sr));
    }
    m_farReverb.setMode(clampv(static_cast<int>(std::lround(getParam(ParamIdE::FAR_MODE))), 0, 3));

    m_depth       = g(ParamIdE::DEPTH);
    m_keysDepth   = g(ParamIdE::KEYS_DEPTH);
    m_arcAmount   = g(ParamIdE::ARC_AMOUNT);
    m_arcClock    = g(ParamIdE::ARC_CLOCK) >= 0.5f;
    m_arcPeriodMin = g(ParamIdE::ARC_PERIOD);
    // Sync choices: when set, the division at the current tempo replaces the free knob.
    auto syncedSeconds = [this](ParamIdE sync, float free) {
        const int d = clampv(static_cast<int>(std::lround(getParam(sync))), 0, NumSyncDivs - 1);
        return syncOn(d) ? static_cast<float>(syncSeconds(d, m_bpm)) : free;
    };
    auto syncedHz = [this](ParamIdE sync, float free) {
        const int d = clampv(static_cast<int>(std::lround(getParam(sync))), 0, NumSyncDivs - 1);
        return syncOn(d) ? static_cast<float>(syncHz(d, m_bpm)) : free;
    };
    m_arcPeriodMin = syncedSeconds(ParamIdE::ARC_SYNC, m_arcPeriodMin * 60.0f) / 60.0f;

    m_bp.m_on          = g(ParamIdE::BRAIN_ON) >= 0.5f;
    m_bp.m_density     = static_cast<int>(std::lround(g(ParamIdE::BRAIN_DENSITY)));
    m_bp.m_rateSeconds = syncedSeconds(ParamIdE::BRAIN_SYNC, g(ParamIdE::BRAIN_RATE));
    m_bp.m_holdMin     = g(ParamIdE::BRAIN_HOLD_MIN);
    m_bp.m_holdMax     = g(ParamIdE::BRAIN_HOLD_MAX);
    m_bp.m_low         = static_cast<int>(std::lround(g(ParamIdE::BRAIN_LOW)));
    m_bp.m_high        = static_cast<int>(std::lround(g(ParamIdE::BRAIN_HIGH)));
    m_bp.m_consonance  = g(ParamIdE::BRAIN_CONSONANCE);
    m_bp.m_wander      = g(ParamIdE::BRAIN_WANDER);
    // The spectrum the conductor judges by: the bank's own template, as the voice renders it.
    m_bp.m_timbre = g(ParamIdE::BRAIN_TIMBRE);
    m_bp.m_spacing = g(ParamIdE::BRAIN_SPACING);
    m_bp2.m_spacing = m_bp.m_spacing;
    m_bp.m_harmonic = g(ParamIdE::BRAIN_HARMONIC);
    m_bp2.m_harmonic = m_bp.m_harmonic;
    m_bp.m_key = g(ParamIdE::BRAIN_KEY);
    m_bp2.m_key = m_bp.m_key;
    m_bp.m_even = g(ParamIdE::BRAIN_EVEN);
    m_bp2.m_even = m_bp.m_even;
    m_bp.m_smooth = g(ParamIdE::BRAIN_SMOOTH);
    m_bp2.m_smooth = m_bp.m_smooth;
    // Blend was declared, documented and tested in the third research round -- and never read
    // into the conductor. The test drove the conductor directly, so it passed; through the
    // engine the knob did nothing. Found while adding the lines below it.
    m_bp.m_blend = g(ParamIdE::BRAIN_BLEND);
    m_bp2.m_blend = m_bp.m_blend;
    m_bp.m_cascade = g(ParamIdE::BRAIN_CASCADE);
    m_bp2.m_cascade = m_bp.m_cascade;
    m_bp.m_surprise = g(ParamIdE::BRAIN_SURPRISE);
    m_bp2.m_surprise = m_bp.m_surprise;
    m_bp.m_homeostat = g(ParamIdE::BRAIN_HOMEOSTAT);
    m_bp2.m_homeostat = m_bp.m_homeostat;
    m_bp.m_dejavu = g(ParamIdE::BRAIN_DEJA_VU);
    m_bp2.m_dejavu = m_bp.m_dejavu;
    // The register roles and the intervals that belong to them. Both conductors get them: the
    // background plane is the one that most needs to be told not to put a third in the bass.
    m_bp.m_layers      = g(ParamIdE::BRAIN_LAYERS);       m_bp2.m_layers      = m_bp.m_layers;
    m_bp.m_bassHold    = g(ParamIdE::BRAIN_BASS_HOLD);     m_bp2.m_bassHold    = m_bp.m_bassHold;
    m_bp.m_topSoft     = g(ParamIdE::BRAIN_TOP_SOFT);      m_bp2.m_topSoft     = m_bp.m_topSoft;
    m_bp.m_lowSpacing  = g(ParamIdE::BRAIN_LOW_SPACING);   m_bp2.m_lowSpacing  = m_bp.m_lowSpacing;
    m_bp.m_thirdFloor  = static_cast<int>(std::lround(g(ParamIdE::BRAIN_THIRD_FLOOR)));
    m_bp2.m_thirdFloor = m_bp.m_thirdFloor;
    m_bp.m_leading     = g(ParamIdE::BRAIN_LEADING);      m_bp2.m_leading     = m_bp.m_leading;
    m_bp.m_thirds      = g(ParamIdE::BRAIN_THIRDS);       m_bp2.m_thirds      = m_bp.m_thirds;
    m_bp.m_seconds     = g(ParamIdE::BRAIN_SECONDS);      m_bp2.m_seconds     = m_bp.m_seconds;
    m_bp.m_seventh     = g(ParamIdE::BRAIN_SEVENTH);      m_bp2.m_seventh     = m_bp.m_seventh;
    // What the clock may and may not do.
    m_bp.m_rateBreath   = g(ParamIdE::BRAIN_RATE_BREATH);    m_bp2.m_rateBreath   = m_bp.m_rateBreath;
    m_bp.m_breathPeriod = g(ParamIdE::BRAIN_BREATH_PERIOD);  m_bp2.m_breathPeriod = m_bp.m_breathPeriod;
    m_bp.m_overlap      = g(ParamIdE::BRAIN_OVERLAP);       m_bp2.m_overlap      = m_bp.m_overlap;
    m_bp.m_onsetGuard   = g(ParamIdE::BRAIN_ONSET_GUARD) >= 0.5f;
    m_bp2.m_onsetGuard  = m_bp.m_onsetGuard;
    m_bp.m_releaseGap   = g(ParamIdE::BRAIN_RELEASE_GAP);    m_bp2.m_releaseGap   = m_bp.m_releaseGap;
    m_bp.m_retrigger    = g(ParamIdE::BRAIN_RETRIGGER);     m_bp2.m_retrigger    = m_bp.m_retrigger;
    m_bp.m_densitySlew  = g(ParamIdE::BRAIN_DENSITY_SLEW);   m_bp2.m_densitySlew  = m_bp.m_densitySlew;
    m_bp.m_silence      = g(ParamIdE::BRAIN_SILENCE);       m_bp2.m_silence      = m_bp.m_silence;
    m_bp.m_silenceLen   = g(ParamIdE::BRAIN_SILENCE_LEN);    m_bp2.m_silenceLen   = m_bp.m_silenceLen;
    // Root and long form.
    m_bp.m_rootSteps  = static_cast<int>(std::lround(g(ParamIdE::BRAIN_ROOT_STEPS)));  m_bp2.m_rootSteps  = m_bp.m_rootSteps;
    m_bp.m_rootDown   = g(ParamIdE::BRAIN_ROOT_DOWN);        m_bp2.m_rootDown   = m_bp.m_rootDown;
    m_bp.m_pivot      = g(ParamIdE::BRAIN_PIVOT);           m_bp2.m_pivot      = m_bp.m_pivot;
    m_bp.m_home       = g(ParamIdE::BRAIN_HOME);            m_bp2.m_home       = m_bp.m_home;
    m_bp.m_homeTime   = g(ParamIdE::BRAIN_HOME_TIME);        m_bp2.m_homeTime   = m_bp.m_homeTime;
    m_bp.m_memory     = g(ParamIdE::BRAIN_MEMORY);          m_bp2.m_memory     = m_bp.m_memory;
    m_bp.m_degreeSwap = g(ParamIdE::BRAIN_DEGREE_SWAP);      m_bp2.m_degreeSwap = m_bp.m_degreeSwap;
    // The review against ambient harmony (25.09.2026): the root's targets, the undertone sets and
    // the Harmonic Cloud. The facts about the tuning the review's fixes read are set with the tuning below.
    m_bp.m_rootTargets = clampv(static_cast<int>(std::lround(g(ParamIdE::BRAIN_ROOT_TARGETS))), 0, 3);   m_bp2.m_rootTargets = m_bp.m_rootTargets;
    m_bp.m_utonal     = g(ParamIdE::BRAIN_UTONAL);          m_bp2.m_utonal     = m_bp.m_utonal;
    m_bp.m_series     = g(ParamIdE::BRAIN_SERIES);          m_bp2.m_series     = m_bp.m_series;
    m_bp.m_loop = static_cast<int>(std::lround(g(ParamIdE::BRAIN_LOOP)));
    m_bp2.m_loop = m_bp.m_loop;
    m_bp.m_spread = g(ParamIdE::BRAIN_SPREAD);
    m_bp2.m_spread = m_bp.m_spread;
    m_bp.m_bias = g(ParamIdE::BRAIN_BIAS);
    m_bp2.m_bias = m_bp.m_bias;
    // The spectrum is needed by the conductor's Timbre ear and by the timbre scale, and the
    // scale is read further down, so the question is asked here from the raw value.
    const bool wantTimbreScale = std::lround(getParam(ParamIdE::SCALE)) == TimbreScaleIndex;
    {   // Computed every block now, not only when Timbre or the timbre scale asks: the panel's
        // Tuning display draws the roughness curve from it. A dozen pows; nothing downstream
        // reads it unless Timbre is up, so the sound is untouched.
        (void)wantTimbreScale;
        const int count = std::min(BrainSpectrum::Max, std::max(1, m_vp.m_partials));
        const float hc = 1.0f + m_vp.m_brightness * m_vp.m_brightness * 31.0f;
        const double B = static_cast<double>(m_vp.m_inharmonic) * m_vp.m_inharmonic * 0.02;
        m_brainSpec.m_count = count;
        for (int h = 1; h <= count; ++h) {
            double a = std::pow(static_cast<double>(h), -static_cast<double>(m_vp.m_tilt));
            if (m_vp.m_oddEven > 0.0f && (h % 2) == 0) a *= 1.0 - m_vp.m_oddEven;
            if (m_vp.m_oddEven < 0.0f && (h % 2) == 1 && h > 1) a *= 1.0 + m_vp.m_oddEven;
            if (static_cast<float>(h) > hc) { const float x = std::min((static_cast<float>(h) - hc) / 6.0f, 1.0f); a *= 0.5 * (1.0 + std::cos(Pi * x)); }
            m_brainSpec.m_amp[h - 1] = a;
            m_brainSpec.m_ratio[h - 1] = m_vp.m_match > 0.0f ? static_cast<double>(m_vp.m_partialRatio[h - 1])
                                                       : h * (B > 0.0 ? std::sqrt(1.0 + B * h * h) : 1.0);
        }
    }
    m_bp.m_spectrum = &m_brainSpec;
    m_bp2.m_timbre = m_bp.m_timbre;
    m_bp2.m_spectrum = &m_brainSpec;
    m_brainQuant     = clampv(static_cast<int>(std::lround(g(ParamIdE::BRAIN_QUANTIZE))), 0, NumSyncDivs - 1);
    // Autoplay. In Chords the conductor keeps the cluster full and exchanges one voice at a time;
    // the rate can come from the clock instead of the seconds knob.
    m_bp.m_mode         = static_cast<BrainModeE>(clampv(static_cast<int>(std::lround(g(ParamIdE::AUTO_MODE))), 0, NumBrainModes - 1));
    m_bp.m_voiceLead    = g(ParamIdE::AUTO_LEAD);
    m_bp.m_chordTension = g(ParamIdE::AUTO_TENSION);
    m_bp.m_rootMove     = g(ParamIdE::AUTO_ROOT_MOVE);
    if (m_bp.m_mode == BrainModeE::CHORDS) m_bp.m_rateSeconds = syncedSeconds(ParamIdE::AUTO_SYNC, g(ParamIdE::AUTO_RATE));
    {   // Step is a trigger: it fires on the rising edge and the host's switch is left alone.
        const bool now = g(ParamIdE::AUTO_STEP) >= 0.5f;
        if (now && !m_autoStepWas) m_brain.requestStep();
        m_autoStepWas = now;
        if (m_autoStepAsked.exchange(false, std::memory_order_relaxed)) m_brain.requestStep();
    }
    m_vp.m_pressDistance = g(ParamIdE::PRESS_DISTANCE);
    m_vp.m_pressBright   = g(ParamIdE::PRESS_BRIGHT);
    m_vp.m_pressLevel    = g(ParamIdE::PRESS_LEVEL);
    m_vp.m_slideCutoff   = g(ParamIdE::SLIDE_CUTOFF);
    m_vp.m_slideZ        = g(ParamIdE::SLIDE_Z);
    m_bendRange        = g(ParamIdE::BEND_RANGE);
    // The second conductor. Its root follows the first one's plus an interval, so the two stay
    // in one harmony however far the first one's root wanders.
    m_brain2On        = g(ParamIdE::BRAIN2_ON) >= 0.5f;
    m_bp2.m_on          = m_brain2On;
    m_bp2.m_density     = static_cast<int>(std::lround(g(ParamIdE::BRAIN2_DENSITY)));
    m_bp2.m_rateSeconds = g(ParamIdE::BRAIN2_RATE);
    m_bp2.m_holdMin     = g(ParamIdE::BRAIN2_HOLD_MIN);
    m_bp2.m_holdMax     = g(ParamIdE::BRAIN2_HOLD_MAX);
    m_bp2.m_low         = static_cast<int>(std::lround(g(ParamIdE::BRAIN2_LOW)));
    m_bp2.m_high        = static_cast<int>(std::lround(g(ParamIdE::BRAIN2_HIGH)));
    m_bp2.m_consonance  = g(ParamIdE::BRAIN2_CONSONANCE);
    m_bp2.m_wander      = 0.0f;   // it follows the first conductor's root instead of wandering itself
    m_brain2Depth     = g(ParamIdE::BRAIN2_DEPTH);
    m_brain2Interval  = static_cast<int>(std::lround(g(ParamIdE::BRAIN2_INTERVAL)));
    // Golden: the background layer's clock is stretched against the foreground's by the one ratio
    // that has no good rational approximation, so the two never fall into a simple relation and
    // are never heard as one (Anti 15). It overrides Brain 2's own rate and holds, which is the
    // point -- a ratio cannot be guaranteed while both ends are set by hand.
    if (g(ParamIdE::BRAIN2_GOLDEN) >= 0.5f) {
        constexpr double phi = 1.6180339887498949;
        m_bp2.m_rateSeconds = static_cast<float>(m_bp.m_rateSeconds * phi);
        m_bp2.m_holdMin     = static_cast<float>(m_bp.m_holdMin * phi);
        m_bp2.m_holdMax     = static_cast<float>(m_bp.m_holdMax * phi);
    }

    // Hour-scale arc: a very slow drift that leans on density, brightness and depth.
    const float a = m_arcOut * m_arcAmount;
    m_bp.m_density = clampv(m_bp.m_density + static_cast<int>(std::lround(a * 2.0f)), 1, ClusterBrain::Slots);
    // And on the harmony, if asked. Lerdahl and Krumhansl (2007) modelled tonal tension and tested
    // it against listeners: tension rises with distance from the tonic in pitch space and with
    // surface dissonance, and falls back as the music returns. This instrument's proxies for those
    // are the three judgements the conductor already makes -- rootedness (Harmonic), stability in
    // the key (Key), and consonance -- so the arc's rise loosens all three and its fall tightens
    // them. The same lean, in the same direction, as the arc gives density and brightness: the
    // climax of the night is denser, brighter, and further from home.
    m_arcHarmony = g(ParamIdE::ARC_HARMONY);
    m_arcLean = m_arcOut * m_arcHarmony;
    if (m_arcHarmony > 0.0f) {
        const float lean = m_arcLean;
        auto leaned = [lean](float base) {
            // Tense: down towards a fraction of itself. Relaxed: up towards one. Continuous
            // through zero, where nothing happens.
            return lean >= 0.0f ? base * (1.0f - 0.8f * lean) : base + (1.0f - base) * (0.6f * -lean);
        };
        m_bp.m_harmonic = clampv(leaned(m_bp.m_harmonic), 0.0f, 1.0f);
        m_bp.m_key = clampv(leaned(m_bp.m_key), 0.0f, 1.0f);
        m_bp.m_consonance = clampv(leaned(m_bp.m_consonance), 0.0f, 1.0f);
        m_bp2.m_harmonic = m_bp.m_harmonic; m_bp2.m_key = m_bp.m_key;
    }
    m_vp.m_brightness = clampv(m_vp.m_brightness * (1.0f + 0.25f * a), 0.0f, 1.0f);
    m_depth = clampv(m_depth * (1.0f + 0.3f * a), 0.0f, 1.0f);

    m_ensemble.set(g(ParamIdE::ENSEMBLE_MIX), g(ParamIdE::ENSEMBLE_DEPTH), syncedHz(ParamIdE::ENSEMBLE_SYNC, g(ParamIdE::ENSEMBLE_RATE)));
    m_ensemble.setMode(clampv(static_cast<int>(std::lround(getParam(ParamIdE::ENS_MODE))), 0, 2));
    m_haas.set(g(ParamIdE::HAAS), g(ParamIdE::HAAS_TIME));
    m_early.setRoom(g(ParamIdE::EARLY_SIZE), g(ParamIdE::EARLY_ABSORB), g(ParamIdE::EARLY_WIDTH));
    m_early.setLevel(g(ParamIdE::EARLY_LEVEL));
    {   // Where the room thinks the sound is: the level-weighted mean of the sounding voices'
        // pan and distance. One position for the whole near bus, which is the simplification the
        // class documents; it follows the music, which is what the early pattern is for.
        float wsum = 0.0f, pan = 0.0f, dist = 0.0f;
        for (const auto& v : m_voices) {
            if (!v.isActive()) continue;
            const float wgt = v.level();
            wsum += wgt; pan += wgt * v.pan(); dist += wgt * v.distance();
        }
        if (wsum > 1.0e-6f) m_early.setSource(pan / wsum, dist / wsum);
    }
    m_delay.set(syncedSeconds(ParamIdE::DELAY_SYNC_L, g(ParamIdE::DELAY_TIME_L)), syncedSeconds(ParamIdE::DELAY_SYNC_R, g(ParamIdE::DELAY_TIME_R)),
               g(ParamIdE::DELAY_FEEDBACK), g(ParamIdE::DELAY_CROSS), g(ParamIdE::DELAY_DAMP), g(ParamIdE::DELAY_ABSORB));
    m_delay.setDuck(g(ParamIdE::DELAY_DUCK));
    m_delayMix   = g(ParamIdE::DELAY_MIX);
    m_delayToFar = g(ParamIdE::DELAY_TO_FAR);
    m_delay2.set(syncedSeconds(ParamIdE::DELAY2_SYNC_L, g(ParamIdE::DELAY2_TIME_L)), syncedSeconds(ParamIdE::DELAY2_SYNC_R, g(ParamIdE::DELAY2_TIME_R)),
                g(ParamIdE::DELAY2_FEEDBACK), g(ParamIdE::DELAY2_CROSS), g(ParamIdE::DELAY2_DAMP), g(ParamIdE::DELAY2_ABSORB));
    m_delay2Mix   = g(ParamIdE::DELAY2_MIX);
    m_delay2ToFar = g(ParamIdE::DELAY2_TO_FAR);
    m_cloud.set(syncedHz(ParamIdE::CLOUD_SYNC, g(ParamIdE::CLOUD_DENSITY)), g(ParamIdE::CLOUD_SIZE), g(ParamIdE::CLOUD_PITCH), g(ParamIdE::CLOUD_SPRAY), g(ParamIdE::CLOUD_LEVEL));
    m_cloud.setLoop(g(ParamIdE::CLOUD_FEEDBACK), g(ParamIdE::CLOUD_TONE));
    m_cloud.setScatter(g(ParamIdE::CLOUD_TRANSPOSE), g(ParamIdE::CLOUD_SCATTER));
    m_cloud.setSwarm(g(ParamIdE::CLOUD_SWARM));
    m_cloud.setShift(CloudShiftSemitones[clampv(static_cast<int>(std::lround(g(ParamIdE::CLOUD_SHIFT))), 0, 6)]);
    m_cloud.setResonators(g(ParamIdE::CLOUD_RESONANCE), static_cast<int>(std::lround(g(ParamIdE::CLOUD_RES_MODE))),
                         static_cast<int>(std::lround(g(ParamIdE::CLOUD_RES_NOTES))), g(ParamIdE::CLOUD_RES_DECAY));
    m_cloudSend = g(ParamIdE::CLOUD_SEND);
    m_cloudToNear = g(ParamIdE::CLOUD_TO_NEAR);
    // Send Low Cut: one high-pass in front of all three reverbs' inputs (25.09.2026).
    m_sendLowcut = g(ParamIdE::SEND_LOWCUT);
    m_nearReverb.setSendLowcut(m_sendLowcut);
    m_farReverb.setSendLowcut(m_sendLowcut);
    if (m_sendLowcut > 21.0f) { m_roomSendHpL.setQ(clampv(m_sendLowcut, 20.0f, 1000.0f), 0.7071f, static_cast<float>(m_sr)); m_roomSendHpR.copyCoefficients(m_roomSendHpL); }
    m_nearReverb.setSpace(0.3f, 20000.0f, g(ParamIdE::NEAR_LOWCUT));
    m_nearReverb.set(0.6f, g(ParamIdE::NEAR_DECAY), g(ParamIdE::NEAR_DAMP), 5.0f, false, g(ParamIdE::NEAR_MIX));
    m_unmask.set(g(ParamIdE::FAR_UNMASK), g(ParamIdE::FAR_UNMASK_SPREAD), g(ParamIdE::FAR_UNMASK_RETURN));
    // The guide's second round (25.09.2026): the main room ducks under the near bus on the same
    // knob as the far hall, feeds the far hall (To Far), the far return loses the low middle of its
    // centre (Mid Low Cut), and the Foundation has a ceiling of its own.
    m_roomUnmask.set(g(ParamIdE::FAR_UNMASK), g(ParamIdE::FAR_UNMASK_SPREAD), g(ParamIdE::FAR_UNMASK_RETURN));
    m_roomToFar = g(ParamIdE::ROOM_TO_FAR);
    m_farMidLowcut = g(ParamIdE::FAR_MID_LOWCUT);
    if (m_farMidLowcut > 21.0f) m_farMidHp.setQ(clampv(m_farMidLowcut, 20.0f, 1000.0f), 0.7071f, static_cast<float>(m_sr));
    m_subCeiling = g(ParamIdE::SUB_CEILING);
    m_bodyLevel = g(ParamIdE::BODY_LEVEL);
    m_bodyPitch = g(ParamIdE::BODY_PITCH);
    m_body.set(static_cast<BodyMaterialE>(clampv(static_cast<int>(std::lround(g(ParamIdE::BODY_MATERIAL))), 0, NumBodyMaterials - 1)),
              static_cast<float>(frequencyOf(m_brain.root())) * m_bodyPitch * m_vp.m_pitchMul,
              g(ParamIdE::BODY_DECAY), g(ParamIdE::BODY_TONE), g(ParamIdE::BODY_SPREAD));
    m_patina.set(g(ParamIdE::PATINA_AMOUNT), g(ParamIdE::PATINA_WOW), g(ParamIdE::PATINA_HISS), g(ParamIdE::PATINA_AGE));
    m_farReverb.setSpace(g(ParamIdE::FAR_ASYM), g(ParamIdE::FAR_HIGHCUT), g(ParamIdE::FAR_LOWCUT));
    m_farReverb.set(g(ParamIdE::FAR_SIZE), g(ParamIdE::FAR_DECAY), g(ParamIdE::FAR_DAMP), g(ParamIdE::FAR_PRE_DELAY), g(ParamIdE::FAR_FREEZE) >= 0.5f, 1.0f);
    m_farLevel = g(ParamIdE::FAR_LEVEL);
    m_midSide.set(g(ParamIdE::BASS_MONO), g(ParamIdE::SIDE_AIR), g(ParamIdE::WIDTH));
    m_midSide.setMonoGuard(g(ParamIdE::MONO_GUARD) >= 0.5f);
    m_midSide.setTilt(g(ParamIdE::TILT2), g(ParamIdE::TILT_PIVOT));
    m_diffuser.set(g(ParamIdE::FAR_DIFFUSE));
    m_farDiffuse = g(ParamIdE::FAR_DIFFUSE);   // also drives the air saturation ahead of the reverb
    m_sympathy = g(ParamIdE::SYMPATHY);
    m_vp.m_sympathy = m_sympathy;
    m_roomLevel    = g(ParamIdE::ROOM_LEVEL);
    m_roomSource   = static_cast<int>(std::lround(g(ParamIdE::ROOM_SOURCE)));
    m_roomPreDelay = static_cast<int>(g(ParamIdE::ROOM_PRE_DELAY) * 0.001f * static_cast<float>(m_sr));
    m_roomHighcut  = g(ParamIdE::ROOM_HIGHCUT);
    m_roomLowcut   = g(ParamIdE::ROOM_LOWCUT);
    m_subsonicHz   = g(ParamIdE::SUBSONIC);
    m_roomMorph    = m_hasImpulseB ? g(ParamIdE::ROOM_MORPH) : 0.0f;
    m_fbBus   = g(ParamIdE::FEEDBACK_BUS);
    m_fbFm    = g(ParamIdE::FEEDBACK_FM);
    m_fbTone  = g(ParamIdE::FEEDBACK_TONE);
    m_fbDrive = g(ParamIdE::FEEDBACK_DRIVE);
    m_fbBias  = g(ParamIdE::FEEDBACK_BIAS);
    m_vp.m_fmAmount = m_fbFm;

    // Cosmos
    m_cosmosSend   = g(ParamIdE::COSMOS_SEND);
    m_cosmosReturn = g(ParamIdE::COSMOS_RETURN);
    m_cosmosToFar  = g(ParamIdE::COSMOS_TO_FAR);
    m_cosmosNebula = g(ParamIdE::COSMOS_NEBULA);
    const float shiftHz = g(ParamIdE::COSMOS_SHIFT) * (1.0f + 0.5f * g(ParamIdE::COSMOS_SHIFT_DRIFT) * m_shiftDrift.value());
    m_shifter.set(shiftHz, shiftHz == 0.0f ? 0.0f : 1.0f);
    const float rootHz = static_cast<float>(scaleFrequency(*m_scale, m_brain.root(), 60 + clampv(static_cast<int>(std::lround(g(ParamIdE::ROOT_NOTE))), 0, 11), g(ParamIdE::REF_PITCH), m_snapKeys));
    m_resonator.set(rootHz * g(ParamIdE::COSMOS_RES_PITCH), g(ParamIdE::COSMOS_RES_FEEDBACK), g(ParamIdE::COSMOS_RES));
    {   // The cloud's scatter and resonators hear the same root, placed in the scale against the key's
        // tonic; the Memory's Seek listens for the scale from that tonic.
        const int keyRoot = 60 + clampv(static_cast<int>(std::lround(g(ParamIdE::ROOT_NOTE))), 0, 11);
        const double tonicHz = scaleFrequency(*m_scale, keyRoot, keyRoot, g(ParamIdE::REF_PITCH), m_snapKeys);
        m_cloud.setHarmony(*m_scale, tonicHz, rootHz);
        m_memory.setHarmony(*m_scale, tonicHz);
    }

    // Memory
    m_memSend   = g(ParamIdE::MEM_SEND);
    m_memReturn = g(ParamIdE::MEM_RETURN);
    m_memToFar  = g(ParamIdE::MEM_TO_FAR);
    m_memory.setShape(2 << clampv(static_cast<int>(std::lround(g(ParamIdE::MEM_LINES))), 0, 2),
                     g(ParamIdE::MEM_SIZE), g(ParamIdE::MEM_BLUR), g(ParamIdE::MEM_DRIFT));
    m_memory.setDecay(g(ParamIdE::MEM_HOLD), g(ParamIdE::MEM_AGE), g(ParamIdE::MEM_RENEW), g(ParamIdE::MEM_DRIVE));
    m_memory.setRecall(g(ParamIdE::MEM_RECALL), g(ParamIdE::MEM_SEEK), g(ParamIdE::MEM_GRAIN));
    m_memory.setTape(g(ParamIdE::MEM_REVERSE) >= 0.5f, g(ParamIdE::MEM_HALF) >= 0.5f,
                    g(ParamIdE::MEM_FREEZE) >= 0.5f, g(ParamIdE::MEM_ERASE) >= 0.5f);
    m_vowel.set(g(ParamIdE::COSMOS_VOWEL), g(ParamIdE::COSMOS_VOWEL_RATE));
    m_nebula.set(g(ParamIdE::COSMOS_SMEAR));
    m_cosmosShimmer = g(ParamIdE::COSMOS_SHIMMER);
    const int sp = clampv(static_cast<int>(std::lround(g(ParamIdE::COSMOS_SHIMMER_PITCH))), 0, NumShimmerPitches - 1);
    m_shimmerL.setSemitones(ShimmerPitchSemitones[sp]);
    m_shimmerR.setSemitones(ShimmerPitchSemitones[sp]);
    m_shimmerSpec.setSemitones(ShimmerPitchSemitones[sp]);
    m_shimmerMode = clampv(static_cast<int>(std::lround(g(ParamIdE::COSMOS_SHIMMER_MODE))), 0, 1);

    // Tuning
    const int scaleIdx = clampv(static_cast<int>(std::lround(g(ParamIdE::SCALE))), 0, NumScaleChoices - 1);
    // The scale that is computed rather than tabulated. It is rebuilt only when the spectrum it
    // is made of has actually moved, and at most a few times a second: sweeping the dissonance
    // curve is a third of a millisecond, which is nothing now and then and too much every block.
    // A knob turn therefore retunes the instrument in small steps rather than continuously, which
    // is also kinder to a chord that is already sounding.
    bool scaleRebuilt = false;
    if (scaleIdx == TimbreScaleIndex && m_brainSpec.m_count > 0) {
        double sig = m_brainSpec.m_count;
        for (int i = 0; i < m_brainSpec.m_count; ++i) sig += m_brainSpec.m_ratio[i] * 7.0 + m_brainSpec.m_amp[i] * 131.0;
        if (++m_timbreScaleWait >= 96 && std::fabs(sig - m_timbreScaleSig) > 1.0e-9) {
            m_timbreScaleWait = 0;
            m_timbreScaleSig = sig;
            FixedScale built;
            if (makeTimbreScale(m_brainSpec, built)) { m_scales[TimbreScaleIndex] = built; scaleRebuilt = true; }
        }
    }
    m_scale = &m_scales[scaleIdx];
    m_refPitch = g(ParamIdE::REF_PITCH);
    {   // The stretched octave. A change while notes are held retunes them like a purity change.
        const float st = g(ParamIdE::TUNE_STRETCH);
        if (st != m_stretchCents) { m_stretchCents = st; m_stretchChanged = true; }
    }
    m_snapKeys = std::lround(g(ParamIdE::KEY_MAP)) == 0;
    const int rootPc = clampv(static_cast<int>(std::lround(g(ParamIdE::ROOT_NOTE))), 0, 11);
    {
        // R4.7: when the root moves, the ground does not move under what is already sounding. Each
        // voice keeps the frequency it was given, by carrying the ratio the move would have made;
        // notes that begin afterwards take the new root's ratios, so the harmony changes by what
        // enters rather than by everything sliding at once. Off, everything glides as before.
        const int newRoot = 60 + rootPc;
        if (newRoot != m_rootNote && g(ParamIdE::TUNE_HOLD_SOUNDING) >= 0.5f) {
            double before[MaxVoices];
            int i = 0;
            for (const auto& v : m_voices) { before[i] = v.isActive() ? frequencyOf(v.note()) : 0.0; ++i; }
            m_rootNote = newRoot;
            i = 0;
            for (auto& v : m_voices) {
                const double b = before[i++];
                if (b > 0.0) { const double a2 = frequencyOf(v.note()); if (a2 > 0.0) v.setRootComp(v.rootComp() * static_cast<float>(b / a2)); }
            }
        } else m_rootNote = newRoot;
    }
    {   // What the conductors need to know about the tuning (the review's F1 and F6): the scale, for
        // a key profile of its own and for placing a difference tone; whether it repeats at the
        // octave; and whether a MIDI step is a semitone at all.
        const bool octave = std::fabs(m_scale->m_period - 2.0) < 1.0e-9;
        const double firstHz = scaleFrequency(*m_scale, m_rootNote, m_rootNote, m_refPitch, m_snapKeys);
        for (BrainParams* b : { &m_bp, &m_bp2 }) {
            b->m_scale = m_scale;
            b->m_octavePeriodic = octave;
            b->m_consecutive = !m_snapKeys || !octave;   // a non-octave scale is walked degree by degree whatever Key Map says
            b->m_scaleRootHz = firstHz;
        }
    }
    {
        const float purity = g(ParamIdE::TUNE_PURITY), drift = g(ParamIdE::TUNE_DRIFT);
        float wander = drift > 0.0f ? 0.5f * drift * m_purityDrift.value() : 0.0f;   // drifter is advanced in process()
        // The fluctuation guard. Fastl and Zwicker: the sensation of fluctuation peaks at a
        // modulation rate of 4 Hz and is gone by about 20; roughness takes over at around 70.
        // Between two and eight hertz a beating chord is heard as wobble, which in a sleep concert
        // is the one thing it must not be. Purity Drift makes beats without knowing where they
        // land; the BEAT source already measures where they landed. Above zero the guard reins
        // the drift in while the beat sits in that band, so the beat slows out of it; below zero
        // it does the opposite and seeks the wobble out. Zero leaves the drift exactly alone.
        m_guardFactor = 1.0f;
        {
            const float guard = g(ParamIdE::TUNE_GUARD);
            if (guard != 0.0f && drift > 0.0f && m_beatHz > 0.05f) {
                const float x = std::log2(m_beatHz / 4.0f);                 // 0 at 4 Hz, +-1 at 2 and 8
                const float inBand = std::exp(-x * x * 1.25f);              // 1 at 4 Hz, 0.29 at the edges
                if (guard > 0.0f) m_guardFactor = 1.0f - guard * inBand;
                else m_guardFactor = 1.0f + (-guard) * (1.0f - inBand) * (m_beatHz < 4.0f ? 1.0f : -1.0f);
                m_guardFactor = clampv(m_guardFactor, 0.0f, 2.0f);
                wander *= m_guardFactor;
            }
        }
        m_purityCur = clampv(purity + wander, 0.0f, 1.0f);
        m_adaptAmt = g(ParamIdE::TUNE_ADAPT);
        {
            static const double Transpose[7] = { 0.0, 0.41503749927884381, 0.58496250072115619, 1.0, -0.41503749927884381, -0.58496250072115619, -1.0 };
            m_transposeTarget = Transpose[clampv(static_cast<int>(std::lround(g(ParamIdE::TRANSPOSE))), 0, 6)];
        }
        m_retune = m_purityCur < 0.9999 || drift > 0.0f || m_stretchChanged || scaleRebuilt || m_adaptAmt > 0.0f || m_commaCents != 0.0
               || m_transposeCur != m_transposeTarget || m_transposeCur != 0.0;
        m_stretchChanged = false;
    }
    m_vp.m_freeze = g(ParamIdE::FREEZE) >= 0.5f;
    m_vp.m_airGhost = std::lround(g(ParamIdE::AIR_MODE)) == 1;
    m_vp.m_rootHz = frequencyOf(m_brain.root());
    m_portamento = g(ParamIdE::PORTAMENTO);
    m_portaGravity = g(ParamIdE::PORTA_GRAVITY);
    m_fbTape = g(ParamIdE::FEEDBACK_TAPE);
    {   // Coherence: four Kuramoto oscillators, coupled by K = Coherence; their sines become
        // offsets on brightness, depth, pan and the z-plane point, scaled by Depth.
        const float depth = g(ParamIdE::COHERENCE_DEPTH);
        m_vp.m_cohBrightness = 0.15f * depth * std::sin(m_kuraPhase[0]);
        m_depth = clampv(m_depth + 0.15f * depth * std::sin(m_kuraPhase[1]), 0.0f, 1.0f);
        m_vp.m_cohPan = 0.4f * depth * std::sin(m_kuraPhase[2]);
        m_vp.m_cohZ = 0.25f * depth * std::sin(m_kuraPhase[3]);
    }
    if (rootPc != m_lastRootPc) {
        m_lastRootPc = rootPc;
        m_brain.setRoot(48 + rootPc);
    }
    // ---- the near layer (13.09.2026)
    m_rolesUsed = false;
    for (int k = 0; k < Slots; ++k) if (m_vp.m_slot[k].m_role != SlotRoleE::ALL) m_rolesUsed = true;
    m_np.m_level     = g(ParamIdE::FORE_LEVEL);
    m_np.m_kind      = clampv(static_cast<int>(std::lround(g(ParamIdE::FORE_KIND))), 0, 2);
    m_np.m_rate      = g(ParamIdE::FORE_RATE);
    m_np.m_chance    = g(ParamIdE::FORE_CHANCE);
    m_np.m_cluster   = g(ParamIdE::FORE_CLUSTER);
    m_np.m_length    = g(ParamIdE::FORE_LENGTH);
    m_np.m_pitch     = clampv(static_cast<int>(std::lround(g(ParamIdE::FORE_PITCH))), 0, 4);
    m_np.m_spread    = g(ParamIdE::FORE_SPREAD);
    m_np.m_approach  = g(ParamIdE::FORE_APPROACH);
    m_np.m_distance  = g(ParamIdE::FORE_DISTANCE);
    m_np.m_dry       = g(ParamIdE::FORE_DRY);
    m_np.m_toDelay2  = g(ParamIdE::FORE_DELAY2);
    m_np.m_toCosmos  = g(ParamIdE::FORE_COSMOS);
    m_np.m_proximity = g(ParamIdE::FORE_PROXIMITY);
    m_np.m_hold      = g(ParamIdE::FORE_HOLD) >= 0.5f;
    m_np.m_glide     = g(ParamIdE::FORE_GLIDE);
    m_np.m_steps     = static_cast<int>(std::lround(g(ParamIdE::FORE_STEPS)));
    m_np.m_stepSeconds = g(ParamIdE::FORE_STEP);
    m_np.m_stepSync  = clampv(static_cast<int>(std::lround(g(ParamIdE::FORE_STEP_SYNC))), 0, NumSyncDivs - 1);
    m_np.m_mutation  = g(ParamIdE::FORE_MUTATION);
    m_np.m_scatter   = g(ParamIdE::FORE_SCATTER);
    m_np.m_bloom     = g(ParamIdE::FORE_BLOOM);
    m_np.m_attack    = g(ParamIdE::FORE_ATTACK);
    m_np.m_release   = g(ParamIdE::FORE_RELEASE);
    m_nearGain     = dbToGain(g(ParamIdE::FORE_GAIN));
    {
        // What a near event's voice renders with: the sound preset's voice with its four slots
        // put out and the Near Source in the last one, its own envelope, its own filter, its own
        // strike; no z-plane, no air, no bloom, no breath -- the event is placed by the scheduler
        // and shaped by the section, and owes the background nothing but the room around it.
        m_vpNear = m_vp;
        for (int k = 0; k < Slots; ++k) { m_vpNear.m_slot[k].m_type = SourceTypeE::OFF; m_vpNear.m_slot[k].m_role = SlotRoleE::ALL; }
        SlotParams& s = m_vpNear.m_slot[Slots - 1];
        s = SlotParams{};
        s.m_type          = static_cast<SourceTypeE>(clampv(static_cast<int>(std::lround(g(ParamIdE::FORE_TYPE))), 0, NumSourceTypes - 1));
        s.m_level         = m_np.m_level;
        s.m_octave        = static_cast<int>(std::lround(g(ParamIdE::FORE_OCTAVE)));
        s.m_ratio         = static_cast<int>(std::lround(g(ParamIdE::FORE_RATIO)));
        s.m_position      = g(ParamIdE::FORE_POSITION);
        s.m_positionDrift = g(ParamIdE::FORE_POS_DRIFT);
        s.m_density       = g(ParamIdE::FORE_DENSITY);
        s.m_follow        = g(ParamIdE::FORE_FOLLOW) >= 0.5f;
        s.m_bright        = g(ParamIdE::FORE_BRIGHT);
        s.m_bowForce      = g(ParamIdE::FORE_FORCE);
        s.m_bowSpeed      = g(ParamIdE::FORE_SPEED);
        s.m_noise         = static_cast<NoiseKindE>(clampv(static_cast<int>(std::lround(g(ParamIdE::FORE_NOISE))), 0, NumNoiseKinds - 1));
        s.m_noiseQ        = g(ParamIdE::FORE_NOISE_Q);
        s.m_fmRatio       = g(ParamIdE::FORE_FM_RATIO);
        s.m_fmIndex       = g(ParamIdE::FORE_FM_INDEX);
        s.m_partials      = static_cast<int>(std::lround(g(ParamIdE::FORE_PARTIALS)));
        s.m_tilt          = g(ParamIdE::FORE_TILT);
        s.m_inharm        = g(ParamIdE::FORE_INHARM);
        s.m_drift         = g(ParamIdE::FORE_DRIFT);
        s.m_table         = static_cast<int>(std::lround(g(ParamIdE::FORE_TABLE)));
        m_vpNear.m_attack      = m_np.m_attack;
        m_vpNear.m_decay       = g(ParamIdE::FORE_DECAY);
        m_vpNear.m_sustain     = g(ParamIdE::FORE_SUSTAIN);
        m_vpNear.m_release     = m_np.m_release;
        m_vpNear.m_cutoff      = g(ParamIdE::FORE_CUTOFF);
        m_vpNear.m_resonance   = g(ParamIdE::FORE_RESONANCE);
        m_vpNear.m_filterModel = clampv(static_cast<int>(std::lround(g(ParamIdE::FORE_FILTER_MODEL))), 0, NumFilterModels - 1);
        m_vpNear.m_filterEnv   = g(ParamIdE::FORE_FILTER_ENV);
        m_vpNear.m_filterOn    = true;
        m_vpNear.m_filterParallel = false;
        m_vpNear.m_filterDrive = 0.0f;
        m_vpNear.m_filterMorph = 0.0f;
        m_vpNear.m_fold        = 0.0f;
        m_vpNear.m_zMode       = 0;
        m_vpNear.m_air         = 0.0f;
        m_vpNear.m_strikeLevel = g(ParamIdE::FORE_STRIKE);
        m_vpNear.m_strikeType  = static_cast<int>(std::lround(g(ParamIdE::FORE_STRIKE_TYPE)));
        m_vpNear.m_strikeDecay = g(ParamIdE::FORE_STRIKE_DECAY);
        m_vpNear.m_strikeDamp  = g(ParamIdE::FORE_STRIKE_DAMP);
        m_vpNear.m_strikeBrain = true;
        m_vpNear.m_velAttack   = 0.0f;
        m_vpNear.m_bloom       = 0.0f;
        m_vpNear.m_breath      = 0.0f;
        m_vpNear.m_freeze      = false;
        m_vpNear.m_sympathy    = 0.0f;
        m_vpNear.m_fmAmount    = 0.0f;
        m_vpNear.m_partialSpread = 0.0f;
        m_vpNear.m_lowCut      = 0.0f;
        {   // The near source's own clip in its slot -- the event's pick of the pool -- or Source 4's where it has none.
            const int a = m_nearPoolActive.load(std::memory_order_acquire);
            if (a >= 0 && !m_nearPools[a].empty()) {
                const std::vector<Texture>& pool = m_nearPools[a];
                m_vpNear.m_texture[Slots - 1] = &pool[std::min(static_cast<size_t>(std::max(0, m_nearPick)), pool.size() - 1)];
            }
        }
    }
    const int seed = static_cast<int>(std::lround(g(ParamIdE::SEED)));
    if (seed != m_seed) {
        m_seed = seed;
        m_rng.seed(static_cast<uint64_t>(m_seed) + 1);
        m_brain.reset(m_rng.fork(), 48 + rootPc);
        m_brain2.reset(0x2B2A1Full + static_cast<uint64_t>(m_seed), 48 + rootPc);
        m_arc.init(m_rng);
    }
}

} // namespace ambient
