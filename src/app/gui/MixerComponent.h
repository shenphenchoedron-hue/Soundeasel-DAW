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
    // grows with the longest plugin chain, so every plugin on every strip can be seen
    int preferredHeight() const;
    std::function<void()> onClose;
    std::function<void()> onPreferredHeightChanged;

    static constexpr int kStripWidth = 98, kHeaderHeight = 28, kPreferredHeight = 520;
    class Strip;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();
    void setInsertsFolded(bool);
    void updateFoldButton();
    void applyInsertLayout();

    AppContext& app;
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Strip>> strips;
    std::unique_ptr<Strip> master;
    int insertRows = 0; // plugin rows on every strip: the longest chain (all strips stay aligned)
    juce::TextButton closeBtn{"Hide"};
    juce::TextButton foldBtn;
    bool insertsFolded = false; // FX plugin lists folded to one line on every strip (saved in the settings)
};

} // namespace mc::gui
