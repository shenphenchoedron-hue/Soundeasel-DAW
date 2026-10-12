#include "gui/AudioTracksComponent.h"
#include "gui/SettingsComponent.h"
#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include "gui/TrackListComponent.h"
#include "gui/TrackSliders.h"
#include "sequencer/Timing.h"

namespace mc::gui {

namespace {
namespace tc = theme::col;
using model::Tick;
constexpr int kEdge = 6; // px at a clip edge that trim instead of move

juce::Colour trackColour(const model::Track* t) { return t && t->colour ? juce::Colour(t->colour) : juce::Colour(0xff5aae84); }
double ticksPerSecond(double bpm) { return bpm / 60.0 * model::kPPQ; }

// Input selector item ids: 1 = none, 100 + i = mono input i, 200 + i = stereo pair i / i+1, 3 = audio settings
void fillInputBox(juce::ComboBox& box, const juce::StringArray& inputs, const model::AudioTrack& t)
{
    box.clear(juce::dontSendNotification);
    if (inputs.isEmpty()) {
        box.addItem("No inputs - set up audio...", 3);
        box.setSelectedId(3, juce::dontSendNotification);
        return;
    }
    box.addItem("No input", 1);
    box.addSectionHeading("Mono");
    for (int i = 0; i < inputs.size(); ++i) box.addItem("In " + juce::String(i + 1) + ": " + inputs[i], 100 + i);
    if (inputs.size() > 1) {
        box.addSectionHeading("Stereo");
        for (int i = 0; i + 1 < inputs.size(); i += 2)
            box.addItem("In " + juce::String(i + 1) + "+" + juce::String(i + 2) + " (stereo)", 200 + i);
    }
    box.addSeparator();
    box.addItem("Audio settings...", 3);
    int id = 1;
    if (t.input >= 0 && t.input < inputs.size()) id = t.stereoInput && t.input + 1 < inputs.size() ? 200 + t.input : 100 + t.input;
    box.setSelectedId(id, juce::dontSendNotification);
}
} // namespace

// ============================================================ one audio track
class AudioTracksComponent::Lane final : public juce::Component {
public:
    Lane(AudioTracksComponent& o, model::TrackId i) : owner(o), app(o.app), roll(o.roll), id(i)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*>{&colourBtn, &name, &volBtn, &mute, &solo, &delBtn, &armBtn, &monBtn,
                                                                          &inputBox, &volume, &pan, &inMeter, &outMeter})
            addAndMakeVisible(c);
        name.setEditable(false, true);
        name.setFont(theme::uiFont(14.0f, true));
        name.setColour(juce::Label::textColourId, tc::text);
        name.setTooltip("Track name (double-click to rename)");
        name.onTextChange = [this] { app.changeTrack(id, [&](model::TrackProperties& p) { p.name = name.getText().toStdString(); }); };

