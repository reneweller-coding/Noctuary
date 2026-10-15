/**
 * @file Shifter.cpp
 * @brief The spectral pitch shifter: frame analysis, peak-locked shifting and overlap-add.
 *
 * Shifter.h explains why the shimmer shifts in the spectrum rather than with two read heads; this
 * file is the phase vocoder itself. process() runs sample by sample on the audio thread: every
 * input sample goes into a N ring, every output sample comes out of a 2 N overlap-add buffer,
 * and every Hop samples frame() is run once per channel. frame() windows the last N samples
 * with a Hann, takes the FFT, finds the spectral peaks (local maxima over two bins either side,
 * no more than 70 dB under the loudest), and for each peak works out its true frequency from the
 * phase advance since the previous frame. The peak and every bin of its region -- up to the trough
 * before the next peak -- are then moved by the same number of bins to the shifted frequency and
 * turned by one angle, so that the region keeps its internal phase relations (Laroche and
 * Dolson's identity phase locking) and the output bin continues the phase it had at the shifted
 * frequency in the previous frame. The shifted half-spectrum is mirrored into a real signal,
 * inverse transformed, windowed again and added into the output ring.
 *
 * Everything is allocated in prepare(); process() may run in place, since the input sample is
 * read before the output sample is written. The shift ratio is set by setSemitones() and is read
 * at the next frame.
 * @author Rene Weller
 */
#include "ambient/Shifter.h"
#include <algorithm>
#include <cmath>

