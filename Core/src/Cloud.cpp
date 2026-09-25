/**
 * @file Cloud.cpp
 * @brief The granular cloud: onsets, grains, the feedback loop and the tuned resonators.
 *
 * Cloud.h says what the cloud is; this is how it runs. process() works in sub-blocks of kSub
 * samples: the input and what the loop handed back one sub-block ago are written into the history,
 * spawnSub() draws the onsets (the Poisson stream, or the Hawkes flocks of Swarm), spawn() places
 * each new grain behind the write head with its length, its rate, the interval drawRatio() gives it
 * and its pan, renderGrains() runs the live grains through the vector kernel of GrainRing.h onto one
 * bus per resonator, runResonators() rings the Band or Comb resonators on those buses, and mixSub()
 * blends dry grains and resonators at Level into the output and sends the same signal through Tone,
 * a DC blocker, the ADAA saturation and the throttle on its own mean level back into the loop's
 * ring -- shifted in the spectrum first when Shift is on. setHarmony() and the two build functions
 * turn the scale and the conductor's root into the interval list the scatter draws from and the
 * notes the resonators sit on; they are called every block but rebuild only when the harmony has
 * actually moved. The constants below are the loop's gain structure, with the measurements that
 * set them.
 */
#include "ambient/Cloud.h"
#include "ambient/Adaa.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ambient {

namespace {

constexpr double kLn1000 = 6.907755278982137;   ///< ln 1000: the fall to -60 dB, for the resonators' ring times
/**
 * @brief What the loop multiplies by at Feedback 1.
 *
 * Each channel of the cloud comes out 11.7 dB below the
 * history it reads: 0.6 over the square root of the overlap, a Hann window's three eighths of the
 * power, and an equal-power pan that gives each side half of every grain. A gain of one would
 * never sustain -- measured, 2.9 lost 2.5 dB a pass and was gone within half a minute. 4.2 puts a
 * full turn of the loop 0.8 dB above unity, which the Tone filter's losses bring back towards one
 * and the ceiling holds.
 */
constexpr float kLoopGain = 4.2f;
constexpr float kLoopCeiling = 0.15f;   ///< the loop's mean level at which its feedback reaches zero
constexpr float kLoopDrive = 1.5f;      ///< into the saturation, and out of it again by the same factor
constexpr float kResGain = 4.0f;        ///< the resonators' makeup against the dry grains

} // namespace

void GrainCloud::prepare(double sampleRate, uint64_t seed)
{
    sr_ = sampleRate;
    rng_.seed(seed);
    // Eight seconds. A grain transposed up reads its history faster than it is written, and an
    // 800 ms grain two octaves up at the far end of Spray needs more than six behind the write head.
    cap_ = static_cast<int>(8.0 * sr_) + 4 * kSub;
    hist_.assign(static_cast<size_t>(2 * cap_), 0.0f);
    w_ = 0; t_ = 0; live_ = 0; started_ = 0; tail_ = 0;
    hazard_ = 0.0; nextHazard_ = 0.0; excite_ = 0.0;
    exciteDecay_ = std::exp(-1.0 / (0.25 * sr_));
    std::memset(fbRingL_, 0, sizeof(fbRingL_));
    std::memset(fbRingR_, 0, sizeof(fbRingR_));
    toneL_ = toneR_ = dcxL_ = dcxR_ = dcyL_ = dcyR_ = 0.0f;
    satL_.reset(); satR_.reset();
    loopEnv_ = 0.0f; fbCur_ = 0.0f;
    dcR_ = 1.0f - static_cast<float>(kTwoPi * 20.0 / sr_);
    envC_ = 1.0f - std::exp(-1.0f / (0.1f * static_cast<float>(sr_)));
    setLoop(feedback_, toneHz_);
    shift_.prepare(sr_);
    shift_.setSemitones(shiftSemis_);
    comb_.assign(static_cast<size_t>(kMaxRes * 2 * kCombLen), 0.0f);
    combW_ = 0;
    combA_ = 1.0f - std::exp(-kTwoPi * 6000.0f / static_cast<float>(sr_));
    combLag_ = (1.0f - combA_) / combA_;   // the one-pole's delay at low frequencies, in samples
    std::memset(bandU_, 0, sizeof(bandU_));
    std::memset(bandV_, 0, sizeof(bandV_));
    std::memset(combLp_, 0, sizeof(combLp_));
    resCur_ = resAmount_;
    levelCur_ = level_;
    if (haveHarmony_) {   // the resonators' angles belong to the sample rate
        buildResonators();
        for (int k = 0; k < numRes_; ++k) resTheta_[k] = resThetaTarget_[k];
    }
}

