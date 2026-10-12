#include "gui/MidiOverviewComponent.h"
#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include "gui/TrackListComponent.h"
#include "sequencer/Timing.h"

namespace mc::gui {

namespace {
namespace tc = theme::col;
using model::Tick;
}

class MidiOverviewComponent::Row final : public juce::Component, public juce::SettableTooltipClient {
public:
    Row(MidiOverviewComponent& o, model::TrackId i) : owner(o), app(o.app), roll(o.roll), id(i)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*>{&name, &editBtn, &mute, &solo, &instBtn, &meter})
            addAndMakeVisible(c);
        name.setFont(theme::uiFont(14.0f, true));
        name.setColour(juce::Label::textColourId, tc::text);
        name.setInterceptsMouseClicks(false, false);
        editBtn.setTooltip("Open this track in the piano roll");
        editBtn.onClick = [this] { open(); };
        mute.setClickingTogglesState(true);
        solo.setClickingTogglesState(true);
        theme::setStyle(mute, theme::styleWarm);
        mute.setTooltip("Mute");
        solo.setTooltip("Solo");
        mute.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.mute = mute.getToggleState(); }); };
        solo.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.solo = solo.getToggleState(); }); };
        instBtn.onClick = [this] { showInstrumentMenu(app, id, instBtn); };
        meter.setTooltip("Track level: moves when this track makes sound. Red: the signal clipped - click to reset");
        setTooltip("Double-click the notes to open the track in the piano roll");
        refresh();
    }

    model::TrackId trackId() const { return id; }

    void refresh()
    {
        auto* t = app.project.midiTrack(id);
        if (!t) return;
        name.setText(t->name, juce::dontSendNotification);
        mute.setToggleState(t->mute, juce::dontSendNotification);
        solo.setToggleState(t->solo, juce::dontSendNotification);
        const bool missing = app.isPluginMissing(id);
        instBtn.setButtonText(t->plugin.empty() ? "(no instrument)" : (missing ? "Plugin missing: " : "") + juce::String(t->plugin.name));
        instBtn.setTooltip(missing ? app.pluginError(id) : "Instrument: click to choose the sound of this track");
        theme::setStyle(instBtn, missing || t->plugin.empty() ? theme::styleDanger : theme::styleSelector);
        colour = t->colour ? juce::Colour(t->colour) : tc::accent;
        active = app.project.activeTrack == id;
        repaint();
    }

    void updateMeter() { const auto l = app.levels(id); meter.push(l.left, l.right); }
    void playheadMoved(double oldX, double newX)
    {
        const int x0 = owner.cellsX();
        repaint(x0 + (int)oldX - 3, 0, 7, getHeight());
        repaint(x0 + (int)newX - 3, 0, 7, getHeight());
    }

    void resized() override
    {
        using namespace theme;
        auto r = juce::Rectangle<int>(0, 0, owner.cardWidth - 1, getHeight()).reduced(gap, 5).withTrimmedLeft(4);
        auto l1 = r.removeFromTop(22);
        solo.setBounds(l1.removeFromRight(26));
        l1.removeFromRight(gapS);
        mute.setBounds(l1.removeFromRight(26));
        l1.removeFromRight(gapS);
        editBtn.setBounds(l1.removeFromRight(44));
        l1.removeFromRight(gap);
        name.setBounds(l1);
        r.removeFromTop(gapS);
        instBtn.setBounds(r.removeFromTop(22));
        meter.setBounds(juce::Rectangle<int>(owner.cardWidth, 0, PianoRollComponent::kKeyboardWidth, getHeight()).reduced(24, 6));
    }

    void paint(juce::Graphics& g) override
    {
        const int cardW = owner.cardWidth - 1, x0 = owner.cellsX(), x1 = getWidth();
        g.setColour(tc::panel.interpolatedWith(colour, active ? 0.30f : 0.10f));
        g.fillRect(0, 0, cardW, getHeight());
        g.setColour(colour);
        g.fillRect(0, 0, active ? 6 : 4, getHeight());
        g.setColour(tc::border);
        g.fillRect(cardW, 0, 1, getHeight());
        g.setColour(tc::keyWhite);
        g.fillRect(owner.cardWidth, 0, PianoRollComponent::kKeyboardWidth, getHeight());
        g.setColour(tc::border);
        g.drawVerticalLine(x0 - 1, 0.0f, (float)getHeight());

        g.saveState();
        g.reduceClipRegion(x0, 0, x1 - x0, getHeight());
        g.setColour(tc::rollBg);
        g.fillRect(x0, 0, x1 - x0, getHeight());
        // bar lines
        const auto bar = seq::ticksPerBar(app.project.timeSig);
        g.setColour(tc::gridBar.withAlpha(0.45f));
        for (auto t = std::max<Tick>(0, (Tick)roll.xToTick(0) / bar * bar); roll.tickToX((double)t) < x1 - x0; t += bar)
            g.drawVerticalLine(x0 + (int)roll.tickToX((double)t), 0.0f, (float)getHeight());
        // mini notes: the track's pitch range fills the lane height
        if (auto* t = app.project.midiTrack(id); t && !t->notes().empty()) {
            int lo = 127, hi = 0;
            for (auto& n : t->notes()) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
            lo -= 2; hi += 2;
            const float top = 5.0f, h = (float)getHeight() - 10.0f;
            const float rowH = std::clamp(h / (float)(hi - lo + 1), 3.0f, 6.0f);
            const double from = roll.xToTick(0), to = roll.xToTick(x1 - x0);
            g.setColour(colour.brighter(0.25f));
            for (auto& n : t->notes()) {
                if ((double)(n.start + n.length) < from || (double)n.start > to) continue;
                const float x = (float)(x0 + roll.tickToX((double)n.start));
                const float w = std::max(2.0f, (float)(n.length * roll.pxPerTick) - 1.0f);
                const float y = top + (float)(hi - n.pitch) / (float)(hi - lo) * (h - rowH);
                g.fillRoundedRectangle(x, y, w, rowH, std::min(1.5f, rowH * 0.5f));
            }
        } else {
            g.setColour(juce::Colour(0xff8994a1));
            g.setFont(theme::uiFont(12.5f));
            g.drawText("No notes yet - double-click to open the piano roll", x0 + 12, 0, x1 - x0 - 24, getHeight(), juce::Justification::centredLeft);
        }
        const float px = (float)(x0 + roll.tickToX(app.engine->position()));
        g.setColour(tc::playhead);
        g.drawLine(px, 0, px, (float)getHeight(), 1.5f);
        g.restoreState();
        g.setColour(tc::border);
        g.drawHorizontalLine(getHeight() - 1, 0.0f, (float)getWidth());
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        app.selectTrack(id);
        if (e.x >= owner.cellsX()) app.setPosition(roll.snap(roll.xToTick(e.x - owner.cellsX())));
    }
    void mouseDoubleClick(const juce::MouseEvent&) override { open(); }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (!roll.handleTimeWheel(e, w, e.x - owner.cellsX())) juce::Component::mouseWheelMove(e, w); // scroll the page
    }

