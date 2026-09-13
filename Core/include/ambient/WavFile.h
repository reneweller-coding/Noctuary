// Noctuary -- minimal WAV reader (PCM 8/16/24/32 and 32-bit float, any channel
// count mixed to mono). Framework-free so the render tool and the Quest app can load
// textures and wavetables; the plugin uses JUCE's readers for other formats.
#pragma once
#include <string>
#include <vector>

namespace ambient {

// Returns false if the file is missing or not a WAV the reader understands.
// Which file a reference actually means. A pack names its clip as it lies in the source library
// ("../Textures/a_bell.wav"); what ships is the same audio as FLAC at half the download. Returns
// the named file if it is there, else the .flac beside it, else an empty string -- so a caller can
// ask "is this loadable" without knowing which of the two spellings arrived.
std::string resolveAudioFile(const char* path);

bool readWavMono(const char* path, std::vector<float>& mono, int& sampleRate);
// A clip's two channels for a texture slot: `left` always, `right` empty when the file was mono
// (and when it had more than two channels, which are folded into the two). Every caller that loads
// a texture wants exactly this, and each of them used to fold to mono on its own.
bool readWavStereo(const char* path, std::vector<float>& left, std::vector<float>& right, int& sampleRate);
// All channels, deinterleaved (impulse responses keep their stereo).
bool readWavChannels(const char* path, std::vector<std::vector<float>>& channels, int& sampleRate);
// A wavetable file: its samples folded to mono, and how long one cycle is in them. The length comes
// from the file where the file says it -- the "clm " chunk Serum writes and Vital copies, Surge's
// "srge" chunk, the header of Surge's own .wt format -- and is otherwise worked out from the samples
// (detectCycleLength, CycleTable.h). WAV, FLAC and .wt.
bool readWavetableFile(const char* path, std::vector<float>& mono, int& cycleLen);

} // namespace ambient