void GrainCloud::set(float densityPerSec, float sizeMs, float pitch, float spraySec, float level)
{
    density_ = clampv(densityPerSec, 0.1f, 200.0f);
    size_ = clampv(sizeMs, 5.0f, 2000.0f);
    pitch_ = clampv(pitch, 0.0f, 1.0f);
    spray_ = clampv(spraySec, 0.0f, 3.5f);
    level_ = clampv(level, 0.0f, 2.0f);
}

void GrainCloud::setLoop(float feedback, float toneHz)
{
    feedback_ = clampv(feedback, 0.0f, 1.0f);
    toneHz_ = clampv(toneHz, 100.0f, 20000.0f);
    const float hz = std::min(toneHz_, 0.45f * static_cast<float>(sr_));
    toneC_ = 1.0f - std::exp(-kTwoPi * hz / static_cast<float>(sr_));
}

void GrainCloud::setShift(float semitones)
{
    const float s = clampv(semitones, -24.0f, 24.0f);
    if (s == shiftSemis_) return;
    // A shift switched on starts from silence, not from what it heard the last time it ran.
    if (shiftSemis_ == 0.0f) shift_.reset();
    shiftSemis_ = s;
    if (s != 0.0f) shift_.setSemitones(s);
}

void GrainCloud::setScatter(float transposeSemitones, float scatter)
{
    transposeRatio_ = std::pow(2.0, static_cast<double>(clampv(transposeSemitones, -24.0f, 24.0f)) / 12.0);
    scatter_ = clampv(scatter, 0.0f, 1.0f);
}

void GrainCloud::setSwarm(float swarm) { swarm_ = clampv(swarm, 0.0f, 1.0f); }

void GrainCloud::setResonators(float amount, int mode, int notes, float decaySeconds)
{
    resAmount_ = clampv(amount, 0.0f, 1.0f);
    mode = clampv(mode, 0, 1);
    if (mode != resMode_) {   // the other kind starts from silence, not from the last one's state
        resMode_ = mode;
        std::memset(bandU_, 0, sizeof(bandU_));
        std::memset(bandV_, 0, sizeof(bandV_));
        std::memset(combLp_, 0, sizeof(combLp_));
        std::fill(comb_.begin(), comb_.end(), 0.0f);
    }
    resDecay_ = clampv(decaySeconds, 0.05f, 30.0f);
    notes = clampv(notes, 0, 3);
    if (notes != resNotes_) {
        resNotes_ = notes;
        if (haveHarmony_) buildResonators();
    }
}

void GrainCloud::setHarmony(const FixedScale& scale, double tonicHz, double rootHz)
{
    if (!(tonicHz > 1.0) || !(rootHz > 1.0) || !std::isfinite(tonicHz) || !std::isfinite(rootHz)) return;
    const int count = clampv(scale.count, 1, FixedScale::kMax);
    double sig = count * 1000.003 + scale.period;
    for (int k = 0; k < count; ++k) sig += scale.ratios[k] * (k + 1.618);
    if (haveHarmony_ && sig == scaleSig_ && std::fabs(tonicHz / tonicHz_ - 1.0) < 1.0e-7
        && std::fabs(rootHz / rootHz_ - 1.0) < 1.0e-7)
        return;
    scaleSig_ = sig; tonicHz_ = tonicHz; rootHz_ = rootHz; count_ = count;
    period_ = (scale.period > 1.0001 && std::isfinite(scale.period)) ? scale.period : 2.0;
    // A Scala file can carry a degree of zero, which the tuning code answers with a frequency of
    // zero by design; here it would be a logarithm of zero. Such a degree is taken as the tonic.
    for (int k = 0; k < count; ++k)
        ratios_[k] = (scale.ratios[k] > 0.0 && std::isfinite(scale.ratios[k])) ? scale.ratios[k] : 1.0;
    const bool first = !haveHarmony_;
    haveHarmony_ = true;
    buildIntervals();
    buildResonators();
    if (first) for (int k = 0; k < numRes_; ++k) resTheta_[k] = resThetaTarget_[k];
}

