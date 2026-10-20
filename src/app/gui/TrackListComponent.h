#pragma once
#include "AppContext.h"

namespace mc::gui {

// Instrument popup (Internal / SoundFonts / plugin formats / editor / bypass) for one track.
void showInstrumentMenu(AppContext&, model::TrackId, juce::Component& target);
// Colour picker call-out; every track in the list gets the colour (one undo step).
void showTrackColourPicker(AppContext&, std::vector<model::TrackId>, juce::Component& target);

// Right-click menu of a track card: copy, paste after it, duplicate, Human Touch (instrument
// tracks) and delete. For a step sequencer line it works on the whole step sequencer.
void showTrackMenu(AppContext&, model::TrackId);

// Attach to a child (e.g. an editable name label, which swallows clicks) so right-clicking
// it opens showTrackMenu. Must outlive the component it is attached to.
struct TrackMenuOnRightClick final : juce::MouseListener {
    TrackMenuOnRightClick(AppContext& a, std::function<model::TrackId()> f) : app(a), track(std::move(f)) {}
    void mouseDown(const juce::MouseEvent& e) override { if (e.mods.isPopupMenu()) if (const auto id = track()) showTrackMenu(app, id); }
    AppContext& app;
    std::function<model::TrackId()> track;
};

class TrackListComponent final : public juce::Component, private juce::ChangeListener {
public:
    explicit TrackListComponent(AppContext&);
    ~TrackListComponent() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void refreshMeters(); // UI timer

    class Row;
private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();

    AppContext& app;
    juce::TextButton addBtn{"+ Track"}, delBtn{"- Track"};
    juce::Viewport viewport;
    juce::Component content;
    std::vector<std::unique_ptr<Row>> rows;
};

} // namespace mc::gui
