/**
 * @file EngineRender.cpp
 * @brief What the engine renders: process() with its denormal control and its sleep
 *        check, and renderChunk, which is the signal path from the voices through both planes to the
 *        master.
 *
 * Split out of Engine.cpp.
 */
#include "ambient/Engine.h"
#include "ambient/PresetMap.h"
#include "ambient/PresetMeta.h"
#include <cmath>
#include <ctime>     // the clock the arc can follow
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

// ---------------------------------------------------------------- audio

/**
 * @brief Message thread, before this engine is heard: take a cluster over from the one that is leaving.
 *
 * The notes are started exactly as the conductor's own would be, so they carry this preset's
 * depth, its spatial placing and its envelopes -- the same chord, played by another instrument.
 */
void Engine::adoptCluster(const int* notes, const float* vels, int count, bool second)
{
    // The inherited notes are not new notes. They were sounding on the engine that is leaving and
    // they go on sounding here, so they arrive already grown: the Bloom open, and every source
    // that waits for its Delay already in. Started from zero instead, a preset whose texture
    // enters after twenty seconds would spend the whole crossfade and the twenty seconds after it
    // being only half of itself -- the crossfade would change the instrument into a sketch of the
    // instrument. A minute is past every delay the parameter allows.
    constexpr float kInherited = 60.0f;
    if (second) {
        brain2_.adopt(notes, vels, count, [this](const BrainEvent& e) {
            if (e.type == BrainEvent::Type::NoteOn) startNote(e.note, e.velocity, OwnerBrain2, clampv(depth_ * brain2Depth_, 0.0f, 1.0f), kInherited);
            else stopNote(e.note, OwnerBrain2);
        });
    } else {
        brain_.adopt(notes, vels, count, [this](const BrainEvent& e) {
            if (e.type == BrainEvent::Type::NoteOn) {
                const float u = rng_.uniform();
                float d = (u < 0.4f) ? depth_ * 0.15f * rng_.uniform()
                                     : depth_ * (0.55f + 0.45f * rng_.uniform());
                if (layerDepth_ > 0.0f) {
                    const int n = e.note;
                    d += (depth_ * (n < 43 ? 0.08f : n < 60 ? 0.22f : n < 84 ? 0.55f : 0.9f) - d) * layerDepth_;
                }
                startNote(e.note, e.velocity, OwnerBrain, clampv(d, 0.0f, 1.0f), kInherited);
            } else stopNote(e.note, OwnerBrain);
        });
    }
    // A cluster thinner than this conductor asks for is not a cluster it can keep. What it
    // inherited is the leaving preset's chord, and that can be one note where the arriving preset
    // wants four -- the old one may itself have been a few seconds old. Left at that, the new
    // conductor waits its OWN event rate for the second note, which in this library is a hundred
    // seconds: the change lands on a thin chord and stays there. So it goes on filling from here,
    // and ClusterBrain::update stops it the moment the cluster is full.
    //
    // After the adopt, because adopt() clears the flag itself; and the density is read from the
    // parameter rather than from bp_, which is filled on the audio thread -- this runs on the
    // message thread while the incoming engine is being prepared, before it has rendered a block.
    const bool on = getParam(second ? ParamId::Brain2On : ParamId::BrainOn) >= 0.5f;
    const int want = static_cast<int>(std::lround(getParam(second ? ParamId::Brain2Density : ParamId::BrainDensity)));
    if (on && count < want) (second ? brain2_ : brain_).requestFill();
}

void Engine::process(float* L, float* R, int n)
{
    blocksBegun_.fetch_add(1, std::memory_order_acq_rel);
#if AMBIENT_HAS_MXCSR
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr(savedCsr | 0x8040);   // flush-to-zero + denormals-are-zero
#endif
#if AMBIENT_HAS_FPCR
    // The same thing on ARM: FPCR bit 24 is flush-to-zero. Without it the Quest ran every decaying
    // reverb tail and envelope into denormals, where the FPU falls off a cliff.
    uint64_t savedFpcr = 0;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(savedFpcr));
    const uint64_t fpcrFz = savedFpcr | (1ull << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcrFz));