void GrainCloud::buildIntervals()
{
    const double P = period_;
    // The conductor's root as a degree of the scale: its ratio to the tonic, folded into one
    // period and matched to the nearest degree -- the end of the period counted as the tonic again.
    double r = rootHz_ / tonicHz_;
    for (int guard = 0; guard < 64 && r >= P; ++guard) r /= P;
    for (int guard = 0; guard < 64 && r < 1.0; ++guard) r *= P;
    int d = 0;
    double best = 1.0e9;
    for (int k = 0; k < count_; ++k) {
        const double e = std::min(std::fabs(std::log(r / ratios_[k])), std::fabs(std::log(r / (ratios_[k] * P))));
        if (e < best) { best = e; d = k; }
    }
    // The scale rotated to begin on that degree: the intervals the root has above it within a period.
    for (int j = 0; j < count_; ++j) {
        const int k = (d + j) % count_;
        double q = ratios_[k] / ratios_[d];
        if (d + j >= count_) q *= P;
        rot_[j] = q;
    }
    // Every interval within an octave either way, the unison left out -- it is what a grain
    // without scatter plays -- ordered from the most consonant outwards, and the nearer of two
    // equally consonant intervals first.
    struct Cand { double v, c, dist; };
    Cand cand[kMaxIv];
    int nc = 0;
    for (int o = -1; o <= 1; ++o) {
        const double po = o < 0 ? 1.0 / P : (o > 0 ? P : 1.0);
        for (int j = 0; j < count_ && nc < kMaxIv; ++j) {
            if (o == 0 && j == 0) continue;
            const double v = rot_[j] * po;
            if (v < 0.499 || v > 2.001) continue;
            cand[nc++] = { v, intervalConsonance(v), std::fabs(std::log2(v)) };
        }
    }
    std::stable_sort(cand, cand + nc, [](const Cand& a, const Cand& b) {
        return a.c != b.c ? a.c > b.c : a.dist < b.dist;
    });
    numIv_ = nc;
    for (int k = 0; k < nc; ++k) iv_[k] = static_cast<float>(cand[k].v);
}

