/**
 * @file WavFile.h
 * @brief Minimal WAV reader (PCM 8/16/24/32 and 32-bit float, any channel
 *        count mixed to mono).
 *
 * Framework-free so the render tool and the Quest app can load
 * textures and wavetables; the plugin uses JUCE's readers for other formats.
 *
 * Every reader here goes through resolveAudioFile first, so a preset that names a .wav is served
 * the .flac that ships beside it without knowing; FLAC is decoded through dr_flac (one translation
 * unit, WavFile.cpp). All of it runs on a loader or message thread -- a decode takes milliseconds
 * and allocates -- and never on the audio thread; what comes out is a plain block of floats that
 * the Texture, the user wavetable and the convolution Room then read without further ado.
 */
#pragma once
#include <string>
#include <vector>

namespace ambient {

/**
 * @brief Which file a reference actually means.
 *
 * A pack names its clip as it lies in the source library
 * ("../Textures/a_bell.wav"); what ships is the same audio as FLAC at half the download. Returns
 * the named file if it is there, else the .flac beside it, else an empty string -- so a caller can
 * ask "is this loadable" without knowing which of the two spellings arrived.
 *
 * Only opens the file to see that it exists; nothing is decoded.
 *
 * @param path  the path as the preset or pack names it; nullptr or empty gives an empty string
 * @return      the path that exists (@p path itself or its .flac twin), or "" when neither does
 */
std::string resolveAudioFile(const char* path);

/**
 * @brief Returns false if the file is missing or not a WAV the reader understands.
 *
 * Reads a file and folds every channel into one, each at 1/channels. Goes through
 * readWavChannels, so WAV and FLAC are both accepted and the .flac beside a named .wav counts.
 *
 * @param path        the file as named (see resolveAudioFile)
 * @param mono        receives the mixed-down samples; cleared on failure
 * @param sampleRate  receives the file's sample rate in Hz (untouched on failure)
 * @return            true when at least one sample was read
 */
bool readWavMono(const char* path, std::vector<float>& mono, int& sampleRate);

/**
 * @brief A clip's two channels for a texture slot: `left` always, `right` empty when the file was mono
 *        (and when it had more than two channels, which are folded into the two).
 *
 * Every caller that loads
 * a texture wants exactly this, and each of them used to fold to mono on its own.
 *
 * With more than two channels the odd ones are averaged to the left and the even ones to the right,
 * so nothing a player's own file holds is quietly dropped.
 *
 * @param path        the file as named (see resolveAudioFile)
 * @param left        receives the left (or only) channel; cleared first
 * @param right       receives the right channel, or stays empty for a mono file
 * @param sampleRate  receives the file's sample rate in Hz
 * @return            false when the file is missing, unreadable or empty
 */
bool readWavStereo(const char* path, std::vector<float>& left, std::vector<float>& right, int& sampleRate);

/**
 * @brief All channels, deinterleaved (impulse responses keep their stereo).
 *
 * The one place that actually parses: RIFF/WAVE with a "fmt " chunk (WAVE_FORMAT_EXTENSIBLE is
 * unwrapped to its sub-format), PCM at 8, 16, 24 or 32 bits or 32-bit float, and a "data" chunk
 * whose stated length is clipped to what the file really holds, so a recording cut off by pulling
 * the plug (length 0xFFFFFFFF) reads to its end instead of asking for sixteen gigabytes. A file
 * whose first four bytes say "fLaC" is decoded as FLAC whatever its name.
 *
 * @param path        the file as named (see resolveAudioFile)
 * @param channels    receives one vector per channel, all the same length; cleared first
 * @param sampleRate  receives the file's sample rate in Hz
 * @return            true when at least one frame was read
 */
bool readWavChannels(const char* path, std::vector<std::vector<float>>& channels, int& sampleRate);

/**
 * @brief A wavetable file: its samples folded to mono, and how long one cycle is in them.
 *
 * The length comes
 * from the file where the file says it -- the "clm " chunk Serum writes and Vital copies, Surge's
 * "srge" chunk, the header of Surge's own .wt format -- and is otherwise worked out from the samples
 * (detectCycleLength, CycleTable.h). WAV, FLAC and .wt.
 *
 * A frame length written into the file name ("-WT512", "_wt1024") is honoured after the chunks and
 * before the detector, and a stated length longer than the file is ignored.
 *
 * @param path      the file as named (see resolveAudioFile); a Surge .wt is tried first
 * @param mono      receives the samples, consecutive cycles end to end; cleared on failure
 * @param cycleLen  receives the samples per cycle, 0 on failure
 * @return          true when samples were read and a cycle length above zero was found
 */
bool readWavetableFile(const char* path, std::vector<float>& mono, int& cycleLen);

} // namespace ambient
