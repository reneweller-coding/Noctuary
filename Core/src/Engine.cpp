#include <chrono>
#include <thread>
#include "ambient/Engine.h"
#include "ambient/PresetMap.h"
#include "ambient/PresetMeta.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
  #include <xmmintrin.h>
  #include <pmmintrin.h>
  #define AMBIENT_HAS_MXCSR 1
#else
  #define AMBIENT_HAS_MXCSR 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
  #define AMBIENT_HAS_FPCR 1
#else
  #define AMBIENT_HAS_FPCR 0
#endif

namespace ambient {

Engine::Engine()
{
    // The envelopes start from a usable shape rather than from nothing.
    for (auto& e : envPending_) e.parse("0:0/1:1/4:0");
    for (auto& e : envShape_) e.parse("0:0/1:1/4:0");
    for (auto& e : srcEnvPending_) e.parse(kSrcEnvDefault);
    for (auto& e : srcEnvShape_) e.parse(kSrcEnvDefault);
    for (int i = 0; i < kNumParams; ++i) {
        const float def = paramTable()[static_cast<size_t>(i)].def;
        params_[i].store(def, std::memory_order_relaxed);
        slotA_[i].store(def, std::memory_order_relaxed);
        slotB_[i].store(def, std::memory_order_relaxed);
    }
    for (int i = 0; i < kUserScaleIndex; ++i) makeBuiltinScale(i, scales_[i]);
    makeBuiltinScale(0, scales_[kUserScaleIndex]);
    std::strncpy(scales_[kUserScaleIndex].name, "User (Scala)", sizeof(FixedScale::name) - 1);
    // The timbre scale starts as twelve equal steps and is replaced the moment it is asked for.
    makeBuiltinScale(0, scales_[kTimbreScaleIndex]);
    std::strncpy(scales_[kTimbreScaleIndex].name, "Timbre (Sethares)", sizeof(FixedScale::name) - 1);
    scale_ = &scales_[3];
    masterSmooth_.snap(dbToGain(-6.0f));
    for (auto& d : noteDistance_) d.store(-1.0f, std::memory_order_relaxed);
    for (auto& l : noteLevel_) l.store(0.0f, std::memory_order_relaxed);
}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    sr_ = sampleRate;
    maxBlock_ = std::max(maxBlockSize, kControlBlock);
    // The map's cached preset vectors are NOT built here. With the library loaded that is eight
    // thousand presets and it measured 1.75 s of the 1.78 s this function took -- paid on every
    // plugin instance, every offline render, every sample-rate change, whether or not the map was
    // ever switched on. The host builds it in the background (PresetMap::warmupAsync); until it
    // is ready the map does not engage, and everything else in the instrument is unaffected.
    for (auto* s : { &smDelayMix_, &smDelayToFar_, &smDelay2Mix_, &smDelay2ToFar_, &smCloudSend_, &smCosmosSend_, &smCosmosReturn_, &smCosmosToFar_, &smFarLevel_, &smFarWidth_, &smEnvelop_, &smComod_,
                     &smMemSend_, &smMemReturn_, &smMemToFar_ })
        s->setTime(0.02f, sr_);
    smDelayMix_.snap(getParam(ParamId::DelayMix)); smDelayToFar_.snap(getParam(ParamId::DelayToFar));
    smDelay2Mix_.snap(getParam(ParamId::Delay2Mix)); smDelay2ToFar_.snap(getParam(ParamId::Delay2ToFar));
    smCloudSend_.snap(getParam(ParamId::CloudSend)); smCosmosSend_.snap(getParam(ParamId::CosmosSend));
    smMemSend_.snap(getParam(ParamId::MemSend)); smMemReturn_.snap(getParam(ParamId::MemReturn)); smMemToFar_.snap(getParam(ParamId::MemToFar));
    smCosmosReturn_.snap(getParam(ParamId::CosmosReturn)); smCosmosToFar_.snap(getParam(ParamId::CosmosToFar));
    smFarLevel_.snap(getParam(ParamId::FarLevel)); smFarWidth_.snap(getParam(ParamId::FarWidth));
    for (int i = 0; i < kNumParams; ++i) blendCur_[i].store(getParam(static_cast<ParamId>(i)), std::memory_order_relaxed);
    blendActive_.store(false, std::memory_order_relaxed);
    for (auto* b : { &nearL_, &nearR_, &farL_, &farR_, &wetL_, &wetR_, &dryL_, &dryR_, &foreD2L_, &foreD2R_, &foreCoL_, &foreCoR_, &cosL_, &cosR_, &nebL_, &nebR_, &shimL_, &shimR_, &fbInL_, &fbInR_, &fbMono_, &memL_, &memR_,
                     &cloudL_, &cloudR_,
                     &roomInL_, &roomInR_, &roomOutL_, &roomOutR_ })
        b->assign(static_cast<size_t>(maxBlock_), 0.0f);
    {   // Room: pre-delay ring (>= 300 ms) and the convolver with a generated hall unless a file was set
        int ring = 1; while (ring < static_cast<int>(0.32 * sr_) + maxBlock_) ring <<= 1;
        roomDelayL_.assign(static_cast<size_t>(ring), 0.0f); roomDelayR_.assign(static_cast<size_t>(ring), 0.0f);
        roomDelayMask_ = ring - 1; roomDelayW_ = 0; roomLpL_ = roomLpR_ = 0.0f; roomLevelCur_ = 0.0f; roomTailLeft_ = 0;
        const bool keepUser = userImpulse_ && room_.hasImpulse() && std::fabs(room_.impulseSeconds()) > 0.0f;
        std::vector<float> keepL, keepR;   // a user impulse survives a sample-rate change only through the host reloading it
        room_.prepare(sr_, roomMaxSeconds_);
        hasImpulseB_ = false;   // prepare empties both impulses; a B comes back only when it is loaded again
        if (!keepUser) { room_.generateDefault(static_cast<uint64_t>(seed_) + 7); userImpulse_ = false; }
        (void)keepL; (void)keepR;
    }
    {
        // A block behind, plus as far back as the tape's wow and flutter reach (0.33 % of a
        // second) -- two blocks alone were shorter than that at small host buffers, and the head
        // wrapped round to the previous pass.
        int ring = 1; while (ring < 2 * maxBlock_ + static_cast<int>(0.005 * sr_) + 16) ring <<= 1;
        fbRingL_.assign(static_cast<size_t>(ring), 0.0f);
        fbRingR_.assign(static_cast<size_t>(ring), 0.0f);
        fbMask_ = ring - 1; fbW_ = 0;
        fbLpL_ = fbLpR_ = fbEnv_ = 0.0f;
    }
    seed_ = static_cast<int>(getParam(ParamId::Seed));
    rng_.seed(static_cast<uint64_t>(seed_) + 1);
    for (auto& v : voices_) v.prepare(sr_, rng_.fork());
    // Two tables built on first use: the built-in wavetables and the Z-plane grid. Their first
    // use was on the audio thread, under the lock the language puts round a static's
    // construction. Touched here so that they exist before any block is rendered.
    (void)builtinTable(0);
    zWarmTables();
    zWarmShapes();
    arc_.init(rng_);
    shiftDrift_.init(rng_);
    ensemble_.prepare(sr_);
    delay_.prepare(sr_);
    delay2_.prepare(sr_);
    cloud_.prepare(sr_, rng_.fork());
    memory_.prepare(sr_, 0x4D454D4F52ull);   // its own stream: a fork here would move every voice's dice
    nearReverb_.prepare(sr_);
    farReverb_.prepare(sr_);
    midSide_.prepare(sr_);
    shifter_.prepare(sr_);
    resonator_.prepare(sr_);
    vowel_.prepare(sr_, rng_.fork());
    nebula_.prepare(sr_, rng_.fork());
    shimmerL_.prepare(sr_);
    shimmerR_.prepare(sr_);
    shimmerSpec_.prepare(sr_);
    shimmerWasOn_ = false;
    shimmerLpL_ = shimmerLpR_ = 0.0f;
    masterSmooth_.setTime(0.02f, sr_);
    loudness_.prepare(sr_);
    smBlur_.setTime(0.02f, sr_);
    smBody_.setTime(0.02f, sr_);
    unmask_.prepare(sr_);
    haas_.prepare(sr_);
    early_.prepare(sr_);
    diffuser_.prepare(sr_);
    coupleBuf_.assign(static_cast<size_t>(maxBlock_), 0.0f);
    smRoomMorph_.setTime(0.05f, sr_);
    body_.prepare(sr_, 0xB0D1B0D1ull);
    patina_.prepare(sr_, 0x9A7104ull);
    blur_.prepare(sr_, 0x5EED5EEDull);
    auxRng_.seed(0xA5A5A5A5ull);
    strikeRng_.seed(0x57A1CEull);
    near_.reset(0x4E454152ull + static_cast<uint64_t>(seed_ + 1));
    nearPickRng_ = 0x9E3779B9u ^ (static_cast<uint32_t>(seed_ + 1) * 2654435761u);   // the pool's draw, never zero
    if (nearPickRng_ == 0u) nearPickRng_ = 0x9E3779B9u;   // its own stream, from the seed
    tideDrift_.init(auxRng_); rotDrift_.init(auxRng_);
    vecDriftX_.init(auxRng_); vecDriftY_.init(auxRng_);
    dcXL_ = dcXR_ = dcYL_ = dcYR_ = 0.0f;
    pdcXL_ = pdcXR_ = pdcYL_ = pdcYR_ = 0.0f;
    lastRootPc_ = -1;
    readParams();
    brain_.reset(rng_.fork(), rootNote_ - 12);   // the brain's root lives an octave below the key root
    brain2_.reset(0x2B2A1Full, rootNote_ - 12);  // its own stream, so switching it on never moves the first one
    for (auto& h : midiHeld_) h = false;
}

