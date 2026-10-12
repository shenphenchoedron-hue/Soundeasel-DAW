#include "gui/EqEditor.h"
#include "dsp/Equalizer.h"
#include "gui/Theme.h"
#include <optional>

namespace mc::gui {

namespace {
namespace tc = theme::col;
using Band = model::EqBand;
using Type = model::EqBand::Type;

constexpr double kMinF = 20.0, kMaxF = 20000.0, kRangeDb = 24.0;
const juce::Colour kBandColours[model::EqSettings::kMaxBands] = {
    juce::Colour(0xffe5a742), juce::Colour(0xff35a9c8), juce::Colour(0xffd46a6a), juce::Colour(0xff5aae84),
    juce::Colour(0xff8a7cc8), juce::Colour(0xffe08f4f), juce::Colour(0xffc77a98), juce::Colour(0xff9aac5a)};

bool hasGain(Type t) { return t == Type::Bell || t == Type::LowShelf || t == Type::HighShelf; }
const char* typeName(Type t)
{
    switch (t) {
        case Type::Bell: return "Bell";
        case Type::LowShelf: return "Low shelf";
        case Type::HighShelf: return "High shelf";
        case Type::LowCut: return "Low cut";
        case Type::HighCut: return "High cut";
    }
    return "";
}
juce::String freqText(double f) { return f >= 1000.0 ? juce::String(f / 1000.0, f >= 10000.0 ? 1 : 2) + " kHz" : juce::String(juce::roundToInt(f)) + " Hz"; }

class EqEditor;

// ------------------------------------------------------------ graph
class Graph final : public juce::Component {
public:
    explicit Graph(EqEditor& e) : editor(e) { setRepaintsOnMouseActivity(false); }
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove(const juce::MouseEvent& e) override { setMouseCursor(bandAt(e.position) >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor); }

