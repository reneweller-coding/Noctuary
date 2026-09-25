/**
 * @file Near.h
 * @brief The near events: what the instrument plays close to the ear, on a clock of its
 *        own (13.09.2026).
 *
 * The conductor makes the background; nothing in the instrument made a foreground except the
 * Strike, which decorates a note the conductor was playing anyway. This is a second, small
 * conductor for the foreground: every so often -- the gap drawn the way the conductor draws its
 * own, a shifted exponential so that there is never a pulse -- it plays one thing near, from the
 * Near Source (a slot of its own, Engine.h), and then waits. Three kinds of thing:
 *
 *   Note      one tone, held for Length, with the source's own attack and release.
 *   Phrase    one tone that slides to a second degree part-way, on the voice's portamento with
 *             its consonance gravity -- the singing string, the flute bending into its note.
 *   Sequence  a Berlin-school line: a ring of Steps notes on a clock of eighths, transposed
 *             with the conductor's root, mutating one step at a time (the Turing machine's shift
 *             register), ghost notes and far accents scattered through it, the filter breathing
 *             open and shut over its run -- and it arrives out of the far plane and leaves into it.
 *
 * What it plays is chosen against the harmony, never by a die alone: Consonant takes a degree of
 * the tuning that is consonant with the root AND with every note sounding -- the fifth, the ninth,
 * the pure third first -- and avoids what already sounds. The rules it keeps, all of them measured
 * by the selftest and the audit: never during the conductor's planned silence, never within six
 * seconds of a root change, never two events at once, never into nothing at all; and while a Note
 * or a Phrase sounds the conductor may be asked to hold its decisions (Hold Brain), so the
 * background stands still under the soloist.
 *
 * Nothing here renders: it emits NearNotes, and the engine turns them into voices (or the audit
 * into lines). Its dice are its own stream, so switching it on moves nothing else in a preset.
 */
#pragma once
#include "Dsp.h"
#include "Clock.h"
#include <cmath>
#include <cstdint>

namespace ambient {

/** @brief The Near Events section's settings, as the engine reads them from the parameter table once per block. */
struct NearParams {
    float level = 0.0f;          ///< 0: off
    int   kind = 0;              ///< 0 Note, 1 Phrase, 2 Sequence
    float rate = 120.0f;         ///< mean seconds from the end of one event to the start of the next
    float chance = 1.0f;         ///< the coin at each due moment
    float cluster = 0.0f;        ///< the coin weighted by the conductor's excitation
    float length = 6.0f;         ///< seconds an event lasts; a sequence's whole run
    int   pitch = 0;             ///< 0 Consonant, 1 Highest, 2 Lowest, 3 Root, 4 Cluster
    float spread = 0.5f;         ///< how far from the centre an event may sit
    float approach = 0.0f;       ///< fraction of the event spent arriving from the far plane (and leaving into it); negative: leaving from the start
    float distance = 0.0f;       ///< where the event sits, 0 at the ear .. 1 on the horizon (what Approach arrives at)
    float dry = 0.0f;            ///< the share of the event's voice that goes past every reverb and delay
    float toDelay2 = 0.0f;       ///< an extra share into the second delay's input (a send, added, not taken away)
    float toCosmos = 0.0f;       ///< ... and into the Cosmos send, likewise
    float proximity = 0.6f;      ///< the near field's low lift on the event's voice
    bool  hold = true;           ///< the conductor waits while a Note or a Phrase sounds
    float glide = 0.0f;          ///< Phrase: seconds of the slide (0: two fifths of the length)
    int   steps = 7;             ///< Sequence: the ring
    float stepSeconds = 0.4f;    ///< Sequence: a step, when not synced
    int   stepSync = 0;          ///< Sequence: kSyncDivNames index, 0 = free
    float mutation = 0.12f;      ///< Sequence: chance per cycle that one step changes
    float scatter = 0.3f;        ///< Sequence: ghost notes between the steps, accents sent far
    float bloom = 0.5f;          ///< Sequence: the filter opening over the run and closing again
    float attack = 0.3f,    ///< @brief the source's attack in seconds
          release = 2.0f;   ///< the source's, for the gates
};

/** @brief One thing the engine should do. */
struct NearNote {
    /** @brief What to do: start a note, end it, slide it to another degree, or move its plane. */
    enum class Type {
        On,      ///< @brief start `note` at `velocity` on plane `distance`, panned `pan`, shaped by releaseMul and cutoffMul
        Off,     ///< @brief let `note` go
        Glide,   ///< @brief slide the sounding note to `note` (plus `detune`) over `seconds`
        Move     ///< @brief move the sounding note's plane to `distance` over `seconds`
    };
    Type  type = Type::On;       ///< which of the four
    int   note = 60;             ///< the MIDI note it concerns (On, Off: the note; Glide: the destination; Move: the sounding note)
    float velocity = 0.7f;       ///< On: 0 .. 1
    float distance = 0.0f;       ///< On: the plane the note starts on; Move: where to go
    float pan = 0.0f;            ///< On: -1 .. 1
    float releaseMul = 1.0f;     ///< On: the gate, as a share of the source's release
    float cutoffMul = 1.0f;      ///< On: the bloom, a factor on the near filter's cutoff
    float seconds = 0.0f;        ///< Glide, Move: how long
    float detune = 0.0f;         ///< Glide: semitones off the note (a flute's meri bend, a quarter tone down)
};

/** @brief What the scheduler is told about the piece at each step. */
struct NearInputs {
    int    root = 50;            ///< the conductor's root, as a MIDI note
    int    cluster[12] = {};     ///< the conductor's sounding notes
    int    count = 0;            ///< how many of cluster are sounding
    double excitation = 0.0;     ///< the cascade's, in multiples of the base rate
    bool   silence = false;      ///< the conductor's planned pause
    double rootAge = 1.0e9;      ///< seconds since the root last moved
    double bpm = 90.0;           ///< the clock's tempo, for a synced step
    double sinceOnset = 1.0e9;   ///< seconds since the conductor last began a note
    bool   releasing = false;    ///< a conductor's voice is letting go right now
};

/**
 * @brief The foreground's scheduler: decides when an event is due, what it plays and how it
 *        unfolds, and emits NearNotes for the engine to carry out.
 *
 * Header-only and templated on its callbacks, so the engine hands it its tuning and its ear and the
 * audit hands it a logger. Control rate, audio thread; no allocation.
 */
class NearEvents {
public:
    static constexpr int kMaxSteps = 16;   ///< the longest sequence ring

