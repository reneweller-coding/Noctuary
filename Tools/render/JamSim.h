#pragma once
/**
 * @file JamSim.h
 * @brief The family jam offline (plan E3, 20.11.2026): a leader's posts carried over a simulated network to a follower,
 *        through the instrument's own message code (Plugin/JamNotes.h) -- the same file in Noctuary and Empyrean.
 *
 * The acceptance measurement of plan E3 runs a leader's conductors in one audit (`--jam-lead FILE`: every change of
 * its root or of its sounding notes, as the plugin's audio thread posts them) and a follower's in another
 * (`--jam-follow FILE`). Between the two this file plays the jam's bus (Plugin/Jam.cpp):
 *  - the leader's bus thread wakes every 20 ms: a post goes out at its next wake; the notes again every
 *    JamNotes::RepeatMs, the key every 2 s, a hello every second;
 *  - each datagram is lost with a probability, delayed by a base latency plus an exponential tail, now and then held
 *    back much longer (so it arrives after newer ones), and now and then cut short (malformed);
 *  - the follower decodes what arrives in the order it arrives -- the notes with decodeJamNotes(), JamNotesOrder and
 *    JamNotesHeld, as Jam.cpp does -- and holds the root and the notes as JamBus::stateAt() and
 *    JamBus::leaderNotes() would (a leader not heard for 4 s is gone; its notes are forgotten after 2 s).
 * Header-only, standard library only; seeded, so a run repeats.
 * @author Rene Weller
 */
