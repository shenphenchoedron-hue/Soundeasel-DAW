#include "gui/AutomationLineEditor.h"
#include "gui/Theme.h"
#include <cmath>
#include <limits>

namespace mc::gui {

using model::Tick;

int AutomationLineEditor::pointAt(const Points& pts, juce::Point<float> p) const
{
    for (int i = (int)pts.size() - 1; i >= 0; --i)
        if (p.getDistanceFrom({tickToX((double)pts[(size_t)i].tick), valueToY(pts[(size_t)i].gain)}) < 7.0f) return i;
    return -1;
}

void AutomationLineEditor::mouseDown(juce::Point<float> pos, const Points& pts)
{
    const int hit = pointAt(pts, pos);
    Drag d;
    d.original = d.points = pts;
    d.start = pos;
    if (hit >= 0) {
        for (int i = 0; i < (int)pts.size(); ++i)
            if (pts[(size_t)i].tick == pts[(size_t)hit].tick) d.group.push_back(i);
        d.point = hit;
    } else {
        // the piece of the line under the mouse (anywhere in the lane, no need to hit the line exactly)
        const double tk = xToTick(pos.x);
        int b = 0;
        while (b < (int)pts.size() && (double)pts[(size_t)b].tick <= tk) ++b;
        d.segA = b - 1;
        d.segB = b < (int)pts.size() ? b : -1;
        d.point = d.segA >= 0 ? d.segA : d.segB;
    }
    line = d;
}

bool AutomationLineEditor::mouseDrag(juce::Point<float> pos)
{
    if (!line) return false;
    auto& d = *line;
    if (!d.moved && pos.getDistanceFrom(d.start) < 3.0f) return false;
    d.moved = true;
    d.points = d.original;
    const float dy = pos.y - d.start.y;
    if (!d.group.empty()) {
        // The first movement decides: sideways moves the split point in time, up/down changes its value
        // (the pieces on both sides become ramps towards the point).
        if (d.axis == 0) {
            const auto delta = pos - d.start;
            d.axis = std::abs(delta.x) > std::abs(delta.y) ? 1 : 2;
        }
        if (d.axis == 1) {
            const int first = d.group.front(), last = d.group.back();
            const Tick lo = first > 0 ? d.points[(size_t)first - 1].tick : 0;
            const Tick hi = last + 1 < (int)d.points.size() ? d.points[(size_t)last + 1].tick : std::numeric_limits<Tick>::max();
            const Tick t = juce::jlimit(lo, hi, snap(xToTick(pos.x)));
            for (int i : d.group) d.points[(size_t)i].tick = t;
        } else {
            // A split point whose two sides have the same value moves as one point; if it is a step
            // (two dots), only the grabbed dot moves.
            bool together = true;
            for (int i : d.group) together = together && std::abs(d.original[(size_t)i].gain - d.original[(size_t)d.point].gain) < 1.0e-4f;
            const float v = yToValue(valueToY(d.original[(size_t)d.point].gain) + dy);
            if (together) for (int i : d.group) d.points[(size_t)i].gain = v;
            else d.points[(size_t)d.point].gain = v;
        }
    } else if (d.points.empty()) {
        // no split points yet: the whole line moves (one flat level)
        d.points.push_back({0, yToValue(valueToY(emptyValue) + dy)});
        d.point = 0;
    } else {
        // move only this piece: its two end points
        for (int i : {d.segA, d.segB})
            if (i >= 0) d.points[(size_t)i].gain = yToValue(valueToY(d.original[(size_t)i].gain) + dy);
    }
    return true;
}

void AutomationLineEditor::mouseUp()
{
    if (!line) return;
    auto d = *line;
    line.reset();
    if (d.moved) { commit(std::move(d.points)); return; }
    if (!d.group.empty()) return; // clicked a split point without moving it
    // a click splits the line here (no change in value until a piece is moved)
    auto pts = d.original;
    const Tick t = snap(xToTick(d.start.x));
    for (auto& p : pts) if (p.tick == t) return; // already split here
    const float v = pts.empty() ? emptyValue : model::automationAt(pts, (double)t);
    pts.push_back({t, v}); // left side of the split
    pts.push_back({t, v}); // right side
    commit(std::move(pts));
}

void AutomationLineEditor::removeSplitPoint(const Points& model, int index)
{
    if (index < 0 || index >= (int)model.size()) return;
    auto pts = model;
    const Tick at = pts[(size_t)index].tick;
    std::erase_if(pts, [at](const model::AutomationPoint& q) { return q.tick == at; });
    commit(std::move(pts));
}

void AutomationLineEditor::paint(juce::Graphics& g, const Points& model, float x0, float x1, juce::Colour colour, bool showPoints) const
{
    const auto& pts = points(model);
    juce::Path path;
    if (pts.empty()) {
        const float y = valueToY(emptyValue);
        path.startNewSubPath(x0, y);
        path.lineTo(x1, y);
    } else {
        path.startNewSubPath(x0, valueToY(pts.front().gain));
        for (auto& p : pts) path.lineTo(tickToX((double)p.tick), valueToY(p.gain));
        path.lineTo(x1, valueToY(pts.back().gain));
    }
    g.setColour(colour.withAlpha(showPoints ? 1.0f : 0.55f));
    g.strokePath(path, juce::PathStrokeType(showPoints ? 2.0f : 1.2f));
    if (!showPoints) return;
    for (size_t i = 0; i < pts.size(); ++i) {
        const float x = tickToX((double)pts[i].tick), y = valueToY(pts[i].gain);
        const bool hot = line && ((int)i == line->point || (int)i == line->segA || (int)i == line->segB);
        g.setColour(hot ? juce::Colours::white : colour);
        g.fillEllipse(x - 4.5f, y - 4.5f, 9.0f, 9.0f);
        g.setColour(juce::Colours::black.withAlpha(0.6f));
        g.drawEllipse(x - 4.5f, y - 4.5f, 9.0f, 9.0f, 1.0f);
    }
    // value while dragging
    if (line && valueText) {
        const int i = line->point >= 0 ? line->point : (line->segA >= 0 ? line->segA : line->segB);
        if (i >= 0 && i < (int)pts.size()) {
            const float x = tickToX((double)pts[(size_t)i].tick), y = valueToY(pts[(size_t)i].gain);
            const float by = y - 22.0f < 0.0f ? y + 6.0f : y - 22.0f; // keep the bubble inside short lanes
            g.setColour(theme::col::display.withAlpha(0.9f));
            g.fillRoundedRectangle(x + 8, by, 64, 16, 3.0f);
            g.setColour(theme::col::displayText);
            g.setFont(theme::monoFont(11.5f));
            g.drawText(valueText(pts[(size_t)i].gain), (int)x + 8, (int)by, 64, 16, juce::Justification::centred);
        }
    }
}

} // namespace mc::gui
