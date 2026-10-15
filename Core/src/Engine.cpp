/**
 * @file Engine.cpp
 * @brief The engine's life cycle and its outward face: construction and prepare, the preset loaders,
 *        morph and map, the tuning arithmetic, the textures, the display taps and the notes.
 *
 * Engine.h declares the one class that owns the whole instrument -- the voices, the two conductors,
 * the near layer, both planes of effects and the master -- and its definitions are spread over three
 * files. This one holds what happens off the audio thread and at the edges of it: the constructor
 * and prepare(), which build every table, ring and effect before a block is rendered; the preset
 * loaders; the morph slots, the route and the preset map's blend; the user scale, the user wavetable
 * and the textures, which cross from the message thread into the audio thread through double
 * buffers and waitForQuiet(); the tuning arithmetic (frequencyOf, matchedPartialRatio and
 * adaptiveOffset); the read-only taps the panel draws from; and the note handling -- allocation,
 * the register places for the slot roles, the near events and the keys. EngineControl.cpp holds
 * what the engine decides once per block (the modulation sources, the clock, readParams) and
 * EngineRender.cpp what it renders (process and renderChunk); both were split out of this file when
 * it had grown past fourteen hundred lines.
 *
 * Threads matter in every function here. What is called set..., apply... or load... runs on the
 * message thread and publishes through an atomic version or a double buffer; what the conductors
 * and the near scheduler call back into (startNote, stopNote, nearEmit) runs on the audio thread
 * inside renderChunk. The comments above the texture setters and above waitForQuiet() explain the
 * one rule that keeps the two apart: a buffer is only written once every block that could still be
 * holding it has ended.
 */
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

Engine::Engine()
{
    // The envelopes start from a usable shape rather than from nothing.
    for (auto& e : m_envPending) e.parse("0:0/1:1/4:0");
    for (auto& e : m_envShape) e.parse("0:0/1:1/4:0");
    for (auto& e : m_srcEnvPending) e.parse(SrcEnvDefault);
    for (auto& e : m_srcEnvShape) e.parse(SrcEnvDefault);
    for (int i = 0; i < NumParams; ++i) {
        const float def = paramTable()[static_cast<size_t>(i)].m_def;
        m_params[i].store(def, std::memory_order_relaxed);
        m_slotA[i].store(def, std::memory_order_relaxed);
        m_slotB[i].store(def, std::memory_order_relaxed);
    }
    for (int i = 0; i < UserScaleIndex; ++i) makeBuiltinScale(i, m_scales[i]);
    makeBuiltinScale(0, m_scales[UserScaleIndex]);
    std::strncpy(m_scales[UserScaleIndex].m_name, "User (Scala)", sizeof(FixedScale::m_name) - 1);
    // The timbre scale starts as twelve equal steps and is replaced the moment it is asked for.
    makeBuiltinScale(0, m_scales[TimbreScaleIndex]);
    std::strncpy(m_scales[TimbreScaleIndex].m_name, "Timbre (Sethares)", sizeof(FixedScale::m_name) - 1);
    m_scale = &m_scales[3];
    m_masterSmooth.snap(dbToGain(-6.0f));
    for (auto& d : m_noteDistance) d.store(-1.0f, std::memory_order_relaxed);
    for (auto& l : m_noteLevel) l.store(0.0f, std::memory_order_relaxed);
}