        mute.setClickingTogglesState(true);
        solo.setClickingTogglesState(true);
        theme::setStyle(mute, theme::styleWarm);
        mute.setTooltip("Mute");
        solo.setTooltip("Solo");
        mute.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.mute = mute.getToggleState(); }); };
        solo.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.solo = solo.getToggleState(); }); };
        delBtn.setTooltip("Delete audio track (can be undone; recorded files stay on disk)");
        delBtn.onClick = [this] { auto& ctx = app; const auto tid = id; juce::MessageManager::callAsync([&ctx, tid] { ctx.deleteTrack(tid); }); };

        volBtn.setClickingTogglesState(true);
        theme::setStyle(volBtn, theme::styleWarm);
        volBtn.setTooltip("Volume line: change the volume in steps over time.\n"
                          "Click to split the line, then drag a piece up/down (steps).\n"
                          "Drag a split point up/down for a rising or falling volume, sideways to move it in time,\n"
                          "double-click or right-click it to remove it. Turn off to move and trim clips again.");
        volBtn.onClick = [this] { app.setShowVolumeLine(id, volBtn.getToggleState()); };
        armBtn.setClickingTogglesState(true);
        theme::setStyle(armBtn, theme::styleRecord);
        armBtn.setTooltip("Record enable: this track records its input when you press Rec");
        armBtn.onClick = [this] { app.setTrackArmed(id, armBtn.getToggleState()); };
        monBtn.setClickingTogglesState(true);
        monBtn.setTooltip("Monitor: hear the input through this track (use headphones to avoid feedback)");
        monBtn.onClick = [this] { app.setTrackMonitor(id, monBtn.getToggleState()); };

        inputBox.setTooltip("Audio input this track records from");
        inputBox.onChange = [this] {
            const int sel = inputBox.getSelectedId();
            if (sel == 3) { SettingsComponent::show(app); refresh(); return; }
            if (sel == 1) app.setTrackInput(id, -1, false);
            else if (sel >= 200) app.setTrackInput(id, sel - 200, true);
            else if (sel >= 100) app.setTrackInput(id, sel - 100, false);
        };

        fader::setup(volume);
        volume.setTooltip("Volume");
        pan.setRange(-1, 1, 0.01);
        pan.setTooltip("Pan");
        pan.setDoubleClickReturnValue(true, 0.0);
        for (auto* s : {&volume, &pan}) s->setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        connectTrackSliders(app, id, volume, pan);
        colourBtn.setTooltip("Track colour");
        colourBtn.onClick = [this] { showTrackColourPicker(app, {id}, colourBtn); };
        inMeter.setInputStyle(true);
        inMeter.setTooltip("Input level (shown while the track is record-enabled or monitoring). Red at the end: the input clipped - click to reset");
        outMeter.setTooltip("Track output level. Red at the top: the signal clipped - click to reset");
        refresh();
    }

    model::TrackId trackId() const { return id; }

    void refresh()
    {
        auto* t = app.project.audioTrack(id);
        if (!t) return;
        name.setText(t->name, juce::dontSendNotification);
        mute.setToggleState(t->mute, juce::dontSendNotification);
        solo.setToggleState(t->solo, juce::dontSendNotification);
        armBtn.setToggleState(t->armed, juce::dontSendNotification);
        monBtn.setToggleState(t->monitor, juce::dontSendNotification);
        volBtn.setToggleState(t->showVolumeLine, juce::dontSendNotification);
        volume.setValue(fader::gainToDb(t->volume), juce::dontSendNotification);
        pan.setValue(t->pan, juce::dontSendNotification);
        fillInputBox(inputBox, app.inputNames(), *t);
        colour = trackColour(t);
        colourBtn.colour = colour;
        colourBtn.repaint();
        repaint();
    }

    // Repaints only what the moving playhead touches (and a growing recording).
    void playheadMoved(double oldX, double newX)
    {
        const int x0 = owner.cellsX(), h = getHeight();
        repaint(x0 + (int)oldX - 3, 0, 7, h);
        repaint(x0 + (int)newX - 3, 0, 7, h);
        if (auto* t = app.project.audioTrack(id); t && t->armed && app.engine->isRecordingAudio()) {
            const double start = app.engine->audioRecordStart();
            if (start >= 0) {
                const int a = x0 + (int)roll.tickToX(start) - 3;
                repaint(a, 0, std::max(1, x0 + (int)newX + 4 - a), h);
            }
        }
    }

    void updateMeters()
    {
        const auto l = app.levels(id);
        outMeter.push(l.left, l.right);
        inMeter.push(l.input, l.input);
    }

    void resized() override
    {
        using namespace theme;
        auto r = juce::Rectangle<int>(0, 0, owner.cardWidth - 1, getHeight()).reduced(gap, 5).withTrimmedLeft(4);
        auto l1 = r.removeFromTop(22);
        delBtn.setBounds(l1.removeFromRight(26));
        l1.removeFromRight(gapS);
        solo.setBounds(l1.removeFromRight(26));
        l1.removeFromRight(gapS);
        mute.setBounds(l1.removeFromRight(26));
        l1.removeFromRight(gapS);
        volBtn.setBounds(l1.removeFromRight(40));
        l1.removeFromRight(gap);
        colourBtn.setBounds(l1.removeFromLeft(18).withSizeKeepingCentre(16, 16));
        name.setBounds(l1);
        r.removeFromTop(gapS);
        auto l2 = r.removeFromTop(22);
        armBtn.setBounds(l2.removeFromLeft(30));
        l2.removeFromLeft(gapS);
        monBtn.setBounds(l2.removeFromLeft(44));
        l2.removeFromLeft(gapS);
        inputBox.setBounds(l2);
        r.removeFromTop(gapS);
        auto l3 = r.removeFromTop(16);
        pan.setBounds(l3.removeFromRight(72));
        l3.removeFromRight(gap);
        volume.setBounds(l3);
        r.removeFromTop(gapS + 2);
        inMeter.setBounds(r.removeFromTop(8).withTrimmedLeft(24));
        // output meter in the column under the piano keyboard
        outMeter.setBounds(juce::Rectangle<int>(owner.cardWidth, 0, PianoRollComponent::kKeyboardWidth, getHeight()).reduced(22, 8).withTrimmedBottom(12));
    }

    void paint(juce::Graphics& g) override
    {
        const int cardW = owner.cardWidth - 1, x0 = owner.cellsX(), x1 = getWidth();
        // card
        g.setColour(tc::panel.interpolatedWith(colour, 0.12f));
        g.fillRect(0, 0, cardW, getHeight());
        g.setColour(colour);
        g.fillRect(0, 0, 4, getHeight());
        g.setColour(tc::border);
        g.fillRect(cardW, 0, 1, getHeight());
        g.setColour(tc::textDim);
        g.setFont(theme::uiFont(11.0f, true));
        g.drawText("IN", theme::gap + 4, inMeter.getY() - 3, 20, 14, juce::Justification::centredLeft);
        // meter column
        g.setColour(tc::keyWhite);
        g.fillRect(owner.cardWidth, 0, PianoRollComponent::kKeyboardWidth, getHeight());
        g.setColour(tc::textDim);
        g.drawText("OUT", owner.cardWidth, getHeight() - 18, PianoRollComponent::kKeyboardWidth, 14, juce::Justification::centred);
        g.setColour(tc::border);
        g.drawVerticalLine(x0 - 1, 0.0f, (float)getHeight());

        // lane
        g.saveState();
        g.reduceClipRegion(x0, 0, x1 - x0, getHeight());
        g.setColour(tc::rollBg);
        g.fillRect(x0, 0, x1 - x0, getHeight());
        drawGrid(g, x0, x1);
        auto* t = app.project.audioTrack(id);
        if (t) {
            for (auto& c : t->clips()) {
                if (drag && c.id == drag->clip.id) continue;
                drawClip(g, c, x0, c.id == selected);
            }
            if (drag) drawClip(g, drag->clip, x0, true);
            drawRecording(g, *t, x0);
            drawVolumeLine(g, *t, x0, x1);
            if (t->clips().empty() && !app.isRecording() && !t->showVolumeLine) {
                g.setColour(juce::Colour(0xff8994a1));
                g.setFont(theme::uiFont(13.0f));
                g.drawText(app.engine->numInputChannels() > 0
                               ? "Press the red record button on the left, then Rec - or drop an audio file here"
                               : "Drop an audio file here (WAV, AIFF, FLAC, Ogg, MP3) - or double-click to import",
                           x0 + 12, 0, x1 - x0 - 24, getHeight(), juce::Justification::centredLeft);
            }
        }
        const float px = (float)(x0 + roll.tickToX(app.engine->position()));
        g.setColour(tc::playhead);
        g.drawLine(px, 0, px, (float)getHeight(), 1.5f);
        g.restoreState();
        g.setColour(tc::border);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());
    }

    // ---- mouse (lane area only; the card has its own controls)
    void mouseDown(const juce::MouseEvent& e) override
    {
        if (e.x < owner.cellsX()) return;
        grabKeyboardFocus();
        auto* t = app.project.audioTrack(id);
        if (!t) return;
        if (t->showVolumeLine) { lineDown(e, *t); return; }
        const auto* hit = clipAt(e.x);
        if (e.mods.isPopupMenu()) { showMenu(hit, e); return; }
        if (!hit) {
            selected = 0;
            app.setPosition(owner.tickAtX(e.x));
            repaint();
            return;
        }
        selected = hit->id;
        const int cx0 = clipX(*hit), cx1 = clipX(*hit, true);
        Drag d;
        d.clip = *hit;
        d.original = *hit;
        d.mode = e.x - cx0 < kEdge && cx1 - cx0 > 3 * kEdge ? Drag::TrimLeft : cx1 - e.x < kEdge && cx1 - cx0 > 3 * kEdge ? Drag::TrimRight : Drag::Move;
        d.grabTick = roll.xToTick(e.x - owner.cellsX());
        drag = d;
        repaint();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (line) { lineDrag(e); return; }
        if (!drag) return;
        auto* t = app.project.audioTrack(id);
        if (!t) return;
        const double tps = ticksPerSecond(app.project.tempoBpm);
        const auto& o = drag->original;
        const double nowTick = roll.xToTick(e.x - owner.cellsX());
        auto data = app.audioFile(o.file);
        const double fileLen = data ? data->lengthSeconds : o.offset + o.length;
        auto& c = drag->clip;
        if (drag->mode == Drag::Move) {
            c.start = roll.snap((double)o.start + nowTick - drag->grabTick);
        } else if (drag->mode == Drag::TrimLeft) {
            const double earliest = (double)o.start - o.offset * tps;         // file start
            const double latest = (double)o.start + (o.length - 0.01) * tps;  // keep 10 ms
            const auto ns = (Tick)juce::jlimit(std::max(0.0, earliest), latest, (double)roll.snap(nowTick));
            const double delta = (double)(ns - o.start) / tps;
            c.start = ns;
            c.offset = std::max(0.0, o.offset + delta);
            c.length = o.length - (c.offset - o.offset);
        } else {
            const double endTick = (double)roll.snap(nowTick);
            c.length = juce::jlimit(0.01, std::max(0.01, fileLen - o.offset), (endTick - (double)o.start) / tps);
        }
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (line) { lineUp(); return; }
        if (!drag) return;
        const auto d = *drag;
        drag.reset();
        if (!(d.clip == d.original)) replaceClip(d.clip, d.mode == Drag::Move ? "Move audio clip" : "Trim audio clip");
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        auto mc = juce::MouseCursor::NormalCursor;
        auto* t = app.project.audioTrack(id);
        if (t && t->showVolumeLine) {
            if (e.x >= owner.cellsX()) {
                const int hit = pointAt(t->volumeAutomation, e.position);
                mc = hit >= 0 ? juce::MouseCursor::UpDownLeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor;
            }
            setMouseCursor(mc);
            return;
        }
        if (e.x >= owner.cellsX())
            if (const auto* c = clipAt(e.x)) {
                const int cx0 = clipX(*c), cx1 = clipX(*c, true);
                mc = (e.x - cx0 < kEdge || cx1 - e.x < kEdge) && cx1 - cx0 > 3 * kEdge ? juce::MouseCursor::LeftRightResizeCursor
                                                                                            : juce::MouseCursor::DraggingHandCursor;
            }
        setMouseCursor(mc);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (auto* t = app.project.audioTrack(id); t && t->showVolumeLine) {
            // double-click a split point: remove it
            if (const int hit = pointAt(t->volumeAutomation, e.position); hit >= 0) {
                auto pts = t->volumeAutomation;
                const Tick at = pts[(size_t)hit].tick;
                std::erase_if(pts, [at](const model::AutomationPoint& q) { return q.tick == at; });
                app.setVolumeAutomation(id, std::move(pts));
            }
            return;
        }
        if (e.x >= owner.cellsX() && !clipAt(e.x)) owner.importDialog(id, owner.tickAtX(e.x));
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (!roll.handleTimeWheel(e, w, e.x - owner.cellsX())) juce::Component::mouseWheelMove(e, w); // scroll the page
    }

    bool keyPressed(const juce::KeyPress& k) override
    {
        if (selected && (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)) { deleteClip(selected); return true; }
        return false;
    }

    model::ClipId selected = 0;

