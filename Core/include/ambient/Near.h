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
    float m_level = 0.0f;          ///< 0: off
    int   m_kind = 0;              ///< 0 Note, 1 Phrase, 2 Sequence
    float m_rate = 120.0f;         ///< mean seconds from the end of one event to the start of the next
    float m_chance = 1.0f;         ///< the coin at each due moment
    float m_cluster = 0.0f;        ///< the coin weighted by the conductor's excitation
    float m_length = 6.0f;         ///< seconds an event lasts; a sequence's whole run
    int   m_pitch = 0;             ///< 0 Consonant, 1 Highest, 2 Lowest, 3 Root, 4 Cluster
    float m_spread = 0.5f;         ///< how far from the centre an event may sit
    float m_approach = 0.0f;       ///< fraction of the event spent arriving from the far plane (and leaving into it); negative: leaving from the start
    float m_distance = 0.0f;       ///< where the event sits, 0 at the ear .. 1 on the horizon (what Approach arrives at)
    float m_dry = 0.0f;            ///< the share of the event's voice that goes past every reverb and delay
    float m_toDelay2 = 0.0f;       ///< an extra share into the second delay's input (a send, added, not taken away)
    float m_toCosmos = 0.0f;       ///< ... and into the Cosmos send, likewise
    float m_proximity = 0.6f;      ///< the near field's low lift on the event's voice
    bool  m_hold = true;           ///< the conductor waits while a Note or a Phrase sounds
    float m_glide = 0.0f;          ///< Phrase: seconds of the slide (0: two fifths of the length)
    int   m_steps = 7;             ///< Sequence: the ring
    float m_stepSeconds = 0.4f;    ///< Sequence: a step, when not synced
    int   m_stepSync = 0;          ///< Sequence: SyncDivNames index, 0 = free
    float m_mutation = 0.12f;      ///< Sequence: chance per cycle that one step changes
    float m_scatter = 0.3f;        ///< Sequence: ghost notes between the steps, accents sent far
    float m_bloom = 0.5f;          ///< Sequence: the filter opening over the run and closing again
    float m_attack = 0.3f,    ///< @brief the source's attack in seconds
          m_release = 2.0f;   ///< the source's, for the gates
};

/** @brief One thing the engine should do. */
struct NearNote {
    /** @brief What to do: start a note, end it, slide it to another degree, or move its plane. */
    enum class TypeE {
        ON,      ///< @brief start `note` at `velocity` on plane `distance`, panned `pan`, shaped by releaseMul and cutoffMul
        OFF,     ///< @brief let `note` go
        GLIDE,   ///< @brief slide the sounding note to `note` (plus `detune`) over `seconds`
        MOVE     ///< @brief move the sounding note's plane to `distance` over `seconds`
    };
    TypeE  m_type = TypeE::ON;       ///< which of the four
    int   m_note = 60;             ///< the MIDI note it concerns (On, Off: the note; Glide: the destination; Move: the sounding note)
    float m_velocity = 0.7f;       ///< On: 0 .. 1
    float m_distance = 0.0f;       ///< On: the plane the note starts on; Move: where to go
    float m_pan = 0.0f;            ///< On: -1 .. 1
    float m_releaseMul = 1.0f;     ///< On: the gate, as a share of the source's release
    float m_cutoffMul = 1.0f;      ///< On: the bloom, a factor on the near filter's cutoff
    float m_seconds = 0.0f;        ///< Glide, Move: how long
    float m_detune = 0.0f;         ///< Glide: semitones off the note (a flute's meri bend, a quarter tone down)
};

/** @brief What the scheduler is told about the piece at each step. */
struct NearInputs {
    int    m_root = 50;            ///< the conductor's root, as a MIDI note
    int    m_cluster[12] = {};     ///< the conductor's sounding notes
    int    m_count = 0;            ///< how many of cluster are sounding
    double m_excitation = 0.0;     ///< the cascade's, in multiples of the base rate
    bool   m_silence = false;      ///< the conductor's planned pause
    double m_rootAge = 1.0e9;      ///< seconds since the root last moved
    double m_bpm = 90.0;           ///< the clock's tempo, for a synced step
    double m_sinceOnset = 1.0e9;   ///< seconds since the conductor last began a note
    bool   m_releasing = false;    ///< a conductor's voice is letting go right now
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
    static constexpr int MaxSteps = 16;   ///< the longest sequence ring