    float freqToX(double f) const { return (float)(std::log(f / kMinF) / std::log(kMaxF / kMinF)) * (float)getWidth(); }
    double xToFreq(float x) const { return kMinF * std::pow(kMaxF / kMinF, juce::jlimit(0.0, 1.0, (double)x / getWidth())); }
    float dbToY(double db) const { return (float)((0.5 - db / (2 * kRangeDb)) * getHeight()); }
    double yToDb(float y) const { return juce::jlimit(-kRangeDb, kRangeDb, (0.5 - (double)y / getHeight()) * 2 * kRangeDb); }
    juce::Point<float> handlePos(const Band& b) const { return {freqToX(b.freq), hasGain(b.type) ? dbToY(b.gainDb) : dbToY(0)}; }
    int bandAt(juce::Point<float>) const;

private:
    EqEditor& editor;
};

// ------------------------------------------------------------ editor
class EqEditor final : public juce::Component {
public:
    EqEditor(AppContext& a, model::TrackId i) : app(a), id(i), graph(*this)
    {
        if (auto* t = app.project.track(id)) before = model::TrackProperties::from(*t);
        title.setText("Parametric EQ  -  " + juce::String(before ? before->name : std::string()), juce::dontSendNotification);
        title.setFont(theme::uiFont(15.0f, true));
        onBtn.setClickingTogglesState(true);
        theme::setStyle(onBtn, theme::stylePlay);
        onBtn.setToggleState(before && before->eq.enabled, juce::dontSendNotification);
        onBtn.onClick = [this] { edit([&](model::EqSettings& e) { e.enabled = onBtn.getToggleState(); }); updateOnText(); };
        addBtn.setTooltip("Add a band (or double-click in the graph)");
        addBtn.onClick = [this] { addBand(1000.0, 0.0); };
        hint.setText("Double-click in the graph to add a band. Drag a point: frequency and gain. Mouse wheel on a point: width (Q). "
                     "Right-click a point for type / delete.", juce::dontSendNotification);
        hint.setFont(theme::uiFont(12.0f));
        hint.setColour(juce::Label::textColourId, tc::textDim);
        hint.setJustificationType(juce::Justification::topLeft);

        for (auto t : {Type::Bell, Type::LowShelf, Type::HighShelf, Type::LowCut, Type::HighCut}) typeBox.addItem(typeName(t), (int)t + 1);
        typeBox.onChange = [this] { editBand([&](Band& b) { b.type = (Type)(typeBox.getSelectedId() - 1); }); refreshBandControls(); };
        auto knob = [this](juce::Slider& s, juce::Label& l, const char* name, double lo, double hi, double step, double mid, const char* suffix) {
            l.setText(name, juce::dontSendNotification);
            l.setJustificationType(juce::Justification::centred);
            l.setFont(theme::uiFont(12.5f, true));
            l.setColour(juce::Label::textColourId, tc::textDim);
            s.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 78, 20);
            s.setRange(lo, hi, step);
            if (mid > lo && mid < hi) s.setSkewFactorFromMidPoint(mid);
            s.setTextValueSuffix(suffix);
            s.setRotaryParameters(juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
            addAndMakeVisible(s);
            addAndMakeVisible(l);
        };
        knob(freq, freqLabel, "Frequency", kMinF, kMaxF, 1, 1000, " Hz");
        knob(gain, gainLabel, "Gain", -kRangeDb, kRangeDb, 0.1, 0, " dB");
        knob(q, qLabel, "Q (width)", 0.1, 18, 0.01, 1.5, "");
        gain.setDoubleClickReturnValue(true, 0.0);
        freq.onValueChange = [this] { editBand([&](Band& b) { b.freq = (float)freq.getValue(); }); };
        gain.onValueChange = [this] { editBand([&](Band& b) { b.gainDb = (float)gain.getValue(); }); };
        q.onValueChange = [this] { editBand([&](Band& b) { b.q = (float)q.getValue(); }); };
        bandOn.setClickingTogglesState(true);
        bandOn.setTooltip("Switch this band on/off");
        bandOn.onClick = [this] { editBand([&](Band& b) { b.enabled = bandOn.getToggleState(); }); };
        delBtn.setTooltip("Delete this band");
        delBtn.onClick = [this] { deleteBand(selected); };
        bandTitle.setFont(theme::uiFont(14.0f, true));

        for (juce::Component* c : std::initializer_list<juce::Component*>{&title, &onBtn, &graph, &addBtn, &hint, &bandTitle, &typeBox, &bandOn, &delBtn})
            addAndMakeVisible(c);
        updateOnText();
        if (!eq().bands.empty()) selected = 0;
        refreshBandControls();
        setSize(660, 470);
    }

    ~EqEditor() override
    {
        auto* t = app.project.track(id);
        if (!t || !before) return;
        auto after = model::TrackProperties::from(*t);
        if (after.eq == before->eq) return;
        before->applyTo(*t);
        app.undo.perform(std::make_unique<model::SetTrackPropertiesCommand>(id, *before, after));
    }