void Engine::reset()
{
    for (auto& v : voices_) v.kill();
    for (auto& h : midiHeld_) h = false;
    brain_.reset(rng_.fork(), rootNote_ - 12);
    strikeRng_.seed(0x57A1CEull);   // its own stream, so a reset gives the same piece back
    strikeExcAvg_ = 0.0;
    near_.reset(0x4E454152ull + static_cast<uint64_t>(seed_ + 1));
    nearPickRng_ = 0x9E3779B9u ^ (static_cast<uint32_t>(seed_ + 1) * 2654435761u);   // the pool's draw, never zero
    if (nearPickRng_ == 0u) nearPickRng_ = 0x9E3779B9u;
}

bool Engine::applyPreset(int index)
{
    if (index < 0 || index >= numPresets()) return false;
    const Preset& p = preset(index);
    const bool ok = ambient::applyPreset(p, [this](ParamId id, float v) { setParam(id, v); });
    return applyPresetModulation(p) && ok;
}

bool Engine::applySoundPreset(int index)
{
    if (index < 0 || index >= numPresets()) return false;
    const Preset& p = preset(index);
    // Modulation belongs to the sound layer, not to Cosmos: loading a sound preset brings its
    // matrix and its envelope shapes with it.
    const bool ok = ambient::applyPreset(p, [this](ParamId id, float v) { setParam(id, v); }, PresetScope::Sound);
    return applyPresetModulation(p) && ok;
}

bool Engine::applyCosmosPreset(int index)
{
    if (index < 0 || index >= numCosmosPresets()) return false;
    return ambient::applyPreset(cosmosPreset(index), [this](ParamId id, float v) { setParam(id, v); }, PresetScope::Cosmos);
}

bool Engine::applyZPreset(int index)
{
    if (index < 0 || index >= numZPresets()) return false;
    return ambient::applyPreset(zPreset(index), [this](ParamId id, float v) { setParam(id, v); }, PresetScope::ZPlane);
}

