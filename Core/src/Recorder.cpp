/**
 * @file Recorder.cpp
 * @brief The framework-free WAV recorder: a lock-free ring fed by the audio thread, drained to disk
 *        by a thread of its own.
 *
 * Two threads meet here and never wait for each other. The audio thread calls write() once per
 * block and only ever touches the ring and its head index; if the ring is full the block is
 * counted as dropped and the thread moves on, because a stall there is a click in the room. The
 * writer thread started by start() sleeps in 20 ms steps, copies whatever has arrived into an
 * 8192-float chunk and hands it to fwrite, and runs until stop() asks it to and the ring is empty.
 * The ring holds half a million floats, about 5.4 s of stereo at 48 kHz, which is far more than any
 * disk hiccup the recorder has met.
 *
 * The file is 32-bit float WAV (format tag 3). The RIFF header is written with a zero data length
 * when the file is opened and patched in stop(), once the frame count is known; a recording that
 * outgrows the 32-bit length fields is capped at 0xFFFFFFFF there, which is what every other WAV
 * writer does and what readWavChannels (WavFile.cpp) knows to expect. The Quest app and any host
 * without a writer of its own record through this; the desktop plugin records through JUCE.
 * @author Rene Weller
 */
#include "ambient/Recorder.h"
#include <chrono>
#include <cstring>

namespace ambient
{

bool WavRecorder::start( const char *path, int sampleRate, int channels )
{
    stop();
    m_file = std::fopen( path, "wb" );
    if ( m_file == nullptr ) return false;
    m_sampleRate = sampleRate;
    m_channels = channels;
    const uint32_t size = 1u << 19;   // 512k floats = ~5.4 s of stereo at 48 kHz
    m_ring.assign( size, 0.0f );
    m_mask = size - 1;
    m_head.store( 0 );
    m_tail.store( 0 );
    m_framesWritten.store( 0 );
    m_framesDropped.store( 0 );
    writeHeader( 0 );
    m_stopRequested.store( false );
    m_recording.store( true );
    m_thread = std::thread( [this] { run(); } );
    return true;
}

void WavRecorder::stop()
{
    if ( !m_recording.load() && m_file == nullptr ) return;
    m_recording.store( false );
    m_stopRequested.store( true );
    if ( m_thread.joinable() ) m_thread.join();
    if ( m_file )
    {
        const uint64_t bytes = m_framesWritten.load() * static_cast<uint64_t>( m_channels ) * 4u;
        writeHeader( static_cast<uint32_t>( bytes > 0xFFFFFFFFull ? 0xFFFFFFFFull : bytes ) );
        std::fclose( m_file );
        m_file = nullptr;
    }
}

void WavRecorder::write( const float *L, const float *R, int n )
{
    if ( !m_recording.load( std::memory_order_relaxed ) ) return;
    const uint32_t head = m_head.load( std::memory_order_relaxed );
    const uint32_t tail = m_tail.load( std::memory_order_acquire );
    const uint32_t free = m_mask + 1 - ( head - tail );
    const uint32_t need = static_cast<uint32_t>( n ) * 2u;
    if ( need > free )
    {
        m_framesDropped.fetch_add( static_cast<uint64_t>( n ), std::memory_order_relaxed );
        return;
    }
    float *r = m_ring.data();
    for ( int i = 0; i < n; ++i )
    {
        r[( head + static_cast<uint32_t>( i ) * 2u ) & m_mask] = L[i];
        r[( head + static_cast<uint32_t>( i ) * 2u + 1u ) & m_mask] = R[i];
    }
    m_head.store( head + need, std::memory_order_release );
}

void WavRecorder::run()
{
    std::vector<float> chunk( 8192 );
    for ( ;; )
    {
        const uint32_t head = m_head.load( std::memory_order_acquire );
        uint32_t       tail = m_tail.load( std::memory_order_relaxed );
        uint32_t       avail = head - tail;
        if ( avail == 0 )
        {
            if ( m_stopRequested.load() ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
            continue;
        }
        while ( avail > 0 )
        {
            const uint32_t take = std::min<uint32_t>( avail, static_cast<uint32_t>( chunk.size() ) );
            for ( uint32_t i = 0; i < take; ++i ) chunk[i] = m_ring[( tail + i ) & m_mask];
            std::fwrite( chunk.data(), sizeof( float ), take, m_file );
            m_framesWritten.fetch_add( take / 2u, std::memory_order_relaxed );
            tail += take;
            avail -= take;
        }
        m_tail.store( tail, std::memory_order_release );
    }
    std::fflush( m_file );
}

void WavRecorder::writeHeader( uint32_t dataBytes )
{
    if ( m_file == nullptr ) return;
    std::fseek( m_file, 0, SEEK_SET );
    auto u32 = [&]( uint32_t v ) { std::fwrite( &v, 4, 1, m_file ); };
    auto u16 = [&]( uint16_t v ) { std::fwrite( &v, 2, 1, m_file ); };
    std::fwrite( "RIFF", 1, 4, m_file );
    u32( 36 + dataBytes );
    std::fwrite( "WAVE", 1, 4, m_file );
    std::fwrite( "fmt ", 1, 4, m_file );
    u32( 16 );
    u16( 3 );
    u16( static_cast<uint16_t>( m_channels ) );
    u32( static_cast<uint32_t>( m_sampleRate ) );
    u32( static_cast<uint32_t>( m_sampleRate * m_channels * 4 ) );
    u16( static_cast<uint16_t>( m_channels * 4 ) );
    u16( 32 );
    std::fwrite( "data", 1, 4, m_file );
    u32( dataBytes );
    std::fseek( m_file, 0, SEEK_END );
}

}   // namespace ambient
