#include "gui/PluginBrowser.h"
#include "gui/Theme.h"
#include "plugins/BasicSynth.h"

namespace mc::gui {

namespace {
// Runs the scan loop on a worker thread; the plugin loading itself is marshalled
// to the message thread by the host. The message thread must therefore never
// block waiting for this thread (ThreadWithProgressWindow does on cancel, which
// would deadlock), so cancellation only signals and completion is polled.
class ScanThread final : public juce::Thread, private juce::Timer {
public:
    ScanThread(AppContext& a, std::function<void()> d)
        : Thread("Plugin scan"), app(a), done(std::move(d)),
          window("Scanning plugins...", {}, juce::MessageBoxIconType::NoIcon)
    {
        window.addProgressBarComponent(progress);
        window.addButton("Cancel", 1);
        window.enterModalState(true, juce::ModalCallbackFunction::create([this](int) { signalThreadShouldExit(); }), false);
    }
    void launch()
    {
        startThread();
        startTimer(100);
    }
    void run() override
    {
        app.plugins->rescan([this](float f, const juce::String& item) {
            progress = f;
            { const juce::ScopedLock sl(lock); message = item; }
            return !threadShouldExit();
        });
    }
private:
    void timerCallback() override
    {
        { const juce::ScopedLock sl(lock); window.setMessage(message); }
        if (isThreadRunning()) return;
        stopTimer();
        if (window.isCurrentlyModal()) window.exitModalState(0);
        window.setVisible(false);
        if (done) done();
        delete this;
    }

    AppContext& app;
    std::function<void()> done;
    double progress = 0.0;
    juce::CriticalSection lock;
    juce::String message;
    juce::AlertWindow window;
};

class BrowserWindow final : public juce::DocumentWindow {
public:
    BrowserWindow(AppContext& app, PluginBrowser::Choose choose, bool effects)
        : DocumentWindow(effects ? "Effect plugins" : "Instrument plugins", theme::col::appBg, closeButton)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new PluginBrowser(app, [this, choose](const plugins::PluginInfo& i) {
            choose(i);
            closeButtonPressed();
        }, effects), false);
        setResizable(true, true);
        centreWithSize(720, 480);
        setVisible(true);
    }
    void closeButtonPressed() override
    {
        juce::Component::SafePointer<juce::Component> self(this);
        juce::MessageManager::callAsync([self] { delete self.getComponent(); });
    }
};

enum Col { Name = 1, Manufacturer, Format, Category, Show };
} // namespace

void PluginBrowser::rescanWithProgress(AppContext& app, std::function<void()> done)
{
    (new ScanThread(app, std::move(done)))->launch();
}

void PluginBrowser::show(AppContext& app, Choose onChoose, bool effects) { new BrowserWindow(app, std::move(onChoose), effects); }

PluginBrowser::PluginBrowser(AppContext& a, Choose c, bool fx) : app(a), onChoose(std::move(c)), effects(fx)
{
    formatFilter.addItem("All formats", 1);
    int id = 2;
    for (auto& f : app.plugins->allFormats()) formatFilter.addItem(f, id++);
    formatFilter.setSelectedId(1, juce::dontSendNotification);
    formatFilter.onChange = [this] { refilter(); };
    search.setTextToShowWhenEmpty("Search...", juce::Colours::grey);
    search.onTextChange = [this] { refilter(); };
    rescanBtn.onClick = [this] {
        juce::Component::SafePointer<PluginBrowser> self(this);
        rescanWithProgress(app, [self] { if (self) self->refilter(); });
    };
    chooseBtn.onClick = [this] {
        const int r = table.getSelectedRow();
        if (r >= 0 && r < (int)filtered.size() && onChoose) onChoose(filtered[(size_t)r]);
    };
    auto& h = table.getHeader();
    if (!effects) h.addColumn("Show", Show, 52, 52, 52, juce::TableHeaderComponent::notResizable | juce::TableHeaderComponent::sortable);
    h.addColumn("Name", Name, 240);
    h.addColumn("Manufacturer", Manufacturer, 160);
    h.addColumn("Format", Format, 90);
    h.addColumn("Category", Category, 140);
    h.setSortColumnId(Name, true);
    for (juce::Component* comp : std::initializer_list<juce::Component*>{&formatFilter, &search, &rescanBtn, &chooseBtn, &table, &status}) addAndMakeVisible(comp);
    if (!effects) {
        menuHint.setFont(theme::uiFont(12.5f));
        menuHint.setColour(juce::Label::textColourId, theme::col::textDim);
        tickListedBtn.setTooltip("Tick every plugin in the list below (use the search and format filter first)");
        tickListedBtn.onClick = [this] {
            auto keys = app.project.instrumentMenu;
            for (auto& p : filtered) if (canTick(p)) keys.insert(model::Project::pluginMenuKey(p.format, p.identifier));
            app.setInstrumentMenu(std::move(keys));
            refilter();
        };
        untickAllBtn.setTooltip("Remove all ticks: the instrument menus list every plugin again");
        untickAllBtn.onClick = [this] { app.setInstrumentMenu({}); refilter(); };
        tickedOnly.setTooltip("List only the ticked plugins");
        tickedOnly.onClick = [this] { refilter(); };
        for (juce::Component* comp : std::initializer_list<juce::Component*>{&menuHint, &tickListedBtn, &untickAllBtn, &tickedOnly}) addAndMakeVisible(comp);
    }
    status.setColour(juce::Label::textColourId, theme::col::textDim);
    status.setFont(theme::uiFont(12.5f));
    table.setRowHeight(26);
    table.setColour(juce::ListBox::outlineColourId, theme::col::border);
    table.setOutlineThickness(1);
    refilter();
    setSize(720, 480);
}

