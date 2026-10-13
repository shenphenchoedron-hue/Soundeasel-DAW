#include "gui/MixerComponent.h"
#include "gui/EffectEditors.h"
#include "gui/EqEditor.h"
#include "gui/InsertsComponent.h"
#include "gui/SettingsComponent.h"
#include "gui/Theme.h"
#include "gui/TrackSliders.h"
#include "gui/TrackListComponent.h"

namespace mc::gui {

namespace {
namespace tc = theme::col;

juce::String panText(double v)
{
    const int p = juce::roundToInt(v * 100);
    return p == 0 ? juce::String("C") : (p < 0 ? "L " + juce::String(-p) : "R " + juce::String(p));
}
} // namespace

class MixerComponent::Strip final : public juce::Component, public juce::SettableTooltipClient {
public:
    // id 0 = Mains
    Strip(AppContext& a, model::TrackId i) : app(a), id(i)
    {
        for (juce::Component* c : std::initializer_list<juce::Component*>{&name, &source, &inputBox, &outBox, &pan, &mute, &solo, &arm, &fader, &meter, &value,
                                                                          &eqBtn, &eqEdit, &compBtn, &compEdit, &revBtn, &revEdit})
            addChildComponent(c);
        for (juce::Component* c : std::initializer_list<juce::Component*>{&name, &fader, &meter, &value, &mute})
            c->setVisible(true);

        name.setFont(theme::uiFont(13.5f, true));
        name.setJustificationType(juce::Justification::centred);
        name.setColour(juce::Label::textColourId, tc::text);
        name.setInterceptsMouseClicks(false, false);
        value.setFont(theme::monoFont(12.5f));
        value.setJustificationType(juce::Justification::centred);
        value.setColour(juce::Label::textColourId, tc::displayText);
        value.setColour(juce::Label::backgroundColourId, tc::display);
        value.setInterceptsMouseClicks(false, false);

        fader::setup(fader);
        fader.setTooltip(id ? "Track volume (double-click = 0 dB)" : "Mains volume: everything you hear (double-click = 0 dB)");
        fader.onDragStart = [this] { dragging = true; beginEdit(); };
        fader.onValueChange = [this] {
            const float g = (float)fader::dbToGain(fader.getValue());
            value.setText(fader::text(g), juce::dontSendNotification);
            if (dragging) liveVolume(g);
            else { beginEdit(); liveVolume(g); endEdit(); } // wheel, double-click
        };
        fader.onDragEnd = [this] { dragging = false; endEdit(); };

        mute.setClickingTogglesState(true);
        theme::setStyle(mute, theme::styleWarm);
        mute.setTooltip(id ? "Mute" : "Mute Mains (silences everything)");
        mute.onClick = [this] {
            if (id == 0) { auto after = app.project.master; after.mute = mute.getToggleState(); app.setMaster(app.project.master, after); }
            else app.changeTrack(id, [&](auto& p) { p.mute = mute.getToggleState(); });
        };
        pan.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange(-1, 1, 0.01);
        pan.setDoubleClickReturnValue(true, 0.0);
        pan.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        pan.textFromValueFunction = panText;
        pan.setPopupDisplayEnabled(true, true, nullptr);
        pan.setTooltip("Pan (double-click = centre)");
        pan.onDragStart = [this] { panDragging = true; beginEdit(); };
        pan.onValueChange = [this] {
            if (panDragging) livePan((float)pan.getValue());
            else { beginEdit(); livePan((float)pan.getValue()); endEdit(); }
        };
        pan.onDragEnd = [this] { panDragging = false; endEdit(); };
        pan.setVisible(true);
        if (id == 0) {
            pan.setTooltip("Stereo balance of Mains (double-click = centre)");
            outBox.setVisible(true);
            outBox.setTooltip("Stereo output pair of your sound device that Mains plays on");
            outBox.onChange = [this] {
                const int sel = outBox.getSelectedId();
                if (sel == 1000) { SettingsComponent::show(app); refresh(); }
                else if (sel > 0) app.setMainsOutputPair(sel - 1);
            };
            refresh();
            return;
        }

        inserts = std::make_unique<InsertsComponent>(app, id);
        addAndMakeVisible(*inserts);
        // built-in effects: on/off + editor
        for (auto* b : {&eqBtn, &eqEdit, &compBtn, &revBtn, &compEdit, &revEdit}) b->setVisible(true);
        eqBtn.setClickingTogglesState(true);
        theme::setStyle(eqBtn, theme::stylePlay);
        eqBtn.setTooltip("Parametric EQ on/off (turn off if you use an EQ plugin)");
        eqEdit.setTooltip("EQ settings: add bands and set their frequency, gain and width");
        eqBtn.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.eq.enabled = eqBtn.getToggleState(); }); };
        eqEdit.onClick = [this] { showEqEditor(app, id, eqEdit); };
        compBtn.setClickingTogglesState(true);
        revBtn.setClickingTogglesState(true);
        theme::setStyle(compBtn, theme::stylePlay);
        theme::setStyle(revBtn, theme::stylePlay);
        compBtn.setTooltip("Compressor on/off (turn off if you use a compressor plugin)");
        revBtn.setTooltip("Reverb on/off (turn off if you use a reverb plugin)");
        compEdit.setTooltip("Compressor settings: threshold, ratio, knee, attack, release, makeup, peak/RMS");
        revEdit.setTooltip("Reverb settings: mix, size, damping, width, pre-delay");
        compBtn.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.compressor.enabled = compBtn.getToggleState(); }); };
        revBtn.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.reverb.enabled = revBtn.getToggleState(); }); };
        compEdit.onClick = [this] { showEffectEditor(app, id, StripEffect::Compressor, compEdit); };
        revEdit.onClick = [this] { showEffectEditor(app, id, StripEffect::Reverb, revEdit); };

        connectTrackSliders(app, id, fader, pan, [this] { value.setText(fader::text(fader::dbToGain(fader.getValue())), juce::dontSendNotification); });
        solo.setVisible(true);
        solo.setClickingTogglesState(true);
        solo.setTooltip("Solo");
        solo.onClick = [this] { app.changeTrack(id, [&](auto& p) { p.solo = solo.getToggleState(); }); };


        if (app.project.audioTrack(id)) {
            arm.setVisible(true);
            inputBox.setVisible(true);
            arm.setClickingTogglesState(true);
            theme::setStyle(arm, theme::styleRecord);
            arm.setTooltip("Record enable");
            arm.onClick = [this] { app.setTrackArmed(id, arm.getToggleState()); };
            inputBox.setTooltip("Audio input");
            inputBox.onChange = [this] {
                const int sel = inputBox.getSelectedId();
                if (sel == 3) { SettingsComponent::show(app); refresh(); }
                else if (sel == 1) app.setTrackInput(id, -1, false);
                else if (sel >= 200) app.setTrackInput(id, sel - 200, true);
                else if (sel >= 100) app.setTrackInput(id, sel - 100, false);
            };
        } else {
            source.setVisible(true);
            theme::setStyle(source, theme::styleSelector);
            source.onClick = [this] { showInstrumentMenu(app, id, source); };
        }
        refresh();
    }

    model::TrackId trackId() const { return id; }

    // Every strip gets room for the same number of plugin rows (the longest chain in the mixer);
    // folded, the list is one summary line on every strip.
    void setInsertLayout(int rows, bool fold, std::function<void()> toggleFold)
    {
        if (inserts) inserts->onToggleCollapsed = std::move(toggleFold);
        if (rows == insertRows && fold == insertsFolded) return;
        insertRows = rows;
        insertsFolded = fold;
        if (inserts) inserts->setCollapsed(fold);
        resized();
    }

    void refresh()
    {
        if (id == 0) {
            const auto& m = app.project.master;
            name.setText("Mains", juce::dontSendNotification);
            mute.setToggleState(m.mute, juce::dontSendNotification);
            if (!dragging) fader.setValue(fader::gainToDb(m.volume), juce::dontSendNotification);
            value.setText(fader::text(m.volume), juce::dontSendNotification);
            if (!panDragging) pan.setValue(m.balance, juce::dontSendNotification);
            colour = tc::accent2;
            auto* d = app.deviceManager.getCurrentAudioDevice();
            outputName = app.audioOutputOk() && d ? d->getName() : juce::String("No sound device!");
            setTooltip("All tracks are mixed here (stereo) and sent to: " + outputName);
            outBox.clear(juce::dontSendNotification);
            const auto pairs = app.mainsOutputPairs();
            for (int i = 0; i < pairs.size(); ++i) outBox.addItem(pairs[i], i + 1);
            if (pairs.isEmpty()) outBox.addItem(app.audioOutputOk() ? "Mono device (1 output)" : "No sound device", 999);
            outBox.addSeparator();
            outBox.addItem("Sound settings...", 1000);
            outBox.setSelectedId(pairs.isEmpty() ? 999 : app.mainsOutputPair() + 1, juce::dontSendNotification);
            repaint();
            return;
        }
        auto* t = app.project.track(id);
        if (!t) return;
        name.setText(t->name, juce::dontSendNotification);
        mute.setToggleState(t->mute, juce::dontSendNotification);
        solo.setToggleState(t->solo, juce::dontSendNotification);
        if (!dragging) fader.setValue(fader::gainToDb(t->volume), juce::dontSendNotification);
        if (!panDragging) pan.setValue(t->pan, juce::dontSendNotification);
        value.setText(fader::text(t->volume), juce::dontSendNotification);
        colour = t->colour ? juce::Colour(t->colour) : tc::accent;
        active = app.project.activeTrack == id;
        if (inserts) {
            inserts->refresh();
            if (insertsLast != t->insertsAfterBuiltins) { insertsLast = t->insertsAfterBuiltins; resized(); }
        }
        eqBtn.setToggleState(t->eq.enabled, juce::dontSendNotification);
        eqBtn.setButtonText(t->eq.bands.empty() ? "EQ" : "EQ " + juce::String((int)t->eq.bands.size()));
        compBtn.setToggleState(t->compressor.enabled, juce::dontSendNotification);
        revBtn.setToggleState(t->reverb.enabled, juce::dontSendNotification);
        if (auto* m = dynamic_cast<model::MidiTrack*>(t)) {
            kind = m->isStepLine() ? "STEP SEQ" : "INSTRUMENT";
            const bool missing = app.isPluginMissing(id);
            source.setButtonText(m->plugin.empty() ? "(no instrument)" : juce::String(m->plugin.name));
            source.setTooltip(missing ? app.pluginError(id) : "Instrument: click to choose the sound of this track");
            theme::setStyle(source, missing || m->plugin.empty() ? theme::styleDanger : theme::styleSelector);
            source.repaint();
        } else if (auto* a = dynamic_cast<model::AudioTrack*>(t)) {
            kind = "AUDIO";
            arm.setToggleState(a->armed, juce::dontSendNotification);
            const auto inputs = app.inputNames();
            inputBox.clear(juce::dontSendNotification);
            if (inputs.isEmpty()) { inputBox.addItem("No inputs...", 3); inputBox.setSelectedId(3, juce::dontSendNotification); }
            else {
                inputBox.addItem("No input", 1);
                for (int i = 0; i < inputs.size(); ++i) inputBox.addItem("In " + juce::String(i + 1), 100 + i);
                for (int i = 0; i + 1 < inputs.size(); i += 2) inputBox.addItem("In " + juce::String(i + 1) + "+" + juce::String(i + 2), 200 + i);
                inputBox.addSeparator();
                inputBox.addItem("Audio settings...", 3);
                int sel = 1;
                if (a->input >= 0 && a->input < inputs.size()) sel = a->stereoInput && a->input + 1 < inputs.size() ? 200 + a->input : 100 + a->input;
                inputBox.setSelectedId(sel, juce::dontSendNotification);
            }
        }
        repaint();
    }

    void updateMeter()
    {
        const auto l = app.levels(id);
        meter.push(l.left, l.right);
        if (id != 0) {
            const float gr = std::max(l.gainReduction, grShown - 0.6f);
            if (std::abs(gr - grShown) > 0.05f) { grShown = gr; repaint(grArea); }
        }
    }

    void resized() override
    {
        using namespace theme;
        auto r = getLocalBounds().reduced(5, 4);
        r.removeFromTop(4);                         // colour bar
        r.removeFromTop(14);                        // kind label
        name.setBounds(r.removeFromTop(20));
        r.removeFromTop(gapS);
        auto src = r.removeFromTop(22);
        source.setBounds(src);
        inputBox.setBounds(src);
        outBox.setBounds(src);
        r.removeFromTop(gapS);
        auto knob = r.removeFromTop(38);
        pan.setBounds(knob.withSizeKeepingCentre(38, 38));
        r.removeFromTop(gapS);
        auto btns = r.removeFromTop(22);
        if (id == 0) mute.setBounds(btns.withSizeKeepingCentre(40, 22));
        else {
            const int n = arm.isVisible() ? 3 : 2;
            const int w = (btns.getWidth() - (n - 1) * gapS) / n;
            mute.setBounds(btns.removeFromLeft(w)); btns.removeFromLeft(gapS);
            solo.setBounds(btns.removeFromLeft(w)); btns.removeFromLeft(gapS);
            if (n == 3) arm.setBounds(btns);
        }
        r.removeFromTop(gap);
        if (id != 0) {
            // shown in processing order: FX plugins before or after the built-in effects.
            // The plugin list shows every plugin; only when the strip is too short is the fader made
            // smaller (down to kMinFader) and, after that, the list scrolls.
            constexpr int kMinFader = 90;
            const int builtinsH = 22 + gapS + 22 + 5 + 22 + gap;
            const int belowH = 20 + gapS + kMinFader; // value label + fader
            const int room = r.getHeight() - builtinsH - gap - belowH;
            const int insertsH = insertsFolded ? InsertsComponent::collapsedHeight()
                                               : std::max(InsertsComponent::preferredHeight(), std::min(InsertsComponent::heightFor(insertRows), room));
            auto placeInserts = [&] {
                if (!inserts) return;
                inserts->setBounds(r.removeFromTop(insertsH));
                r.removeFromTop(gap);
            };
            if (!insertsLast) placeInserts();
            auto q = r.removeFromTop(22);
            eqEdit.setBounds(q.removeFromRight(24));
            q.removeFromRight(2);
            eqBtn.setBounds(q);
            r.removeFromTop(gapS);
            auto c = r.removeFromTop(22);
            compEdit.setBounds(c.removeFromRight(24));
            c.removeFromRight(2);
            compBtn.setBounds(c);
            grArea = r.removeFromTop(5).reduced(1, 1);
            auto v = r.removeFromTop(22);
            revEdit.setBounds(v.removeFromRight(24));
            v.removeFromRight(2);
            revBtn.setBounds(v);
            r.removeFromTop(gap);
            if (insertsLast) placeInserts();
        }
        value.setBounds(r.removeFromBottom(20));
        r.removeFromBottom(gapS);
        auto area = r;
        meter.setBounds(area.removeFromRight(id == 0 ? 20 : 14).withTrimmedBottom(id == 0 ? 11 : 0));
        area.removeFromRight(gapS);
        scaleArea = area.removeFromRight(22);
        fader.setBounds(area);
    }

    void paint(juce::Graphics& g) override
    {
        const auto bg = id == 0 ? tc::panel.interpolatedWith(tc::accent2, 0.10f) : tc::panel.interpolatedWith(colour, active ? 0.22f : 0.07f);
        g.fillAll(bg);
        g.setColour(colour);
        g.fillRect(0, 0, getWidth(), 5);
        g.setColour(tc::textDim);
        g.setFont(theme::uiFont(10.5f, true));
        g.drawText(id == 0 ? juce::String(app.isStereoOutput() ? "STEREO OUT" : "MONO OUT") : kind,
                   getLocalBounds().withTrimmedTop(8).withHeight(12), juce::Justification::centred);
        if (id == 0) {
            // L / R under the stereo meter, "BAL" next to the balance knob
            g.setFont(theme::uiFont(9.5f, true));
            g.setColour(tc::textDim);
            const auto mb = meter.getBounds();
            g.drawText("L", mb.getX() - 2, mb.getBottom() + 1, mb.getWidth() / 2 + 2, 10, juce::Justification::centred);
            g.drawText("R", mb.getCentreX(), mb.getBottom() + 1, mb.getWidth() / 2 + 2, 10, juce::Justification::centred);
            g.drawText("BAL", pan.getX() - 30, pan.getY(), 28, pan.getHeight(), juce::Justification::centredRight);
        }
        // dB scale next to the fader
        g.setFont(theme::uiFont(9.5f));
        g.setColour(tc::textDim);
        const auto fb = fader.getBounds();
        const float y0 = (float)fb.getY() + 8, y1 = (float)fb.getBottom() - 8;
        for (double db : {6.0, 0.0, -6.0, -12.0, -24.0, -40.0, -60.0}) {
            const float prop = (float)fader.valueToProportionOfLength(db);
            const float y = y1 - prop * (y1 - y0);
            g.drawText(db <= fader::kMinDb ? juce::String("-inf") : (db > 0 ? "+" : "") + juce::String((int)db),
                       scaleArea.getX(), (int)y - 6, scaleArea.getWidth(), 12, juce::Justification::centredRight);
        }
        if (id != 0 && compBtn.getToggleState() && grShown > 0.1f) { // compressor gain reduction (0..24 dB, from the right)
            const int w = std::max(2, juce::roundToInt((float)grArea.getWidth() * juce::jlimit(0.0f, 1.0f, grShown / 24.0f)));
            g.setColour(tc::warm);
            g.fillRoundedRectangle(grArea.withLeft(grArea.getRight() - w).toFloat(), 1.5f);
        }
        g.setColour(tc::border);
        g.drawVerticalLine(getWidth() - 1, 0.0f, (float)getHeight());
        if (active) {
            g.setColour(colour);
            g.drawRect(getLocalBounds().withTrimmedRight(1), 2);
        }
    }

    void mouseDown(const juce::MouseEvent&) override { if (id) app.selectTrack(id); }

