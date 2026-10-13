#pragma once
// Effect plugin slots ("inserts") on a mixer channel strip: one row per plugin
// (click: open its window, drag: change the order, power button: bypass,
// right-click: move / replace / remove) and a "+ FX" button that lists the
// scanned effect plugins. Folded ("collapsed"), it is one line that sums up the
// chain; clicking it unfolds the list again.
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
    void paintOverChildren(juce::Graphics&) override;
    static constexpr int kRowHeight = 20, kVisibleRows = 2; // kVisibleRows: rows shown at least
    // height that shows `rows` plugin rows plus the "+ FX" button
    static int heightFor(int rows) { return std::max(rows, kVisibleRows) * (kRowHeight + 2) + kRowHeight + 2; }
    static int preferredHeight() { return heightFor(kVisibleRows); }
    static int collapsedHeight() { return kRowHeight; }

    void setCollapsed(bool);
    bool isCollapsed() const { return collapsed; }
    std::function<void()> onToggleCollapsed; // the fold buttons ask the mixer (all strips fold together)

private:
    class Row;
    // drag-to-reorder (driven by the dragged row; positions are in `list` coordinates)
    void dragMove(int from, juce::Point<int> posInList);
    void dragEnd(int from);
    int gapAt(int yInList) const;
    int dragFrom = -1, dropGap = -1; // dropGap: 0..n, the gap between rows where the plugin lands
    AppContext& app;
    model::TrackId id;
    struct ListView final : juce::Viewport {
        std::function<void()> onScroll;
        void visibleAreaChanged(const juce::Rectangle<int>&) override { if (onScroll) onScroll(); }
    };
    ListView view;
    juce::Component list;
    std::vector<std::unique_ptr<Row>> rows;
    juce::TextButton addBtn{"+ FX"};
    juce::TextButton foldBtn{juce::String::fromUTF8("\xe2\x96\xb4")}; // ▴ fold the list
    std::unique_ptr<juce::Button> summaryBtn;                           // the folded line
    bool collapsed = false;
    void refreshSummary();
};

} // namespace mc::gui
