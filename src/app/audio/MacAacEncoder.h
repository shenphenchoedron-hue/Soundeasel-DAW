#pragma once
// AAC audio in an .m4a (MPEG-4) file, encoded by the AAC encoder built into macOS
// (Core Audio). Apple platforms only. Kept free of JUCE so the Core Audio headers
// never meet JUCE's in the same translation unit.
#include <memory>
#include <string>

namespace mc::audio {

class MacAacEncoder {
public:
    ~MacAacEncoder();
    // bitrate in bits per second (e.g. 256000). Sample rates up to 48 kHz. nullptr + error on failure.
    static std::unique_ptr<MacAacEncoder> create(const std::string& path, double sampleRate, int numChannels,
                                                 int bitrate, std::string& error);
    // numChannels non-interleaved float channels, as passed to create().
    bool write(const float* const* channels, int numSamples, std::string& error);
    // Encodes the rest and closes the file. Called by the destructor if needed (errors are lost then).
    bool close(std::string& error);

private:
    MacAacEncoder() = default;
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace mc::audio