private:
    // Mains only (track strips use connectTrackSliders): live while dragging, one undo step.
    void beginEdit() { if (!masterBefore) masterBefore = app.project.master; }
    void endEdit()
    {
        if (masterBefore && !(*masterBefore == app.project.master)) app.setMaster(*masterBefore, app.project.master);
        masterBefore.reset();
    }
    void liveVolume(float g) { auto m = app.project.master; m.volume = g; app.setMasterLive(m); }
    void livePan(float p) { auto m = app.project.master; m.balance = p; app.setMasterLive(m); }

    AppContext& app;
    model::TrackId id;
    juce::Colour colour = tc::accent;
    juce::String kind, outputName;
    bool active = false, dragging = false, panDragging = false;
    std::optional<model::MasterBus> masterBefore;
    juce::Label name, value;
    juce::TextButton source, mute{"M"}, solo{"S"}, arm{juce::String::fromUTF8("\xe2\x97\x8f")};
    std::unique_ptr<InsertsComponent> inserts;
    bool insertsLast = false;
    int insertRows = InsertsComponent::kVisibleRows;
    bool insertsFolded = false;
    juce::TextButton eqBtn{"EQ"}, eqEdit{juce::String::fromUTF8("\xe2\x80\xa6")};
    juce::TextButton compBtn{"COMP"}, revBtn{"REVERB"}, compEdit{juce::String::fromUTF8("\xe2\x80\xa6")}, revEdit{juce::String::fromUTF8("\xe2\x80\xa6")}; // …
    juce::Rectangle<int> grArea;
    float grShown = 0;
    juce::ComboBox inputBox, outBox;
    juce::Slider pan, fader{juce::Slider::LinearVertical, juce::Slider::NoTextBox};
    LevelMeter meter;
    juce::Rectangle<int> scaleArea;
};

