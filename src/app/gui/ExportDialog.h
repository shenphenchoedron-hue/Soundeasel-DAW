#pragma once
// File > Export song: pick the format (WAV, MP3, OGG, FLAC, MIDI, M4A) and its settings,
// then the file. Audio formats are rendered offline on a worker thread behind a progress
// window, so the app stays responsive and the export can be cancelled.
#include "AppContext.h"

namespace mc::gui {

class ExportDialog final : public juce::Component {
public:
    static void show(AppContext&);
    // Message thread. Exports must be stopped before the AppContext (plugin hosts) goes away.
    static bool isExportRunning();
    static void cancelRunningExports(); // discards unfinished files; blocks until the workers have stopped

    explicit ExportDialog(AppContext&);
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void formatChanged();
    void updateNote();
    audio::ExportOptions currentOptions() const;
    void remember(const audio::ExportOptions&);
    void exportClicked();

    AppContext& app;
    juce::Label formatLabel{{}, "Format"}, rateLabel{{}, "Sample rate"}, qualityLabel{{}, "Quality"}, rangeLabel{{}, "Range"}, note;
    juce::ComboBox format, rate, quality, range;
    juce::ToggleButton tail{"Let reverb and released notes ring out at the end"};
    juce::ToggleButton normalize{"Normalize (loudest peak at -1 dB)"};
    juce::TextButton exportBtn{"Export..."}, cancelBtn{"Cancel"};
};

} // namespace mc::gui