#endif
    // Pending user scale / wavetable (written from the message thread).
    const int uv = userVersion_.load(std::memory_order_acquire);
    if (uv != userSeen_) {
        userBusy_.store(true, std::memory_order_release);
        scales_[kUserScaleIndex] = userPending_;
        userBusy_.store(false, std::memory_order_release);
        userSeen_ = uv;
    }
    const int tv = tableVersion_.load(std::memory_order_acquire);
    if (tv != tableSeen_) {
        tableBusy_.store(true, std::memory_order_release);
        userTable_ = userTablePending_;
        tableBusy_.store(false, std::memory_order_release);
        tableSeen_ = tv;
    }
    arc_.update(static_cast<float>(n / sr_), 1.0f / (60.0f * std::max(arcPeriodMin_, 0.5f)), rng_);
    // The arc, as the engine reads it. Off, it is the drifter's own value, assigned rather than
    // followed, so that nothing about the existing presets moves by a bit. On, it is the clock's:
    // the generative apps' idea (Eno's Reflection, Endel) that a piece which runs all night should
    // know what time it is, so that its dawn and the real one coincide. The hour is asked for once
    // a second, which is a hundred times more often than it changes anything. Switching between
    // the two glides over twenty seconds instead of jumping, because the arc leans on density and
    // brightness and a jump there is a jump in the sound.
    {
        const float dt = static_cast<float>(n / sr_);
        if (arcClock_ && clockHourOverride_ >= 0.0) {
            clockHour_ = clockHourOverride_;
        } else if (arcClock_) {
            if (++clockCheck_ >= static_cast<int>(sr_ / std::max(1, n))) {
                clockCheck_ = 0;
                const std::time_t t = std::time(nullptr);
                std::tm lt {};
#if defined(_WIN32)
                localtime_s(&lt, &t);
#else
                localtime_r(&t, &lt);
#endif
                clockHour_ = lt.tm_hour + lt.tm_min / 60.0 + lt.tm_sec / 3600.0;
            }
        }
        const float target = arcClock_ ? clockArcValue(clockHour_) : arc_.value();
        if (arcClock_ != arcClockWas_) { arcClockWas_ = arcClock_; arcGlide_ = 20.0f; }
        if (arcGlide_ > 0.0f) {
            arcGlide_ = std::max(0.0f, arcGlide_ - dt);
            arcOut_ += (target - arcOut_) * (1.0f - std::exp(-dt / 5.0f));
        } else arcOut_ = target;
    }
    if (tide_ > 0.0f) tideDrift_.update(static_cast<float>(n / sr_), 1.0f / (60.0f * std::max(tidePeriod_, 0.5f)), auxRng_);
    if (farRotate_ > 0.0f) rotDrift_.update(static_cast<float>(n / sr_), 0.008f, auxRng_);
    arcValue_.store(arcOut_ * arcAmount_, std::memory_order_relaxed);
    shiftDrift_.update(static_cast<float>(n / sr_), 0.03f, rng_);
    purityDrift_.update(static_cast<float>(n / sr_), getParam(ParamId::TuneDriftRate), rng_);
    lastBlockSeconds_ = static_cast<float>(n / sr_);
    {   // Kuramoto bank: dθ_i = ω_i + K/N Σ sin(θ_j − θ_i). Natural periods 23/31/41/53 s over Rate;
        // K up to 0.6 rad/s locks them (the spread of ω is 0.15 rad/s), 0 leaves them independent.
        const float rate = std::max(getParam(ParamId::CoherenceRate), 0.05f);
        const float K = 0.6f * getParam(ParamId::Coherence) * rate;   // scales with the rate, so the lock is the same at every tempo
        const float dt = static_cast<float>(n / sr_);
        const float periods[4] = { 23.0f, 31.0f, 41.0f, 53.0f };
        float dth[4];
        // The coupling is deliberately NOT symmetric. With every pair pulling on the other
        // equally, a high Coherence settles into exact synchrony and stays there: four
        // oscillators behaving as one, which is the opposite of what this section is for. An
        // antisymmetric perturbation of the weights -- each oscillator pulled a little harder by
        // the one behind it in the ring than by the one in front -- has no such fixed point, so
        // the bank locks in frequency and keeps a slowly turning spread of phase. That is what a
        // ring of coupled biological oscillators does, and it is why they never look identical.
        // The weights depend on j - i and nothing else, so they are four numbers, not sixteen
        // sines a block. And with Coherence at zero the coupling term is multiplied by zero:
        // the sixteen sines of the phase differences are computed for nothing at all.
        static const float kW[4] = { 1.0f, 1.0f + 0.22f, 1.0f, 1.0f - 0.22f };   // sin(2 pi k / 4) = 0, 1, 0, -1
        if (K == 0.0f) {
            for (int i = 0; i < 4; ++i) dth[i] = kTwoPi * rate / periods[i];
        } else {
            for (int i = 0; i < 4; ++i) {
                float coupling = 0.0f;
                for (int j = 0; j < 4; ++j)
                    coupling += kW[((j - i) & 3)] * std::sin(kuraPhase_[j] - kuraPhase_[i]);
                dth[i] = kTwoPi * rate / periods[i] + K * coupling * 0.25f;
            }
        }
        for (int i = 0; i < 4; ++i) { kuraPhase_[i] += dth[i] * dt; if (kuraPhase_[i] > kTwoPi) kuraPhase_[i] -= kTwoPi; if (kuraPhase_[i] < 0.0f) kuraPhase_[i] += kTwoPi; }
    }
    {   // morph position glides toward its target at 1/glide per second (glide 0 = jump)
        const float target = clampv(getParam(ParamId::MorphPos), 0.0f, 1.0f);
        const float glide = getParam(ParamId::MorphGlide);
        float cur = morphCur_.load(std::memory_order_relaxed);
        if (glide <= 0.001f) cur = target;
        else {
            const float step = static_cast<float>(n / sr_) / glide;
            cur += clampv(target - cur, -step, step);
        }
        morphCur_.store(cur, std::memory_order_relaxed);
    }
    updateBlend(n);
    stepClock(n / sr_);
    stepModulation(static_cast<float>(n / sr_));
    readParams();
    // Transpose glides: an octave in two seconds, whichever way, and stops exactly on the ratio.
    if (transposeCur_ != transposeTarget_) {
        const double step = 0.5 * (n / sr_);
        transposeCur_ += clampv(transposeTarget_ - transposeCur_, -step, step);
    }
    // The comma's way home: the shared offset moves towards minus the mean of the sounding
    // notes' own offsets, three cents a minute, so the ensemble's centre returns to the
    // reference while every interval inside it stays pure. With nothing sounding, or the
    // knob at zero, it returns to zero the same slow way.
    if (adaptAmt_ > 0.0f || commaCents_ != 0.0) {
        double sum = 0.0; int count = 0;
        if (adaptAmt_ > 0.0f)
            for (const auto& v : voices_) if (v.isActive()) { sum += adaptCents_[v.note()]; ++count; }
        commaTarget_ = count > 0 ? -sum / count : 0.0;
        const double step = (3.0 / 60.0) * (n / sr_);
        commaCents_ += clampv(commaTarget_ - commaCents_, -step, step);
        if (std::fabs(commaCents_) < 1.0e-6 && commaTarget_ == 0.0) commaCents_ = 0.0;
    }
    if (retune_)   // tuning purity / drift: every sounding voice glides to its current frequency
        for (auto& v : voices_) if (v.isActive()) v.setTargetFrequency(frequencyOf(v.note()) * v.rootComp());

    int pos = 0;
    while (pos < n) {
        const int chunk = std::min(n - pos, maxBlock_);
        stemPos_ = pos;
        renderChunk(L + pos, R + pos, chunk);
        pos += chunk;
    }
    // Sleep: nothing sounding and the tails gone for two seconds -> the next blocks skip the
    // effect chain (zeros out) until a voice starts. The brain keeps running inside renderChunk,
    // so a generative patch wakes itself; MIDI and OSC notes wake it through the voices.
    {
        bool anyVoice = false;
        for (auto& v : voices_) if (v.isActive()) { anyVoice = true; break; }
        float peak = 0.0f;
        for (int i = 0; i < n; ++i) peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        if (!anyVoice && peak < 3.2e-5f) silentSamples_ = std::min<long long>(silentSamples_ + n, 1LL << 40); else silentSamples_ = 0;
        asleep_ = silentSamples_ > static_cast<long long>(2.0 * sr_);
    }

    uint64_t m0 = 0, m1 = 0;
    int active = 0;
    for (auto& d : noteDistance_) d.store(-1.0f, std::memory_order_relaxed);
    for (auto& l : noteLevel_) l.store(0.0f, std::memory_order_relaxed);
    for (auto& v : voices_) {
        if (!v.isActive()) continue;
        ++active;
        const int nt = v.note();
        if (nt >= 0 && nt < 64) m0 |= (1ull << nt);
        else if (nt >= 64 && nt < 128) m1 |= (1ull << (nt - 64));
        if (nt >= 0 && nt < 128) {
            noteDistance_[nt].store(v.distance(), std::memory_order_relaxed);
            if (v.level() > noteLevel_[nt].load(std::memory_order_relaxed)) noteLevel_[nt].store(v.level(), std::memory_order_relaxed);
        }
    }
    // Done: whatever this block picked up, it has let go of. See Engine::waitForQuiet.
    blocksDone_.fetch_add(1, std::memory_order_release);
    mask_[0].store(m0, std::memory_order_relaxed);
    mask_[1].store(m1, std::memory_order_relaxed);
    activeVoices_.store(active, std::memory_order_relaxed);
    brainRoot_.store(brain_.root(), std::memory_order_relaxed);
#if AMBIENT_HAS_MXCSR
    _mm_setcsr(savedCsr);
#endif
#if AMBIENT_HAS_FPCR
    __asm__ __volatile__("msr fpcr, %0" : : "r"(savedFpcr));
#endif
}

