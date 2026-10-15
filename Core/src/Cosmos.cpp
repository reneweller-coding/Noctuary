/**
 * @file Cosmos.cpp
 * @brief The Cosmos processors and the two transforms the instrument shares.
 *
 * Cosmos.h lists the processors; this file is their inner loops. FreqShifter runs Niemitalo's
 * Hilbert pair (the tables below) on each channel and rotates the analytic signal by a phasor, the
 * right ear three per cent slower than the left; CombResonator is a stereo feedback comb with a
 * one-pole in the loop and glided delays, the right a hair longer than the left; VowelFilter
 * re-tunes three band passes every 64 samples along a drifting path through a-e-i-o-u; PitchShifter
 * is the two-head granular shifter of the shimmer loop. Fft is the plain radix-2 complex transform
 * with precomputed twiddles and bit reversal, and RealFft the real-signal transform built on half
 * of one -- its derivation and the in-place hazards are in the header, the step-by-step index
 * bookkeeping in the bodies here. Nebula takes a Hann frame of N every Hop samples, smooths each
 * bin's magnitude with the Smear coefficient, gives every bin a fresh random phase and overlap-adds
 * the real inverse. Everything here runs on the audio thread except the constructors and prepare(),
 * which allocate. ringRead() and sin01() come from Effects.h and Dsp.h.
 * @author Rene Weller
 */
#include "ambient/Cosmos.h"
#include "ambient/Effects.h"
#include <cmath>
#include <algorithm>

