/**
 * @file NameList.h
 * @brief A list of preset names in a plain text file, one a line: the player's "never again" for sound presets
 *        (20.11.2026, plan C2, the counterpart to the favourites; Empyrean's NameList.h of the same day). Read so that a
 *        missing, empty or garbled file is harmless, written so that a crash in the middle never leaves half a file.
 *
 * The instrument keeps it beside the favourites (Documents/Noctuary/never_again.txt), the render app reads it from
 * there; the night director and the journeys pass over the names in it. Reading never writes; the file is written only
 * when the player changes the list. AMBIENT_NEVER_AGAIN_FILE names another file instead (the selftest's, the
 * screenshots'), so the player's own list is never touched by them.
 * @author Rene Weller
 */
#pragma once

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace ambient
{

/**
 * @brief Whether a line can be a preset's name: 1 to 200 bytes of well-formed UTF-8 without control characters.
 * @param[in] s  the line, trimmed
 * @return true when it is kept
 */
inline bool nameLineOk( const std::string &s )
{
    if ( s.empty() || s.size() > 200 ) return false;
    for ( size_t i = 0; i < s.size(); )
    {
        const unsigned char c = static_cast<unsigned char>( s[i] );
        const int           more = c < 0x80               ? 0
                                   : ( c & 0xE0 ) == 0xC0 ? 1
                                   : ( c & 0xF0 ) == 0xE0 ? 2
                                   : ( c & 0xF8 ) == 0xF0 ? 3
                                                          : -1;
        if ( more < 0 || c < 0x20 || c == 0x7F || ( more == 1 && c < 0xC2 ) ) return false;   // control, overlong
        if ( i + static_cast<size_t>( more ) >= s.size() ) return false;                      // cut off
        for ( int k = 1; k <= more; ++k )
            if ( ( static_cast<unsigned char>( s[i + static_cast<size_t>( k )] ) & 0xC0 ) != 0x80 ) return false;
        i += 1 + static_cast<size_t>( more );
    }
    return true;
}

/**
 * @brief A UTF-8 path as the file system wants it.
 * @param[in] path  the path, UTF-8
 * @return the path
 */
inline std::filesystem::path utf8Path( const std::string &path )
{
    return std::filesystem::path( std::u8string( reinterpret_cast<const char8_t *>( path.data() ), path.size() ) );
}

/**
 * @brief Reads a list of names: one a line, trimmed; empty lines, lines beginning with '#', lines that cannot be a
 *        name (nameLineOk) and repeats are passed over; a byte order mark and CR LF are fine.
 * @param[in] path  the file (UTF-8 path)
 * @return the names in the file's order; empty when the file is missing or unreadable
 */
inline std::vector<std::string> readNameList( const std::string &path )
{
    std::vector<std::string> out;
    std::ifstream            in( utf8Path( path ), std::ios::binary );
    if ( !in ) return out;
    std::string line;
    bool        first = true;
    while ( out.size() < 100000 && std::getline( in, line ) )
    {
        if ( first && line.size() >= 3 && line.compare( 0, 3, "\xEF\xBB\xBF" ) == 0 ) line.erase( 0, 3 );
        first = false;
        const size_t a = line.find_first_not_of( " \t\r" ), b = line.find_last_not_of( " \t\r" );
        if ( a == std::string::npos ) continue;
        line = line.substr( a, b - a + 1 );
        if ( line[0] == '#' ) continue;
        if ( nameLineOk( line ) && std::find( out.begin(), out.end(), line ) == out.end() ) out.push_back( line );
    }
    return out;
}

/**
 * @brief Writes a list of names, one a line (LF) after a comment line saying what the file is, all at once: into a
 *        file beside it first (.new), which then takes the list's place -- the old list stays whole if anything fails
 *        on the way. The folder is made when missing.
 * @param[in] path   the file (UTF-8 path)
 * @param[in] names  the names; one that cannot be a name (nameLineOk) is left out
 * @return true when the file holds the list
 */
inline bool writeNameList( const std::string &path, const std::vector<std::string> &names )
{
    const std::filesystem::path target = utf8Path( path );
    std::filesystem::path       temp = target;
    temp += ".new";
    std::error_code ec;
    if ( target.has_parent_path() ) std::filesystem::create_directories( target.parent_path(), ec );
    {
        std::ofstream out( temp, std::ios::binary | std::ios::trunc );
        if ( !out ) return false;
        out << "# Noctuary: sound presets never again, one name a line. The night director and the journeys pass "
               "over them.\n";
        for ( const std::string &n : names )
            if ( nameLineOk( n ) && n[0] != '#' ) out << n << '\n';
        out.flush();
        if ( !out ) return false;
    }
    std::filesystem::rename( temp, target, ec );   // replaces the old file in one step (MoveFileEx on Windows)
    if ( ec )
    {
        std::filesystem::remove( temp, ec );
        return false;
    }
    return true;
}

/**
 * @brief Where the "never again" list lies: AMBIENT_NEVER_AGAIN_FILE when it is set (tests, screenshots), else
 *        never_again.txt in the Noctuary folder of the player's documents, beside favourites.txt.
 * @param[in] documents  the player's documents folder (UTF-8)
 * @return the file's path (UTF-8)
 */
inline std::string neverAgainPath( const std::string &documents )
{
    if ( const char *env = std::getenv( "AMBIENT_NEVER_AGAIN_FILE" ) )
        if ( *env != 0 ) return env;
    const std::u8string u = ( utf8Path( documents ) / "Noctuary" / "never_again.txt" ).u8string();
    return std::string( u.begin(), u.end() );
}

}   // namespace ambient