// ============================================================ mixer
MixerComponent::MixerComponent(AppContext& a) : app(a)
{
    addAndMakeVisible(viewport);
    addAndMakeVisible(closeBtn);
    viewport.setViewedComponent(&content, false);
    viewport.setScrollBarsShown(false, true);
    closeBtn.setTooltip("Hide the mixer (F3)");
    closeBtn.onClick = [this] { if (onClose) onClose(); };
    addAndMakeVisible(foldBtn);
    insertsFolded = app.settings.mixerInsertsCollapsed();
    foldBtn.onClick = [this] { setInsertsFolded(!insertsFolded); };
    updateFoldButton();
    master = std::make_unique<Strip>(app, 0);
    addAndMakeVisible(*master);
    app.addChangeListener(this);
    rebuild();
}

MixerComponent::~MixerComponent() { app.removeChangeListener(this); }

void MixerComponent::paint(juce::Graphics& g)
{
    g.fillAll(tc::appBg);
    g.setColour(tc::panel);
    g.fillRect(0, 0, getWidth(), kHeaderHeight);
    g.setColour(tc::border);
    g.drawHorizontalLine(0, 0.0f, (float)getWidth());
    g.drawHorizontalLine(kHeaderHeight - 1, 0.0f, (float)getWidth());
    g.setColour(tc::textDim);
    g.setFont(theme::uiFont(13.0f, true));
    g.drawText("MIXER", theme::gap + 4, 0, 60, kHeaderHeight, juce::Justification::centredLeft);
    g.setFont(theme::uiFont(12.5f));
    g.drawText("Every track has a strip here. All tracks are mixed into Mains, which goes to your speakers / headphones.",
               80, 0, getWidth() - 270, kHeaderHeight, juce::Justification::centredLeft);
    if (strips.empty()) {
        g.setFont(theme::uiFont(14.0f));
        g.drawText("No tracks yet", viewport.getBounds(), juce::Justification::centred);
    }
}

