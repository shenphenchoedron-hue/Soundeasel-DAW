#pragma once
// Mouse editing and drawing of an automation line (the volume line of audio tracks,
// the sustain pedal line of MIDI tracks). The line is divided into pieces by split
// points; a split point is a pair of points at the same time (left value, right value):
// - click: split the line there (no change in value until a piece is moved)
// - drag a piece up/down: only that piece moves (steps)
// - drag a split point up/down: the pieces next to it become ramps (sideways: move it in time;
//   the first movement decides which)
// - double-click a split point (or use the menu): remove it
// The owner maps ticks/values to pixels and stores the result (one undo step per gesture).
#include "model/Track.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <optional>

namespace mc::gui {

class AutomationLineEditor {
public:
    using Points = std::vector<model::AutomationPoint>;

    // ---- set by the owner
    std::function<float(double tick)> tickToX;
    std::function<double(float x)> xToTick;
    std::function<float(float value)> valueToY;
    std::function<float(float y)> yToValue;
    std::function<model::Tick(double tick)> snap;
    std::function<juce::String(float value)> valueText; // shown next to the point while dragging
    std::function<void(Points)> commit;                 // the edited line (undoable change)
    float emptyValue = 1.0f;                            // value of a line without points

    // Points to draw: the line being dragged, else the model's line.
    const Points& points(const Points& model) const { return line ? line->points : model; }
    bool isDragging() const { return line.has_value(); }
    int pointAt(const Points&, juce::Point<float>) const; // -1: none

    void mouseDown(juce::Point<float>, const Points& model);
    bool mouseDrag(juce::Point<float>);  // true: repaint
    void mouseUp();
    void cancel() { line.reset(); }
    void removeSplitPoint(const Points& model, int index); // both points of the split at that time

    // Line, points and (while dragging) the value bubble, between x0 and x1.
    void paint(juce::Graphics&, const Points& model, float x0, float x1, juce::Colour, bool showPoints) const;

private:
    struct Drag {
        Points points, original;
        std::vector<int> group;    // dragged split point (indices of the points at the same time), or
        int segA = -1, segB = -1;  // the piece under the mouse: its end points (-1 = open end)
        juce::Point<float> start;
        bool moved = false;
        int axis = 0;              // split point drags: 0 = undecided, 1 = sideways (time), 2 = up/down (value)
        int point = -1;            // grabbed point, also used for the value display
    };
    std::optional<Drag> line;
};

} // namespace mc::gui
