#pragma once
/**
 * @file JamNotes.h
 * @brief The family jam's notes (plan E3, 20.11.2026): what a leader sounds, on the wire and in a follower's hands.
 *
 * Jam.h gives a follower the leader's root; which notes the leader plays, it never knew, and a follower's chord could
 * stand a semitone against the leader's. A leader now also says the notes its conductors sound, in hertz, as they
 * sound (a kept or retuned pitch as it is, not its key), and a follower whose "Hear leader" is on (Settings, Family
 * jam; off by default) gives them to its conductors as further notes of another (ClusterBrain::hearAlso).
 * @code
 *   /family/notes  ,sii f...   instance, sequence number, count, then count notes in Hz (float32)
 * @endcode
 * sent at once when the set changes and again every RepeatMs, each datagram with the next sequence number. A follower
 *  - ignores a datagram that is not exactly this (a wrong address or type tags, a count that does not match them or
 *    exceeds MaxNotes, bytes left over, a note that is not a finite number between 10 Hz and 24 kHz);
 *  - ignores one whose sequence number is not newer than the newest it took from that leader (late or doubled on the
 *    way); after ForgetMs of silence any number is new again (the leader started over);
 *  - forgets the notes ForgetMs after the last one it took: a leader gone, or a network that lost the messages, means
 *    the follower plays as if it heard nothing.
 *
 * Header-only and free of JUCE, so the jam's bus (Jam.cpp) and the offline jam of the tools (Tools/render, the
 * acceptance measurement) run the same encoding, the same checks and the same forgetting. Nothing here allocates.
 * @author Rene Weller
 */
#include <cmath>
#include <cstdint>
#include <cstring>

namespace frame
{

/** @brief One set of a leader's notes, as sent and as taken. */
struct JamNotes
{
    static constexpr int     MaxNotes = 32;     ///< the most notes in one message (the conductors' slots and more)
    static constexpr int64_t RepeatMs = 500;    ///< a leader sends its set again this often, changed or not
    static constexpr int64_t ForgetMs = 2000;   ///< a follower forgets the notes this long after the last message
    static constexpr int     MaxBytes = 256;    ///< room for the largest message, in bytes

    int32_t m_seq = 0;              ///< the sequence number
    int     m_count = 0;            ///< notes in m_hz
    float   m_hz[MaxNotes] = {};    ///< the notes, Hz

