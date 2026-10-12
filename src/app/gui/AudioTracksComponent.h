#pragma once
// Audio tracks panel, shown below the piano roll. Every audio track has a card
// in the left column (name, mute/solo, record arm, input monitoring, input,
// volume/pan) and a lane on the right with its clips, aligned with the piano
// roll grid (same scroll and zoom). Audio files can be dropped onto the panel.
#include "AppContext.h"
#include "gui/LevelMeter.h"
#include "gui/PianoRollComponent.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace mc::gui {

class AudioTracksComponent final : public juce::Component, public juce::FileDragAndDropTarget, private juce::ChangeListener {
public:
    AudioTracksComponent(AppContext&, PianoRollComponent&);
    ~AudioTracksComponent() override;
    void resized() override;
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;

    static constexpr int kHeaderHeight = 34, kLaneHeight = 96;
    int preferredHeight() const { return kHeaderHeight + (int)lanes.size() * kLaneHeight; }
    void refresh();              // UI timer: playhead, meters, growing recordings
    void viewChanged();          // piano roll scrolled/zoomed
    std::function<void()> onHeightChanged;

    int cardWidth = 291;
    int cellsX() const { return cardWidth + PianoRollComponent::kKeyboardWidth; }

    // file drops: onto a lane = that track, elsewhere = a new audio track
    bool isInterestedInFileDrag(const juce::StringArray&) override;
    void fileDragEnter(const juce::StringArray&, int x, int y) override { fileDragMove({}, x, y); }
    void fileDragMove(const juce::StringArray&, int x, int y) override;
    void fileDragExit(const juce::StringArray&) override { dropAt = {-1, -1}; repaint(); }
    void filesDropped(const juce::StringArray&, int x, int y) override;

    void importDialog(model::TrackId track, model::Tick at); // track 0 = new track
    void importFiles(const juce::StringArray& paths, model::TrackId track, model::Tick at);

    class Lane;
private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void rebuild();
    model::Tick tickAtX(int x) const; // panel x -> snapped timeline tick
    Lane* laneAtY(int y) const;

    AppContext& app;
    PianoRollComponent& roll;
    std::vector<std::unique_ptr<Lane>> lanes;
    juce::TextButton addBtn{"+ Audio track"}, importBtn{"Import audio..."};
    juce::Label hint;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Point<int> dropAt{-1, -1};
    double lastPlayX = -1;
};

} // namespace mc::gui