namespace ambient
{

namespace
{
constexpr double Tau = 6.283185307179586;   ///< 2 pi: the Hann window and every phase in frame() are in radians
/**
 * @brief Gain of the overlap-add, so that a frame passed through unchanged comes out at unity.
 *
 * Hann analysis and Hann synthesis at a quarter-frame hop add up to one and a half.
 */
constexpr float OverlapGain = 2.0f / 3.0f;
}   // namespace

void SpectralShifter::prepare( double )
{
    m_window.resize( N );
    for ( int i = 0; i < N; ++i )
        m_window[static_cast<size_t>( i )] = static_cast<float>( 0.5 - 0.5 * std::cos( Tau * i / N ) );
    const size_t half = static_cast<size_t>( N / 2 + 1 );
    for ( auto &c : m_ch )
    {
        c.m_in.assign( N, 0.0f );
        c.m_out.assign( 2 * N, 0.0f );
        c.m_re.assign( N, 0.0f );
        c.m_im.assign( N, 0.0f );
        c.m_power.assign( half, 0.0f );
        c.m_prevRe.assign( half, 0.0f );
        c.m_prevIm.assign( half, 0.0f );
        c.m_accRe.assign( half, 0.0f );
        c.m_accIm.assign( half, 0.0f );
        c.m_prevOutRe.assign( half, 0.0f );
        c.m_prevOutIm.assign( half, 0.0f );
        c.m_inPos = 0;
        c.m_outPos = 0;
    }
    m_peaks.assign( static_cast<size_t>( N / 2 ), 0 );
    m_hopCounter = 0;
}

void SpectralShifter::reset()
{
    for ( auto &c : m_ch )
    {
        for ( auto *v : { &c.m_in, &c.m_out, &c.m_re, &c.m_im, &c.m_power, &c.m_prevRe, &c.m_prevIm, &c.m_accRe,
                          &c.m_accIm, &c.m_prevOutRe, &c.m_prevOutIm } )
            std::fill( v->begin(), v->end(), 0.0f );
    }
    m_hopCounter = 0;
}

void SpectralShifter::setSemitones( float semitones )
{
    m_ratio = std::pow( 2.0, static_cast<double>( clampv( semitones, -36.0f, 36.0f ) ) / 12.0 );
}

void SpectralShifter::frame( Channel &c )
{
    constexpr int K = N / 2;
    const int     inMask = N - 1, outMask = 2 * N - 1;
    for ( int i = 0; i < N; ++i )
        c.m_re[static_cast<size_t>( i )] =
            c.m_in[static_cast<size_t>( ( c.m_inPos - N + i ) & inMask )] * m_window[static_cast<size_t>( i )];
    m_fft.forward( c.m_re.data(), c.m_re.data(), c.m_im.data() );
    float top = 0.0f;
    for ( int k = 0; k <= K; ++k )
    {
        const float p = c.m_re[static_cast<size_t>( k )] * c.m_re[static_cast<size_t>( k )] +
                        c.m_im[static_cast<size_t>( k )] * c.m_im[static_cast<size_t>( k )];
        c.m_power[static_cast<size_t>( k )] = p;
        top = std::max( top, p );
    }
    std::fill( c.m_accRe.begin(), c.m_accRe.end(), 0.0f );
    std::fill( c.m_accIm.begin(), c.m_accIm.end(), 0.0f );
    if ( top > 1.0e-18f )
    {
        // The peaks: local maxima over two bins either side, no more than 70 dB under the loudest.
        const float floor = top * 1.0e-7f;
        int         np = 0;
        for ( int k = 2; k < K - 2; ++k )
        {
            const float p = c.m_power[static_cast<size_t>( k )];
            if ( p > floor && p >= c.m_power[static_cast<size_t>( k - 1 )] &&
                 p > c.m_power[static_cast<size_t>( k + 1 )] && p >= c.m_power[static_cast<size_t>( k - 2 )] &&
                 p > c.m_power[static_cast<size_t>( k + 2 )] )
                m_peaks[static_cast<size_t>( np++ )] = k;
        }
        const double w0 = Tau / N;
        int          lo = 1;
        for ( int j = 0; j < np; ++j )
        {
            const int kp = m_peaks[static_cast<size_t>( j )];
            // A peak's region runs to the trough between it and the next peak.
            int hi = K - 1;
            if ( j + 1 < np )
            {
                const int kn = m_peaks[static_cast<size_t>( j + 1 )];
                hi = kp;
                float least = c.m_power[static_cast<size_t>( kp )];
                for ( int k = kp + 1; k < kn; ++k )
                    if ( c.m_power[static_cast<size_t>( k )] < least )
                    {
                        least = c.m_power[static_cast<size_t>( k )];
                        hi = k;
                    }
            }
            const int kq = static_cast<int>( std::lround( static_cast<double>( kp ) * m_ratio ) );
            if ( kq >= 1 && kq < K - 1 )
            {
                const int dk = kq - kp;
                // The peak's own frequency, from how far its phase moved since the last frame beyond
                // what the centre of its bin accounts for.
                const double ph = std::atan2( c.m_im[static_cast<size_t>( kp )], c.m_re[static_cast<size_t>( kp )] );
                const double prev =
                    std::atan2( c.m_prevIm[static_cast<size_t>( kp )], c.m_prevRe[static_cast<size_t>( kp )] );
                double dev = ph - prev - w0 * kp * Hop;
                dev -= Tau * std::round( dev / Tau );
                const double omega = w0 * kp + dev / Hop;
                // At its new place it continues the phase that bin had at the shifted frequency, and
                // the whole region is turned by the same angle, so it moves as one.
                const double outPrev =
                    std::atan2( c.m_prevOutIm[static_cast<size_t>( kq )], c.m_prevOutRe[static_cast<size_t>( kq )] );
                const double rot = outPrev + omega * m_ratio * Hop - ph;
                const float  cr = static_cast<float>( std::cos( rot ) ), sr = static_cast<float>( std::sin( rot ) );
                for ( int k = lo; k <= hi; ++k )
                {
                    const int q = k + dk;
                    if ( q < 1 || q >= K ) continue;
                    const float re = c.m_re[static_cast<size_t>( k )], im = c.m_im[static_cast<size_t>( k )];
                    c.m_accRe[static_cast<size_t>( q )] += re * cr - im * sr;
                    c.m_accIm[static_cast<size_t>( q )] += re * sr + im * cr;
                }
            }
            lo = hi + 1;
        }
    }
    // What the next frame measures its phases against.
    std::copy( c.m_re.begin(), c.m_re.begin() + K + 1, c.m_prevRe.begin() );
    std::copy( c.m_im.begin(), c.m_im.begin() + K + 1, c.m_prevIm.begin() );
    std::copy( c.m_accRe.begin(), c.m_accRe.end(), c.m_prevOutRe.begin() );
    std::copy( c.m_accIm.begin(), c.m_accIm.end(), c.m_prevOutIm.begin() );
    // Back to time: the shifted half-spectrum mirrored into a real signal, windowed and overlapped.
    std::fill( c.m_re.begin(), c.m_re.end(), 0.0f );
    std::fill( c.m_im.begin(), c.m_im.end(), 0.0f );
    for ( int k = 1; k < K; ++k )
    {
        c.m_re[static_cast<size_t>( k )] = c.m_accRe[static_cast<size_t>( k )];
        c.m_im[static_cast<size_t>( k )] = c.m_accIm[static_cast<size_t>( k )];
        c.m_re[static_cast<size_t>( N - k )] = c.m_accRe[static_cast<size_t>( k )];
        c.m_im[static_cast<size_t>( N - k )] = -c.m_accIm[static_cast<size_t>( k )];
    }
    m_fft.inverse( c.m_re.data(), c.m_im.data(), c.m_re.data() );
    for ( int i = 0; i < N; ++i )
        c.m_out[static_cast<size_t>( ( c.m_outPos + i ) & outMask )] +=
            c.m_re[static_cast<size_t>( i )] * m_window[static_cast<size_t>( i )] * OverlapGain;
}

void SpectralShifter::process( const float *inL, const float *inR, float *outL, float *outR, int n )
{
    const int    inMask = N - 1, outMask = 2 * N - 1;
    const float *ins[2] = { inL, inR };
    float       *outs[2] = { outL, outR };
    for ( int i = 0; i < n; ++i )
    {
        for ( int ci = 0; ci < 2; ++ci )
        {
            Channel    &c = m_ch[ci];
            const float x = ins[ci][i];   // read before the write: in place is allowed
            c.m_in[static_cast<size_t>( c.m_inPos )] = x;
            c.m_inPos = ( c.m_inPos + 1 ) & inMask;
            const size_t o = static_cast<size_t>( c.m_outPos );
            outs[ci][i] = c.m_out[o];
            c.m_out[o] = 0.0f;
            c.m_outPos = ( c.m_outPos + 1 ) & outMask;
        }
        if ( ++m_hopCounter >= Hop )
        {
            m_hopCounter = 0;
            frame( m_ch[0] );
            frame( m_ch[1] );
        }
    }
}

}   // namespace ambient
