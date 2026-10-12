#pragma once
// CLAP host (instruments and audio effects). Uses the header-only CLAP SDK
// (third_party/clap, MIT). Bundles are loaded once and stay loaded for the
// session; scanning runs on the message thread and remembers plugins that
// crashed the app while being scanned, so they are skipped next time.
#include "plugins/InstrumentPlugin.h"

namespace mc::plugins {

class ClapPluginHost final : public InstrumentPluginHost {
public:
    explicit ClapPluginHost(juce::File crashedListFile = {}) : crashedList(std::move(crashedListFile)) {}
    std::string hostName() const override { return "CLAP"; }
    juce::StringArray formats() const override { return {"CLAP"}; }
    std::vector<PluginInfo> scan(const std::function<bool(float, const juce::String&)>&) override;
    bool stillExists(const PluginInfo& i) const override;
    std::unique_ptr<InstrumentPlugin> create(const PluginInfo&, double, int, juce::String& error) override;

    static juce::FileSearchPath defaultSearchPaths();

private:
    juce::File crashedList;
};

} // namespace mc::plugins
