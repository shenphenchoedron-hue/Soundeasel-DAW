#pragma once
// Lists scanned instrument plugins with format filter and search; rescans on demand.
// Instruments: the Show column ticks which plugins this project's instrument menus list
// (none ticked = all), so the menus are not flooded by hundreds of plugins.
#include "AppContext.h"

namespace mc::gui {

class PluginBrowser final : public juce::Component, private juce::TableListBoxModel {
public:
    using Choose = std::function<void(const plugins::PluginInfo&)>;
    PluginBrowser(AppContext&, Choose onChoose, bool effects = false);
    void resized() override;
    void paint(juce::Graphics&) override;

    static void show(AppContext&, Choose onChoose, bool effects = false); // effects: list audio effect plugins
    static void rescanWithProgress(AppContext&, std::function<void()> done);

private:
    int getNumRows() override { return (int)filtered.size(); }
    void paintRowBackground(juce::Graphics&, int row, int w, int h, bool selected) override;
    void paintCell(juce::Graphics&, int row, int col, int w, int h, bool selected) override;
    void cellClicked(int row, int col, const juce::MouseEvent&) override;
    void cellDoubleClicked(int row, int col, const juce::MouseEvent&) override;
    void returnKeyPressed(int row) override;
    bool keyPressed(const juce::KeyPress&) override; // space ticks/unticks the selected plugins
    juce::String getCellTooltip(int row, int col) override;
    bool canTick(const plugins::PluginInfo&) const;  // built-in instruments are always shown
    bool isTicked(const plugins::PluginInfo&) const;
    void toggleTicks(const std::vector<int>& rows);
    void updateStatus();
    void sortOrderChanged(int col, bool forwards) override;
    void refilter();

    AppContext& app;
    Choose onChoose;
    juce::ComboBox formatFilter;
    juce::TextEditor search;
    juce::TextButton rescanBtn{"Rescan plugins"}, chooseBtn{"Use selected"};
    juce::Label menuHint;
    juce::TextButton tickListedBtn{"Tick all listed"}, untickAllBtn{"Untick all"};
    juce::ToggleButton tickedOnly{"Ticked only"};
    juce::TableListBox table{"plugins", this};
    juce::TooltipWindow tooltips{this}; // own window: the main window's tooltips do not reach it
    juce::Label status;
    std::vector<plugins::PluginInfo> filtered;
    int sortCol = 1;
    bool sortForward = true;
    bool effects = false;
};

} // namespace mc::gui