    /**
     * @brief Puts the scheduler at the beginning of a piece: nothing sounding, counters at zero, the first event
     *        due in twenty to forty seconds, and the dice seeded.
     * @param seed  the scheduler's own random stream (part of the preset's seed)
     */
    void reset(uint64_t seed)
    {
        rng_.seed(seed);
        active_ = false; kind_ = 0; elapsed_ = 0.0; length_ = 0.0;
        timer_ = 20.0 + 20.0 * static_cast<double>(rng_.uniform());   // the first, not at once
        note_ = -1; lastNote_ = -1; glided_ = false; moved_ = false;
        events_ = 0; refused_ = 0; waited_ = 0.0; holdTail_ = 0.0;
        excAvg_ = 0.0;
        stepsUsed_ = 0; stepPos_ = 0; stepLeft_ = 0.0; ghostDue_ = false; mutations_ = 0;
        for (auto& s : steps_) s = Step{};
        gateLeft_ = 0.0; gateNote_ = -1;
        phraseNotes_ = 0; phraseDone_ = 0; firstNote_ = -1; bent_ = 0;
        tempoDrift_.init(rng_); timingDrift_.init(rng_); densityPhase_ = static_cast<double>(rng_.uniform());
    }

    /** @return whether an event is sounding right now */
    bool  active() const   { return active_; }
    /**
     * @brief The conductor is asked to begin no new note: while a Note or a Phrase sounds, and for ten
     *        seconds after it, so the horizon's return is heard before the background moves again.
     * @return true while the conductor should hold its decisions (only with Hold Brain on)
     */
    bool  holding() const  { return hold_ && ((active_ && kind_ != 2) || holdTail_ > 0.0); }
    /**
     * @brief A sequence is running: the conductor keeps its root and halves its pace under it.
     * @return true while a Sequence event is active
     */
    bool  sequenceRunning() const { return active_ && kind_ == 2; }
    /** @return how far through the current event, 0 .. 1 (0 when none) */
    float progress() const { return active_ && length_ > 0.0 ? static_cast<float>(elapsed_ / length_) : 0.0f; }
    /** @return events begun since reset() */
    int   events() const   { return events_; }
    /** @return due moments that were refused by a rule or the coin since reset() */
    int   refused() const  { return refused_; }
    /** @return sequence steps mutated since reset() */
    int   mutations() const { return mutations_; }
    /** @return the kind of the current (or last) event: 0 Note, 1 Phrase, 2 Sequence */
    int   kind() const     { return kind_; }
    /** @return seconds until the next event is due (may be negative while waiting for stillness) */
    double nextIn() const  { return timer_; }
    /** @return the MIDI note sounding (a Note or Phrase's, or a sequence's last step), -1 for none */
    int   currentNote() const { return note_; }