private:
    // ---- volume line
    // The volume line is divided into pieces by split points. A split point is a pair of points at the
    // same time (left value, right value), so every piece can be moved up/down on its own (steps).
    struct LineDrag {
        std::vector<model::AutomationPoint> points, original;
        std::vector<int> group;    // dragged split point (indices of the points at the same time), or
        int segA = -1, segB = -1;  // the piece under the mouse: its end points (-1 = open end)
        juce::Point<float> start;
        bool moved = false;
        int axis = 0;              // split point drags: 0 = undecided, 1 = sideways (time), 2 = up/down (level)
        int point = -1;            // grabbed point, also used for the value display
    };
    std::optional<LineDrag> line;

    float lineTop() const { return 6.0f; }
    float lineBottom() const { return (float)getHeight() - 7.0f; }
    // 0 dB sits at 71 % of the height, +6 dB at the top (square-law scale: more room for useful levels)
    float gainToY(float g) const { return lineBottom() - std::sqrt(juce::jlimit(0.0f, 2.0f, g) / 2.0f) * (lineBottom() - lineTop()); }
    float yToGain(float y) const
    {
        const float p = juce::jlimit(0.0f, 1.0f, (lineBottom() - y) / (lineBottom() - lineTop()));
        return 2.0f * p * p;
    }
    float tickToLaneX(double t) const { return (float)(owner.cellsX() + roll.tickToX(t)); }
    double laneXToTick(float x) const { return roll.xToTick(x - owner.cellsX()); }

    const std::vector<model::AutomationPoint>& linePoints(const model::AudioTrack& t) const { return line ? line->points : t.volumeAutomation; }

    int pointAt(const std::vector<model::AutomationPoint>& pts, juce::Point<float> p) const
    {
        for (int i = (int)pts.size() - 1; i >= 0; --i)
            if (p.getDistanceFrom({tickToLaneX((double)pts[(size_t)i].tick), gainToY(pts[(size_t)i].gain)}) < 7.0f) return i;
        return -1;
    }
    void drawVolumeLine(juce::Graphics& g, const model::AudioTrack& t, int x0, int x1)
    {
        const auto& pts = linePoints(t);
        const bool editing = t.showVolumeLine;
        if (!editing && pts.empty()) return;
        const auto lineCol = juce::Colour(0xffffd54a);
        if (editing) {
            g.setColour(juce::Colours::black.withAlpha(0.28f)); // clips stay visible but recede
            g.fillRect(x0, 0, x1 - x0, getHeight());
            const float y0 = gainToY(1.0f);
            g.setColour(juce::Colours::white.withAlpha(0.25f));
            const float dashes[] = {4.0f, 4.0f};
            g.drawDashedLine({(float)x0, y0, (float)x1, y0}, dashes, 2, 1.0f);
            g.setFont(theme::uiFont(10.5f, true));
            g.drawText("0 dB", x0 + 4, (int)y0 - 13, 40, 12, juce::Justification::left);
        }
        juce::Path path;
        if (pts.empty()) {
            const float y = gainToY(1.0f);
            path.startNewSubPath((float)x0, y);
            path.lineTo((float)x1, y);
        } else {
            path.startNewSubPath((float)x0, gainToY(pts.front().gain));
            for (auto& p : pts) path.lineTo(tickToLaneX((double)p.tick), gainToY(p.gain));
            path.lineTo((float)x1, gainToY(pts.back().gain));
        }
        g.setColour(lineCol.withAlpha(editing ? 1.0f : 0.55f));
        g.strokePath(path, juce::PathStrokeType(editing ? 2.0f : 1.2f));
        if (!editing) return;
        for (size_t i = 0; i < pts.size(); ++i) {
            const float x = tickToLaneX((double)pts[i].tick), y = gainToY(pts[i].gain);
            const bool hot = line && ((int)i == line->point || (int)i == line->segA || (int)i == line->segB);
            g.setColour(hot ? juce::Colours::white : lineCol);
            g.fillEllipse(x - 4.5f, y - 4.5f, 9.0f, 9.0f);
            g.setColour(juce::Colours::black.withAlpha(0.6f));
            g.drawEllipse(x - 4.5f, y - 4.5f, 9.0f, 9.0f, 1.0f);
        }
        // value while dragging
        if (line) {
            const int i = line->point >= 0 ? line->point : (line->segA >= 0 ? line->segA : line->segB);
            if (i >= 0 && i < (int)pts.size()) {
                const float x = tickToLaneX((double)pts[(size_t)i].tick), y = gainToY(pts[(size_t)i].gain);
                g.setColour(tc::display.withAlpha(0.9f));
                g.fillRoundedRectangle(x + 8, y - 22, 64, 16, 3.0f);
                g.setColour(tc::displayText);
                g.setFont(theme::monoFont(11.5f));
                g.drawText(fader::text(pts[(size_t)i].gain) + " dB", (int)x + 8, (int)y - 22, 64, 16, juce::Justification::centred);
            }
        }
    }

    void lineDown(const juce::MouseEvent& e, const model::AudioTrack& t)
    {
        const auto& pts = t.volumeAutomation;
        const int hit = pointAt(pts, e.position);
        if (e.mods.isPopupMenu()) { lineMenu(hit); return; }
        LineDrag d;
        d.original = d.points = pts;
        d.start = e.position;
        if (hit >= 0) {
            for (int i = 0; i < (int)pts.size(); ++i)
                if (pts[(size_t)i].tick == pts[(size_t)hit].tick) d.group.push_back(i);
            d.point = hit;
        } else {
            // the piece of the line under the mouse (anywhere in the lane, no need to hit the line exactly)
            const double tk = laneXToTick(e.position.x);
            int b = 0;
            while (b < (int)pts.size() && (double)pts[(size_t)b].tick <= tk) ++b;
            d.segA = b - 1;
            d.segB = b < (int)pts.size() ? b : -1;
            d.point = d.segA >= 0 ? d.segA : d.segB;
        }
        line = d;
        repaint();
    }

    void lineDrag(const juce::MouseEvent& e)
    {
        auto& d = *line;
        if (!d.moved && e.position.getDistanceFrom(d.start) < 3.0f) return;
        d.moved = true;
        d.points = d.original;
        if (!d.group.empty()) {
            // The first movement decides: sideways moves the split point in time, up/down changes its level
            // (the pieces on both sides become ramps: the volume rises or falls towards the point).
            if (d.axis == 0) {
                const auto delta = e.position - d.start;
                d.axis = std::abs(delta.x) > std::abs(delta.y) ? 1 : 2;
            }
            if (d.axis == 1) {
                const int first = d.group.front(), last = d.group.back();
                const Tick lo = first > 0 ? d.points[(size_t)first - 1].tick : 0;
                const Tick hi = last + 1 < (int)d.points.size() ? d.points[(size_t)last + 1].tick : std::numeric_limits<Tick>::max();
                const Tick t = juce::jlimit(lo, hi, roll.snap(laneXToTick(e.position.x)));
                for (int i : d.group) d.points[(size_t)i].tick = t;
            } else {
                // A split point whose two sides are at the same level moves as one point; if it is a step
                // (two dots), only the grabbed dot moves.
                bool together = true;
                for (int i : d.group) together = together && std::abs(d.original[(size_t)i].gain - d.original[(size_t)d.point].gain) < 1.0e-4f;
                const float g = yToGain(gainToY(d.original[(size_t)d.point].gain) + e.position.y - d.start.y);
                if (together) for (int i : d.group) d.points[(size_t)i].gain = g;
                else d.points[(size_t)d.point].gain = g;
            }
        } else if (d.points.empty()) {
            // no split points yet: the whole line moves (one flat level)
            d.points.push_back({0, yToGain(gainToY(1.0f) + e.position.y - d.start.y)});
            d.point = 0;
        } else {
            // move only this piece: its two end points
            const float dy = e.position.y - d.start.y;
            for (int i : {d.segA, d.segB})
                if (i >= 0) d.points[(size_t)i].gain = yToGain(gainToY(d.original[(size_t)i].gain) + dy);
        }
        repaint();
    }

    void lineUp()
    {
        auto d = *line;
        line.reset();
        if (d.moved) { app.setVolumeAutomation(id, std::move(d.points)); repaint(); return; }
        if (!d.group.empty()) { repaint(); return; } // clicked a split point without moving it
        // a click splits the line here (no change in volume until a piece is moved)
        auto pts = d.original;
        const Tick t = roll.snap(laneXToTick(d.start.x));
        for (auto& p : pts) if (p.tick == t) { repaint(); return; } // already split here
        const float v = model::automationAt(pts, (double)t);
        pts.push_back({t, v}); // left side of the split
        pts.push_back({t, v}); // right side
        app.setVolumeAutomation(id, std::move(pts));
    }

    void lineMenu(int hit)
    {
        juce::PopupMenu m;
        m.addSectionHeader("Volume line");
        m.addItem(1, "Delete split point", hit >= 0);
        m.addItem(2, "Remove the whole volume line");
        m.addSeparator();
        m.addItem(3, "Done (hide the volume line editing)");
        juce::Component::SafePointer<Lane> self(this);
        m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [self, hit](int r) {
            if (!self || r == 0) return;
            auto* t = self->app.project.audioTrack(self->id);
            if (!t) return;
            if (r == 1 && hit >= 0 && hit < (int)t->volumeAutomation.size()) {
                auto pts = t->volumeAutomation;
                const Tick at = pts[(size_t)hit].tick;
                std::erase_if(pts, [at](const model::AutomationPoint& q) { return q.tick == at; });
                self->app.setVolumeAutomation(self->id, std::move(pts));
            } else if (r == 2) {
                self->app.setVolumeAutomation(self->id, {});
            } else if (r == 3) {
                self->app.setShowVolumeLine(self->id, false);
            }
        });
    }

    struct Drag {
        enum Mode { Move, TrimLeft, TrimRight } mode = Move;
        model::AudioClip clip, original;
        double grabTick = 0;
    };

    int clipX(const model::AudioClip& c, bool end = false) const
    {
        const double t = end ? c.end(app.project.tempoBpm) : (double)c.start;
        return owner.cellsX() + (int)std::round(roll.tickToX(t));
    }

    const model::AudioClip* clipAt(int x) const
    {
        auto* t = app.project.audioTrack(id);
        if (!t) return nullptr;
        const auto& clips = t->clips();
        for (auto it = clips.rbegin(); it != clips.rend(); ++it) // topmost (latest) first
            if (x >= clipX(*it) && x < std::max(clipX(*it, true), clipX(*it) + 4)) return &*it;
        return nullptr;
    }

    void drawGrid(juce::Graphics& g, int x0, int x1)
    {
        const auto& p = app.project;
        const auto bar = seq::ticksPerBar(p.timeSig), beat = seq::ticksPerBeat(p.timeSig);
        const auto first = std::max<Tick>(0, (Tick)roll.xToTick(0) / beat * beat);
        const bool showBeats = roll.pxPerTick * (double)beat > 6;
        for (auto t = first; roll.tickToX((double)t) < x1 - x0; t += beat) {
            const bool isBar = t % bar == 0;
            if (!isBar && !showBeats) continue;
            g.setColour(isBar ? tc::gridBar.withAlpha(0.6f) : tc::gridSub);
            g.drawVerticalLine(x0 + (int)roll.tickToX((double)t), 0.0f, (float)getHeight());
        }
    }

    void drawClip(juce::Graphics& g, const model::AudioClip& c, int x0, bool sel)
    {
        const float cx0 = (float)(x0 + roll.tickToX((double)c.start));
        const float cx1 = (float)(x0 + roll.tickToX(c.end(app.project.tempoBpm)));
        if (cx1 < (float)x0 || cx0 > (float)getWidth()) return;
        const juce::Rectangle<float> r(cx0, 4.0f, std::max(3.0f, cx1 - cx0), (float)getHeight() - 9.0f);
        auto data = app.audioFile(c.file);
        const bool loading = !data && app.isAudioFileLoading(c.file);
        const auto base = data || loading ? colour : tc::record;
        g.setColour(base.withAlpha(0.28f));
        g.fillRoundedRectangle(r, 4.0f);
        // title bar
        auto title = r.withHeight(16.0f);
        g.setColour(base.withAlpha(0.75f));
        g.fillRoundedRectangle(title, 4.0f);
        g.setColour(juce::Colours::white);
        g.setFont(theme::uiFont(11.5f, true));
        juce::String label = juce::String::fromUTF8(c.name.c_str());
        if (loading) label = "Loading... " + label;
        else if (!data) label = "File missing: " + label;
        else if (std::abs(c.gain - 1.0f) > 0.001f) label << "  (" << fader::text(c.gain) << " dB)";
        g.drawText(label, title.reduced(5, 0).toNearestInt(), juce::Justification::centredLeft, true);
        // waveform
        if (data) {
            const auto wave = r.withTrimmedTop(17.0f).reduced(0, 2);
            const float mid = wave.getCentreY(), half = wave.getHeight() * 0.5f;
            const double tps = ticksPerSecond(app.project.tempoBpm);
            g.setColour(colour.brighter(0.5f).withAlpha(0.9f));
            const int from = std::max(x0, (int)cx0), to = std::min(getWidth(), (int)cx1);
            for (int x = from; x < to; ++x) {
                const double t0 = roll.xToTick(x - x0), t1 = roll.xToTick(x + 1 - x0);
                const double s0 = c.offset + (t0 - (double)c.start) / tps, s1 = c.offset + (t1 - (double)c.start) / tps;
                const float p = std::min(1.0f, data->peakAt(s0, s1) * c.gain);
                const float h = std::max(0.5f, p * half);
                g.fillRect((float)x, mid - h, 1.0f, 2 * h);
            }
        }
        g.setColour(sel ? juce::Colours::white : base.darker(0.2f));
        g.drawRoundedRectangle(r.reduced(0.5f), 4.0f, sel ? 2.0f : 1.0f);
    }

    void drawRecording(juce::Graphics& g, const model::AudioTrack& t, int x0)
    {
        if (!t.armed || !app.engine->isRecordingAudio()) return;
        const double start = app.engine->audioRecordStart();
        if (start < 0) return;
        const float a = (float)(x0 + roll.tickToX(start)), b = (float)(x0 + roll.tickToX(app.engine->position()));
        const juce::Rectangle<float> r(a, 4.0f, std::max(2.0f, b - a), (float)getHeight() - 9.0f);
        g.setColour(tc::record.withAlpha(0.3f));
        g.fillRoundedRectangle(r, 4.0f);
        g.setColour(tc::record);
        g.drawRoundedRectangle(r, 4.0f, 1.5f);
        g.setColour(juce::Colours::white);
        g.setFont(theme::uiFont(11.5f, true));
        g.drawText("Recording...", r.reduced(6, 2).toNearestInt(), juce::Justification::topLeft, false);
    }

    void replaceClip(const model::AudioClip& c, const std::string& what)
    {
        auto* t = app.project.audioTrack(id);
        if (!t) return;
        auto clips = t->clips();
        for (auto& x : clips) if (x.id == c.id) x = c;
        app.setAudioClips(id, std::move(clips), what);
    }

    void deleteClip(model::ClipId cid)
    {
        auto* t = app.project.audioTrack(id);
        if (!t) return;
        auto clips = t->clips();
        std::erase_if(clips, [cid](const model::AudioClip& c) { return c.id == cid; });
        selected = 0;
        app.setAudioClips(id, std::move(clips), "Delete audio clip");
    }

    void showMenu(const model::AudioClip* hit, const juce::MouseEvent& e)
    {
        const auto at = owner.tickAtX(e.x);
        juce::PopupMenu m;
        if (hit) {
            selected = hit->id;
            repaint();
            const auto c = *hit;
            const double play = app.engine->position();
            const bool canSplit = play > (double)c.start + 1 && play < c.end(app.project.tempoBpm) - 1;
            m.addSectionHeader(juce::String::fromUTF8(c.name.c_str()));
            m.addItem(1, "Split at playhead", canSplit);
            m.addItem(2, "Duplicate (place after)");
            juce::PopupMenu gain;
            for (int db : {-12, -6, -3, 0, 3, 6})
                gain.addItem(100 + db, (db > 0 ? "+" : "") + juce::String(db) + " dB", true, std::abs(fader::gainToDb(c.gain) - db) < 0.05);
            m.addSubMenu("Clip gain", gain);
            m.addItem(4, "Show file in folder");
            m.addSeparator();
            m.addItem(3, "Delete clip");
            juce::Component::SafePointer<Lane> self(this);
            m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [self, c, play](int r) {
                if (!self || r == 0) return;
                auto& ctx = self->app;
                auto* t = ctx.project.audioTrack(self->id);
                if (!t) return;
                auto clips = t->clips();
                if (r == 1) {
                    const double tps = ticksPerSecond(ctx.project.tempoBpm);
                    auto left = c, right = c;
                    const auto cut = (Tick)std::llround(play);
                    left.length = (double)(cut - c.start) / tps;
                    right.id = 0;
                    right.start = cut;
                    right.offset = c.offset + left.length;
                    right.length = c.length - left.length;
                    for (auto& x : clips) if (x.id == c.id) x = left;
                    clips.push_back(right);
                    ctx.setAudioClips(self->id, std::move(clips), "Split audio clip");
                } else if (r == 2) {
                    auto copy = c;
                    copy.id = 0;
                    copy.start = (Tick)std::ceil(c.end(ctx.project.tempoBpm));
                    clips.push_back(copy);
                    ctx.setAudioClips(self->id, std::move(clips), "Duplicate audio clip");
                } else if (r == 3) {
                    self->deleteClip(c.id);
                } else if (r == 4) {
                    juce::File(juce::String::fromUTF8(c.file.c_str())).revealToUser();
                } else if (r >= 100 - 12 && r <= 100 + 6) {
                    auto g = c;
                    g.gain = (float)fader::dbToGain(r - 100);
                    self->replaceClip(g, "Clip gain");
                }
            });
        } else {
            m.addItem(1, "Import audio file here...");
            juce::Component::SafePointer<Lane> self(this);
            m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [self, at](int r) {
                if (self && r == 1) self->owner.importDialog(self->id, at);
            });
        }
    }

    struct Swatch final : juce::Button {
        Swatch() : juce::Button("colour") {}
        juce::Colour colour;
        void paintButton(juce::Graphics& g, bool over, bool) override
        {
            auto r = getLocalBounds().toFloat().reduced(1.5f);
            g.setColour(colour);
            g.fillEllipse(r);
            g.setColour(over ? tc::accent : colour.darker(0.25f));
            g.drawEllipse(r, 1.0f);
        }
    };

    AudioTracksComponent& owner;
    AppContext& app;
    PianoRollComponent& roll;
    model::TrackId id;
    juce::Colour colour{0xff5aae84};
    std::optional<Drag> drag;
    Swatch colourBtn;
    juce::Label name;
    juce::TextButton volBtn{"VOL"}, mute{"M"}, solo{"S"}, delBtn{"X"}, armBtn{juce::String::fromUTF8("\xe2\x97\x8f")}, monBtn{"Mon"}; // ●
    juce::ComboBox inputBox;
    juce::Slider volume{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    juce::Slider pan{juce::Slider::LinearHorizontal, juce::Slider::NoTextBox};
    LevelMeter inMeter{false, false}, outMeter{true, true};
};