void PluginBrowser::paint(juce::Graphics& g) { g.fillAll(theme::col::panel); }

void PluginBrowser::resized()
{
    auto r = getLocalBounds().reduced(theme::gap);
    auto top = r.removeFromTop(28);
    formatFilter.setBounds(top.removeFromLeft(150));
    top.removeFromLeft(6);
    rescanBtn.setBounds(top.removeFromRight(120));
    top.removeFromRight(6);
    search.setBounds(top);
    if (!effects) {
        r.removeFromTop(6);
        auto tools = r.removeFromTop(26);
        untickAllBtn.setBounds(tools.removeFromRight(96));
        tools.removeFromRight(6);
        tickListedBtn.setBounds(tools.removeFromRight(110));
        tools.removeFromRight(6);
        tickedOnly.setBounds(tools.removeFromRight(100));
        tools.removeFromRight(6);
        menuHint.setBounds(tools);
    }
    auto bottom = r.removeFromBottom(30);
    chooseBtn.setBounds(bottom.removeFromRight(120).reduced(0, 2));
    status.setBounds(bottom);
    r.removeFromTop(6);
    table.setBounds(r);
}

void PluginBrowser::refilter()
{
    filtered.clear();
    const auto fmt = formatFilter.getSelectedId() > 1 ? formatFilter.getText() : juce::String();
    const auto q = search.getText().trim();
    for (auto& p : app.plugins->plugins()) {
        if (p.isInstrument == effects) continue;
        if (fmt.isNotEmpty() && fmt != juce::String(p.format)) continue;
        if (q.isNotEmpty() && !juce::String(p.name).containsIgnoreCase(q) && !juce::String(p.manufacturer).containsIgnoreCase(q)) continue;
        if (!effects && tickedOnly.getToggleState() && !(canTick(p) && isTicked(p))) continue;
        filtered.push_back(p);
    }
    auto key = [this](const plugins::PluginInfo& p) -> const std::string& {
        switch (sortCol) { case Manufacturer: return p.manufacturer; case Format: return p.format; case Category: return p.category; default: return p.name; }
    };
    std::stable_sort(filtered.begin(), filtered.end(), [&](auto& a, auto& b) {
        int c;
        if (sortCol == Show) c = (int)(canTick(b) && isTicked(b)) - (int)(canTick(a) && isTicked(a)); // ticked first
        else c = juce::String(key(a)).compareIgnoreCase(juce::String(key(b)));
        if (c == 0 && sortCol == Show) c = juce::String(a.name).compareIgnoreCase(juce::String(b.name));
        return sortForward ? c < 0 : c > 0;
    });
    table.updateContent();
    table.repaint();
    updateStatus();
}

bool PluginBrowser::canTick(const plugins::PluginInfo& p) const { return p.format != plugins::InternalPluginHost::kFormat; }

bool PluginBrowser::isTicked(const plugins::PluginInfo& p) const
{
    return app.project.instrumentMenu.count(model::Project::pluginMenuKey(p.format, p.identifier)) > 0;
}

void PluginBrowser::toggleTicks(const std::vector<int>& rows)
{
    // several selected rows: all get the opposite of the first one's state
    std::vector<const plugins::PluginInfo*> ps;
    for (int r : rows) if (r >= 0 && r < (int)filtered.size() && canTick(filtered[(size_t)r])) ps.push_back(&filtered[(size_t)r]);
    if (ps.empty()) return;
    const bool tick = !isTicked(*ps.front());
    auto keys = app.project.instrumentMenu;
    for (auto* p : ps) {
        const auto k = model::Project::pluginMenuKey(p->format, p->identifier);
        if (tick) keys.insert(k); else keys.erase(k);
    }
    app.setInstrumentMenu(std::move(keys));
    if (tickedOnly.getToggleState()) refilter(); // unticked rows leave the list
    else { table.repaint(); updateStatus(); }
}