    /**
     * @brief Advance `dt`. `freqOf(int) -> double`, `consonance(double fa, double fb) -> double` (the
     *        conductor's own ear), `emit(const NearNote&)`.
     *
     * Called once per control block. With Level at 0 an event still sounding is ended and nothing
     * new is begun. Otherwise a running event is advanced (run()); when none is running the timer
     * counts down and, once due, the rules are asked (no silence, six seconds since the root moved,
     * something sounding), stillness is waited for (up to half the rate), and the coin, weighted by
     * the cascade's excitation against its own average, decides whether begin() is called.
     *
     * @tparam FreqFn      callable as double(int midiNote): the instrument's tuning
     * @tparam ConsFn      callable as double(double fa, double fb): consonance of two frequencies, 0 .. 1
     * @tparam EmitFn      callable as void(const NearNote&)
     * @param dt           seconds since the last call
     * @param p            the section's settings
     * @param in           what the piece is doing right now
     * @param freqOf       the tuning
     * @param consonance   the ear
     * @param emit         receives every NearNote
     */
    template <class FreqFn, class ConsFn, class EmitFn>
    void update(double dt, const NearParams& p, const NearInputs& in, FreqFn&& freqOf, ConsFn&& consonance, EmitFn&& emit)
    {
        hold_ = p.hold;
        rate_ = std::max(10.0, static_cast<double>(p.rate));
        stepSec_ = syncOn(p.stepSync) ? std::max(0.05, syncSeconds(p.stepSync, in.bpm)) : std::max(0.05, static_cast<double>(p.stepSeconds));
        if (p.level <= 0.0f) {
            if (active_) end(emit);     // switched off mid-event: let the note go
            timer_ = std::max(timer_, 5.0);
            return;
        }
        if (holdTail_ > 0.0) holdTail_ -= dt;
        if (active_) { run(dt, p, in, freqOf, consonance, emit); return; }
        timer_ = std::min(timer_, rate_);   // the first wait is never longer than the rate itself
        timer_ -= dt;
        if (timer_ > 0.0) return;
        // Due. The rules first: not in a silence, not just after a root change, not into nothing.
        if (in.silence || in.rootAge < 6.0 || in.count <= 0) { timer_ = 2.0; ++refused_; return; }
        // And into stillness: the near bus has been steady for four seconds -- no onset of the
        // conductor's, no voice letting go -- which is what makes the event an answer and not an
        // interruption. Waited for at most half the rate, then the event goes anyway.
        if ((in.sinceOnset < 4.0 || in.releasing) && waited_ < 0.5 * rate_) { waited_ += dt; return; }
        waited_ = 0.0;
        // The coin, weighted by the cascade against its own average, as the strike's is.
        excAvg_ += 0.1 * (in.excitation - excAvg_);
        const double lift = 1.0 + 2.0 * static_cast<double>(p.cluster) * (in.excitation - excAvg_);
        const double chance = clampv(static_cast<double>(p.chance) * lift, 0.0, 1.0);
        if (chance < 0.999 && rng_.uniform() >= chance) { timer_ = gap(0.3 * p.rate); ++refused_; return; }
        begin(p, in, freqOf, consonance, emit);
    }

private:
    /** @brief One step of the sequence ring. */
    struct Step {
        int offset = 7;          ///< @brief semitones over the root
        int octave = 0;          ///< @brief 0, or 12 for a step an octave up
        float velocity = 0.7f;   ///< @brief 0.55 .. 1
        bool open = true;        ///< @brief a long gate (half a step) rather than a short one
        bool accent = false;     ///< @brief sent far: played on the horizon
        bool rest = false;       ///< @brief a silent step
    };

    /**
     * @brief The gap the conductor draws: exponential, shifted so the floor is a floor and not a peak.
     * @param mean  the mean gap in seconds
     * @return      a gap of at least a fifth of the mean (capped at five seconds), exponentially distributed above it
     */
    double gap(double mean) const
    {
        const double floor = std::min(5.0, 0.2 * mean);
        const double u = -std::log(1.0 - static_cast<double>(rng_.uniform()) + 1e-9);
        return floor + u * std::max(mean - floor, 0.25 * mean);
    }
    mutable Rng rng_;   ///< the scheduler's own dice (mutable so gap() can draw from a const method)

