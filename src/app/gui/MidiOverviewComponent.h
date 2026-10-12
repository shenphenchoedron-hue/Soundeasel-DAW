#pragma once
// Compact view of the piano roll (instrument) tracks, shown while the piano roll
// is closed: per track a small card (name, mute/solo, instrument, level) and a
// mini version of its notes, aligned with the time grid of the other lanes.
#include "AppContext.h"
#include "gui/PianoRollComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace mc::gui {

class MidiOverviewComponent final : public juce::Component, private juce::ChangeListener {
public:
    MidiOverviewComponent(AppContext&, PianoRollComponent&);
    ~MidiOverviewComponent() override;
    void resized() override;
    void paint(juce::Graphics&) override;

    static constexpr int kRowHeight = 58;
    int preferredHeight() const { return std::max(1, (int)rows.size()) * kRowHeight; }
    void refresh();      // UI timer: playhead + meters
    void viewChanged();  // time scroll/zoom changed
    std::function<void(model::TrackId)> onOpenTrack; // open the piano roll on this track
    std::function<void()> onHeightChanged;

    int cardWidth = 291;
    int cellsX() const { return cardWidth + PianoRollComponent::kKeyboardWidth; }

    class Row;
private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();

    AppContext& app;
    PianoRollComponent& roll;
    std::vector<std::unique_ptr<Row>> rows;
    double lastPlayX = -1;
};

} // namespace mc::gui
