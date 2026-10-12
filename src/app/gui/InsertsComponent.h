#pragma once
// Effect plugin slots ("inserts") on a mixer channel strip: one row per plugin
// (click: open its window, power button: bypass, right-click: move / replace /
// remove) and a "+ FX" button that lists the scanned effect plugins.
#include "AppContext.h"

namespace mc::gui {

// Menu of effect plugins (per format). replaceIndex >= 0 replaces that slot.
void showEffectMenu(AppContext&, model::TrackId, juce::Component& target, int replaceIndex = -1);

class InsertsComponent final : public juce::Component {
public:
    InsertsComponent(AppContext&, model::TrackId);
    ~InsertsComponent() override;
    void refresh();
    void resized() override;
    void paint(juce::Graphics&) override;
    static constexpr int kRowHeight = 20, kVisibleRows = 2;
    static int preferredHeight() { return kVisibleRows * (kRowHeight + 2) + kRowHeight + 2; }

private:
    class Row;
    AppContext& app;
    model::TrackId id;
    juce::Viewport view;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addBtn{"+ FX"};
};

} // namespace mc::gui