void GrainCloud::buildResonators()
{
    const double P = period_;
    // The two octaves above C3: low enough to be a chord under the cloud, high enough that a grain
    // of a hundred milliseconds holds a dozen periods of the lowest note.
    double base = rootHz_;
    for (int guard = 0; guard < 64 && base >= 261.6255653; ++guard) base *= 0.5;
    for (int guard = 0; guard < 64 && base < 130.8127827; ++guard) base *= 2.0;
    // The interval of the scale nearest to a or to b, measured from the root.
    const auto nearest = [&](double a, double b) {
        if (count_ < 2) return a;
        int bj = 1;
        double be = 1.0e9;
        for (int j = 1; j < count_; ++j) {
            const double e = std::min(std::fabs(std::log(rot_[j] / a)), std::fabs(std::log(rot_[j] / b)));
            if (e < be) { be = e; bj = j; }
        }
        return rot_[bj];
    };
    double hz[kMaxRes];
    int n = 0;
    switch (resNotes_) {
    case 0: {   // the scale: every degree in both octaves, the least consonant left out beyond twelve
        struct Note { double hz, c; };
        Note list[2 * FixedScale::kMax];
        int nl = 0;
        for (int o = 0; o < 2; ++o)
            for (int j = 0; j < count_; ++j)
                list[nl++] = { base * rot_[j] * (o != 0 ? P : 1.0), intervalConsonance(rot_[j]) };
        std::stable_sort(list, list + nl, [](const Note& a, const Note& b) { return a.c > b.c; });
        n = std::min(nl, static_cast<int>(kMaxRes));
        std::sort(list, list + n, [](const Note& a, const Note& b) { return a.hz < b.hz; });
        for (int k = 0; k < n; ++k) hz[k] = list[k].hz;
        break;
    }
    case 1: {   // the chord on the root: the third the scale has nearer (major or minor) and the fifth
        const double third = nearest(1.25, 1.2), fifth = nearest(1.5, 1.5);
        const double c[6] = { 1.0, third, fifth, 2.0, 2.0 * third, 2.0 * fifth };
        for (double v : c) hz[n++] = base * v;
        break;
    }
    case 2: {
        const double fifth = nearest(1.5, 1.5);
        const double c[4] = { 1.0, fifth, 2.0, 2.0 * fifth };
        for (double v : c) hz[n++] = base * v;
        break;
    }
    default: {
        const double c[3] = { 1.0, 2.0, 4.0 };
        for (double v : c) hz[n++] = base * v;
        break;
    }
    }
    const double top = 0.4 * sr_;
    for (int k = 0; k < n; ++k) {
        const double f = std::min(hz[k], top);
        resHz_[k] = static_cast<float>(f);
        resThetaTarget_[k] = static_cast<float>(2.0 * 3.141592653589793 * f / sr_);
    }
    for (int k = numRes_; k < n; ++k) resTheta_[k] = resThetaTarget_[k];   // a new resonator starts on its note
    numRes_ = n;
}

double GrainCloud::drawRatio()
{
    if (scatter_ <= 0.0f || numIv_ == 0) return 1.0;
    const float s = scatter_;
    double ratio = 1.0;
    // Up to a quarter, more and more grains leave the unison; up to a half, the intervals they may
    // take open from the top of the list -- the most consonant -- until every one of them is in.
    if (rng_.uniform() < std::min(1.0f, 4.0f * s)) {
        const int want = static_cast<int>(std::ceil(std::min(1.0f, 2.0f * s) * static_cast<float>(numIv_)));
        const int open = std::clamp(want, std::min(2, numIv_), numIv_);
        ratio = static_cast<double>(iv_[rng_.below(open)]);
    }
    // Beyond a half the grains begin to leave the scale: a growing share of them is bent by up to
    // an octave either way, and at 1 every one of them is.
    if (s > 0.5f) {
        const float q = 2.0f * (s - 0.5f);
        if (rng_.uniform() < q) ratio *= std::pow(2.0, static_cast<double>(q * rng_.bipolar()));
    }
    return ratio;
}

void GrainCloud::spawn(int w0, int offset)
{
    if (live_ >= kMaxGrains) return;
    static const double kRates[4] = { 2.0, 0.5, 1.5, 4.0 };
    double len = static_cast<double>(size_) * (0.7 + 0.6 * static_cast<double>(rng_.uniform())) * sr_ / 1000.0;
    double rate = 1.0 + 0.02 * static_cast<double>(rng_.bipolar());
    if (rng_.uniform() < pitch_) rate = kRates[rng_.below(rng_.uniform() < 0.7f ? 2 : 4)];
    rate *= transposeRatio_ * drawRatio();
    rate = std::clamp(rate, 0.125, 8.0);
    const double spray = static_cast<double>(spray_) * static_cast<double>(rng_.uniform()) * sr_;
    // A grain may never read what has not been written yet: it starts far enough behind the write
    // head that it is still behind it when it ends, even read faster than the head moves. Where the
    // history is not long enough for that, the grain is shortened rather than dropped.
    const double limit = static_cast<double>(cap_) - 2.0 * kSub - 16.0;
    const double speed = std::max(rate, 1.0);
    double behind = len * speed + spray + 4.0;
    if (behind > limit) {
        len = (limit - spray - 4.0) / speed;
        if (len < 32.0) return;
        behind = len * speed + spray + 4.0;
    }
    const float overlap = std::max(density_ * size_ * 0.001f, 1.0f);
    const float gain = 0.6f / std::sqrt(overlap);   // roughly constant loudness
    const float pan = rng_.bipolar();
    const float angle = (pan + 1.0f) * 0.25f * kPi;
    RingGrain& g = grains_[live_++];
    double pos = static_cast<double>(w0 + offset) - behind;
    while (pos < 0.0) pos += cap_;
    while (pos >= cap_) pos -= cap_;
    g.pos = pos;
    g.rate = rate;
    g.len = static_cast<int>(len);
    g.age = 0;
    g.start = offset;
    g.gl = std::cos(angle) * gain;
    g.gr = std::sin(angle) * gain;
    g.wc = 1.0f; g.ws = 0.0f;
    phasorFrom(1.0 / len, g.rc, g.rs);
    g.bus = (routing_ && numRes_ > 1) ? rng_.below(numRes_) : 0;
    ++started_;
}