    /**
     * @brief Whether two sets hold the same notes in the same order (the sequence number aside).
     * @param[in] o  the other set
     * @return true when they do
     */
    bool sameNotes( const JamNotes &o ) const
    {
        if ( m_count != o.m_count ) return false;
        for ( int k = 0; k < m_count; ++k )
            if ( m_hz[k] != o.m_hz[k] ) return false;
        return true;
    }
};

namespace jamwire
{

/**
 * @brief Appends an OSC string (the text, its terminator, nulls to the next multiple of four); false without room.
 * @param[out]    out  the message
 * @param[in]     cap  its room in bytes
 * @param[in,out] pos  the write position
 * @param[in]     s    the text
 * @return false when there is no room
 */
inline bool putString( char *out, int cap, int &pos, const char *s )
{
    const int n = static_cast<int>( std::strlen( s ) ) + 1;
    const int padded = ( n + 3 ) & ~3;
    if ( pos + padded > cap ) return false;
    std::memcpy( out + pos, s, static_cast<size_t>( n ) );
    for ( int k = n; k < padded; ++k ) out[pos + k] = 0;
    pos += padded;
    return true;
}

/**
 * @brief Appends a big-endian 32-bit word; false without room.
 * @param[out]    out  the message
 * @param[in]     cap  its room in bytes
 * @param[in,out] pos  the write position
 * @param[in]     v    the word
 * @return false when there is no room
 */
inline bool putWord( char *out, int cap, int &pos, uint32_t v )
{
    if ( pos + 4 > cap ) return false;
    out[pos] = static_cast<char>( v >> 24 );
    out[pos + 1] = static_cast<char>( v >> 16 );
    out[pos + 2] = static_cast<char>( v >> 8 );
    out[pos + 3] = static_cast<char>( v );
    pos += 4;
    return true;
}

/**
 * @brief Reads a big-endian 32-bit word; false past the end.
 * @param[in]     d     the datagram
 * @param[in]     size  its length in bytes
 * @param[in,out] pos   the read position
 * @param[out]    v     receives the word
 * @return false past the end
 */
inline bool getWord( const char *d, int size, int &pos, uint32_t &v )
{
    if ( pos + 4 > size ) return false;
    const auto *u = reinterpret_cast<const unsigned char *>( d + pos );
    v = ( static_cast<uint32_t>( u[0] ) << 24 ) | ( static_cast<uint32_t>( u[1] ) << 16 ) |
        ( static_cast<uint32_t>( u[2] ) << 8 ) | static_cast<uint32_t>( u[3] );
    pos += 4;
    return true;
}

/**
 * @brief Reads an OSC string into @p out; false when it is unterminated, runs past the end or does not fit.
 * @param[in]     d     the datagram
 * @param[in]     size  its length in bytes
 * @param[in,out] pos   the read position, moved past the string and its padding
 * @param[out]    out   receives the text
 * @param[in]     cap   the room in out, in bytes
 * @return false when the string is malformed or longer than @p cap - 1
 */
inline bool getString( const char *d, int size, int &pos, char *out, int cap )
{
    int end = pos;
    while ( end < size && d[end] != 0 ) ++end;
    if ( end >= size || end - pos > cap - 1 ) return false;
    std::memcpy( out, d + pos, static_cast<size_t>( end - pos ) );
    out[end - pos] = 0;
    pos = ( end + 4 ) & ~3;
    return pos <= size;
}

}   // namespace jamwire

/** @brief The address of the notes' message. */
inline constexpr const char *JamNotesAddress = "/family/notes";

/**
 * @brief Writes the message for @p notes from instance @p id.
 * @param[out] out    the message
 * @param[in]  cap    its room (JamNotes::MaxBytes is enough for an id of up to 23 characters)
 * @param[in]  id     the leader's instance
 * @param[in]  notes  the set and its sequence number (at most MaxNotes are written)
 * @return the message's length in bytes, 0 when it does not fit
 */
inline int encodeJamNotes( char *out, int cap, const char *id, const JamNotes &notes )
{
    const int n = notes.m_count < 0 ? 0 : notes.m_count > JamNotes::MaxNotes ? JamNotes::MaxNotes : notes.m_count;
    char      tags[4 + JamNotes::MaxNotes + 1] = ",sii";
    for ( int k = 0; k < n; ++k ) tags[4 + k] = 'f';
    tags[4 + n] = 0;
    int pos = 0;
    if ( !jamwire::putString( out, cap, pos, JamNotesAddress ) || !jamwire::putString( out, cap, pos, tags ) ||
         !jamwire::putString( out, cap, pos, id ) ||
         !jamwire::putWord( out, cap, pos, static_cast<uint32_t>( notes.m_seq ) ) ||
         !jamwire::putWord( out, cap, pos, static_cast<uint32_t>( n ) ) )
        return 0;
    for ( int k = 0; k < n; ++k )
    {
        uint32_t w;
        std::memcpy( &w, &notes.m_hz[k], 4 );
        if ( !jamwire::putWord( out, cap, pos, w ) ) return 0;
    }
    return pos;
}

/**
 * @brief Reads a notes message; false for anything that is not exactly one (see the file's comment).
 * @param[in]  d      the datagram
 * @param[in]  size   its length in bytes
 * @param[out] id     receives the sender's instance
 * @param[in]  idCap  the room in @p id
 * @param[out] notes  receives the set and its sequence number
 * @return true when it is a well-formed notes message
 */
inline bool decodeJamNotes( const char *d, int size, char *id, int idCap, JamNotes &notes )
{
    if ( d == nullptr || size <= 0 || size > 4096 || ( size & 3 ) != 0 ) return false;
    int  pos = 0;
    char address[16], tags[4 + JamNotes::MaxNotes + 2];
    if ( !jamwire::getString( d, size, pos, address, sizeof( address ) ) ||
         std::strcmp( address, JamNotesAddress ) != 0 || !jamwire::getString( d, size, pos, tags, sizeof( tags ) ) ||
         std::strncmp( tags, ",sii", 4 ) != 0 )
        return false;
    const int n = static_cast<int>( std::strlen( tags ) ) - 4;
    if ( n < 0 || n > JamNotes::MaxNotes ) return false;
    for ( int k = 0; k < n; ++k )
        if ( tags[4 + k] != 'f' ) return false;
    uint32_t seq = 0, count = 0;
    if ( !jamwire::getString( d, size, pos, id, idCap ) || id[0] == 0 || !jamwire::getWord( d, size, pos, seq ) ||
         !jamwire::getWord( d, size, pos, count ) || count != static_cast<uint32_t>( n ) )
        return false;
    for ( int k = 0; k < n; ++k )
    {
        uint32_t w = 0;
        if ( !jamwire::getWord( d, size, pos, w ) ) return false;
        float hz;
        std::memcpy( &hz, &w, 4 );
        if ( !std::isfinite( hz ) || hz < 10.0f || hz > 24000.0f ) return false;
        notes.m_hz[k] = hz;
    }
    if ( pos != size ) return false;   // bytes left over: not ours
    notes.m_seq = static_cast<int32_t>( seq );
    notes.m_count = n;
    return true;
}

/**
 * @brief A follower's filter on the sequence numbers of one leader: only what is newer than the newest taken passes,
 *        and after JamNotes::ForgetMs of silence anything does.
 */
class JamNotesOrder
{
public:
    /**
     * @brief Whether a message from @p id with number @p seq, arriving at @p nowMs, is to be taken; takes it if so.
     * @param[in] id     the sender's instance
     * @param[in] seq    its sequence number
     * @param[in] nowMs  the arrival, ms on the follower's clock
     * @return true when it is newer than everything taken from that leader, or the leader was silent long enough
     */
    bool accept( const char *id, int32_t seq, int64_t nowMs )
    {
        const bool fresh = m_have && nowMs - m_lastMs < JamNotes::ForgetMs;
        if ( fresh && std::strcmp( id, m_id ) != 0 ) return false;   // one leader at a time
        if ( fresh && static_cast<int32_t>( static_cast<uint32_t>( seq ) - static_cast<uint32_t>( m_seq ) ) <= 0 )
            return false;   // late, or doubled on the way
        size_t k = 0;
        for ( ; k + 1 < sizeof( m_id ) && id[k] != 0; ++k ) m_id[k] = id[k];
        m_id[k] = 0;
        m_seq = seq;
        m_lastMs = nowMs;
        m_have = true;
        return true;
    }
    /** @brief Forgets the leader: the next message is taken whatever its number. */
    void clear() { m_have = false; }

private:
    char    m_id[24] = {};    ///< the leader taken from
    int32_t m_seq = 0;        ///< the newest number taken
    int64_t m_lastMs = 0;     ///< when it arrived
    bool    m_have = false;   ///< whether anything was taken
};

/** @brief What a follower holds of the leader's notes, and for how long (the audio thread's side). */
class JamNotesHeld
{
public:
    /**
     * @brief Takes a set that arrived at @p atMs.
     * @param[in] notes  the set
     * @param[in] atMs   its arrival, ms
     */
    void take( const JamNotes &notes, int64_t atMs )
    {
        m_notes = notes;
        m_atMs = atMs;
        m_have = true;
    }
    /**
     * @brief The notes as they stand at @p nowMs, none once JamNotes::ForgetMs have passed since the last set.
     * @param[in]  nowMs  the time, ms
     * @param[out] hz     receives the notes (room for JamNotes::MaxNotes)
     * @return how many
     */
    int notesAt( int64_t nowMs, double *hz ) const
    {
        if ( !m_have || nowMs - m_atMs >= JamNotes::ForgetMs ) return 0;
        for ( int k = 0; k < m_notes.m_count; ++k ) hz[k] = static_cast<double>( m_notes.m_hz[k] );
        return m_notes.m_count;
    }
    /** @brief Forgets everything. */
    void clear() { m_have = false; }

private:
    JamNotes m_notes;           ///< the last set taken
    int64_t  m_atMs = 0;        ///< when it arrived
    bool     m_have = false;    ///< whether one was taken
};

}   // namespace frame