void MixerComponent::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop(kHeaderHeight);
    closeBtn.setBounds(header.removeFromRight(70).reduced(6, 3));
    foldBtn.setBounds(header.removeFromRight(110).reduced(0, 3));
    master->setBounds(r.removeFromRight(kStripWidth + 12).withTrimmedLeft(2));
    viewport.setBounds(r);
    const bool scrolls = (int)strips.size() * kStripWidth > r.getWidth();
    const int h = r.getHeight() - (scrolls ? viewport.getScrollBarThickness() : 0);
    content.setSize(std::max(r.getWidth(), (int)strips.size() * kStripWidth), std::max(100, h));
    for (size_t i = 0; i < strips.size(); ++i) strips[i]->setBounds((int)i * kStripWidth, 0, kStripWidth, content.getHeight());
}

void MixerComponent::changeListenerCallback(juce::ChangeBroadcaster*) { rebuild(); }

int MixerComponent::preferredHeight() const
{
    if (insertsFolded) return kPreferredHeight - (InsertsComponent::preferredHeight() - InsertsComponent::collapsedHeight());
    const int extra = std::max(0, insertRows - InsertsComponent::kVisibleRows) * (InsertsComponent::kRowHeight + 2);
    return kPreferredHeight + extra;
}

void MixerComponent::setInsertsFolded(bool fold)
{
    if (fold == insertsFolded) return;
    insertsFolded = fold;
    app.settings.setMixerInsertsCollapsed(fold);
    updateFoldButton();
    applyInsertLayout();
    if (onPreferredHeightChanged) onPreferredHeightChanged();
}