void PluginBrowser::updateStatus()
{
    juce::String note;
    if (!app.plugins->hasCache()) note = "  (not scanned yet - click Rescan)";
    else if (effects && !app.plugins->effectsScanned()) note = "  (effects are found by a new scan - click Rescan)";
    status.setText(juce::String((int)filtered.size()) + (effects ? " effects" : " instruments") + note, juce::dontSendNotification);
    if (effects) return;
    int ticked = 0, tickable = 0;
    for (auto& p : app.plugins->plugins())
        if (p.isInstrument && canTick(p)) { ++tickable; ticked += isTicked(p) ? 1 : 0; }
    menuHint.setText(ticked == 0 ? "Tick the plugins the instrument menus should show in this project. None ticked: all " + juce::String(tickable) + " are shown."
                                 : juce::String(ticked) + " of " + juce::String(tickable) + " ticked: only these are shown in this project's instrument menus.",
                     juce::dontSendNotification);
    untickAllBtn.setEnabled(ticked > 0);
}

void PluginBrowser::paintRowBackground(juce::Graphics& g, int row, int, int, bool selected)
{
    g.fillAll(selected ? theme::col::accentSoft : (row % 2 ? theme::col::panel : theme::col::field));
}

void PluginBrowser::paintCell(juce::Graphics& g, int row, int col, int w, int h, bool)
{
    if (row >= (int)filtered.size()) return;
    auto& p = filtered[(size_t)row];
    if (col == Show) {
        const float sz = 16.0f;
        juce::Rectangle<float> box((float)w / 2 - sz / 2, (float)h / 2 - sz / 2, sz, sz);
        const bool can = canTick(p), on = can ? isTicked(p) : true;
        g.setColour(on ? theme::col::accent.withAlpha(can ? 1.0f : 0.35f) : theme::col::field);
        g.fillRoundedRectangle(box, 3.0f);
        g.setColour(on ? theme::col::accent.darker(0.3f).withAlpha(can ? 1.0f : 0.35f) : theme::col::textDim);
        g.drawRoundedRectangle(box, 3.0f, 1.2f);
        if (on) {
            juce::Path tick;
            tick.startNewSubPath(box.getX() + 3.5f, box.getCentreY());
            tick.lineTo(box.getX() + 6.8f, box.getBottom() - 4.0f);
            tick.lineTo(box.getRight() - 3.5f, box.getY() + 4.0f);
            g.setColour(juce::Colours::white.withAlpha(can ? 1.0f : 0.8f));
            g.strokePath(tick, juce::PathStrokeType(2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        return;
    }
    const std::string* s = &p.name;
    if (col == Manufacturer) s = &p.manufacturer;
    else if (col == Format) s = &p.format;
    else if (col == Category) s = &p.category;
    g.setColour(col == Name ? theme::col::text : theme::col::textDim);
    g.setFont(theme::uiFont(13.5f, col == Name));
    g.drawText(juce::String(*s), 6, 0, w - 10, h, juce::Justification::centredLeft);
}

void PluginBrowser::cellClicked(int row, int col, const juce::MouseEvent&)
{
    if (col == Show) toggleTicks({row});
}

void PluginBrowser::cellDoubleClicked(int row, int col, const juce::MouseEvent&)
{
    if (col == Show) return; // fast clicks on the tick box only toggle it
    if (row >= 0 && row < (int)filtered.size() && onChoose) onChoose(filtered[(size_t)row]);
}

void PluginBrowser::returnKeyPressed(int row)
{
    if (row >= 0 && row < (int)filtered.size() && onChoose) onChoose(filtered[(size_t)row]);
}

bool PluginBrowser::keyPressed(const juce::KeyPress& k)
{
    if (effects || k != juce::KeyPress::spaceKey) return false;
    std::vector<int> rows;
    const auto sel = table.getSelectedRows();
    for (int i = 0; i < sel.size(); ++i) rows.push_back(sel[i]);
    toggleTicks(rows);
    return true;
}

juce::String PluginBrowser::getCellTooltip(int row, int col)
{
    if (col != Show || row < 0 || row >= (int)filtered.size()) return {};
    return canTick(filtered[(size_t)row]) ? "Ticked: shown in this project's instrument menus (space ticks the selected plugins)"
                                          : "Built-in instruments are always shown";
}

void PluginBrowser::sortOrderChanged(int col, bool fwd)
{
    sortCol = col;
    sortForward = fwd;
    refilter();
}

} // namespace mc::gui