    /**
     * @brief How welcome an interval to the root is, by the ear of this music: the fifth first, then
     *        the ninth and the pure third, the octave, the sixth and the fourth, the harmonic seventh.
     * @param ratio  the frequency ratio to the root, folded into one octave here
     * @return       a weight 0.12 .. 1 (0.12 for an interval within 25 cents of none of the table's)
     */
    static float intervalWeight(double ratio)
    {
        while (ratio >= 2.0) ratio *= 0.5;
        while (ratio < 1.0) ratio *= 2.0;
        const double cents = 1200.0 * std::log2(ratio);
        struct W { double c; float w; };
        static const W table[] = { { 702.0, 1.0f }, { 204.0, 0.85f }, { 386.0, 0.8f }, { 0.0, 0.6f }, { 1200.0, 0.6f },
                                   { 884.0, 0.5f }, { 498.0, 0.5f }, { 969.0, 0.45f }, { 316.0, 0.4f }, { 1088.0, 0.2f } };
        for (const W& w : table) if (std::fabs(cents - w.c) < 25.0) return w.w;
        return 0.12f;
    }

    /**
     * @brief A degree for the foreground, against the root and everything sounding.
     *
     * The fixed rules (Highest, Lowest, Root, Cluster) answer at once. Consonant weighs every key in
     * a window from a fifth under the cluster's middle to a twelfth over its top by intervalWeight()
     * to the root, by the consonance with each sounding note (a doubling and anything inside a
     * critical band of a sounding voice are all but excluded), and by the register (a fifth above
     * the top is favoured, inside the chord discouraged), then draws by weight.
     *
     * @tparam FreqFn     the tuning (see update)
     * @tparam ConsFn     the ear (see update)
     * @param p           the settings (pitch rule)
     * @param in          the root and the cluster
     * @param freqOf      the tuning
     * @param consonance  the ear
     * @param avoid       a MIDI note to weight down (the previous note), or -1
     * @return            the MIDI note chosen (the cluster's middle when nothing has weight)
     */
    template <class FreqFn, class ConsFn>
    int choose(const NearParams& p, const NearInputs& in, FreqFn&& freqOf, ConsFn&& consonance, int avoid)
    {
        int lo = 127, hi = 0;
        for (int i = 0; i < in.count; ++i) { lo = std::min(lo, in.cluster[i]); hi = std::max(hi, in.cluster[i]); }
        if (in.count <= 0) { lo = hi = in.root + 12; }
        switch (p.pitch) {
        case 1: return hi;
        case 2: return lo;
        case 3: return in.root;
        case 4: return in.cluster[rng_.below(std::max(1, in.count))];
        default: break;
        }
        // The window: from a fifth under the cluster's middle to a twelfth over its top, so the
        // register a soloist wants -- above the body -- is in it.
        const int centre = (lo + hi) / 2;
        const int from = clampv(centre - 7, 24, 108), to = clampv(hi + 19, 24, 108);
        const double fRoot = freqOf(in.root);
        float weights[128] = {};
        double seen[128]; int nSeen = 0;
        float total = 0.0f;
        for (int c = from; c <= to; ++c) {
            const double f = freqOf(c);
            bool dup = false;   // a tuning with fewer than twelve degrees maps two keys to one note
            for (int k = 0; k < nSeen; ++k) if (std::fabs(1200.0 * std::log2(f / seen[k])) < 5.0) { dup = true; break; }
            if (dup) continue;
            seen[nSeen++] = f;
            float w = intervalWeight(f / fRoot);
            int nearest = 128;
            for (int i = 0; i < in.count; ++i) {
                const double fk = freqOf(in.cluster[i]);
                w *= 0.35f + 0.65f * static_cast<float>(consonance(f, fk));
                if (std::fabs(1200.0 * std::log2(f / fk)) < 5.0) w *= 0.3f;   // already sounding: a doubling, not a voice
                // Never inside a critical band of a sounding voice (Glasberg and Moore's ERB):
                // the drone would mask the flute, and the event would be paid for and not heard.
                const double erb = 24.7 * (0.00437 * 0.5 * (f + fk) + 1.0);
                if (std::fabs(f - fk) < erb && std::fabs(f - fk) > 1.0e-6) w *= 0.05f;
                nearest = std::min(nearest, std::abs(c - in.cluster[i]));
            }
            // The register: a soloist stands at least a fifth above the highest body voice, or in
            // a gap of a major third or more; inside the chord it is one more voice of the chord.
            if (c >= hi + 7) w *= 1.4f;
            else if (nearest < 4) w *= 0.35f;
            if (c == avoid) w *= 0.4f;
            weights[c] = w; total += w;
        }
        if (total <= 0.0f) return centre;
        float r = rng_.uniform() * total;
        for (int c = from; c <= to; ++c) { r -= weights[c]; if (r <= 0.0f && weights[c] > 0.0f) return c; }
        return centre;
    }