// ============================================================ panel
AudioTracksComponent::AudioTracksComponent(AppContext& a, PianoRollComponent& r) : app(a), roll(r)
{
    addAndMakeVisible(addBtn);
    addAndMakeVisible(importBtn);
    addAndMakeVisible(hint);
    addBtn.setTooltip("Add an audio track to record a microphone, guitar or other input");
    addBtn.onClick = [this] { app.addAudioTrack(); };
    importBtn.setTooltip("Import an audio file (WAV, AIFF, FLAC, Ogg, MP3) onto a new audio track at the playhead");
    importBtn.onClick = [this] { importDialog(0, (Tick)app.engine->position()); };
    hint.setFont(theme::uiFont(12.5f));
    hint.setColour(juce::Label::textColourId, tc::textDim);
    app.addChangeListener(this);
    rebuild();
}

AudioTracksComponent::~AudioTracksComponent() { app.removeChangeListener(this); }

void AudioTracksComponent::paint(juce::Graphics& g)
{
    g.fillAll(tc::rollBg);
    g.setColour(tc::panel);
    g.fillRect(0, 0, getWidth(), kHeaderHeight);
    g.setColour(tc::borderSoft);
    g.drawHorizontalLine(kHeaderHeight - 1, 0.0f, (float)getWidth());
    g.setColour(tc::textDim);
    g.setFont(theme::uiFont(13.0f, true));
    g.drawText("AUDIO TRACKS", theme::gap + 4, 0, 120, kHeaderHeight, juce::Justification::centredLeft);
}