void Engine::prepare(double sampleRate, int maxBlockSize)
{
    m_sr = sampleRate;
    m_maxBlock = std::max(maxBlockSize, ControlBlock);
    // The map's cached preset vectors are NOT built here. With the library loaded that is eight
    // thousand presets and it measured 1.75 s of the 1.78 s this function took -- paid on every
    // plugin instance, every offline render, every sample-rate change, whether or not the map was
    // ever switched on. The host builds it in the background (PresetMap::warmupAsync); until it
    // is ready the map does not engage, and everything else in the instrument is unaffected.
    for (auto* s : { &m_smDelayMix, &m_smDelayToFar, &m_smDelay2Mix, &m_smDelay2ToFar, &m_smCloudSend, &m_smCosmosSend, &m_smCosmosReturn, &m_smCosmosToFar, &m_smFarLevel, &m_smFarWidth, &m_smEnvelop, &m_smComod,
                     &m_smMemSend, &m_smMemReturn, &m_smMemToFar })
        s->setTime(0.02f, m_sr);
    m_smDelayMix.snap(getParam(ParamIdE::DELAY_MIX)); m_smDelayToFar.snap(getParam(ParamIdE::DELAY_TO_FAR));
    m_smDelay2Mix.snap(getParam(ParamIdE::DELAY2_MIX)); m_smDelay2ToFar.snap(getParam(ParamIdE::DELAY2_TO_FAR));
    m_smCloudSend.snap(getParam(ParamIdE::CLOUD_SEND)); m_smCosmosSend.snap(getParam(ParamIdE::COSMOS_SEND));
    m_smMemSend.snap(getParam(ParamIdE::MEM_SEND)); m_smMemReturn.snap(getParam(ParamIdE::MEM_RETURN)); m_smMemToFar.snap(getParam(ParamIdE::MEM_TO_FAR));
    m_smCosmosReturn.snap(getParam(ParamIdE::COSMOS_RETURN)); m_smCosmosToFar.snap(getParam(ParamIdE::COSMOS_TO_FAR));
    m_smFarLevel.snap(getParam(ParamIdE::FAR_LEVEL)); m_smFarWidth.snap(getParam(ParamIdE::FAR_WIDTH));
    for (int i = 0; i < NumParams; ++i) m_blendCur[i].store(getParam(static_cast<ParamIdE>(i)), std::memory_order_relaxed);
    m_blendActive.store(false, std::memory_order_relaxed);
    for (auto* b : { &m_nearL, &m_nearR, &m_farL, &m_farR, &m_wetL, &m_wetR, &m_dryL, &m_dryR, &m_foreD2L, &m_foreD2R, &m_foreCoL, &m_foreCoR, &m_cosL, &m_cosR, &m_nebL, &m_nebR, &m_shimL, &m_shimR, &m_fbInL, &m_fbInR, &m_fbMono, &m_memL, &m_memR,
                     &m_cloudL, &m_cloudR,
                     &m_roomInL, &m_roomInR, &m_roomOutL, &m_roomOutR })
        b->assign(static_cast<size_t>(m_maxBlock), 0.0f);
    {   // Room: pre-delay ring (>= 300 ms) and the convolver with a generated hall unless a file was set
        int ring = 1; while (ring < static_cast<int>(0.32 * m_sr) + m_maxBlock) ring <<= 1;
        m_roomDelayL.assign(static_cast<size_t>(ring), 0.0f); m_roomDelayR.assign(static_cast<size_t>(ring), 0.0f);
        m_roomDelayMask = ring - 1; m_roomDelayW = 0; m_roomLpL = m_roomLpR = 0.0f; m_roomLevelCur = 0.0f; m_roomTailLeft = 0;
        const bool keepUser = m_userImpulse && m_room.hasImpulse() && std::fabs(m_room.impulseSeconds()) > 0.0f;
        std::vector<float> keepL, keepR;   // a user impulse survives a sample-rate change only through the host reloading it
        m_room.prepare(m_sr, m_roomMaxSeconds);
        m_hasImpulseB = false;   // prepare empties both impulses; a B comes back only when it is loaded again
        if (!keepUser) { m_room.generateDefault(static_cast<uint64_t>(m_seed) + 7); m_userImpulse = false; }
        (void)keepL; (void)keepR;
    }
    {
        // A block behind, plus as far back as the tape's wow and flutter reach (0.33 % of a
        // second) -- two blocks alone were shorter than that at small host buffers, and the head
        // wrapped round to the previous pass.
        int ring = 1; while (ring < 2 * m_maxBlock + static_cast<int>(0.005 * m_sr) + 16) ring <<= 1;
        m_fbRingL.assign(static_cast<size_t>(ring), 0.0f);
        m_fbRingR.assign(static_cast<size_t>(ring), 0.0f);
        m_fbMask = ring - 1; m_fbW = 0;
        m_fbLpL = m_fbLpR = m_fbEnv = 0.0f;
    }
    m_seed = static_cast<int>(getParam(ParamIdE::SEED));
    m_rng.seed(static_cast<uint64_t>(m_seed) + 1);
    for (auto& v : m_voices) v.prepare(m_sr, m_rng.fork());
    // Two tables built on first use: the built-in wavetables and the Z-plane grid. Their first
    // use was on the audio thread, under the lock the language puts round a static's
    // construction. Touched here so that they exist before any block is rendered.
    (void)builtinTable(0);
    zWarmTables();
    zWarmShapes();
    m_arc.init(m_rng);
    m_shiftDrift.init(m_rng);
    m_ensemble.prepare(m_sr);
    m_delay.prepare(m_sr);
    m_delay2.prepare(m_sr);
    m_cloud.prepare(m_sr, m_rng.fork());
    m_memory.prepare(m_sr, 0x4D454D4F52ull);   // its own stream: a fork here would move every voice's dice
    m_nearReverb.prepare(m_sr);
    m_farReverb.prepare(m_sr);
    m_midSide.prepare(m_sr);
    m_shifter.prepare(m_sr);
    m_resonator.prepare(m_sr);
    m_vowel.prepare(m_sr, m_rng.fork());
    m_nebula.prepare(m_sr, m_rng.fork());
    m_shimmerL.prepare(m_sr);
    m_shimmerR.prepare(m_sr);
    m_shimmerSpec.prepare(m_sr);
    m_shimmerWasOn = false;
    m_shimmerLpL = m_shimmerLpR = 0.0f;
    m_masterSmooth.setTime(0.02f, m_sr);
    m_loudness.prepare(m_sr);
    m_smBlur.setTime(0.02f, m_sr);
    m_smBody.setTime(0.02f, m_sr);
    m_unmask.prepare(m_sr);
    m_roomUnmask.prepare(m_sr);
    m_smRoomToFar.setTime(0.02f, m_sr);
    m_smRoomToFar.snap(getParam(ParamIdE::ROOM_TO_FAR));
    m_farMidHp.reset();
    m_subPeak = 0.0f; m_subLimGain = 1.0f;
    m_airOsL.reset(); m_airOsR.reset(); m_fbOsL.reset(); m_fbOsR.reset();
    m_airOsOn = m_fbOsOn = false;
    m_haas.prepare(m_sr);
    m_early.prepare(m_sr);
    m_diffuser.prepare(m_sr);
    m_coupleBuf.assign(static_cast<size_t>(m_maxBlock), 0.0f);
    m_smRoomMorph.setTime(0.05f, m_sr);
    m_body.prepare(m_sr, 0xB0D1B0D1ull);
    m_patina.prepare(m_sr, 0x9A7104ull);
    m_blur.prepare(m_sr, 0x5EED5EEDull);
    m_auxRng.seed(0xA5A5A5A5ull);
    m_strikeRng.seed(0x57A1CEull);
    m_near.reset(0x4E454152ull + static_cast<uint64_t>(m_seed + 1));
    m_nearPickRng = 0x9E3779B9u ^ (static_cast<uint32_t>(m_seed + 1) * 2654435761u);   // the pool's draw, never zero
    if (m_nearPickRng == 0u) m_nearPickRng = 0x9E3779B9u;   // its own stream, from the seed
    m_tideDrift.init(m_auxRng); m_rotDrift.init(m_auxRng);
    m_vecDriftX.init(m_auxRng); m_vecDriftY.init(m_auxRng);
    m_dcXL = m_dcXR = m_dcYL = m_dcYR = 0.0f;
    m_pdcXL = m_pdcXR = m_pdcYL = m_pdcYR = 0.0f;
    m_lastRootPc = -1;
    readParams();
    m_brain.reset(m_rng.fork(), m_rootNote - 12);   // the brain's root lives an octave below the key root
    m_brain2.reset(0x2B2A1Full, m_rootNote - 12);  // its own stream, so switching it on never moves the first one
    for (auto& h : m_midiHeld) h = false;
}

