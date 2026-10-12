#pragma once
// Mixer: one channel strip per track (piano roll, step sequencer lines and audio
// tracks, in project order) plus the Mains strip on the right. Every strip
// shows where its sound comes from (instrument or audio input), pan, mute/solo
// (and record enable on audio tracks), a fader and a level meter.
#include "AppContext.h"
#include "gui/LevelMeter.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace mc::gui {

class MixerComponent final : public juce::Component, private juce::ChangeListener {
public:
    explicit MixerComponent(AppContext&);
    ~MixerComponent() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refreshMeters();                 // UI timer
    std::function<void()> onClose;

    static constexpr int kStripWidth = 98, kHeaderHeight = 28, kPreferredHeight = 520;
    class Strip;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();

    AppContext& app;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Strip>> strips;
    std::unique_ptr<Strip> master;
    juce::TextButton closeBtn{"Hide"};
};

} // namespace mc::gui
