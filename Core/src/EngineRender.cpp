/**
 * @file EngineRender.cpp
 * @brief What the engine renders: process() with its denormal control and its sleep
 *        check, and renderChunk, which is the signal path from the voices through both planes to the
 *        master.
 *
 * Split out of Engine.cpp.
 * @author Rene Weller
 */
#include "ambient/Engine.h"
#include "ambient/PresetMap.h"
#include "ambient/PresetMeta.h"
#include <cmath>
#include <ctime>   // the clock the arc can follow
#include <cstring>
#include <algorithm>

#if defined( _M_X64 ) || defined( __x86_64__ ) || defined( _M_IX86 ) || defined( __i386__ )
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
#if defined( __aarch64__ ) || defined( _M_ARM64 )
  /**
   * @brief 1 on 64-bit ARM: Engine::process() sets bit 24 of FPCR (flush-to-zero) for the length of a
   *        block, which is what keeps the Quest's decaying tails and envelopes out of denormals.
   */
  #define AMBIENT_HAS_FPCR 1
#else
  /** @brief 0 on every other architecture: there is no FPCR to set. */
  #define AMBIENT_HAS_FPCR 0
#endif

namespace ambient
{

// ---------------------------------------------------------------- audio

/**
 * @brief Message thread, before this engine is heard: take a cluster over from the one that is leaving.
 *
 * The notes are started exactly as the conductor's own would be, so they carry this preset's
 * depth, its spatial placing and its envelopes -- the same chord, played by another instrument.
 */
void Engine::adoptCluster( const int *notes, const float *vels, int count, bool second )
{
    // The inherited notes are not new notes. They were sounding on the engine that is leaving and
    // they go on sounding here, so they arrive already grown: the Bloom open, and every source
    // that waits for its Delay already in. Started from zero instead, a preset whose texture
    // enters after twenty seconds would spend the whole crossfade and the twenty seconds after it
    // being only half of itself -- the crossfade would change the instrument into a sketch of the
    // instrument. A minute is past every delay the parameter allows.
    constexpr float Inherited = 60.0f;
    if ( second )
    {
        m_brain2.adopt( notes, vels, count,
                        [this]( const BrainEvent &e )
                        {
                            if ( e.m_type == BrainEvent::TypeE::NOTE_ON )
                                startNote( e.m_note, e.m_velocity, OWNER_BRAIN2,
                                           clampv( m_depth * m_brain2Depth, 0.0f, 1.0f ), Inherited );
                            else
                                stopNote( e.m_note, OWNER_BRAIN2 );
                        } );
    }
    else
    {
        m_brain.adopt( notes, vels, count,
                       [this]( const BrainEvent &e )
                       {
                           if ( e.m_type == BrainEvent::TypeE::NOTE_ON )
                           {
                               const float u = m_rng.uniform();
                               float       d = ( u < 0.4f ) ? m_depth * 0.15f * m_rng.uniform()
                                                            : m_depth * ( 0.55f + 0.45f * m_rng.uniform() );
                               if ( m_layerDepth > 0.0f )
                               {
                                   const int n = e.m_note;
                                   d += ( m_depth * ( n < 43   ? 0.08f
                                                      : n < 60 ? 0.22f
                                                      : n < 84 ? 0.55f
                                                               : 0.9f ) -
                                          d ) *
                                        m_layerDepth;
                               }
                               startNote( e.m_note, e.m_velocity, OWNER_BRAIN, clampv( d, 0.0f, 1.0f ), Inherited );
                           }
                           else
                               stopNote( e.m_note, OWNER_BRAIN );
                       } );
    }
    // A cluster thinner than this conductor asks for is not a cluster it can keep. What it
    // inherited is the leaving preset's chord, and that can be one note where the arriving preset
    // wants four -- the old one may itself have been a few seconds old. Left at that, the new
    // conductor waits its OWN event rate for the second note, which in this library is a hundred
    // seconds: the change lands on a thin chord and stays there. So it goes on filling from here,
    // and ClusterBrain::update stops it the moment the cluster is full.
    //
    // After the adopt, because adopt() clears the flag itself; and the density is read from the
    // parameter rather than from m_bp, which is filled on the audio thread -- this runs on the
    // message thread while the incoming engine is being prepared, before it has rendered a block.
    const bool on = getParam( second ? ParamIdE::BRAIN2_ON : ParamIdE::BRAIN_ON ) >= 0.5f;
    const int  want =
        static_cast<int>( std::lround( getParam( second ? ParamIdE::BRAIN2_DENSITY : ParamIdE::BRAIN_DENSITY ) ) );
    if ( on && count < want ) ( second ? m_brain2 : m_brain ).requestFill();
}

void Engine::process( float *L, float *R, int n )
{
    m_blocksBegun.fetch_add( 1, std::memory_order_acq_rel );
    // The family jam (02.10.2026): the conductor's root on the leader's pitch class, the nearest such note to its own.
    if ( m_jamRoot >= 0 && ( ( m_brain.root() % 12 ) + 12 ) % 12 != m_jamRoot )
    {
        const int r = m_brain.root();
        int       d = ( ( m_jamRoot - r % 12 ) % 12 + 12 ) % 12;
        if ( d > 6 ) d -= 12;
        m_brain.setRoot( r + d );
    }
#if AMBIENT_HAS_MXCSR
    const unsigned int savedCsr = _mm_getcsr();
    _mm_setcsr( savedCsr | 0x8040 );   // flush-to-zero + denormals-are-zero
#endif
#if AMBIENT_HAS_FPCR
    // The same thing on ARM: FPCR bit 24 is flush-to-zero. Without it the Quest ran every decaying
    // reverb tail and envelope into denormals, where the FPU falls off a cliff.
    uint64_t savedFpcr = 0;
    __asm__ __volatile__( "mrs %0, fpcr" : "=r"( savedFpcr ) );
    const uint64_t fpcrFz = savedFpcr | ( 1ull << 24 );
    __asm__ __volatile__( "msr fpcr, %0" : : "r"( fpcrFz ) );
#endif
    // Pending user scale / wavetable (written from the message thread).
    const int uv = m_userVersion.load( std::memory_order_acquire );
    if ( uv != m_userSeen )
    {
        m_userBusy.store( true, std::memory_order_release );
        m_scales[UserScaleIndex] = m_userPending;
        m_userBusy.store( false, std::memory_order_release );
        m_userSeen = uv;
    }
    const int tv = m_tableVersion.load( std::memory_order_acquire );
    if ( tv != m_tableSeen )
    {
        m_tableBusy.store( true, std::memory_order_release );
        m_userTable = m_userTablePending;
        m_tableBusy.store( false, std::memory_order_release );
        m_tableSeen = tv;
    }
    m_arc.update( static_cast<float>( n / m_sr ), 1.0f / ( 60.0f * std::max( m_arcPeriodMin, 0.5f ) ), m_rng );
    // The arc, as the engine reads it. Off, it is the drifter's own value, assigned rather than
    // followed, so that nothing about the existing presets moves by a bit. On, it is the clock's:
    // the generative apps' idea (Eno's Reflection, Endel) that a piece which runs all night should
    // know what time it is, so that its dawn and the real one coincide. The hour is asked for once
    // a second, which is a hundred times more often than it changes anything. Switching between
    // the two glides over twenty seconds instead of jumping, because the arc leans on density and
    // brightness and a jump there is a jump in the sound.
    {
        const float dt = static_cast<float>( n / m_sr );
        if ( m_arcClock && m_clockHourOverride >= 0.0 )
        {
            m_clockHour = m_clockHourOverride;
        }
        else if ( m_arcClock )
        {
            if ( ++m_clockCheck >= static_cast<int>( m_sr / std::max( 1, n ) ) )
            {
                m_clockCheck = 0;
                const std::time_t t = std::time( nullptr );
                std::tm           lt{};
#if defined( _WIN32 )
                localtime_s( &lt, &t );
#else
                localtime_r( &t, &lt );
#endif
                m_clockHour = lt.tm_hour + lt.tm_min / 60.0 + lt.tm_sec / 3600.0;
            }
        }
        const float target = m_arcClock ? clockArcValue( m_clockHour ) : m_arc.value();
        if ( m_arcClock != m_arcClockWas )
        {
            m_arcClockWas = m_arcClock;
            m_arcGlide = 20.0f;
        }
        if ( m_arcGlide > 0.0f )
        {
            m_arcGlide = std::max( 0.0f, m_arcGlide - dt );
            m_arcOut += ( target - m_arcOut ) * ( 1.0f - std::exp( -dt / 5.0f ) );
        }
        else
            m_arcOut = target;
    }
    if ( m_tide > 0.0f )
        m_tideDrift.update( static_cast<float>( n / m_sr ), 1.0f / ( 60.0f * std::max( m_tidePeriod, 0.5f ) ),
                            m_auxRng );
    if ( m_farRotate > 0.0f ) m_rotDrift.update( static_cast<float>( n / m_sr ), 0.008f, m_auxRng );
    m_arcValue.store( m_arcOut * m_arcAmount, std::memory_order_relaxed );
    m_shiftDrift.update( static_cast<float>( n / m_sr ), 0.03f, m_rng );
    m_purityDrift.update( static_cast<float>( n / m_sr ), getParam( ParamIdE::TUNE_DRIFT_RATE ), m_rng );
    m_lastBlockSeconds = static_cast<float>( n / m_sr );
    {   // Kuramoto bank: dθ_i = ω_i + K/N Σ sin(θ_j − θ_i). Natural periods 23/31/41/53 s over Rate;
        // K up to 0.6 rad/s locks them (the spread of ω is 0.15 rad/s), 0 leaves them independent.
        const float rate = std::max( getParam( ParamIdE::COHERENCE_RATE ), 0.05f );
        const float K = 0.6f * getParam( ParamIdE::COHERENCE ) *
                        rate;   // scales with the rate, so the lock is the same at every tempo
        const float dt = static_cast<float>( n / m_sr );
        const float periods[4] = { 23.0f, 31.0f, 41.0f, 53.0f };
        float       dth[4];
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
        static const float W[4] = { 1.0f, 1.0f + 0.22f, 1.0f, 1.0f - 0.22f };   // sin(2 pi k / 4) = 0, 1, 0, -1
        if ( K == 0.0f )
        {
            for ( int i = 0; i < 4; ++i ) dth[i] = TwoPi * rate / periods[i];
        }
        else
        {
            for ( int i = 0; i < 4; ++i )
            {
                float coupling = 0.0f;
                for ( int j = 0; j < 4; ++j )
                    coupling += W[( ( j - i ) & 3 )] * std::sin( m_kuraPhase[j] - m_kuraPhase[i] );
                dth[i] = TwoPi * rate / periods[i] + K * coupling * 0.25f;
            }
        }
        for ( int i = 0; i < 4; ++i )
        {
            m_kuraPhase[i] += dth[i] * dt;
            if ( m_kuraPhase[i] > TwoPi ) m_kuraPhase[i] -= TwoPi;
            if ( m_kuraPhase[i] < 0.0f ) m_kuraPhase[i] += TwoPi;
        }
    }
    {   // morph position glides toward its target at 1/glide per second (glide 0 = jump)
        const float target = clampv( getParam( ParamIdE::MORPH_POS ), 0.0f, 1.0f );
        const float glide = getParam( ParamIdE::MORPH_GLIDE );
        float       cur = m_morphCur.load( std::memory_order_relaxed );
        if ( glide <= 0.001f )
            cur = target;
        else
        {
            const float step = static_cast<float>( n / m_sr ) / glide;
            cur += clampv( target - cur, -step, step );
        }
        m_morphCur.store( cur, std::memory_order_relaxed );
    }
    updateBlend( n );
    stepClock( n / m_sr );
    stepModulation( static_cast<float>( n / m_sr ) );
    readParams();
    // Transpose glides: an octave in two seconds, whichever way, and stops exactly on the ratio.
    if ( m_transposeCur != m_transposeTarget )
    {
        const double step = 0.5 * ( n / m_sr );
        m_transposeCur += clampv( m_transposeTarget - m_transposeCur, -step, step );
    }
    // The comma's way home: the shared offset moves towards minus the mean of the sounding
    // notes' own offsets, three cents a minute, so the ensemble's centre returns to the
    // reference while every interval inside it stays pure. With nothing sounding, or the
    // knob at zero, it returns to zero the same slow way.
    if ( m_adaptAmt > 0.0f || m_commaCents != 0.0 )
    {
        double sum = 0.0;
        int    count = 0;
        if ( m_adaptAmt > 0.0f )
            for ( const auto &v : m_voices )
                if ( v.isActive() )
                {
                    sum += m_adaptCents[v.note()];
                    ++count;
                }
        m_commaTarget = count > 0 ? -sum / count : 0.0;
        const double step = ( 3.0 / 60.0 ) * ( n / m_sr );
        m_commaCents += clampv( m_commaTarget - m_commaCents, -step, step );
        if ( std::fabs( m_commaCents ) < 1.0e-6 && m_commaTarget == 0.0 ) m_commaCents = 0.0;
    }
    if ( m_retune )   // tuning purity / drift: every sounding voice glides to its current frequency
        for ( auto &v : m_voices )
            if ( v.isActive() ) v.setTargetFrequency( frequencyOf( v.note() ) * v.rootComp() );

    int pos = 0;
    while ( pos < n )
    {
        const int chunk = std::min( n - pos, m_maxBlock );
        m_stemPos = pos;
        m_tapPos = pos;
        renderChunk( L + pos, R + pos, chunk );
        pos += chunk;
    }
    m_tapPos = 0;
    // Sleep: nothing sounding and the tails gone for two seconds -> the next blocks skip the
    // effect chain (zeros out) until a voice starts. The brain keeps running inside renderChunk,
    // so a generative patch wakes itself; MIDI and OSC notes wake it through the voices.
    {
        bool anyVoice = false;
        for ( auto &v : m_voices )
            if ( v.isActive() )
            {
                anyVoice = true;
                break;
            }
        float peak = 0.0f;
        for ( int i = 0; i < n; ++i ) peak = std::max( peak, std::max( std::fabs( L[i] ), std::fabs( R[i] ) ) );
        if ( !anyVoice && peak < 3.2e-5f )
            m_silentSamples = std::min<long long>( m_silentSamples + n, 1LL << 40 );
        else
            m_silentSamples = 0;
        m_asleep = m_silentSamples > static_cast<long long>( 2.0 * m_sr );
    }

    uint64_t m0 = 0, m1 = 0;
    int      active = 0;
    for ( auto &d : m_noteDistance ) d.store( -1.0f, std::memory_order_relaxed );
    for ( auto &l : m_noteLevel ) l.store( 0.0f, std::memory_order_relaxed );
    for ( auto &v : m_voices )
    {
        if ( !v.isActive() ) continue;
        ++active;
        const int nt = v.note();
        if ( nt >= 0 && nt < 64 )
            m0 |= ( 1ull << nt );
        else if ( nt >= 64 && nt < 128 )
            m1 |= ( 1ull << ( nt - 64 ) );
        if ( nt >= 0 && nt < 128 )
        {
            m_noteDistance[nt].store( v.distance(), std::memory_order_relaxed );
            if ( v.level() > m_noteLevel[nt].load( std::memory_order_relaxed ) )
                m_noteLevel[nt].store( v.level(), std::memory_order_relaxed );
        }
    }
    // Done: whatever this block picked up, it has let go of. See Engine::waitForQuiet.
    m_blocksDone.fetch_add( 1, std::memory_order_release );
    m_mask[0].store( m0, std::memory_order_relaxed );
    m_mask[1].store( m1, std::memory_order_relaxed );
    m_activeVoices.store( active, std::memory_order_relaxed );
    m_brainRoot.store( m_brain.root(), std::memory_order_relaxed );
#if AMBIENT_HAS_MXCSR
    _mm_setcsr( savedCsr );
#endif
#if AMBIENT_HAS_FPCR
    __asm__ __volatile__( "msr fpcr, %0" : : "r"( savedFpcr ) );
#endif
}

void Engine::renderChunk( float *L, float *R, int n )
{
    float       *nl = m_nearL.data();
    float       *nr = m_nearR.data();
    float       *fl = m_farL.data();
    float       *fr = m_farR.data();
    float       *wl = m_wetL.data();
    float       *wr = m_wetR.data();
    const size_t bytes = sizeof( float ) * static_cast<size_t>( n );
    std::memset( nl, 0, bytes );
    std::memset( nr, 0, bytes );
    std::memset( fl, 0, bytes );
    std::memset( fr, 0, bytes );
    float *dl = m_dryL.data();
    float *dr = m_dryR.data();
    std::memset( dl, 0, bytes );
    std::memset( dr, 0, bytes );
    // The foreground's sends, gathered per event below and handed to the two effects where they
    // take their input. Cleared only while one of them is open, so a preset without them pays
    // nothing at all.
    const float foreD2 = clampv( m_np.m_toDelay2, 0.0f, 1.0f ), foreCo = clampv( m_np.m_toCosmos, 0.0f, 1.0f );
    const bool  foreSends = foreD2 > 0.0f || foreCo > 0.0f;
    float      *s2l = m_foreD2L.data();
    float      *s2r = m_foreD2R.data();
    float      *scl = m_foreCoL.data();
    float      *scr = m_foreCoR.data();
    if ( foreSends )
    {
        std::memset( s2l, 0, bytes );
        std::memset( s2r, 0, bytes );
        std::memset( scl, 0, bytes );
        std::memset( scr, 0, bytes );
    }

    int anchor = -1;
    for ( int i = 0; i < 128; ++i )
        if ( m_midiHeld[i] )
        {
            anchor = i;
            break;
        }

    auto freqOf = [this]( int note ) { return frequencyOf( note ); };
    auto emit = [this]( const BrainEvent &e )
    {
        if ( e.m_type == BrainEvent::TypeE::NOTE_ON )
        {
            // Rich's contrast: some notes intimately close, most of them deep in the background.
            const float u = m_rng.uniform();
            float d = ( u < 0.4f ) ? m_depth * 0.15f * m_rng.uniform() : m_depth * ( 0.55f + 0.45f * m_rng.uniform() );
            // Layer Depth takes that away from the dice and gives it to the role (R8.4): the
            // foundation and the body stand near, the colour in the middle, the air far back. The
            // dice keep whatever share of it Layer Depth leaves them.
            if ( m_layerDepth > 0.0f )
            {
                const int   n = e.m_note;
                const float byRole = m_depth * ( n < 43 ? 0.08f : n < 60 ? 0.22f : n < 84 ? 0.55f : 0.9f );
                d += ( byRole - d ) * m_layerDepth;
            }
            startNote( e.m_note, e.m_velocity, OWNER_BRAIN, clampv( d, 0.0f, 1.0f ) );
        }
        else
        {
            stopNote( e.m_note, OWNER_BRAIN );
        }
    };
    // The second conductor plays on one plane, deep, so it reads as a background line rather
    // than as more of the same cluster.
    auto emit2 = [this]( const BrainEvent &e )
    {
        if ( e.m_type == BrainEvent::TypeE::NOTE_ON )
            startNote( e.m_note, e.m_velocity, OWNER_BRAIN2, clampv( m_depth * m_brain2Depth, 0.0f, 1.0f ) );
        else
            stopNote( e.m_note, OWNER_BRAIN2 );
    };

    // Feedback loop, input side: what the previous chunk wrote into the ring comes back as
    // phase modulation of the partials and/or as signal into the near bus (before ensemble,
    // delays and reverbs -- "after the reverb back before the filter").
    const bool fbOn = m_fbBus > 0.0f || m_fbFm > 0.0f;
    float     *fbl = m_fbInL.data();
    float     *fbr = m_fbInR.data();
    float     *fbm = m_fbMono.data();
    if ( fbOn )
    {
        // The level throttle applies to the bus path only (that one adds energy and can run
        // away); phase modulation moves energy between partials without adding any, so it
        // gets the saturated signal unthrottled. The throttle ramps across the chunk.
        const float regTarget = clampv( ( 0.1f - m_fbEnv ) / 0.1f, 0.0f, 1.0f );
        const float regStep = ( regTarget - m_fbReg ) / static_cast<float>( n );
        // Tape: wow (slow, irregular, up to 3 ms) and flutter (6 Hz, up to 0.3 ms) move the read
        // position -- a fractional delay in the loop, so every pass through the loop smears a
        // little more, the way a tape loop goes soft with each generation.
        const float wowSamples =
            m_fbTape > 0.0f
                ? ( 0.003f * m_tapeWow.update( static_cast<float>( n / m_sr ), 0.3f, m_rng ) * 0.5f + 0.0015f ) *
                      m_fbTape * static_cast<float>( m_sr )
                : 0.0f;
        const double flutterInc = 6.0 / m_sr;
        for ( int i = 0; i < n; ++i )
        {
            float l, r;
            if ( m_fbTape > 0.0f )
            {
                m_tapeFlutterPhase += flutterInc;
                if ( m_tapeFlutterPhase >= 1.0 ) m_tapeFlutterPhase -= 1.0;
                const float delay = wowSamples + 0.0003f * m_fbTape * static_cast<float>( m_sr ) *
                                                     ( 0.5f + 0.5f * sin01( m_tapeFlutterPhase ) );
                const int   di = static_cast<int>( delay );
                const float fr = delay - static_cast<float>( di );
                const int   i0 = ( m_fbW - n + i - di ) & m_fbMask, i1 = ( i0 - 1 ) & m_fbMask;
                l = m_fbRingL[static_cast<size_t>( i0 )] +
                    fr * ( m_fbRingL[static_cast<size_t>( i1 )] - m_fbRingL[static_cast<size_t>( i0 )] );
                r = m_fbRingR[static_cast<size_t>( i0 )] +
                    fr * ( m_fbRingR[static_cast<size_t>( i1 )] - m_fbRingR[static_cast<size_t>( i0 )] );
            }
            else
            {
                const int idx = ( m_fbW - n + i ) & m_fbMask;
                l = m_fbRingL[static_cast<size_t>( idx )];
                r = m_fbRingR[static_cast<size_t>( idx )];
            }
            const float reg = m_fbReg + regStep * static_cast<float>( i + 1 );
            fbl[i] = l * reg;
            fbr[i] = r * reg;
            fbm[i] = 0.5f * ( l + r );
        }
        m_fbReg = regTarget;
    }

    bool anyVoice = false;
    for ( int p = 0; p < n; p += ControlBlock )
    {
        const int len = std::min( ControlBlock, n - p );
        {   // Quantize: the decisions are held back and made at the next note value of the clock,
            // so the conductor lands on the grid instead of wherever the dice fell. All of the
            // waiting time is handed over at the tick, so the mean rate is unchanged.
            const double dt = len / m_sr;
            // The foreground's asks are set inside stepNear: while a near Note or a Phrase speaks
            // the conductor begins no new note (its holds and releases run on), and under a
            // sequence it keeps its root and takes twice as long between events.
            const BrainParams *bpNow = &m_bp;
            BrainParams        bpSlow;
            if ( m_near.sequenceRunning() )
            {
                bpSlow = m_bp;
                bpSlow.m_rateSeconds *= 2.0f;
                bpNow = &bpSlow;
            }
            if ( syncOn( m_brainQuant ) && m_running )
            {
                m_quantAcc += dt;
                const double b = syncBeats( m_brainQuant );
                const double now = std::floor( m_beat / b ), before = std::floor( m_lastBeat / b );
                m_lastBeat = m_beat;
                if ( now != before )
                {
                    m_brain.update( m_quantAcc, *bpNow, anchor, freqOf, emit );
                    m_quantAcc = 0.0;
                }
            }
            else
            {
                m_quantAcc = 0.0;
                m_lastBeat = m_beat;
                m_brain.update( dt, *bpNow, anchor, freqOf, emit );
            }
            if ( m_brain2On )
            {
                m_brain2.setRoot( clampv( m_brain.root() + m_brain2Interval, 0, 127 ) );
                m_brain2.update( dt, m_bp2, -1, freqOf, emit2 );
            }
            stepNear( dt, [this]( const NearNote &e ) { nearEmit( e ); } );
        }
        const float *fm = ( fbOn && m_fbFm > 0.0f ) ? fbm + p : nullptr;
        const float *couple = m_sympathy > 0.0f ? m_coupleBuf.data() + p : nullptr;
        const float  dry = clampv( m_np.m_dry, 0.0f, 1.0f );
        for ( auto &v : m_voices )
        {
            if ( !( v.isActive() || v.isStriking() ) ) continue;
            anyVoice = true;
            if ( v.owner() == OWNER_NEAR && ( dry > 0.0f || foreSends ) )
            {
                // The near layer's Dry share: the voice is rendered apart, and that share of both
                // its planes goes to the dry bus -- added to the output after every reverb and
                // delay -- while the rest takes the usual way through the planes. The two sends
                // are taken from the same rendering: a share of the event, ADDED to what the
                // second delay and the Cosmos already hear on the near bus, the way an aux send
                // on a desk adds rather than diverts. The event stays where Distance put it.
                float tnl[ControlBlock], tnr[ControlBlock], tfl[ControlBlock], tfr[ControlBlock];
                std::memset( tnl, 0, sizeof( float ) * static_cast<size_t>( len ) );
                std::memset( tnr, 0, sizeof( float ) * static_cast<size_t>( len ) );
                std::memset( tfl, 0, sizeof( float ) * static_cast<size_t>( len ) );
                std::memset( tfr, 0, sizeof( float ) * static_cast<size_t>( len ) );
                v.render( tnl, tnr, tfl, tfr, len, m_vpNear, fm, couple );
                const float wet = 1.0f - dry;
                for ( int i = 0; i < len; ++i )
                {
                    nl[p + i] += tnl[i] * wet;
                    nr[p + i] += tnr[i] * wet;
                    fl[p + i] += tfl[i] * wet;
                    fr[p + i] += tfr[i] * wet;
                    dl[p + i] += ( tnl[i] + tfl[i] ) * dry;
                    dr[p + i] += ( tnr[i] + tfr[i] ) * dry;
                }
                if ( foreSends )
                    for ( int i = 0; i < len; ++i )
                    {
                        const float sL = tnl[i] + tfl[i], sR = tnr[i] + tfr[i];
                        s2l[p + i] += sL * foreD2;
                        s2r[p + i] += sR * foreD2;
                        scl[p + i] += sL * foreCo;
                        scr[p + i] += sR * foreCo;
                    }
            }
            else
            {
                v.render( nl + p, nr + p, fl + p, fr + p, len, v.owner() == OWNER_NEAR ? m_vpNear : m_vp, fm, couple );
            }
        }
    }
    if ( m_asleep && !anyVoice )
    {   // sleeping: the whole effect chain is skipped, output stays silent
        std::memset( L, 0, bytes );
        std::memset( R, 0, bytes );
        // The stems too. They are the host's buffers (multi-out) or the render tool's, and
        // leaving them alone means whatever was in them last is heard again -- silence in the
        // mix and a burst of the previous block on every stem.
        if ( m_stems != nullptr )
            for ( int c = 0; c < NumStems * 2; ++c ) std::memset( m_stems[c] + m_stemPos, 0, bytes );
        return;
    }
    if ( fbOn && m_fbBus > 0.0f )
        for ( int i = 0; i < n; ++i )
        {
            nl[i] += fbl[i] * m_fbBus;
            nr[i] += fbr[i] * m_fbBus;
        }

    // Blur: the near bus through a spectral smear before anything else hears it, so a note's
    // attack is wiped into texture and one note flows into the next.
    if ( m_blurMix > 0.0f || m_smBlur.m_value > 1.0e-4f )
    {
        float *bl = m_nebL.data();
        float *br = m_nebR.data();
        m_blur.process( nl, nr, bl, br, n );
        for ( int i = 0; i < n; ++i )
        {
            const float m = m_smBlur.next( m_blurMix );
            nl[i] = nl[i] * ( 1.0f - m ) + bl[i] * m;
            nr[i] = nr[i] * ( 1.0f - m ) + br[i] * m;
        }
    }
    // Foreground plane: ensemble, asymmetric delay (echoes partly recede into the far plane), small room.
    m_ensemble.process( nl, nr, n );
    m_delay.process( nl, nr, wl, wr, n );
    for ( int i = 0; i < n; ++i )
    {
        const float m = m_smDelayMix.next( m_delayMix ), tf = m_smDelayToFar.next( m_delayToFar );
        nl[i] += wl[i] * m;
        nr[i] += wr[i] * m;
        fl[i] += wl[i] * tf;
        fr[i] += wr[i] * tf;
    }
    // Second delay in series: it hears the first delay's echoes and spins longer chains -- and,
    // with To Delay 2 open, an extra share of every foreground event straight into its input. Into
    // the INPUT and not onto the plane: on the plane the event would go through everything else a
    // second time; here it is the delay that gets more of it, which is what a send is.
    if ( foreD2 > 0.0f )
    {
        for ( int i = 0; i < n; ++i )
        {
            s2l[i] += nl[i];
            s2r[i] += nr[i];
        }   // the send buffer is the delay's input from here
        m_delay2.process( s2l, s2r, wl, wr, n );
    }
    else
        m_delay2.process( nl, nr, wl, wr, n );
    for ( int i = 0; i < n; ++i )
    {
        const float m = m_smDelay2Mix.next( m_delay2Mix ), tf = m_smDelay2ToFar.next( m_delay2ToFar );
        nl[i] += wl[i] * m;
        nr[i] += wr[i] * m;
        fl[i] += wl[i] * tf;
        fr[i] += wr[i] * tf;
    }
    // Granular cloud on the far plane: grains of the foreground, scattered and transposed,
    // dropped into the far reverb.
    // Kept running while it still sounds with the send closed: its loop and its resonators ring on,
    // and a cloud stopped mid-grain would start again from there the next time it was opened.
    if ( m_cloudSend > 0.0f || m_smCloudSend.m_value > 1e-4f || !m_cloud.quiet() )
    {
        float *cl = m_cosL.data();
        float *cr = m_cosR.data();
        for ( int i = 0; i < n; ++i )
        {
            const float s = m_smCloudSend.next( m_cloudSend );
            cl[i] = nl[i] * s;
            cr[i] = nr[i] * s;
        }
        if ( m_cloudToNear <= 0.0f )
        {
            m_cloud.process( cl, cr, fl, fr, n );
        }
        else
        {
            // To Near: the cloud is written into its own pair and then shared between the planes,
            // equal power, so turning it forward does not change how much of it there is. At 0 the
            // branch above runs and the render is what it always was, sample for sample.
            std::fill( m_cloudL.begin(), m_cloudL.begin() + n, 0.0f );
            std::fill( m_cloudR.begin(), m_cloudR.begin() + n, 0.0f );
            m_cloud.process( cl, cr, m_cloudL.data(), m_cloudR.data(), n );
            const float a = 0.5f * Pi * clampv( m_cloudToNear, 0.0f, 1.0f );
            const float near = std::sin( a ), far = std::cos( a );
            for ( int i = 0; i < n; ++i )
            {
                fl[i] += m_cloudL[static_cast<size_t>( i )] * far;
                fr[i] += m_cloudR[static_cast<size_t>( i )] * far;
                nl[i] += m_cloudL[static_cast<size_t>( i )] * near;
                nr[i] += m_cloudR[static_cast<size_t>( i )] * near;
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
    const float cosmosSendNow =
        m_cosmosSend *
        std::clamp( 1.0f + 3.0f * m_cosmosSwell * ( m_cascadeNow - m_cascadeAvg ) / std::max( m_cascadeAvg, 0.15f ),
                    0.0f, 2.0f );
    if ( cosmosSendNow > 0.0f || m_smCosmosSend.m_value > 1e-4f || foreCo > 0.0f )
    {
        float *cl = m_cosL.data();
        float *cr = m_cosR.data();
        // The plane's own send, and the foreground's on top of it (To Cosmos): a preset may send
        // nothing but its events into the deep space and leave the bed out of it entirely.
        for ( int i = 0; i < n; ++i )
        {
            const float s = m_smCosmosSend.next( cosmosSendNow );
            cl[i] = nl[i] * s + scl[i];
            cr[i] = nr[i] * s + scr[i];
        }
        m_shifter.process( cl, cr, n );
        m_resonator.process( cl, cr, n );
        m_vowel.process( cl, cr, n );
        if ( m_cosmosNebula > 0.0f )
        {
            float *bl = m_nebL.data();
            float *br = m_nebR.data();
            m_nebula.process( cl, cr, bl, br, n );
            const float m = m_cosmosNebula;
            for ( int i = 0; i < n; ++i )
            {
                cl[i] = cl[i] * ( 1.0f - m ) + bl[i] * m;
                cr[i] = cr[i] * ( 1.0f - m ) + br[i] * m;
            }
        }
        for ( int i = 0; i < n; ++i )
        {
            const float ret = m_smCosmosReturn.next( m_cosmosReturn ), tf = m_smCosmosToFar.next( m_cosmosToFar );
            nl[i] += cl[i] * ret;
            nr[i] += cr[i] * ret;
            fl[i] += cl[i] * tf;
            fr[i] += cr[i] * tf;
            m_cosTap[( m_cosTapW + i ) & 4095] =
                0.5f * ( cl[i] + cr[i] ) * ret;   // what the return adds, for the picture
            if ( m_stems != nullptr )
            {
                m_stems[4][m_stemPos + i] = cl[i] * ret;
                m_stems[5][m_stemPos + i] = cr[i] * ret;
            }
        }
        m_cosTapW = ( m_cosTapW + n ) & 4095;
    }
    else
    {
        for ( int i = 0; i < n; ++i ) m_cosTap[( m_cosTapW + i ) & 4095] = 0.0f;
        m_cosTapW = ( m_cosTapW + n ) & 4095;
        if ( m_stems != nullptr )
            for ( int i = 0; i < n; ++i )
            {
                m_stems[4][m_stemPos + i] = 0.0f;
                m_stems[5][m_stemPos + i] = 0.0f;
            }
    }
    // Memory: a second parallel world off the near bus, returned to both planes like the Cosmos and
    // counted into the Cosmos stem, where the parallel paths are heard apart from the dry one. It keeps
    // running with the send closed for as long as it still holds anything.
    if ( m_memSend > 0.0f || m_smMemSend.m_value > 1e-4f || !m_memory.quiet() )
    {
        float *ml = m_memL.data();
        float *mr = m_memR.data();
        for ( int i = 0; i < n; ++i )
        {
            const float s = m_smMemSend.next( m_memSend );
            ml[i] = nl[i] * s;
            mr[i] = nr[i] * s;
        }
        m_memory.process( ml, mr, n );
        for ( int i = 0; i < n; ++i )
        {
            const float ret = m_smMemReturn.next( m_memReturn ), tf = m_smMemToFar.next( m_memToFar );
            nl[i] += ml[i] * ret;
            nr[i] += mr[i] * ret;
            fl[i] += ml[i] * tf;
            fr[i] += mr[i] * tf;
            if ( m_stems != nullptr )
            {
                m_stems[4][m_stemPos + i] += ml[i] * ret;
                m_stems[5][m_stemPos + i] += mr[i] * ret;
            }
        }
    }
    m_nearReverb.process( nl, nr, n );

    // Background plane: 100 % wet, dark, wide. Shimmer feeds the pitch-shifted previous
    // block of the far reverb back into its input (the classic rising cloud).
    float *sl = m_shimL.data();
    float *sr = m_shimR.data();
    if ( m_cosmosShimmer > 0.0f )
        for ( int i = 0; i < n; ++i )
        {
            fl[i] += sl[i];
            fr[i] += sr[i];
        }
    // Room: the convolution reverb hears the far sends (before the FDN) or the finished
    // near bus, through a pre-delay; it keeps running for one impulse length after its
    // level reaches zero so the tail can finish, and costs nothing while silent.
    const bool roomOn = m_roomLevel > 0.0005f || m_roomLevelCur > 0.0005f || m_roomTailLeft > 0;
    float     *rl = m_roomInL.data();
    float     *rr = m_roomInR.data();
    if ( roomOn )
    {
        const float *srcL = m_roomSource == 0 ? fl : nl;
        const float *srcR = m_roomSource == 0 ? fr : nr;
        // The convolution's own latency is taken out of the pre-delay rather than added to it, so
        // Pre-Delay means what it says down to the latency itself (5 ms).
        const int preDelay = std::max( 0, m_roomPreDelay - m_room.latency() );
        for ( int i = 0; i < n; ++i )
        {
            m_roomDelayL[static_cast<size_t>( m_roomDelayW )] = srcL[i];
            m_roomDelayR[static_cast<size_t>( m_roomDelayW )] = srcR[i];
            const int rd = ( m_roomDelayW - preDelay ) & m_roomDelayMask;
            rl[i] = m_roomDelayL[static_cast<size_t>( rd )];
            rr[i] = m_roomDelayR[static_cast<size_t>( rd )];
            m_roomDelayW = ( m_roomDelayW + 1 ) & m_roomDelayMask;
            if ( m_sendLowcut > 21.0f )
            {   // Send Low Cut: the room, like the two halls, is never fed under it
                float lp, bp, hp;
                m_roomSendHpL.tick( rl[i], lp, bp, hp );
                rl[i] = hp;
                m_roomSendHpR.tick( rr[i], lp, bp, hp );
                rr[i] = hp;
            }
        }
        m_roomTailLeft = m_roomLevel > 0.0005f ? m_room.tailSamples() : std::max( 0L, m_roomTailLeft - n );
    }
    // The room's answer is made here, ahead of the far hall, rather than after it (25.09.2026): the
    // production guide's rooms are serial -- the main room's output feeds the far room at ten to
    // twenty per cent, so the horizon sounds like the same place going on and not like a second,
    // foreign one (Lustmord's "endless" rooms). Its input was taken above either way, so moving the
    // convolution up changes nothing about what the room itself hears. The return goes into the mix
    // further down, ducked under the near bus like the far hall.
    float *ol = m_roomOutL.data();
    float *orr = m_roomOutR.data();
    if ( roomOn )
    {
        // Morph: the second impulse is blended into the first inside the convolution, which is the
        // same as fading from one room's answer to the other's and costs one room. The smoother
        // moves a block at a time; each stage of the convolver takes its value as its blocks begin.
        m_smRoomMorph.m_value = m_roomMorph + ( m_smRoomMorph.m_value - m_roomMorph ) *
                                                  std::pow( 1.0f - m_smRoomMorph.m_coef, static_cast<float>( n ) );
        if ( std::fabs( m_smRoomMorph.m_value - m_roomMorph ) < 1.0e-4f ) m_smRoomMorph.m_value = m_roomMorph;
        m_room.setMorph( m_smRoomMorph.m_value );
        m_room.process( rl, rr, ol, orr, n );
        const float lpc = 1.0f - std::exp( -TwoPi * m_roomHighcut / static_cast<float>( m_sr ) );
        const float rhpc = m_roomLowcut <= 21.0f
                               ? 0.0f
                               : 1.0f - std::exp( -TwoPi * ( m_roomLowcut / 1.5538f ) / static_cast<float>( m_sr ) );
        const float levelC = 1.0f - std::exp( -1.0f / ( 0.05f * static_cast<float>( m_sr ) ) );
        // An energy-normalised impulse returns the far sends at their own power, which is far
        // louder than the FDN's output at Far Level 1; 0.35 puts Room Level 1 in the same league
        // (measured: -15.8 dBFS raw vs. -23 dBFS for the FDN on the default patch).
        const float roomGain = 0.35f;
        for ( int i = 0; i < n; ++i )
        {
            m_roomLevelCur += ( m_roomLevel - m_roomLevelCur ) * levelC;
            m_roomLpL += lpc * ( ol[i] - m_roomLpL );
            m_roomLpR += lpc * ( orr[i] - m_roomLpR );
            float tL = m_roomLpL, tR = m_roomLpR;
            if ( rhpc > 0.0f )
            {   // the low end out of the room, 12 dB/oct
                m_roomHpL1 += rhpc * ( tL - m_roomHpL1 );
                const float aL = tL - m_roomHpL1;
                m_roomHpR1 += rhpc * ( tR - m_roomHpR1 );
                const float aR = tR - m_roomHpR1;
                m_roomHpL2 += rhpc * ( aL - m_roomHpL2 );
                tL = aL - m_roomHpL2;
                m_roomHpR2 += rhpc * ( aR - m_roomHpR2 );
                tR = aR - m_roomHpR2;
            }
            ol[i] = tL * m_roomLevelCur * roomGain;
            orr[i] = tR * m_roomLevelCur * roomGain;
            // To Far: the room's return, before it is ducked, into the far hall's input.
            const float tf = m_smRoomToFar.next( m_roomToFar );
            fl[i] += ol[i] * tf;
            fr[i] += orr[i] * tf;
        }
    }
    else
    {
        m_roomLpL = m_roomLpR = 0.0f;
        m_smRoomToFar.snap( m_roomToFar );
    }
    m_diffuser.process( fl, fr, n );
    {
        // Air saturates. A real room is not linear at a peak -- the medium itself gives a little,
        // and a dense cluster fired into a hall comes back thickened rather than reflected. This
        // is a very gentle asymmetric shaper between the diffuser and the reverb's own feedback
        // network, so only the peaks are touched and the tail is what changes character, not the
        // level. Amount rides on Diffuse, which is already the "how much room" control, so there
        // is no new knob for a thing nobody would know how to set. At four times the rate since
        // 25.09.2026 (the guide's "4x on everything nonlinear"); the far send is only ever heard
        // through the hall, so the 0.6 ms the oversampling takes is a pre-delay nobody measures.
        const float amt = 0.35f * m_farDiffuse;
        if ( amt > 0.001f )
        {
            if ( !m_airOsOn )
            {
                m_airOsL.reset();
                m_airOsR.reset();
                m_airOsOn = true;
            }
            const float pre = 1.0f + 2.2f * amt, post = 1.0f / ( 1.0f + 0.55f * amt );
            // Asymmetric on purpose: a touch of second harmonic reads as warmth, where the
            // symmetric curve of a plain tanh only ever reads as compression.
            auto air = [pre, post]( float x )
            {
                const float a = x * pre, t = std::tanh( a );
                return ( t + 0.06f * a * a * ( a > 0.0f ? 1.0f : -1.0f ) * ( 1.0f - std::fabs( t ) ) ) * post;
            };
            for ( int i = 0; i < n; ++i )
            {
                fl[i] = m_airOsL.process( fl[i], air );
                fr[i] = m_airOsR.process( fr[i], air );
            }
        }
        else
            m_airOsOn = false;
    }
    m_farReverb.process( fl, fr, n );
    if ( m_farRotate > 0.0f )
    {   // the background slowly turns: left and right rotate into each other
        const float a = m_rotDrift.value() * m_farRotate * 0.6f, c = std::cos( a ), sn = std::sin( a );
        for ( int i = 0; i < n; ++i )
        {
            const float l = fl[i], r = fr[i];
            fl[i] = l * c - r * sn;
            fr[i] = l * sn + r * c;
        }
    }
    if ( m_cosmosShimmer > 0.0f )
    {
        // Spectral: phase-locked peak shifting (Shifter.h), as clean on the hundredth pass as on the
        // first. Grain: the two-head shifter the shimmer always had, flutter and all. The spectral
        // one starts empty each time the shimmer opens, rather than with what it heard last time.
        if ( m_shimmerMode == 0 )
        {
            if ( !m_shimmerWasOn ) m_shimmerSpec.reset();
            m_shimmerSpec.process( fl, fr, sl, sr, n );
        }
        else
        {
            m_shimmerL.process( fl, sl, n );
            m_shimmerR.process( fr, sr, n );
        }
        m_shimmerWasOn = true;
        const float lpc = 1.0f - std::exp( -TwoPi * 4000.0f / static_cast<float>( m_sr ) );
        const float amt = m_cosmosShimmer * 0.5f;
        // Self-regulating loop: the feedback is throttled by the level of the far reverb
        // itself (the reverb integrates every injection), so the cloud blooms up to a fixed
        // ceiling and holds there instead of running into the clipper.
        const float envC = 1.0f - std::exp( -1.0f / ( 0.1f * static_cast<float>( m_sr ) ) );
        for ( int i = 0; i < n; ++i )
        {
            m_shimmerLpL += lpc * ( sl[i] - m_shimmerLpL );
            m_shimmerLpR += lpc * ( sr[i] - m_shimmerLpR );
            const float mag = 0.5f * ( std::fabs( fl[i] ) + std::fabs( fr[i] ) );
            m_shimmerEnv += envC * ( mag - m_shimmerEnv );
            const float reg =
                clampv( ( 0.12f - m_shimmerEnv ) / 0.12f, 0.0f, 1.0f );   // 1 when quiet, 0 at the ceiling
            sl[i] = m_shimmerLpL * amt * reg;
            sr[i] = m_shimmerLpR * amt * reg;
        }
    }
    else
    {
        m_shimmerLpL = m_shimmerLpR = 0.0f;
        m_shimmerEnv = 0.0f;
        m_shimmerWasOn = false;
        std::memset( sl, 0, bytes );
        std::memset( sr, 0, bytes );
    }

    // Unmasking: the background gives way to the foreground band by band, before the two planes
    // are summed (the near bus is the side chain, and it is finished by now).
    m_unmask.process( nl, nr, fl, fr, n );
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
    if ( m_envelop > 0.0f || m_smEnvelop.m_value > 1.0e-4f )
    {
        for ( int i = 0; i < n; ++i )
        {
            const float mid = 0.5f * ( fl[i] + fr[i] );
            float       side = 0.5f * ( fl[i] - fr[i] );
            float       lp, bp, hp;
            m_envBell.tick( side, lp, bp, hp );
            side += m_smEnvelop.next( m_envelop ) * m_envBell.m_k * bp;   // k * bp: unity at the centre, no phase turn
            fl[i] = mid + side;
            fr[i] = mid - side;
        }
    }
    // Comodulation: the whole background breathes to one random envelope, a new target about
    // nine times a second and a smooth curve between, so every band of it rises and falls
    // together and the ear can hear the foreground through it (comodulation masking release).
    // Its own random stream, so the knob touches nothing else that draws from the engine's.
    if ( m_comod > 0.0f || m_smComod.m_value > 1.0e-4f )
    {
        if ( !m_comodInit )
        {
            m_comodRng.seed( 0xC0D0ull );
            m_comodDrift.init( m_comodRng );
            m_comodInit = true;
        }
        const float dt = 1.0f / static_cast<float>( m_sr );
        for ( int i = 0; i < n; ++i )
        {
            const float depth = m_smComod.next( m_comod );
            const float m = m_comodDrift.update( dt, 9.0f, m_comodRng );   // -1 .. 1
            const float g = 1.0f - depth * 0.5f * ( 1.0f + m );            // 1 .. 1 - depth
            fl[i] *= g;
            fr[i] *= g;
        }
    }
    // Mid Low Cut (25.09.2026): the production guide thinks of the far return in mid and side --
    // its sides may be as wide as they like, its middle is high-passed from 300 Hz, so the centre
    // below that belongs to the near plane and the sub alone. Second order, on the middle only;
    // the sides pass untouched, and so does everything with the knob at 20 Hz.
    if ( m_farMidLowcut > 21.0f )
    {
        for ( int i = 0; i < n; ++i )
        {
            const float mid = 0.5f * ( fl[i] + fr[i] ), side = 0.5f * ( fl[i] - fr[i] );
            float       lp, bp, hp;
            m_farMidHp.tick( mid, lp, bp, hp );
            fl[i] = hp + side;
            fr[i] = hp - side;
        }
    }
    // The Haas band, on the foreground only: the background has the reverb's own width and does
    // not need help. After the unmask, so what the side chain measured is the plane as it was.
    m_haas.process( nl, nr, n );
    // The room's early reflections, from the near bus and added to it: they belong in front of
    // the listener with the dry sound, not behind it with the tail.
    if ( m_early.active() )
    {
        // The room listens to both channels of the near bus and adds its reflections into the same
        // two buffers, so it listens to copies, in the delay scratch that is free by now.
        std::memcpy( wl, nl, bytes );
        std::memcpy( wr, nr, bytes );
        m_early.process( wl, wr, nl, nr, n );
    }
    // Sympathy: this block's foreground is what the voices will hear of each other in the next
    // one. A block of delay is what makes the loop safe, and at these depths inaudible.
    // Bounded: the coupling goes back into the voices before their filters, and sixteen voices
    // through a resonant filter can add up to a loop gain above one. Clamped here the loop can
    // saturate, but it cannot climb to infinity and leave NaN in every filter. Identity below
    // twice full scale, so a healthy mix hears nothing of it.
    if ( m_sympathy > 0.0f && static_cast<int>( m_coupleBuf.size() ) >= n )
        for ( int i = 0; i < n; ++i )
            m_coupleBuf[static_cast<size_t>( i )] = clampv( 0.5f * ( nl[i] + nr[i] ), -2.0f, 2.0f );

    const float master = dbToGain( m_masterGain );
    for ( int i = 0; i < n; ++i )
    {
        const float far = m_smFarLevel.next( m_farLevel );
        // The width of the background alone. A mix in which everything is spread as wide as it
        // will go has no depth left: the far plane pulled in towards the centre while the
        // foreground stays wide is the funnel that reads as distance rather than as width. At 1
        // the background is as the reverb made it, which is where every existing preset is.
        const float fw = m_smFarWidth.next( m_farWidth );
        float       bgL = fl[i], bgR = fr[i];
        if ( fw != 1.0f )
        {
            const float mid = 0.5f * ( bgL + bgR ), side = 0.5f * ( bgL - bgR ) * fw;
            bgL = mid + side;
            bgR = mid - side;
        }
        // The near layer's dry share joins here, past everything: it is the foreground's, so it
        // counts on the near stem.
        L[i] = nl[i] + bgL * far + dl[i];
        R[i] = nr[i] + bgR * far + dr[i];
        if ( m_stems != nullptr )
        {
            // The Cosmos return was added into the near bus above, so it has to come out of the
            // near stem or it would be counted twice and the four would no longer sum to the mix.
            // What the Cosmos sent into the far plane cannot be separated here at all -- the
            // reverb has already mixed it with everything else -- and belongs to the far stem,
            // which is where it is heard.
            m_stems[0][m_stemPos + i] = nl[i] - m_stems[4][m_stemPos + i] + dl[i];
            m_stems[1][m_stemPos + i] = nr[i] - m_stems[5][m_stemPos + i] + dr[i];
            m_stems[2][m_stemPos + i] = bgL * far;   // the far stem as it is heard: width included
            m_stems[3][m_stemPos + i] = bgR * far;
        }
    }
    if ( roomOn )
    {
        // The room's return, made above, ducked under the near bus band by band like the far hall
        // (the guide: "the near bus ducks the main room"), and added to the mix.
        m_roomUnmask.process( nl, nr, ol, orr, n );
        for ( int i = 0; i < n; ++i )
        {
            L[i] += ol[i];
            R[i] += orr[i];
            if ( m_stems != nullptr )
            {
                m_stems[6][m_stemPos + i] = ol[i];
                m_stems[7][m_stemPos + i] = orr[i];
            }
        }
    }
    else
    {
        if ( m_stems != nullptr )
            for ( int i = 0; i < n; ++i )
            {
                m_stems[6][m_stemPos + i] = 0.0f;
                m_stems[7][m_stemPos + i] = 0.0f;
            }
    }

    // Feedback loop, output side: the mix (before mid/side, sub and master) goes into the ring,
    // low-passed at Tone, driven into a soft saturation (tanh-like, gain-compensated), and
    // throttled by its own mean level like the shimmer loop: feedback -> 0 at a mean level of
    // 0.1 (about -20 dBFS), so a hot loop hisses and holds where a naked one would run into
    // the clipper -- and the loop can thicken a drone without taking it over (a higher
    // ceiling let Distant Storm climb 10 dB and collapse to a correlation of 0.6).
    if ( fbOn )
    {
        if ( !m_fbOsOn )
        {
            m_fbOsL.reset();
            m_fbOsR.reset();
        }   // back on: from silence, not from what it heard last time
        const float lpc = 1.0f - std::exp( -TwoPi * m_fbTone / static_cast<float>( m_sr ) );
        const float envC = 1.0f - std::exp( -1.0f / ( 0.05f * static_cast<float>( m_sr ) ) );
        const float drive = 1.0f + 9.0f * m_fbDrive;
        const float comp = 1.0f / std::sqrt( drive );
        auto sat = [](float x) {   // rational tanh, exact to 1e-3 up to |x| = 3
            x = clampv(x, -3.0f, 3.0f);
            const float x2 = x * x;
            return x * (27.0f + x2) / (27.0f + 9.0f * x2);
        };
        // DC blocker in the loop: a saturating loop with a DC gain above one locks onto a DC
        // operating point (Feedback Hiss sat at +0.11 before this), so nothing below 10 Hz circulates.
        const float hpc = 1.0f - TwoPi * 10.0f / static_cast<float>( m_sr );
        const float biasLp = 1.0f - std::exp( -TwoPi * 60.0f / static_cast<float>( m_sr ) );
        const float biasEnv = 1.0f - std::exp( -TwoPi * 5.0f / static_cast<float>( m_sr ) );
        for ( int i = 0; i < n; ++i )
        {
            m_fbLpL += lpc * ( L[i] - m_fbLpL );
            m_fbLpR += lpc * ( R[i] - m_fbLpR );
            float xl = m_fbLpL - m_fbHpXL + hpc * m_fbHpYL;
            m_fbHpXL = m_fbLpL;
            m_fbHpYL = xl;
            float xr = m_fbLpR - m_fbHpXR + hpc * m_fbHpYR;
            m_fbHpXR = m_fbLpR;
            m_fbHpYR = xr;
            float biasL = 0.0f, biasR = 0.0f;   // the operating point's offset, in the curve's own units
            if ( m_fbBias > 0.0f )
            {
                // The operating point: the bass's level, slow, pushed against the curve. The
                // offset goes in before the drive, so what it does scales with Drive as a real
                // stage's would; the DC it makes on the way out is the blocker's to clean up.
                m_fbBiasLpL += biasLp * ( xl - m_fbBiasLpL );
                m_fbBiasLpR += biasLp * ( xr - m_fbBiasLpR );
                m_fbBiasEnvL += biasEnv * ( std::fabs( m_fbBiasLpL ) - m_fbBiasEnvL );
                m_fbBiasEnvR += biasEnv * ( std::fabs( m_fbBiasLpR ) - m_fbBiasEnvR );
                // In the curve's units, after the drive, and never past 1.2: an offset larger than
                // the curve pushes the whole signal off its end and returns a flat line (measured:
                // the loop went from -40 to -87 dB when the offset was let ride on Drive).
                biasL = m_fbBias * 1.2f * m_fbBiasEnvL / ( m_fbBiasEnvL + 0.01f );
                biasR = m_fbBias * 1.2f * m_fbBiasEnvR / ( m_fbBiasEnvR + 0.01f );
            }
            const float mag = 0.5f * ( std::fabs( L[i] ) + std::fabs( R[i] ) );
            m_fbEnv += envC * ( mag - m_fbEnv );
            const int idx = ( m_fbW + i ) & m_fbMask;
            // Tape: asymmetric saturation (an even-order term the DC blocker cleans up on the next
            // pass) and a noise floor that rises with the level in the loop. The curve runs at four
            // times the rate (25.09.2026): in a loop an alias comes round again on every pass.
            const float asym = m_fbTape > 0.0f ? 0.2f * m_fbTape : 0.0f;
            auto        curveL = [&]( float v ) { return sat( ( v + asym * v * v ) * drive - biasL ) * comp; };
            auto        curveR = [&]( float v ) { return sat( ( v + asym * v * v ) * drive - biasR ) * comp; };
            m_fbRingL[static_cast<size_t>( idx )] = m_fbOsL.process( xl, curveL );
            m_fbRingR[static_cast<size_t>( idx )] = m_fbOsR.process( xr, curveR );
            if ( m_fbTape > 0.0f )
            {
                const float noise = 0.02f * m_fbTape * m_fbEnv;
                m_fbRingL[static_cast<size_t>( idx )] += noise * m_rng.bipolar();
                m_fbRingR[static_cast<size_t>( idx )] += noise * m_rng.bipolar();
            }
        }
        m_fbW = ( m_fbW + n ) & m_fbMask;
    }
    else
    {
        m_fbLpL = m_fbLpR = m_fbEnv = 0.0f;
        m_fbHpXL = m_fbHpXR = m_fbHpYL = m_fbHpYR = 0.0f;
        m_fbReg = 1.0f;
    }
    m_fbOsOn = fbOn;

    // Foundation: a dry sub voice on the brain's root, gliding between roots; the two ears
    // may run a few Hz apart (binaural beat), which Bass Mono leaves alone below its crossover
    // only in the mid channel -- so the beat is kept by feeding it after the mid/side stage.
    float     *subL = wl;
    float     *subR = wr;   // the delay scratch buffers are free by now
    const bool subOn = m_subLevel > 0.0f || m_subLevelCur > 1e-4f;
    if ( subOn )
    {
        const double rootHz = frequencyOf( m_brain.root() );
        double       subHz = rootHz / ( m_subOctave == 1 ? 2.0 : 4.0 );
        if ( m_subSource == 2 )
        {
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
            for ( const auto &v : m_voices )   // the near events are the foreground, not the chord the sub stands under
                if ( v.isActive() && v.owner() != OWNER_NEAR && ( lowest <= 0.0 || v.frequency() < lowest ) )
                    lowest = v.frequency();
            if ( lowest > 0.0 )
            {
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
                if ( lo > 0.0 && std::isfinite( hi ) )
                {
                    subHz = lowest / ( m_subOctave == 1 ? 2.0 : 4.0 );
                    while ( subHz >= hi ) subHz *= 0.5;
                    while ( subHz < lo ) subHz *= 2.0;
                }
            }
        }
        else if ( m_subSource == 1 )
        {
            // Ghost tone (Rich's combination tones): the difference between the two lowest sounding
            // voices is the tone the ear makes by itself in just intonation (3:2 -> f/2, 5:4 -> f/4,
            // 4:3 -> f/3). The sub doubles it, folded into the register the root mode would use,
            // so switching the source never changes the sub's range. With fewer than two distinct
            // pitches it falls back to the root.
            double f1 = 0.0, f2 = 0.0;
            for ( const auto &v : m_voices )
                if ( v.isActive() && ( f1 <= 0.0 || v.frequency() < f1 ) ) f1 = v.frequency();
            if ( f1 > 0.0 )
                for ( const auto &v : m_voices )
                    if ( v.isActive() && v.frequency() / f1 > 1.003 && ( f2 <= 0.0 || v.frequency() < f2 ) )
                        f2 = v.frequency();
            const double lo = subHz * 0.75, hi = lo * 2.0;   // the octave around the root sub
            if ( f1 > 0.0 && f2 > 0.0 && lo > 0.0 && std::isfinite( hi ) )
            {
                double ghost = f2 - f1;   // for the empty window, see Lowest above
                while ( ghost >= hi ) ghost *= 0.5;
                while ( ghost < lo ) ghost *= 2.0;
                subHz = ghost;
            }
        }
        const double target = std::log( std::max( subHz * static_cast<double>( m_vp.m_pitchMul ),
                                                  10.0 ) );   // the tide moves the sub with the voices
        if ( m_subFreqCur <= 0.0 ) m_subFreqCur = target;
        const double glideC = 1.0 - std::exp( -1.0 / ( std::max( m_subGlide, 0.05f ) * m_sr ) );
        const float  levelC = 1.0f - std::exp( -1.0f / ( 2.0f * static_cast<float>( m_sr ) ) );
        // The glide is a one-pole in the log domain, so its end point over this block is known in
        // closed form: take exp twice and walk the frequency linearly, instead of an exp per sample.
        const double reach = 1.0 - std::pow( 1.0 - glideC, static_cast<double>( n ) );
        const double fBegin = std::exp( m_subFreqCur );
        const double freqEnd = m_subFreqCur + ( target - m_subFreqCur ) * reach;
        const double fEnd = std::exp( freqEnd );
        const double fStep = n > 0 ? ( fEnd - fBegin ) / static_cast<double>( n ) : 0.0;
        double       f = fBegin;
        m_subFreqCur = freqEnd;
        for ( int i = 0; i < n; ++i )
        {
            m_subLevelCur += ( m_subLevel - m_subLevelCur ) * levelC;
            f += fStep;
            const double fL = std::max( f - 0.5 * m_subBinaural, 5.0 ), fR = std::max( f + 0.5 * m_subBinaural, 5.0 );
            m_subPhaseL += fL / m_sr;
            if ( m_subPhaseL >= 1.0 ) m_subPhaseL -= 1.0;
            m_subPhaseR += fR / m_sr;
            if ( m_subPhaseR >= 1.0 ) m_subPhaseR -= 1.0;
            const float triL = 4.0f * std::fabs( static_cast<float>( m_subPhaseL ) - 0.5f ) - 1.0f;
            const float triR = 4.0f * std::fabs( static_cast<float>( m_subPhaseR ) - 0.5f ) - 1.0f;
            const float g = m_subLevelCur * 0.45f;
            float       sL = ( 1.0f - m_subTone ) * sin01( m_subPhaseL ) + m_subTone * triL;
            float       sR = ( 1.0f - m_subTone ) * sin01( m_subPhaseR ) + m_subTone * triR;
            if ( m_subHarmonics > 0.0f )
            {
                // The residue (25.09.2026): the second and third harmonic at -24 and -27 dB at
                // full, from the sub's own phase, so they are exact and alias nothing. A speaker
                // that cannot move at the fundamental still gives the ear the note from them.
                const float h2 = 0.0631f * m_subHarmonics, h3 = 0.0447f * m_subHarmonics;
                double      q = m_subPhaseL * 2.0;
                q -= std::floor( q );
                sL += h2 * sin01( q );
                q = m_subPhaseL * 3.0;
                q -= std::floor( q );
                sL += h3 * sin01( q );
                q = m_subPhaseR * 2.0;
                q -= std::floor( q );
                sR += h2 * sin01( q );
                q = m_subPhaseR * 3.0;
                q -= std::floor( q );
                sR += h3 * sin01( q );
            }
            if ( m_subBeat > 0.0f || m_subBeatCur > 1.0e-4f )
            {
                // Beat (25.09.2026): a second sine Beat Hz above the sub, the same in both ears, so
                // the pair's level swells and fades once every 1/Beat seconds -- the slow breath
                // of a Lustmord sub, on the basilar membrane rather than between the ears. Half
                // and half, so the pair peaks where the sub alone did; the second sine fades in
                // over the first 0.05 Hz of the knob so turning it from 0 does not step.
                m_subBeatCur += ( m_subBeat - m_subBeatCur ) * levelC;
                m_subBeatPhase += ( f + m_subBeatCur ) / m_sr;
                if ( m_subBeatPhase >= 1.0 ) m_subBeatPhase -= 1.0;
                const float s2 = sin01( m_subBeatPhase );
                const float w = 0.5f * std::min( m_subBeatCur / 0.05f, 1.0f );
                sL = sL * ( 1.0f - w ) + s2 * w;
                sR = sR * ( 1.0f - w ) + s2 * w;
            }
            subL[i] = g * sL;
            subR[i] = g * sR;
            if ( m_subPulse > 0.0f || m_subPulseCur > 1.0e-4f )
            {
                // A raised cosine at the Binaural rate: 1 at the top of every cycle, 1 - Pulse at
                // the bottom, no corner anywhere. With Binaural at zero the phase stands still at
                // the top and nothing moves.
                m_subPulseCur += ( m_subPulse - m_subPulseCur ) * levelC;
                m_subPulsePhase += m_subBinaural / m_sr;
                if ( m_subPulsePhase >= 1.0 ) m_subPulsePhase -= 1.0;
                double q = m_subPulsePhase + 0.25;
                if ( q >= 1.0 ) q -= 1.0;   // sin01 reads a table: the phase must stay in [0, 1)
                const float pulse = 1.0f - m_subPulseCur * ( 0.5f - 0.5f * sin01( q ) );
                subL[i] *= pulse;
                subR[i] *= pulse;
            }
        }
    }
    // The body: the whole mix passes through a bank of modes and their answer is added back. It
    // sits before the mid/side stage, so its own width is treated like everything else's.
    if ( m_bodyLevel > 0.0f || m_smBody.m_value > 1.0e-4f )
    {
        float *mono = nl;   // the near bus is finished with; reuse it
        // The bank is linear, so the level rides on its input: one smoothed multiply per sample
        // and the body can be turned up mid-note without a step.
        for ( int i = 0; i < n; ++i ) mono[i] = 0.5f * ( L[i] + R[i] ) * m_smBody.next( m_bodyLevel );
        m_body.process( mono, L, R, n, 1.0f );
    }
    m_midSide.process( L, R, n );
    if ( subOn )
    {
        // The Foundation's own ceiling (25.09.2026). A beating sub swings its peaks by up to six
        // decibels (the production guide, section 8), and summed straight into the master it was the
        // sub that drove the soft clipper -- which then pumped everything else with it. The guide's
        // answer is a limiter on the sub alone, before the sum: here, at Ceiling dBFS at the output
        // (the master gain is taken out of it), instant to hold a peak, 5 ms to act on it and half a
        // second to let go -- slow enough that a 25 Hz sine is not distorted by its own limiter.
        const float master = dbToGain( m_masterGain );
        const float ceiling = dbToGain( m_subCeiling ) / std::max( master, 1.0e-3f );
        const float hold = std::exp( -1.0f / ( 0.5f * static_cast<float>( m_sr ) ) );
        const float att = 1.0f - std::exp( -1.0f / ( 0.005f * static_cast<float>( m_sr ) ) );
        const float rel = 1.0f - std::exp( -1.0f / ( 0.5f * static_cast<float>( m_sr ) ) );
        for ( int i = 0; i < n; ++i )
        {
            const float peak = std::max( std::fabs( subL[i] ), std::fabs( subR[i] ) );
            m_subPeak = peak > m_subPeak ? peak : m_subPeak * hold;
            const float target = m_subPeak > ceiling ? ceiling / m_subPeak : 1.0f;
            m_subLimGain += ( target < m_subLimGain ? att : rel ) * ( target - m_subLimGain );
            L[i] += subL[i] * m_subLimGain;
            R[i] += subR[i] * m_subLimGain;
        }
    }
    else
    {
        m_subPeak = 0.0f;
        m_subLimGain = 1.0f;
    }
    // Output DC blocker at 4 Hz, below the lowest sub the Foundation can reach. Several paths
    // can leave an offset behind -- FM at an integer ratio, the asymmetric tape term, a granular
    // window over a clip that carries one, the shimmer's pitch shifter -- and an offset costs
    // headroom in the soft clipper without ever being heard. One filter at the end covers them all.
    const float dcR = 1.0f - TwoPi * 4.0f / static_cast<float>( m_sr );
    // ...all but the Patina's own clipper, which sits before it. An offset that has grown past the
    // clipper's knee -- the far hall integrated one to -2.1 in thirty seconds -- leaves the music
    // riding on a rail the clipper flattens, and the blocker at the end then takes the rail away
    // and leaves nothing: three presets of the 2.0 library died that way, exactly to zero, while
    // every one of their stems played on. The same filter ahead of the Patina, and only while the
    // Patina is on, so a preset without it is rendered sample for sample as before.
    if ( m_patina.active() )
    {
        for ( int i = 0; i < n; ++i )
        {
            const float yl = L[i] - m_pdcXL + dcR * m_pdcYL;
            m_pdcXL = L[i];
            m_pdcYL = yl;
            L[i] = yl;
            const float yr = R[i] - m_pdcXR + dcR * m_pdcYR;
            m_pdcXR = R[i];
            m_pdcYR = yr;
            R[i] = yr;
        }
    }
    // The master's age, before the gain and the clipper: tape wow, lost highs, a noise floor.
    m_patina.process( L, R, n );
    // Subsonic: four cascaded one-pole high-passes, 24 dB/oct, on top of the 4 Hz DC blocker.
    // Off at zero, and zero is the default -- this instrument's Foundation reaches lower than the
    // frequency a mastering engineer cuts at, so the cut has to be the player's decision.
    // Four one-poles reach -3 dB at 2.299 times their own corner, so the pole goes there and the
    // knob keeps meaning the frequency the output is 3 dB down at.
    const float sub = m_subsonicHz;
    const float subC = sub <= 0.5f ? 0.0f : 1.0f - std::exp( -TwoPi * ( sub / 2.299f ) / static_cast<float>( m_sr ) );
    for ( int i = 0; i < n; ++i )
    {
        const float g = m_masterSmooth.next( master );
        float       yl = L[i] - m_dcXL + dcR * m_dcYL;
        m_dcXL = L[i];
        m_dcYL = yl;
        float yr = R[i] - m_dcXR + dcR * m_dcYR;
        m_dcXR = R[i];
        m_dcYR = yr;
        if ( subC > 0.0f )
        {
            m_subL1 += subC * ( yl - m_subL1 );
            yl -= m_subL1;
            m_subR1 += subC * ( yr - m_subR1 );
            yr -= m_subR1;
            m_subL2 += subC * ( yl - m_subL2 );
            yl -= m_subL2;
            m_subR2 += subC * ( yr - m_subR2 );
            yr -= m_subR2;
            m_subL3 += subC * ( yl - m_subL3 );
            yl -= m_subL3;
            m_subR3 += subC * ( yr - m_subR3 );
            yr -= m_subR3;
            m_subL4 += subC * ( yl - m_subL4 );
            yl -= m_subL4;
            m_subR4 += subC * ( yr - m_subR4 );
            yr -= m_subR4;
        }
        L[i] = softClip( yl * g );
        R[i] = softClip( yr * g );
        m_outTap[( m_outTapW + i ) & ( OutTapLen - 1 )] = 0.5f * ( L[i] + R[i] );
    }
    m_outTapW = ( m_outTapW + n ) & ( OutTapLen - 1 );
    // The loudness meter sees exactly what a file would: after the master gain and the clipper.
    m_loudness.process( L, R, n );
}

}   // namespace ambient

namespace ambient
{

/**
 * @brief The conductors on their own clock, with nothing rendered: the rule book's check is an hour of
 *        notes measured, and an hour of audio is not the way to get them.
 *
 * The seed and every parameter
 * come through readParams(), so what is measured is what the render would have played.
 */
void Engine::auditConductor( double seconds, double dt,
                             const std::function<void( double, int, const BrainEvent & )> &sink,
                             const std::function<void( double, int )>                     &rootSink )
{
    readParams();
    auto   freqOf = [this]( int note ) { return frequencyOf( note ); };
    double t = 0.0;
    int    lastRoot = m_brain.root();
    if ( rootSink ) rootSink( 0.0, lastRoot );
    const long steps = static_cast<long>( seconds / dt );
    for ( long i = 0; i < steps; ++i )
    {
        {
            const BrainParams *bpNow = &m_bp;
            BrainParams        bpSlow;
            if ( m_near.sequenceRunning() )
            {
                bpSlow = m_bp;
                bpSlow.m_rateSeconds *= 2.0f;
                bpNow = &bpSlow;
            }
            m_brain.update( dt, *bpNow, -1, freqOf, [&]( const BrainEvent &e ) { sink( t, 1, e ); } );
            if ( m_brain2On )
            {
                m_brain2.setRoot( clampv( m_brain.root() + m_brain2Interval, 0, 127 ) );
                m_brain2.update( dt, m_bp2, -1, freqOf, [&]( const BrainEvent &e ) { sink( t, 2, e ); } );
            }
        }
        // The near events as a third conductor: its notes on and off, as the render would play
        // them (a glide is reported as the new note beginning, a move not at all).
        stepNear( dt,
                  [&]( const NearNote &e )
                  {
                      if ( e.m_type == NearNote::TypeE::MOVE ) return;
                      sink( t, 3,
                            BrainEvent{ e.m_type == NearNote::TypeE::OFF ? BrainEvent::TypeE::NOTE_OFF
                                                                         : BrainEvent::TypeE::NOTE_ON,
                                        e.m_note, e.m_velocity } );
                  } );
        if ( m_brain.root() != lastRoot )
        {
            lastRoot = m_brain.root();
            if ( rootSink ) rootSink( t, lastRoot );
        }
        t += dt;
    }
}

}   // namespace ambient