    /**
     * @brief Begins an event of the kind the settings say.
     *
     * A Sequence fills its ring (every step a degree chosen against the harmony, some an octave
     * up, a few rests, most open, a handful accented) and lets runSequence() play it. A Note or a
     * Phrase chooses its note, decides how many slides a phrase gets (two to four, the last home)
     * and emits the On -- on the horizon when it is to arrive -- followed by the Move that brings it
     * to its place (or, with a negative approach, sends it out from the first moment).
     *
     * @tparam FreqFn     the tuning (see update)
     * @tparam ConsFn     the ear (see update)
     * @tparam EmitFn     the sink (see update)
     * @param p           the settings
     * @param in          the root and the cluster
     * @param freqOf      the tuning
     * @param consonance  the ear
     * @param emit        receives the NearNotes
     */
    template <class FreqFn, class ConsFn, class EmitFn>
    void begin(const NearParams& p, const NearInputs& in, FreqFn&& freqOf, ConsFn&& consonance, EmitFn&& emit)
    {
        active_ = true; kind_ = clampv(p.kind, 0, 2); elapsed_ = 0.0; length_ = std::max(0.5, static_cast<double>(p.length));
        approach_ = clampv(p.approach, -0.45f, 0.45f);
        home_ = clampv(p.distance, 0.0f, 1.0f);
        glided_ = false; moved_ = false; ++events_;
        if (kind_ == 2) {
            // The ring: every step a degree chosen against the harmony, some an octave up, a
            // few rests, most of them open, a handful accented.
            stepsUsed_ = clampv(p.steps, 3, kMaxSteps);
            for (int s = 0; s < stepsUsed_; ++s) {
                Step& st = steps_[s];
                st.offset = choose(p, in, freqOf, consonance, -1) - in.root;
                st.octave = rng_.uniform() < 0.2f ? 12 : 0;
                st.velocity = 0.55f + 0.45f * rng_.uniform();
                st.open = rng_.uniform() < 0.6f;
                st.accent = rng_.uniform() < 0.15f * p.scatter;
                st.rest = s > 0 && rng_.uniform() < 0.1f;
            }
            stepPos_ = 0; stepLeft_ = 0.0; ghostDue_ = false; gateLeft_ = 0.0; gateNote_ = -1;
            note_ = -1;
            return;
        }
        note_ = choose(p, in, freqOf, consonance, lastNote_);
        lastNote_ = note_;
        // A phrase is two to four notes over the length, each a slide to another degree, the last
        // of them back to the first -- and at the end the flute's meri: a quarter tone down and
        // back, the bend a shakuhachi closes a phrase with.
        firstNote_ = note_;
        phraseNotes_ = kind_ == 1 ? 2 + rng_.below(3) : 1;
        phraseDone_ = 1; bent_ = 0;
        NearNote e;
        e.type = NearNote::Type::On; e.note = note_;
        e.velocity = 0.55f + 0.35f * rng_.uniform();
        // Where it begins: on the horizon when it is to arrive, else where the preset puts it.
        e.distance = approach_ > 0.0f ? 1.0f : home_;
        e.pan = p.spread * rng_.bipolar();
        e.releaseMul = 1.0f; e.cutoffMul = 1.0f;
        emit(e);
        if (approach_ > 0.0f) {          // arriving: from the horizon to its place
            NearNote m; m.type = NearNote::Type::Move; m.note = note_; m.distance = home_;
            m.seconds = static_cast<float>(approach_ * length_);
            emit(m);
        } else if (approach_ < 0.0f) {   // leaving: from its place out to the horizon, from the first moment
            NearNote m; m.type = NearNote::Type::Move; m.note = note_; m.distance = 1.0f;
            m.seconds = static_cast<float>(-approach_ * length_);
            emit(m);
        }
    }