void AudioTracksComponent::paintOverChildren(juce::Graphics& g)
{
    if (dropAt.x < 0) return;
    auto* lane = laneAtY(dropAt.y);
    const auto area = lane ? lane->getBounds() : juce::Rectangle<int>(0, 0, getWidth(), kHeaderHeight);
    g.setColour(tc::accent.withAlpha(0.18f));
    g.fillRect(area);
    g.setColour(tc::accent);
    g.drawRect(area, 2);
    if (dropAt.x >= cellsX()) g.drawVerticalLine(dropAt.x, (float)area.getY(), (float)area.getBottom());
    g.setFont(theme::uiFont(13.0f, true));
    g.drawText(lane ? "Drop to add the audio to this track" : "Drop to add the audio on a new track", area.reduced(12, 0).withTrimmedLeft(cellsX()),
               juce::Justification::centredLeft);
}

void AudioTracksComponent::resized()
{
    auto top = juce::Rectangle<int>(0, 0, getWidth(), kHeaderHeight).reduced(theme::gap, 6);
    top.removeFromLeft(130);
    addBtn.setBounds(top.removeFromLeft(110));
    top.removeFromLeft(theme::gapS);
    importBtn.setBounds(top.removeFromLeft(118));
    top.removeFromLeft(theme::gap);
    hint.setBounds(top);
    const int w = std::max(0, getWidth() - PianoRollComponent::kScrollbarWidth);
    int y = kHeaderHeight;
    for (auto& l : lanes) { l->setBounds(0, y, w, kLaneHeight); y += kLaneHeight; }
}

