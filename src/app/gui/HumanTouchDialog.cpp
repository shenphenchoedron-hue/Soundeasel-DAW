#include "gui/HumanTouchDialog.h"
#include "gui/Theme.h"
#include <algorithm>
#include <cmath>

namespace mc::gui {

namespace {
constexpr int kWidth = 560, kRowH = 28, kPad = 16, kLabelW = 130, kHintW = 150;

model::Tick timingRange() { return model::kPPQ / 4; } // 100 % placement = one 1/16 note
} // namespace

void HumanTouchDialog::show(AppContext& app, model::TrackId id)
{
    auto* t = app.project.midiTrack(id);
    if (!t || t->isStepLine()) return;
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new HumanTouchDialog(app, id));
    o.dialogTitle = "Human Touch - " + juce::String(t->name);
    o.dialogBackgroundColour = theme::col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync(); // modal
}

HumanTouchDialog::HumanTouchDialog(AppContext& a, model::TrackId i) : app(a), id(i)
{
    title.setFont(theme::uiFont(16.0f, true));
    title.setColour(juce::Label::textColourId, theme::col::text);
    info.setFont(theme::uiFont(13.0f));
    info.setColour(juce::Label::textColourId, theme::col::textDim);
    info.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(title);
    addAndMakeVisible(info);
    if (auto* t = app.project.midiTrack(id)) title.setText("Human Touch for \"" + juce::String(t->name) + "\"", juce::dontSendNotification);

    struct Setup { Row* row; const char* label; const char* key; double def; const char* tip; };
    const Setup rows[] = {
        {&notes, "Notes affected", "notes", 100.0, "How many of the notes are changed. They are picked at random: 50 % changes every second note on average."},
        {&timing, "Placement", "timing", 10.0, "How far a note may move earlier or later. 100 % = up to one 1/16 note."},
        {&length, "Length", "length", 10.0, "How much a note may get shorter or longer, in % of its own length."},
        {&velocity, "Velocity", "velocity", 0.0, "How much a note may get softer or louder, in % of its own velocity."},
    };
    for (auto& s : rows) {
        auto& r = *s.row;
        r.label.setText(s.label, juce::dontSendNotification);
        r.label.setFont(theme::uiFont(14.0f, true));
        r.label.setColour(juce::Label::textColourId, theme::col::text);
        r.label.setJustificationType(juce::Justification::centredRight);
        r.slider.setRange(0.0, 100.0, 1.0);
        r.slider.setTextValueSuffix(" %");
        r.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, kRowH - 6);
        r.slider.setValue(std::clamp(app.settings.humanizeSetting(s.key, s.def), 0.0, 100.0), juce::dontSendNotification);
        r.slider.setDoubleClickReturnValue(true, s.def);
        r.slider.setTooltip(s.tip);
        r.slider.onValueChange = [this] { update(); };
        r.label.setTooltip(s.tip);
        r.hint.setFont(theme::uiFont(12.5f));
        r.hint.setColour(juce::Label::textColourId, theme::col::textDim);
        for (juce::Component* c : {(juce::Component*)&r.label, (juce::Component*)&r.slider, (juce::Component*)&r.hint}) addAndMakeVisible(c);
    }
    onlyNew.setToggleState(app.settings.humanizeSetting("onlyNew", 1.0) != 0.0, juce::dontSendNotification);
    onlyNew.setTooltip("On: notes that had Human Touch before keep their place, so you can give new notes Human Touch without "
                       "moving the old ones further. Off: all notes are varied again.");
    onlyNew.onClick = [this] { update(); };
    addAndMakeVisible(onlyNew);

    applyBtn.setTooltip("Apply random variation to the notes (can be undone with Ctrl/Cmd+Z)");
    applyBtn.onClick = [this] { apply(); };
    cancelBtn.onClick = [this] { close(0); };
    addAndMakeVisible(applyBtn);
    addAndMakeVisible(cancelBtn);
    update();
    setSize(kWidth, kPad + 26 + 40 + 4 * (kRowH + 8) + 8 + kRowH + 16 + 30 + kPad);
}

