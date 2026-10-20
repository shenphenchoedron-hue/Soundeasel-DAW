#pragma once
// Human Touch window for one instrument track: random variation of placement,
// length and velocity of the notes already in its piano roll (one undo step).
// Run it again after adding notes; "only new notes" leaves earlier ones alone.
#include "AppContext.h"

namespace mc::gui {

class HumanTouchDialog final : public juce::Component {
public:
    static void show(AppContext&, model::TrackId);
    HumanTouchDialog(AppContext&, model::TrackId);
    void resized() override;
    void paint(juce::Graphics&) override;

private:
    struct Row {
        juce::Label label;
        juce::Slider slider{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
        juce::Label hint;
    };
    model::HumanizeSettings settings() const;
    void update();
    void apply();
    void close(int result);

    AppContext& app;
    model::TrackId id;
    juce::Label title, info;
    Row notes, timing, length, velocity;
    juce::ToggleButton onlyNew{"Only notes that have not had Human Touch yet (e.g. notes added since last time)"};
    juce::TextButton applyBtn{"Apply Human Touch"}, cancelBtn{"Cancel"};
};

} // namespace mc::gui