void GrainCloud::spawnSub(int w0, int m)
{
    // Onsets by time rescaling: the hazard -- the intensity integrated over time -- is compared
    // against an exponential draw, which at a constant intensity is exactly the Poisson stream the
    // cloud always had. The intensity is the base rate plus an excitation that every onset raises
    // and that decays with a quarter of a second (Hawkes 1971). The base rate gives up the share
    // of onsets the excitation makes, so the mean density stays what the knob says.
    const double nstar = 0.9 * static_cast<double>(swarm_);   // the branching ratio: offspring per grain
    const double mu = static_cast<double>(density_) * (1.0 - nstar);
    const double jump = nstar / 0.25;                         // so the kernel integrates to n*
    const double dt = 1.0 / sr_;
    for (int i = 0; i < m; ++i) {
        hazard_ += (mu + excite_) * dt;
        excite_ *= exciteDecay_;
        if (hazard_ >= nextHazard_) {
            hazard_ = 0.0;
            nextHazard_ = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1.0e-12);
            spawn(w0, i);
            excite_ += jump;
        }
    }
}

void GrainCloud::renderGrains(int m)
{
    const float* h = hist_.data();
    for (int c = 0; c < live_; ) {
        RingGrain& g = grains_[c];
        {   // keep the window phasor on the unit circle (a grain can run for 96000 samples)
            const float r2 = g.wc * g.wc + g.ws * g.ws, fix = 1.5f - 0.5f * r2;
            g.wc *= fix; g.ws *= fix;
        }
        const int begin = g.start < m ? g.start : m;
        g.start = 0;
        const int count = std::min(m - begin, g.len - g.age);
        if (count > 0) {
            const int b = routing_ ? (g.bus < numRes_ ? g.bus : g.bus % numRes_) : 0;
            renderRingGrain(h, cap_, 2, g, busL_[b] + begin, busR_[b] + begin, count);
            g.age += count;
        }
        // Dead: the last live grain takes its place, and `c` stays where it is, because what now
        // sits there has not been rendered yet.
        if (g.age >= g.len) grains_[c] = grains_[--live_];
        else ++c;
    }
}

