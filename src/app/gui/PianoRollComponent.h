#pragma once
// Piano roll: views and edits the active MidiTrack model through commands.
// Owns no musical data itself (only view state and the current selection).
#include "AppContext.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <set>

namespace mc::gui {

class PianoRollComponent final : public juce::Component, private juce::ScrollBar::Listener, private juce::ChangeListener {
public:
    explicit PianoRollComponent(AppContext&);
    ~PianoRollComponent() override;

    static constexpr int kKeyboardWidth = 64, kScrollbarWidth = 10;
    std::function<void()> onViewChanged; // scroll/zoom changed (the step sequencer panel follows the grid)
    void resized() override;
    void paint(juce::Graphics&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void refreshPlayhead(); // called by a UI timer

    // ---- view state (shared by subcomponents)
    double pxPerTick = 0.1;
    double rowHeight = 14.0;
    double scrollX = 0;      // ticks at left edge
    double scrollY = 0;      // pixels from pitch 127 at top
    double tickToX(double t) const { return (t - scrollX) * pxPerTick; }
    double xToTick(double x) const { return x / pxPerTick + scrollX; }
    double pitchToY(int p) const { return (127 - p) * rowHeight - scrollY; }
    int yToPitch(double y) const { return juce::jlimit(0, 127, 127 - (int)std::floor((y + scrollY) / rowHeight)); }
    model::Tick snap(double tick) const;
    model::Tick gridTicks() const { return app.grid.ticks(); }
    void zoomHorizontal(double factor, double anchorX);
    void zoomVertical(double factor, double anchorY);
    void scrollBy(double dxTicks, double dyPixels);
    // Ctrl+wheel (zoom) / Shift+wheel (scroll) on the shared time axis, for lanes aligned with the
    // grid (step sequencer, audio and MIDI lanes). laneX is the mouse x relative to the grid start.
    // Returns false if not handled (e.g. over a track card): let the panel scroll then.
    bool handleTimeWheel(const juce::MouseEvent& e, const juce::MouseWheelDetails& w, int laneX)
    {
        if (laneX < 0) return false;
        if (e.mods.isCommandDown()) { zoomHorizontal(w.deltaY > 0 ? 1.15 : 1 / 1.15, laneX); return true; }
        if (e.mods.isShiftDown()) { scrollBy(-(w.deltaY != 0 ? w.deltaY : w.deltaX) * 600 / pxPerTick * 0.1, 0); return true; }
        return false;
    }
    // Zoom around the centre of the visible grid (toolbar buttons / keys). The keyboard follows the row height.
    void zoomInH()  { zoomHorizontal(1.25, gridCentre().x); }
    void zoomOutH() { zoomHorizontal(1 / 1.25, gridCentre().x); }
    void zoomInV()  { zoomVertical(1.2, gridCentre().y); }
    void zoomOutV() { zoomVertical(1 / 1.2, gridCentre().y); }
    juce::Point<double> gridCentre() const;
    void updateScrollbars();

    AppContext& app;
    std::set<model::NoteId> selection;
    model::TrackId selectionTrack = 0;

    void copySelection();
    void paste();
    void deleteSelection();
    void selectAll();

private:
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    class Timeline; class Keyboard; class Grid; class VelocityLane;
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<Keyboard> keyboard;
    std::unique_ptr<Grid> grid;
    std::unique_ptr<VelocityLane> velocity;
    juce::ScrollBar hbar{false}, vbar{true};
    double lastPlayheadX = -1;
    double lastPlayheadPos = 0; // engine position at previous refresh, to detect jumps
    bool firstLayout = true;
    friend class Grid;
};

} // namespace mc::gui