void Engine::reset()
{
    for (auto& v : m_voices) v.kill();
    for (auto& h : m_midiHeld) h = false;
    m_brain.reset(m_rng.fork(), m_rootNote - 12);
    m_strikeRng.seed(0x57A1CEull);   // its own stream, so a reset gives the same piece back
    m_strikeExcAvg = 0.0;
    m_near.reset(0x4E454152ull + static_cast<uint64_t>(m_seed + 1));
    m_nearPickRng = 0x9E3779B9u ^ (static_cast<uint32_t>(m_seed + 1) * 2654435761u);   // the pool's draw, never zero
    if (m_nearPickRng == 0u) m_nearPickRng = 0x9E3779B9u;
}

bool Engine::applyPreset(int index)
{
    if (index < 0 || index >= numPresets()) return false;
    const Preset& p = preset(index);
    const bool ok = ambient::applyPreset(p, [this](ParamIdE id, float v) { setParam(id, v); });
    return applyPresetModulation(p) && ok;
}

bool Engine::applySoundPreset(int index)
{
    if (index < 0 || index >= numPresets()) return false;
    const Preset& p = preset(index);
    // Modulation belongs to the sound layer, not to Cosmos: loading a sound preset brings its
    // matrix and its envelope shapes with it.
    const bool ok = ambient::applyPreset(p, [this](ParamIdE id, float v) { setParam(id, v); }, PresetScopeE::SOUND);
    return applyPresetModulation(p) && ok;
}

bool Engine::applyCosmosPreset(int index)
{
    if (index < 0 || index >= numCosmosPresets()) return false;
    return ambient::applyPreset(cosmosPreset(index), [this](ParamIdE id, float v) { setParam(id, v); }, PresetScopeE::COSMOS);
}

bool Engine::applyZPreset(int index)
{
    if (index < 0 || index >= numZPresets()) return false;
    return ambient::applyPreset(zPreset(index), [this](ParamIdE id, float v) { setParam(id, v); }, PresetScopeE::Z_PLANE);
}

bool Engine::applyStrikePreset(int index)
{
    if (index < 0 || index >= numStrikePresets()) return false;
    return ambient::applyPreset(strikePreset(index), [this](ParamIdE id, float v) { setParam(id, v); }, PresetScopeE::STRIKE);
}

bool Engine::applyNearPreset(int index)
{
    if (index < 0 || index >= numNearPresets()) return false;
    return ambient::applyPreset(nearPreset(index), [this](ParamIdE id, float v) { setParam(id, v); }, PresetScopeE::NEAR_LAYER);
}

// ---------------------------------------------------------------- morph

void Engine::setMorphSlot(int slot, const float* values)
{
    auto& s = (slot == 0) ? m_slotA : m_slotB;
    for (int i = 0; i < NumParams; ++i) s[i].store(values[i], std::memory_order_relaxed);
    // Values first, the flag after: the audio thread reads the flag to decide whether to read the
    // values at all, so it must never see "chosen" over a half-written snapshot.
    m_slotSet[slot & 1].store(true, std::memory_order_relaxed);
}

