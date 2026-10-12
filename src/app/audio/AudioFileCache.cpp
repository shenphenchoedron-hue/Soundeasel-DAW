#include "audio/AudioFileCache.h"
#include <juce_events/juce_events.h>

namespace mc::audio {

namespace {
// Anything outside this range is a broken (or malicious) file header.
constexpr double kMinRate = 1000.0, kMaxRate = 768000.0;
constexpr juce::int64 kMaxSamples = (juce::int64)1 << 30; // per channel, after resampling (~6 h at 48 kHz)
}

float AudioFileData::peakAt(double fromSec, double toSec) const
{
    if (peaks.empty()) return 0.0f;
    auto i0 = (size_t)std::max(0.0, std::floor(fromSec * kPeaksPerSecond));
    auto i1 = (size_t)std::max(0.0, std::ceil(toSec * kPeaksPerSecond));
    i1 = std::min(std::max(i1, i0 + 1), peaks.size());
    float m = 0.0f;
    for (auto i = i0; i < i1; ++i) m = std::max(m, peaks[i]);
    return m;
}

AudioFileCache::AudioFileCache() { formats.registerBasicFormats(); }

AudioFileCache::~AudioFileCache()
{
    *alive = false;
    pool.removeAllJobs(true, 10000);
}

bool AudioFileCache::canRead(const juce::File& f) const
{
    return f.existsAsFile() && formats.findFormatForFileExtension(f.getFileExtension()) != nullptr;
}

double AudioFileCache::fileLength(const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r(formats.createReaderFor(f));
    if (!r || r->sampleRate < kMinRate || r->sampleRate > kMaxRate || r->lengthInSamples <= 0) return 0.0;
    return (double)r->lengthInSamples / r->sampleRate;
}

void AudioFileCache::setRate(double sampleRate)
{
    if (sampleRate == rate) return;
    // New device rate: everything must be resampled again (results for the old rate are dropped).
    cache.clear();
    failed.clear();
    loading.clear();
    rate = sampleRate;
}

std::shared_ptr<const AudioFileData> AudioFileCache::find(const std::string& path, double sampleRate)
{
    setRate(sampleRate > 0 ? sampleRate : 44100.0);
    auto it = cache.find(path);
    return it == cache.end() ? nullptr : it->second;
}

void AudioFileCache::request(const std::string& path, double sampleRate)
{
    setRate(sampleRate > 0 ? sampleRate : 44100.0);
    if (cache.count(path) || failed.count(path) || loading.count(path)) return;
    loading.insert(path);
    const double sr = rate;
    std::weak_ptr<bool> token = alive;
    pool.addJob([this, path, sr, token] {
        auto data = decode(path, sr);
        juce::MessageManager::callAsync([this, path, sr, token, data] {
            const auto ok = token.lock();
            if (!ok || !*ok) return;     // the cache is gone
            if (sr != rate || !loading.erase(path)) return; // rate changed meanwhile: a new request will follow
            if (data) cache[path] = data;
            else failed.insert(path);
            if (onLoaded) onLoaded();
        });
    });
}

std::shared_ptr<AudioFileData> AudioFileCache::decode(const std::string& path, double sampleRate)
{
    juce::AudioFormatManager fm; // own instance: safe on any thread
    fm.registerBasicFormats();
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    std::unique_ptr<juce::AudioFormatReader> reader(file.existsAsFile() ? fm.createReaderFor(file) : nullptr);
    if (!reader || reader->lengthInSamples <= 0) return nullptr;
    const double fileRate = reader->sampleRate;
    if (!(fileRate >= kMinRate && fileRate <= kMaxRate)) return nullptr;

    // Sizes in 64 bit, capped before anything is allocated.
    const juce::int64 len64 = reader->lengthInSamples;
    const juce::int64 outLen64 = (juce::int64)std::ceil((double)len64 * sampleRate / fileRate);
    if (len64 > kMaxSamples || outLen64 > kMaxSamples) return nullptr;
    const int len = (int)len64;

    const int channels = (int)juce::jlimit(1u, 2u, reader->numChannels);
    juce::AudioBuffer<float> raw(channels, len);
    reader->read(&raw, 0, len, 0, true, channels > 1);

    auto d = std::make_shared<AudioFileData>();
    d->lengthSeconds = (double)len / fileRate;
    d->sampleRate = sampleRate;

    // peaks (from the original data)
    const double perPeak = fileRate / AudioFileData::kPeaksPerSecond;
    const auto numPeaks = (size_t)std::max<juce::int64>(1, (juce::int64)std::ceil((double)len / perPeak));
    d->peaks.assign(numPeaks, 0.0f);
    for (int c = 0; c < channels; ++c) {
        const float* s = raw.getReadPointer(c);
        for (int i = 0; i < len; ++i) {
            auto& p = d->peaks[std::min(numPeaks - 1, (size_t)(i / perPeak))];
            p = std::max(p, std::abs(s[i]));
        }
    }

    // resample to the device rate
    if (std::abs(fileRate - sampleRate) < 0.5) {
        d->audio = std::move(raw);
    } else {
        const double ratio = fileRate / sampleRate;
        const int outLen = (int)outLen64;
        d->audio.setSize(channels, outLen);
        juce::HeapBlock<float> in((size_t)len + 64, true); // padded so the interpolator never reads past the end
        for (int c = 0; c < channels; ++c) {
            juce::WindowedSincInterpolator interp;
            juce::FloatVectorOperations::copy(in.get(), raw.getReadPointer(c), len);
            juce::FloatVectorOperations::clear(in.get() + len, 64);
            interp.process(ratio, in.get(), d->audio.getWritePointer(c), outLen, len + 64, 0);
        }
    }
    return d;
}

} // namespace mc::audio