#include "../../Plugin/JamNotes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace frame
{

/** @brief One post of a leader: its root and the notes its conductors sound, from this moment on. */
struct JamLeadPost
{
    double   m_t = 0.0;     ///< when, s on the leader's clock
    int      m_root = -1;   ///< root pitch class
    JamNotes m_notes;       ///< the notes (m_seq unused)
};

/**
 * @brief Writes a post as a line `L,<seconds>,<root>,<count>,<Hz>...` (the floats exactly).
 * @param[in] f  the file
 * @param[in] p  the post
 */
inline void writeJamLeadPost( std::FILE *f, const JamLeadPost &p )
{
    std::fprintf( f, "L,%.6f,%d,%d", p.m_t, p.m_root, p.m_notes.m_count );
    for ( int k = 0; k < p.m_notes.m_count; ++k ) std::fprintf( f, ",%.9g", static_cast<double>( p.m_notes.m_hz[k] ) );
    std::fprintf( f, "\n" );
}

/**
 * @brief Reads the posts a leader's audit wrote (writeJamLeadPost()).
 * @param[in]  path   the file
 * @param[out] posts  receives them, in order
 * @return false when the file cannot be read or a line is malformed
 */
inline bool readJamLeadPosts( const char *path, std::vector<JamLeadPost> &posts )
{
    std::FILE *f = std::fopen( path, "r" );
    if ( f == nullptr ) return false;
    char line[2048];
    bool ok = true;
    while ( ok && std::fgets( line, sizeof( line ), f ) )
    {
        if ( line[0] != 'L' ) continue;
        JamLeadPost p;
        int         used = 0;
        ok = std::sscanf( line, "L,%lf,%d,%d%n", &p.m_t, &p.m_root, &p.m_notes.m_count, &used ) == 3 &&
             p.m_notes.m_count >= 0 && p.m_notes.m_count <= JamNotes::MaxNotes;
        const char *s = line + used;
        for ( int k = 0; ok && k < p.m_notes.m_count; ++k )
        {
            int n = 0;
            ok = std::sscanf( s, ",%f%n", &p.m_notes.m_hz[k], &n ) == 1;
            s += n;
        }
        if ( ok ) posts.push_back( p );
    }
    std::fclose( f );
    return ok;
}

/** @brief The simulated network's conditions. */
struct JamNetwork
{
    double   m_loss = 0.05;        ///< share of datagrams lost
    double   m_baseMs = 5.0;       ///< latency every datagram has, ms
    double   m_tailMs = 25.0;      ///< mean of the exponential latency on top, ms
    double   m_lateShare = 0.02;   ///< share held back much longer (200 .. 1500 ms more): they arrive after newer ones
    double   m_badShare = 0.01;    ///< share of notes datagrams cut short by four bytes (malformed)
    uint64_t m_seed = 1;           ///< the dice
};

/** @brief The jam between a leader's posts and a follower's audit (see the file's comment). */
class JamSim
{
public:
    /**
     * @brief Schedules every datagram the leader's bus would send for @p posts over @p seconds, and its fate.
     * @param[in] posts    the leader's posts, in time order
     * @param[in] seconds  the length of the run
     * @param[in] net      the network
     * @param[in] hear     whether the follower hears the notes ("Hear leader")
     */
    JamSim( const std::vector<JamLeadPost> &posts, double seconds, const JamNetwork &net, bool hear ) : m_hear( hear )
    {
        std::mt19937_64                        rng( net.m_seed );
        std::uniform_real_distribution<double> u( 0.0, 1.0 );
        const double                           wake = 0.020;   // the bus thread's loop (waitUntilReady 20 ms)
        size_t                                 next = 0;
        int                                    root = -1, sentRoot = -1;
        JamNotes                               notes;
        bool                                   haveNotes = false;
        double                                 lastNotes = -1.0, lastKey = -1.0, lastHello = -1.0;
        int32_t                                seq = 0;
        for ( double t = 0.0; t < seconds; t += wake )
        {
            bool newNotes = false;
            while ( next < posts.size() && posts[next].m_t <= t )
            {   // what the audio thread posted since the last wake
                const JamLeadPost &p = posts[next++];
                root = p.m_root;
                if ( !haveNotes || !p.m_notes.sameNotes( notes ) ) newNotes = true;
                notes = p.m_notes;
                haveNotes = true;
            }
            if ( lastHello < 0.0 || t - lastHello >= 1.0 )
            {
                lastHello = t;
                send( t, Datagram::HELLO, -1, nullptr, net, rng, u );
            }
            if ( root >= 0 && ( root != sentRoot || t - lastKey >= 2.0 ) )
            {
                sentRoot = root;
                lastKey = t;
                send( t, Datagram::KEY, root, nullptr, net, rng, u );
            }
            if ( haveNotes && ( newNotes || t - lastNotes >= JamNotes::RepeatMs / 1000.0 ) )
            {
                lastNotes = t;
                notes.m_seq = seq++;
                send( t, Datagram::NOTES, -1, &notes, net, rng, u );
            }
        }
        std::stable_sort( m_grams.begin(), m_grams.end(),
                          []( const Datagram &a, const Datagram &b ) { return a.m_arrive < b.m_arrive; } );
    }

    /**
     * @brief The follower at @p t: takes every datagram that has arrived by then, in the order it arrived.
     * @param[in] t  s on the follower's clock (the same as the leader's)
     */
    void advance( double t )
    {
        while ( m_next < m_grams.size() && m_grams[m_next].m_arrive <= t )
        {
            const Datagram &g = m_grams[m_next++];
            const int64_t   at = static_cast<int64_t>( std::llround( g.m_arrive * 1000.0 ) );
            if ( g.m_kind == Datagram::HELLO )
                m_seenMs = at;
            else if ( g.m_kind == Datagram::KEY )
            {
                m_root = g.m_root;
                m_seenMs = at;
            }
            else if ( m_hear )
            {
                JamNotes n;
                char     id[24];
                if ( !decodeJamNotes( g.m_bytes.data(), static_cast<int>( g.m_bytes.size() ), id, sizeof( id ), n ) )
                    ++m_malformed;
                else if ( !m_order.accept( id, n.m_seq, at ) )
                    ++m_stale;
                else
                {
                    m_held.take( n, at );
                    ++m_taken;
                }
            }
        }
    }
    /**
     * @brief The leader's root as the follower holds it at @p t (-1: no leader heard for 4 s).
     * @param[in] t  s
     * @return the pitch class, or -1
     */
    int root( double t ) const
    {
        const int64_t now = static_cast<int64_t>( std::llround( t * 1000.0 ) );
        return m_seenMs >= 0 && now - m_seenMs < 4000 ? m_root : -1;
    }
    /**
     * @brief The leader's notes as the follower holds them at @p t (none unless heard; forgotten after 2 s).
     * @param[in]  t   s
     * @param[out] hz  receives them (room for JamNotes::MaxNotes)
     * @return how many
     */
    int notes( double t, double *hz ) const
    {
        if ( !m_hear ) return 0;
        return m_held.notesAt( static_cast<int64_t>( std::llround( t * 1000.0 ) ), hz );
    }
    /**
     * @brief The run's counts, as a line `# jam sent <n> lost <n> late <n> malformed-sent <n> taken <n> stale <n>
     *        malformed <n>`.
     * @return the line
     */
    std::string summary() const
    {
        char b[256];
        std::snprintf( b, sizeof( b ), "# jam sent %d lost %d late %d malformed-sent %d taken %d stale %d malformed %d",
                       m_sent, m_lost, m_late, m_badSent, m_taken, m_stale, m_malformed );
        return b;
    }

private:
    /** @brief One datagram on its way. */
    struct Datagram
    {
        /** @brief What a datagram says. */
        enum KindE
        {
            HELLO,   ///< /family/hello: the leader is there
            KEY,     ///< /family/key: its root
            NOTES    ///< /family/notes: its sounding notes
        };
        KindE             m_kind = HELLO;   ///< what it says
        double            m_arrive = 0.0;   ///< when it arrives, s
        int               m_root = -1;      ///< a key's root
        std::vector<char> m_bytes;          ///< a notes message, as encodeJamNotes() wrote it
    };

    /**
     * @brief Sends one datagram at @p t: lost, or scheduled with its latency (and, for notes, maybe cut short).
     * @param[in]     t     the send time, s
     * @param[in]     kind  what it is
     * @param[in]     root  a key's root
     * @param[in]     n     a notes message's set, or nullptr
     * @param[in]     net   the network
     * @param[in,out] rng   the dice
     * @param[in,out] u     a uniform distribution on [0, 1)
     */
    void send( double t, Datagram::KindE kind, int root, const JamNotes *n, const JamNetwork &net,
               std::mt19937_64 &rng, std::uniform_real_distribution<double> &u )
    {
        ++m_sent;
        // every die is thrown whatever the outcome, so the stream does not depend on what a datagram carries
        const double lose = u( rng ), tail = u( rng ), late = u( rng ), lateBy = u( rng ), bad = u( rng );
        if ( lose < net.m_loss )
        {
            ++m_lost;
            return;
        }
        Datagram g;
        g.m_kind = kind;
        g.m_root = root;
        double ms = net.m_baseMs - net.m_tailMs * std::log( 1.0 - tail );
        if ( late < net.m_lateShare )
        {
            ms += 200.0 + 1300.0 * lateBy;
            ++m_late;
        }
        g.m_arrive = t + ms / 1000.0;
        if ( n != nullptr )
        {
            char      bytes[JamNotes::MaxBytes];
            const int len = encodeJamNotes( bytes, sizeof( bytes ), "e3leader", *n );
            int       keep = len;
            if ( bad < net.m_badShare && len > 4 )
            {
                keep = len - 4;
                ++m_badSent;
            }
            g.m_bytes.assign( bytes, bytes + keep );
        }
        m_grams.push_back( std::move( g ) );
    }

    bool                  m_hear = false;    ///< the follower hears the notes
    std::vector<Datagram> m_grams;           ///< every datagram that arrives, in the order it arrives
    size_t                m_next = 0;        ///< the next to take
    int                   m_root = -1;       ///< the root last heard
    int64_t               m_seenMs = -1;     ///< when the leader was last heard (hello or key), ms
    JamNotesOrder         m_order;           ///< the follower's filter on the numbers
    JamNotesHeld          m_held;            ///< what it holds
    int m_sent = 0;        ///< datagrams the leader sent
    int m_lost = 0;        ///< of them, lost on the way
    int m_late = 0;        ///< of them, held back long enough to arrive after newer ones
    int m_badSent = 0;     ///< of them, cut short (malformed)
    int m_taken = 0;       ///< notes messages the follower took
    int m_stale = 0;       ///< notes messages it turned away as late or doubled
    int m_malformed = 0;   ///< notes messages it turned away as malformed
};

}   // namespace frame