void MixerComponent::updateFoldButton()
{
    foldBtn.setButtonText(juce::String::fromUTF8(insertsFolded ? "FX plugins \xe2\x96\xb8" : "FX plugins \xe2\x96\xbe")); // ▸ / ▾
    foldBtn.setTooltip(insertsFolded ? "Unfold the FX plugin lists on all strips (to add, move or open plugins)"
                                     : "Fold the FX plugin lists on all strips to one line each (more room for the faders)");
}

void MixerComponent::applyInsertLayout()
{
    for (auto& s : strips) s->setInsertLayout(insertRows, insertsFolded, [this] { setInsertsFolded(!insertsFolded); });
}

void MixerComponent::rebuild()
{
    std::vector<model::TrackId> ids;
    for (auto& t : app.project.tracks()) ids.push_back(t->id());
    bool same = ids.size() == strips.size();
    for (size_t i = 0; same && i < ids.size(); ++i) same = strips[i]->trackId() == ids[i];
    if (!same) {
        strips.clear();
        for (auto id : ids) { strips.push_back(std::make_unique<Strip>(app, id)); content.addAndMakeVisible(*strips.back()); }
        resized();
        repaint();
    } else {
        for (auto& s : strips) s->refresh();
    }
    master->refresh();

    int rowsNeeded = InsertsComponent::kVisibleRows;
    for (auto& t : app.project.tracks()) rowsNeeded = std::max(rowsNeeded, (int)t->inserts.size());
    if (rowsNeeded != insertRows || !same) {
        const bool heightChanged = rowsNeeded != insertRows && !insertsFolded;
        insertRows = rowsNeeded;
        applyInsertLayout();
        if (heightChanged && onPreferredHeightChanged) onPreferredHeightChanged();
    }
}

void MixerComponent::refreshMeters()
{
    for (auto& s : strips) s->updateMeter();
    master->updateMeter();
}

} // namespace mc::gui