private:
    void open() { if (owner.onOpenTrack) owner.onOpenTrack(id); }

    MidiOverviewComponent& owner;
    AppContext& app;
    PianoRollComponent& roll;
    model::TrackId id;
    juce::Colour colour = tc::accent;
    bool active = false;
    juce::Label name;
    juce::TextButton editBtn{"Edit"}, mute{"M"}, solo{"S"}, instBtn;
    LevelMeter meter;
};

MidiOverviewComponent::MidiOverviewComponent(AppContext& a, PianoRollComponent& r) : app(a), roll(r)
{
    app.addChangeListener(this);
    rebuild();
}

MidiOverviewComponent::~MidiOverviewComponent() { app.removeChangeListener(this); }

void MidiOverviewComponent::paint(juce::Graphics& g)
{
    g.fillAll(tc::rollBg);
    if (rows.empty()) {
        g.setColour(tc::panel);
        g.fillRect(0, 0, cardWidth - 1, getHeight());
        g.setColour(tc::textDim);
        g.setFont(theme::uiFont(13.0f));
        g.drawText("No instrument tracks", theme::gap + 4, 0, cardWidth, getHeight(), juce::Justification::centredLeft);
    }
}

void MidiOverviewComponent::resized()
{
    const int w = std::max(0, getWidth() - PianoRollComponent::kScrollbarWidth);
    int y = 0;
    for (auto& r : rows) { r->setBounds(0, y, w, kRowHeight); y += kRowHeight; }
}

void MidiOverviewComponent::changeListenerCallback(juce::ChangeBroadcaster*) { rebuild(); }

void MidiOverviewComponent::rebuild()
{
    const int oldH = preferredHeight();
    std::vector<model::TrackId> ids;
    for (auto& t : app.project.tracks())
        if (auto* m = dynamic_cast<model::MidiTrack*>(t.get()); m && !m->isStepLine()) ids.push_back(t->id());
    bool same = ids.size() == rows.size();
    for (size_t i = 0; same && i < ids.size(); ++i) same = rows[i]->trackId() == ids[i];
    if (!same) {
        rows.clear();
        for (auto id : ids) { rows.push_back(std::make_unique<Row>(*this, id)); addAndMakeVisible(*rows.back()); }
        resized();
    } else {
        for (auto& r : rows) r->refresh();
    }
    if (preferredHeight() != oldH && onHeightChanged) onHeightChanged();
}

void MidiOverviewComponent::viewChanged()
{
    for (auto& r : rows) r->repaint();
}

void MidiOverviewComponent::refresh()
{
    if (!isVisible()) return;
    for (auto& r : rows) r->updateMeter();
    const double x = roll.tickToX(app.engine->position());
    if (x == lastPlayX) return;
    for (auto& r : rows) r->playheadMoved(lastPlayX, x); // only the strips the playhead moves over
    lastPlayX = x;
}

} // namespace mc::gui
