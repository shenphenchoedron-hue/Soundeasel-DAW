#include "gui/EffectEditors.h"
#include "gui/Theme.h"
#include <optional>

namespace mc::gui {

namespace {
namespace tc = theme::col;

class EffectEditor final : public juce::Component, private juce::Timer {
public:
    EffectEditor(AppContext& a, model::TrackId i, StripEffect k) : app(a), id(i), kind(k)
    {
        if (auto* t = app.project.track(id)) before = model::TrackProperties::from(*t);
        const auto& p = *before;
        title.setText(juce::String(kind == StripEffect::Compressor ? "Compressor" : "Reverb") + "  -  " + juce::String(p.name), juce::dontSendNotification);
        title.setFont(theme::uiFont(15.0f, true));
        addAndMakeVisible(title);
        onBtn.setClickingTogglesState(true);
        theme::setStyle(onBtn, theme::stylePlay);
        onBtn.onClick = [this] { edit([&](model::TrackProperties& q) {
            if (kind == StripEffect::Compressor) q.compressor.enabled = onBtn.getToggleState(); else q.reverb.enabled = onBtn.getToggleState(); }); updateOnText(); };
        addAndMakeVisible(onBtn);

        if (kind == StripEffect::Compressor) {
            const auto& c = p.compressor;
            onBtn.setToggleState(c.enabled, juce::dontSendNotification);
            addKnob("Threshold", -60, 0, 0.1, c.thresholdDb, -30, " dB", [](auto& q, double v) { q.compressor.thresholdDb = (float)v; });
            addKnob("Ratio", 1, 20, 0.1, c.ratio, 4, ":1", [](auto& q, double v) { q.compressor.ratio = (float)v; });
            addKnob("Knee", 0, 24, 0.1, c.kneeDb, 12, " dB", [](auto& q, double v) { q.compressor.kneeDb = (float)v; });
            addKnob("Attack", 0.1, 200, 0.1, c.attackMs, 10, " ms", [](auto& q, double v) { q.compressor.attackMs = (float)v; });
            addKnob("Release", 5, 2000, 1, c.releaseMs, 150, " ms", [](auto& q, double v) { q.compressor.releaseMs = (float)v; });
            addKnob("Makeup", 0, 24, 0.1, c.makeupDb, 12, " dB", [](auto& q, double v) { q.compressor.makeupDb = (float)v; });
            detector.addItem("Peak detector", 1);
            detector.addItem("RMS detector", 2);
            detector.setSelectedId(c.rms ? 2 : 1, juce::dontSendNotification);
            detector.setTooltip("Peak reacts to every transient; RMS follows the average loudness (smoother)");
            detector.onChange = [this] { edit([&](model::TrackProperties& q) { q.compressor.rms = detector.getSelectedId() == 2; }); };
            addAndMakeVisible(detector);
            startTimerHz(30);
        } else {
            const auto& r = p.reverb;
            onBtn.setToggleState(r.enabled, juce::dontSendNotification);
            addKnob("Mix", 0, 100, 1, r.mix * 100.0, 50, " %", [](auto& q, double v) { q.reverb.mix = (float)(v / 100.0); });
            addKnob("Size", 0, 100, 1, r.size * 100.0, 50, " %", [](auto& q, double v) { q.reverb.size = (float)(v / 100.0); });
            addKnob("Damping", 0, 100, 1, r.damping * 100.0, 50, " %", [](auto& q, double v) { q.reverb.damping = (float)(v / 100.0); });
            addKnob("Width", 0, 100, 1, r.width * 100.0, 50, " %", [](auto& q, double v) { q.reverb.width = (float)(v / 100.0); });
            addKnob("Pre-delay", 0, 100, 1, r.preDelayMs, 30, " ms", [](auto& q, double v) { q.reverb.preDelayMs = (float)v; });
        }
        defaultsBtn.setTooltip("Reset all settings of this effect");
        defaultsBtn.onClick = [this] {
            edit([&](model::TrackProperties& q) {
                if (kind == StripEffect::Compressor) { const bool on = q.compressor.enabled; q.compressor = {}; q.compressor.enabled = on; }
                else { const bool on = q.reverb.enabled; q.reverb = {}; q.reverb.enabled = on; }
            });
            refreshKnobs();
        };
        addAndMakeVisible(defaultsBtn);
        updateOnText();
        const int cols = 3, rows = ((int)knobs.size() + cols - 1) / cols;
        setSize(cols * kKnobW + 2 * kPad, kPad + 28 + 8 + rows * kKnobH + (kind == StripEffect::Compressor ? 46 : 0) + 34 + kPad);
    }

