#pragma once
// Format-agnostic plugin interface (instruments and audio effects). Tracks,
// sequencer and engine only ever see this; VST3/AU/LV2/CLAP details live in
// concrete hosts.
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include <string>

namespace mc::plugins {

// Result of a scan; persisted in the plugin cache.
struct PluginInfo {
    std::string format;        // "VST3", "AudioUnit", "LV2", "CLAP", "Internal"
    std::string identifier;    // unique, stable id within the host
    std::string name;
    std::string manufacturer;
    std::string category;
    std::string hostData;      // opaque, host specific (e.g. JUCE PluginDescription XML)
    bool isInstrument = true;  // false: audio effect (used as an insert on a channel strip)
};

class InstrumentPlugin {
public:
    virtual ~InstrumentPlugin() = default;

    virtual std::string name() const = 0;
    virtual std::string format() const = 0;

    // Message thread
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void release() = 0;
    virtual bool hasEditor() const = 0;
    virtual juce::Component* createEditor() = 0;   // caller owns the returned component
    // Message thread, while the plugin is still being processed by the audio thread and
    // just before it is taken out of the engine (lets e.g. CLAP plugins stop processing
    // on the audio thread, as their spec requires).
    virtual void prepareToRemove() {}
    virtual juce::MemoryBlock getState() = 0;
    virtual void setState(const juce::MemoryBlock&) = 0;

    // Audio thread; must not allocate. `audio` has >= 2 channels. Instruments get a
    // cleared buffer; effects get the track's audio and process it in place.
    virtual void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) = 0;
};

// One per plugin technology (or family, e.g. everything JUCE can host).
class InstrumentPluginHost {
public:
    virtual ~InstrumentPluginHost() = default;
    virtual std::string hostName() const = 0;
    virtual juce::StringArray formats() const = 0;
    virtual bool handles(const std::string& format) const { return formats().contains(format); }

    // Called from a background thread. Must survive (skip) defective plugins.
    // `progress(fraction, currentItem)` returns false to cancel.
    virtual std::vector<PluginInfo> scan(const std::function<bool(float, const juce::String&)>& progress) = 0;

    virtual bool stillExists(const PluginInfo&) const = 0;
    virtual std::unique_ptr<InstrumentPlugin> create(const PluginInfo&, double sampleRate, int blockSize, juce::String& error) = 0;
};

} // namespace mc::plugins