void AudioTracksComponent::changeListenerCallback(juce::ChangeBroadcaster*) { rebuild(); }

void AudioTracksComponent::rebuild()
{
    const int oldH = preferredHeight();
    std::vector<model::TrackId> ids;
    for (auto* a : app.audioTracks()) ids.push_back(a->id());
    bool same = ids.size() == lanes.size();
    for (size_t i = 0; same && i < ids.size(); ++i) same = lanes[i]->trackId() == ids[i];
    if (!same) {
        lanes.clear();
        for (auto id : ids) { lanes.push_back(std::make_unique<Lane>(*this, id)); addAndMakeVisible(*lanes.back()); }
        resized();
    } else {
        for (auto& l : lanes) l->refresh();
    }
    hint.setText(lanes.empty() ? "Record vocals or instruments, or drag audio files here."
                               : "Drag clips to move, edges to trim. Right-click a clip to split, duplicate or delete it.",
                 juce::dontSendNotification);
    if (preferredHeight() != oldH && onHeightChanged) onHeightChanged();
}

void AudioTracksComponent::viewChanged()
{
    for (auto& l : lanes) l->repaint();
}

void AudioTracksComponent::refresh()
{
    for (auto& l : lanes) l->updateMeters();
    const double x = roll.tickToX(app.engine->position());
    if (x == lastPlayX && !app.engine->isRecordingAudio()) return;
    for (auto& l : lanes) l->playheadMoved(lastPlayX, x);
    lastPlayX = x;
}

