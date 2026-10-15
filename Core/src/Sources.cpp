/**
 * @file Sources.cpp
 * @brief The source slots: what a voice plays beside (or instead of) its own strand bank.
 *
 * A voice owns Slots SourceSlot objects (Sources.h), and every one of them is a small synthesiser
 * of its own, chosen by SlotParams::type: a wavetable read as a spectrum into the phasor bank
 * (Harmonic), the same bank fed by the voice's additive formula (Additive), single cycles read as
 * samples (Wavetable), two-operator FM, a granular player over a recording (Texture), a Paulstretch
 * of it (Stretch), its band model played back at any pitch (Spectral), the clip as it is (Clip),
 * eleven colours of noise, and a bowed string (Bow). This file holds the far types -- the planes
 * that are made to be sustained and to be sent into the reverb -- together with the name tables the
 * editor shows, the five built-in wavetables, the Wavetable's morphing and analysis, the Texture's
 * level measurement and band model, and SourceSlot's prepare(), noteOn() and the render()
 * dispatcher that hands each block to the type's own routine. The near sources (Flute, Murmur,
 * Bowl, Ice, Drops, Clip) and the signals (Whistler to Dial) live in SourcesNear.cpp; the state
 * they share with the Bow -- the two delay lines -- is allocated here.
 *
 * render() is called from Voice::render on the audio thread once per control block of at most
 * ControlBlock samples and ADDS into its two output buffers; prepare() runs on the message thread
 * and does every allocation (the stretch buffers, the shared transforms, the string, the built-in
 * tables), so the audio thread never allocates. Every type is calibrated so that Level means the
 * same loudness: a Wavetable slot at Level 1 renders at about -22 dBFS, and the others -- the noise
 * colours one at a time, the string, the grains -- were measured against it and corrected, which
 * the selftest holds them to. Most of the comments below are measurements: what was heard, what
 * was measured, and why the number that stands there stands there.
 * @author Rene Weller
 */
#include "ambient/Sources.h"
#include "ambient/Simd.h"
#include "ambient/Voice.h"     // ControlBlock
#include "ambient/Cosmos.h"    // Fft for wavetable analysis
#include <cmath>
#include <cstring>
#include <algorithm>
#include <type_traits>   // the interpolation is a tag, not a branch in the sample loop
#include <memory>      // the shared transforms of the Stretch type

namespace ambient {

const char* const SourceTypeNames[NumSourceTypes] = { "Off", "Harmonic", "FM", "Texture", "Noise", "Additive", "Stretch", "Bow", "Spectral", "Wavetable",
                                                        "Flute", "Murmur", "Bowl", "Ice", "Drops", "Clip",
                                                        "Whistler", "Shaker", "Chime", "Geiger", "Tube", "Krell", "Beacon", "Morse", "Dial" };
const char* const NoiseKindNames[NumNoiseKinds] = {
    "White", "Pink", "Brown", "Blue", "Violet", "Grey", "Band", "Wind", "Crackle", "Digital", "Cicada",
};
const char* const SlotRoleNames[NumSlotRoles] = { "All", "Lowest", "Inner", "Highest" };
const char* const TableNames[NumTables] = { "Classic", "Organ", "Vocal", "Glass", "Metal", "User" };
const char* const SlotRatioNames[NumSlotRatios] = { "1/1", "9/8", "6/5", "5/4", "4/3", "3/2", "8/5", "5/3", "7/4", "2/1" };
const double      SlotRatios[NumSlotRatios] = { 1.0, 9.0 / 8.0, 6.0 / 5.0, 5.0 / 4.0, 4.0 / 3.0, 3.0 / 2.0, 8.0 / 5.0, 5.0 / 3.0, 7.0 / 4.0, 2.0 };
static constexpr float Sqrt2    = 1.41421356237f;   ///< sqrt(2): a copy's full weight on one channel in the equal-power pan of setBankPitch
static constexpr float Sqrt2Inv = 0.70710678119f;   ///< 1 / sqrt(2): the level of a spread unison group, whose Pan is composed into the copies instead
const char* const FollowNames[2] = { "Free", "Note" };

// ---------------------------------------------------------------- wavetables

namespace {

/**
 * @brief Scales one frame of partial amplitudes to unit energy, so every frame of a table weighs the same.
 *
 * The sum of the squares comes to one afterwards; a silent frame is left as it is rather than
 * divided by nothing. Called for every frame a built-in table is written with and for every frame
 * Wavetable::analyse() cuts from a file.
 * @param[in,out] a  the frame's TablePartials amplitudes, rewritten in place
 */
void normaliseFrame(float* a)
{
    float sq = 0.0f;
    for (int h = 0; h < TablePartials; ++h) sq += a[h] * a[h];
    if (sq <= 0.0f) return;
    const float s = 1.0f / std::sqrt(sq);
    for (int h = 0; h < TablePartials; ++h) a[h] *= s;
}

/**
 * @brief The five built-in wavetables (Classic, Organ, Vocal, Glass, Metal), written from formulas.
 *
 * Each is a handful of frames of TablePartials partial amplitudes, every frame normalised to unit
 * energy, in the order TableNames lists them; the sixth name, User, is the table a file provides
 * and is not in here. Built once by builtins() and shared by every slot of every voice.
 */
struct BuiltinTables {
    Wavetable m_t[NumTables - 1];   ///< the tables in the order of TableNames, the User slot excluded

