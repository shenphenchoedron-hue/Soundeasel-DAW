#include "gui/InsertsComponent.h"
#include "gui/PluginBrowser.h"
#include "gui/Theme.h"

namespace mc::gui {

namespace {
namespace tc = theme::col;

void choose(AppContext& app, model::TrackId id, int replaceIndex, const plugins::PluginInfo& info)
{
    if (replaceIndex >= 0) app.replaceInsert(id, replaceIndex, info);
    else app.addInsert(id, info);
}
} // namespace

void showEffectMenu(AppContext& app, model::TrackId id, juce::Component& target, int replaceIndex)
{
    juce::PopupMenu m;
    const auto& list = app.plugins->plugins();
    std::map<std::string, std::map<std::string, juce::PopupMenu>> byFormat; // format -> manufacturer -> items
    std::map<std::string, int> counts;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& p = list[i];
        if (p.isInstrument) continue;
        byFormat[p.format][p.manufacturer.empty() ? "Other" : p.manufacturer].addItem((int)i + 1000, juce::String(p.name));
        ++counts[p.format];
    }
    if (!app.plugins->effectsScanned()) {
        m.addItem(2, "Scan for effect plugins (needed once)...");
        m.addSeparator();
    }
    for (auto& [fmt, makers] : byFormat) {
        juce::PopupMenu sub;
        if (counts[fmt] <= 14) { // few plugins: one flat list
            for (auto& [maker, items] : makers)
                for (juce::PopupMenu::MenuItemIterator it(items); it.next();) {
                    auto item = it.getItem();
                    item.text << "  (" << juce::String(maker) << ")";
                    sub.addItem(item);
                }
        } else {
            for (auto& [maker, items] : makers) sub.addSubMenu(juce::String(maker), items);
        }
        m.addSubMenu(juce::String(fmt == "AudioUnit" ? "AU" : fmt) + "  (" + juce::String(counts[fmt]) + ")", sub);
    }
    if (byFormat.empty() && app.plugins->effectsScanned()) m.addItem(-1, "No effect plugins found", false);
    m.addSeparator();
    m.addItem(1, "Browse effect plugins...");
    m.addItem(3, "Rescan plugins...");
    m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&target), [&ctx = app, id, replaceIndex](int r) {
        if (r >= 1000) {
            auto& l = ctx.plugins->plugins();
            if ((size_t)(r - 1000) < l.size()) choose(ctx, id, replaceIndex, l[(size_t)(r - 1000)]);
        } else if (r == 1) {
            PluginBrowser::show(ctx, [&ctx, id, replaceIndex](const plugins::PluginInfo& i) { choose(ctx, id, replaceIndex, i); }, true);
        } else if (r == 2 || r == 3) {
            PluginBrowser::rescanWithProgress(ctx, [] {});
        }
    });
}

// ------------------------------------------------------------ one slot
// Compact, left-aligned plugin name (the strip is narrow).
struct SlotButton final : juce::Button {
    SlotButton() : juce::Button("slot") {}
    bool failed = false, bypassed = false;
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(down ? tc::fieldHover.darker(0.05f) : over ? tc::fieldHover : tc::field);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(failed ? tc::record : over ? tc::accent : tc::border);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        g.setColour(failed ? tc::record : bypassed ? tc::textDim.withAlpha(0.6f) : tc::text);
        g.setFont(theme::uiFont(11.0f, true));
        g.drawFittedText(getButtonText(), getLocalBounds().reduced(4, 0), juce::Justification::centredLeft, 1, 0.8f);
    }
};

class InsertsComponent::Row final : public juce::Component {
public:
    Row(AppContext& a, model::TrackId t, int i) : app(a), id(t), index(i)
    {
        addAndMakeVisible(nameBtn);
        addAndMakeVisible(powerBtn);
        nameBtn.onClick = [this] {
            if (popupClick) return; // right-click: the menu is shown by mouseDown
            if (app.insertError(id, index).isNotEmpty()) showEffectMenu(app, id, nameBtn, index); // missing: offer a replacement
            else app.openInsertEditor(id, index);
        };
        powerBtn.setClickingTogglesState(true);
        theme::setStyle(powerBtn, theme::stylePlay);
        powerBtn.setTooltip("On / bypass");
        powerBtn.onClick = [this] { app.setInsertBypass(id, index, !powerBtn.getToggleState()); };
        nameBtn.addMouseListener(this, false);
    }