    /**
     * @brief Puts the scheduler at the beginning of a piece: nothing sounding, counters at zero, the first event
     *        due in twenty to forty seconds, and the dice seeded.
     * @param seed  the scheduler's own random stream (part of the preset's seed)
     */
    void reset(uint64_t seed)
    {
        m_rng.seed(seed);
        m_active = false; m_kind = 0; m_elapsed = 0.0; m_length = 0.0;
        m_timer = 20.0 + 20.0 * static_cast<double>(m_rng.uniform());   // the first, not at once
        m_note = -1; m_lastNote = -1; m_glided = false; m_moved = false;
        m_events = 0; m_refused = 0; m_waited = 0.0; m_holdTail = 0.0;
        m_excAvg = 0.0;
        m_stepsUsed = 0; m_stepPos = 0; m_stepLeft = 0.0; m_ghostDue = false; m_mutations = 0;
        for (auto& s : m_steps) s = Step{};
        m_gateLeft = 0.0; m_gateNote = -1;
        m_phraseNotes = 0; m_phraseDone = 0; m_firstNote = -1; m_bent = 0;
        m_tempoDrift.init(m_rng); m_timingDrift.init(m_rng); m_densityPhase = static_cast<double>(m_rng.uniform());
    }

    /** @return whether an event is sounding right now */
    bool  active() const   { return m_active; }
    /**
     * @brief The conductor is asked to begin no new note: while a Note or a Phrase sounds, and for ten
     *        seconds after it, so the horizon's return is heard before the background moves again.
     * @return true while the conductor should hold its decisions (only with Hold Brain on)
     */
    bool  holding() const  { return m_hold && ((m_active && m_kind != 2) || m_holdTail > 0.0); }
    /**
     * @brief A sequence is running: the conductor keeps its root and halves its pace under it.
     * @return true while a Sequence event is active
     */
    bool  sequenceRunning() const { return m_active && m_kind == 2; }
    /** @return how far through the current event, 0 .. 1 (0 when none) */
    float progress() const { return m_active && m_length > 0.0 ? static_cast<float>(m_elapsed / m_length) : 0.0f; }
    /** @return events begun since reset() */
    int   events() const   { return m_events; }
    /** @return due moments that were refused by a rule or the coin since reset() */
    int   refused() const  { return m_refused; }
    /** @return sequence steps mutated since reset() */
    int   mutations() const { return m_mutations; }
    /** @return the kind of the current (or last) event: 0 Note, 1 Phrase, 2 Sequence */
    int   kind() const     { return m_kind; }
    /** @return seconds until the next event is due (may be negative while waiting for stillness) */
    double nextIn() const  { return m_timer; }
    /** @return the MIDI note sounding (a Note or Phrase's, or a sequence's last step), -1 for none */
    int   currentNote() const { return m_note; }

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
    template <class FreqFnT, class ConsFnT, class EmitFnT>
    void update(double dt, const NearParams& p, const NearInputs& in, FreqFnT&& freqOf, ConsFnT&& consonance, EmitFnT&& emit)
    {
        m_hold = p.m_hold;
        m_rate = std::max(10.0, static_cast<double>(p.m_rate));
        m_stepSec = syncOn(p.m_stepSync) ? std::max(0.05, syncSeconds(p.m_stepSync, in.m_bpm)) : std::max(0.05, static_cast<double>(p.m_stepSeconds));
        if (p.m_level <= 0.0f) {
            if (m_active) end(emit);     // switched off mid-event: let the note go
            m_timer = std::max(m_timer, 5.0);
            return;
        }
        if (m_holdTail > 0.0) m_holdTail -= dt;
        if (m_active) { run(dt, p, in, freqOf, consonance, emit); return; }
        m_timer = std::min(m_timer, m_rate);   // the first wait is never longer than the rate itself
        m_timer -= dt;
        if (m_timer > 0.0) return;
        // Due. The rules first: not in a silence, not just after a root change, not into nothing.
        if (in.m_silence || in.m_rootAge < 6.0 || in.m_count <= 0) { m_timer = 2.0; ++m_refused; return; }
        // And into stillness: the near bus has been steady for four seconds -- no onset of the
        // conductor's, no voice letting go -- which is what makes the event an answer and not an
        // interruption. Waited for at most half the rate, then the event goes anyway.
        if ((in.m_sinceOnset < 4.0 || in.m_releasing) && m_waited < 0.5 * m_rate) { m_waited += dt; return; }
        m_waited = 0.0;
        // The coin, weighted by the cascade against its own average, as the strike's is.
        m_excAvg += 0.1 * (in.m_excitation - m_excAvg);
        const double lift = 1.0 + 2.0 * static_cast<double>(p.m_cluster) * (in.m_excitation - m_excAvg);
        const double chance = clampv(static_cast<double>(p.m_chance) * lift, 0.0, 1.0);
        if (chance < 0.999 && m_rng.uniform() >= chance) { m_timer = gap(0.3 * p.m_rate); ++m_refused; return; }
        begin(p, in, freqOf, consonance, emit);
    }

private:
    /** @brief One step of the sequence ring. */
    struct Step {
        int m_offset = 7;          ///< @brief semitones over the root
        int m_octave = 0;          ///< @brief 0, or 12 for a step an octave up
        float m_velocity = 0.7f;   ///< @brief 0.55 .. 1
        bool m_open = true;        ///< @brief a long gate (half a step) rather than a short one
        bool m_accent = false;     ///< @brief sent far: played on the horizon
        bool m_rest = false;       ///< @brief a silent step
    };