void Engine::captureMorphSlot(int slot)
{
    auto& s = (slot == 0) ? m_slotA : m_slotB;
    for (int i = 0; i < NumParams; ++i) s[i].store(m_params[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_slotSet[slot & 1].store(true, std::memory_order_relaxed);
}

void Engine::clearMorphSlot(int slot)
{
    m_slotSet[slot & 1].store(false, std::memory_order_relaxed);
}

void Engine::morphSlot(int slot, float* out) const
{
    const auto& s = (slot == 0) ? m_slotA : m_slotB;
    for (int i = 0; i < NumParams; ++i) out[i] = s[i].load(std::memory_order_relaxed);
}

bool Engine::routeStep(double dt, float& x, float& y, float& radius)
{
    // Pick up an edit made on the message thread (fixed-size object, no allocation), and restart
    // from the cursor so a route changed while it plays does not jump.
    const int rv = m_routeVersion.load(std::memory_order_acquire);
    if (rv != m_routeSeen) { m_route = m_routePending; m_routeSeen = rv; m_routeWasActive = false; }
    const bool active = getParam(ParamIdE::ROUTE_ACTIVE) >= 0.5f && m_route.count() > 0;
    if (active && !m_routeWasActive) {   // (re)start from where the cursor is now
        m_route.start(getParam(ParamIdE::MAP_X), getParam(ParamIdE::MAP_Y), getParam(ParamIdE::MAP_RADIUS), getParam(ParamIdE::ROUTE_LOOP) >= 0.5f);
    }
    if (!active && m_routeWasActive) m_route.stop();
    m_routeWasActive = active;
    if (!active) { x = getParam(ParamIdE::MAP_X); y = getParam(ParamIdE::MAP_Y); radius = getParam(ParamIdE::MAP_RADIUS); return false; }
    const bool running = m_route.update(static_cast<float>(dt) * getParam(ParamIdE::ROUTE_SPEED), x, y, radius);
    setParam(ParamIdE::MAP_X, x); setParam(ParamIdE::MAP_Y, y); setParam(ParamIdE::MAP_RADIUS, radius);
    if (getParam(ParamIdE::MAP_ACTIVE) < 0.5f) setParam(ParamIdE::MAP_ACTIVE, 1.0f);   // a route always plays through the map
    if (!running) setParam(ParamIdE::ROUTE_ACTIVE, 0.0f);                              // ended (not looping): the cursor stays
    return running;
}

void Engine::updateBlend(int n)
{
    // Map mode: blend the presets around the cursor, glide every parameter toward it.
    const bool on = getParam(ParamIdE::MAP_ACTIVE) >= 0.5f && PresetMap::ready() && numPresetMeta() > 0;
    const bool was = m_blendActive.load(std::memory_order_relaxed);
    if (!on) { if (was) m_blendActive.store(false, std::memory_order_relaxed); return; }
    if (!was) {   // start from what is sounding now: no jump when the map takes over
        for (int i = 0; i < NumParams; ++i) m_blendCur[i].store(effectiveParam(static_cast<ParamIdE>(i)), std::memory_order_relaxed);
        m_blendActive.store(true, std::memory_order_relaxed);
    }
    // The neighbours and the blended target only change when the cursor does. Standing still is
    // the normal case -- a hand resting on the map, a route between two points -- and the search
    // reads every preset's position while the blend runs three powers over every float parameter.
    // Both were done on every block for an answer that was the same as the block before.
    const float mx = getParam(ParamIdE::MAP_X), my = getParam(ParamIdE::MAP_Y), mr = getParam(ParamIdE::MAP_RADIUS);
    if (mx != m_blendX || my != m_blendY || mr != m_blendR || !m_blendHave) {
        m_blendX = mx; m_blendY = my; m_blendR = mr; m_blendHave = true;
        PresetMap::blend(PresetMap::neighbours(mx, my, mr), m_blendTarget);
    }
    const float glide = std::max(getParam(ParamIdE::MORPH_GLIDE), 0.05f);
    const float coef = 1.0f - std::exp(-static_cast<float>(n / m_sr) / (glide / 3.0f));   // ~95 % after `glide` seconds
    for (const ParamDesc& d : paramTable()) {
        const int i = static_cast<int>(d.m_id);
        if (isPerformanceParam(d.m_id)) { m_blendCur[i].store(getParam(d.m_id), std::memory_order_relaxed); continue; }
        const float cur = m_blendCur[i].load(std::memory_order_relaxed), tgt = m_blendTarget[i];
        float next;
        switch (d.m_kind) {
        case ParamKindE::FLOAT: {
            const float span = std::max(d.m_max - d.m_min, 1e-9f);
            // Arrived is arrived: three powers for a parameter that is already on its target is
            // the same waste readParams was making, and here it is every float on every block.
            if (std::fabs(tgt - cur) <= 1.0e-7f * span) { next = tgt; break; }
            const float pc = std::pow(clampv((cur - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
            const float pt = std::pow(clampv((tgt - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
            next = d.m_min + span * std::pow(pc + (pt - pc) * coef, 1.0f / d.m_skew);
            break;
        }
        case ParamKindE::INT:
            next = cur + (tgt - cur) * coef;
            break;
        default:
            next = tgt;   // choices and switches follow the strongest neighbour
        }
        m_blendCur[i].store(next, std::memory_order_relaxed);
    }
}

float Engine::effectiveParam(ParamIdE id) const
{
    const float live = getParam(id);
    if (m_blendActive.load(std::memory_order_relaxed) && !isPerformanceParam(id)) {
        const float v = m_blendCur[static_cast<int>(id)].load(std::memory_order_relaxed);
        return paramDesc(id).m_kind == ParamKindE::INT ? static_cast<float>(std::lround(v)) : v;
    }
    if (isMorphParam(id) || getParam(ParamIdE::MORPH_ACTIVE) < 0.5f) return live;
    // An end nobody has chosen is the sound as the knobs have it, not the defaults. With neither
    // chosen Morph is therefore silent in effect, and with one chosen it runs from what is playing
    // now to that one -- which is what "morph to B" means to anyone who has not read this line.
    const bool setA = m_slotSet[0].load(std::memory_order_relaxed), setB = m_slotSet[1].load(std::memory_order_relaxed);
    if (!setA && !setB) return live;
    const int i = static_cast<int>(id);
    const float a = setA ? m_slotA[i].load(std::memory_order_relaxed) : live;
    const float b = setB ? m_slotB[i].load(std::memory_order_relaxed) : live;
    const float t = m_morphCur.load(std::memory_order_relaxed);
    const ParamDesc& d = paramDesc(id);
    switch (d.m_kind) {
    case ParamKindE::FLOAT: {
        // Interpolate in the skewed (perceptual) domain, like the host's knob travel.
        const float span = std::max(d.m_max - d.m_min, 1e-9f);
        const float pa = std::pow(clampv((a - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
        const float pb = std::pow(clampv((b - d.m_min) / span, 0.0f, 1.0f), d.m_skew);
        const float p = pa + (pb - pa) * t;
        return d.m_min + span * std::pow(p, 1.0f / d.m_skew);
    }
    case ParamKindE::INT:
        return static_cast<float>(std::lround(a + (b - a) * t));
    default:
        return t < 0.5f ? a : b;   // choices and switches flip halfway
    }
}

void Engine::setUserScale(const FixedScale& s)
{
    while (m_userBusy.load(std::memory_order_acquire)) { /* audio thread is copying, microseconds */ }
    m_userPending = s;
    m_userVersion.fetch_add(1, std::memory_order_release);
}

void Engine::setUserWavetable(const Wavetable& t)
{
    while (m_tableBusy.load(std::memory_order_acquire)) { /* audio thread copying, microseconds */ }
    m_userTablePending = t;
    m_userTableFrames.store(t.m_frames, std::memory_order_relaxed);
    m_tableVersion.fetch_add(1, std::memory_order_release);
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
    const int active = m_cyclesActive.load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    m_userCycles[target] = t;
    m_cyclesActive.store(target, std::memory_order_release);
}

/**
 * @brief Message thread: returns once every block that had begun by the time it was called has ended.
 *
 * After that, any block still running began later and therefore read whatever this thread had
 * already published -- which is what makes the other half of a double buffer safe to write.
 *
 * There is no guess in it. When the audio thread is not running the two counters are equal on the
 * first look and it returns at once, which is a host with its transport stopped; when a block is
 * in flight it waits for that block and no longer, whether that block is 16 samples or 2048. The
 * version before this one counted finished blocks and gave up after two milliseconds of seeing
 * none, on the theory that nothing was rendering -- which is also what a slow block looks like
 * from outside, and a 2048-sample block at 44.1 kHz is 46 milliseconds long.
 */
void Engine::waitForQuiet()
{
    const unsigned long long begun = m_blocksBegun.load(std::memory_order_acquire);
    for (int spin = 0; spin < 8000; ++spin) {
        if (m_blocksDone.load(std::memory_order_acquire) >= begun) return;
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

/**
 * @brief A recording's offset, taken out where it comes in.
 *
 * A field recording, a rendered texture, a
 * phrase cut from a radio play: a few of them carry a direct current of a few per cent, which no
 * grain window removes and which every long loop downstream -- the far hall above all -- would
 * integrate until the master's clipper rails on it (three presets of the 2.0 library went silent
 * that way). Subtracting the mean changes nothing anyone hears and keeps a seamless clip seamless.
 *
 * Message thread, on a copy that is not yet published. The mean is summed in double and taken out
 * in place; a mean under a millionth is left alone, so a clip that is already centred is not touched.
 *
 * @param p       the first sample of the run; a null pointer or a count of zero does nothing
 * @param count   how many samples the run holds, `stride` apart
 * @param stride  the step between two samples of the run: 1 for a mono buffer, 2 for one channel
 *                of an interleaved stereo pair
 */
static void removeOffset(float* p, size_t count, size_t stride)
{
    if (p == nullptr || count == 0) return;
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) sum += p[i * stride];
    const float mean = static_cast<float>(sum / static_cast<double>(count));
    if (std::fabs(mean) < 1.0e-6f) return;
    for (size_t i = 0; i < count; ++i) p[i * stride] -= mean;
}

/**
 * @brief The offset taken out of every channel a texture holds: the mono sum, and where the clip is
 *        stereo the left and the right of the interleaved pair, each on its own.
 *
 * Called by every texture setter and by makeTexture(), on the buffer that is about to be published
 * and before Texture::measure(), so the audio thread never plays a clip with a direct current in it
 * and the measurements describe the clip as it will be heard.
 *
 * @param t  the texture whose mono and lr buffers are corrected in place
 */
static void removeOffset(Texture& t)
{
    removeOffset(t.m_mono.data(), t.m_mono.size(), 1);
    if (!t.m_lr.empty()) {
        removeOffset(t.m_lr.data(), t.m_lr.size() / 2, 2);
        removeOffset(t.m_lr.data() + 1, t.m_lr.size() / 2, 2);
    }
}

void Engine::setTexture(int slot, const float* L, const float* R, int n, double sampleRate, double baseHz, bool seamless)
{
    if (R == nullptr) { setTexture(slot, L, n, sampleRate, baseHz, seamless); return; }
    if (slot < 0 || slot >= Slots || L == nullptr) return;
    const int len = std::max(n, 0);
    // The mono sum for the analysis and for the types that read one signal, and the interleaved
    // pair for the grain loop. Built here rather than in every caller, so a host, the render tool
    // and the Quest cannot disagree about what the sum is.
    std::vector<float> mono(static_cast<size_t>(len), 0.0f);
    for (int i = 0; i < len; ++i) mono[static_cast<size_t>(i)] = 0.5f * (L[i] + R[i]);
    waitForQuiet();
    const int active = m_textureActive[slot].load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    Texture& t = m_textures[slot][target];
    t.m_mono = std::move(mono);
    t.m_lr.assign(static_cast<size_t>(2 * len), 0.0f);
    for (int i = 0; i < len; ++i) { t.m_lr[static_cast<size_t>(2 * i)] = L[i]; t.m_lr[static_cast<size_t>(2 * i + 1)] = R[i]; }
    removeOffset(t);
    t.m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    t.m_baseHz = baseHz > 0.0 ? baseHz : 261.6256;
    t.m_seamless = seamless;
    t.measure();
    m_textureActive[slot].store(target, std::memory_order_release);
}

void Engine::setTexture(int slot, const float* mono, int n, double sampleRate, double baseHz, bool seamless)
{
    if (slot < 0 || slot >= Slots) return;
    const int active = m_textureActive[slot].load(std::memory_order_acquire);
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
    Texture& t = m_textures[slot][target];
    t.m_mono.assign(mono, mono + std::max(n, 0));
    t.m_lr.clear();          // this buffer may have held a stereo clip: a mono one is mono again
    removeOffset(t);
    t.m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    t.m_baseHz = baseHz > 0.0 ? baseHz : 261.6256;
    t.m_seamless = seamless;
    t.measure();
    m_textureActive[slot].store(target, std::memory_order_release);
}

void Engine::setTexture(int slot, const Texture& src)
{
    if (slot < 0 || slot >= Slots || src.empty()) return;
    waitForQuiet();
    const int active = m_textureActive[slot].load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    m_textures[slot][target] = src;
    m_textureActive[slot].store(target, std::memory_order_release);
}

void Engine::clearTexture(int slot)
{
    if (slot < 0 || slot >= Slots) return;
    m_textureActive[slot].store(-1, std::memory_order_release);
}

Texture Engine::makeTexture(const float* L, const float* R, int n, double sampleRate, double baseHz, bool seamless, double maxSeconds)
{
    Texture t;
    if (L == nullptr || n <= 0) return t;
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int len = std::min(n, static_cast<int>(std::max(1.0, maxSeconds) * sr));
    t.m_mono.assign(static_cast<size_t>(len), 0.0f);
    for (int i = 0; i < len; ++i) t.m_mono[static_cast<size_t>(i)] = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
    if (R != nullptr) {
        t.m_lr.assign(static_cast<size_t>(2 * len), 0.0f);
        for (int i = 0; i < len; ++i) { t.m_lr[static_cast<size_t>(2 * i)] = L[i]; t.m_lr[static_cast<size_t>(2 * i + 1)] = R[i]; }
    }
    removeOffset(t);
    t.m_sampleRate = sr;
    t.m_baseHz = baseHz > 0.0 ? baseHz : 261.6256;
    t.m_seamless = seamless;
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
    const int active = m_nearPoolActive.load(std::memory_order_acquire);
    const int target = active < 0 ? 0 : 1 - active;
    m_nearPools[target] = std::move(pool);
    m_nearPoolActive.store(target, std::memory_order_release);
}

/**
 * @brief Where Match would put partial h: on the degree of the current scale nearest to it, counted in
 *        periods of that scale from the fundamental.
 *
 * For a twelve-tone scale the seventh partial moves
 * from 3369 cents to 3400 and the fifth from 2786 to 2800 -- small moves, and after them a chord
 * in that scale does not beat. The timbre scale is excluded, because it is itself computed from
 * the spectrum: a scale that follows the partials and partials that follow the scale would chase
 * each other round in a circle, and neither would mean anything.
 */
double Engine::matchedPartialRatio(int h) const
{
    if (h < 1) return 1.0;
    const FixedScale& s = *m_scale;
    if (m_scale == &m_scales[TimbreScaleIndex] || s.m_count <= 0 || !(s.m_period > 1.0)) return static_cast<double>(h);
    const double logP = std::log(s.m_period);
    const double x = std::log(static_cast<double>(h)) / logP;         // the partial, in periods above f0
    const int n = static_cast<int>(std::floor(x));
    const double within = std::pow(s.m_period, x - n);                     // 1 .. period
    double best = s.m_ratios[0], bestErr = 1e9;
    for (int d = 0; d <= s.m_count; ++d) {
        const double r = d < s.m_count ? s.m_ratios[d] : s.m_period;
        const double err = std::fabs(std::log(r / within));
        if (err < bestErr) { bestErr = err; best = r; }
    }
    return std::pow(s.m_period, n) * best;
}

double Engine::frequencyOf(int note) const
{
    // Purity blends between 12-TET (0) and the chosen scale (1) in the log domain: at 1 the
    // partials of different notes lock, at 0 they beat like a piano; in between the beating
    // slows down as the intervals close in on their ratios.
    const double pure = scaleFrequency(*m_scale, note, m_rootNote, m_refPitch, m_snapKeys);
    const double p = m_purityCur;
    double f = pure;
    if (p < 0.9999) {
        const double et = m_refPitch * std::pow(2.0, (note - 69) / 12.0);
        f = std::exp(std::log(et) + (std::log(pure) - std::log(et)) * clampv(p, 0.0, 1.0));
    }
    // Adaptive intonation: the note's own offset and the shared comma offset, both in cents,
    // scaled by the amount so that the knob glides everything home rather than switching it.
    if (m_adaptAmt > 0.0f && note >= 0 && note < 128)
        f *= std::pow(2.0, static_cast<double>(m_adaptAmt) * (static_cast<double>(m_adaptCents[note]) + m_commaCents) / 1200.0);
    // Transpose: the pure interval, applied to everything at once so every ratio inside is kept.
    if (m_transposeCur != 0.0) f *= std::pow(2.0, m_transposeCur);
    // The stretched octave. Listeners prefer octaves a little wider than 2:1 -- ten to twenty
    // cents at the extremes of the range (Ward 1954; Terhardt) -- and a piano is tuned that
    // way (the Railsback curve). Every octave away from the reference pitch is widened by
    // Stretch cents, in both directions, so the reference itself does not move:
    //     log2 f' = log2 A4 + (1 + s/1200) (log2 f - log2 A4)
    if (m_stretchCents > 0.0f && f > 0.0 && m_refPitch > 0.0)
        f = m_refPitch * std::pow(f / m_refPitch, 1.0 + m_stretchCents / 1200.0);
    return f;
}

/**
 * @brief The pure offset for a note arriving into a chord.
 *
 * Against every sounding voice the interval
 * is reduced to an octave and matched to the nearest small-integer ratio (5-limit and the
 * septimal tritone); the offset that would make that interval exact is weighted by the ratio's
 * simplicity -- the fifth speaks louder than the minor seventh -- and the weighted mean is the
 * answer, capped at thirty cents so a note that fits nothing is not thrown across a quarter tone.
 * The sounding voices' frequencies come through frequencyOf, so they carry their own offsets
 * and the shared comma, and a note tuned against them lands pure in the world as it is now.
 */
float Engine::adaptiveOffset(int note) const
{
    if (note < 0 || note > 127) return 0.0f;
    static const struct { int m_num, m_den; } Ratios[] = {
        { 1, 1 }, { 16, 15 }, { 9, 8 }, { 6, 5 }, { 5, 4 }, { 4, 3 }, { 7, 5 }, { 3, 2 }, { 8, 5 }, { 5, 3 }, { 9, 5 }, { 15, 8 } };
    // The plain frequency: what the note gets with no offset of its own (the comma still applies,
    // and cancels in every ratio below because the sounding voices carry it too).
    double plain = frequencyOf(note);
    if (m_adaptAmt > 0.0f) plain /= std::pow(2.0, static_cast<double>(m_adaptAmt) * static_cast<double>(m_adaptCents[note]) / 1200.0);
    double sum = 0.0, weight = 0.0;
    for (const auto& v : m_voices) {
        if (!v.isActive() || v.note() == note) continue;
        const double g = frequencyOf(v.note());
        if (!(g > 0.0) || !(plain > 0.0)) continue;
        double r = plain / g;
        int oct = 0;
        while (r >= 2.0) { r *= 0.5; ++oct; }
        while (r < 1.0)  { r *= 2.0; --oct; }
        double bestCents = 1e9; int bestNum = 1, bestDen = 1;
        for (const auto& q : Ratios) {
            const double c = 1200.0 * std::log2(static_cast<double>(q.m_num) / q.m_den / r);   // + : the pure ratio is above
            if (std::fabs(c) < std::fabs(bestCents)) { bestCents = c; bestNum = q.m_num; bestDen = q.m_den; }
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
    static const char* const names[NumStems] = { "near", "far", "cosmos", "room" };
    return (i >= 0 && i < NumStems) ? names[i] : "";
}

const Voice* Engine::loudestVoice() const
{
    const Voice* best = nullptr;
    for (const auto& v : m_voices)
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
    const int k = slot < 0 ? 0 : (slot >= Slots ? Slots - 1 : slot);
    const int a = m_textureActive[k].load(std::memory_order_relaxed);
    const int len = a >= 0 ? static_cast<int>(m_textures[k][a].m_mono.size()) : 0;
    return (v != nullptr && len > 0) ? v->displayGrains(slot, out, maxCount, len) : 0;
}

int Engine::voiceStage(VoiceStage* out, int maxCount) const
{
    int n = 0;
    for (const auto& v : m_voices) {
        if (n >= maxCount) break;
        if (!v.isActive()) continue;
        out[n++] = { v.pan(), v.distance(), v.level(), v.note(), v.owner() };
    }
    return n;
}

int Engine::cosmosTap(float* out, int n) const
{
    n = clampv(n, 0, 4096);
    const int w = m_cosTapW;
    for (int i = 0; i < n; ++i) out[i] = m_cosTap[(w - n + i) & 4095];
    return n;
}

int Engine::outputTap(float* out, int n) const
{
    n = clampv(n, 0, OutTapLen);
    const int w = m_outTapW;
    for (int i = 0; i < n; ++i) out[i] = m_outTap[(w - n + i) & (OutTapLen - 1)];
    return n;
}

void Engine::soundingNotes(bool (&out)[128]) const
{
    const uint64_t m0 = m_mask[0].load(std::memory_order_relaxed), m1 = m_mask[1].load(std::memory_order_relaxed);
    for (int i = 0; i < 64; ++i)  out[i] = (m0 >> i) & 1u;
    for (int i = 0; i < 64; ++i)  out[64 + i] = (m1 >> i) & 1u;
}

float Engine::noteDistance(int note) const
{
    if (note < 0 || note > 127) return -1.0f;
    return m_noteDistance[note].load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------- notes

Voice* Engine::allocate(int note, int owner)
{
    for (auto& v : m_voices) if (v.isActive() && v.note() == note && v.owner() == owner) return &v;
    for (auto& v : m_voices) if (!v.isActive()) return &v;
    Voice* best = nullptr;
    for (auto& v : m_voices) if (v.isReleasing() && (best == nullptr || v.level() < best->level())) best = &v;
    if (best) return best;
    for (auto& v : m_voices) if (best == nullptr || v.m_order < best->m_order) best = &v;
    return best;
}

void Engine::startNote(int note, float velocity, int owner, float distance, float ageSeconds)
{
    if (note < 0 || note > 127) return;
    if (m_noteTap != nullptr && tapChannel(owner) > 0) m_noteTap->add(m_tapPos, tapChannel(owner), note, velocity, true);   // MIDI out
    // The modulation envelopes run on a phrase clock: it restarts when a note arrives into
    // silence, not on every note of a cluster, or a slow shape would never get anywhere.
    bool anySounding = false;
    for (const auto& v : m_voices) if (v.isActive()) { anySounding = true; break; }
    // A note arriving into silence starts a phrase: the modulation envelopes go back to zero, and
    // so does any LFO set to Retrigger. That mode had been stored by two thousand seven hundred
    // presets and read by nobody -- Lfo::step uses the rate, the phase and the depth and has never
    // looked at the mode at all (12.09.2026).
    if (!anySounding) { for (double& t : m_envTime) t = 0.0; m_phraseStart = true; }
    m_randomPerNote = m_rng.bipolar();
    Voice* v = allocate(note, owner);
    v->m_order = ++m_order;
    // Adaptive intonation: the offset is decided once, as the note arrives, against what is
    // sounding then; a note already sounding in another voice keeps the offset it has.
    if (m_adaptAmt > 0.0f) {
        bool already = false;
        for (const auto& x : m_voices) if (&x != v && x.isActive() && x.note() == note) already = true;
        if (!already) m_adaptCents[note] = adaptiveOffset(note);
    } else m_adaptCents[note] = 0.0f;
    const double hz = frequencyOf(note);
    // Does this note strike? Yours always does -- you played it. The conductor's is a coin at
    // Chance, and with Cluster up the coin is weighted by the cascade's excitation, so the
    // strikes arrive where the events already crowd together and the long gaps stay empty.
    // At Chance 1 no coin is thrown at all, which is what keeps every older preset bit for bit.
    bool allowStrike = true;
    if (owner != OWNER_MIDI && m_vp.m_strikeLevel > 0.0f && m_vp.m_strikeBrain && m_strikeChance < 0.999f) {
        const double exc = (owner == OWNER_BRAIN2 ? m_brain2 : m_brain).excitation();
        m_strikeExcAvg += 0.1 * (exc - m_strikeExcAvg);   // what this piece's cascade usually runs at
        const double lift = 1.0 + 2.0 * m_strikeCluster * (exc - m_strikeExcAvg);
        const double p = std::clamp(static_cast<double>(m_strikeChance) * lift, 0.0, 1.0);
        allowStrike = m_strikeRng.uniform() < p;
    }
    // Where the note stands among its owner's notes, for the slot roles: the lowest, the highest,
    // or between. The keys are one group, the two conductors another (the shadow plays in the
    // same cluster), the near events a third. Only computed where a slot has a role at all.
    if (m_rolesUsed) {
        int lo = 128, hi = -1;
        const int mine = owner == OWNER_MIDI ? 0 : (owner == OWNER_NEAR ? 2 : 1);
        for (const auto& x : m_voices) {
            if (&x == v || !x.isActive() || x.isReleasing()) continue;
            const int g = x.owner() == OWNER_MIDI ? 0 : (x.owner() == OWNER_NEAR ? 2 : 1);
            if (g != mine) continue;
            lo = std::min(lo, x.note()); hi = std::max(hi, x.note());
        }
        v->setPlace(note <= lo, note >= hi);
    } else v->setPlace(true, true);
    v->noteOn(note, hz, velocity, owner, distance, m_vp, allowStrike, ageSeconds);
    if (m_rolesUsed) updatePlaces();   // the others' places have changed with this note's arrival
    if (owner == OWNER_MIDI) {   // portamento: a key slides in from the previous key
        if (m_portamento > 0.0f && m_lastKeyHz > 0.0 && std::fabs(m_lastKeyHz - hz) > 1e-6) v->glideFrom(m_lastKeyHz, m_portamento, m_portaGravity);
        m_lastKeyHz = hz;
    }
}

void Engine::stopNote(int note, int owner)
{
    if (m_noteTap != nullptr && tapChannel(owner) > 0)   // MIDI out: the off of a note this conductor holds
        for (const auto& v : m_voices)
            if (v.isActive() && !v.isReleasing() && v.note() == note && v.owner() == owner) {
                m_noteTap->add(m_tapPos, tapChannel(owner), note, 0.0f, false);
                break;
            }
    for (auto& v : m_voices) if (v.isActive() && v.note() == note && v.owner() == owner) v.noteOff();
    if (m_rolesUsed) updatePlaces();   // a note letting go hands its place on
}

/**
 * @brief Every sounding note's place in its group, for the slot roles.
 *
 * Releasing notes hand theirs on:
 * they are left out of the reckoning, so the note above a bass that is fading becomes the lowest
 * while the bass is still audible, which is when the ear wants the cello to move up to it.
 */
void Engine::updatePlaces()
{
    int lo[3] = { 128, 128, 128 }, hi[3] = { -1, -1, -1 };
    auto group = [](int owner) { return owner == OWNER_MIDI ? 0 : (owner == OWNER_NEAR ? 2 : 1); };
    for (const auto& v : m_voices) {
        if (!v.isActive() || v.isReleasing()) continue;
        const int g = group(v.owner());
        lo[g] = std::min(lo[g], v.note()); hi[g] = std::max(hi[g], v.note());
    }
    for (auto& v : m_voices) {
        if (!v.isActive()) continue;
        const int g = group(v.owner());
        v.setPlace(v.note() <= lo[g], v.note() >= hi[g]);
    }
}

/**
 * @brief A near event's note: on the near source, with the event's shape, never a strike unless the
 *        section has one, and placed by the scheduler rather than by the dice.
 */
void Engine::startNearNote(const NearNote& e)
{
    if (e.m_note < 0 || e.m_note > 127) return;
    // The event's clip: one of the pool, and never the one just played where there is a choice.
    // Chosen here, before the voice begins, and written into the near voice parameters at once,
    // so the first block of the note already reads it (readParams sets the same pointer again
    // at every control block after this).
    {
        const int a = m_nearPoolActive.load(std::memory_order_acquire);
        if (a >= 0 && m_nearPools[a].size() > 1) {
            const int n = static_cast<int>(m_nearPools[a].size());
            m_nearPickRng ^= m_nearPickRng << 13; m_nearPickRng ^= m_nearPickRng >> 17; m_nearPickRng ^= m_nearPickRng << 5;
            const int last = std::min(std::max(0, m_nearPick), n - 1);
            int pick = static_cast<int>(m_nearPickRng % static_cast<uint32_t>(n - 1));
            if (pick >= last) ++pick;
            m_nearPick = pick;
            m_vpNear.m_texture[Slots - 1] = &m_nearPools[a][static_cast<size_t>(pick)];
        }
    }
    Voice* v = allocate(e.m_note, OWNER_NEAR);
    v->m_order = ++m_order;
    v->setPlace(true, true);
    v->setNearShape(e.m_releaseMul, e.m_cutoffMul, 6.0f * clampv(m_np.m_proximity, 0.0f, 1.0f), e.m_pan, m_nearGain);
    v->noteOn(e.m_note, frequencyOf(e.m_note), e.m_velocity, OWNER_NEAR, e.m_distance, m_vpNear, m_vpNear.m_strikeLevel > 0.0f, 0.0f);
}

void Engine::nearEmit(const NearNote& e)
{
    switch (e.m_type) {
    case NearNote::TypeE::ON:  startNearNote(e); break;
    case NearNote::TypeE::OFF: stopNote(e.m_note, OWNER_NEAR); break;
    case NearNote::TypeE::GLIDE:
        for (auto& v : m_voices)
            if (v.isActive() && v.owner() == OWNER_NEAR && !v.isReleasing())
                v.glideTo(e.m_note, frequencyOf(e.m_note) * std::pow(2.0, static_cast<double>(e.m_detune) / 12.0), e.m_seconds, m_portaGravity);
        break;
    case NearNote::TypeE::MOVE:
        for (auto& v : m_voices) if (v.isActive() && v.owner() == OWNER_NEAR) v.moveTo(e.m_distance, e.m_seconds);
        break;
    }
}

void Engine::noteOn(int note, float velocity)
{
    if (note < 0 || note > 127) return;
    if (m_hold && m_midiHeld[note]) {   // Hold: pressing a sounding key releases it
        m_midiHeld[note] = false;
        stopNote(note, OWNER_MIDI);
        return;
    }
    m_midiHeld[note] = true;
    startNote(note, velocity, OWNER_MIDI, m_keysDepth);
}

void Engine::noteOff(int note)
{
    if (note < 0 || note > 127) return;
    if (m_hold) return;   // keys latch
    m_midiHeld[note] = false;
    stopNote(note, OWNER_MIDI);
}

void Engine::setPressure(int note, float v)
{
    for (auto& x : m_voices) if (x.isActive() && (note < 0 || x.note() == note)) x.setPressure(v);
}

void Engine::setSlide(int note, float v)
{
    for (auto& x : m_voices) if (x.isActive() && (note < 0 || x.note() == note)) x.setSlide(v);
}

void Engine::setHeadYaw(float degrees)
{
    m_headYawDeg.store(degrees, std::memory_order_relaxed);
}

void Engine::setWheel(float v)
{
    m_wheelTarget = clampv(v, 0.0f, 1.0f);
}

void Engine::setBend(int note, float normalised)
{
    const float semis = clampv(normalised, -1.0f, 1.0f) * m_bendRange;
    for (auto& x : m_voices) if (x.isActive() && (note < 0 || x.note() == note)) x.setBend(semis);
}

void Engine::allNotesOff()
{
    if (m_noteTap != nullptr)   // MIDI out: every note a conductor holds gets its off
        for (const auto& v : m_voices)
            if (v.isActive() && !v.isReleasing() && tapChannel(v.owner()) > 0) m_noteTap->add(m_tapPos, tapChannel(v.owner()), v.note(), 0.0f, false);
    for (auto& h : m_midiHeld) h = false;
    for (auto& v : m_voices) v.noteOff();
}

// ---------------------------------------------------------------- parameters

} // namespace ambient