    /** @brief Writes every frame of the five tables and normalises each of them to unit energy. */
    BuiltinTables()
    {
        auto frame = [&](Wavetable& w, int fi) -> float* { w.m_frames = std::max(w.m_frames, fi + 1); return w.m_amp[fi]; };
        // Classic: sine -> triangle -> saw -> square -> narrow pulse.
        {
            Wavetable& w = m_t[0];
            float* f0 = frame(w, 0); f0[0] = 1.0f;
            float* f1 = frame(w, 1); for (int h = 1; h <= 32; h += 2) f1[h - 1] = 1.0f / static_cast<float>(h * h);
            float* f2 = frame(w, 2); for (int h = 1; h <= 32; ++h) f2[h - 1] = 1.0f / static_cast<float>(h);
            float* f3 = frame(w, 3); for (int h = 1; h <= 32; h += 2) f3[h - 1] = 1.0f / static_cast<float>(h);
            float* f4 = frame(w, 4); for (int h = 1; h <= 32; ++h) f4[h - 1] = std::fabs(std::sin(Pi * 0.2f * static_cast<float>(h))) / static_cast<float>(h);
        }
        // Organ: 16'+8' -> flutes 8'4'2' -> full drawbars -> mixture with fifths.
        {
            Wavetable& w = m_t[1];
            float* f0 = frame(w, 0); f0[0] = 1.0f; f0[1] = 0.8f;
            float* f1 = frame(w, 1); f1[0] = 1.0f; f1[1] = 0.7f; f1[3] = 0.5f; f1[7] = 0.3f;
            float* f2 = frame(w, 2); { const int hs[] = { 1, 2, 3, 4, 5, 6, 8 }; const float g[] = { 1.0f, 0.8f, 0.6f, 0.5f, 0.4f, 0.35f, 0.3f }; for (int i = 0; i < 7; ++i) f2[hs[i] - 1] = g[i]; }
            float* f3 = frame(w, 3); { const int hs[] = { 1, 2, 3, 4, 6, 8, 10, 12, 16, 24 }; for (int i = 0; i < 10; ++i) f3[hs[i] - 1] = 1.0f / std::sqrt(static_cast<float>(i + 1)); }
        }
        // Vocal: formant envelopes a e i o u evaluated at a 130 Hz fundamental, with a 1/h tilt.
        {
            Wavetable& w = m_t[2];
            const float F[5][3] = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 270, 2300, 3000 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
            const float B[3] = { 90.0f, 110.0f, 160.0f };
            const float G[3] = { 1.0f, 0.5f, 0.25f };
            for (int v = 0; v < 5; ++v) {
                float* f = frame(w, v);
                for (int h = 1; h <= 32; ++h) {
                    const float fh = 130.0f * static_cast<float>(h);
                    float a = 0.02f;
                    for (int k = 0; k < 3; ++k) { const float x = (fh - F[v][k]) / B[k]; a += G[k] * std::exp(-x * x); }
                    f[h - 1] = a / std::sqrt(static_cast<float>(h));
                }
            }
        }
        // Glass: sparse, high, thinning out toward the last frame.
        {
            Wavetable& w = m_t[3];
            float* f0 = frame(w, 0); f0[0] = 1.0f;
            float* f1 = frame(w, 1); f1[0] = 1.0f; f1[2] = 0.6f; f1[7] = 0.3f;
            float* f2 = frame(w, 2); f2[0] = 1.0f; f2[3] = 0.5f; f2[8] = 0.35f; f2[15] = 0.25f;
            float* f3 = frame(w, 3); f3[0] = 1.0f; f3[2] = 0.6f; f3[6] = 0.45f; f3[11] = 0.35f; f3[18] = 0.3f; f3[26] = 0.25f;
        }
        // Metal: dense, odd-heavy, comb-dipped.
        {
            Wavetable& w = m_t[4];
            float* f0 = frame(w, 0); for (int h = 1; h <= 32; h += 2) f0[h - 1] = 1.0f / std::sqrt(static_cast<float>(h));
            float* f1 = frame(w, 1); for (int h = 1; h <= 32; ++h) f1[h - 1] = std::fabs(std::cos(0.7f * static_cast<float>(h))) / std::sqrt(static_cast<float>(h));
            float* f2 = frame(w, 2); for (int h = 1; h <= 32; ++h) f2[h - 1] = ((h % 3) == 0 ? 1.0f : 0.3f) / std::sqrt(static_cast<float>(h));
            float* f3 = frame(w, 3); for (int h = 1; h <= 32; ++h) f3[h - 1] = (0.5f + 0.5f * std::sin(2.3f * static_cast<float>(h) + 1.0f)) / std::pow(static_cast<float>(h), 0.35f);
        }
        for (auto& w : m_t) for (int fi = 0; fi < w.m_frames; ++fi) normaliseFrame(w.m_amp[fi]);
    }
};

/**
 * @brief The built-in tables, constructed on the first call and read-only from then on.
 * @return the one BuiltinTables instance
 */
const BuiltinTables& builtins() { static const BuiltinTables b; return b; }

} // namespace

const Wavetable& builtinTable(int index)
{
    return builtins().m_t[clampv(index, 0, NumTables - 2)];
}

bool loopFromName(const char* fileName)
{
    if (fileName == nullptr) return false;
    const char* base = fileName;
    for (const char* p = fileName; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    // Lower-case copy without the extension, then look for the token.
    char buf[256];
    int n = 0;
    for (const char* p = base; *p && n < 255; ++p) buf[n++] = static_cast<char>((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
    buf[n] = 0;
    for (int i = n - 1; i > 0; --i) if (buf[i] == '.') { buf[i] = 0; break; }
    return std::strstr(buf, "_loop") != nullptr || std::strstr(buf, "-loop") != nullptr;
}

double baseHzFromName(const char* fileName)
{
    if (fileName == nullptr) return 0.0;
    // Strip directories and the extension.
    const char* base = fileName;
    for (const char* p = fileName; *p; ++p) if (*p == '/' || *p == '\\') base = p + 1;
    int len = static_cast<int>(std::strlen(base));
    for (int i = len - 1; i > 0; --i) if (base[i] == '.') { len = i; break; }
    // The last token after '_', '-' or ' ': letter, optional #/b, octave digit(s).
    int start = len;
    while (start > 0 && base[start - 1] != '_' && base[start - 1] != '-' && base[start - 1] != ' ') --start;
    const char* t = base + start;
    const int tlen = len - start;
    if (tlen < 2 || tlen > 4) return 0.0;
    int pc = -1;
    switch (t[0]) { case 'C': pc = 0; break; case 'D': pc = 2; break; case 'E': pc = 4; break; case 'F': pc = 5; break;
                    case 'G': pc = 7; break; case 'A': pc = 9; break; case 'B': pc = 11; break; default: return 0.0; }
    int i = 1;
    if (t[i] == '#') { pc += 1; ++i; }
    else if (t[i] == 'b') { pc -= 1; ++i; }
    if (i >= tlen) return 0.0;
    int octave = 0; bool neg = false;
    if (t[i] == '-') { neg = true; ++i; }
    if (i >= tlen) return 0.0;
    for (; i < tlen; ++i) { if (t[i] < '0' || t[i] > '9') return 0.0; octave = octave * 10 + (t[i] - '0'); }
    if (neg) octave = -octave;
    const int midi = (octave + 1) * 12 + pc;
    if (midi < 0 || midi > 127) return 0.0;
    return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

void Wavetable::spectrumAt(float pos, float* out, float transport) const
{
    if (m_frames <= 0) { std::memset(out, 0, sizeof(float) * TablePartials); return; }
    if (m_frames == 1) { std::memcpy(out, m_amp[0], sizeof(float) * TablePartials); return; }
    const float x = clampv(pos, 0.0f, 1.0f) * static_cast<float>(m_frames - 1);
    const int i = std::min(static_cast<int>(x), m_frames - 2);
    const float f = x - static_cast<float>(i);
    for (int h = 0; h < TablePartials; ++h) out[h] = m_amp[i][h] + f * (m_amp[i + 1][h] - m_amp[i][h]);
    if (transport <= 0.0f || f <= 0.0f || f >= 1.0f) return;
    // Optimal transport along the partial axis. Mass is what a partial's amplitude is read as
    // (negative entries count as nothing); the two frames' distributions are normalised, walked
    // together by cumulative mass, and every slice put down at (1 - f) * from + f * to, split
    // between the two nearest partials when that lands between them. What comes out carries the
    // blended total, and the plain blend is faded into it by the amount.
    const float* A = m_amp[i];
    const float* B = m_amp[i + 1];
    float mA = 0.0f, mB = 0.0f;
    for (int h = 0; h < TablePartials; ++h) { mA += std::max(A[h], 0.0f); mB += std::max(B[h], 0.0f); }
    if (mA <= 1.0e-9f || mB <= 1.0e-9f) return;
    float ot[TablePartials] = {};
    const float total = (1.0f - f) * mA + f * mB;
    int ia = 0, ib = 0;
    float ra = std::max(A[0], 0.0f) / mA, rb = std::max(B[0], 0.0f) / mB;
    for (int guard = 0; guard < 4 * TablePartials && ia < TablePartials && ib < TablePartials; ++guard) {
        if (ra <= 1.0e-7f) { if (++ia < TablePartials) ra = std::max(A[ia], 0.0f) / mA; continue; }
        if (rb <= 1.0e-7f) { if (++ib < TablePartials) rb = std::max(B[ib], 0.0f) / mB; continue; }
        const float m = std::min(ra, rb);
        const float xPos = (1.0f - f) * static_cast<float>(ia) + f * static_cast<float>(ib);
        const int x0 = std::min(static_cast<int>(xPos), TablePartials - 1);
        const float fr = xPos - static_cast<float>(x0);
        ot[x0] += m * total * (1.0f - fr);
        if (x0 + 1 < TablePartials) ot[x0 + 1] += m * total * fr;
        ra -= m; rb -= m;
    }
    const float t = clampv(transport, 0.0f, 1.0f);
    for (int h = 0; h < TablePartials; ++h) out[h] += t * (ot[h] - out[h]);
}

bool Wavetable::analyse(const float* mono, int n, int frameLen)
{
    m_frames = 0;
    if (mono == nullptr || frameLen < 8 || n < frameLen) return false;
    const int total = n / frameLen;
    const int keep = std::min(total, TableFrames);
    // A power of two goes through the FFT, as it always has. Any other length -- an Adventure Kid
    // single cycle is 600 samples -- is summed directly, bin by bin: thirty-two bins of one short
    // cycle need no transform. The FFT itself must never see such a length: it builds its bit
    // reversal for the next power of two above it, and a frame of 300 samples would make it swap
    // entries two hundred past the end of its own buffers.
    const bool pow2 = (frameLen & (frameLen - 1)) == 0;
    std::unique_ptr<Fft> fft;
    std::vector<float> re, im;
    if (pow2) {
        fft = std::make_unique<Fft>(frameLen);
        re.assign(static_cast<size_t>(frameLen), 0.0f);
        im.assign(static_cast<size_t>(frameLen), 0.0f);
    }
    for (int k = 0; k < keep; ++k) {
        const int src = (keep == total) ? k : static_cast<int>(static_cast<long long>(k) * (total - 1) / std::max(keep - 1, 1));
        const float* x = mono + static_cast<size_t>(src) * static_cast<size_t>(frameLen);
        if (pow2) {
            std::memcpy(re.data(), x, sizeof(float) * static_cast<size_t>(frameLen));
            std::fill(im.begin(), im.end(), 0.0f);
            fft->transform(re.data(), im.data(), false);
            for (int h = 1; h <= TablePartials; ++h)
                m_amp[k][h - 1] = h < frameLen / 2 ? std::sqrt(re[static_cast<size_t>(h)] * re[static_cast<size_t>(h)] + im[static_cast<size_t>(h)] * im[static_cast<size_t>(h)]) : 0.0f;
        } else {
            for (int h = 1; h <= TablePartials; ++h) {
                if (2 * h >= frameLen) { m_amp[k][h - 1] = 0.0f; continue; }
                const double w = 2.0 * 3.14159265358979323846 * h / frameLen;
                double sr = 0.0, si = 0.0;
                for (int i = 0; i < frameLen; ++i) { sr += x[i] * std::cos(w * i); si += x[i] * std::sin(w * i); }
                m_amp[k][h - 1] = static_cast<float>(std::sqrt(sr * sr + si * si));
            }
        }
        normaliseFrame(m_amp[k]);
    }
    m_frames = keep;
    return true;
}

// ---------------------------------------------------------------- slot

namespace {
/**
 * @brief The fractional part of @p x: a phase brought back into [0, 1).
 * @param[in] x  a phase in cycles, of any magnitude
 * @return x minus floor(x)
 */
inline double wrap01(double x) { return x - std::floor(x); }
}

void SourceSlot::prepare(double sampleRate, uint64_t seed)
{
    m_sr = sampleRate;
    m_rng.seed(seed);
    m_posDrift.init(m_rng);
    m_idxDrift.init(m_rng);
    for (int h = 0; h < Bank; ++h) { phasorFrom(m_rng.uniform(), m_pc[h], m_ps[h]); m_rc[h] = 1.0f; m_rs[h] = 0.0f; m_amp[h] = m_ampStep[h] = 0.0f; m_wL[h] = m_wR[h] = 1.0f; }
    {   // the shimmer drifters seed from a side stream, so the slot's own stream is what it always was
        Rng aux; aux.seed(seed ^ 0xD1B54A32D192ED03ull);
        for (auto& d : m_shim) d.init(aux);
        m_pitchDrift.init(aux);
    }
    m_active = 0;
    m_cTilt = -1.0f; m_cOdd = -9.0f; m_cPartials = -1;
    m_phC = m_phM = 0.0;
    for (auto& g : m_grains) g.m_on = false;
    m_live = 0;
    m_spawnIn = 0.0;
    m_gL = m_gR = 0.0f;
    // Stretch: the buffers, sized once for the longest window; the shared transforms, built here
    // on the message thread so render() only ever reads them.
    m_st.m_out.assign(static_cast<size_t>(2 * StretchMaxN), 0.0f);
    m_st.m_re.assign(static_cast<size_t>(StretchMaxN), 0.0f);
    m_st.m_im.assign(static_cast<size_t>(StretchMaxN), 0.0f);
    m_st.m_win.assign(static_cast<size_t>(StretchMaxN), 0.0f);
    m_st.m_n = 0; m_st.m_outPos = 0; m_st.m_hopLeft = 0; m_st.m_advance = 0.0;
    for (int n = StretchMinN; n <= StretchMaxN; n <<= 1) (void)stretchFft(n);
    // Bow: the string itself, allocated here rather than carried in the object.
    m_bowNut.assign(static_cast<size_t>(BowMax), 0.0f);
    m_bowBridge.assign(static_cast<size_t>(BowMax), 0.0f);
    m_bowW = 0; m_bowLp = 0.0f; m_bowReady = false;
    m_fluteReady = false; m_rubReady = false; m_murReady = false;
    for (auto& d : m_drops) d.m_on = false;
    m_dropNext = 0.0;
    m_specAdvance = 0.0;
    // Wavetable: the start phases come from a stream of their own, and the built-in tables are
    // made here, on this thread, rather than by the first block that asks for one.
    m_cyRng.seed(seed ^ 0x6A09E667F3BCC909ull);
    for (double& ph : m_cyPhase) ph = 0.0;
    m_cyPrimed = false;
    (void)builtinCycleTable(0);
}

const RealFft& SourceSlot::stretchFft(int n)
{
    // One transform per size, shared by every slot of every voice: the tables are read-only once
    // built, and building them in prepare() keeps the allocation off the audio thread.
    static std::vector<std::unique_ptr<RealFft>> table;
    static std::vector<int> sizes;
    for (size_t i = 0; i < sizes.size(); ++i) if (sizes[i] == n) return *table[i];
    table.push_back(std::make_unique<RealFft>(n));
    sizes.push_back(n);
    return *table.back();
}

void SourceSlot::noteOn(bool fresh)
{
    if (!fresh) return;
    for (int h = 0; h < Bank; ++h) { phasorFrom(m_rng.uniform(), m_pc[h], m_ps[h]); m_amp[h] = m_ampStep[h] = 0.0f; }
    m_active = 0;
    m_phC = m_rng.uniform(); m_phM = m_rng.uniform();
    m_hpX = m_hpY = 0.0f;
    for (auto& g : m_grains) g.m_on = false;
    m_spawnIn = 0.0;
    m_st.m_advance = 0.0;   // a fresh note reads from Position again
    // A fresh note blows the pipe from rest, puts the stick to a still bowl, starts a new phrase,
    // and plays the clip again from its Position.
    m_fluteReady = false; m_rubReady = false; m_murReady = false;
    for (auto& d : m_drops) d.m_on = false;
    m_clipPos = -1.0; m_clipDone = false;
    for (double& ph : m_cyPhase) ph = m_cyRng.uniform();
    m_cyPrimed = false;
    m_specAdvance = 0.0;
    for (int b = 0; b < TablePartials; ++b) {
        m_specA[b] = m_specAStep[b] = m_specN[b] = m_specNStep[b] = 0.0f;
        m_specBp[b] = m_specLp[b] = 0.0f;
    }
}

void SourceSlot::render(float* outL, float* outR, int n, double noteHz, const SlotParams& p,
                        const Wavetable* table, const Texture* texture, float driftRate,
                        const CycleTable* cycles)
{
    if (p.m_type == SourceTypeE::OFF || n <= 0) { m_lastType = p.m_type; return; }   // (Source 1 in Additive mode also lands here with n = 0)
    n = std::min(n, ControlBlock);
    const float dt = static_cast<float>(n / m_sr);
    if (p.m_type != m_lastType) {   // switching type: start clean, no leftover phasor amplitudes or grains
        for (int h = 0; h < Bank; ++h) { m_amp[h] = m_ampStep[h] = 0.0f; }
        m_active = 0;
        for (auto& g : m_grains) g.m_on = false;
        if (!m_st.m_out.empty()) std::fill(m_st.m_out.begin(), m_st.m_out.end(), 0.0f);
        m_st.m_n = 0; m_st.m_hopLeft = 0;
        m_cyPrimed = false;
        // The near sources start clean too: the string's lines are the pipe's, and a bowl that
        // was ringing as a bow is not a bowl.
        m_fluteReady = false; m_rubReady = false; m_murReady = false;
        for (auto& d : m_drops) d.m_on = false;
        m_clipPos = -1.0; m_clipDone = false;
        m_sigReady = false;   // the signals start their clocks again
        m_lastType = p.m_type;
    }
    double hz = noteHz * SlotRatios[clampv(p.m_ratio, 0, NumSlotRatios - 1)] * std::pow(2.0, clampv(p.m_octave, -2, 2));
    // Slow independent pitch drift: each source on its own curve, so three sources on pure ratios
    // beat like an ensemble in a room whose temperature moves, never in lockstep.
    if (p.m_drift > 0.0f) hz *= std::pow(2.0, static_cast<double>(p.m_drift * m_pitchDrift.update(dt, driftRate > 0.0f ? driftRate : 0.05f, m_rng)) / 1200.0);

    // Unison in a bank type: the copies are spread across the field, so the slot produces two
    // channels of its own instead of one signal placed by Pan -- and a pair of channels is not
    // placed by multiplying each of them with its own ear's gain. That does not move the group,
    // it filters it: a copy standing where the pan came from is attenuated by how far it stands
    // there, and at the end of the knob it is gone. So for a spread group the Pan is composed
    // into the copies' own places instead (setBankPitch and renderCycles do it), exactly as a
    // voice composes its strand fan into its own centre, and what is left here is the level.
    const int copies = clampv(p.m_unison, 1, SlotUnison);
    const bool wide = copies > 1 && (p.m_type == SourceTypeE::HARMONIC || p.m_type == SourceTypeE::WAVETABLE);
    // Level and pan ramp across the block (equal power).
    const float angle = (clampv(p.m_pan, -1.0f, 1.0f) + 1.0f) * 0.25f * Pi;
    const float tL = wide ? p.m_level * Sqrt2Inv : p.m_level * std::cos(angle);
    const float tR = wide ? p.m_level * Sqrt2Inv : p.m_level * std::sin(angle);
    const float sL = (tL - m_gL) / static_cast<float>(n), sR = (tR - m_gR) / static_cast<float>(n);

    if (p.m_type == SourceTypeE::NOISE) {
        float bufL[ControlBlock];
        std::memset(bufL, 0, sizeof(float) * static_cast<size_t>(n));
        renderNoise(bufL, n, hz, p, dt);        // left into bufL, right into m_scratch
        for (int i = 0; i < n; ++i) { outL[i] += bufL[i]; outR[i] += m_scratch[i]; }
        m_gL = tL; m_gR = tR;
        return;
    }
    if (p.m_type == SourceTypeE::TEXTURE || p.m_type == SourceTypeE::CLIP) {
        // Grains carry their own level/pan (fixed at spawn), written straight to L/R; the clip
        // is written the same way, with its level and pan applied inside.
        float bufL[ControlBlock];
        std::memset(bufL, 0, sizeof(float) * static_cast<size_t>(n));
        if (p.m_type == SourceTypeE::TEXTURE) renderTexture(bufL, n, hz, hz / std::max(noteHz, 1.0), p, texture, dt);   // left into bufL, right into m_scratch
        else renderClip(bufL, n, hz, hz / std::max(noteHz, 1.0), p, texture, dt);
        for (int i = 0; i < n; ++i) { outL[i] += bufL[i]; outR[i] += m_scratch[i]; }
        m_gL = tL; m_gR = tR;
        return;
    }

    std::memset(m_scratch, 0, sizeof(float) * static_cast<size_t>(n));
    if (wide) std::memset(m_scratchR, 0, sizeof(float) * static_cast<size_t>(n));
    if (p.m_type == SourceTypeE::HARMONIC) renderWavetable(m_scratch, n, hz, p, table, dt, wide ? m_scratchR : nullptr);
    else if (p.m_type == SourceTypeE::WAVETABLE) renderCycles(m_scratch, n, hz, p, cycles, dt, wide ? m_scratchR : nullptr);
    else if (p.m_type == SourceTypeE::ADDITIVE) renderAdditive(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::STRETCH) renderStretch(m_scratch, n, hz, hz / std::max(noteHz, 1.0), p, texture, dt);
    else if (p.m_type == SourceTypeE::BOW) renderBow(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::FLUTE) renderFlute(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::MURMUR) renderMurmur(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::BOWL) renderRub(m_scratch, n, hz, p, dt, false);
    else if (p.m_type == SourceTypeE::ICE) renderRub(m_scratch, n, hz, p, dt, true);
    else if (p.m_type == SourceTypeE::DROPS) renderDrops(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::WHISTLER) renderWhistler(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::SHAKER) renderShaker(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::CHIME) renderChime(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::GEIGER) renderGeiger(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::TUBE) renderTube(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::KRELL) renderKrell(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::BEACON) renderBeacon(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::MORSE) renderMorse(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::DIAL) renderDial(m_scratch, n, hz, p, dt);
    else if (p.m_type == SourceTypeE::SPECTRAL)
        renderSpectral(m_scratch, n, p.m_follow ? hz / std::max(1.0, texture != nullptr ? texture->m_baseHz : 1.0)
                                             : hz / std::max(noteHz, 1.0), p, texture, dt);
    else renderFm(m_scratch, n, hz, p, dt);
    for (int i = 0; i < n; ++i) {
        m_gL += sL; m_gR += sR;
        outL[i] += m_scratch[i] * m_gL;
        outR[i] += (wide ? m_scratchR[i] : m_scratch[i]) * m_gR;
    }
    m_gL = tL; m_gR = tR;
}

void SourceSlot::renderWavetable(float* out, int n, double hz, const SlotParams& p, const Wavetable* table, float dt, float* outR)
{
    // Control: spectrum at the (wandering) position, targets normalised, rotations refreshed.
    const float wander = m_posDrift.update(dt, 0.02f, m_rng) * 0.5f * p.m_positionDrift;
    m_dispPos = clampv(p.m_position + wander, 0.0f, 1.0f);        // what the picture should show
    float spec[TablePartials];
    if (table != nullptr) table->spectrumAt(p.m_position + wander, spec, p.m_transport);
    else std::memset(spec, 0, sizeof(spec));
    // Root: give the fundamental a floor.
    //
    // A wavetable here is a spectrum, and a spectrum cut from a recording keeps whatever that
    // recording had -- which for a bell, a bowed harmonic or overtone singing is very little at
    // the bottom. Measured over 1200 frames of the library: the energy sits at partial 5.8 on
    // average, the fundamental holds 24 % of it, and a QUARTER of all frames have less than a
    // tenth of their energy there. Played high that is the sound of the material; played low it
    // is why "the wavetable sources are thin and only work up top" -- what one hears is the
    // sixth partial of a note whose own pitch is not in the sound at all.
    //
    // At 1 the fundamental is brought up to half the energy of the frame, and renderBank's
    // constant-power rule pulls the rest down to make room, so this shifts weight rather than
    // adding level. It is a knob and not a repair of the tables: which of the two readings is
    // right belongs to the material, and at 0 -- what every preset has -- the frame is exactly
    // what was analysed.
    if (p.m_root > 0.0f) {
        double e = 0.0;
        for (int h = 0; h < TablePartials; ++h) e += static_cast<double>(spec[h]) * spec[h];
        const double f0 = static_cast<double>(spec[0]) * spec[0];
        const double want = 0.5 * clampv(static_cast<double>(p.m_root), 0.0, 1.0);
        if (e > 0.0 && f0 < want * e) {
            const double rest = e - f0;
            spec[0] = static_cast<float>(std::sqrt(want * rest / std::max(1.0 - want, 1e-6)));
        }
    }
    const int H = setBankPitch(hz, p, TablePartials);
    renderBank(spec, H, n, out, p.m_unison, outR);
}

/**
 * @brief Where each copy of the bank sits: its pitch, and its place in the field.
 *
 * The copies are spread symmetrically about the written pitch -- with two they sit at plus and
 * minus half the detune, with three the middle one is exactly in tune -- and across the field the
 * same way, so the sound stays centred however many there are. At one copy the ratio is exactly
 * 1.0 and the arithmetic below is `hz * h`, which is what stood here before unison existed: every
 * preset in the library renders to the bit.
 */

int SourceSlot::setBankPitch(double hz, const SlotParams& p, int partials)
{
    const int copies = clampv(p.m_unison, 1, SlotUnison);
    const double nyq = 0.45 * m_sr;
    const float width = clampv(p.m_uniWidth, 0.0f, 1.0f);
    int H = 0;
    for (int c = 0; c < copies; ++c) {
        // -1 .. +1 across the copies, and 0 when there is only one of them.
        const double place = copies > 1 ? (2.0 * c / (copies - 1) - 1.0) : 0.0;
        const double ratio = copies > 1 ? std::pow(2.0, place * 0.5 * static_cast<double>(p.m_uniDetune) / 1200.0) : 1.0;
        // Equal power about the centre, scaled so that a copy standing in the middle arrives at
        // full strength in both channels rather than at 0.707 of it. What the caller adds is the
        // level alone; the slot's Pan is composed into the copy's own place here (and stopped at
        // the ear, as a voice stops its strands), because a group that is placed one channel at a
        // time is not moved by its Pan but thinned by it. Without the factor the level would drop
        // three decibels when unison is turned on instead of thickening. Measured.
        const float panC = clampv((copies > 1 ? clampv(p.m_pan, -1.0f, 1.0f) : 0.0f)
                                  + static_cast<float>(place) * width, -1.0f, 1.0f);
        const float angle = (panC + 1.0f) * 0.25f * Pi;
        const float wl = std::cos(angle) * Sqrt2, wr = std::sin(angle) * Sqrt2;
        const int base = c * TablePartials;
        int fit = 0;
        for (int h = 1; h <= partials; ++h) {
            const double fh = hz * ratio * h;
            if (fh >= nyq) break;
            const int i = base + h - 1;
            phasorFrom(fh / m_sr, m_rc[i], m_rs[i]);
            const float r2 = m_pc[i] * m_pc[i] + m_ps[i] * m_ps[i];
            const float fix = 1.5f - 0.5f * r2;
            m_pc[i] *= fix; m_ps[i] *= fix;
            m_wL[i] = wl; m_wR[i] = wr;
            fit = h;
        }
        if (c == 0) H = fit;
    }
    return H;
}

/**
 * @brief `uni` copies of the same spectrum, laid end to end in the flat bank: copy c occupies entries
 *        c*TablePartials .. c*TablePartials+31 at its own slightly detuned pitch, so the amplitudes
 *        repeat every TablePartials and the same SIMD loop steps all of them.
 *
 * The energy is divided
 * among the copies -- the constant-power rule the single bank always used, applied to the whole
 * list rather than to one copy of it -- so turning unison up thickens the sound without raising
 * the level. At one copy every line below does exactly what it did before, entry for entry.
 */
void SourceSlot::renderBank(const float* spec, int H, int n, float* out, int uni, float* outR)
{
    float sumSq = 0.0f;
    for (int h = 0; h < H; ++h) sumSq += spec[h] * spec[h];
    const int copies = clampv(uni, 1, SlotUnison);
    const float scale = sumSq > 0.0f ? 0.5f / std::sqrt(sumSq * static_cast<float>(copies)) : 0.0f;
    const float invLen = 1.0f / static_cast<float>(n);
    const int span = copies * TablePartials;
    for (int h = 0; h < span; ++h) {
        const int part = h % TablePartials;
        const float tgt = (part < H) ? spec[part] * scale : 0.0f;
        m_ampStep[h] = (tgt - m_amp[h]) * invLen;
    }
    // A copy that has just been turned off fades out rather than stopping mid-cycle.
    for (int h = span; h < m_active; ++h) m_ampStep[h] = -m_amp[h] * invLen;
    int act = copies > 1 ? span : H;
    for (int h = act; h < m_active; ++h) if (std::fabs(m_amp[h]) > 1e-6f) act = h + 1;
    m_active = act;

    if (copies > 1 && outR != nullptr) {
        for (int i = 0; i < n; ++i)
            phasorBankStepStereo(m_pc, m_ps, m_rc, m_rs, m_amp, m_ampStep, m_active, m_wL, m_wR, out[i], outR[i]);
        return;
    }
    for (int i = 0; i < n; ++i)
        out[i] = phasorBankStep(m_pc, m_ps, m_rc, m_rs, m_amp, m_ampStep, m_active);
}

/**
 * @brief The Wavetable type: single cycles read as samples (CycleTable.h).
 *
 * A phase per unison copy, the
 * copies detuned and placed across the field exactly as the Harmonic type places its own
 * (setBankPitch), the energy divided among them, and Position glided across the block so a turning
 * knob never steps. The level a note reads follows its highest copy; when that changes, the block
 * reads both levels and fades from the old one to the new, so a glide across a boundary is not
 * heard as the top octave of harmonics switching off.
 */
void SourceSlot::renderCycles(float* out, int n, double hz, const SlotParams& p, const CycleTable* table, float dt, float* outR)
{
    const float wander = m_posDrift.update(dt, 0.02f, m_rng) * 0.5f * p.m_positionDrift;
    auto silence = [&] {
        std::memset(out, 0, sizeof(float) * static_cast<size_t>(n));
        if (outR != nullptr) std::memset(outR, 0, sizeof(float) * static_cast<size_t>(n));
    };
    if (table == nullptr || table->empty()) { silence(); m_cyPrimed = false; return; }

    const int copies = clampv(p.m_unison, 1, SlotUnison);
    const float width = clampv(p.m_uniWidth, 0.0f, 1.0f);
    double inc[SlotUnison] = {};
    float wl[SlotUnison] = {}, wr[SlotUnison] = {};
    double top = 0.0;
    for (int c = 0; c < copies; ++c) {
        const double place = copies > 1 ? (2.0 * c / (copies - 1) - 1.0) : 0.0;
        const double ratio = copies > 1 ? std::pow(2.0, place * 0.5 * static_cast<double>(p.m_uniDetune) / 1200.0) : 1.0;
        inc[c] = hz * ratio / m_sr;
        top = std::max(top, hz * ratio);
        // The slot's Pan is part of the copy's place, not a gain on each channel: see setBankPitch.
        const float panC = clampv((copies > 1 ? clampv(p.m_pan, -1.0f, 1.0f) : 0.0f)
                                  + static_cast<float>(place) * width, -1.0f, 1.0f);
        const float angle = (panC + 1.0f) * 0.25f * Pi;
        wl[c] = std::cos(angle) * Sqrt2;
        wr[c] = std::sin(angle) * Sqrt2;
    }
    const float pos = clampv(p.m_position + wander, 0.0f, 1.0f);
    m_dispPos = pos;                                            // what the picture should show
    const int level = cycleLevelFor(top, m_sr, m_cyPrimed ? m_cyLevel : -1);
    const int levelFrom = m_cyPrimed ? m_cyLevel : level;
    const float posFrom = m_cyPrimed ? m_cyPos : pos;
    m_cyLevel = level;
    m_cyPos = pos;
    m_cyPrimed = true;
    // Even a sine would alias: nothing to play.
    if (static_cast<double>(CycleTable::levelHarmonics(level)) * top >= 0.5 * m_sr) { silence(); return; }

    const float gain = 1.0f / std::sqrt(static_cast<float>(copies));
    const int last = table->m_frames - 1;
    const float inv = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i + 1) * inv;
        const float x = (posFrom + (pos - posFrom) * t) * static_cast<float>(last);
        const int f0 = clampv(static_cast<int>(x), 0, last);
        const int f1 = f0 < last ? f0 + 1 : last;
        const float fr = x - static_cast<float>(f0);
        float accL = 0.0f, accR = 0.0f;
        for (int c = 0; c < copies; ++c) {
            const double ph = m_cyPhase[c];
            float v = table->sample(level, f0, ph);
            if (fr > 0.0f) v += fr * (table->sample(level, f1, ph) - v);
            if (levelFrom != level) {
                float u = table->sample(levelFrom, f0, ph);
                if (fr > 0.0f) u += fr * (table->sample(levelFrom, f1, ph) - u);
                v = u + t * (v - u);
            }
            double next = ph + inc[c];
            if (next >= 1.0) next -= std::floor(next);
            m_cyPhase[c] = next;
            if (outR != nullptr) { accL += v * wl[c]; accR += v * wr[c]; }
            else accL += v;
        }
        out[i] = accL * gain;
        if (outR != nullptr) outR[i] = accR * gain;
    }
}

/**
 * @brief Additive in a slot: the voice's spectrum formula (tilt, brightness window, odd/even, inharmonic
 *        stretch, per-partial shimmer) on the slot's own bank -- one strand, so a second or third
 *        additive source costs what a wavetable slot costs.
 */
void SourceSlot::renderAdditive(float* out, int n, double hz, const SlotParams& p, float dt)
{
    const int partials = clampv(p.m_partials, 1, TablePartials);
    if (p.m_tilt != m_cTilt || p.m_oddEven != m_cOdd || partials != m_cPartials) {
        for (int h = 1; h <= partials; ++h) {
            float a = std::pow(static_cast<float>(h), -p.m_tilt);
            if (p.m_oddEven > 0.0f && (h % 2) == 0) a *= 1.0f - p.m_oddEven;
            if (p.m_oddEven < 0.0f && (h % 2) == 1 && h > 1) a *= 1.0f + p.m_oddEven;
            m_tiltCache[h - 1] = a;
        }
        m_cTilt = p.m_tilt; m_cOdd = p.m_oddEven; m_cPartials = partials;
    }
    const float hc = 1.0f + p.m_bright * p.m_bright * 31.0f;
    const float B = p.m_inharm * p.m_inharm * 0.02f;
    const double nyq = 0.45 * m_sr;
    float spec[TablePartials];
    int H = 0;
    for (int h = 1; h <= partials; ++h) {
        const double stretch = B > 0.0f ? std::sqrt(1.0 + B * static_cast<double>(h * h)) : 1.0;
        const double fh = hz * h * stretch;
        if (fh >= nyq) break;
        phasorFrom(fh / m_sr, m_rc[h - 1], m_rs[h - 1]);
        const float r2 = m_pc[h - 1] * m_pc[h - 1] + m_ps[h - 1] * m_ps[h - 1];
        const float fix = 1.5f - 0.5f * r2;
        m_pc[h - 1] *= fix; m_ps[h - 1] *= fix;
        float a = m_tiltCache[h - 1];
        if (static_cast<float>(h) > hc) {
            const float x = std::min((static_cast<float>(h) - hc) / 6.0f, 1.0f);
            a *= 0.5f * (1.0f + std::cos(Pi * x));
        }
        a *= 1.0f + 0.9f * p.m_shimmer * m_shim[h - 1].update(dt, p.m_shimmerRate, m_rng);
        spec[h - 1] = a;
        H = h;
    }
    renderBank(spec, H, n, out);
}

int SourceSlot::displayGrains(GrainInfo* out, int maxCount, int clipLen) const
{
    int n = 0;
    for (int c = 0; c < m_live && n < maxCount; ++c) {
        const Grain& g = m_grains[c];
        if (!g.m_on || g.m_len <= 0) continue;
        GrainInfo& o = out[n++];
        o.m_pos  = clipLen > 0 ? static_cast<float>(g.m_pos / static_cast<double>(clipLen)) : 0.0f;
        o.m_age  = static_cast<float>(g.m_age) / static_cast<float>(g.m_len);
        o.m_gain = g.m_gain;
        const float sum = g.m_gl + g.m_gr;
        o.m_pan  = sum > 1e-6f ? (g.m_gr - g.m_gl) / sum : 0.0f;
    }
    return n;
}

void SourceSlot::renderFm(float* out, int n, double hz, const SlotParams& p, float dt)
{
    // Two operators; the index shrinks above 3 kHz so high notes do not alias, and wanders
    // slowly by up to a factor two with Pos Drift.
    const float d = m_idxDrift.update(dt, 0.03f, m_rng);
    const float idx = p.m_fmIndex * std::min(1.0f, 3000.0f / static_cast<float>(std::max(hz, 20.0))) * std::pow(2.0f, 0.5f * d * p.m_positionDrift);
    const double incC = hz / m_sr, incM = hz * static_cast<double>(clampv(p.m_fmRatio, 0.25f, 16.0f)) / m_sr;
    const double k = idx / TwoPi;
    // DC blocker (10 Hz): with an integer ratio the phase-modulated wave carries a DC term
    // (J1(index) for ratio 1), which would sit in the reverbs and pump the clipper.
    const float hpc = 1.0f - TwoPi * 10.0f / static_cast<float>(m_sr);
    for (int i = 0; i < n; ++i) {
        const float m = sin01(m_phM);
        const float x = 0.5f * sin01(wrap01(m_phC + k * m));
        const float y = x - m_hpX + hpc * m_hpY;
        m_hpX = x; m_hpY = y;
        out[i] = y;
        m_phC += incC; if (m_phC >= 1.0) m_phC -= 1.0;
        m_phM += incM; if (m_phM >= 1.0) m_phM -= 1.0;
    }
}

void Texture::measure()
{
    // The wavetable and FM slots normalise themselves to unity, so a Texture slot has to be
    // brought to the same reference or Level means something different in each slot: a field
    // recording at -25 dBFS RMS entered the mix twenty decibels below its neighbours.
    //
    // Measured on what actually plays: the grain loop reads `lr` for a stereo clip and `mono` for
    // a mono one. For a decorrelated recording the mono sum is up to 3 dB below either channel, so
    // taking the reference from `mono` in both cases would make every wide clip enter the mix
    // louder than it used to the moment the second channel was wired up -- a library-wide level
    // change smuggled in behind a stereo fix.
    double sq = 0.0;
    size_t count = 0;
    if (stereo()) { for (float v : m_lr) sq += static_cast<double>(v) * v; count = m_lr.size(); }
    else          { for (float v : m_mono) sq += static_cast<double>(v) * v; count = m_mono.size(); }
    const double rms = count == 0 ? 0.0 : std::sqrt(sq / static_cast<double>(count));
    m_gain = rms > 1e-6 ? static_cast<float>(clampv(0.3 / rms, 0.5, 16.0)) : 1.0f;
    analyse();
}

/**
 * @brief Measure the clip into the band model.
 *
 * Runs where measure() runs -- the loader thread, once per
 * clip -- and never in render(): a six-second clip is under three hundred transforms.
 */
void Texture::analyse()
{
    m_spectral = SpectralModel();
    const int total = static_cast<int>(m_mono.size());
    const int N = 4096;
    if (total < 2 * N) return;
    // The hop is a quarter of the window, stretched if it would take more frames than the model
    // holds, so a long recording is described more coarsely rather than cut short.
    constexpr int MaxFrames = 4096;
    int hop = N / 4;
    if ((total - N) / hop + 1 > MaxFrames) hop = (total - N) / (MaxFrames - 1) + 1;
    const int frames = (total - N) / hop + 1;
    if (frames < 2) return;

    SpectralModel& m = m_spectral;
    m.m_frames = frames;
    m.m_hop = static_cast<float>(hop / m_sampleRate);
    m.m_amp.assign(static_cast<size_t>(frames) * SpectralModel::Bands, 0.0f);
    m.m_freq.assign(static_cast<size_t>(frames) * SpectralModel::Bands, 0.0f);
    m.m_tone.assign(static_cast<size_t>(frames) * SpectralModel::Bands, 0.0f);

    // The bands sit on the ear's own scale, one equal step of the ERB rate apart (Glasberg and
    // Moore 1990), so a band is about as wide as the ear's own resolution at that frequency: a
    // sixth of an octave down low, a whole octave up high. Thirty-two of them span 45 Hz to 16 kHz.
    const double top = std::min(16000.0, 0.45 * m_sampleRate);
    auto erbRate = [](double f) { return 21.4 * std::log10(0.00437 * f + 1.0); };
    auto erbInv  = [](double e) { return (std::pow(10.0, e / 21.4) - 1.0) / 0.00437; };
    const double e0 = erbRate(45.0), e1 = erbRate(top);
    double edge[SpectralModel::Bands + 1];
    for (int b = 0; b <= SpectralModel::Bands; ++b)
        edge[b] = erbInv(e0 + (e1 - e0) * b / SpectralModel::Bands);
    const double binHz = m_sampleRate / N;
    int lo[SpectralModel::Bands], hi[SpectralModel::Bands];
    for (int b = 0; b < SpectralModel::Bands; ++b) {
        lo[b] = std::max(1, static_cast<int>(edge[b] / binHz));
        hi[b] = std::max(lo[b] + 1, std::min(N / 2 - 1, static_cast<int>(edge[b + 1] / binHz)));
        m.m_centre[b] = static_cast<float>(std::sqrt(edge[b] * edge[b + 1]));
        m.m_bandQ[b] = static_cast<float>(clampv(m.m_centre[b] / std::max(1.0, edge[b + 1] - edge[b]), 0.7, 12.0));
    }

    const Fft fft(N);
    std::vector<float> re(static_cast<size_t>(N)), im(static_cast<size_t>(N));
    std::vector<float> win(static_cast<size_t>(N)), mag(static_cast<size_t>(N / 2 + 1));
    std::vector<double> logSum(static_cast<size_t>(N / 2 + 2));
    for (int i = 0; i < N; ++i)
        win[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(TwoPi * static_cast<float>(i) / static_cast<float>(N));
    // A Hann window of this length puts a sinusoid's energy across about three bins and halves
    // its amplitude; both are taken out here so a band's stored amplitude is the amplitude of the
    // sound and not of the analysis.
    const float norm = 2.0f / static_cast<float>(N) * 2.0f;

    for (int f = 0; f < frames; ++f) {
        const int off = f * hop;
        for (int i = 0; i < N; ++i) { re[static_cast<size_t>(i)] = m_mono[static_cast<size_t>(off + i)] * win[static_cast<size_t>(i)]; im[static_cast<size_t>(i)] = 0.0f; }
        fft.transform(re.data(), im.data(), false);
        for (int k = 0; k <= N / 2; ++k)
            mag[static_cast<size_t>(k)] = std::sqrt(re[static_cast<size_t>(k)] * re[static_cast<size_t>(k)] + im[static_cast<size_t>(k)] * im[static_cast<size_t>(k)]) * norm;
        // The local noise floor: the geometric mean of the magnitudes over twenty-one bins either
        // side. A partial stands well above it; noise sits on it. This is what decides how tonal a
        // band is, and it needs no fundamental and no pitch tracker to say so.
        logSum[0] = 0.0;
        for (int k = 0; k <= N / 2; ++k)
            logSum[static_cast<size_t>(k + 1)] = logSum[static_cast<size_t>(k)] + std::log(static_cast<double>(mag[static_cast<size_t>(k)]) + 1e-12);
        for (int b = 0; b < SpectralModel::Bands; ++b) {
            double power = 0.0, toneSum = 0.0;
            float peak = 0.0f; int peakBin = lo[b];
            for (int k = lo[b]; k <= hi[b]; ++k) {
                const float v = mag[static_cast<size_t>(k)];
                power += static_cast<double>(v) * v;
                if (v > peak) { peak = v; peakBin = k; }
                const int a = std::max(0, k - 10), c = std::min(N / 2, k + 10);
                const double floorMag = std::exp((logSum[static_cast<size_t>(c + 1)] - logSum[static_cast<size_t>(a)]) / (c - a + 1));
                const double above = static_cast<double>(v) / (floorMag + 1e-12);
                toneSum += clampv((above - 1.2) / 3.0, 0.0, 1.0) * static_cast<double>(v) * v;
            }
            // Where in the band the strongest partial sits, to a fraction of a bin: three
            // magnitudes around the peak fit a parabola, which is the standard refinement and is
            // worth having, because a band is wide and a partial at its edge is not its centre.
            double refined = peakBin;
            if (peakBin > 0 && peakBin < N / 2) {
                const double a = std::log(static_cast<double>(mag[static_cast<size_t>(peakBin - 1)]) + 1e-12);
                const double c = std::log(static_cast<double>(mag[static_cast<size_t>(peakBin + 1)]) + 1e-12);
                const double bb = std::log(static_cast<double>(mag[static_cast<size_t>(peakBin)]) + 1e-12);
                const double den = a - 2.0 * bb + c;
                if (std::fabs(den) > 1e-9) refined += clampv(0.5 * (a - c) / den, -0.5, 0.5);
            }
            const size_t idx = static_cast<size_t>(f) * SpectralModel::Bands + static_cast<size_t>(b);
            m.m_amp[idx] = static_cast<float>(std::sqrt(power));
            m.m_freq[idx] = static_cast<float>(refined * binHz);
            m.m_tone[idx] = power > 1e-18 ? static_cast<float>(toneSum / power) : 0.0f;
        }
    }
}

/**
 * @brief Catmull-Rom through four samples: the curve that passes through y1 and y2 with the slopes the
 *        neighbours imply.
 *
 * Against the straight line it folds back less of what it cannot represent when
 * a clip is played above its own pitch, and takes less off the top when it is played below.
 * Needs p[-stride] .. p[2*stride]; the caller guarantees that window, see `herm` in renderTexture.
 * `p` points at the sample before the fraction and `stride` is 1 for a mono clip, 2 for one channel
 * of an interleaved pair.
 * @param[in] p       the sample before the fraction; p[-stride] .. p[2 * stride] must be readable
 * @param[in] stride  1 for a mono clip, 2 for one channel of an interleaved stereo pair
 * @param[in] t       the fraction between p[0] and p[stride], 0 .. 1
 * @return the sample on the curve at that fraction
 */
static inline float hermiteAt(const float* p, int stride, float t)
{
    const float y0 = p[-stride], y1 = p[0], y2 = p[stride], y3 = p[2 * stride];
    const float a = 0.5f * ((y3 - y0) + 3.0f * (y1 - y2));
    const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    const float c = 0.5f * (y2 - y0);
    return ((a * t + b) * t + c) * t + y1;
}

/**
 * @brief One sample of a clip at a fractional position, either way of joining the dots.
 *
 * The choice is a template tag and not a branch, because the grain loop this serves is the
 * instrument's largest single cost and a test inside it is paid by every sample (see renderTexture).
 * @param[in] p       the sample before the fraction (for H the window of hermiteAt() must be readable)
 * @param[in] stride  1 for a mono clip, 2 for one channel of an interleaved stereo pair
 * @param[in] t       the fraction between p[0] and p[stride], 0 .. 1
 * @return the interpolated sample
 * @tparam H      true for the Catmull-Rom curve of hermiteAt(), false for the straight line
 */
template <bool H>
static inline float readAt(const float* p, int stride, float t)
{
    return H ? hermiteAt(p, stride, t) : p[0] + t * (p[stride] - p[0]);
}

void SourceSlot::renderTexture(float* outL, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt)
{
    // outL receives the left channel, m_scratch the right (the caller adds both).
    std::memset(m_scratch, 0, sizeof(float) * static_cast<size_t>(n));
    if (tex == nullptr || tex->empty()) return;
    // A stereo clip is read as a pair and keeps its own image; a mono one takes the path it always
    // took, to the instruction. Which of the two is a compile-time tag below, for the same reason
    // the interpolation is one: this loop is the instrument's largest single cost and a test inside
    // it is paid by every clip, including the ones that would not have used the answer.
    const bool wide = tex->stereo();
    const float* s = wide ? tex->m_lr.data() : tex->m_mono.data();
    const int stride = wide ? 2 : 1;
    const int len = static_cast<int>(tex->m_mono.size());
    const float wander = m_posDrift.update(dt, 0.02f, m_rng) * 0.15f * p.m_positionDrift;
    const int maxGrains = clampv(p.m_grains, 1, SlotGrains);

    // Spawn grains at Density per second (jittered), around Position.
    m_spawnIn -= dt;
    while (m_spawnIn <= 0.0) {
        // Where in THIS block the grain is due. m_spawnIn counts down from the block's end, so
        // the moment is dt + m_spawnIn from its start; at 200 grains a second that is a quarter
        // of a grain per block, and rounding every one of them to sample 0 is what built the comb.
        const int startAt = clampv(static_cast<int>((static_cast<double>(dt) + m_spawnIn) * m_sr), 0, n - 1);
        m_spawnIn += (1.0 / std::max(p.m_density, 0.1f)) * (0.7 + 0.6 * m_rng.uniform());
        // Append. Grains is a ceiling on how many may be STARTED, so it is tested here and
        // nowhere else: turning it down stops new grains and lets the sounding ones finish.
        if (m_live >= maxGrains || m_live >= SlotGrains) continue;   // full: this grain is dropped
        Grain* g = &m_grains[m_live];
        const double resample = tex->m_sampleRate / m_sr;
        // Note: the sample is pitched to the note (recorded at baseHz). Free: original speed,
        // with octave and ratio acting as a playback-speed multiplier.
        const double rate = (p.m_follow ? hz / std::max(tex->m_baseHz, 20.0) : speed) * resample;
        // A grain reads `glen * rate` samples of the clip, so a grain longer than the clip can hold
        // at that rate used to spawn nothing at all -- and "nothing at all" meant SILENCE, not a
        // shorter grain. A one-second clip with 300 ms grains goes quiet two octaves up, because
        // 300 ms at four times speed wants 1.2 s of material that is not there. Nothing said so:
        // the slot simply stopped. Shorten the grain to what the clip can give instead; at the top
        // of the keyboard the grains get shorter, which is what a granular player does anyway.
        int glen = std::max(64, static_cast<int>(p.m_grainMs * 0.001f * static_cast<float>(m_sr)));
        const double room = (static_cast<double>(len) - 2.0) / std::max(rate, 1e-9);
        if (room < 64.0) continue;                      // a clip too short for any grain at all
        if (static_cast<double>(glen) > room) glen = static_cast<int>(room);
        // What is left for Position to choose from. Shortening the grain above leaves a span of
        // very nearly nothing, and when `room` lands on a whole number -- which it does exactly
        // whenever the rate is a power of two, so at every Free slot standing at its own speed --
        // it leaves nothing at all. `span <= 0 -> no grain` then meant the slot fell silent for
        // good instead of playing the one grain the clip can hold, starting at its beginning.
        const double span = std::max(0.0, static_cast<double>(len) - static_cast<double>(glen) * rate - 2.0);
        const double centre = clampv(static_cast<double>(p.m_position) + wander, 0.0, 1.0) * span;
        // Spread scatters the start point around Position; at 1 a grain may come from anywhere.
        double start = centre + (m_rng.bipolar() * clampv(p.m_spread, 0.0f, 1.0f)) * len;
        start = clampv(start, 0.0, span);
        // Overlap normalisation, capped by the grains actually available: asking for more overlap
        // than the slot can hold used to make the sound quieter instead of denser.
        const float overlap = std::min(std::max(1.0f, p.m_density * p.m_grainMs * 0.001f),
                                       static_cast<float>(maxGrains));
        // For a Hann-windowed stream at overlap N the sum has RMS sqrt(N)*0.612*source, so the
        // constant that returns the source's own level is 1/0.612 = 1.63, not the 0.7 that stood
        // here -- which is where the missing 7.4 dB were.
        const float norm = 1.63f / std::sqrt(overlap);
        const float pan = clampv(p.m_pan + 0.3f * m_rng.bipolar(), -1.0f, 1.0f);
        const float angle = (pan + 1.0f) * 0.25f * Pi;
        g->m_pos = start; g->m_rate = rate; g->m_len = glen; g->m_age = 0; g->m_on = true; g->m_start = startAt;
        g->m_gain = p.m_level * norm * tex->m_gain;
        g->m_gl = g->m_gain * std::cos(angle); g->m_gr = g->m_gain * std::sin(angle);
        // Hann window as a rotating phasor: w = 0.5 - 0.5*wc, advanced by one grain-length step.
        g->m_wc = 1.0f; g->m_ws = 0.0f;
        phasorFrom(1.0 / static_cast<double>(glen), g->m_rc, g->m_rs);
        ++m_live;
    }
    // The grain loop is the instrument's largest single cost: measured with VTune over a preset
    // with four texture slots, `renderTexture` is 3.11 s of 9.87 s, 31 % of the whole render, and
    // it reaches 59 % with the density knobs up. So it is worth writing properly.
    //
    // Two things were in the way of that, and both are the same thing: a test inside the sample
    // loop. `age >= len || pos >= len - 2 || pos < 0` was evaluated once per grain per SAMPLE, and
    // it can be evaluated once per grain per BLOCK -- a grain runs out of window or runs off the
    // end of the clip at a moment that is known in advance from its rate. Hoisted, the loop has no
    // branch left in it, every iteration is the same arithmetic, and the compiler can do what it
    // could never do before.
    // The live grains, densely packed, and every one of them regardless of what Grains says now.
    // That ceiling belongs to the spawn loop above and to nowhere else: turning it down used to
    // leave everything above the new value cut off mid-waveform (a click), never advanced again,
    // and still marked as in use, so its place was gone for good.
    for (int c = 0; c < m_live; ) {
        Grain& g = m_grains[c];
        {   // keep the window phasor on the unit circle (a grain can run for 48000 samples)
            const float r2 = g.m_wc * g.m_wc + g.m_ws * g.m_ws, fix = 1.5f - 0.5f * r2;
            g.m_wc *= fix; g.m_ws *= fix;
        }
        // How far this grain gets before one of the three conditions stops it. `pos` walks by
        // `rate` a sample, so the number of samples until it reaches len-2 is a division, not a
        // search; the window ends after len - age samples; a negative position (which the spawn
        // never produces) ends it at once.
        // A grain that began part-way through this block writes from there; every one after its
        // first block starts at zero again.
        const int begin = g.m_start < n ? g.m_start : n;
        g.m_start = 0;
        const int room = n - begin;
        float* const oL = outL + begin;
        float* const oR = m_scratch + begin;
        // Hermite reads one sample before the position and two after it, so its window into the
        // clip is two narrower at each end than the straight line's. The bound is computed here
        // once, as everything else about the end of a grain is.
        const bool herm = p.m_interp == 1 && len >= 8;
        const double lo = herm ? 1.0 : 0.0;
        const double hi = static_cast<double>(len) - (herm ? 3.0 : 2.0);
        int safe = room;
        if (g.m_pos < lo || g.m_age >= g.m_len || g.m_pos >= hi) {
            safe = 0;
        } else {
            const int left = g.m_len - g.m_age;
            if (left < safe) safe = left;
            if (g.m_rate > 0.0) {
                const double room = (hi - g.m_pos) / g.m_rate;
                const int steps = static_cast<int>(std::ceil(room));
                if (steps < safe) safe = steps < 0 ? 0 : steps;
            }
        }
        // Position from the block's start rather than accumulated: over a grain of 48000 samples
        // the running sum drifts, and every lane of a vector needs the same expression anyway.
        const double base = g.m_pos;
        const float gl = g.m_gl, gr = g.m_gr, rc = g.m_rc, rs = g.m_rs;
        float wc = g.m_wc, ws = g.m_ws;
        int i = 0;
        // The interpolation is chosen once per grain, not once per sample. Written as a plain
        // `if` inside the loop it cost seven per cent EVEN WITH HERMITE OFF -- measured, 6.96 s
        // against 7.45: a branch in the innermost loop is paid by everybody. As a tag the
        // compiler resolves it before the loop exists and the linear path is what it was.
        const auto runGrain = [&](auto hermTag, auto wideTag) {
            constexpr bool H = decltype(hermTag)::value;
            constexpr bool W = decltype(wideTag)::value;
            (void) stride;
#if AMBIENT_HAS_AVX && defined(__AVX2__)
        // Eight SAMPLES of one grain at a time, not eight grains. Vectorising across grains is the
        // obvious cut and the wrong one: they all add into the same output sample, so every step
        // ends in a horizontal reduction, and grains begin and end at different moments, so the
        // lanes are never all alive. Across samples the accumulation is a plain vector add, the
        // lifetime stays scalar, and the only hard part is the interpolation -- eight different
        // places in the clip, which is what the gather is for.
        //
        // The window is eight phasors seeded at r^0..r^7 and turned by r^8 each pass: the same
        // arithmetic as the scalar path, eight at a time. Its value drifts from the scalar one in
        // the last bit or two after enough turns, exactly as the phasor bank in Simd.h does, and
        // for the same reason.
        if (safe >= 8) {
            alignas(32) float wcL[8], wsL[8];
            {
                float c = wc, sn = ws;
                for (int k = 0; k < 8; ++k) {
                    wcL[k] = c; wsL[k] = sn;
                    const float nc = c * rc - sn * rs;
                    sn = sn * rc + c * rs;
                    c = nc;
                }
            }
            const float c2 = rc * rc - rs * rs,   s2 = 2.0f * rc * rs;      // r^2
            const float c4 = c2 * c2 - s2 * s2,   s4 = 2.0f * c2 * s2;      // r^4
            const float c8 = c4 * c4 - s4 * s4,   s8 = 2.0f * c4 * s4;      // r^8
            __m256 vwc = _mm256_load_ps(wcL), vws = _mm256_load_ps(wsL);
            const __m256 vc8 = _mm256_set1_ps(c8), vs8 = _mm256_set1_ps(s8);
            const __m256 vgl = _mm256_set1_ps(gl), vgr = _mm256_set1_ps(gr);
            const __m256 vhalf = _mm256_set1_ps(0.5f);
            // The lanes are computed in double, and that was tried the other way round: keeping the
            // base in double and the eight offsets in float fits them in one register instead of
            // two and drops the conversions and the lane splits, which is fifteen instructions in
            // the head of the loop reduced to six. Measured with VTune, that version was SLOWER --
            // renderTexture 1.04 s against 0.80 s, thirty per cent worse. The head of this loop is
            // not what it waits for; it waits for the gather, and the two broadcasts a float
            // version needs per pass (one from a double, one from an integer) cross between the
            // scalar and vector sides on every one of them. Left as it is, with the measurement
            // written down so nobody spends the afternoon on it twice.
            const __m256d vrate = _mm256_set1_pd(g.m_rate);
            const __m256d Lo = _mm256_setr_pd(0.0, 1.0, 2.0, 3.0);
            const __m256d Hi = _mm256_setr_pd(4.0, 5.0, 6.0, 7.0);
            for (; i + 8 <= safe; i += 8) {
                const __m256d b  = _mm256_set1_pd(base + g.m_rate * i);
                const __m256d pa = _mm256_add_pd(b, _mm256_mul_pd(vrate, Lo));
                const __m256d pb = _mm256_add_pd(b, _mm256_mul_pd(vrate, Hi));
                const __m128i ia = _mm256_cvttpd_epi32(pa);
                const __m128i ib = _mm256_cvttpd_epi32(pb);
                const __m256i idx = _mm256_insertf128_si256(_mm256_castsi128_si256(ia), ib, 1);
                const __m128 fa = _mm256_cvtpd_ps(_mm256_sub_pd(pa, _mm256_cvtepi32_pd(ia)));
                const __m128 fb = _mm256_cvtpd_ps(_mm256_sub_pd(pb, _mm256_cvtepi32_pd(ib)));
                const __m256 frac = _mm256_insertf128_ps(_mm256_castps128_ps256(fa), fb, 1);
                // One channel at eight places. For an interleaved pair the neighbour a sample
                // ahead is two floats along instead of one, and the index doubles -- which is why
                // the pair is stored interleaved: the second channel's gather lands in the cache
                // lines the first one has just pulled in, so it costs an instruction and not a
                // memory stall.
                constexpr int st = W ? 2 : 1;
                (void) st;                       // folded into the addresses below; MSVC counts that as unused
                __m256i idxS = idx;
                if constexpr (W) idxS = _mm256_add_epi32(idx, idx);
                const auto readEight = [&](const float* p) -> __m256 {
                    if constexpr (H) {
                        // Catmull-Rom through four samples. Two more gathers, which is most of
                        // what it costs: the gather is what this loop waits for, so Hermite is
                        // roughly twice the work of the line and is a choice, not the default.
                        const __m256 y0 = _mm256_i32gather_ps(p - st, idxS, 4);
                        const __m256 y1 = _mm256_i32gather_ps(p,          idxS, 4);
                        const __m256 y2 = _mm256_i32gather_ps(p + st, idxS, 4);
                        const __m256 y3 = _mm256_i32gather_ps(p + 2 * st, idxS, 4);
                        const __m256 h  = _mm256_set1_ps(0.5f);
                        const __m256 ca = _mm256_mul_ps(h, _mm256_add_ps(_mm256_sub_ps(y3, y0),
                                            _mm256_mul_ps(_mm256_set1_ps(3.0f), _mm256_sub_ps(y1, y2))));
                        const __m256 cb = _mm256_add_ps(_mm256_sub_ps(y0, _mm256_mul_ps(_mm256_set1_ps(2.5f), y1)),
                                            _mm256_sub_ps(_mm256_add_ps(y2, y2), _mm256_mul_ps(h, y3)));
                        const __m256 cc = _mm256_mul_ps(h, _mm256_sub_ps(y2, y0));
                        return _mm256_fmadd_ps(_mm256_fmadd_ps(_mm256_fmadd_ps(ca, frac, cb), frac, cc), frac, y1);
                    } else {
                        const __m256 s0 = _mm256_i32gather_ps(p,      idxS, 4);
                        const __m256 s1 = _mm256_i32gather_ps(p + st, idxS, 4);
                        return _mm256_fmadd_ps(frac, _mm256_sub_ps(s1, s0), s0);
                    }
                };
                const __m256 vL = readEight(s);
                __m256 vR = vL;
                if constexpr (W) vR = readEight(s + 1);
                const __m256 w  = _mm256_sub_ps(vhalf, _mm256_mul_ps(vhalf, vwc));
                _mm256_storeu_ps(oL + i, _mm256_add_ps(_mm256_loadu_ps(oL + i), _mm256_mul_ps(_mm256_mul_ps(vL, w), vgl)));
                _mm256_storeu_ps(oR + i, _mm256_add_ps(_mm256_loadu_ps(oR + i), _mm256_mul_ps(_mm256_mul_ps(vR, w), vgr)));
                const __m256 nc = _mm256_sub_ps(_mm256_mul_ps(vwc, vc8), _mm256_mul_ps(vws, vs8));
                vws = _mm256_add_ps(_mm256_mul_ps(vws, vc8), _mm256_mul_ps(vwc, vs8));
                vwc = nc;
            }
            // Lane 0 is the phasor for sample i, which is where the tail picks it up.
            alignas(32) float lastC[8], lastS[8];
            _mm256_store_ps(lastC, vwc);
            _mm256_store_ps(lastS, vws);
            wc = lastC[0]; ws = lastS[0];
        }
#elif AMBIENT_HAS_NEON
        // The Quest. Four lanes, and the interpolation stays scalar because NEON has no gather:
        // eight places in the clip are eight loads however they are written. What vectorises is
        // the window, the level and the two accumulations -- most of the arithmetic, none of the
        // memory. Written to the same shape as the AVX path above so the two can be read together.
        if (safe >= 4) {
            float wcL[4], wsL[4];
            {
                float c = wc, sn = ws;
                for (int k = 0; k < 4; ++k) {
                    wcL[k] = c; wsL[k] = sn;
                    const float nc = c * rc - sn * rs;
                    sn = sn * rc + c * rs;
                    c = nc;
                }
            }
            const float c2 = rc * rc - rs * rs, s2 = 2.0f * rc * rs;   // r^2
            const float c4 = c2 * c2 - s2 * s2, s4 = 2.0f * c2 * s2;   // r^4
            float32x4_t vwc = vld1q_f32(wcL), vws = vld1q_f32(wsL);
            const float32x4_t vc4 = vdupq_n_f32(c4), vs4 = vdupq_n_f32(s4);
            const float32x4_t vgl = vdupq_n_f32(gl), vgr = vdupq_n_f32(gr);
            const float32x4_t vhalf = vdupq_n_f32(0.5f);
            constexpr int st = W ? 2 : 1;
            for (; i + 4 <= safe; i += 4) {
                float laneL[4], laneR[4];
                for (int k = 0; k < 4; ++k) {
                    const double pos = base + g.m_rate * (i + k);
                    const int ip = static_cast<int>(pos);
                    const float f = static_cast<float>(pos - ip);
                    laneL[k] = readAt<H>(s + st * ip, st, f);
                    if constexpr (W) laneR[k] = readAt<H>(s + st * ip + 1, st, f);
                }
                (void) laneR;
                const float32x4_t vL = vld1q_f32(laneL);
                float32x4_t vR = vL;
                if constexpr (W) vR = vld1q_f32(laneR);
                const float32x4_t w  = vsubq_f32(vhalf, vmulq_f32(vhalf, vwc));
                vst1q_f32(oL + i, vaddq_f32(vld1q_f32(oL + i), vmulq_f32(vmulq_f32(vL, w), vgl)));
                vst1q_f32(oR + i, vaddq_f32(vld1q_f32(oR + i), vmulq_f32(vmulq_f32(vR, w), vgr)));
                const float32x4_t nc = vsubq_f32(vmulq_f32(vwc, vc4), vmulq_f32(vws, vs4));
                vws = vaddq_f32(vmulq_f32(vws, vc4), vmulq_f32(vwc, vs4));
                vwc = nc;
            }
            float lastC[4], lastS[4];
            vst1q_f32(lastC, vwc);
            vst1q_f32(lastS, vws);
            wc = lastC[0]; ws = lastS[0];
        }
#endif
        constexpr int stT = W ? 2 : 1;
        for (; i < safe; ++i) {
            const double pos = base + g.m_rate * i;
            const int ip = static_cast<int>(pos);
            const float f = static_cast<float>(pos - ip);
            const float vL = readAt<H>(s + stT * ip, stT, f);
            const float vR = W ? readAt<H>(s + stT * ip + 1, stT, f) : vL;
            const float w = 0.5f - 0.5f * wc;
            oL[i] += vL * w * gl;
            oR[i] += vR * w * gr;
            const float nc = wc * rc - ws * rs;
            ws = ws * rc + wc * rs;
            wc = nc;
        }
        };
        // Four bodies, chosen once per grain per block: line or curve, one channel or two.
        if (herm) { if (wide) runGrain(std::true_type{},  std::true_type{});  else runGrain(std::true_type{},  std::false_type{}); }
        else      { if (wide) runGrain(std::false_type{}, std::true_type{});  else runGrain(std::false_type{}, std::false_type{}); }
        g.m_pos = base + g.m_rate * safe;
        g.m_age += safe;
        g.m_wc = wc; g.m_ws = ws;
        if (safe < room) {
            // Dead. The last live grain takes its place and the count drops; `c` stays where it
            // is, because what now sits there has not been rendered yet.
            g.m_on = false;
            --m_live;
            if (c != m_live) m_grains[c] = m_grains[m_live];
        } else {
            ++c;
        }
    }
}

// ---------------------------------------------------------------- stretch

/**
 * @brief Paulstretch, in a voice.
 *
 * A window of the clip is transformed, its magnitudes are kept and its
 * phases thrown away and drawn afresh, and the result is overlap-added at a quarter of the
 * window -- the same machinery as the Cosmos's Nebula, pointed at a recording instead of at the
 * mix. Every frame is a plausible piece of the clip's spectrum with no memory of where its
 * transients were, so the analysis position can crawl through the recording at a thousandth of
 * its speed and what comes out is a continuum: twenty seconds of rain becoming an evening of it.
 *
 * Pitch is applied when the window is READ, as a resampling step through the clip, and the
 * stretch is applied to how far the read position moves between frames. The two do not know
 * about each other, which is the point: Follow = Note plays a chromatic sample across the
 * keyboard without a high note ending sooner than a low one.
 *
 * This is the one frame: renderStretch() calls it whenever the overlap-add has run out of hop.
 */

void SourceSlot::stretchFrame(const SlotParams& p, const Texture* tex, double rate, int N)
{
    const float* s = tex->m_mono.data();
    const int len = static_cast<int>(tex->m_mono.size());
    if (m_st.m_n != N) {   // a new window size: its Hann, and a clean start for the overlap
        m_st.m_n = N;
        for (int i = 0; i < N; ++i) m_st.m_win[static_cast<size_t>(i)] = 0.5f - 0.5f * std::cos(TwoPi * i / N);
    }
    // The seam. A clip marked seamless wraps straight round; any other loops over [zone, len)
    // and fades its last `zone` samples into its first `zone`, so the wrap lands on the sample
    // the fade has been arriving at. Half of Xfade of the clip, at most a quarter of it.
    const double zone = tex->m_seamless ? 0.0 : clampv(0.5 * static_cast<double>(p.m_xfade), 0.0, 0.25) * len;
    const double loopLen = static_cast<double>(len) - zone;
    auto wrap = [&](double q) {
        if (loopLen <= 1.0) return 0.0;
        // Far away -- a high note over a short clip puts the window's start millions of samples
        // out -- one fmod brings it into reach; walking there one loop at a time took hundreds of
        // thousands of steps per read. Near the loop, the same two steps as before.
        if (q >= len + 2.0 * loopLen || q < zone - 2.0 * loopLen) {
            q = zone + std::fmod(q - zone, loopLen);
            if (q < zone) q += loopLen;
        }
        while (q >= len) q -= loopLen;
        while (q < zone) q += loopLen;
        return q;
    };
    auto read = [&](double q) {
        q = wrap(q);
        const int ip = static_cast<int>(q);
        const float f = static_cast<float>(q - ip);
        const int ip1 = ip + 1 < len ? ip + 1 : ip;
        float v = s[ip] + f * (s[ip1] - s[ip]);
        if (zone > 0.0 && q > len - zone) {
            const double a = (q - (len - zone)) / zone;          // 0 at the start of the fade, 1 at the end
            double q2 = q - loopLen;                              // the matching point near the clip's start
            if (q2 < 0.0) q2 = 0.0;
            const int jp = static_cast<int>(q2);
            const float g = static_cast<float>(q2 - jp);
            const int jp1 = jp + 1 < len ? jp + 1 : jp;
            const float w = s[jp] + g * (s[jp1] - s[jp]);
            v = static_cast<float>((1.0 - a) * v + a * w);
        }
        return v;
    };
    // The window, read around the analysis position at the pitch's rate.
    const double centre = clampv(static_cast<double>(p.m_position), 0.0, 1.0) * loopLen + zone + m_st.m_advance;
    const double start = centre - 0.5 * N * rate;
    for (int i = 0; i < N; ++i)
        m_st.m_re[static_cast<size_t>(i)] = read(start + i * rate) * m_st.m_win[static_cast<size_t>(i)];
    const RealFft& fft = stretchFft(N);
    fft.forward(m_st.m_re.data(), m_st.m_re.data(), m_st.m_im.data());
    // Keep the magnitudes, draw the phases: what makes it a continuum rather than a loop.
    for (int k = 0; k <= N / 2; ++k) {
        const float m = std::sqrt(m_st.m_re[static_cast<size_t>(k)] * m_st.m_re[static_cast<size_t>(k)] + m_st.m_im[static_cast<size_t>(k)] * m_st.m_im[static_cast<size_t>(k)]);
        const float ph = m_rng.uniform();
        const float r = m * sin01(ph + 0.25 >= 1.0 ? ph - 0.75 : ph + 0.25), q = m * sin01(ph);
        m_st.m_re[static_cast<size_t>(k)] = r; m_st.m_im[static_cast<size_t>(k)] = q;
        if (k > 0 && k < N / 2) { m_st.m_re[static_cast<size_t>(N - k)] = r; m_st.m_im[static_cast<size_t>(N - k)] = -q; }
    }
    m_st.m_im[0] = 0.0f; m_st.m_im[static_cast<size_t>(N / 2)] = 0.0f;
    // The scratch is the imaginary half itself: this transform is shared by every slot of
    // every voice, so it must not write into itself (see RealFft).
    fft.inverse(m_st.m_re.data(), m_st.m_im.data(), m_st.m_re.data(), m_st.m_im.data());
    // Overlap-add at a quarter of the window. 1.3 is the Nebula's measured unity constant for
    // random-phase resynthesis at this hop with Hann in and out; the clip's own gain brings a
    // quiet recording to the level the other types normalise themselves to.
    const int outMask = 2 * StretchMaxN - 1;
    const float g = 1.3f * tex->m_gain;
    for (int i = 0; i < N; ++i)
        m_st.m_out[static_cast<size_t>((m_st.m_outPos + i) & outMask)] += m_st.m_re[static_cast<size_t>(i)] * m_st.m_win[static_cast<size_t>(i)] * g;
    // The read moves on by a hop, slowed by the stretch. At 1000 that is four samples of clip per
    // frame of a third of a second.
    m_st.m_advance += (0.25 * N) * rate / std::max(static_cast<double>(p.m_stretch), 1.0);
    if (loopLen > 1.0) while (m_st.m_advance >= loopLen) m_st.m_advance -= loopLen;
    m_st.m_hopLeft += N / 4;
}

void SourceSlot::renderStretch(float* out, int n, double hz, double speed, const SlotParams& p, const Texture* tex, float dt)
{
    if (tex == nullptr || tex->empty() || m_st.m_out.empty()) { std::memset(out, 0, sizeof(float) * static_cast<size_t>(n)); return; }
    // Position wanders the way it does for the grains, and the window comes from Grain.
    (void)m_posDrift.update(dt, 0.02f, m_rng);
    int N = StretchMinN;
    while (N < static_cast<int>(p.m_grainMs * 0.001f * static_cast<float>(m_sr)) && N < StretchMaxN) N <<= 1;
    const double resample = tex->m_sampleRate / m_sr;
    const double rate = (p.m_follow ? hz / std::max(tex->m_baseHz, 20.0) : speed) * resample;
    const int outMask = 2 * StretchMaxN - 1;
    for (int i = 0; i < n; ++i) {
        while (m_st.m_hopLeft <= 0) stretchFrame(p, tex, rate, N);
        const size_t o = static_cast<size_t>(m_st.m_outPos & outMask);
        out[i] = m_st.m_out[o];
        m_st.m_out[o] = 0.0f;
        ++m_st.m_outPos;
        --m_st.m_hopLeft;
    }
}

namespace {
/**
 * @brief Paul Kellet's pink filter, all seven terms.
 *
 * The three-pole short form is 1.7 dB per octave too
 * steep -- measured, which is why it is not used here.
 * @param[in,out] st  the channel's noise state; its seven pink terms are advanced
 * @param[in]     w   one sample of white noise, -1 .. 1
 * @return one sample of pink noise, scaled to sit near the white's level
 */
inline float pinkStep(SourceSlot::NoiseState& st, float w)
{
    st.m_pink[0] = 0.99886f * st.m_pink[0] + w * 0.0555179f;
    st.m_pink[1] = 0.99332f * st.m_pink[1] + w * 0.0750759f;
    st.m_pink[2] = 0.96900f * st.m_pink[2] + w * 0.1538520f;
    st.m_pink[3] = 0.86650f * st.m_pink[3] + w * 0.3104856f;
    st.m_pink[4] = 0.55000f * st.m_pink[4] + w * 0.5329522f;
    st.m_pink[5] = -0.7616f * st.m_pink[5] - w * 0.0168980f;
    const float out = st.m_pink[0] + st.m_pink[1] + st.m_pink[2] + st.m_pink[3] + st.m_pink[4] + st.m_pink[5]
                    + st.m_pink[6] + w * 0.5362f;
    st.m_pink[6] = w * 0.115926f;
    return out * 0.18f;
}
} // namespace

// ---------------------------------------------------------------- noise

/**
 * @brief Ten colours.
 *
 * The three textbook slopes (pink, brown, blue/violet) plus grey, a resonant band
 * that can track the note, a wandering band that is wind, sparse crackle and sample-and-hold
 * digital noise. Each is normalised so that Level means roughly the same loudness across the
 * lot -- the same lesson the Texture slot taught: a source whose Level means something different
 * from its neighbour's is a source nobody uses.
 */
void SourceSlot::renderNoise(float* outL, int n, double hz, const SlotParams& p, float dt)
{
    std::memset(m_scratch, 0, sizeof(float) * static_cast<size_t>(n));
    const NoiseKindE kind = static_cast<NoiseKindE>(clampv(static_cast<int>(p.m_noise), 0, NumNoiseKinds - 1));

    // Position picks the band centre (or the colour), and Pos Drift lets it wander. With Pitch =
    // Note the centre follows the played note instead, which turns Band into a formant.
    const float wander = m_noiseDrift.update(dt, 0.03f, m_rng) * p.m_positionDrift;
    const float posN = clampv(p.m_position + 0.35f * wander, 0.0f, 1.0f);
    double centre = 40.0 * std::pow(300.0, static_cast<double>(posN));      // 40 Hz .. 12 kHz
    if (p.m_follow) centre = clampv(hz * (0.5 + 8.0 * posN), 30.0, 0.45 * m_sr);
    // The band pass below is a Chamberlin state-variable filter, and that one is only stable
    // with its centre below a sixth of the sample rate (f <= 1). Above that it need not blow up
    // at once -- two presets in eight thousand ran clean for forty-five seconds and then went
    // non-finite: a Wind slot following the note an octave up, Position drifting, the centre
    // pinned at 0.45 sr with f near 2. The clamp is the difference between a filter that is
    // stable and one that usually is.
    centre = clampv(centre, 20.0, std::min(0.45 * m_sr, m_sr / 6.0));

    // State-variable band pass; q from Noise Q, wider for Wind so it breathes rather than whistles.
    const float f = 2.0f * std::sin(static_cast<float>(Pi * centre / m_sr));
    const float qAmount = clampv(p.m_noiseQ, 0.0f, 1.0f);
    const float damp = kind == NoiseKindE::WIND ? (0.6f - 0.5f * qAmount) : (0.7f - 0.66f * qAmount);

    const double rate = kind == NoiseKindE::DIGITAL
        ? clampv(200.0 * std::pow(100.0, static_cast<double>(posN)), 100.0, 0.5 * m_sr)   // 200 Hz .. 20 kHz
        : 0.0;
    const double crackleRate = clampv(static_cast<double>(p.m_density) * 8.0, 1.0, 4000.0);

    // Level per colour, measured so a slot at Level 1 lands near the wavetable slot's output.
    // Measured against a Wavetable slot at the same Level (-22.8 dBFS) and corrected, one colour
    // at a time. Without this a violet slot was thirty decibels below a pink one at the same
    // setting, which is the same trap the Texture slot fell into.
    // Measured with the voice filter open against a Wavetable slot at the same Level (-22.0
    // dBFS) and corrected one colour at a time. Without this a violet slot sat eleven decibels
    // above a pink one at the same setting -- the trap the Texture slot fell into.
    static const float Gain[NumNoiseKinds] = {
        0.85f,   // White
        1.29f,   // Pink
        1.74f,   // Brown
        3.19f,   // Blue
        0.99f,   // Violet
        0.86f,   // Grey
        3.20f,   // Band
        3.07f,   // Wind
        1.80f,   // Crackle
        0.64f,   // Digital
        4.0f,    // Cicada: clicks ringing in a narrow band have a crest the others do not; at 12 the peaks hit the clipper
    };
    const float gain = Gain[static_cast<int>(kind)] * p.m_level;
    // Cicada: the band sits where the insects sing, 4 to 8 kHz from Position, however the other
    // colours read the knob; Density scales how fast each insect's pulses come.
    const float fCic = kind == NoiseKindE::CICADA
        ? 2.0f * std::sin(static_cast<float>(Pi * clampv(4000.0 * std::pow(2.0, static_cast<double>(posN)), 20.0, std::min(0.45 * m_sr, m_sr / 6.0)) / m_sr))
        : f;
    const float dampCic = 0.25f - 0.22f * qAmount;

    for (int c = 0; c < 2; ++c) {
        NoiseState& st = m_noise[c];
        float* dst = (c == 0) ? outL : m_scratch;
        for (int i = 0; i < n; ++i) {
            const float w = m_rng.bipolar();
            float v = 0.0f;
            switch (kind) {
            case NoiseKindE::WHITE: v = w; break;
            case NoiseKindE::PINK: {
                // Paul Kellet's economy filter: three poles, about 1 dB from a true 1/f slope.
                v = pinkStep(st, w);
                break;
            }
            case NoiseKindE::BROWN: {
                st.m_brown = clampv(st.m_brown + 0.02f * w, -1.0f, 1.0f);
                v = st.m_brown;
                break;
            }
            case NoiseKindE::BLUE: {   // differentiated pink: +3 dB per octave
                const float pink = pinkStep(st, w);
                v = pink - st.m_prev;
                st.m_prev = pink;
                break;
            }
            case NoiseKindE::VIOLET:    // differentiated white: +6 dB per octave
                v = w - st.m_prev;
                st.m_prev = w;
                break;
            case NoiseKindE::GREY: {
                // White with the ear's most sensitive region taken out, so it sounds flat rather
                // than measuring flat: a broad dip around 3 kHz through the same band pass.
                st.m_bp2 += f * st.m_bp1;
                const float hp = w - st.m_bp2 - 0.4f * st.m_bp1;
                st.m_bp1 += f * hp;
                v = w - 0.75f * st.m_bp1;
                break;
            }
            case NoiseKindE::BAND:
            case NoiseKindE::WIND: {
                st.m_bp2 += f * st.m_bp1;
                const float hp = w - st.m_bp2 - damp * st.m_bp1;
                st.m_bp1 += f * hp;
                v = st.m_bp1;
                break;
            }
            case NoiseKindE::CRACKLE: {
                // Sparse impulses, each a short decaying blip: vinyl, embers, rain on a roof.
                st.m_nextGrain -= 1.0;
                if (st.m_nextGrain <= 0.0) {
                    st.m_nextGrain = -std::log(1.0 - static_cast<double>(m_rng.uniform()) + 1e-9) * m_sr / crackleRate;
                    st.m_crackle = m_rng.bipolar();
                    st.m_crackleDecay = std::exp(-1.0f / (0.0015f * (1.0f + 6.0f * posN) * static_cast<float>(m_sr)));
                }
                v = st.m_crackle;
                st.m_crackle *= st.m_crackleDecay;
                break;
            }
            case NoiseKindE::DIGITAL: {
                st.m_holdLeft -= rate;
                if (st.m_holdLeft <= 0.0) { st.m_holdLeft += m_sr; st.m_hold = w; }
                v = st.m_hold;
                break;
            }
            case NoiseKindE::CICADA: {
                // Stridulation: an insect rubs a file across a scraper, one click per tooth,
                // sixty to a hundred and twenty a second, in bursts of half a second to two, and
                // rests between the bursts. Two insects per ear, their clocks their own, so the
                // pair never falls into step; each breathes in level over a few seconds, which is
                // the swell of a real chorus. Every click rings in the band pass below, and the
                // band's Q is the insect's resonator.
                float pulses = 0.0f;
                for (int k = 0; k < 2; ++k) {
                    if (st.m_burstLeft[k] > 0.0) {
                        st.m_burstLeft[k] -= 1.0;
                        st.m_pulseLeft[k] -= 1.0;
                        if (st.m_pulseLeft[k] <= 0.0) {
                            st.m_pulseLeft[k] += m_sr / static_cast<double>(st.m_pulseRate[k] * std::max(0.25f, p.m_density / 12.0f));
                            pulses += 0.3f * st.m_breathe[k] * (0.8f + 0.2f * m_rng.uniform());
                        }
                        if (st.m_burstLeft[k] <= 0.0) st.m_pauseLeft[k] = (0.4 + 3.6 * static_cast<double>(m_rng.uniform())) * m_sr;
                    } else {
                        st.m_pauseLeft[k] -= 1.0;
                        if (st.m_pauseLeft[k] <= 0.0) {
                            st.m_burstLeft[k] = (0.4 + 1.6 * static_cast<double>(m_rng.uniform())) * m_sr;
                            st.m_pulseRate[k] = 60.0f + 60.0f * m_rng.uniform();
                            st.m_breatheTo[k] = 0.4f + 0.6f * m_rng.uniform();
                        }
                    }
                    st.m_breathe[k] += (st.m_breatheTo[k] - st.m_breathe[k]) * 2.0e-5f;
                }
                st.m_bp2 += fCic * st.m_bp1;
                const float hp = pulses - st.m_bp2 - dampCic * st.m_bp1;
                st.m_bp1 += fCic * hp;
                v = st.m_bp1;
                break;
            }
            default: break;
            }
            dst[i] = v;
        }
        // Whatever the filter did, it does not get to poison the mix for the rest of the session:
        // a state that has gone non-finite is a state that starts again.
        if (!std::isfinite(st.m_bp1) || !std::isfinite(st.m_bp2)) {
            st.m_bp1 = st.m_bp2 = 0.0f;
            std::memset(dst, 0, sizeof(float) * static_cast<size_t>(n));
        }
    }

    // Pan and level, ramped across the block like the other types.
    const float angle = (clampv(p.m_pan, -1.0f, 1.0f) + 1.0f) * 0.25f * Pi;
    const float gl = gain * std::cos(angle), gr = gain * std::sin(angle);
    for (int i = 0; i < n; ++i) {
        const float l = outL[i], r = m_scratch[i];
        outL[i] = l * gl;
        m_scratch[i] = r * gr;
    }
}


// ---------------------------------------------------------------- Bow

/**
 * @fn void ambient::SourceSlot::renderBow(float* out, int n, double hz, const SlotParams& p, float dt)
 * @brief A bowed string, after McIntyre, Schumacher and Woodhouse (1983) and the waveguide form of Smith
 *        (2010).
 *
 * The instrument had a struck source (Strike) but nothing continuously excited, and the
 * two are not the same thing: a struck body rings and dies, a bowed one is driven for as long as
 * the bow moves and settles into a stick-slip oscillation of its own -- which is exactly what a
 * drone wants, a note that sustains because it is being fed rather than because its release is long.
 *
 * The string is two waveguides meeting at the bow -- one to the nut, one to the bridge -- so that
 * the bow's position along the string decides which partials it favours, as it does on a real
 * instrument. The nut reflects and inverts; the bridge reflects, inverts and loses the highs,
 * which is what makes the upper partials die first. Every sample the friction between bow and string is evaluated:
 *
 *     dv   = v_bow - v_string           the relative velocity at the bow
 *     rho  = min(1, (F / (|dv| + eps))^0.8)   the Stribeck curve: sticking while dv is small,
 *                                             slipping once it is not
 *     out  = dv * rho
 *
 * so a slow bow with a heavy hand sticks for most of the period and releases suddenly -- the
 * Helmholtz motion -- and a fast bow with a light hand slips more and gives the thinner, airier
 * tone. The friction force is fed back into the string, and the string's own motion changes the
 * relative velocity on the next sample, which is the loop that makes it oscillate at all.
 *
 * It is guarded rather than trusted: the loop gain is below one by construction, the injected
 * force is clamped, and a non-finite state resets the string. A physical model that runs away is
 * a burst of full-scale noise, and this instrument's whole point is that it never does that.
 */

/**
 * @brief Playing a recording back from its band model rather than from its samples.
 *
 * Two numbers that a sampler has to share are separate here. `transpose` scales every band's
 * frequency, so the note decides the pitch; Rate scales how fast the model is read, so it decides
 * the speed. At Rate 0 the read head stands still and the clip becomes one endless chord -- a
 * frozen moment of a recording, held at whatever pitch is played, which no amount of granular
 * overlap can do without a texture of its own.
 *
 * Each band is rebuilt from two things: an oscillator at the band's strongest partial, weighted by
 * how tonal the band measured, and a band of noise at the same place, weighted by the rest. Breath
 * tilts that balance -- all the way to the partials on its own, or all the way to the wind.
 */
void SourceSlot::renderSpectral(float* out, int n, double transpose, const SlotParams& p, const Texture* tex, float dt)
{
    if (tex == nullptr || tex->m_spectral.empty()) { m_active = 0; return; }
    const SpectralModel& m = tex->m_spectral;
    const int B = SpectralModel::Bands;

    // Where in the model to read: Position sets the point, Rate carries the head on from it, and
    // the drift wanders around both. All three in frames, wrapped, never stepped.
    const float wander = m_posDrift.update(dt, 0.02f, m_rng) * 0.15f * p.m_positionDrift;
    const double span = static_cast<double>(m.m_frames - 1);
    m_specAdvance += static_cast<double>(p.m_specRate) * (dt / std::max(1.0e-4f, m.m_hop));
    if (m_specAdvance > 1.0e9 || m_specAdvance < -1.0e9) m_specAdvance = 0.0;
    double fr = static_cast<double>(clampv(p.m_position + wander, 0.0f, 1.0f)) * span + m_specAdvance;
    fr = std::fmod(fr, span);
    if (fr < 0.0) fr += span;
    const int i0 = clampv(static_cast<int>(fr), 0, m.m_frames - 1);
    const int i1 = (i0 + 1) % m.m_frames;
    const float frac = static_cast<float>(fr - i0);
    const float* a0 = m.m_amp.data() + static_cast<size_t>(i0) * B;
    const float* a1 = m.m_amp.data() + static_cast<size_t>(i1) * B;
    const float* f0 = m.m_freq.data() + static_cast<size_t>(i0) * B;
    const float* f1 = m.m_freq.data() + static_cast<size_t>(i1) * B;
    const float* t0 = m.m_tone.data() + static_cast<size_t>(i0) * B;
    const float* t1 = m.m_tone.data() + static_cast<size_t>(i1) * B;

    const float breath = clampv(p.m_specBreath, -1.0f, 1.0f);
    // Bright tilts the whole model around a kilohertz, the same gesture the additive types make.
    const float tilt = clampv(p.m_bright, 0.0f, 1.0f) - 0.5f;
    const float gain = tex->m_gain;
    const double nyq = 0.45 * m_sr;
    const float invLen = 1.0f / static_cast<float>(n);
    const float invSr = static_cast<float>(1.0 / m_sr);
    int act = 0;
    for (int b = 0; b < B; ++b) {
        const float amp = (a0[b] + frac * (a1[b] - a0[b])) * gain;
        double hzB = static_cast<double>(f0[b] + frac * (f1[b] - f0[b])) * transpose;
        const float tone = clampv(t0[b] + frac * (t1[b] - t0[b]) - breath, 0.0f, 1.0f);
        float g = amp;
        if (g > 0.0f) {
            const float oct = std::log2(static_cast<float>(std::max(20.0, hzB)) / 1000.0f);
            g *= std::pow(2.0f, tilt * oct);          // +-6 dB per octave at the ends of Bright
        }
        if (hzB >= nyq || hzB < 15.0 || !(g > 0.0f)) { g = 0.0f; hzB = 1000.0; }
        m_specAStep[b] = (g * tone - m_specA[b]) * invLen;
        m_specNStep[b] = (g * (1.0f - tone) - m_specN[b]) * invLen;
        // The oscillator's rotation, and the noise band's own filter, both at the same frequency.
        phasorFrom(hzB / m_sr, m_rc[b], m_rs[b]);
        const float r2 = m_pc[b] * m_pc[b] + m_ps[b] * m_ps[b];
        const float fix = 1.5f - 0.5f * r2;           // keep the phasor on the unit circle
        m_pc[b] *= fix; m_ps[b] *= fix;
        m_specF[b] = std::min(1.4f, static_cast<float>(TwoPi * hzB) * invSr);
        m_specQ[b] = 1.0f / m.m_bandQ[b];
        if (g > 1.0e-6f) act = b + 1;
    }
    m_active = act;

    // A state-variable band pass per band, all fed from one white noise: the bands overlap the way
    // the ear's own do, so one source is right -- six-and-thirty independent noises would be six
    // and thirty times the wind.
    for (int i = 0; i < n; ++i) {
        const float white = m_rng.bipolar();
        float sum = 0.0f;
        for (int b = 0; b < B; ++b) {
            m_specA[b] += m_specAStep[b];
            m_specN[b] += m_specNStep[b];
            const float c = m_pc[b] * m_rc[b] - m_ps[b] * m_rs[b];
            const float s = m_pc[b] * m_rs[b] + m_ps[b] * m_rc[b];
            m_pc[b] = c; m_ps[b] = s;
            const float hp = white - m_specLp[b] - m_specQ[b] * m_specBp[b];
            m_specBp[b] += m_specF[b] * hp;
            m_specLp[b] += m_specF[b] * m_specBp[b];
            // The band pass peaks at 1/q, so the noise is divided by it: what comes out is the
            // band's own amplitude, not the filter's.
            sum += m_specA[b] * s + m_specN[b] * m_specBp[b] * m_specQ[b] * 1.6f;
        }
        out[i] += sum;
    }
    for (int b = 0; b < B; ++b) m_amp[b] = m_specA[b] + m_specN[b];   // so the scope shows the bands
}

void SourceSlot::renderBow(float* out, int n, double hz, const SlotParams& p, float dt)
{
    (void)dt;
    const double f0 = hz > 20.0 ? hz : 20.0;
    const double period = m_sr / f0;
    if (period < 4.0 || period >= static_cast<double>(BowMax - 4)) { std::memset(out, 0, sizeof(float) * static_cast<size_t>(n)); return; }
    if (!m_bowReady) {
        std::fill(m_bowNut.begin(), m_bowNut.end(), 0.0f);
        std::fill(m_bowBridge.begin(), m_bowBridge.end(), 0.0f);
        m_bowW = 0; m_bowLp = 0.0f; m_bowReady = true;
        // A breath of noise to start it: a string exactly at rest is a fixed point, and the
        // friction curve alone would never leave it.
        for (int i = 0; i < BowMax; ++i) { m_bowNut[i] = 0.002f * m_rng.bipolar(); m_bowBridge[i] = 0.002f * m_rng.bipolar(); }
    }
    // The friction characteristic is the whole model. It has to FALL as the slipping gets
    // faster -- more slip, less force -- because that negative resistance is what feeds the
    // oscillation; a curve that merely saturates has a stable fixed point, and the string sticks
    // to the bow and stays there. The first version of this did exactly that, and measured as a
    // constant with no pitch at all. The shape below is the one the waveguide literature uses
    // (Smith 2010; the bow table of the Synthesis ToolKit): a steep inverse power of the
    // relative velocity, whose width is set by how hard the bow presses.
    const float slope = 5.0f - 4.0f * clampv(p.m_bowForce, 0.0f, 1.0f);   // heavier hand, wider stick
    const float speed = clampv(p.m_bowSpeed, 0.0f, 1.0f);
    const float vBow = 0.6f * speed;                                    // a bow at rest bows nothing
    // A bow that has stopped is still lying on the string, and hair that does not move absorbs:
    // stop bowing and the note dies in a tenth of a second, it does not ring on like a plucked
    // string. Without this the junction is a lossless termination and the string keeps its energy
    // for seconds, which is what the test measured. The loss is confined to the bottom sixth of
    // the Speed range, so nothing that is being played is touched by it.
    const float rest = std::max(0.0f, 1.0f - 6.0f * speed);
    const float contact = 1.0f - 0.02f * rest * clampv(p.m_bowForce, 0.0f, 1.0f);
    // Where the bow sits, as a fraction of the string, kept away from the ends.
    const float rel = 0.06f + 0.34f * clampv(p.m_position, 0.0f, 1.0f);
    // The bridge's reflection loses the highs, which is what makes a string's upper partials die
    // first. Bright is the slot's own brightness knob, used here for the same thing.
    const float damp = 0.55f - 0.45f * clampv(p.m_bright, 0.0f, 1.0f);
    const float lpCoef = 1.0f - damp;
    // The string is two waveguides that meet at the bow: from the bow to the nut and back, and to
    // the bridge and back, and the two together are one period. Both were whole numbers of
    // samples, and the sum of them came to 2*floor(period/2) -- so the period was rounded down to
    // an EVEN number of samples and the string played sharp: measured +10 cents at C4, +25 at C5,
    // +55 at C6, and unevenly, so the intervals came out wrong rather than merely the pitch. They
    // are fractions now, read with a linear interpolation between two taps, which has exactly the
    // right delay at the fundamental and costs the top of the spectrum a little of what the bridge
    // filter is taking from it anyway. Measured after: within half a cent up to C6.
    // The bridge filter is part of the loop and a one-pole delays -- (1-g)/g samples at the
    // fundamental -- so its share is subtracted from that side. Unsubtracted it made the string
    // flat, and since g IS the Bright knob, the pitch moved with the tone: 17 cents from one end
    // of Bright to the other, measured at A4.
    const double lpDelay = (1.0 - static_cast<double>(lpCoef)) / std::max(static_cast<double>(lpCoef), 1.0e-3);
    const double dNut    = std::max(1.0, static_cast<double>(rel) * period);
    const double dBridge = std::max(1.0, period - dNut - lpDelay);
    const int   nutI = static_cast<int>(dNut),    briI = static_cast<int>(dBridge);
    const float nutF = static_cast<float>(dNut - nutI), briF = static_cast<float>(dBridge - briI);
    for (int i = 0; i < n; ++i) {
        // What arrives at the bow from each side, having been reflected at its end: the nut
        // inverts, the bridge inverts and damps.
        const float n0 = m_bowNut[(m_bowW - nutI + BowMax) & (BowMax - 1)];
        const float n1 = m_bowNut[(m_bowW - nutI - 1 + BowMax) & (BowMax - 1)];
        const float vl = -(n0 + nutF * (n1 - n0));
        const float b0 = m_bowBridge[(m_bowW - briI + BowMax) & (BowMax - 1)];
        const float b1 = m_bowBridge[(m_bowW - briI - 1 + BowMax) & (BowMax - 1)];
        m_bowLp += lpCoef * ((b0 + briF * (b1 - b0)) - m_bowLp);
        const float vr = -0.995f * m_bowLp;
        const float dv = vBow - (vl + vr);
        float rho = std::pow(std::fabs((dv - 0.001f) * slope) + 0.75f, -4.0f);
        if (rho > 1.0f) rho = 1.0f;
        float f = dv * rho;
        if (f > 1.0f) f = 1.0f; else if (f < -1.0f) f = -1.0f;
        float toNut = (vr + f) * contact, toBridge = (vl + f) * contact;
        if (!(toNut > -8.0f && toNut < 8.0f) || !(toBridge > -8.0f && toBridge < 8.0f)) {
            std::fill(m_bowNut.begin(), m_bowNut.end(), 0.0f);
            std::fill(m_bowBridge.begin(), m_bowBridge.end(), 0.0f);
            m_bowLp = 0.0f; toNut = toBridge = 0.0f;
        }
        m_bowNut[m_bowW & (BowMax - 1)] = toNut;
        m_bowBridge[m_bowW & (BowMax - 1)] = toBridge;
        ++m_bowW;
        // What the bridge radiates. The factor is measured, not guessed: with the same note, the
        // same Level and everything after the sources switched off, a wavetable slot renders at
        // an RMS of 0.103, and this brings the string to within a decibel of it. The selftest
        // measures both and fails if they drift more than six decibels apart.
        out[i] += vr * 0.78f;
    }
}

} // namespace ambient
