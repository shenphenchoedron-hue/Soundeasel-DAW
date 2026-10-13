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
    bool failed = false, bypassed = false, grip = true;
    void paintButton(juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced(0.5f);
        g.setColour(down ? tc::fieldHover.darker(0.05f) : over ? tc::fieldHover : tc::field);
        g.fillRoundedRectangle(r, 3.0f);
        g.setColour(failed ? tc::record : over ? tc::accent : tc::border);
        g.drawRoundedRectangle(r, 3.0f, 1.0f);
        auto textArea = getLocalBounds().reduced(4, 0);
        if (grip && (over || down)) { // grip: the slot can be dragged to another place in the chain
            g.setColour(tc::textDim.withAlpha(0.8f));
            const float cy = r.getCentreY();
            for (int col = 0; col < 2; ++col)
                for (int row = -1; row <= 1; ++row)
                    g.fillEllipse(3.0f + (float)col * 3.0f, cy + (float)row * 3.5f - 0.75f, 1.5f, 1.5f);
            textArea.removeFromLeft(6);
        }
        g.setColour(failed ? tc::record : bypassed ? tc::textDim.withAlpha(0.6f) : tc::text);
        g.setFont(theme::uiFont(11.0f, true));
        g.drawFittedText(getButtonText(), textArea, juce::Justification::centredLeft, 1, 0.8f);
    }
};

class InsertsComponent::Row final : public juce::Component {
public:
    Row(InsertsComponent& o, AppContext& a, model::TrackId t, int i) : owner(o), app(a), id(t), index(i)
    {
        addAndMakeVisible(nameBtn);
        addAndMakeVisible(powerBtn);
        nameBtn.onClick = [this] {
            if (popupClick || dragged) return; // right-click: the menu is shown by mouseDown; drag: reordered in mouseUp
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
                                            : juce::String(r.name) + " (" + juce::String(r.format) + ")\nClick to open its window, drag up/down to change the order, right-click for more.");
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

    // drag the name up/down to move the plugin in the chain (processing order = top to bottom)
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (popupClick || e.mods.isPopupMenu()) return;
        if (!dragged) {
            auto* t = app.project.track(id);
            if (!t || t->inserts.size() < 2 || e.getDistanceFromDragStart() < 4) return;
            dragged = true;
            nameBtn.setMouseCursor(juce::MouseCursor::DraggingHandCursor);
            juce::Component::beginDragAutoRepeat(40); // keeps scrolling while the mouse rests at the list's edge
        }
        owner.dragMove(index, e.getEventRelativeTo(&owner.list).getPosition());
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (!dragged) return;
        juce::Component::beginDragAutoRepeat(0);
        nameBtn.setMouseCursor(juce::MouseCursor::NormalCursor);
        owner.dragEnd(index);
        dragged = false; // the button's onClick (skipped for this drag) has already run before the listener's mouseUp
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragged = false;
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
        m.addItem(8, "Move to top (first)", index > 0);
        m.addItem(9, "Move to bottom (last)", index + 1 < n);
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
            else if (r == 8) ctx.moveInsert(tid, i, 0);
            else if (r == 9) { if (auto* tr = ctx.project.track(tid)) ctx.moveInsert(tid, i, (int)tr->inserts.size() - 1); }
        });
    }

private:
    InsertsComponent& owner;
    AppContext& app;
    model::TrackId id;
    int index;
    bool popupClick = false, dragged = false;
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
    view.onScroll = [this] { repaint(); }; // the "more" badges depend on the scroll position
    addBtn.setTooltip("Add an effect plugin (VST3, AU, LV2, CLAP) to this channel strip");
    addBtn.onClick = [this] { showEffectMenu(app, id, addBtn); };
    addAndMakeVisible(foldBtn);
    foldBtn.setTooltip("Fold the FX plugin lists (all strips)");
    foldBtn.onClick = [this] { if (onToggleCollapsed) onToggleCollapsed(); };
    auto summary = std::make_unique<SlotButton>();
    summary->grip = false;
    summaryBtn = std::move(summary);
    addChildComponent(*summaryBtn);
    summaryBtn->onClick = [this] { if (onToggleCollapsed) onToggleCollapsed(); };
    refresh();
}