    void paint(juce::Graphics& g) override { g.fillAll(tc::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(12);
        auto top = r.removeFromTop(28);
        onBtn.setBounds(top.removeFromRight(64));
        top.removeFromRight(8);
        addBtn.setBounds(top.removeFromRight(100));
        title.setBounds(top);
        r.removeFromTop(8);
        graph.setBounds(r.removeFromTop(220));
        r.removeFromTop(4);
        hint.setBounds(r.removeFromTop(32));
        r.removeFromTop(6);
        auto row = r.removeFromTop(26);
        bandTitle.setBounds(row.removeFromLeft(80));
        typeBox.setBounds(row.removeFromLeft(130));
        row.removeFromLeft(8);
        bandOn.setBounds(row.removeFromLeft(60));
        row.removeFromLeft(8);
        delBtn.setBounds(row.removeFromLeft(80));
        r.removeFromTop(6);
        auto knobs = r;
        const int w = 110;
        for (auto [s, l] : {std::pair{&freq, &freqLabel}, std::pair{&gain, &gainLabel}, std::pair{&q, &qLabel}}) {
            auto cell = knobs.removeFromLeft(w).reduced(4, 0);
            l->setBounds(cell.removeFromTop(16));
            s->setBounds(cell);
        }
    }

    // ---- used by the graph
    const model::EqSettings& eq() const
    {
        static const model::EqSettings empty;
        auto* t = app.project.track(id);
        return t ? t->eq : empty;
    }
    double sampleRate() const { return app.engine->sampleRate(); }
    int selected = -1;

    void select(int i) { selected = i; refreshBandControls(); graph.repaint(); }

    void addBand(double f, double db)
    {
        if ((int)eq().bands.size() >= model::EqSettings::kMaxBands) return;
        Band b;
        b.type = f < 60 ? Type::LowCut : f > 12000 ? Type::HighCut : Type::Bell;
        b.freq = (float)f;
        b.gainDb = hasGain(b.type) ? (float)juce::jlimit(-kRangeDb, kRangeDb, db) : 0.0f;
        b.q = b.type == Type::Bell ? 1.0f : 0.707f;
        edit([&](model::EqSettings& e) { e.bands.push_back(b); e.enabled = true; });
        onBtn.setToggleState(true, juce::dontSendNotification);
        updateOnText();
        select((int)eq().bands.size() - 1);
    }

    void deleteBand(int i)
    {
        if (i < 0 || i >= (int)eq().bands.size()) return;
        edit([&](model::EqSettings& e) { e.bands.erase(e.bands.begin() + i); });
        select(eq().bands.empty() ? -1 : std::min(i, (int)eq().bands.size() - 1));
    }

    void editBand(const std::function<void(Band&)>& f, int index = -2)
    {
        const int i = index == -2 ? selected : index;
        if (i < 0 || i >= (int)eq().bands.size()) return;
        edit([&](model::EqSettings& e) { f(e.bands[(size_t)i]); });
        graph.repaint();
    }

    void refreshBandControls()
    {
        const bool has = selected >= 0 && selected < (int)eq().bands.size();
        for (juce::Component* c : std::initializer_list<juce::Component*>{&typeBox, &bandOn, &delBtn, &freq, &gain, &q, &freqLabel, &gainLabel, &qLabel, &bandTitle})
            c->setEnabled(has);
        addBtn.setEnabled((int)eq().bands.size() < model::EqSettings::kMaxBands);
        if (!has) { bandTitle.setText("No band", juce::dontSendNotification); return; }
        const auto& b = eq().bands[(size_t)selected];
        bandTitle.setText("Band " + juce::String(selected + 1), juce::dontSendNotification);
        bandTitle.setColour(juce::Label::textColourId, kBandColours[selected % model::EqSettings::kMaxBands].darker(0.2f));
        typeBox.setSelectedId((int)b.type + 1, juce::dontSendNotification);
        bandOn.setToggleState(b.enabled, juce::dontSendNotification);
        bandOn.setButtonText(b.enabled ? "On" : "Off");
        freq.setValue(b.freq, juce::dontSendNotification);
        gain.setValue(b.gainDb, juce::dontSendNotification);
        gain.setEnabled(hasGain(b.type));
        gainLabel.setEnabled(hasGain(b.type));
        q.setValue(b.q, juce::dontSendNotification);
    }

private:
    // Live change without undo (the destructor records one step).
    void edit(const std::function<void(model::EqSettings&)>& f)
    {
        auto* t = app.project.track(id);
        if (!t) return;
        f(t->eq);
        app.modelChangedWithoutUndo();
        graph.repaint();
    }
    void updateOnText() { onBtn.setButtonText(onBtn.getToggleState() ? "ON" : "OFF"); }

    AppContext& app;
    model::TrackId id;
    std::optional<model::TrackProperties> before;
    juce::Label title, hint, bandTitle, freqLabel, gainLabel, qLabel;
    juce::TextButton onBtn, addBtn{"+ Add band"}, bandOn{"On"}, delBtn{"Delete"};
    juce::ComboBox typeBox;
    juce::Slider freq, gain, q;
    Graph graph;
};

// ------------------------------------------------------------ graph implementation
int Graph::bandAt(juce::Point<float> p) const
{
    const auto& bands = editor.eq().bands;
    for (int i = (int)bands.size() - 1; i >= 0; --i)
        if (p.getDistanceFrom(handlePos(bands[(size_t)i])) < 9.0f) return i;
    return -1;
}

void Graph::paint(juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour(tc::rollBg);
    g.fillRoundedRectangle(r, 4.0f);
    g.setFont(theme::uiFont(10.5f));
    // grid
    for (double f : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0}) {
        const float x = freqToX(f);
        g.setColour(tc::gridSub.brighter(0.3f));
        g.drawVerticalLine((int)x, 0.0f, r.getBottom());
        g.setColour(tc::gridBeat.brighter(0.6f));
        g.drawText(f >= 1000 ? juce::String((int)(f / 1000)) + "k" : juce::String((int)f), (int)x + 3, getHeight() - 14, 30, 12, juce::Justification::left);
    }
    for (double db : {-18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0}) {
        const float y = dbToY(db);
        g.setColour(db == 0.0 ? tc::gridBar.withAlpha(0.7f) : tc::gridSub.brighter(0.3f));
        g.drawHorizontalLine((int)y, 0.0f, r.getRight());
        g.setColour(tc::gridBeat.brighter(0.6f));
        g.drawText((db > 0 ? "+" : "") + juce::String((int)db), 3, (int)y - 12, 30, 12, juce::Justification::left);
    }
    const auto& eq = editor.eq();
    const double sr = editor.sampleRate();
    const int w = getWidth();
    // single band curves
    for (size_t i = 0; i < eq.bands.size(); ++i) {
        const auto& b = eq.bands[i];
        if (!b.enabled) continue;
        juce::Path p;
        for (int x = 0; x <= w; x += 2) {
            const float y = dbToY(dsp::Equalizer::bandResponseDb(b, xToFreq((float)x), sr));
            if (x == 0) p.startNewSubPath(0, y); else p.lineTo((float)x, y);
        }
        g.setColour(kBandColours[i % model::EqSettings::kMaxBands].withAlpha((int)i == editor.selected ? 0.75f : 0.35f));
        g.strokePath(p, juce::PathStrokeType(1.0f));
    }
    // total response
    juce::Path total;
    for (int x = 0; x <= w; ++x) {
        const float y = juce::jlimit(-50.0f, (float)getHeight() + 50, dbToY(dsp::Equalizer::responseDb(eq, xToFreq((float)x), sr)));
        if (x == 0) total.startNewSubPath(0, y); else total.lineTo((float)x, y);
    }
    juce::Path fill(total);
    fill.lineTo((float)w, dbToY(0));
    fill.lineTo(0, dbToY(0));
    fill.closeSubPath();
    const auto curveCol = eq.enabled ? juce::Colour(0xffffd54a) : juce::Colour(0xff8994a1);
    g.setColour(curveCol.withAlpha(0.18f));
    g.fillPath(fill);
    g.setColour(curveCol);
    g.strokePath(total, juce::PathStrokeType(2.0f));
    // handles
    g.setFont(theme::uiFont(11.0f, true));
    for (size_t i = 0; i < eq.bands.size(); ++i) {
        const auto& b = eq.bands[i];
        const auto p = handlePos(b);
        const auto c = kBandColours[i % model::EqSettings::kMaxBands];
        const bool sel = (int)i == editor.selected;
        const float rad = sel ? 9.0f : 7.5f;
        g.setColour(b.enabled ? c : c.withSaturation(0.1f).withAlpha(0.6f));
        g.fillEllipse(p.x - rad, p.y - rad, 2 * rad, 2 * rad);
        g.setColour(sel ? juce::Colours::white : juce::Colours::black.withAlpha(0.5f));
        g.drawEllipse(p.x - rad, p.y - rad, 2 * rad, 2 * rad, sel ? 2.0f : 1.0f);
        g.setColour(juce::Colours::black);
        g.drawText(juce::String((int)i + 1), juce::Rectangle<float>(p.x - rad, p.y - rad, 2 * rad, 2 * rad), juce::Justification::centred);
        if (sel) {
            juce::String info = juce::String(typeName(b.type)) + "  " + freqText(b.freq);
            if (hasGain(b.type)) info << "  " << (b.gainDb > 0 ? "+" : "") << juce::String(b.gainDb, 1) << " dB";
            info << "  Q " << juce::String(b.q, 2);
            g.setColour(tc::display.withAlpha(0.85f));
            const float tx = juce::jlimit(4.0f, (float)getWidth() - 214.0f, p.x + 12);
            const float ty = juce::jlimit(4.0f, (float)getHeight() - 22.0f, p.y - 26);
            g.fillRoundedRectangle(tx, ty, 210, 18, 3.0f);
            g.setColour(tc::displayText);
            g.drawText(info, juce::Rectangle<float>(tx, ty, 210, 18), juce::Justification::centred);
        }
    }
    if (eq.bands.empty()) {
        g.setColour(juce::Colour(0xffc9d1da));
        g.setFont(theme::uiFont(14.0f));
        g.drawText("Double-click here to add a band", getLocalBounds(), juce::Justification::centred);
    }
}

void Graph::mouseDown(const juce::MouseEvent& e)
{
    const int hit = bandAt(e.position);
    editor.select(hit);
    if (hit < 0 || !e.mods.isPopupMenu()) return;
    juce::PopupMenu m;
    const auto& b = editor.eq().bands[(size_t)hit];
    m.addSectionHeader("Band " + juce::String(hit + 1));
    for (auto t : {Type::Bell, Type::LowShelf, Type::HighShelf, Type::LowCut, Type::HighCut})
        m.addItem(10 + (int)t, typeName(t), true, b.type == t);
    m.addSeparator();
    m.addItem(1, "On", true, b.enabled);
    m.addItem(2, "Delete band");
    juce::Component::SafePointer<Graph> self(this);
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(), [self, hit](int r) {
        if (!self || r == 0) return;
        auto& ed = self->editor;
        if (r == 2) ed.deleteBand(hit);
        else if (r == 1) ed.editBand([](Band& x) { x.enabled = !x.enabled; }, hit);
        else if (r >= 10) ed.editBand([r](Band& x) { x.type = (Type)(r - 10); }, hit);
        ed.refreshBandControls();
    });
}

void Graph::mouseDrag(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || editor.selected < 0) return;
    const double f = juce::jlimit(kMinF, kMaxF, xToFreq(e.position.x));
    const double db = yToDb(e.position.y);
    editor.editBand([&](Band& b) {
        b.freq = (float)f;
        if (hasGain(b.type)) b.gainDb = (float)(std::round(db * 10.0) / 10.0);
    });
    editor.refreshBandControls();
}

void Graph::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (bandAt(e.position) >= 0) return;
    editor.addBand(xToFreq(e.position.x), yToDb(e.position.y));
}

void Graph::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int hit = bandAt(e.position);
    const int i = hit >= 0 ? hit : editor.selected;
    if (i < 0) return;
    editor.select(i);
    const float factor = w.deltaY > 0 ? 1.12f : 1 / 1.12f;
    editor.editBand([factor](Band& b) { b.q = juce::jlimit(0.1f, 18.0f, b.q * factor); });
    editor.refreshBandControls();
}
} // namespace

void showEqEditor(AppContext& app, model::TrackId id, juce::Component& target)
{
    if (!app.project.track(id)) return;
    juce::CallOutBox::launchAsynchronously(std::make_unique<EqEditor>(app, id), target.getScreenBounds(), nullptr);
}

} // namespace mc::gui