namespace ambient
{

namespace
{
/**
 * @brief The smallest power of two that is at least @p n: the ring sizes, so a mask can wrap them.
 * @param[in] n  the number of samples a ring has to hold
 * @return the first power of two >= n (1 for n <= 1)
 */
int pow2At( int n )
{
    int p = 1;
    while ( p < n ) p <<= 1;
    return p;
}
/**
 * @name Hilbert pair
 * Olli Niemitalo's 90-degree phase-splitting all-pass pair (coefficients squared).
 * Two chains of four second-order all-passes whose outputs are 90 degrees apart across the band;
 * FreqShifter::Hilbert::tick() runs both on one input and takes chain A a sample late as the real
 * part and chain B as the imaginary part.
 * @{ */
const float HilbertA[4] = { 0.6923878f, 0.9360654322959f, 0.9882295226860f,
                            0.9987488452737f };   ///< chain A, the real part (one sample delayed)
const float HilbertB[4] = { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f,
                            0.9952884791278f };   ///< chain B, the imaginary part
/** @} */
}   // namespace

// ---------------------------------------------------------------- FreqShifter

void FreqShifter::Hilbert::tick( float in, float &re, float &im )
{
    float a = in;
    for ( int k = 0; k < 4; ++k )
    {
        const float out = HilbertA[k] * ( a + m_y2[k] ) - m_x2[k];
        m_x2[k] = m_x1[k];
        m_x1[k] = a;
        m_y2[k] = m_y1[k];
        m_y1[k] = out;
        a = out;
    }
    float b = in;
    for ( int k = 0; k < 4; ++k )
    {
        const float out = HilbertB[k] * ( b + m_y2b[k] ) - m_x2b[k];
        m_x2b[k] = m_x1b[k];
        m_x1b[k] = b;
        m_y2b[k] = m_y1b[k];
        m_y1b[k] = out;
        b = out;
    }
    re = m_delayed;   // chain A lags chain B by one sample
    m_delayed = a;
    im = b;
}

void FreqShifter::prepare( double sampleRate )
{
    m_sr = sampleRate;
    m_hL = Hilbert{};
    m_hR = Hilbert{};
    m_ph = 0.0;
}

void FreqShifter::process( float *L, float *R, int n )
{
    if ( m_mix <= 0.0f ) return;
    for ( int i = 0; i < n; ++i )
    {
        m_ph += m_shift / m_sr;
        if ( m_ph > 1.0e6 || m_ph < -1.0e6 ) m_ph = 0.0;
        double pl = m_ph - std::floor( m_ph );
        double pr = m_ph * 0.97;
        pr -= std::floor( pr );   // right ear shifted 3 % less: slow cosmic beating
        double ql = pl + 0.25;
        if ( ql >= 1.0 ) ql -= 1.0;
        double qr = pr + 0.25;
        if ( qr >= 1.0 ) qr -= 1.0;
        float re, im;
        m_hL.tick( L[i], re, im );
        const float sl = re * sin01( ql ) + im * sin01( pl );   // positive shift moves the spectrum up
        m_hR.tick( R[i], re, im );
        const float sr = re * sin01( qr ) + im * sin01( pr );
        L[i] = L[i] * ( 1.0f - m_mix ) + sl * m_mix;
        R[i] = R[i] * ( 1.0f - m_mix ) + sr * m_mix;
    }
}

// ---------------------------------------------------------------- CombResonator

void CombResonator::prepare( double sampleRate )
{
    m_sr = sampleRate;
    const int size = pow2At( static_cast<int>( m_sr / 20.0 ) + 8 );
    m_bufL.assign( static_cast<size_t>( size ), 0.0f );
    m_bufR.assign( static_cast<size_t>( size ), 0.0f );
    m_mask = size - 1;
    m_w = 0;
    m_lpL = m_lpR = 0.0f;
    m_dLcur = m_dRcur = 0.0f;
}

void CombResonator::set( float freqHz, float feedback, float mix )
{
    const float maxD = static_cast<float>( m_mask ) - 4.0f;
    const float f = clampv( freqHz, 20.0f, 4000.0f );
    m_dL = clampv( static_cast<float>( m_sr ) / f, 2.0f, maxD );
    m_dR = clampv( m_dL * 1.003f, 2.0f, maxD );
    if ( m_dLcur <= 0.0f )
    {
        m_dLcur = m_dL;
        m_dRcur = m_dR;
    }
    m_fb = clampv( feedback, 0.0f, 0.97f );
    m_mix = clampv( mix, 0.0f, 1.0f );
}

void CombResonator::process( float *L, float *R, int n )
{
    if ( m_mix <= 0.0f )
    {
        for ( int i = 0; i < n; ++i )
        {
            m_bufL[static_cast<size_t>( m_w & m_mask )] = L[i];
            m_bufR[static_cast<size_t>( m_w & m_mask )] = R[i];
            ++m_w;
        }
        return;
    }
    float *bl = m_bufL.data();
    float *br = m_bufR.data();
    for ( int i = 0; i < n; ++i )
    {
        m_dLcur += ( m_dL - m_dLcur ) * 0.0005f;
        m_dRcur += ( m_dR - m_dRcur ) * 0.0005f;
        const float yl = ringRead( bl, m_mask, m_w, m_dLcur + 1.0f );
        const float yr = ringRead( br, m_mask, m_w, m_dRcur + 1.0f );
        m_lpL += 0.5f * ( yl - m_lpL );
        m_lpR += 0.5f * ( yr - m_lpR );
        const float vl = L[i] + m_fb * m_lpL;
        const float vr = R[i] + m_fb * m_lpR;
        bl[m_w & m_mask] = vl;
        br[m_w & m_mask] = vr;
        const float norm = ( 1.0f - m_fb ) + 0.05f;   // resonant peak stays near unity gain
        L[i] = L[i] * ( 1.0f - m_mix ) + vl * m_mix * norm;
        R[i] = R[i] * ( 1.0f - m_mix ) + vr * m_mix * norm;
        ++m_w;
    }
}

// ---------------------------------------------------------------- VowelFilter

void VowelFilter::prepare( double sampleRate, uint64_t seed )
{
    m_sr = sampleRate;
    m_rng.seed( seed );
    m_morph.init( m_rng );
    for ( auto &f : m_fL ) f.reset();
    for ( auto &f : m_fR ) f.reset();
    m_counter = 0;
}

void VowelFilter::process( float *L, float *R, int n )
{
    static const float Formants[5][3] = {
        { 800.0f, 1150.0f, 2900.0f },   // a
        { 400.0f, 1600.0f, 2700.0f },   // e
        { 350.0f, 1700.0f, 2700.0f },   // i
        { 450.0f, 800.0f, 2830.0f },    // o
        { 325.0f, 700.0f, 2530.0f },    // u
    };
    static const float Gain[3] = { 1.0f, 0.5f, 0.25f };
    if ( m_mix <= 0.0f ) return;
    for ( int i = 0; i < n; ++i )
    {
        if ( m_counter-- <= 0 )
        {
            m_counter = 63;
            const float m = 2.0f + 2.0f * m_morph.update( 64.0f / static_cast<float>( m_sr ), m_rate, m_rng );
            const int   i0 = clampv( static_cast<int>( m ), 0, 3 );
            const float t = clampv( m - static_cast<float>( i0 ), 0.0f, 1.0f );
            for ( int k = 0; k < 3; ++k )
            {
                const float f = std::pow( Formants[i0][k], 1.0f - t ) * std::pow( Formants[i0 + 1][k], t );
                m_fL[k].setQ( f, 8.0f, static_cast<float>( m_sr ) );
                m_fR[k].setQ( f * 1.01f, 8.0f, static_cast<float>( m_sr ) );
            }
        }
        float wl = 0.0f, wr = 0.0f, lp, bp, hp;
        for ( int k = 0; k < 3; ++k )
        {
            m_fL[k].tick( L[i], lp, bp, hp );
            wl += bp * Gain[k];
            m_fR[k].tick( R[i], lp, bp, hp );
            wr += bp * Gain[k];
        }
        L[i] = L[i] * ( 1.0f - m_mix ) + wl * m_mix * 0.8f;
        R[i] = R[i] * ( 1.0f - m_mix ) + wr * m_mix * 0.8f;
    }
}

// ---------------------------------------------------------------- PitchShifter

void PitchShifter::prepare( double sampleRate )
{
    m_sr = sampleRate;
    const int size = pow2At( static_cast<int>( 0.2 * m_sr ) + 8 );
    m_buf.assign( static_cast<size_t>( size ), 0.0f );
    m_mask = size - 1;
    m_w = 0;
    m_window = 0.08f * static_cast<float>( m_sr );
    m_pos = 0.0f;
}

void PitchShifter::setSemitones( float st )
{
    m_rate = std::pow( 2.0f, st / 12.0f );
}

void PitchShifter::process( const float *in, float *out, int n )
{
    float      *b = m_buf.data();
    const float step = ( m_rate - 1.0f ) / m_window;
    for ( int i = 0; i < n; ++i )
    {
        b[m_w & m_mask] = in[i];
        m_pos += step;
        m_pos -= std::floor( m_pos );
        float p2 = m_pos + 0.5f;
        if ( p2 >= 1.0f ) p2 -= 1.0f;
        const float d1 = ( 1.0f - m_pos ) * m_window + 1.0f;
        const float d2 = ( 1.0f - p2 ) * m_window + 1.0f;
        const float g1 = sin01( 0.5 * m_pos );   // sin(pi * p)
        const float g2 = sin01( 0.5 * p2 );
        out[i] = ringRead( b, m_mask, m_w, d1 ) * g1 + ringRead( b, m_mask, m_w, d2 ) * g2;
        ++m_w;
    }
}

// ---------------------------------------------------------------- Fft

Fft::Fft( int n ) : m_n( n )
{
    m_cos.resize( static_cast<size_t>( n / 2 ) );
    m_sin.resize( static_cast<size_t>( n / 2 ) );
    for ( int i = 0; i < n / 2; ++i )
    {
        m_cos[static_cast<size_t>( i )] = std::cos( TwoPi * i / n );
        m_sin[static_cast<size_t>( i )] = std::sin( TwoPi * i / n );
    }
    m_rev.resize( static_cast<size_t>( n ) );
    int bits = 0;
    while ( ( 1 << bits ) < n ) ++bits;
    for ( int i = 0; i < n; ++i )
    {
        int r = 0;
        for ( int b = 0; b < bits; ++b )
            if ( i & ( 1 << b ) ) r |= 1 << ( bits - 1 - b );
        m_rev[static_cast<size_t>( i )] = r;
    }
}

void Fft::transform( float *re, float *im, bool inverse ) const
{
    for ( int i = 0; i < m_n; ++i )
    {
        const int j = m_rev[static_cast<size_t>( i )];
        if ( j > i )
        {
            std::swap( re[i], re[j] );
            std::swap( im[i], im[j] );
        }
    }
    for ( int len = 2; len <= m_n; len <<= 1 )
    {
        const int half = len / 2, stride = m_n / len;
        for ( int start = 0; start < m_n; start += len )
        {
            for ( int k = 0; k < half; ++k )
            {
                const float c = m_cos[static_cast<size_t>( k * stride )];
                const float s =
                    inverse ? m_sin[static_cast<size_t>( k * stride )] : -m_sin[static_cast<size_t>( k * stride )];
                const int   a = start + k, b = a + half;
                const float tr = re[b] * c - im[b] * s;
                const float ti = re[b] * s + im[b] * c;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
    if ( inverse )
    {
        const float inv = 1.0f / static_cast<float>( m_n );
        for ( int i = 0; i < m_n; ++i )
        {
            re[i] *= inv;
            im[i] *= inv;
        }
    }
}

// ---------------------------------------------------------------- RealFft

RealFft::RealFft( int n ) : m_n( n ), m_half( n / 2 )
{
    const int m = n / 2;
    m_tc.resize( static_cast<size_t>( m + 1 ) );
    m_ts.resize( static_cast<size_t>( m + 1 ) );
    for ( int k = 0; k <= m; ++k )
    {
        m_tc[static_cast<size_t>( k )] = static_cast<float>( std::cos( TwoPi * k / n ) );
        m_ts[static_cast<size_t>( k )] = static_cast<float>( std::sin( TwoPi * k / n ) );
    }
    m_work.assign( static_cast<size_t>( m ), 0.0f );
}

void RealFft::forward( const float *x, float *re, float *im ) const
{
    const int m = m_n / 2;
    // The pairs, read forward so that writing re[k] cannot reach a sample not yet taken: it wants
    // x[2k] and x[2k+1], and 2k >= k.
    for ( int k = 0; k < m; ++k )
    {
        const float a = x[2 * k], b = x[2 * k + 1];
        re[k] = a;
        im[k] = b;
    }
    m_half.transform( re, im, false );
    // Apart again. k and m-k are done together because each needs the other's value, so the loop
    // runs to m/2 and the two ends are written in one step; the middle bin (k = m/2, where k and
    // m-k are the same bin) and the two ends fall out of the same formula.
    const float z0r = re[0], z0i = im[0];
    for ( int k = 1; k <= m / 2; ++k )
    {
        const int   j = m - k;
        const float akr = re[k], aki = im[k], bkr = re[j], bki = im[j];
        // Fe = (Z[k] + conj(Z[m-k])) / 2,  Fo = -i (Z[k] - conj(Z[m-k])) / 2
        const float er = 0.5f * ( akr + bkr ), ei = 0.5f * ( aki - bki );
        const float or_ = 0.5f * ( aki + bki ), oi = -0.5f * ( akr - bkr );
        const float wc = m_tc[static_cast<size_t>( k )], ws = -m_ts[static_cast<size_t>( k )];   // e^(-2 pi i k / n)
        // X[k] = Fe + W * Fo
        const float xr = er + ( or_ * wc - oi * ws ), xi = ei + ( or_ * ws + oi * wc );
        // X[m-k] is the same with W at m-k, which is the conjugate of the rotation at k reflected:
        // e^(-2 pi i (m-k) / n) = -conj(e^(-2 pi i k / n)). Fe and Fo at m-k are the conjugates.
        const float er2 = er, ei2 = -ei, or2 = or_, oi2 = -oi;
        const float wc2 = -wc, ws2 = ws;
        const float yr = er2 + ( or2 * wc2 - oi2 * ws2 ), yi = ei2 + ( or2 * ws2 + oi2 * wc2 );
        re[k] = xr;
        im[k] = xi;
        re[j] = yr;
        im[j] = yi;
    }
    // The ends: X[0] and X[n/2] are both real, and both come out of Z[0] alone.
    re[0] = z0r + z0i;
    im[0] = 0.0f;
    re[m] = z0r - z0i;
    im[m] = 0.0f;
    // The conjugate mirror, so the array reads like the one Fft leaves behind.
    for ( int k = 1; k < m; ++k )
    {
        re[m_n - k] = re[k];
        im[m_n - k] = -im[k];
    }
}

void RealFft::inverse( const float *re, const float *im, float *x, float *scratch ) const
{
    const int m = m_n / 2;
    // Z's real parts go into x[0 .. m-1] and its imaginary parts into the scratch half, which is
    // what lets x be the caller's own `re` array: every write at k or m-k lands on a bin this
    // step has already read.
    float      *zi = scratch;
    const float x0 = re[0], xm = re[m];
    for ( int k = 1; k < m - k; ++k )
    {
        const int   j = m - k;
        const float akr = re[k], aki = im[k], bkr = re[j], bki = im[j];
        // Fe = (X[k] + conj(X[m-k])) / 2
        const float er = 0.5f * ( akr + bkr ), ei = 0.5f * ( aki - bki );
        // Fo = (X[k] - conj(X[m-k])) / 2 * e^(+2 pi i k / n), undoing the rotation the forward put on
        const float dr = 0.5f * ( akr - bkr ), di = 0.5f * ( aki + bki );
        const float wc = m_tc[static_cast<size_t>( k )], ws = m_ts[static_cast<size_t>( k )];
        const float fr = dr * wc - di * ws, fi = dr * ws + di * wc;
        // Z[k] = Fe + i Fo,  Z[m-k] = conj(Fe) + i conj(Fo)
        x[k] = er - fi;
        zi[k] = ei + fr;
        x[j] = er + fi;
        zi[j] = fr - ei;
    }
    // Z[0], out of the two bins that are real: X[0] = Re Z0 + Im Z0, X[n/2] = Re Z0 - Im Z0.
    x[0] = 0.5f * ( x0 + xm );
    zi[0] = 0.5f * ( x0 - xm );
    // The middle bin, where k and m-k are the same one and the pair above would write it twice:
    // the forward leaves X[m/2] = conj(Z[m/2]), so this is its own inverse.
    if ( m > 1 && ( m & 1 ) == 0 )
    {
        const int k = m / 2;
        x[k] = re[k];
        zi[k] = -im[k];
    }
    m_half.transform( x, zi, true );
    // z[k] -> x[2k], x[2k+1], backward: the write at 2k is past every x[k'] with k' < k still to
    // be read, and the imaginary parts come from the scratch, which nothing here writes.
    for ( int k = m - 1; k >= 0; --k )
    {
        const float a = x[k];
        x[2 * k] = a;
        x[2 * k + 1] = zi[k];
    }
}

// ---------------------------------------------------------------- Nebula

void Nebula::prepare( double, uint64_t seed )
{
    m_window.resize( N );
    for ( int i = 0; i < N; ++i ) m_window[static_cast<size_t>( i )] = 0.5f - 0.5f * std::cos( TwoPi * i / N );
    Rng r;
    r.seed( seed );
    for ( auto &c : m_ch )
    {
        c.m_in.assign( N, 0.0f );
        c.m_out.assign( 2 * N, 0.0f );
        c.m_mag.assign( N / 2 + 1, 0.0f );
        c.m_re.assign( N, 0.0f );
        c.m_im.assign( N, 0.0f );
        c.m_inPos = 0;
        c.m_outPos = 0;
        c.m_rng.seed( r.fork() );
    }
    m_hopCounter = 0;
}

void Nebula::set( float smear )
{
    const float s = clampv( smear, 0.0f, 1.0f );
    // The floor matters. Without it, Smear = 1 gives alpha = 0 exactly, the magnitude smoother
    // never updates, every bin stays at the zero it started from, and the Nebula falls silent --
    // so the top of the knob was not "the smoothest setting" but "off", with nothing to say so.
    // At 2e-4 and this hop rate the time constant is minutes, which is what the top of a smear
    // control should be. Found by rendering the preset bank: one preset out of 257 sat exactly
    // on that corner and measured as the carrier, untouched.
    m_alpha = std::max( ( 1.0f - s ) * ( 1.0f - s ), 2.0e-4f );
}

void Nebula::frame( Channel &c )
{
    const int inMask = N - 1, outMask = 2 * N - 1;
    // The windowed frame, written into the buffer the spectrum will take its place in: the real
    // transform reads and writes the same array (13.09.2026 -- this was a complex transform on a
    // signal whose imaginary half was a row of zeros, and the row had to be written first).
    for ( int i = 0; i < N; ++i )
        c.m_re[static_cast<size_t>( i )] =
            c.m_in[static_cast<size_t>( ( c.m_inPos - N + i ) & inMask )] * m_window[static_cast<size_t>( i )];
    m_fft.forward( c.m_re.data(), c.m_re.data(), c.m_im.data() );
    for ( int k = 0; k <= N / 2; ++k )
    {
        const float m = std::sqrt( c.m_re[static_cast<size_t>( k )] * c.m_re[static_cast<size_t>( k )] +
                                   c.m_im[static_cast<size_t>( k )] * c.m_im[static_cast<size_t>( k )] );
        c.m_mag[static_cast<size_t>( k )] += m_alpha * ( m - c.m_mag[static_cast<size_t>( k )] );
    }
    for ( int k = 0; k <= N / 2; ++k )
    {
        const float ph = c.m_rng.uniform();
        const float m = c.m_mag[static_cast<size_t>( k )];
        const float r = m * sin01( ph + 0.25 >= 1.0 ? ph - 0.75 : ph + 0.25 ), q = m * sin01( ph );
        c.m_re[static_cast<size_t>( k )] = r;
        c.m_im[static_cast<size_t>( k )] = q;
        if ( k > 0 && k < N / 2 )
        {
            c.m_re[static_cast<size_t>( N - k )] = r;
            c.m_im[static_cast<size_t>( N - k )] = -q;
        }
    }
    c.m_im[0] = 0.0f;
    c.m_im[N / 2] = 0.0f;
    // The spectrum above is built conjugate symmetric on purpose, so the way back is the real
    // inverse; the mirror it writes above N/2 is read by nothing and could be left out.
    m_fft.inverse( c.m_re.data(), c.m_im.data(), c.m_re.data() );
    for ( int i = 0; i < N; ++i )
        c.m_out[static_cast<size_t>( ( c.m_outPos + i ) & outMask )] +=
            c.m_re[static_cast<size_t>( i )] * m_window[static_cast<size_t>( i )] *
            1.3f;   // measured: unity level for random-phase resynthesis
}

void Nebula::process( const float *inL, const float *inR, float *wetL, float *wetR, int n )
{
    const int    inMask = N - 1, outMask = 2 * N - 1;
    const float *ins[2] = { inL, inR };
    float       *wets[2] = { wetL, wetR };
    for ( int i = 0; i < n; ++i )
    {
        for ( int c = 0; c < 2; ++c )
        {
            Channel &ch = m_ch[c];
            ch.m_in[static_cast<size_t>( ch.m_inPos & inMask )] = ins[c][i];
            ++ch.m_inPos;
            const size_t o = static_cast<size_t>( ch.m_outPos & outMask );
            wets[c][i] = ch.m_out[o];
            ch.m_out[o] = 0.0f;
            ++ch.m_outPos;
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