InsertsComponent::~InsertsComponent() = default;

void InsertsComponent::refresh()
{
    auto* t = app.project.track(id);
    const int n = t ? (int)t->inserts.size() : 0;
    if ((int)rows.size() != n) {
        const bool added = n > (int)rows.size();
        dragFrom = dropGap = -1; // the chain changed under a drag: drop it
        rows.clear();
        for (int i = 0; i < n; ++i) { rows.push_back(std::make_unique<Row>(*this, app, id, i)); list.addAndMakeVisible(*rows.back()); }
        resized();
        if (added) view.setViewPosition(0, list.getHeight()); // a new plugin goes last: keep it in sight
        repaint();
    }
    for (auto& r : rows) r->refresh();
    addBtn.setButtonText(n == 0 ? "+ FX plugin" : "+ FX");
    refreshSummary();
}

void InsertsComponent::setCollapsed(bool c)
{
    if (c == collapsed) return;
    collapsed = c;
    view.setVisible(!c);
    addBtn.setVisible(!c);
    foldBtn.setVisible(!c);
    summaryBtn->setVisible(c);
    refreshSummary();
    resized();
    repaint();
}

// The folded line: number of plugins (and how many are bypassed or failed); the tooltip lists the chain.
void InsertsComponent::refreshSummary()
{
    auto* t = app.project.track(id);
    auto* b = static_cast<SlotButton*>(summaryBtn.get());
    const int n = t ? (int)t->inserts.size() : 0;
    int off = 0, failed = 0;
    juce::String tip = n == 0 ? juce::String("No FX plugins on this strip.") : juce::String("FX plugins (top to bottom):");
    for (int i = 0; i < n; ++i) {
        const auto& r = t->inserts[(size_t)i];
        const bool err = app.insertError(id, i).isNotEmpty();
        off += r.bypassed ? 1 : 0;
        failed += err ? 1 : 0;
        tip << "\n" << juce::String(i + 1) << ". " << juce::String(r.name) << (err ? "  (not loaded)" : r.bypassed ? "  (off)" : "");
    }
    tip << "\nClick to unfold the FX lists.";
    juce::String text = juce::String::fromUTF8("\xe2\x96\xb8 "); // ▸
    if (n == 0) text << "No FX";
    else {
        text << juce::String(n) << " FX";
        if (failed) text << " (" << juce::String(failed) << " missing)";
        else if (off) text << " (" << juce::String(off) << " off)";
    }
    b->setButtonText(text);
    b->setTooltip(tip);
    b->failed = failed > 0;
    b->bypassed = n == 0 || off == n;
    b->repaint();
}

void InsertsComponent::resized()
{
    auto r = getLocalBounds();
    summaryBtn->setBounds(r.withHeight(std::min(r.getHeight(), kRowHeight)));
    auto bottom = r.removeFromBottom(kRowHeight);
    foldBtn.setBounds(bottom.removeFromRight(18));
    bottom.removeFromRight(2);
    addBtn.setBounds(bottom);
    r.removeFromBottom(2);
    view.setBounds(r);
    const int w = r.getWidth() - ((int)rows.size() > kVisibleRows ? view.getScrollBarThickness() + 1 : 0);
    list.setSize(std::max(1, w), std::max(1, (int)rows.size() * (kRowHeight + 2)));
    for (size_t i = 0; i < rows.size(); ++i) rows[i]->setBounds(0, (int)i * (kRowHeight + 2), w, kRowHeight);
}

// ------------------------------------------------------------ drag to reorder
int InsertsComponent::gapAt(int yInList) const
{
    constexpr int pitch = kRowHeight + 2;
    const int g = (int)std::floor(((double)yInList + pitch * 0.5) / pitch);
    return juce::jlimit(0, (int)rows.size(), g);
}