    /**
     * @brief The gap the conductor draws: exponential, shifted so the floor is a floor and not a peak.
     * @param mean  the mean gap in seconds
     * @return      a gap of at least a fifth of the mean (capped at five seconds), exponentially distributed above it
     */
    double gap(double mean) const
    {
        const double floor = std::min(5.0, 0.2 * mean);
        const double u = -std::log(1.0 - static_cast<double>(m_rng.uniform()) + 1e-9);
        return floor + u * std::max(mean - floor, 0.25 * mean);
    }
    mutable Rng m_rng;   ///< the scheduler's own dice (mutable so gap() can draw from a const method)

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
        struct W { double m_c; float m_w; };
        static const W table[] = { { 702.0, 1.0f }, { 204.0, 0.85f }, { 386.0, 0.8f }, { 0.0, 0.6f }, { 1200.0, 0.6f },
                                   { 884.0, 0.5f }, { 498.0, 0.5f }, { 969.0, 0.45f }, { 316.0, 0.4f }, { 1088.0, 0.2f } };
        for (const W& w : table) if (std::fabs(cents - w.m_c) < 25.0) return w.m_w;
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
    template <class FreqFnT, class ConsFnT>
    int choose(const NearParams& p, const NearInputs& in, FreqFnT&& freqOf, ConsFnT&& consonance, int avoid)
    {
        int lo = 127, hi = 0;
        for (int i = 0; i < in.m_count; ++i) { lo = std::min(lo, in.m_cluster[i]); hi = std::max(hi, in.m_cluster[i]); }
        if (in.m_count <= 0) { lo = hi = in.m_root + 12; }
        switch (p.m_pitch) {
        case 1: return hi;
        case 2: return lo;
        case 3: return in.m_root;
        case 4: return in.m_cluster[m_rng.below(std::max(1, in.m_count))];
        default: break;
        }
        // The window: from a fifth under the cluster's middle to a twelfth over its top, so the
        // register a soloist wants -- above the body -- is in it.
        const int centre = (lo + hi) / 2;
        const int from = clampv(centre - 7, 24, 108), to = clampv(hi + 19, 24, 108);
        const double fRoot = freqOf(in.m_root);
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
            for (int i = 0; i < in.m_count; ++i) {
                const double fk = freqOf(in.m_cluster[i]);
                w *= 0.35f + 0.65f * static_cast<float>(consonance(f, fk));
                if (std::fabs(1200.0 * std::log2(f / fk)) < 5.0) w *= 0.3f;   // already sounding: a doubling, not a voice
                // Never inside a critical band of a sounding voice (Glasberg and Moore's ERB):
                // the drone would mask the flute, and the event would be paid for and not heard.
                const double erb = 24.7 * (0.00437 * 0.5 * (f + fk) + 1.0);
                if (std::fabs(f - fk) < erb && std::fabs(f - fk) > 1.0e-6) w *= 0.05f;
                nearest = std::min(nearest, std::abs(c - in.m_cluster[i]));
            }
            // The register: a soloist stands at least a fifth above the highest body voice, or in
            // a gap of a major third or more; inside the chord it is one more voice of the chord.
            if (c >= hi + 7) w *= 1.4f;
            else if (nearest < 4) w *= 0.35f;
            if (c == avoid) w *= 0.4f;
            weights[c] = w; total += w;
        }
        if (total <= 0.0f) return centre;
        float r = m_rng.uniform() * total;
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
    template <class FreqFnT, class ConsFnT, class EmitFnT>
    void begin(const NearParams& p, const NearInputs& in, FreqFnT&& freqOf, ConsFnT&& consonance, EmitFnT&& emit)
    {
        m_active = true; m_kind = clampv(p.m_kind, 0, 2); m_elapsed = 0.0; m_length = std::max(0.5, static_cast<double>(p.m_length));
        m_approach = clampv(p.m_approach, -0.45f, 0.45f);
        m_home = clampv(p.m_distance, 0.0f, 1.0f);
        m_glided = false; m_moved = false; ++m_events;
        if (m_kind == 2) {
            // The ring: every step a degree chosen against the harmony, some an octave up, a
            // few rests, most of them open, a handful accented.
            m_stepsUsed = clampv(p.m_steps, 3, MaxSteps);
            for (int s = 0; s < m_stepsUsed; ++s) {
                Step& st = m_steps[s];
                st.m_offset = choose(p, in, freqOf, consonance, -1) - in.m_root;
                st.m_octave = m_rng.uniform() < 0.2f ? 12 : 0;
                st.m_velocity = 0.55f + 0.45f * m_rng.uniform();
                st.m_open = m_rng.uniform() < 0.6f;
                st.m_accent = m_rng.uniform() < 0.15f * p.m_scatter;
                st.m_rest = s > 0 && m_rng.uniform() < 0.1f;
            }
            m_stepPos = 0; m_stepLeft = 0.0; m_ghostDue = false; m_gateLeft = 0.0; m_gateNote = -1;
            m_note = -1;
            return;
        }
        m_note = choose(p, in, freqOf, consonance, m_lastNote);
        m_lastNote = m_note;
        // A phrase is two to four notes over the length, each a slide to another degree, the last
        // of them back to the first -- and at the end the flute's meri: a quarter tone down and
        // back, the bend a shakuhachi closes a phrase with.
        m_firstNote = m_note;
        m_phraseNotes = m_kind == 1 ? 2 + m_rng.below(3) : 1;
        m_phraseDone = 1; m_bent = 0;
        NearNote e;
        e.m_type = NearNote::TypeE::ON; e.m_note = m_note;
        e.m_velocity = 0.55f + 0.35f * m_rng.uniform();
        // Where it begins: on the horizon when it is to arrive, else where the preset puts it.
        e.m_distance = m_approach > 0.0f ? 1.0f : m_home;
        e.m_pan = p.m_spread * m_rng.bipolar();
        e.m_releaseMul = 1.0f; e.m_cutoffMul = 1.0f;
        emit(e);
        if (m_approach > 0.0f) {          // arriving: from the horizon to its place
            NearNote m; m.m_type = NearNote::TypeE::MOVE; m.m_note = m_note; m.m_distance = m_home;
            m.m_seconds = static_cast<float>(m_approach * m_length);
            emit(m);
        } else if (m_approach < 0.0f) {   // leaving: from its place out to the horizon, from the first moment
            NearNote m; m.m_type = NearNote::TypeE::MOVE; m.m_note = m_note; m.m_distance = 1.0f;
            m.m_seconds = static_cast<float>(-m_approach * m_length);
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
    template <class FreqFnT, class ConsFnT, class EmitFnT>
    void run(double dt, const NearParams& p, const NearInputs& in, FreqFnT&& freqOf, ConsFnT&& consonance, EmitFnT&& emit)
    {
        m_elapsed += dt;
        const double q = m_elapsed / m_length;
        if (m_kind == 2) { runSequence(dt, q, p, in, freqOf, consonance, emit); if (m_elapsed >= m_length) end(emit); return; }
        // The Phrase's slides: note k of n begins at k/n of the length, the last one home.
        if (m_kind == 1 && m_phraseDone < m_phraseNotes && q >= static_cast<double>(m_phraseDone) / m_phraseNotes) {
            const bool last = m_phraseDone == m_phraseNotes - 1;
            const int to = last ? m_firstNote : choose(p, in, freqOf, consonance, m_note);
            ++m_phraseDone;
            if (to != m_note) {
                NearNote g; g.m_type = NearNote::TypeE::GLIDE; g.m_note = to;
                g.m_seconds = p.m_glide > 0.0f ? p.m_glide : static_cast<float>(0.2 * m_length / m_phraseNotes);
                emit(g);
                m_note = to; m_lastNote = to;
            }
        }
        // The meri at the end: a quarter tone down at nine tenths, back at nineteen twentieths.
        if (m_kind == 1 && m_bent == 0 && q >= 0.88) {
            m_bent = 1;
            NearNote g; g.m_type = NearNote::TypeE::GLIDE; g.m_note = m_note; g.m_detune = -0.5f;
            g.m_seconds = static_cast<float>(0.05 * m_length);
            emit(g);
        }
        if (m_kind == 1 && m_bent == 1 && q >= 0.95) {
            m_bent = 2;
            NearNote g; g.m_type = NearNote::TypeE::GLIDE; g.m_note = m_note; g.m_detune = 0.0f;
            g.m_seconds = static_cast<float>(0.04 * m_length);
            emit(g);
        }
        // Leaving: back into the far plane over the last stretch.
        if (m_approach > 0.0f && !m_moved && q >= 1.0 - m_approach) {
            m_moved = true;
            NearNote m; m.m_type = NearNote::TypeE::MOVE; m.m_note = m_note; m.m_distance = 1.0f;
            m.m_seconds = static_cast<float>(m_approach * m_length);
            emit(m);
        }
        if (m_elapsed >= m_length) end(emit);
    }

    /**
     * @brief Where the sequence stands between the planes at progress q: out of the far, at its place
     *        for the middle, back into the far. Its place is Distance; the run's approach is symmetric
     *        whichever sign it carries.
     * @param q  progress through the run, 0 .. 1
     * @return   the plane, m_home .. 1
     */
    float trajectory(double q) const
    {
        const double a = std::fabs(static_cast<double>(m_approach));
        float outward = 0.0f;
        if (a > 0.0) {
            if (q < a) outward = static_cast<float>(1.0 - q / a);
            else if (q > 1.0 - a) outward = static_cast<float>((q - (1.0 - a)) / a);
        }
        return m_home + (1.0f - m_home) * outward;
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
    template <class FreqFnT, class ConsFnT, class EmitFnT>
    void runSequence(double dt, double q, const NearParams& p, const NearInputs& in, FreqFnT&& freqOf, ConsFnT&& consonance, EmitFnT&& emit)
    {
        // The gate of the step that is sounding.
        if (m_gateNote >= 0) {
            m_gateLeft -= dt;
            if (m_gateLeft <= 0.0) { NearNote off; off.m_type = NearNote::TypeE::OFF; off.m_note = m_gateNote; emit(off); m_gateNote = -1; }
        }
        m_stepLeft -= dt;
        if (m_stepLeft > 0.0) return;
        // The pulse breathes: the tempo on a drifter, three per cent either way over a minute or
        // so, as tape does; every step a few milliseconds early or late on a drifter of its own,
        // never on white noise, so the timing wanders like a hand and does not jitter.
        const double tempo = 1.0 + 0.03 * static_cast<double>(m_tempoDrift.update(static_cast<float>(m_stepSec), 0.015f, m_rng));
        const double stepSec = m_stepSec * tempo;
        const double micro = 0.010 * static_cast<double>(m_timingDrift.update(static_cast<float>(m_stepSec), 0.3f, m_rng));
        // The density gate: a slow swing between a third and one, the probability a step plays,
        // so the line thins and thickens over a minute the way it does with a hand on the mixer.
        m_densityPhase += stepSec / (40.0 + 50.0 * static_cast<double>(p.m_scatter));
        if (m_densityPhase >= 1.0) m_densityPhase -= 1.0;
        const float gate = 0.65f + 0.35f * sin01(m_densityPhase);
        // Between two steps, a ghost: quiet, dark, short, on the step that has just gone.
        if (m_ghostDue) {
            m_ghostDue = false;
            m_stepLeft += 0.5 * stepSec + micro;
            const Step& prev = m_steps[(m_stepPos + m_stepsUsed - 1) % m_stepsUsed];
            if (!prev.m_rest && m_rng.uniform() < 0.5f * p.m_scatter * gate) play(prev, in.m_root, q, p, 0.3f, 0.2f, emit);
            return;
        }
        if (m_stepPos == 0 && m_elapsed > stepSec) {
            // A cycle has ended: with Mutation's chance, one step changes -- which is the shift
            // register's one bit, and what turns a loop into a line over the minutes. Every field
            // of the step mutates, each at its own share: the degree most, then the octave, the
            // gate, the velocity, the accent, so the line's articulation drifts with its notes.
            if (m_rng.uniform() < p.m_mutation) {
                Step& st = m_steps[m_rng.below(m_stepsUsed)];
                const float what = m_rng.uniform();
                if (what < 0.45f) st.m_offset = choose(p, in, freqOf, consonance, -1) - in.m_root;
                else if (what < 0.65f) st.m_octave = st.m_octave == 0 ? 12 : 0;
                else if (what < 0.8f) st.m_open = !st.m_open;
                else if (what < 0.92f) st.m_velocity = 0.55f + 0.45f * m_rng.uniform();
                else st.m_accent = !st.m_accent;
                ++m_mutations;
            }
        }
        const Step& st = m_steps[m_stepPos];
        if (!st.m_rest && (gate >= 0.999f || m_rng.uniform() < gate)) play(st, in.m_root, q, p, st.m_velocity, st.m_open ? 0.5f : 0.15f, emit);
        m_stepPos = (m_stepPos + 1) % m_stepsUsed;
        m_stepLeft += 0.5 * stepSec + micro;
        m_ghostDue = true;
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
    template <class EmitFnT>
    void play(const Step& st, int root, double q, const NearParams& p, float velocity, float gateShare, EmitFnT&& emit)
    {
        if (m_gateNote >= 0) { NearNote off; off.m_type = NearNote::TypeE::OFF; off.m_note = m_gateNote; emit(off); m_gateNote = -1; }
        const double stepSec = m_stepSec;
        NearNote e;
        e.m_type = NearNote::TypeE::ON;
        e.m_note = clampv(root + st.m_offset + st.m_octave, 0, 127);
        e.m_velocity = velocity;
        e.m_distance = st.m_accent ? std::max(0.8f, m_home) : trajectory(q);
        e.m_pan = p.m_spread * m_rng.bipolar();
        e.m_releaseMul = st.m_open ? 1.0f : 0.25f;
        // The bloom: two octaves under the cutoff as the run begins, an octave over it at its
        // height, and back -- the filter breathing over the minutes, not over the bar.
        e.m_cutoffMul = static_cast<float>(std::pow(2.0, static_cast<double>(p.m_bloom) * (-2.0 + 3.0 * std::sin(3.14159265358979 * q))));
        emit(e);
        m_gateNote = e.m_note;
        m_gateLeft = gateShare * stepSec;
        m_note = e.m_note;
    }

    /**
     * @brief Ends the current event: lets its note go, draws the gap to the next one and, after a Note or
     *        a Phrase, holds the conductor for ten more seconds so the horizon's return is heard.
     * @tparam EmitFn  the sink (see update)
     * @param emit     receives the Off
     */
    template <class EmitFnT>
    void end(EmitFnT&& emit)
    {
        if (m_kind == 2 && m_gateNote >= 0) { NearNote off; off.m_type = NearNote::TypeE::OFF; off.m_note = m_gateNote; emit(off); m_gateNote = -1; }
        if (m_kind != 2 && m_note >= 0) { NearNote off; off.m_type = NearNote::TypeE::OFF; off.m_note = m_note; emit(off); }
        m_active = false;
        m_note = -1;
        m_timer = gap(m_rate);
        if (m_kind != 2) m_holdTail = 10.0;   // the horizon comes back before the conductor moves
    }

private:
    bool   m_active = false,   ///< @brief an event is sounding
           m_hold = true,      ///< @brief Hold Brain, as of the last update()
           m_glided = false,   ///< @brief (kept from an earlier phrase model; reset with the event)
           m_moved = false;    ///< the arriving event has been sent back out
    int    m_kind = 0;            ///< the current event's kind: 0 Note, 1 Phrase, 2 Sequence
    double m_elapsed = 0.0,    ///< @brief seconds into the current event
           m_length = 0.0,     ///< @brief the event's length in seconds
           m_timer = 30.0,     ///< @brief seconds until the next event is due
           m_rate = 120.0,     ///< @brief the mean gap, at least ten seconds
           m_stepSec = 0.4,    ///< @brief a sequence step in seconds, synced or free
           m_waited = 0.0,     ///< @brief seconds spent waiting for stillness
           m_holdTail = 0.0;   ///< seconds the conductor is still held after a Note or Phrase
    float  m_approach = 0.0f;     ///< the event's approach, -0.45 .. 0.45 of its length
    float  m_home = 0.0f;         ///< the event's place between the planes (NearParams::distance)
    int    m_note = -1,        ///< @brief the note sounding, -1 for none
           m_lastNote = -1,    ///< @brief the previous event's note, to be avoided next time
           m_firstNote = -1;   ///< a phrase's first note, where its last slide returns
    int    m_phraseNotes = 0,   ///< @brief notes in the phrase (2 .. 4; 1 for a Note)
           m_phraseDone = 0,    ///< @brief notes of the phrase begun so far
           m_bent = 0;          ///< the meri's stage: 0 not yet, 1 bent down, 2 back
    int    m_events = 0,      ///< @brief events begun since reset()
           m_refused = 0,     ///< @brief due moments refused since reset()
           m_mutations = 0;   ///< steps mutated since reset()
    double m_excAvg = 0.0;        ///< the running average of the cascade's excitation, what the coin is weighted against
    Drifter m_tempoDrift,    ///< @brief the sequence tempo's slow wander
            m_timingDrift;   ///< each step's small early-or-late
    double m_densityPhase = 0.0;  ///< the density gate's phase, 0 .. 1
    Step   m_steps[MaxSteps];    ///< the sequence ring
    int    m_stepsUsed = 0,   ///< @brief steps in the ring, 3 .. MaxSteps
           m_stepPos = 0;     ///< the next step to play
    double m_stepLeft = 0.0,   ///< @brief seconds until the next half-step
           m_gateLeft = 0.0;   ///< seconds until the sounding step is let go
    int    m_gateNote = -1;       ///< the sequence note whose gate is open, -1 for none
    bool   m_ghostDue = false;    ///< the next half-step is a ghost's, not a step's
};

} // namespace ambient