model::Tick AudioTracksComponent::tickAtX(int x) const { return roll.snap(roll.xToTick(x - cellsX())); }

AudioTracksComponent::Lane* AudioTracksComponent::laneAtY(int y) const
{
    for (auto& l : lanes) if (l->getBounds().contains(l->getX(), y)) return l.get();
    return nullptr;
}

bool AudioTracksComponent::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (auto& f : files) if (app.audioFiles.canRead(juce::File(f))) return true;
    return false;
}

void AudioTracksComponent::fileDragMove(const juce::StringArray&, int x, int y)
{
    dropAt = {x, y};
    repaint();
}

void AudioTracksComponent::filesDropped(const juce::StringArray& files, int x, int y)
{
    dropAt = {-1, -1};
    repaint();
    auto* lane = laneAtY(y);
    importFiles(files, lane ? lane->trackId() : 0, x >= cellsX() ? tickAtX(x) : (Tick)app.engine->position());
}

void AudioTracksComponent::importFiles(const juce::StringArray& files, model::TrackId track, model::Tick at)
{
    juce::StringArray errors;
    for (auto& path : files) {
        juce::String err;
        const juce::File f(path);
        if (!app.importAudio(f, track, at, err)) { errors.add(err); continue; }
        if (track == 0) track = app.audioTracks().empty() ? 0 : app.audioTracks().back()->id();
        // several files on one track are placed one after another
        if (auto* a = app.project.audioTrack(track); a && !a->clips().empty()) {
            double end = 0;
            for (auto& c : a->clips()) if (c.file == f.getFullPathName().toStdString()) end = std::max(end, c.end(app.project.tempoBpm));
            at = std::max(at, (Tick)std::ceil(end));
        }
    }
    if (!errors.isEmpty())
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Import audio", errors.joinIntoString("\n"));
}

void AudioTracksComponent::importDialog(model::TrackId track, model::Tick at)
{
    chooser = std::make_unique<juce::FileChooser>("Import audio file", app.settings.lastDirectory(), app.audioFiles.wildcard());
    juce::Component::SafePointer<AudioTracksComponent> self(this);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                         [self, track, at](const juce::FileChooser& fc) {
        if (!self) return;
        const auto results = fc.getResults();
        if (results.isEmpty()) return;
        self->app.settings.setLastDirectory(results.getFirst().getParentDirectory());
        juce::StringArray paths;
        for (auto& f : results) paths.add(f.getFullPathName());
        self->importFiles(paths, track, at);
    });
}

} // namespace mc::gui