void InsertsComponent::dragMove(int from, juce::Point<int> posInList)
{
    if (from < 0 || from >= (int)rows.size()) return;
    if (dragFrom != from) {
        dragFrom = from;
        rows[(size_t)from]->setAlpha(0.45f);
    }
    const auto inView = view.getLocalPoint(&list, posInList);
    view.autoScroll(inView.x, inView.y, kRowHeight / 2, 6); // more plugins than visible rows: scroll at the edges
    const int g = gapAt(list.getLocalPoint(&view, inView).y); // the list may just have scrolled under the mouse
    if (g != dropGap) { dropGap = g; repaint(); }
}

void InsertsComponent::dragEnd(int from)
{
    const int g = dropGap;
    if (from >= 0 && from < (int)rows.size()) rows[(size_t)from]->setAlpha(1.0f);
    dragFrom = dropGap = -1;
    repaint();
    if (g < 0 || from < 0) return;
    const int to = g > from ? g - 1 : g; // the gap index counts the dragged row itself
    if (to == from) return;
    // after the mouse event has finished (the move rebuilds the engine's chain and refreshes the mixer)
    juce::MessageManager::callAsync([&ctx = app, tid = id, from, to] { ctx.moveInsert(tid, from, to); });
}

void InsertsComponent::paintOverChildren(juce::Graphics& g)
{
    if (collapsed) return;
    // Only when the strip is too short for all plugins: say how many are scrolled out of sight.
    if (!rows.empty()) {
        constexpr int pitch = kRowHeight + 2;
        const auto vb = view.getBounds();
        const int top = view.getViewPositionY(), bottom = top + view.getMaximumVisibleHeight();
        int above = 0, below = 0;
        for (int i = 0; i < (int)rows.size(); ++i) {
            if (i * pitch + kRowHeight / 2 < top) ++above;
            else if (i * pitch + kRowHeight / 2 > bottom) ++below;
        }
        auto badge = [&](int n, bool atTop) {
            if (n == 0) return;
            const juce::String text = juce::String::fromUTF8(atTop ? "\xe2\x96\xb2 " : "\xe2\x96\xbc ") + juce::String(n) + " more";
            g.setFont(theme::uiFont(10.0f, true));
            const int w = juce::GlyphArrangement::getStringWidthInt(g.getCurrentFont(), text) + 8;
            const auto r = juce::Rectangle<int>(vb.getRight() - view.getScrollBarThickness() - 1 - w, atTop ? vb.getY() : vb.getBottom() - 13, w, 13);
            g.setColour(tc::accent);
            g.fillRoundedRectangle(r.toFloat(), 3.0f);
            g.setColour(juce::Colours::black);
            g.drawText(text, r, juce::Justification::centred);
        };
        badge(above, true);
        badge(below, false);
    }
    if (dragFrom < 0 || dropGap < 0 || dropGap == dragFrom || dropGap == dragFrom + 1) return; // no move
    const int yList = dropGap * (kRowHeight + 2) - 1;
    const int y = getLocalPoint(&list, juce::Point<int>(0, yList)).y;
    const auto vb = view.getBounds();
    const int yc = juce::jlimit(vb.getY() + 1, vb.getBottom() - 1, y);
    g.setColour(tc::accent);
    g.fillRect(vb.getX(), yc - 1, vb.getWidth(), 2);
    juce::Path tri;
    tri.addTriangle((float)vb.getX(), (float)yc - 4.0f, (float)vb.getX() + 5.0f, (float)yc, (float)vb.getX(), (float)yc + 4.0f);
    g.fillPath(tri);
}

void InsertsComponent::paint(juce::Graphics& g)
{
    if (!rows.empty() || collapsed) return;
    g.setColour(tc::textDim.withAlpha(0.7f));
    g.setFont(theme::uiFont(10.5f));
    g.drawText("No FX plugins", view.getBounds(), juce::Justification::centred);
}

} // namespace mc::gui