    void refresh()
    {
        auto* t = app.project.track(id);
        if (!t || index >= (int)t->inserts.size()) return;
        const auto& r = t->inserts[(size_t)index];
        const auto err = app.insertError(id, index);
        // the vendor's product prefix ("ACE Reverb" -> "Reverb") is dropped when space is short
        juce::String label(r.name);
        if (const auto first = label.upToFirstOccurrenceOf(" ", false, false);
            label.length() > 12 && first.length() <= 4 && first == first.toUpperCase() && label.containsChar(' '))
            label = label.fromFirstOccurrenceOf(" ", false, false);
        nameBtn.setButtonText(label);
        nameBtn.setTooltip(err.isNotEmpty() ? "Not loaded: " + err + "\nClick to choose another plugin."
                                            : juce::String(r.name) + " (" + juce::String(r.format) + ")\nClick to open its window, right-click for more.");
        nameBtn.failed = err.isNotEmpty();
        nameBtn.bypassed = r.bypassed;
        nameBtn.repaint();
        powerBtn.setToggleState(!r.bypassed, juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        powerBtn.setBounds(r.removeFromRight(18));
        r.removeFromRight(2);
        nameBtn.setBounds(r);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        popupClick = e.mods.isPopupMenu();
        if (!popupClick) return;
        auto* t = app.project.track(id);
        if (!t || index >= (int)t->inserts.size()) return;
        const int n = (int)t->inserts.size();
        juce::PopupMenu m;
        m.addSectionHeader(juce::String(t->inserts[(size_t)index].name));
        m.addItem(1, "Open plugin window", app.insertError(id, index).isEmpty());
        m.addItem(2, "Bypass", true, t->inserts[(size_t)index].bypassed);
        m.addSeparator();
        m.addItem(3, "Move up", index > 0);
        m.addItem(4, "Move down", index + 1 < n);
        m.addItem(5, "Replace with...");
        m.addItem(6, "Remove");
        m.addSeparator();
        m.addItem(7, "Plugins after EQ / COMP / REVERB", true, t->insertsAfterBuiltins);
        juce::Component::SafePointer<Row> self(this);
        m.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&nameBtn), [self](int r) {
            if (!self || r == 0) return;
            auto& ctx = self->app;
            const auto tid = self->id;
            const int i = self->index;
            if (r == 1) ctx.openInsertEditor(tid, i);
            else if (r == 2) { if (auto* tr = ctx.project.track(tid); tr && i < (int)tr->inserts.size()) ctx.setInsertBypass(tid, i, !tr->inserts[(size_t)i].bypassed); }
            else if (r == 3) ctx.moveInsert(tid, i, i - 1);
            else if (r == 4) ctx.moveInsert(tid, i, i + 1);
            else if (r == 5) showEffectMenu(ctx, tid, self->nameBtn, i);
            else if (r == 6) ctx.removeInsert(tid, i);
            else if (r == 7) { if (auto* tr = ctx.project.track(tid)) ctx.setInsertsAfterBuiltins(tid, !tr->insertsAfterBuiltins); }
        });
    }

private:
    AppContext& app;
    model::TrackId id;
    int index;
    bool popupClick = false;
    SlotButton nameBtn;
    juce::TextButton powerBtn{juce::String::fromUTF8("\xe2\x8f\xbb")}; // ⏻
};

// ------------------------------------------------------------ list
InsertsComponent::InsertsComponent(AppContext& a, model::TrackId t) : app(a), id(t)
{
    addAndMakeVisible(view);
    addAndMakeVisible(addBtn);
    view.setViewedComponent(&list, false);
    view.setScrollBarsShown(true, false);
    view.setScrollBarThickness(6);
    addBtn.setTooltip("Add an effect plugin (VST3, AU, LV2, CLAP) to this channel strip");
    addBtn.onClick = [this] { showEffectMenu(app, id, addBtn); };
    refresh();
}

InsertsComponent::~InsertsComponent() = default;

void InsertsComponent::refresh()
{
    auto* t = app.project.track(id);
    const int n = t ? (int)t->inserts.size() : 0;
    if ((int)rows.size() != n) {
        rows.clear();
        for (int i = 0; i < n; ++i) { rows.push_back(std::make_unique<Row>(app, id, i)); list.addAndMakeVisible(*rows.back()); }
        resized();
    }
    for (auto& r : rows) r->refresh();
    addBtn.setButtonText(n == 0 ? "+ FX plugin" : "+ FX");
}

void InsertsComponent::resized()
{
    auto r = getLocalBounds();
    addBtn.setBounds(r.removeFromBottom(kRowHeight));
    r.removeFromBottom(2);
    view.setBounds(r);
    const int w = r.getWidth() - ((int)rows.size() > kVisibleRows ? view.getScrollBarThickness() + 1 : 0);
    list.setSize(std::max(1, w), std::max(1, (int)rows.size() * (kRowHeight + 2)));
    for (size_t i = 0; i < rows.size(); ++i) rows[i]->setBounds(0, (int)i * (kRowHeight + 2), w, kRowHeight);
}

void InsertsComponent::paint(juce::Graphics& g)
{
    if (!rows.empty()) return;
    g.setColour(tc::textDim.withAlpha(0.7f));
    g.setFont(theme::uiFont(10.5f));
    g.drawText("No FX plugins", view.getBounds(), juce::Justification::centred);
}

} // namespace mc::gui