model::HumanizeSettings HumanTouchDialog::settings() const
{
    model::HumanizeSettings s;
    s.notesPercent = notes.slider.getValue();
    s.timingPercent = timing.slider.getValue();
    s.timingRange = timingRange();
    s.lengthPercent = length.slider.getValue();
    s.velocityPercent = velocity.slider.getValue();
    s.onlyNew = onlyNew.getToggleState();
    return s;
}

void HumanTouchDialog::update()
{
    auto* t = app.project.midiTrack(id);
    const auto s = settings();
    const int total = t ? (int)t->notes().size() : 0;
    int done = 0;
    if (t) for (auto& n : t->notes()) done += n.humanized ? 1 : 0;
    const int candidates = t ? (int)model::humanizeCandidates(t->notes(), s).size() : 0;
    const int changed = (int)std::llround(s.notesPercent / 100.0 * candidates);

    juce::String text;
    if (total == 0) text = "This track has no notes yet. Place notes in the piano roll first.";
    else {
        text << total << (total == 1 ? " note" : " notes") << " on this track";
        if (done > 0) text << ", " << done << " had Human Touch before, " << (total - done) << " new";
        text << ".\n" << (changed == 1 ? "1 note" : juce::String(changed) + " notes") << " will be changed";
        if (s.onlyNew && done > 0) text << " (only new notes)";
        text << ".";
    }
    info.setText(text, juce::dontSendNotification);

    const double ms = (double)s.timingRange * s.timingPercent / 100.0 / model::kPPQ * 60.0 / std::max(1.0, app.project.tempoBpm) * 1000.0;
    timing.hint.setText(juce::String::fromUTF8("\xc2\xb1 ") + juce::String(ms, 0) + " ms at " + juce::String(app.project.tempoBpm, 0) + " BPM",
                        juce::dontSendNotification);
    notes.hint.setText("picked at random", juce::dontSendNotification);
    length.hint.setText(juce::String::fromUTF8("\xc2\xb1 ") + juce::String((int)s.lengthPercent) + " % of each note", juce::dontSendNotification);
    velocity.hint.setText(s.velocityPercent > 0 ? juce::String::fromUTF8("\xc2\xb1 ") + juce::String((int)s.velocityPercent) + " % loudness" : juce::String("off"),
                          juce::dontSendNotification);
    const bool anything = s.timingPercent > 0 || s.lengthPercent > 0 || s.velocityPercent > 0;
    applyBtn.setEnabled(changed > 0 && anything);
}

void HumanTouchDialog::apply()
{
    const auto s = settings();
    app.settings.setHumanizeSetting("notes", s.notesPercent);
    app.settings.setHumanizeSetting("timing", s.timingPercent);
    app.settings.setHumanizeSetting("length", s.lengthPercent);
    app.settings.setHumanizeSetting("velocity", s.velocityPercent);
    app.settings.setHumanizeSetting("onlyNew", s.onlyNew ? 1.0 : 0.0);
    app.humanizeTrack(id, s);
    close(1);
}

void HumanTouchDialog::close(int result)
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState(result); // deletes this dialog
}

void HumanTouchDialog::paint(juce::Graphics& g)
{
    g.fillAll(theme::col::panel);
}

void HumanTouchDialog::resized()
{
    auto r = getLocalBounds().reduced(kPad);
    title.setBounds(r.removeFromTop(26));
    info.setBounds(r.removeFromTop(40));
    for (auto* row : {&notes, &timing, &length, &velocity}) {
        r.removeFromTop(8);
        auto l = r.removeFromTop(kRowH);
        row->label.setBounds(l.removeFromLeft(kLabelW));
        l.removeFromLeft(theme::gap);
        row->hint.setBounds(l.removeFromRight(kHintW));
        l.removeFromRight(theme::gapS);
        row->slider.setBounds(l);
    }
    r.removeFromTop(8);
    onlyNew.setBounds(r.removeFromTop(kRowH).withTrimmedLeft(kLabelW + theme::gap - 4));
    auto buttons = r.removeFromBottom(30);
    cancelBtn.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(theme::gap);
    applyBtn.setBounds(buttons.removeFromRight(160));
}

} // namespace mc::gui