    /**
     * @brief Advances a running event by @p dt.
     *
     * A Sequence is handed to runSequence(). A Phrase emits its slides at k/n of the length (the
     * last one home) and its closing meri -- a quarter tone down at 88 % and back at 95 %; an
     * arriving Note or Phrase is sent back to the horizon over its last stretch. At the end of the
     * length the event is ended.
     *
     * @tparam FreqFn     the tuning (see update)
     * @tparam ConsFn     the ear (see update)
     * @tparam EmitFn     the sink (see update)
     * @param dt          seconds since the last call
     * @param p           the settings
     * @param in          the root and the cluster
     * @param freqOf      the tuning
     * @param consonance  the ear
     * @param emit        receives the NearNotes
     */
    template <class FreqFn, class ConsFn, class EmitFn>
    void run(double dt, const NearParams& p, const NearInputs& in, FreqFn&& freqOf, ConsFn&& consonance, EmitFn&& emit)
    {
        elapsed_ += dt;
        const double q = elapsed_ / length_;
        if (kind_ == 2) { runSequence(dt, q, p, in, freqOf, consonance, emit); if (elapsed_ >= length_) end(emit); return; }
        // The Phrase's slides: note k of n begins at k/n of the length, the last one home.
        if (kind_ == 1 && phraseDone_ < phraseNotes_ && q >= static_cast<double>(phraseDone_) / phraseNotes_) {
            const bool last = phraseDone_ == phraseNotes_ - 1;
            const int to = last ? firstNote_ : choose(p, in, freqOf, consonance, note_);
            ++phraseDone_;
            if (to != note_) {
                NearNote g; g.type = NearNote::Type::Glide; g.note = to;
                g.seconds = p.glide > 0.0f ? p.glide : static_cast<float>(0.2 * length_ / phraseNotes_);
                emit(g);
                note_ = to; lastNote_ = to;
            }
        }
        // The meri at the end: a quarter tone down at nine tenths, back at nineteen twentieths.
        if (kind_ == 1 && bent_ == 0 && q >= 0.88) {
            bent_ = 1;
            NearNote g; g.type = NearNote::Type::Glide; g.note = note_; g.detune = -0.5f;
            g.seconds = static_cast<float>(0.05 * length_);
            emit(g);
        }
        if (kind_ == 1 && bent_ == 1 && q >= 0.95) {
            bent_ = 2;
            NearNote g; g.type = NearNote::Type::Glide; g.note = note_; g.detune = 0.0f;
            g.seconds = static_cast<float>(0.04 * length_);
            emit(g);
        }
        // Leaving: back into the far plane over the last stretch.
        if (approach_ > 0.0f && !moved_ && q >= 1.0 - approach_) {
            moved_ = true;
            NearNote m; m.type = NearNote::Type::Move; m.note = note_; m.distance = 1.0f;
            m.seconds = static_cast<float>(approach_ * length_);
            emit(m);
        }
        if (elapsed_ >= length_) end(emit);
    }

    /**
     * @brief Where the sequence stands between the planes at progress q: out of the far, at its place
     *        for the middle, back into the far. Its place is Distance; the run's approach is symmetric
     *        whichever sign it carries.
     * @param q  progress through the run, 0 .. 1
     * @return   the plane, home_ .. 1
     */
    float trajectory(double q) const
    {
        const double a = std::fabs(static_cast<double>(approach_));
        float outward = 0.0f;
        if (a > 0.0) {
            if (q < a) outward = static_cast<float>(1.0 - q / a);
            else if (q > 1.0 - a) outward = static_cast<float>((q - (1.0 - a)) / a);
        }
        return home_ + (1.0f - home_) * outward;
    }