void GrainCloud::runResonators(int m, float* wetL, float* wetR)
{
    const float glide = 1.0f - std::exp(-static_cast<float>(m) / (0.12f * static_cast<float>(sr_)));
    if (resMode_ == 0) {
        // Mathews and Smith's phasor filter: the state is a point in the plane, turned by the
        // note's angle and shrunk by the decay on every sample, with the input added to its real
        // part. A rotation keeps the point's distance whatever the angle is, so a resonator whose
        // note glides keeps its level. The imaginary part, scaled so the response peaks at one.
        const float rho = static_cast<float>(std::exp(-kLn1000 / (static_cast<double>(resDecay_) * sr_)));
        const float norm = 2.0f * (1.0f - rho) / rho;
        for (int k = 0; k < numRes_; ++k) {
            resTheta_[k] += (resThetaTarget_[k] - resTheta_[k]) * glide;
            const float c = rho * std::cos(resTheta_[k]), s = rho * std::sin(resTheta_[k]);
            const float* xL = busL_[k];
            const float* xR = busR_[k];
            float uL = bandU_[k][0], vL = bandV_[k][0], uR = bandU_[k][1], vR = bandV_[k][1];
            for (int i = 0; i < m; ++i) {
                const float nuL = xL[i] + c * uL - s * vL;
                vL = s * uL + c * vL;
                uL = nuL;
                const float nuR = xR[i] + c * uR - s * vR;
                vR = s * uR + c * vR;
                uR = nuR;
                wetL[i] += vL * norm;
                wetR[i] += vR * norm;
            }
            bandU_[k][0] = uL; bandV_[k][0] = vL; bandU_[k][1] = uR; bandV_[k][1] = vR;
        }
    } else {
        // A feedback comb per note. The loop's delay is the note's period less the lag of the
        // one-pole that darkens it, read between two samples; its gain is what loses 60 dB in the
        // ring time; the output is scaled so the comb's teeth peak at one.
        constexpr int mask = kCombLen - 1;
        for (int k = 0; k < numRes_; ++k) {
            resTheta_[k] += (resThetaTarget_[k] - resTheta_[k]) * glide;
            const float period = 6.2831853f / std::max(resTheta_[k], 1.0e-4f);
            const float delay = clampv(period - combLag_, 2.0f, static_cast<float>(kCombLen - 3));
            const int di = static_cast<int>(delay);
            const float fr = delay - static_cast<float>(di);
            const float gain = static_cast<float>(std::exp(-kLn1000 * static_cast<double>(delay) / (static_cast<double>(resDecay_) * sr_)));
            const float norm = 1.0f - gain;
            float* bL = comb_.data() + static_cast<size_t>(k) * 2 * kCombLen;
            float* bR = bL + kCombLen;
            float lpL = combLp_[k][0], lpR = combLp_[k][1];
            int w = combW_;
            for (int i = 0; i < m; ++i) {
                const int r0 = (w - di) & mask, r1 = (w - di - 1) & mask;
                const float dL = bL[r0] + fr * (bL[r1] - bL[r0]);
                const float dR = bR[r0] + fr * (bR[r1] - bR[r0]);
                lpL += combA_ * (dL - lpL);
                lpR += combA_ * (dR - lpR);
                const float yL = busL_[k][i] + gain * lpL;
                const float yR = busR_[k][i] + gain * lpR;
                bL[w] = yL;
                bR[w] = yR;
                wetL[i] += yL * norm;
                wetR[i] += yR * norm;
                w = (w + 1) & mask;
            }
            combLp_[k][0] = lpL; combLp_[k][1] = lpR;
        }
        combW_ = (combW_ + m) & mask;
    }
}