bool Engine::applyStrikePreset(int index)
{
    if (index < 0 || index >= numStrikePresets()) return false;
    return ambient::applyPreset(strikePreset(index), [this](ParamId id, float v) { setParam(id, v); }, PresetScope::Strike);
}

bool Engine::applyNearPreset(int index)
{
    if (index < 0 || index >= numNearPresets()) return false;
    return ambient::applyPreset(nearPreset(index), [this](ParamId id, float v) { setParam(id, v); }, PresetScope::Near);
}

// ---------------------------------------------------------------- morph

void Engine::setMorphSlot(int slot, const float* values)
{
    auto& s = (slot == 0) ? slotA_ : slotB_;
    for (int i = 0; i < kNumParams; ++i) s[i].store(values[i], std::memory_order_relaxed);
    // Values first, the flag after: the audio thread reads the flag to decide whether to read the
    // values at all, so it must never see "chosen" over a half-written snapshot.
    slotSet_[slot & 1].store(true, std::memory_order_relaxed);
}

void Engine::captureMorphSlot(int slot)
{
    auto& s = (slot == 0) ? slotA_ : slotB_;
    for (int i = 0; i < kNumParams; ++i) s[i].store(params_[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    slotSet_[slot & 1].store(true, std::memory_order_relaxed);
}

void Engine::clearMorphSlot(int slot)
{
    slotSet_[slot & 1].store(false, std::memory_order_relaxed);
}

void Engine::morphSlot(int slot, float* out) const
{
    const auto& s = (slot == 0) ? slotA_ : slotB_;
    for (int i = 0; i < kNumParams; ++i) out[i] = s[i].load(std::memory_order_relaxed);
}

bool Engine::routeStep(double dt, float& x, float& y, float& radius)
{
    // Pick up an edit made on the message thread (fixed-size object, no allocation), and restart
    // from the cursor so a route changed while it plays does not jump.
    const int rv = routeVersion_.load(std::memory_order_acquire);
    if (rv != routeSeen_) { route_ = routePending_; routeSeen_ = rv; routeWasActive_ = false; }
    const bool active = getParam(ParamId::RouteActive) >= 0.5f && route_.count() > 0;
    if (active && !routeWasActive_) {   // (re)start from where the cursor is now
        route_.start(getParam(ParamId::MapX), getParam(ParamId::MapY), getParam(ParamId::MapRadius), getParam(ParamId::RouteLoop) >= 0.5f);
    }
    if (!active && routeWasActive_) route_.stop();
    routeWasActive_ = active;
    if (!active) { x = getParam(ParamId::MapX); y = getParam(ParamId::MapY); radius = getParam(ParamId::MapRadius); return false; }
    const bool running = route_.update(static_cast<float>(dt) * getParam(ParamId::RouteSpeed), x, y, radius);
    setParam(ParamId::MapX, x); setParam(ParamId::MapY, y); setParam(ParamId::MapRadius, radius);
    if (getParam(ParamId::MapActive) < 0.5f) setParam(ParamId::MapActive, 1.0f);   // a route always plays through the map
    if (!running) setParam(ParamId::RouteActive, 0.0f);                              // ended (not looping): the cursor stays
    return running;
}

void Engine::updateBlend(int n)
{
    // Map mode: blend the presets around the cursor, glide every parameter toward it.
    const bool on = getParam(ParamId::MapActive) >= 0.5f && PresetMap::ready() && numPresetMeta() > 0;
    const bool was = blendActive_.load(std::memory_order_relaxed);
    if (!on) { if (was) blendActive_.store(false, std::memory_order_relaxed); return; }
    if (!was) {   // start from what is sounding now: no jump when the map takes over
        for (int i = 0; i < kNumParams; ++i) blendCur_[i].store(effectiveParam(static_cast<ParamId>(i)), std::memory_order_relaxed);
        blendActive_.store(true, std::memory_order_relaxed);
    }
    // The neighbours and the blended target only change when the cursor does. Standing still is
    // the normal case -- a hand resting on the map, a route between two points -- and the search
    // reads every preset's position while the blend runs three powers over every float parameter.
    // Both were done on every block for an answer that was the same as the block before.
    const float mx = getParam(ParamId::MapX), my = getParam(ParamId::MapY), mr = getParam(ParamId::MapRadius);
    if (mx != blendX_ || my != blendY_ || mr != blendR_ || !blendHave_) {
        blendX_ = mx; blendY_ = my; blendR_ = mr; blendHave_ = true;
        PresetMap::blend(PresetMap::neighbours(mx, my, mr), blendTarget_);
    }
    const float glide = std::max(getParam(ParamId::MorphGlide), 0.05f);
    const float coef = 1.0f - std::exp(-static_cast<float>(n / sr_) / (glide / 3.0f));   // ~95 % after `glide` seconds
    for (const ParamDesc& d : paramTable()) {
        const int i = static_cast<int>(d.id);
        if (isPerformanceParam(d.id)) { blendCur_[i].store(getParam(d.id), std::memory_order_relaxed); continue; }
        const float cur = blendCur_[i].load(std::memory_order_relaxed), tgt = blendTarget_[i];
        float next;
        switch (d.kind) {
        case ParamKind::Float: {
            const float span = std::max(d.max - d.min, 1e-9f);
            // Arrived is arrived: three powers for a parameter that is already on its target is
            // the same waste readParams was making, and here it is every float on every block.
            if (std::fabs(tgt - cur) <= 1.0e-7f * span) { next = tgt; break; }
            const float pc = std::pow(clampv((cur - d.min) / span, 0.0f, 1.0f), d.skew);
            const float pt = std::pow(clampv((tgt - d.min) / span, 0.0f, 1.0f), d.skew);
            next = d.min + span * std::pow(pc + (pt - pc) * coef, 1.0f / d.skew);
            break;
        }
        case ParamKind::Int:
            next = cur + (tgt - cur) * coef;
            break;
        default:
            next = tgt;   // choices and switches follow the strongest neighbour
        }
        blendCur_[i].store(next, std::memory_order_relaxed);
    }
}

float Engine::effectiveParam(ParamId id) const
{
    const float live = getParam(id);
    if (blendActive_.load(std::memory_order_relaxed) && !isPerformanceParam(id)) {
        const float v = blendCur_[static_cast<int>(id)].load(std::memory_order_relaxed);
        return paramDesc(id).kind == ParamKind::Int ? static_cast<float>(std::lround(v)) : v;
    }
    if (isMorphParam(id) || getParam(ParamId::MorphActive) < 0.5f) return live;
    // An end nobody has chosen is the sound as the knobs have it, not the defaults. With neither
    // chosen Morph is therefore silent in effect, and with one chosen it runs from what is playing
    // now to that one -- which is what "morph to B" means to anyone who has not read this line.
    const bool setA = slotSet_[0].load(std::memory_order_relaxed), setB = slotSet_[1].load(std::memory_order_relaxed);
    if (!setA && !setB) return live;
    const int i = static_cast<int>(id);
    const float a = setA ? slotA_[i].load(std::memory_order_relaxed) : live;
    const float b = setB ? slotB_[i].load(std::memory_order_relaxed) : live;
    const float t = morphCur_.load(std::memory_order_relaxed);
    const ParamDesc& d = paramDesc(id);
    switch (d.kind) {
    case ParamKind::Float: {
        // Interpolate in the skewed (perceptual) domain, like the host's knob travel.
        const float span = std::max(d.max - d.min, 1e-9f);
        const float pa = std::pow(clampv((a - d.min) / span, 0.0f, 1.0f), d.skew);
        const float pb = std::pow(clampv((b - d.min) / span, 0.0f, 1.0f), d.skew);
        const float p = pa + (pb - pa) * t;
        return d.min + span * std::pow(p, 1.0f / d.skew);
    }
    case ParamKind::Int:
        return static_cast<float>(std::lround(a + (b - a) * t));
    default:
        return t < 0.5f ? a : b;   // choices and switches flip halfway
    }
}

void Engine::setUserScale(const FixedScale& s)
{
    while (userBusy_.load(std::memory_order_acquire)) { /* audio thread is copying, microseconds */ }
    userPending_ = s;
    userVersion_.fetch_add(1, std::memory_order_release);
}

void Engine::setUserWavetable(const Wavetable& t)
{
    while (tableBusy_.load(std::memory_order_acquire)) { /* audio thread copying, microseconds */ }
    userTablePending_ = t;
    userTableFrames_.store(t.frames, std::memory_order_relaxed);
    tableVersion_.fetch_add(1, std::memory_order_release);
}

bool Engine::loadUserWavetable(const float* mono, int n, int frameLen)
{
    Wavetable t;
    if (!t.analyse(mono, n, frameLen)) return false;
    CycleTable c;
    if (!c.build(mono, n, frameLen)) return false;   // silence: nothing either type could play
    setUserWavetable(t);
    setUserCycles(c);
    return true;
}

void Engine::setUserCycles(const CycleTable& t)
{
    // A texture's double buffer: once no block that may still hold the other copy is running, that
    // copy is written and published. The megabytes are copied here, on the message thread.
    waitForQuiet();
    const int active = cyclesActive_.load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    userCycles_[target] = t;
    cyclesActive_.store(target, std::memory_order_release);
}

// Message thread: returns once every block that had begun by the time it was called has ended.
// After that, any block still running began later and therefore read whatever this thread had
// already published -- which is what makes the other half of a double buffer safe to write.
//
// There is no guess in it. When the audio thread is not running the two counters are equal on the
// first look and it returns at once, which is a host with its transport stopped; when a block is
// in flight it waits for that block and no longer, whether that block is 16 samples or 2048. The
// version before this one counted finished blocks and gave up after two milliseconds of seeing
// none, on the theory that nothing was rendering -- which is also what a slow block looks like
// from outside, and a 2048-sample block at 44.1 kHz is 46 milliseconds long.
void Engine::waitForQuiet()
{
    const unsigned long long begun = blocksBegun_.load(std::memory_order_acquire);
    for (int spin = 0; spin < 8000; ++spin) {
        if (blocksDone_.load(std::memory_order_acquire) >= begun) return;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

// A recording's offset, taken out where it comes in. A field recording, a rendered texture, a
// phrase cut from a radio play: a few of them carry a direct current of a few per cent, which no
// grain window removes and which every long loop downstream -- the far hall above all -- would
// integrate until the master's clipper rails on it (three presets of the 2.0 library went silent
// that way). Subtracting the mean changes nothing anyone hears and keeps a seamless clip seamless.
static void removeOffset(float* p, size_t count, size_t stride)
{
    if (p == nullptr || count == 0) return;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += p[i * stride];
    const float mean = static_cast<float>(sum / static_cast<double>(count));
    if (std::fabs(mean) < 1.0e-6f) return;
    for (size_t i = 0; i < count; ++i) p[i * stride] -= mean;
}

static void removeOffset(Texture& t)
{
    removeOffset(t.mono.data(), t.mono.size(), 1);
    if (!t.lr.empty()) {
        removeOffset(t.lr.data(), t.lr.size() / 2, 2);
        removeOffset(t.lr.data() + 1, t.lr.size() / 2, 2);
    }
}

void Engine::setTexture(int slot, const float* L, const float* R, int n, double sampleRate, double baseHz, bool seamless)
{
    if (R == nullptr) { setTexture(slot, L, n, sampleRate, baseHz, seamless); return; }
    if (slot < 0 || slot >= kSlots || L == nullptr) return;
    const int len = std::max(n, 0);
    // The mono sum for the analysis and for the types that read one signal, and the interleaved
    // pair for the grain loop. Built here rather than in every caller, so a host, the render tool
    // and the Quest cannot disagree about what the sum is.
    std::vector<float> mono(static_cast<size_t>(len), 0.0f);
    for (int i = 0; i < len; ++i) mono[static_cast<size_t>(i)] = 0.5f * (L[i] + R[i]);
    waitForQuiet();
    const int active = textureActive_[slot].load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    Texture& t = textures_[slot][target];
    t.mono = std::move(mono);
    t.lr.assign(static_cast<size_t>(2 * len), 0.0f);
    for (int i = 0; i < len; ++i) { t.lr[static_cast<size_t>(2 * i)] = L[i]; t.lr[static_cast<size_t>(2 * i + 1)] = R[i]; }
    removeOffset(t);
    t.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    t.baseHz = baseHz > 0.0 ? baseHz : 261.6256;
    t.seamless = seamless;
    t.measure();
    textureActive_[slot].store(target, std::memory_order_release);
}

void Engine::setTexture(int slot, const float* mono, int n, double sampleRate, double baseHz, bool seamless)
{
    if (slot < 0 || slot >= kSlots) return;
    const int active = textureActive_[slot].load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    // The buffer about to be written is the one that is NOT playing -- but "not playing" is a
    // statement about now, and the audio thread chose what to play at the top of the block it is
    // in. A block that began before this call may still be holding exactly this buffer, from
    // before the last swap. So: wait for two blocks to go by, which is the proof that every block
    // begun earlier has ended, and every block begun since read the current active and therefore
    // did not choose this one.
    //
    // The flag that used to stand here asked the wrong question. It said which buffer the audio
    // thread had taken up LAST, and there is a window between its reading the active index and
    // its saying so; a writer whose check fell inside that window saw nothing and wrote into a
    // buffer that was about to be read. The thread sanitizer found it, on Linux, in forty seconds.
    waitForQuiet();
    Texture& t = textures_[slot][target];
    t.mono.assign(mono, mono + std::max(n, 0));
    t.lr.clear();          // this buffer may have held a stereo clip: a mono one is mono again
    removeOffset(t);
    t.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    t.baseHz = baseHz > 0.0 ? baseHz : 261.6256;
    t.seamless = seamless;
    t.measure();
    textureActive_[slot].store(target, std::memory_order_release);
}

void Engine::setTexture(int slot, const Texture& src)
{
    if (slot < 0 || slot >= kSlots || src.empty()) return;
    waitForQuiet();
    const int active = textureActive_[slot].load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    textures_[slot][target] = src;
    textureActive_[slot].store(target, std::memory_order_release);
}

void Engine::clearTexture(int slot)
{
    if (slot < 0 || slot >= kSlots) return;
    textureActive_[slot].store(-1, std::memory_order_release);
}

Texture Engine::makeTexture(const float* L, const float* R, int n, double sampleRate, double baseHz, bool seamless, double maxSeconds)
{
    Texture t;
    if (L == nullptr || n <= 0) return t;
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int len = std::min(n, static_cast<int>(std::max(1.0, maxSeconds) * sr));
    t.mono.assign(static_cast<size_t>(len), 0.0f);
    for (int i = 0; i < len; ++i) t.mono[static_cast<size_t>(i)] = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
    if (R != nullptr) {
        t.lr.assign(static_cast<size_t>(2 * len), 0.0f);
        for (int i = 0; i < len; ++i) { t.lr[static_cast<size_t>(2 * i)] = L[i]; t.lr[static_cast<size_t>(2 * i + 1)] = R[i]; }
    }
    removeOffset(t);
    t.sampleRate = sr;
    t.baseHz = baseHz > 0.0 ? baseHz : 261.6256;
    t.seamless = seamless;
    t.measure();
    return t;
}

void Engine::setNearTexture(const float* L, const float* R, int n, double sampleRate, double baseHz, bool seamless)
{
    // A recording of a rover driving for sixteen minutes is a bed, not an event: the near source
    // keeps at most two minutes of a clip, from its start.
    Texture t = makeTexture(L, R, n, sampleRate, baseHz, seamless, 120.0);
    if (t.empty()) return;
    std::vector<Texture> pool;
    pool.push_back(std::move(t));
    setNearTextures(std::move(pool));
}

void Engine::setNearTexture(const Texture& src)
{
    if (src.empty()) return;
    std::vector<Texture> pool;
    pool.push_back(src);
    setNearTextures(std::move(pool));
}

void Engine::setNearTextures(std::vector<Texture> pool)
{
    pool.erase(std::remove_if(pool.begin(), pool.end(), [](const Texture& t) { return t.empty(); }), pool.end());
    if (pool.empty()) { clearNearTexture(); return; }
    // The pool that is not active is free to overwrite once every block begun before now has
    // ended: the block in flight may still read the other one, and the next call waits again
    // before touching that. The pick is not reset here -- it is the audio thread's, and it is
    // clamped to the pool wherever it is read.
    waitForQuiet();
    const int active = nearPoolActive_.load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    nearPools_[target] = std::move(pool);
    nearPoolActive_.store(target, std::memory_order_release);
}

// Where Match would put partial h: on the degree of the current scale nearest to it, counted in
// periods of that scale from the fundamental. For a twelve-tone scale the seventh partial moves
// from 3369 cents to 3400 and the fifth from 2786 to 2800 -- small moves, and after them a chord
// in that scale does not beat. The timbre scale is excluded, because it is itself computed from
// the spectrum: a scale that follows the partials and partials that follow the scale would chase
// each other round in a circle, and neither would mean anything.
double Engine::matchedPartialRatio(int h) const
{
    if (h < 1) return 1.0;
    const FixedScale& s = *scale_;
    if (scale_ == &scales_[kTimbreScaleIndex] || s.count <= 0 || !(s.period > 1.0)) return static_cast<double>(h);
    const double logP = std::log(s.period);
    const double x = std::log(static_cast<double>(h)) / logP;         // the partial, in periods above f0
    const int n = static_cast<int>(std::floor(x));
    const double within = std::pow(s.period, x - n);                     // 1 .. period
    double best = s.ratios[0], bestErr = 1e9;
    for (int d = 0; d <= s.count; ++d) {
        const double r = d < s.count ? s.ratios[d] : s.period;
        const double err = std::fabs(std::log(r / within));
        if (err < bestErr) { bestErr = err; best = r; }
    }
    return std::pow(s.period, n) * best;
}

double Engine::frequencyOf(int note) const
{
    // Purity blends between 12-TET (0) and the chosen scale (1) in the log domain: at 1 the
    // partials of different notes lock, at 0 they beat like a piano; in between the beating
    // slows down as the intervals close in on their ratios.
    const double pure = scaleFrequency(*scale_, note, rootNote_, refPitch_, snapKeys_);
    const double p = purityCur_;
    double f = pure;
    if (p < 0.9999) {
        const double et = refPitch_ * std::pow(2.0, (note - 69) / 12.0);
        f = std::exp(std::log(et) + (std::log(pure) - std::log(et)) * clampv(p, 0.0, 1.0));
    }
    // Adaptive intonation: the note's own offset and the shared comma offset, both in cents,
    // scaled by the amount so that the knob glides everything home rather than switching it.
    if (adaptAmt_ > 0.0f && note >= 0 && note < 128)
        f *= std::pow(2.0, static_cast<double>(adaptAmt_) * (static_cast<double>(adaptCents_[note]) + commaCents_) / 1200.0);
    // Transpose: the pure interval, applied to everything at once so every ratio inside is kept.
    if (transposeCur_ != 0.0) f *= std::pow(2.0, transposeCur_);
    // The stretched octave. Listeners prefer octaves a little wider than 2:1 -- ten to twenty
    // cents at the extremes of the range (Ward 1954; Terhardt) -- and a piano is tuned that
    // way (the Railsback curve). Every octave away from the reference pitch is widened by
    // Stretch cents, in both directions, so the reference itself does not move:
    //     log2 f' = log2 A4 + (1 + s/1200) (log2 f - log2 A4)
    if (stretchCents_ > 0.0f && f > 0.0 && refPitch_ > 0.0)
        f = refPitch_ * std::pow(f / refPitch_, 1.0 + stretchCents_ / 1200.0);
    return f;
}

// The pure offset for a note arriving into a chord. Against every sounding voice the interval
// is reduced to an octave and matched to the nearest small-integer ratio (5-limit and the
// septimal tritone); the offset that would make that interval exact is weighted by the ratio's
// simplicity -- the fifth speaks louder than the minor seventh -- and the weighted mean is the
// answer, capped at thirty cents so a note that fits nothing is not thrown across a quarter tone.
// The sounding voices' frequencies come through frequencyOf, so they carry their own offsets
// and the shared comma, and a note tuned against them lands pure in the world as it is now.
float Engine::adaptiveOffset(int note) const
{
    if (note < 0 || note > 127) return 0.0f;
    static const struct { int num, den; } kRatios[] = {
        { 1, 1 }, { 16, 15 }, { 9, 8 }, { 6, 5 }, { 5, 4 }, { 4, 3 }, { 7, 5 }, { 3, 2 }, { 8, 5 }, { 5, 3 }, { 9, 5 }, { 15, 8 } };
    // The plain frequency: what the note gets with no offset of its own (the comma still applies,
    // and cancels in every ratio below because the sounding voices carry it too).
    double plain = frequencyOf(note);
    if (adaptAmt_ > 0.0f) plain /= std::pow(2.0, static_cast<double>(adaptAmt_) * static_cast<double>(adaptCents_[note]) / 1200.0);
    double sum = 0.0, weight = 0.0;
    for (const auto& v : voices_) {
        if (!v.isActive() || v.note() == note) continue;
        const double g = frequencyOf(v.note());
        if (!(g > 0.0) || !(plain > 0.0)) continue;
        double r = plain / g;
        int oct = 0;
        while (r >= 2.0) { r *= 0.5; ++oct; }
        while (r < 1.0)  { r *= 2.0; --oct; }
        double bestCents = 1e9; int bestNum = 1, bestDen = 1;
        for (const auto& q : kRatios) {
            const double c = 1200.0 * std::log2(static_cast<double>(q.num) / q.den / r);   // + : the pure ratio is above
            if (std::fabs(c) < std::fabs(bestCents)) { bestCents = c; bestNum = q.num; bestDen = q.den; }
        }
        {   // the octave above the last entry is the next unison
            const double c = 1200.0 * std::log2(2.0 / r);
            if (std::fabs(c) < std::fabs(bestCents)) { bestCents = c; bestNum = 1; bestDen = 1; }
        }
        if (std::fabs(bestCents) > 35.0) continue;   // fits nothing: this voice has no say
        const double w = 1.0 / std::max(1.0, std::log2(static_cast<double>(bestNum * bestDen)));
        sum += w * bestCents;
        weight += w;
    }
    if (weight <= 0.0) return 0.0f;
    return static_cast<float>(clampv(sum / weight, -30.0, 30.0));   // raw: frequencyOf scales it by the amount
}

const char* Engine::stemName(int i)
{
    static const char* const names[kNumStems] = { "near", "far", "cosmos", "room" };
    return (i >= 0 && i < kNumStems) ? names[i] : "";
}

const Voice* Engine::loudestVoice() const
{
    const Voice* best = nullptr;
    for (const auto& v : voices_)
        if (v.isActive() && (best == nullptr || v.level() > best->level())) best = &v;
    return best;
}

int Engine::displayPartials(float* out, int maxCount) const
{
    const Voice* v = loudestVoice();
    return v != nullptr ? v->displayPartials(out, maxCount) : 0;
}

float Engine::displayFrequency() const
{
    const Voice* v = loudestVoice();
    return v != nullptr ? static_cast<float>(v->frequency()) : 0.0f;
}

int Engine::displaySlotPartials(int slot, float* out, int maxCount) const
{
    const Voice* v = loudestVoice();
    return v != nullptr ? v->displaySlotPartials(slot, out, maxCount) : 0;
}

float Engine::displaySlotPosition(int slot) const
{
    const Voice* v = loudestVoice();
    return v != nullptr ? v->displaySlotPosition(slot) : -1.0f;
}

bool Engine::displaySlotEnv(int slot, float& shapeSeconds, float& gain) const
{
    const Voice* v = loudestVoice();
    if (v == nullptr) return false;
    shapeSeconds = v->slotEnvTime(slot);
    gain = v->slotGain(slot);
    return shapeSeconds >= 0.0f;
}

int Engine::displayGrains(int slot, SourceSlot::GrainInfo* out, int maxCount) const
{
    const Voice* v = loudestVoice();
    const int k = slot < 0 ? 0 : (slot >= kSlots ? kSlots - 1 : slot);
    const int a = textureActive_[k].load(std::memory_order_relaxed);
    const int len = a >= 0 ? static_cast<int>(textures_[k][a].mono.size()) : 0;
    return (v != nullptr && len > 0) ? v->displayGrains(slot, out, maxCount, len) : 0;
}

int Engine::voiceStage(VoiceStage* out, int maxCount) const
{
    int n = 0;
    for (const auto& v : voices_) {
        if (n >= maxCount) break;
        if (!v.isActive()) continue;
        out[n++] = { v.pan(), v.distance(), v.level(), v.note(), v.owner() };
    }
    return n;
}

int Engine::cosmosTap(float* out, int n) const
{
    n = clampv(n, 0, 4096);
    const int w = cosTapW_;
    for (int i = 0; i < n; ++i) out[i] = cosTap_[(w - n + i) & 4095];
    return n;
}

int Engine::outputTap(float* out, int n) const
{
    n = clampv(n, 0, kOutTapLen);
    const int w = outTapW_;
    for (int i = 0; i < n; ++i) out[i] = outTap_[(w - n + i) & (kOutTapLen - 1)];
    return n;
}

void Engine::soundingNotes(bool (&out)[128]) const
{
    const uint64_t m0 = mask_[0].load(std::memory_order_relaxed), m1 = mask_[1].load(std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i)  out[i] = (m0 >> i) & 1u;
    for (int i = 0; i < 64; ++i)  out[64 + i] = (m1 >> i) & 1u;
}

float Engine::noteDistance(int note) const
{
    if (note < 0 || note > 127) return -1.0f;
    return noteDistance_[note].load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------- notes

Voice* Engine::allocate(int note, int owner)
{
    for (auto& v : voices_) if (v.isActive() && v.note() == note && v.owner() == owner) return &v;
    for (auto& v : voices_) if (!v.isActive()) return &v;
    Voice* best = nullptr;
    for (auto& v : voices_) if (v.isReleasing() && (best == nullptr || v.level() < best->level())) best = &v;
    if (best) return best;
    for (auto& v : voices_) if (best == nullptr || v.order < best->order) best = &v;
    return best;
}

void Engine::startNote(int note, float velocity, int owner, float distance, float ageSeconds)
{
    if (note < 0 || note > 127) return;
    // The modulation envelopes run on a phrase clock: it restarts when a note arrives into
    // silence, not on every note of a cluster, or a slow shape would never get anywhere.
    bool anySounding = false;
    for (const auto& v : voices_) if (v.isActive()) { anySounding = true; break; }
    // A note arriving into silence starts a phrase: the modulation envelopes go back to zero, and
    // so does any LFO set to Retrigger. That mode had been stored by two thousand seven hundred
    // presets and read by nobody -- Lfo::step uses the rate, the phase and the depth and has never
    // looked at the mode at all (12.09.2026).
    if (!anySounding) { for (double& t : envTime_) t = 0.0; phraseStart_ = true; }
    randomPerNote_ = rng_.bipolar();
    Voice* v = allocate(note, owner);
    v->order = ++order_;
    // Adaptive intonation: the offset is decided once, as the note arrives, against what is
    // sounding then; a note already sounding in another voice keeps the offset it has.
    if (adaptAmt_ > 0.0f) {
        bool already = false;
        for (const auto& x : voices_) if (&x != v && x.isActive() && x.note() == note) already = true;
        if (!already) adaptCents_[note] = adaptiveOffset(note);
    } else adaptCents_[note] = 0.0f;
    const double hz = frequencyOf(note);
    // Does this note strike? Yours always does -- you played it. The conductor's is a coin at
    // Chance, and with Cluster up the coin is weighted by the cascade's excitation, so the
    // strikes arrive where the events already crowd together and the long gaps stay empty.
    // At Chance 1 no coin is thrown at all, which is what keeps every older preset bit for bit.
    bool allowStrike = true;
    if (owner != OwnerMidi && vp_.strikeLevel > 0.0f && vp_.strikeBrain && strikeChance_ < 0.999f) {
        const double exc = (owner == OwnerBrain2 ? brain2_ : brain_).excitation();
        strikeExcAvg_ += 0.1 * (exc - strikeExcAvg_);   // what this piece's cascade usually runs at
        const double lift = 1.0 + 2.0 * strikeCluster_ * (exc - strikeExcAvg_);
        const double p = std::clamp(static_cast<double>(strikeChance_) * lift, 0.0, 1.0);
        allowStrike = strikeRng_.uniform() < p;
    }
    // Where the note stands among its owner's notes, for the slot roles: the lowest, the highest,
    // or between. The keys are one group, the two conductors another (the shadow plays in the
    // same cluster), the near events a third. Only computed where a slot has a role at all.
    if (rolesUsed_) {
        int lo = 128, hi = -1;
        const int mine = owner == OwnerMidi ? 0 : (owner == OwnerNear ? 2 : 1);
        for (const auto& x : voices_) {
            if (&x == v || !x.isActive() || x.isReleasing()) continue;
            const int g = x.owner() == OwnerMidi ? 0 : (x.owner() == OwnerNear ? 2 : 1);
            if (g != mine) continue;
            lo = std::min(lo, x.note()); hi = std::max(hi, x.note());
        }
        v->setPlace(note <= lo, note >= hi);
    } else v->setPlace(true, true);
    v->noteOn(note, hz, velocity, owner, distance, vp_, allowStrike, ageSeconds);
    if (rolesUsed_) updatePlaces();   // the others' places have changed with this note's arrival
    if (owner == OwnerMidi) {   // portamento: a key slides in from the previous key
        if (portamento_ > 0.0f && lastKeyHz_ > 0.0 && std::fabs(lastKeyHz_ - hz) > 1e-6) v->glideFrom(lastKeyHz_, portamento_, portaGravity_);
        lastKeyHz_ = hz;
    }
}

void Engine::stopNote(int note, int owner)
{
    for (auto& v : voices_) if (v.isActive() && v.note() == note && v.owner() == owner) v.noteOff();
    if (rolesUsed_) updatePlaces();   // a note letting go hands its place on
}

// Every sounding note's place in its group, for the slot roles. Releasing notes hand theirs on:
// they are left out of the reckoning, so the note above a bass that is fading becomes the lowest
// while the bass is still audible, which is when the ear wants the cello to move up to it.
void Engine::updatePlaces()
{
    int lo[3] = { 128, 128, 128 }, hi[3] = { -1, -1, -1 };
    auto group = [](int owner) { return owner == OwnerMidi ? 0 : (owner == OwnerNear ? 2 : 1); };
    for (const auto& v : voices_) {
        if (!v.isActive() || v.isReleasing()) continue;
        const int g = group(v.owner());
        lo[g] = std::min(lo[g], v.note()); hi[g] = std::max(hi[g], v.note());
    }
    for (auto& v : voices_) {
        if (!v.isActive()) continue;
        const int g = group(v.owner());
        v.setPlace(v.note() <= lo[g], v.note() >= hi[g]);
    }
}

// A near event's note: on the near source, with the event's shape, never a strike unless the
// section has one, and placed by the scheduler rather than by the dice.
void Engine::startNearNote(const NearNote& e)
{
    if (e.note < 0 || e.note > 127) return;
    // The event's clip: one of the pool, and never the one just played where there is a choice.
    // Chosen here, before the voice begins, and written into the near voice parameters at once,
    // so the first block of the note already reads it (readParams sets the same pointer again
    // at every control block after this).
    {
        const int a = nearPoolActive_.load(std::memory_order_acquire);
        if (a >= 0 && nearPools_[a].size() > 1) {
            const int n = static_cast<int>(nearPools_[a].size());
            nearPickRng_ ^= nearPickRng_ << 13; nearPickRng_ ^= nearPickRng_ >> 17; nearPickRng_ ^= nearPickRng_ << 5;
            const int last = std::min(std::max(0, nearPick_), n - 1);
            int pick = static_cast<int>(nearPickRng_ % static_cast<uint32_t>(n - 1));
            if (pick >= last) ++pick;
            nearPick_ = pick;
            vpNear_.texture[kSlots - 1] = &nearPools_[a][static_cast<size_t>(pick)];
        }
    }
    Voice* v = allocate(e.note, OwnerNear);
    v->order = ++order_;
    v->setPlace(true, true);
    v->setNearShape(e.releaseMul, e.cutoffMul, 6.0f * clampv(np_.proximity, 0.0f, 1.0f), e.pan, nearGain_);
    v->noteOn(e.note, frequencyOf(e.note), e.velocity, OwnerNear, e.distance, vpNear_, vpNear_.strikeLevel > 0.0f, 0.0f);
}

void Engine::nearEmit(const NearNote& e)
{
    switch (e.type) {
    case NearNote::Type::On:  startNearNote(e); break;
    case NearNote::Type::Off: stopNote(e.note, OwnerNear); break;
    case NearNote::Type::Glide:
        for (auto& v : voices_)
            if (v.isActive() && v.owner() == OwnerNear && !v.isReleasing())
                v.glideTo(e.note, frequencyOf(e.note) * std::pow(2.0, static_cast<double>(e.detune) / 12.0), e.seconds, portaGravity_);
        break;
    case NearNote::Type::Move:
        for (auto& v : voices_) if (v.isActive() && v.owner() == OwnerNear) v.moveTo(e.distance, e.seconds);
        break;
    }
}

void Engine::noteOn(int note, float velocity)
{
    if (note < 0 || note > 127) return;
    if (hold_ && midiHeld_[note]) {   // Hold: pressing a sounding key releases it
        midiHeld_[note] = false;
        stopNote(note, OwnerMidi);
        return;
    }
    midiHeld_[note] = true;
    startNote(note, velocity, OwnerMidi, keysDepth_);
}

void Engine::noteOff(int note)
{
    if (note < 0 || note > 127) return;
    if (hold_) return;   // keys latch
    midiHeld_[note] = false;
    stopNote(note, OwnerMidi);
}

void Engine::setPressure(int note, float v)
{
    for (auto& x : voices_) if (x.isActive() && (note < 0 || x.note() == note)) x.setPressure(v);
}

void Engine::setSlide(int note, float v)
{
    for (auto& x : voices_) if (x.isActive() && (note < 0 || x.note() == note)) x.setSlide(v);
}

void Engine::setHeadYaw(float degrees)
{
    headYawDeg_.store(degrees, std::memory_order_relaxed);
}

void Engine::setWheel(float v)
{
    wheelTarget_ = clampv(v, 0.0f, 1.0f);
}

void Engine::setBend(int note, float normalised)
{
    const float semis = clampv(normalised, -1.0f, 1.0f) * bendRange_;
    for (auto& x : voices_) if (x.isActive() && (note < 0 || x.note() == note)) x.setBend(semis);
}

void Engine::allNotesOff()
{
    for (auto& h : midiHeld_) h = false;
    for (auto& v : voices_) v.noteOff();
}

// ---------------------------------------------------------------- parameters

} // namespace ambient