    /**
     * @brief Advances a Sequence by @p dt: ends the sounding step's gate when it is up and, when the
     *        next half-step is due, plays a ghost or the next step of the ring.
     *
     * The tempo breathes on a drifter (three per cent), every step lands a few milliseconds early
     * or late on another, and a slow density gate between a third and one decides whether a step
     * plays at all. Between two steps a quiet, dark ghost of the step just gone may sound (Scatter).
     * At the start of each cycle one step may mutate (Mutation): its degree, octave, gate, velocity
     * or accent.
     *
     * @tparam FreqFn     the tuning (see update)
     * @tparam ConsFn     the ear (see update)
     * @tparam EmitFn     the sink (see update)
     * @param dt          seconds since the last call
     * @param q           progress through the run, 0 .. 1
     * @param p           the settings
     * @param in          the root and the cluster
     * @param freqOf      the tuning
     * @param consonance  the ear
     * @param emit        receives the NearNotes
     */
    template <class FreqFn, class ConsFn, class EmitFn>
    void runSequence(double dt, double q, const NearParams& p, const NearInputs& in, FreqFn&& freqOf, ConsFn&& consonance, EmitFn&& emit)
    {
        // The gate of the step that is sounding.
        if (gateNote_ >= 0) {
            gateLeft_ -= dt;
            if (gateLeft_ <= 0.0) { NearNote off; off.type = NearNote::Type::Off; off.note = gateNote_; emit(off); gateNote_ = -1; }
        }
        stepLeft_ -= dt;
        if (stepLeft_ > 0.0) return;
        // The pulse breathes: the tempo on a drifter, three per cent either way over a minute or
        // so, as tape does; every step a few milliseconds early or late on a drifter of its own,
        // never on white noise, so the timing wanders like a hand and does not jitter.
        const double tempo = 1.0 + 0.03 * static_cast<double>(tempoDrift_.update(static_cast<float>(stepSec_), 0.015f, rng_));
        const double stepSec = stepSec_ * tempo;
        const double micro = 0.010 * static_cast<double>(timingDrift_.update(static_cast<float>(stepSec_), 0.3f, rng_));
        // The density gate: a slow swing between a third and one, the probability a step plays,
        // so the line thins and thickens over a minute the way it does with a hand on the mixer.
        densityPhase_ += stepSec / (40.0 + 50.0 * static_cast<double>(p.scatter));
        if (densityPhase_ >= 1.0) densityPhase_ -= 1.0;
        const float gate = 0.65f + 0.35f * sin01(densityPhase_);
        // Between two steps, a ghost: quiet, dark, short, on the step that has just gone.
        if (ghostDue_) {
            ghostDue_ = false;
            stepLeft_ += 0.5 * stepSec + micro;
            const Step& prev = steps_[(stepPos_ + stepsUsed_ - 1) % stepsUsed_];
            if (!prev.rest && rng_.uniform() < 0.5f * p.scatter * gate) play(prev, in.root, q, p, 0.3f, 0.2f, emit);
            return;
        }
        if (stepPos_ == 0 && elapsed_ > stepSec) {
            // A cycle has ended: with Mutation's chance, one step changes -- which is the shift
            // register's one bit, and what turns a loop into a line over the minutes. Every field
            // of the step mutates, each at its own share: the degree most, then the octave, the
            // gate, the velocity, the accent, so the line's articulation drifts with its notes.
            if (rng_.uniform() < p.mutation) {
                Step& st = steps_[rng_.below(stepsUsed_)];
                const float what = rng_.uniform();
                if (what < 0.45f) st.offset = choose(p, in, freqOf, consonance, -1) - in.root;
                else if (what < 0.65f) st.octave = st.octave == 0 ? 12 : 0;
                else if (what < 0.8f) st.open = !st.open;
                else if (what < 0.92f) st.velocity = 0.55f + 0.45f * rng_.uniform();
                else st.accent = !st.accent;
                ++mutations_;
            }
        }
        const Step& st = steps_[stepPos_];
        if (!st.rest && (gate >= 0.999f || rng_.uniform() < gate)) play(st, in.root, q, p, st.velocity, st.open ? 0.5f : 0.15f, emit);
        stepPos_ = (stepPos_ + 1) % stepsUsed_;
        stepLeft_ += 0.5 * stepSec + micro;
        ghostDue_ = true;
    }

    /**
     * @brief Plays one step (or a ghost of one): ends the previous gate, emits the On with the step's
     *        place, gate and bloom, and arms the gate.
     *
     * An accented step is sent to the horizon (0.8 or further); every other one stands where
     * trajectory() puts the run. The bloom is two octaves under the cutoff as the run begins, an
     * octave over it at its height, and back.
     *
     * @tparam EmitFn    the sink (see update)
     * @param st         the step
     * @param root       the conductor's root, as a MIDI note
     * @param q          progress through the run, 0 .. 1
     * @param p          the settings (spread, bloom)
     * @param velocity   0 .. 1
     * @param gateShare  how long the gate stays open, as a share of a step
     * @param emit       receives the NearNotes
     */
    template <class EmitFn>
    void play(const Step& st, int root, double q, const NearParams& p, float velocity, float gateShare, EmitFn&& emit)
    {
        if (gateNote_ >= 0) { NearNote off; off.type = NearNote::Type::Off; off.note = gateNote_; emit(off); gateNote_ = -1; }
        const double stepSec = stepSec_;
        NearNote e;
        e.type = NearNote::Type::On;
        e.note = clampv(root + st.offset + st.octave, 0, 127);
        e.velocity = velocity;
        e.distance = st.accent ? std::max(0.8f, home_) : trajectory(q);
        e.pan = p.spread * rng_.bipolar();
        e.releaseMul = st.open ? 1.0f : 0.25f;
        // The bloom: two octaves under the cutoff as the run begins, an octave over it at its
        // height, and back -- the filter breathing over the minutes, not over the bar.
        e.cutoffMul = static_cast<float>(std::pow(2.0, static_cast<double>(p.bloom) * (-2.0 + 3.0 * std::sin(3.14159265358979 * q))));
        emit(e);
        gateNote_ = e.note;
        gateLeft_ = gateShare * stepSec;
        note_ = e.note;
    }

