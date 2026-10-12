#pragma once
// Audio files used by audio clips, decoded into memory and resampled to the
// device sample rate (so the audio thread only copies samples). Also keeps a
// small peak overview for drawing waveforms.
//
// Decoding runs on background threads: find() never blocks, request() starts
// loading and onLoaded is called on the message thread when a file is ready.
// The public functions are message-thread only.
#include <juce_audio_formats/juce_audio_formats.h>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace mc::audio {

struct AudioFileData {
    juce::AudioBuffer<float> audio;   // resampled to `sampleRate`, 1 or 2 channels
    double sampleRate = 0;            // rate of `audio` (the device rate when loaded)
    double lengthSeconds = 0;
    // Waveform overview: max |sample| (mono) per 1/kPeaksPerSecond seconds.
    static constexpr double kPeaksPerSecond = 400.0;
    std::vector<float> peaks;
    float peakAt(double fromSec, double toSec) const; // max over a time range (for drawing)
};

class AudioFileCache {
public:
    AudioFileCache();
    ~AudioFileCache();

    // Ready data for the file at this rate, or nullptr (not loaded yet, loading or failed).
    std::shared_ptr<const AudioFileData> find(const std::string& path, double sampleRate);
    // Starts decoding in the background unless it is ready, loading or known to fail.
    void request(const std::string& path, double sampleRate);
    bool isLoading(const std::string& path) const { return loading.count(path) > 0; }
    bool isMissing(const std::string& path) const { return failed.count(path) > 0; }
    void forget(const std::string& path) { cache.erase(path); failed.erase(path); }
    // Drops decoded files that are no longer used (keeps memory in check after edits).
    void retainOnly(const std::set<std::string>& used) { std::erase_if(cache, [&](auto& e) { return !used.count(e.first); }); }

    std::function<void()> onLoaded; // message thread, after one or more files finished loading

    // Length in seconds from the file header (0 if unreadable).
    double fileLength(const juce::File&);
    bool canRead(const juce::File&) const;
    juce::String wildcard() const { return formats.getWildcardForAllFormats(); }

    juce::AudioFormatManager formats;

private:
    // Decodes and resamples; runs on a pool thread. nullptr if the file is unusable.
    static std::shared_ptr<AudioFileData> decode(const std::string& path, double sampleRate);
    void setRate(double sampleRate);

    std::map<std::string, std::shared_ptr<const AudioFileData>> cache;
    std::set<std::string> failed, loading;
    double rate = 0;
    juce::ThreadPool pool{2};
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace mc::audio