void Engine::renderChunk(float* L, float* R, int n)
{
    float* nl = nearL_.data(); float* nr = nearR_.data();
    float* fl = farL_.data();  float* fr = farR_.data();
    float* wl = wetL_.data();  float* wr = wetR_.data();
    const size_t bytes = sizeof(float) * static_cast<size_t>(n);
    std::memset(nl, 0, bytes); std::memset(nr, 0, bytes);
    std::memset(fl, 0, bytes); std::memset(fr, 0, bytes);
    float* dl = dryL_.data(); float* dr = dryR_.data();
    std::memset(dl, 0, bytes); std::memset(dr, 0, bytes);
    // The foreground's sends, gathered per event below and handed to the two effects where they
    // take their input. Cleared only while one of them is open, so a preset without them pays
    // nothing at all.
    const float foreD2 = clampv(np_.toDelay2, 0.0f, 1.0f), foreCo = clampv(np_.toCosmos, 0.0f, 1.0f);
    const bool foreSends = foreD2 > 0.0f || foreCo > 0.0f;
    float* s2l = foreD2L_.data(); float* s2r = foreD2R_.data();
    float* scl = foreCoL_.data(); float* scr = foreCoR_.data();
    if (foreSends) {
        std::memset(s2l, 0, bytes); std::memset(s2r, 0, bytes);
        std::memset(scl, 0, bytes); std::memset(scr, 0, bytes);
    }

    int anchor = -1;
    for (int i = 0; i < 128; ++i) if (midiHeld_[i]) { anchor = i; break; }

    auto freqOf = [this](int note) { return frequencyOf(note); };
    auto emit = [this](const BrainEvent& e) {
        if (e.type == BrainEvent::Type::NoteOn) {
            // Rich's contrast: some notes intimately close, most of them deep in the background.
            const float u = rng_.uniform();
            float d = (u < 0.4f) ? depth_ * 0.15f * rng_.uniform()
                                 : depth_ * (0.55f + 0.45f * rng_.uniform());
            // Layer Depth takes that away from the dice and gives it to the role (R8.4): the
            // foundation and the body stand near, the colour in the middle, the air far back. The
            // dice keep whatever share of it Layer Depth leaves them.
            if (layerDepth_ > 0.0f) {
                const int n = e.note;
                const float byRole = depth_ * (n < 43 ? 0.08f : n < 60 ? 0.22f : n < 84 ? 0.55f : 0.9f);
                d += (byRole - d) * layerDepth_;
            }
            startNote(e.note, e.velocity, OwnerBrain, clampv(d, 0.0f, 1.0f));
        } else {
            stopNote(e.note, OwnerBrain);
        }
    };
    // The second conductor plays on one plane, deep, so it reads as a background line rather
    // than as more of the same cluster.
    auto emit2 = [this](const BrainEvent& e) {
        if (e.type == BrainEvent::Type::NoteOn) startNote(e.note, e.velocity, OwnerBrain2, clampv(depth_ * brain2Depth_, 0.0f, 1.0f));
        else stopNote(e.note, OwnerBrain2);
    };

    // Feedback loop, input side: what the previous chunk wrote into the ring comes back as
    // phase modulation of the partials and/or as signal into the near bus (before ensemble,
    // delays and reverbs -- "after the reverb back before the filter").
    const bool fbOn = fbBus_ > 0.0f || fbFm_ > 0.0f;
    float* fbl = fbInL_.data(); float* fbr = fbInR_.data(); float* fbm = fbMono_.data();
    if (fbOn) {
        // The level throttle applies to the bus path only (that one adds energy and can run
        // away); phase modulation moves energy between partials without adding any, so it
        // gets the saturated signal unthrottled. The throttle ramps across the chunk.
        const float regTarget = clampv((0.1f - fbEnv_) / 0.1f, 0.0f, 1.0f);
        const float regStep = (regTarget - fbReg_) / static_cast<float>(n);
        // Tape: wow (slow, irregular, up to 3 ms) and flutter (6 Hz, up to 0.3 ms) move the read
        // position -- a fractional delay in the loop, so every pass through the loop smears a
        // little more, the way a tape loop goes soft with each generation.
        const float wowSamples = fbTape_ > 0.0f ? (0.003f * tapeWow_.update(static_cast<float>(n / sr_), 0.3f, rng_) * 0.5f + 0.0015f) * fbTape_ * static_cast<float>(sr_) : 0.0f;
        const double flutterInc = 6.0 / sr_;
        for (int i = 0; i < n; ++i) {
            float l, r;
            if (fbTape_ > 0.0f) {
                tapeFlutterPhase_ += flutterInc; if (tapeFlutterPhase_ >= 1.0) tapeFlutterPhase_ -= 1.0;
                const float delay = wowSamples + 0.0003f * fbTape_ * static_cast<float>(sr_) * (0.5f + 0.5f * sin01(tapeFlutterPhase_));
                const int di = static_cast<int>(delay); const float fr = delay - static_cast<float>(di);
                const int i0 = (fbW_ - n + i - di) & fbMask_, i1 = (i0 - 1) & fbMask_;
                l = fbRingL_[static_cast<size_t>(i0)] + fr * (fbRingL_[static_cast<size_t>(i1)] - fbRingL_[static_cast<size_t>(i0)]);
                r = fbRingR_[static_cast<size_t>(i0)] + fr * (fbRingR_[static_cast<size_t>(i1)] - fbRingR_[static_cast<size_t>(i0)]);
            } else {
                const int idx = (fbW_ - n + i) & fbMask_;
                l = fbRingL_[static_cast<size_t>(idx)]; r = fbRingR_[static_cast<size_t>(idx)];
            }
            const float reg = fbReg_ + regStep * static_cast<float>(i + 1);
            fbl[i] = l * reg;
            fbr[i] = r * reg;
            fbm[i] = 0.5f * (l + r);
        }
        fbReg_ = regTarget;
    }

    bool anyVoice = false;
    for (int p = 0; p < n; p += kControlBlock) {
        const int len = std::min(kControlBlock, n - p);
        {   // Quantize: the decisions are held back and made at the next note value of the clock,
            // so the conductor lands on the grid instead of wherever the dice fell. All of the
            // waiting time is handed over at the tick, so the mean rate is unchanged.
            const double dt = len / sr_;
            // The foreground's asks are set inside stepNear: while a near Note or a Phrase speaks
            // the conductor begins no new note (its holds and releases run on), and under a
            // sequence it keeps its root and takes twice as long between events.
            const BrainParams* bpNow = &bp_;
            BrainParams bpSlow;
            if (near_.sequenceRunning()) { bpSlow = bp_; bpSlow.rateSeconds *= 2.0f; bpNow = &bpSlow; }
            if (syncOn(brainQuant_) && running_) {
                quantAcc_ += dt;
                const double b = syncBeats(brainQuant_);
                const double now = std::floor(beat_ / b), before = std::floor(lastBeat_ / b);
                lastBeat_ = beat_;
                if (now != before) { brain_.update(quantAcc_, *bpNow, anchor, freqOf, emit); quantAcc_ = 0.0; }
            } else {
                quantAcc_ = 0.0; lastBeat_ = beat_;
                brain_.update(dt, *bpNow, anchor, freqOf, emit);
            }
            if (brain2On_) {
                brain2_.setRoot(clampv(brain_.root() + brain2Interval_, 0, 127));
                brain2_.update(dt, bp2_, -1, freqOf, emit2);
            }
            stepNear(dt, [this](const NearNote& e) { nearEmit(e); });
        }
        const float* fm = (fbOn && fbFm_ > 0.0f) ? fbm + p : nullptr;
        const float* couple = sympathy_ > 0.0f ? coupleBuf_.data() + p : nullptr;
        const float dry = clampv(np_.dry, 0.0f, 1.0f);
        for (auto& v : voices_) {
            if (!(v.isActive() || v.isStriking())) continue;
            anyVoice = true;
            if (v.owner() == OwnerNear && (dry > 0.0f || foreSends)) {
                // The near layer's Dry share: the voice is rendered apart, and that share of both
                // its planes goes to the dry bus -- added to the output after every reverb and
                // delay -- while the rest takes the usual way through the planes. The two sends
                // are taken from the same rendering: a share of the event, ADDED to what the
                // second delay and the Cosmos already hear on the near bus, the way an aux send
                // on a desk adds rather than diverts. The event stays where Distance put it.
                float tnl[kControlBlock], tnr[kControlBlock], tfl[kControlBlock], tfr[kControlBlock];
                std::memset(tnl, 0, sizeof(float) * static_cast<size_t>(len)); std::memset(tnr, 0, sizeof(float) * static_cast<size_t>(len));
                std::memset(tfl, 0, sizeof(float) * static_cast<size_t>(len)); std::memset(tfr, 0, sizeof(float) * static_cast<size_t>(len));
                v.render(tnl, tnr, tfl, tfr, len, vpNear_, fm, couple);
                const float wet = 1.0f - dry;
                for (int i = 0; i < len; ++i) {
                    nl[p + i] += tnl[i] * wet; nr[p + i] += tnr[i] * wet;
                    fl[p + i] += tfl[i] * wet; fr[p + i] += tfr[i] * wet;
                    dl[p + i] += (tnl[i] + tfl[i]) * dry; dr[p + i] += (tnr[i] + tfr[i]) * dry;
                }
                if (foreSends)
                    for (int i = 0; i < len; ++i) {
                        const float sL = tnl[i] + tfl[i], sR = tnr[i] + tfr[i];
                        s2l[p + i] += sL * foreD2; s2r[p + i] += sR * foreD2;
                        scl[p + i] += sL * foreCo; scr[p + i] += sR * foreCo;
                    }
            } else {
                v.render(nl + p, nr + p, fl + p, fr + p, len, v.owner() == OwnerNear ? vpNear_ : vp_, fm, couple);
            }
        }
    }
    if (asleep_ && !anyVoice) {   // sleeping: the whole effect chain is skipped, output stays silent
        std::memset(L, 0, bytes); std::memset(R, 0, bytes);
        // The stems too. They are the host's buffers (multi-out) or the render tool's, and
        // leaving them alone means whatever was in them last is heard again -- silence in the
        // mix and a burst of the previous block on every stem.
        if (stems_ != nullptr)
            for (int c = 0; c < kNumStems * 2; ++c)
                std::memset(stems_[c] + stemPos_, 0, bytes);
        return;
    }
    if (fbOn && fbBus_ > 0.0f)
        for (int i = 0; i < n; ++i) { nl[i] += fbl[i] * fbBus_; nr[i] += fbr[i] * fbBus_; }

    // Blur: the near bus through a spectral smear before anything else hears it, so a note's
    // attack is wiped into texture and one note flows into the next.
    if (blurMix_ > 0.0f || smBlur_.value > 1.0e-4f) {
        float* bl = nebL_.data(); float* br = nebR_.data();
        blur_.process(nl, nr, bl, br, n);
        for (int i = 0; i < n; ++i) { const float m = smBlur_.next(blurMix_); nl[i] = nl[i] * (1.0f - m) + bl[i] * m; nr[i] = nr[i] * (1.0f - m) + br[i] * m; }
    }
    // Foreground plane: ensemble, asymmetric delay (echoes partly recede into the far plane), small room.
    ensemble_.process(nl, nr, n);
    delay_.process(nl, nr, wl, wr, n);
    for (int i = 0; i < n; ++i) {
        const float m = smDelayMix_.next(delayMix_), tf = smDelayToFar_.next(delayToFar_);
        nl[i] += wl[i] * m;  nr[i] += wr[i] * m;
        fl[i] += wl[i] * tf; fr[i] += wr[i] * tf;
    }
    // Second delay in series: it hears the first delay's echoes and spins longer chains -- and,
    // with To Delay 2 open, an extra share of every foreground event straight into its input. Into
    // the INPUT and not onto the plane: on the plane the event would go through everything else a
    // second time; here it is the delay that gets more of it, which is what a send is.
    if (foreD2 > 0.0f) {
        for (int i = 0; i < n; ++i) { s2l[i] += nl[i]; s2r[i] += nr[i]; }   // the send buffer is the delay's input from here
        delay2_.process(s2l, s2r, wl, wr, n);
    } else delay2_.process(nl, nr, wl, wr, n);
    for (int i = 0; i < n; ++i) {
        const float m = smDelay2Mix_.next(delay2Mix_), tf = smDelay2ToFar_.next(delay2ToFar_);
        nl[i] += wl[i] * m;  nr[i] += wr[i] * m;
        fl[i] += wl[i] * tf; fr[i] += wr[i] * tf;
    }
    // Granular cloud on the far plane: grains of the foreground, scattered and transposed,
    // dropped into the far reverb.
    // Kept running while it still sounds with the send closed: its loop and its resonators ring on,
    // and a cloud stopped mid-grain would start again from there the next time it was opened.
    if (cloudSend_ > 0.0f || smCloudSend_.value > 1e-4f || !cloud_.quiet()) {
        float* cl = cosL_.data(); float* cr = cosR_.data();
        for (int i = 0; i < n; ++i) { const float s = smCloudSend_.next(cloudSend_); cl[i] = nl[i] * s; cr[i] = nr[i] * s; }
        if (cloudToNear_ <= 0.0f) {
            cloud_.process(cl, cr, fl, fr, n);
        } else {
            // To Near: the cloud is written into its own pair and then shared between the planes,
            // equal power, so turning it forward does not change how much of it there is. At 0 the
            // branch above runs and the render is what it always was, sample for sample.
            std::fill(cloudL_.begin(), cloudL_.begin() + n, 0.0f);
            std::fill(cloudR_.begin(), cloudR_.begin() + n, 0.0f);
            cloud_.process(cl, cr, cloudL_.data(), cloudR_.data(), n);
            const float a = 0.5f * kPi * clampv(cloudToNear_, 0.0f, 1.0f);
            const float near = std::sin(a), far = std::cos(a);
            for (int i = 0; i < n; ++i) {
                fl[i] += cloudL_[static_cast<size_t>(i)] * far;
                fr[i] += cloudR_[static_cast<size_t>(i)] * far;
                nl[i] += cloudL_[static_cast<size_t>(i)] * near;
                nr[i] += cloudR_[static_cast<size_t>(i)] * near;
            }
        }
    }

    // Cosmos: a parallel send off the near bus, returned to both planes; the dry path is untouched.
    // Swell: the send follows the conductor's cascade, above its own average and below it, so the
    // Cosmos gathers where the events gather and thins out in the long gaps instead of sitting
    // there at one level all night. Measured against the average and not the excitation itself,
    // which is what leaves a piece without a cascade (excitation 0, average 0) exactly as it was.
    // The deviation is taken relative to that average, not as an absolute number: a piece whose
    // cascade only ever reaches a fifth swells as much as one that runs hot, and one with no
    // cascade at all (both zero) is left exactly alone. The floor of 0.15 keeps the division sane.
    const float cosmosSendNow = cosmosSend_
        * std::clamp(1.0f + 3.0f * cosmosSwell_ * (cascadeNow_ - cascadeAvg_) / std::max(cascadeAvg_, 0.15f), 0.0f, 2.0f);
    if (cosmosSendNow > 0.0f || smCosmosSend_.value > 1e-4f || foreCo > 0.0f) {
        float* cl = cosL_.data(); float* cr = cosR_.data();
        // The plane's own send, and the foreground's on top of it (To Cosmos): a preset may send
        // nothing but its events into the deep space and leave the bed out of it entirely.
        for (int i = 0; i < n; ++i) { const float s = smCosmosSend_.next(cosmosSendNow); cl[i] = nl[i] * s + scl[i]; cr[i] = nr[i] * s + scr[i]; }
        shifter_.process(cl, cr, n);
        resonator_.process(cl, cr, n);
        vowel_.process(cl, cr, n);
        if (cosmosNebula_ > 0.0f) {
            float* bl = nebL_.data(); float* br = nebR_.data();
            nebula_.process(cl, cr, bl, br, n);
            const float m = cosmosNebula_;
            for (int i = 0; i < n; ++i) { cl[i] = cl[i] * (1.0f - m) + bl[i] * m; cr[i] = cr[i] * (1.0f - m) + br[i] * m; }
        }
        for (int i = 0; i < n; ++i) {
            const float ret = smCosmosReturn_.next(cosmosReturn_), tf = smCosmosToFar_.next(cosmosToFar_);
            nl[i] += cl[i] * ret; nr[i] += cr[i] * ret;
            fl[i] += cl[i] * tf;  fr[i] += cr[i] * tf;
            cosTap_[(cosTapW_ + i) & 4095] = 0.5f * (cl[i] + cr[i]) * ret;   // what the return adds, for the picture
            if (stems_ != nullptr) { stems_[4][stemPos_ + i] = cl[i] * ret; stems_[5][stemPos_ + i] = cr[i] * ret; }
        }
        cosTapW_ = (cosTapW_ + n) & 4095;
    } else {
        for (int i = 0; i < n; ++i) cosTap_[(cosTapW_ + i) & 4095] = 0.0f;
        cosTapW_ = (cosTapW_ + n) & 4095;
        if (stems_ != nullptr) for (int i = 0; i < n; ++i) { stems_[4][stemPos_ + i] = 0.0f; stems_[5][stemPos_ + i] = 0.0f; }
    }
    // Memory: a second parallel world off the near bus, returned to both planes like the Cosmos and
    // counted into the Cosmos stem, where the parallel paths are heard apart from the dry one. It keeps
    // running with the send closed for as long as it still holds anything.
    if (memSend_ > 0.0f || smMemSend_.value > 1e-4f || !memory_.quiet()) {
        float* ml = memL_.data(); float* mr = memR_.data();
        for (int i = 0; i < n; ++i) { const float s = smMemSend_.next(memSend_); ml[i] = nl[i] * s; mr[i] = nr[i] * s; }
        memory_.process(ml, mr, n);
        for (int i = 0; i < n; ++i) {
            const float ret = smMemReturn_.next(memReturn_), tf = smMemToFar_.next(memToFar_);
            nl[i] += ml[i] * ret; nr[i] += mr[i] * ret;
            fl[i] += ml[i] * tf;  fr[i] += mr[i] * tf;
            if (stems_ != nullptr) { stems_[4][stemPos_ + i] += ml[i] * ret; stems_[5][stemPos_ + i] += mr[i] * ret; }
        }
    }
    nearReverb_.process(nl, nr, n);

    // Background plane: 100 % wet, dark, wide. Shimmer feeds the pitch-shifted previous
    // block of the far reverb back into its input (the classic rising cloud).
    float* sl = shimL_.data(); float* sr = shimR_.data();
    if (cosmosShimmer_ > 0.0f)
        for (int i = 0; i < n; ++i) { fl[i] += sl[i]; fr[i] += sr[i]; }
    // Room: the convolution reverb hears the far sends (before the FDN) or the finished
    // near bus, through a pre-delay; it keeps running for one impulse length after its
    // level reaches zero so the tail can finish, and costs nothing while silent.
    const bool roomOn = roomLevel_ > 0.0005f || roomLevelCur_ > 0.0005f || roomTailLeft_ > 0;
    float* rl = roomInL_.data(); float* rr = roomInR_.data();
    if (roomOn) {
        const float* srcL = roomSource_ == 0 ? fl : nl; const float* srcR = roomSource_ == 0 ? fr : nr;
        // The convolution's own latency is taken out of the pre-delay rather than added to it, so
        // Pre-Delay means what it says down to the latency itself (5 ms).
        const int preDelay = std::max(0, roomPreDelay_ - room_.latency());
        for (int i = 0; i < n; ++i) {
            roomDelayL_[static_cast<size_t>(roomDelayW_)] = srcL[i]; roomDelayR_[static_cast<size_t>(roomDelayW_)] = srcR[i];
            const int rd = (roomDelayW_ - preDelay) & roomDelayMask_;
            rl[i] = roomDelayL_[static_cast<size_t>(rd)]; rr[i] = roomDelayR_[static_cast<size_t>(rd)];
            roomDelayW_ = (roomDelayW_ + 1) & roomDelayMask_;
            if (sendLowcut_ > 21.0f) {   // Send Low Cut: the room, like the two halls, is never fed under it
                float lp, bp, hp;
                roomSendHpL_.tick(rl[i], lp, bp, hp); rl[i] = hp;
                roomSendHpR_.tick(rr[i], lp, bp, hp); rr[i] = hp;
            }
        }
        roomTailLeft_ = roomLevel_ > 0.0005f ? room_.tailSamples() : std::max(0L, roomTailLeft_ - n);
    }
    // The room's answer is made here, ahead of the far hall, rather than after it (25.09.2026): the
    // production guide's rooms are serial -- the main room's output feeds the far room at ten to
    // twenty per cent, so the horizon sounds like the same place going on and not like a second,
    // foreign one (Lustmord's "endless" rooms). Its input was taken above either way, so moving the
    // convolution up changes nothing about what the room itself hears. The return goes into the mix
    // further down, ducked under the near bus like the far hall.
    float* ol = roomOutL_.data(); float* orr = roomOutR_.data();
    if (roomOn) {
        // Morph: the second impulse is blended into the first inside the convolution, which is the
        // same as fading from one room's answer to the other's and costs one room. The smoother
        // moves a block at a time; each stage of the convolver takes its value as its blocks begin.
        smRoomMorph_.value = roomMorph_ + (smRoomMorph_.value - roomMorph_) * std::pow(1.0f - smRoomMorph_.coef, static_cast<float>(n));
        if (std::fabs(smRoomMorph_.value - roomMorph_) < 1.0e-4f) smRoomMorph_.value = roomMorph_;
        room_.setMorph(smRoomMorph_.value);
        room_.process(rl, rr, ol, orr, n);
        const float lpc = 1.0f - std::exp(-kTwoPi * roomHighcut_ / static_cast<float>(sr_));
        const float rhpc = roomLowcut_ <= 21.0f ? 0.0f
                         : 1.0f - std::exp(-kTwoPi * (roomLowcut_ / 1.5538f) / static_cast<float>(sr_));
        const float levelC = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(sr_)));
        // An energy-normalised impulse returns the far sends at their own power, which is far
        // louder than the FDN's output at Far Level 1; 0.35 puts Room Level 1 in the same league
        // (measured: -15.8 dBFS raw vs. -23 dBFS for the FDN on the default patch).
        const float roomGain = 0.35f;
        for (int i = 0; i < n; ++i) {
            roomLevelCur_ += (roomLevel_ - roomLevelCur_) * levelC;
            roomLpL_ += lpc * (ol[i] - roomLpL_);
            roomLpR_ += lpc * (orr[i] - roomLpR_);
            float tL = roomLpL_, tR = roomLpR_;
            if (rhpc > 0.0f) {   // the low end out of the room, 12 dB/oct
                roomHpL1_ += rhpc * (tL - roomHpL1_);  const float aL = tL - roomHpL1_;
                roomHpR1_ += rhpc * (tR - roomHpR1_);  const float aR = tR - roomHpR1_;
                roomHpL2_ += rhpc * (aL - roomHpL2_);  tL = aL - roomHpL2_;
                roomHpR2_ += rhpc * (aR - roomHpR2_);  tR = aR - roomHpR2_;
            }
            ol[i] = tL * roomLevelCur_ * roomGain;
            orr[i] = tR * roomLevelCur_ * roomGain;
            // To Far: the room's return, before it is ducked, into the far hall's input.
            const float tf = smRoomToFar_.next(roomToFar_);
            fl[i] += ol[i] * tf;
            fr[i] += orr[i] * tf;
        }
    } else {
        roomLpL_ = roomLpR_ = 0.0f;
        smRoomToFar_.snap(roomToFar_);
    }
    diffuser_.process(fl, fr, n);
    {
        // Air saturates. A real room is not linear at a peak -- the medium itself gives a little,
        // and a dense cluster fired into a hall comes back thickened rather than reflected. This
        // is a very gentle asymmetric shaper between the diffuser and the reverb's own feedback
        // network, so only the peaks are touched and the tail is what changes character, not the
        // level. Amount rides on Diffuse, which is already the "how much room" control, so there
        // is no new knob for a thing nobody would know how to set. At four times the rate since
        // 25.09.2026 (the guide's "4x on everything nonlinear"); the far send is only ever heard
        // through the hall, so the 0.6 ms the oversampling takes is a pre-delay nobody measures.
        const float amt = 0.35f * farDiffuse_;
        if (amt > 0.001f) {
            if (!airOsOn_) { airOsL_.reset(); airOsR_.reset(); airOsOn_ = true; }
            const float pre = 1.0f + 2.2f * amt, post = 1.0f / (1.0f + 0.55f * amt);
            // Asymmetric on purpose: a touch of second harmonic reads as warmth, where the
            // symmetric curve of a plain tanh only ever reads as compression.
            auto air = [pre, post](float x) {
                const float a = x * pre, t = std::tanh(a);
                return (t + 0.06f * a * a * (a > 0.0f ? 1.0f : -1.0f) * (1.0f - std::fabs(t))) * post;
            };
            for (int i = 0; i < n; ++i) {
                fl[i] = airOsL_.process(fl[i], air);
                fr[i] = airOsR_.process(fr[i], air);
            }
        } else airOsOn_ = false;
    }
    farReverb_.process(fl, fr, n);
    if (farRotate_ > 0.0f) {   // the background slowly turns: left and right rotate into each other
        const float a = rotDrift_.value() * farRotate_ * 0.6f, c = std::cos(a), sn = std::sin(a);
        for (int i = 0; i < n; ++i) { const float l = fl[i], r = fr[i]; fl[i] = l * c - r * sn; fr[i] = l * sn + r * c; }
    }
    if (cosmosShimmer_ > 0.0f) {
        // Spectral: phase-locked peak shifting (Shifter.h), as clean on the hundredth pass as on the
        // first. Grain: the two-head shifter the shimmer always had, flutter and all. The spectral
        // one starts empty each time the shimmer opens, rather than with what it heard last time.
        if (shimmerMode_ == 0) {
            if (!shimmerWasOn_) shimmerSpec_.reset();
            shimmerSpec_.process(fl, fr, sl, sr, n);
        } else {
            shimmerL_.process(fl, sl, n);
            shimmerR_.process(fr, sr, n);
        }
        shimmerWasOn_ = true;
        const float lpc = 1.0f - std::exp(-kTwoPi * 4000.0f / static_cast<float>(sr_));
        const float amt = cosmosShimmer_ * 0.5f;
        // Self-regulating loop: the feedback is throttled by the level of the far reverb
        // itself (the reverb integrates every injection), so the cloud blooms up to a fixed
        // ceiling and holds there instead of running into the clipper.
        const float envC = 1.0f - std::exp(-1.0f / (0.1f * static_cast<float>(sr_)));
        for (int i = 0; i < n; ++i) {
            shimmerLpL_ += lpc * (sl[i] - shimmerLpL_);
            shimmerLpR_ += lpc * (sr[i] - shimmerLpR_);
            const float mag = 0.5f * (std::fabs(fl[i]) + std::fabs(fr[i]));
            shimmerEnv_ += envC * (mag - shimmerEnv_);
            const float reg = clampv((0.12f - shimmerEnv_) / 0.12f, 0.0f, 1.0f);   // 1 when quiet, 0 at the ceiling
            sl[i] = shimmerLpL_ * amt * reg;
            sr[i] = shimmerLpR_ * amt * reg;
        }
    } else {
        shimmerLpL_ = shimmerLpR_ = 0.0f;
        shimmerEnv_ = 0.0f;
        shimmerWasOn_ = false;
        std::memset(sl, 0, bytes); std::memset(sr, 0, bytes);
    }

    // Unmasking: the background gives way to the foreground band by band, before the two planes
    // are summed (the near bus is the side chain, and it is finished by now).
    unmask_.process(nl, nr, fl, fr, n);
    // Envelopment. Spaciousness is two things, and the reverb's width is only one of them: the
    // apparent width of the source. The other is the sense of being inside the room, and Bradley
    // and Soulodre (1995) found it in late lateral energy at LOW frequencies -- below about
    // 500 Hz, where this instrument's background has little side, because the funnel narrows it
    // and Bass Mono folds it. This lifts the far bus's side channel in the band between the two,
    // by up to six decibels, and leaves the mid exactly alone.
    //
    // With a bell. It used to add the difference of two one-poles, and that difference turns the
    // phase inside the band: added to the side it came out some three and a half decibels above
    // it at best -- 2.8 dB measured, once the halls began to hear both channels and the side they
    // return carried the voice's own partials. At the centre of a bell nothing turns, so full
    // Envelop is the six decibels this has always said, and about four at the band's edges.
    if (envelop_ > 0.0f || smEnvelop_.value > 1.0e-4f) {
        for (int i = 0; i < n; ++i) {
            const float mid = 0.5f * (fl[i] + fr[i]);
            float side = 0.5f * (fl[i] - fr[i]);
            float lp, bp, hp;
            envBell_.tick(side, lp, bp, hp);
            side += smEnvelop_.next(envelop_) * envBell_.k * bp;   // k * bp: unity at the centre, no phase turn
            fl[i] = mid + side; fr[i] = mid - side;
        }
    }
    // Comodulation: the whole background breathes to one random envelope, a new target about
    // nine times a second and a smooth curve between, so every band of it rises and falls
    // together and the ear can hear the foreground through it (comodulation masking release).
    // Its own random stream, so the knob touches nothing else that draws from the engine's.
    if (comod_ > 0.0f || smComod_.value > 1.0e-4f) {
        if (!comodInit_) { comodRng_.seed(0xC0D0ull); comodDrift_.init(comodRng_); comodInit_ = true; }
        const float dt = 1.0f / static_cast<float>(sr_);
        for (int i = 0; i < n; ++i) {
            const float depth = smComod_.next(comod_);
            const float m = comodDrift_.update(dt, 9.0f, comodRng_);      // -1 .. 1
            const float g = 1.0f - depth * 0.5f * (1.0f + m);              // 1 .. 1 - depth
            fl[i] *= g; fr[i] *= g;
        }
    }
    // Mid Low Cut (25.09.2026): the production guide thinks of the far return in mid and side --
    // its sides may be as wide as they like, its middle is high-passed from 300 Hz, so the centre
    // below that belongs to the near plane and the sub alone. Second order, on the middle only;
    // the sides pass untouched, and so does everything with the knob at 20 Hz.
    if (farMidLowcut_ > 21.0f) {
        for (int i = 0; i < n; ++i) {
            const float mid = 0.5f * (fl[i] + fr[i]), side = 0.5f * (fl[i] - fr[i]);
            float lp, bp, hp;
            farMidHp_.tick(mid, lp, bp, hp);
            fl[i] = hp + side;
            fr[i] = hp - side;
        }
    }
    // The Haas band, on the foreground only: the background has the reverb's own width and does
    // not need help. After the unmask, so what the side chain measured is the plane as it was.
    haas_.process(nl, nr, n);
    // The room's early reflections, from the near bus and added to it: they belong in front of
    // the listener with the dry sound, not behind it with the tail.
    if (early_.active()) {
        // The room listens to both channels of the near bus and adds its reflections into the same
        // two buffers, so it listens to copies, in the delay scratch that is free by now.
        std::memcpy(wl, nl, bytes);
        std::memcpy(wr, nr, bytes);
        early_.process(wl, wr, nl, nr, n);
    }
    // Sympathy: this block's foreground is what the voices will hear of each other in the next
    // one. A block of delay is what makes the loop safe, and at these depths inaudible.
    // Bounded: the coupling goes back into the voices before their filters, and sixteen voices
    // through a resonant filter can add up to a loop gain above one. Clamped here the loop can
    // saturate, but it cannot climb to infinity and leave NaN in every filter. Identity below
    // twice full scale, so a healthy mix hears nothing of it.
    if (sympathy_ > 0.0f && static_cast<int>(coupleBuf_.size()) >= n)
        for (int i = 0; i < n; ++i) coupleBuf_[static_cast<size_t>(i)] = clampv(0.5f * (nl[i] + nr[i]), -2.0f, 2.0f);

    const float master = dbToGain(masterGain_);
    for (int i = 0; i < n; ++i) {
        const float far = smFarLevel_.next(farLevel_);
        // The width of the background alone. A mix in which everything is spread as wide as it
        // will go has no depth left: the far plane pulled in towards the centre while the
        // foreground stays wide is the funnel that reads as distance rather than as width. At 1
        // the background is as the reverb made it, which is where every existing preset is.
        const float fw = smFarWidth_.next(farWidth_);
        float bgL = fl[i], bgR = fr[i];
        if (fw != 1.0f) {
            const float mid = 0.5f * (bgL + bgR), side = 0.5f * (bgL - bgR) * fw;
            bgL = mid + side; bgR = mid - side;
        }
        // The near layer's dry share joins here, past everything: it is the foreground's, so it
        // counts on the near stem.
        L[i] = nl[i] + bgL * far + dl[i];
        R[i] = nr[i] + bgR * far + dr[i];
        if (stems_ != nullptr) {
            // The Cosmos return was added into the near bus above, so it has to come out of the
            // near stem or it would be counted twice and the four would no longer sum to the mix.
            // What the Cosmos sent into the far plane cannot be separated here at all -- the
            // reverb has already mixed it with everything else -- and belongs to the far stem,
            // which is where it is heard.
            stems_[0][stemPos_ + i] = nl[i] - stems_[4][stemPos_ + i] + dl[i];
            stems_[1][stemPos_ + i] = nr[i] - stems_[5][stemPos_ + i] + dr[i];
            stems_[2][stemPos_ + i] = bgL * far;   // the far stem as it is heard: width included
            stems_[3][stemPos_ + i] = bgR * far;
        }
    }
    if (roomOn) {
        // The room's return, made above, ducked under the near bus band by band like the far hall
        // (the guide: "the near bus ducks the main room"), and added to the mix.
        roomUnmask_.process(nl, nr, ol, orr, n);
        for (int i = 0; i < n; ++i) {
            L[i] += ol[i];
            R[i] += orr[i];
            if (stems_ != nullptr) { stems_[6][stemPos_ + i] = ol[i]; stems_[7][stemPos_ + i] = orr[i]; }
        }
    } else {
        if (stems_ != nullptr) for (int i = 0; i < n; ++i) { stems_[6][stemPos_ + i] = 0.0f; stems_[7][stemPos_ + i] = 0.0f; }
    }

    // Feedback loop, output side: the mix (before mid/side, sub and master) goes into the ring,
    // low-passed at Tone, driven into a soft saturation (tanh-like, gain-compensated), and
    // throttled by its own mean level like the shimmer loop: feedback -> 0 at a mean level of
    // 0.1 (about -20 dBFS), so a hot loop hisses and holds where a naked one would run into
    // the clipper -- and the loop can thicken a drone without taking it over (a higher
    // ceiling let Distant Storm climb 10 dB and collapse to a correlation of 0.6).
    if (fbOn) {
        if (!fbOsOn_) { fbOsL_.reset(); fbOsR_.reset(); }   // back on: from silence, not from what it heard last time
        const float lpc  = 1.0f - std::exp(-kTwoPi * fbTone_ / static_cast<float>(sr_));
        const float envC = 1.0f - std::exp(-1.0f / (0.05f * static_cast<float>(sr_)));
        const float drive = 1.0f + 9.0f * fbDrive_;
        const float comp  = 1.0f / std::sqrt(drive);
        auto sat = [](float x) {   // rational tanh, exact to 1e-3 up to |x| = 3
            x = clampv(x, -3.0f, 3.0f);
            const float x2 = x * x;
            return x * (27.0f + x2) / (27.0f + 9.0f * x2);
        };
        // DC blocker in the loop: a saturating loop with a DC gain above one locks onto a DC
        // operating point (Feedback Hiss sat at +0.11 before this), so nothing below 10 Hz circulates.
        const float hpc = 1.0f - kTwoPi * 10.0f / static_cast<float>(sr_);
        const float biasLp = 1.0f - std::exp(-kTwoPi * 60.0f / static_cast<float>(sr_));
        const float biasEnv = 1.0f - std::exp(-kTwoPi * 5.0f / static_cast<float>(sr_));
        for (int i = 0; i < n; ++i) {
            fbLpL_ += lpc * (L[i] - fbLpL_);
            fbLpR_ += lpc * (R[i] - fbLpR_);
            float xl = fbLpL_ - fbHpXL_ + hpc * fbHpYL_; fbHpXL_ = fbLpL_; fbHpYL_ = xl;
            float xr = fbLpR_ - fbHpXR_ + hpc * fbHpYR_; fbHpXR_ = fbLpR_; fbHpYR_ = xr;
            float biasL = 0.0f, biasR = 0.0f;   // the operating point's offset, in the curve's own units
            if (fbBias_ > 0.0f) {
                // The operating point: the bass's level, slow, pushed against the curve. The
                // offset goes in before the drive, so what it does scales with Drive as a real
                // stage's would; the DC it makes on the way out is the blocker's to clean up.
                fbBiasLpL_ += biasLp * (xl - fbBiasLpL_);
                fbBiasLpR_ += biasLp * (xr - fbBiasLpR_);
                fbBiasEnvL_ += biasEnv * (std::fabs(fbBiasLpL_) - fbBiasEnvL_);
                fbBiasEnvR_ += biasEnv * (std::fabs(fbBiasLpR_) - fbBiasEnvR_);
                // In the curve's units, after the drive, and never past 1.2: an offset larger than
                // the curve pushes the whole signal off its end and returns a flat line (measured:
                // the loop went from -40 to -87 dB when the offset was let ride on Drive).
                biasL = fbBias_ * 1.2f * fbBiasEnvL_ / (fbBiasEnvL_ + 0.01f);
                biasR = fbBias_ * 1.2f * fbBiasEnvR_ / (fbBiasEnvR_ + 0.01f);
            }
            const float mag = 0.5f * (std::fabs(L[i]) + std::fabs(R[i]));
            fbEnv_ += envC * (mag - fbEnv_);
            const int idx = (fbW_ + i) & fbMask_;
            // Tape: asymmetric saturation (an even-order term the DC blocker cleans up on the next
            // pass) and a noise floor that rises with the level in the loop. The curve runs at four
            // times the rate (25.09.2026): in a loop an alias comes round again on every pass.
            const float asym = fbTape_ > 0.0f ? 0.2f * fbTape_ : 0.0f;
            auto curveL = [&](float v) { return sat((v + asym * v * v) * drive - biasL) * comp; };
            auto curveR = [&](float v) { return sat((v + asym * v * v) * drive - biasR) * comp; };
            fbRingL_[static_cast<size_t>(idx)] = fbOsL_.process(xl, curveL);
            fbRingR_[static_cast<size_t>(idx)] = fbOsR_.process(xr, curveR);
            if (fbTape_ > 0.0f) {
                const float noise = 0.02f * fbTape_ * fbEnv_;
                fbRingL_[static_cast<size_t>(idx)] += noise * rng_.bipolar();
                fbRingR_[static_cast<size_t>(idx)] += noise * rng_.bipolar();
            }
        }
        fbW_ = (fbW_ + n) & fbMask_;
    } else {
        fbLpL_ = fbLpR_ = fbEnv_ = 0.0f;
        fbHpXL_ = fbHpXR_ = fbHpYL_ = fbHpYR_ = 0.0f;
        fbReg_ = 1.0f;
    }
    fbOsOn_ = fbOn;

    // Foundation: a dry sub voice on the brain's root, gliding between roots; the two ears
    // may run a few Hz apart (binaural beat), which Bass Mono leaves alone below its crossover
    // only in the mid channel -- so the beat is kept by feeding it after the mid/side stage.
    float* subL = wl; float* subR = wr;   // the delay scratch buffers are free by now
    const bool subOn = subLevel_ > 0.0f || subLevelCur_ > 1e-4f;
    if (subOn) {
        const double rootHz = frequencyOf(brain_.root());
        double subHz = rootHz / (subOctave_ == 1 ? 2.0 : 4.0);
        if (subSource_ == 2) {
            // Lowest: the Foundation stands under the note that is actually lowest, an octave or
            // two below it, and moves when the chord does.
            //
            // The other two sources do not move with the music, and that is what made the
            // instrument sound as though only its additive presets were being played. Root is a
            // pedal on the conductor's root, and the root wanders only on a draw of 0.35 x Wander
            // per event -- at the library's rates, once every quarter of an hour. Difference does
            // follow the voices, but folds its answer back into a fixed octave anchored to the
            // root, so it tracks the interval and not the pitch: the same chord an octave up came
            // out at exactly the same frequency, measured to the tenth of a hertz.
            //
            // The glide does the rest. It is a one-pole in the log domain and stands at eight
            // seconds in most of the library, so the bass slides to the new chord over a breath
            // instead of stepping to it.
            double lowest = 0.0;
            for (const auto& v : voices_)   // the near events are the foreground, not the chord the sub stands under
                if (v.isActive() && v.owner() != OwnerNear && (lowest <= 0.0 || v.frequency() < lowest)) lowest = v.frequency();
            if (lowest > 0.0) {
                // Folded back into the register the root mode would have used, which is what keeps
                // it a foundation. An octave under the lowest voice and nothing else, a chord up
                // in the fifth octave put the "sub" at 220 Hz -- no longer under the music but in
                // the middle of it, doubling the voice rather than carrying it. Measured: the
                // energy below 130 Hz fell by 23 dB, because the sub had simply left the bass.
                //
                // The fold moves it by octaves only, so the pitch class still follows the chord --
                // which is the whole point, and is what separates this from Difference, where the
                // fold is applied to an interval and the pitch class therefore does not follow.
                // What a bass player does: the root of the chord, in the register of a bass.
                const double lo = subHz * 0.75, hi = lo * 2.0;
                // Both bounds have to be real numbers before anything is folded into them. With
                // lo at zero -- a scale whose degree came out at zero hertz, a root the tuning
                // has no frequency for -- "subHz >= 0" is true for every value including zero,
                // and the fold is an audio thread that never leaves the block again. The window
                // is only meaningful when it has a width, so the fold is skipped without one and
                // the Foundation stays on the root, which is what it did before this mode existed.
                if (lo > 0.0 && std::isfinite(hi)) {
                    subHz = lowest / (subOctave_ == 1 ? 2.0 : 4.0);
                    while (subHz >= hi) subHz *= 0.5;
                    while (subHz < lo)  subHz *= 2.0;
                }
            }
        } else if (subSource_ == 1) {
            // Ghost tone (Rich's combination tones): the difference between the two lowest sounding
            // voices is the tone the ear makes by itself in just intonation (3:2 -> f/2, 5:4 -> f/4,
            // 4:3 -> f/3). The sub doubles it, folded into the register the root mode would use,
            // so switching the source never changes the sub's range. With fewer than two distinct
            // pitches it falls back to the root.
            double f1 = 0.0, f2 = 0.0;
            for (const auto& v : voices_)
                if (v.isActive() && (f1 <= 0.0 || v.frequency() < f1)) f1 = v.frequency();
            if (f1 > 0.0)
                for (const auto& v : voices_)
                    if (v.isActive() && v.frequency() / f1 > 1.003 && (f2 <= 0.0 || v.frequency() < f2)) f2 = v.frequency();
            const double lo = subHz * 0.75, hi = lo * 2.0;   // the octave around the root sub
            if (f1 > 0.0 && f2 > 0.0 && lo > 0.0 && std::isfinite(hi)) {
                double ghost = f2 - f1;                     // for the empty window, see Lowest above
                while (ghost >= hi) ghost *= 0.5;
                while (ghost < lo) ghost *= 2.0;
                subHz = ghost;
            }
        }
        const double target = std::log(std::max(subHz * static_cast<double>(vp_.pitchMul), 10.0));   // the tide moves the sub with the voices
        if (subFreqCur_ <= 0.0) subFreqCur_ = target;
        const double glideC = 1.0 - std::exp(-1.0 / (std::max(subGlide_, 0.05f) * sr_));
        const float levelC = 1.0f - std::exp(-1.0f / (2.0f * static_cast<float>(sr_)));
        // The glide is a one-pole in the log domain, so its end point over this block is known in
        // closed form: take exp twice and walk the frequency linearly, instead of an exp per sample.
        const double reach = 1.0 - std::pow(1.0 - glideC, static_cast<double>(n));
        const double fBegin = std::exp(subFreqCur_);
        const double freqEnd = subFreqCur_ + (target - subFreqCur_) * reach;
        const double fEnd = std::exp(freqEnd);
        const double fStep = n > 0 ? (fEnd - fBegin) / static_cast<double>(n) : 0.0;
        double f = fBegin;
        subFreqCur_ = freqEnd;
        for (int i = 0; i < n; ++i) {
            subLevelCur_ += (subLevel_ - subLevelCur_) * levelC;
            f += fStep;
            const double fL = std::max(f - 0.5 * subBinaural_, 5.0), fR = std::max(f + 0.5 * subBinaural_, 5.0);
            subPhaseL_ += fL / sr_; if (subPhaseL_ >= 1.0) subPhaseL_ -= 1.0;
            subPhaseR_ += fR / sr_; if (subPhaseR_ >= 1.0) subPhaseR_ -= 1.0;
            const float triL = 4.0f * std::fabs(static_cast<float>(subPhaseL_) - 0.5f) - 1.0f;
            const float triR = 4.0f * std::fabs(static_cast<float>(subPhaseR_) - 0.5f) - 1.0f;
            const float g = subLevelCur_ * 0.45f;
            float sL = (1.0f - subTone_) * sin01(subPhaseL_) + subTone_ * triL;
            float sR = (1.0f - subTone_) * sin01(subPhaseR_) + subTone_ * triR;
            if (subHarmonics_ > 0.0f) {
                // The residue (25.09.2026): the second and third harmonic at -24 and -27 dB at
                // full, from the sub's own phase, so they are exact and alias nothing. A speaker
                // that cannot move at the fundamental still gives the ear the note from them.
                const float h2 = 0.0631f * subHarmonics_, h3 = 0.0447f * subHarmonics_;
                double q = subPhaseL_ * 2.0; q -= std::floor(q); sL += h2 * sin01(q);
                q = subPhaseL_ * 3.0; q -= std::floor(q);        sL += h3 * sin01(q);
                q = subPhaseR_ * 2.0; q -= std::floor(q);        sR += h2 * sin01(q);
                q = subPhaseR_ * 3.0; q -= std::floor(q);        sR += h3 * sin01(q);
            }
            if (subBeat_ > 0.0f || subBeatCur_ > 1.0e-4f) {
                // Beat (25.09.2026): a second sine Beat Hz above the sub, the same in both ears, so
                // the pair's level swells and fades once every 1/Beat seconds -- the slow breath
                // of a Lustmord sub, on the basilar membrane rather than between the ears. Half
                // and half, so the pair peaks where the sub alone did; the second sine fades in
                // over the first 0.05 Hz of the knob so turning it from 0 does not step.
                subBeatCur_ += (subBeat_ - subBeatCur_) * levelC;
                subBeatPhase_ += (f + subBeatCur_) / sr_; if (subBeatPhase_ >= 1.0) subBeatPhase_ -= 1.0;
                const float s2 = sin01(subBeatPhase_);
                const float w = 0.5f * std::min(subBeatCur_ / 0.05f, 1.0f);
                sL = sL * (1.0f - w) + s2 * w; sR = sR * (1.0f - w) + s2 * w;
            }
            subL[i] = g * sL; subR[i] = g * sR;
            if (subPulse_ > 0.0f || subPulseCur_ > 1.0e-4f) {
                // A raised cosine at the Binaural rate: 1 at the top of every cycle, 1 - Pulse at
                // the bottom, no corner anywhere. With Binaural at zero the phase stands still at
                // the top and nothing moves.
                subPulseCur_ += (subPulse_ - subPulseCur_) * levelC;
                subPulsePhase_ += subBinaural_ / sr_;
                if (subPulsePhase_ >= 1.0) subPulsePhase_ -= 1.0;
                double q = subPulsePhase_ + 0.25; if (q >= 1.0) q -= 1.0;   // sin01 reads a table: the phase must stay in [0, 1)
                const float pulse = 1.0f - subPulseCur_ * (0.5f - 0.5f * sin01(q));
                subL[i] *= pulse; subR[i] *= pulse;
            }
        }
    }
    // The body: the whole mix passes through a bank of modes and their answer is added back. It
    // sits before the mid/side stage, so its own width is treated like everything else's.
    if (bodyLevel_ > 0.0f || smBody_.value > 1.0e-4f) {
        float* mono = nl;   // the near bus is finished with; reuse it
        // The bank is linear, so the level rides on its input: one smoothed multiply per sample
        // and the body can be turned up mid-note without a step.
        for (int i = 0; i < n; ++i) mono[i] = 0.5f * (L[i] + R[i]) * smBody_.next(bodyLevel_);
        body_.process(mono, L, R, n, 1.0f);
    }
    midSide_.process(L, R, n);
    if (subOn) {
        // The Foundation's own ceiling (25.09.2026). A beating sub swings its peaks by up to six
        // decibels (the production guide, section 8), and summed straight into the master it was the
        // sub that drove the soft clipper -- which then pumped everything else with it. The guide's
        // answer is a limiter on the sub alone, before the sum: here, at Ceiling dBFS at the output
        // (the master gain is taken out of it), instant to hold a peak, 5 ms to act on it and half a
        // second to let go -- slow enough that a 25 Hz sine is not distorted by its own limiter.
        const float master = dbToGain(masterGain_);
        const float ceiling = dbToGain(subCeiling_) / std::max(master, 1.0e-3f);
        const float hold = std::exp(-1.0f / (0.5f * static_cast<float>(sr_)));
        const float att = 1.0f - std::exp(-1.0f / (0.005f * static_cast<float>(sr_)));
        const float rel = 1.0f - std::exp(-1.0f / (0.5f * static_cast<float>(sr_)));
        for (int i = 0; i < n; ++i) {
            const float peak = std::max(std::fabs(subL[i]), std::fabs(subR[i]));
            subPeak_ = peak > subPeak_ ? peak : subPeak_ * hold;
            const float target = subPeak_ > ceiling ? ceiling / subPeak_ : 1.0f;
            subLimGain_ += (target < subLimGain_ ? att : rel) * (target - subLimGain_);
            L[i] += subL[i] * subLimGain_;
            R[i] += subR[i] * subLimGain_;
        }
    } else { subPeak_ = 0.0f; subLimGain_ = 1.0f; }
    // Output DC blocker at 4 Hz, below the lowest sub the Foundation can reach. Several paths
    // can leave an offset behind -- FM at an integer ratio, the asymmetric tape term, a granular
    // window over a clip that carries one, the shimmer's pitch shifter -- and an offset costs
    // headroom in the soft clipper without ever being heard. One filter at the end covers them all.
    const float dcR = 1.0f - kTwoPi * 4.0f / static_cast<float>(sr_);
    // ...all but the Patina's own clipper, which sits before it. An offset that has grown past the
    // clipper's knee -- the far hall integrated one to -2.1 in thirty seconds -- leaves the music
    // riding on a rail the clipper flattens, and the blocker at the end then takes the rail away
    // and leaves nothing: three presets of the 2.0 library died that way, exactly to zero, while
    // every one of their stems played on. The same filter ahead of the Patina, and only while the
    // Patina is on, so a preset without it is rendered sample for sample as before.
    if (patina_.active()) {
        for (int i = 0; i < n; ++i) {
            const float yl = L[i] - pdcXL_ + dcR * pdcYL_; pdcXL_ = L[i]; pdcYL_ = yl; L[i] = yl;
            const float yr = R[i] - pdcXR_ + dcR * pdcYR_; pdcXR_ = R[i]; pdcYR_ = yr; R[i] = yr;
        }
    }
    // The master's age, before the gain and the clipper: tape wow, lost highs, a noise floor.
    patina_.process(L, R, n);
    // Subsonic: four cascaded one-pole high-passes, 24 dB/oct, on top of the 4 Hz DC blocker.
    // Off at zero, and zero is the default -- this instrument's Foundation reaches lower than the
    // frequency a mastering engineer cuts at, so the cut has to be the player's decision.
    // Four one-poles reach -3 dB at 2.299 times their own corner, so the pole goes there and the
    // knob keeps meaning the frequency the output is 3 dB down at.
    const float sub = subsonicHz_;
    const float subC = sub <= 0.5f ? 0.0f
                     : 1.0f - std::exp(-kTwoPi * (sub / 2.299f) / static_cast<float>(sr_));
    for (int i = 0; i < n; ++i) {
        const float g = masterSmooth_.next(master);
        float yl = L[i] - dcXL_ + dcR * dcYL_; dcXL_ = L[i]; dcYL_ = yl;
        float yr = R[i] - dcXR_ + dcR * dcYR_; dcXR_ = R[i]; dcYR_ = yr;
        if (subC > 0.0f) {
            subL1_ += subC * (yl - subL1_); yl -= subL1_;
            subR1_ += subC * (yr - subR1_); yr -= subR1_;
            subL2_ += subC * (yl - subL2_); yl -= subL2_;
            subR2_ += subC * (yr - subR2_); yr -= subR2_;
            subL3_ += subC * (yl - subL3_); yl -= subL3_;
            subR3_ += subC * (yr - subR3_); yr -= subR3_;
            subL4_ += subC * (yl - subL4_); yl -= subL4_;
            subR4_ += subC * (yr - subR4_); yr -= subR4_;
        }
        L[i] = softClip(yl * g);
        R[i] = softClip(yr * g);
        outTap_[(outTapW_ + i) & (kOutTapLen - 1)] = 0.5f * (L[i] + R[i]);
    }
    outTapW_ = (outTapW_ + n) & (kOutTapLen - 1);
    // The loudness meter sees exactly what a file would: after the master gain and the clipper.
    loudness_.process(L, R, n);
}

} // namespace ambient