    /**
     * @brief Ends the current event: lets its note go, draws the gap to the next one and, after a Note or
     *        a Phrase, holds the conductor for ten more seconds so the horizon's return is heard.
     * @tparam EmitFn  the sink (see update)
     * @param emit     receives the Off
     */
    template <class EmitFn>
    void end(EmitFn&& emit)
    {
        if (kind_ == 2 && gateNote_ >= 0) { NearNote off; off.type = NearNote::Type::Off; off.note = gateNote_; emit(off); gateNote_ = -1; }
        if (kind_ != 2 && note_ >= 0) { NearNote off; off.type = NearNote::Type::Off; off.note = note_; emit(off); }
        active_ = false;
        note_ = -1;
        timer_ = gap(rate_);
        if (kind_ != 2) holdTail_ = 10.0;   // the horizon comes back before the conductor moves
    }

private:
    bool   active_ = false,   ///< @brief an event is sounding
           hold_ = true,      ///< @brief Hold Brain, as of the last update()
           glided_ = false,   ///< @brief (kept from an earlier phrase model; reset with the event)
           moved_ = false;    ///< the arriving event has been sent back out
    int    kind_ = 0;            ///< the current event's kind: 0 Note, 1 Phrase, 2 Sequence
    double elapsed_ = 0.0,    ///< @brief seconds into the current event
           length_ = 0.0,     ///< @brief the event's length in seconds
           timer_ = 30.0,     ///< @brief seconds until the next event is due
           rate_ = 120.0,     ///< @brief the mean gap, at least ten seconds
           stepSec_ = 0.4,    ///< @brief a sequence step in seconds, synced or free
           waited_ = 0.0,     ///< @brief seconds spent waiting for stillness
           holdTail_ = 0.0;   ///< seconds the conductor is still held after a Note or Phrase
    float  approach_ = 0.0f;     ///< the event's approach, -0.45 .. 0.45 of its length
    float  home_ = 0.0f;         ///< the event's place between the planes (NearParams::distance)
    int    note_ = -1,        ///< @brief the note sounding, -1 for none
           lastNote_ = -1,    ///< @brief the previous event's note, to be avoided next time
           firstNote_ = -1;   ///< a phrase's first note, where its last slide returns
    int    phraseNotes_ = 0,   ///< @brief notes in the phrase (2 .. 4; 1 for a Note)
           phraseDone_ = 0,    ///< @brief notes of the phrase begun so far
           bent_ = 0;          ///< the meri's stage: 0 not yet, 1 bent down, 2 back
    int    events_ = 0,      ///< @brief events begun since reset()
           refused_ = 0,     ///< @brief due moments refused since reset()
           mutations_ = 0;   ///< steps mutated since reset()
    double excAvg_ = 0.0;        ///< the running average of the cascade's excitation, what the coin is weighted against
    Drifter tempoDrift_,    ///< @brief the sequence tempo's slow wander
            timingDrift_;   ///< each step's small early-or-late
    double densityPhase_ = 0.0;  ///< the density gate's phase, 0 .. 1
    Step   steps_[kMaxSteps];    ///< the sequence ring
    int    stepsUsed_ = 0,   ///< @brief steps in the ring, 3 .. kMaxSteps
           stepPos_ = 0;     ///< the next step to play
    double stepLeft_ = 0.0,   ///< @brief seconds until the next half-step
           gateLeft_ = 0.0;   ///< seconds until the sounding step is let go
    int    gateNote_ = -1;       ///< the sequence note whose gate is open, -1 for none
    bool   ghostDue_ = false;    ///< the next half-step is a ghost's, not a step's
};

} // namespace ambient