    ~EffectEditor() override
    {
        // one undo step for everything changed while the editor was open
        auto* t = app.project.track(id);
        if (!t || !before) return;
        auto after = model::TrackProperties::from(*t);
        if (after.compressor == before->compressor && after.reverb == before->reverb) return;
        before->applyTo(*t);
        app.undo.perform(std::make_unique<model::SetTrackPropertiesCommand>(id, *before, after));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced(kPad);
        auto top = r.removeFromTop(28);
        onBtn.setBounds(top.removeFromRight(64));
        title.setBounds(top);
        r.removeFromTop(8);
        for (size_t i = 0; i < knobs.size(); ++i) {
            const int col = (int)i % 3, row = (int)i / 3;
            auto cell = juce::Rectangle<int>(r.getX() + col * kKnobW, r.getY() + row * kKnobH, kKnobW, kKnobH).reduced(4, 0);
            knobs[i]->label.setBounds(cell.removeFromTop(16));
            knobs[i]->slider.setBounds(cell.removeFromTop(kKnobH - 20));
        }
        r.removeFromTop(((int)knobs.size() + 2) / 3 * kKnobH);
        if (kind == StripEffect::Compressor) {
            r.removeFromTop(4);
            grArea = r.removeFromTop(16);
            r.removeFromTop(4);
            auto d = r.removeFromTop(22);
            detector.setBounds(d.removeFromLeft(150));
        }
        r.removeFromTop(8);
        defaultsBtn.setBounds(r.removeFromTop(24).removeFromRight(90));
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(tc::panel);
        if (kind != StripEffect::Compressor) return;
        // gain reduction meter (0 .. 24 dB, grows to the left from the right edge)
        auto bar = grArea.withTrimmedLeft(110).toFloat();
        g.setColour(tc::textDim);
        g.setFont(theme::uiFont(12.0f, true));
        g.drawText("Gain reduction", grArea.withWidth(105), juce::Justification::centredLeft);
        g.setColour(tc::display);
        g.fillRoundedRectangle(bar, 3.0f);
        const float w = bar.getWidth() * juce::jlimit(0.0f, 1.0f, grShown / 24.0f);
        g.setColour(tc::warm);
        g.fillRoundedRectangle(bar.withLeft(bar.getRight() - w), 3.0f);
        g.setColour(tc::displayText);
        g.setFont(theme::monoFont(11.5f));
        g.drawText(juce::String(grShown, 1) + " dB", bar.reduced(6, 0).toNearestInt(), juce::Justification::centredLeft);
    }

private:
    static constexpr int kKnobW = 92, kKnobH = 96, kPad = 12;
    using Setter = std::function<void(model::TrackProperties&, double)>;
    struct Knob {
        juce::Label label;
        juce::Slider slider{juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow};
        Setter set;
    };

    void addKnob(const juce::String& name, double lo, double hi, double step, double value, double mid, const juce::String& suffix, Setter set)
    {
        auto k = std::make_unique<Knob>();
        k->label.setText(name, juce::dontSendNotification);
        k->label.setJustificationType(juce::Justification::centred);
        k->label.setFont(theme::uiFont(12.5f, true));
        k->label.setColour(juce::Label::textColourId, tc::textDim);
        auto& s = k->slider;
        s.setRange(lo, hi, step);
        if (mid > lo && mid < hi) s.setSkewFactorFromMidPoint(mid);
        s.setTextValueSuffix(suffix);
        s.setNumDecimalPlacesToDisplay(step < 1 ? 1 : 0);
        s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 78, 20);
        s.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        s.setValue(value, juce::dontSendNotification);
        k->set = std::move(set);
        auto* raw = k.get();
        s.onValueChange = [this, raw] { edit([&](model::TrackProperties& q) { raw->set(q, raw->slider.getValue()); }); };
        addAndMakeVisible(k->label);
        addAndMakeVisible(k->slider);
        knobs.push_back(std::move(k));
    }

    void refreshKnobs()
    {
        auto* t = app.project.track(id);
        if (!t) return;
        std::vector<double> v;
        if (kind == StripEffect::Compressor) {
            const auto& c = t->compressor;
            v = {c.thresholdDb, c.ratio, c.kneeDb, c.attackMs, c.releaseMs, c.makeupDb};
            detector.setSelectedId(c.rms ? 2 : 1, juce::dontSendNotification);
        } else {
            const auto& r = t->reverb;
            v = {r.mix * 100.0, r.size * 100.0, r.damping * 100.0, r.width * 100.0, r.preDelayMs};
        }
        for (size_t i = 0; i < knobs.size() && i < v.size(); ++i) knobs[i]->slider.setValue(v[i], juce::dontSendNotification);
    }

    // Live change without undo (the destructor records one step).
    void edit(const std::function<void(model::TrackProperties&)>& f)
    {
        auto* t = app.project.track(id);
        if (!t) return;
        auto p = model::TrackProperties::from(*t);
        f(p);
        p.applyTo(*t);
        app.modelChangedWithoutUndo();
    }

    void updateOnText() { onBtn.setButtonText(onBtn.getToggleState() ? "ON" : "OFF"); }

    void timerCallback() override
    {
        const float gr = app.levels(id).gainReduction;
        grShown = std::max(gr, grShown - 0.6f);
        repaint(grArea);
    }

    AppContext& app;
    model::TrackId id;
    StripEffect kind;
    std::optional<model::TrackProperties> before;
    juce::Label title;
    juce::TextButton onBtn, defaultsBtn{"Defaults"};
    juce::ComboBox detector;
    std::vector<std::unique_ptr<Knob>> knobs;
    juce::Rectangle<int> grArea;
    float grShown = 0;
};
} // namespace

void showEffectEditor(AppContext& app, model::TrackId id, StripEffect kind, juce::Component& target)
{
    if (!app.project.track(id)) return;
    juce::CallOutBox::launchAsynchronously(std::make_unique<EffectEditor>(app, id, kind), target.getScreenBounds(), nullptr);
}

} // namespace mc::gui