namespace ambient {

/**
 * @brief The conductors on their own clock, with nothing rendered: the rule book's check is an hour of
 *        notes measured, and an hour of audio is not the way to get them.
 *
 * The seed and every parameter
 * come through readParams(), so what is measured is what the render would have played.
 */
void Engine::auditConductor(double seconds, double dt,
                            const std::function<void(double, int, const BrainEvent&)>& sink,
                            const std::function<void(double, int)>& rootSink)
{
    readParams();
    auto freqOf = [this](int note) { return frequencyOf(note); };
    double t = 0.0;
    int lastRoot = brain_.root();
    if (rootSink) rootSink(0.0, lastRoot);
    const long steps = static_cast<long>(seconds / dt);
    for (long i = 0; i < steps; ++i) {
        {
            const BrainParams* bpNow = &bp_;
            BrainParams bpSlow;
            if (near_.sequenceRunning()) { bpSlow = bp_; bpSlow.rateSeconds *= 2.0f; bpNow = &bpSlow; }
            brain_.update(dt, *bpNow, -1, freqOf, [&](const BrainEvent& e) { sink(t, 1, e); });
            if (brain2On_) {
                brain2_.setRoot(clampv(brain_.root() + brain2Interval_, 0, 127));
                brain2_.update(dt, bp2_, -1, freqOf, [&](const BrainEvent& e) { sink(t, 2, e); });
            }
        }
        // The near events as a third conductor: its notes on and off, as the render would play
        // them (a glide is reported as the new note beginning, a move not at all).
        stepNear(dt, [&](const NearNote& e) {
            if (e.type == NearNote::Type::Move) return;
            sink(t, 3, BrainEvent{ e.type == NearNote::Type::Off ? BrainEvent::Type::NoteOff : BrainEvent::Type::NoteOn, e.note, e.velocity });
        });
        if (brain_.root() != lastRoot) { lastRoot = brain_.root(); if (rootSink) rootSink(t, lastRoot); }
        t += dt;
    }
}

} // namespace ambient