void GrainCloud::mixSub(int m, float* outL, float* outR)
{
    float dryL[kSub], dryR[kSub];
    std::memcpy(dryL, busL_[0], sizeof(float) * static_cast<size_t>(m));
    std::memcpy(dryR, busR_[0], sizeof(float) * static_cast<size_t>(m));
    if (routing_)
        for (int k = 1; k < numRes_; ++k)
            for (int i = 0; i < m; ++i) { dryL[i] += busL_[k][i]; dryR[i] += busR_[k][i]; }
    float wetL[kSub] = {}, wetR[kSub] = {};
    const bool resOn = numRes_ > 0 && (resAmount_ > 0.0f || resCur_ > 1.0e-4f);
    if (resOn) runResonators(m, wetL, wetR);

    // The three amounts glide across the sub-block rather than stepping at its edge.
    const float sm = 1.0f - std::exp(-static_cast<float>(m) / (0.03f * static_cast<float>(sr_)));
    const float res0 = resCur_, res1 = resCur_ + (resAmount_ - resCur_) * sm;
    const float lev0 = levelCur_, lev1 = levelCur_ + (level_ - levelCur_) * sm;
    const float fbTarget = feedback_ * kLoopGain;
    const float fb0 = fbCur_, fb1 = fbCur_ + (fbTarget - fbCur_) * sm;
    resCur_ = res1; levelCur_ = lev1; fbCur_ = fb1;
    const bool loop = fb0 > 1.0e-6f || fb1 > 1.0e-6f;
    const float inv = 1.0f / static_cast<float>(m);
    float peak = 0.0f;
    float loopL[kSub], loopR[kSub], loopG[kSub];
    for (int i = 0; i < m; ++i) {
        const float a = static_cast<float>(i + 1) * inv;
        const float res = res0 + (res1 - res0) * a;
        const float lev = lev0 + (lev1 - lev0) * a;
        const float yL = dryL[i] + (wetL[i] * kResGain - dryL[i]) * res;
        const float yR = dryR[i] + (wetR[i] * kResGain - dryR[i]) * res;
        outL[i] += yL * lev;
        outR[i] += yR * lev;
        peak = std::max(peak, std::fabs(yL) + std::fabs(yR));
        if (loop) {
            // Tone, a DC blocker (a transposed grain can carry an offset and a loop would stack
            // it), the saturation, and the throttle on the loop's own mean level.
            toneL_ += toneC_ * (yL - toneL_);
            toneR_ += toneC_ * (yR - toneR_);
            const float hL = toneL_ - dcxL_ + dcR_ * dcyL_;
            dcxL_ = toneL_; dcyL_ = hL;
            const float hR = toneR_ - dcxR_ + dcR_ * dcyR_;
            dcxR_ = toneR_; dcyR_ = hR;
            loopL[i] = satL_(hL * kLoopDrive) / kLoopDrive;
            loopR[i] = satR_(hR * kLoopDrive) / kLoopDrive;
            loopEnv_ += envC_ * (0.5f * (std::fabs(loopL[i]) + std::fabs(loopR[i])) - loopEnv_);
            const float reg = clampv((kLoopCeiling - loopEnv_) / kLoopCeiling, 0.0f, 1.0f);
            loopG[i] = (fb0 + (fb1 - fb0) * a) * reg;
        }
    }
    if (loop) {
        // Shifted on the way round: every pass of the loop an interval further up or down.
        if (shiftSemis_ != 0.0f) shift_.process(loopL, loopR, loopL, loopR, m);
        for (int i = 0; i < m; ++i) {
            const int fi = static_cast<int>((t_ + i) & (kFbLen - 1));
            fbRingL_[fi] = loopL[i] * loopG[i];
            fbRingR_[fi] = loopR[i] * loopG[i];
        }
    } else {
        for (int i = 0; i < m; ++i) {
            const int fi = static_cast<int>((t_ + i) & (kFbLen - 1));
            fbRingL_[fi] = 0.0f;
            fbRingR_[fi] = 0.0f;
        }
        toneL_ = toneR_ = dcxL_ = dcxR_ = dcyL_ = dcyR_ = 0.0f;
        satL_.reset(); satR_.reset();
        loopEnv_ = 0.0f;
    }
    tail_ = peak > 1.0e-7f ? static_cast<int>(0.25 * sr_) : std::max(0, tail_ - m);
}

void GrainCloud::process(const float* inL, const float* inR, float* outL, float* outR, int n)
{
    float* h = hist_.data();
    for (int p = 0; p < n; ) {
        const int m = std::min(kSub, n - p);
        const int w0 = w_;
        // The history: the input, and what the loop handed over one sub-block ago.
        const long long back = t_ - kSub;
        for (int i = 0; i < m; ++i) {
            const int fi = static_cast<int>((back + i) & (kFbLen - 1));
            h[2 * w_]     = inL[p + i] + fbRingL_[fi];
            h[2 * w_ + 1] = inR[p + i] + fbRingR_[fi];
            if (++w_ == cap_) w_ = 0;
        }
        routing_ = numRes_ > 1 && (resAmount_ > 0.0f || resCur_ > 1.0e-4f);
        spawnSub(w0, m);
        const int buses = routing_ ? numRes_ : 1;
        for (int k = 0; k < buses; ++k) {
            std::memset(busL_[k], 0, sizeof(float) * static_cast<size_t>(m));
            std::memset(busR_[k], 0, sizeof(float) * static_cast<size_t>(m));
        }
        renderGrains(m);
        mixSub(m, outL + p, outR + p);
        t_ += m;
        p += m;
    }
}

} // namespace ambient
