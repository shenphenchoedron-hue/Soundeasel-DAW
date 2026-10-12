#include "gui/ExportDialog.h"
#include "gui/Theme.h"

namespace mc::gui {

using audio::ExportFormat;

namespace {
constexpr double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0};

std::unique_ptr<juce::FileChooser>& fileChooser()
{
    static std::unique_ptr<juce::FileChooser> c; // must outlive the async dialog
    return c;
}

juce::String songTitle(const AppContext& app)
{
    return app.currentFile != juce::File() ? app.currentFile.getFileNameWithoutExtension() : juce::String("Untitled");
}

juce::String formatTime(double seconds)
{
    const int s = (int)std::lround(seconds);
    return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
}

// ---------------------------------------------------------------- export job
// Renders on a worker thread behind a progress window. While the app runs, the message
// thread never waits for the worker; cancelling only raises a flag and completion is
// polled. The session (plugin instances) is destroyed on the message thread.
class ExportJob;
std::vector<ExportJob*>& runningJobs() // message thread
{
    static std::vector<ExportJob*> jobs;
    return jobs;
}

class ExportJob final : public juce::Thread, private juce::Timer {
public:
    ExportJob(std::unique_ptr<audio::RenderSession> s, audio::ExportOptions o, juce::File t, juce::File enc)
        : Thread("Song export"), session(std::move(s)), opts(std::move(o)), target(std::move(t)), encoder(std::move(enc)),
          window("Exporting \"" + target.getFileName() + "\"", "Preparing...", juce::MessageBoxIconType::NoIcon)
    {
        runningJobs().push_back(this);
        window.addProgressBarComponent(progress);
        window.addButton("Cancel", 1);
        // The callback may run after this object is gone: it only touches the shared flag.
        auto flag = cancelled;
        window.enterModalState(true, juce::ModalCallbackFunction::create([flag](int) { flag->store(true); }), false);
        startThread();
        startTimer(80);
    }

    ~ExportJob() override { std::erase(runningJobs(), this); }

    // App shutdown: stops the export (the unfinished file is discarded), waits for the
    // worker, then frees the plugin instances while the plugin hosts still exist.
    void abort()
    {
        cancelled->store(true);
        stopThread(15000);
        stopTimer();
        if (window.isCurrentlyModal()) window.exitModalState(0);
        window.setVisible(false);
        session.reset();
        delete this;
    }

    void run() override
    {
        result = audio::renderSong(*session, opts, target, encoder, [this](double f, const juce::String& stage) {
            progress = juce::jlimit(0.0, 1.0, f);
            if (stage.isNotEmpty()) { const juce::ScopedLock sl(lock); message = stage; }
            return !threadShouldExit() && !cancelled->load();
        });
    }

private:
    void timerCallback() override
    {
        {
            const juce::ScopedLock sl(lock);
            if (message != shown) { window.setMessage(message); shown = message; }
        }
        if (isThreadRunning()) return;
        stopTimer();
        if (window.isCurrentlyModal()) window.exitModalState(0);
        window.setVisible(false);
        const auto warnings = session->warnings;
        session.reset(); // plugin instances are deleted here, on the message thread
        report(warnings);
        delete this;
    }

    void report(const juce::StringArray& warnings)
    {
        juce::String extra;
        if (!warnings.isEmpty()) extra << "\n\nNot included in the export:\n" << warnings.joinIntoString("\n");
        if (result.cancelled) return;
        if (!result.ok) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export failed", result.error + extra);
            return;
        }
        juce::String msg;
        msg << "\"" << target.getFileName() << "\" is ready (" << formatTime(result.seconds) << ").";
        if (result.clipped)
            msg << "\n\nThe song is louder than the maximum (peak +"
                << juce::String(juce::Decibels::gainToDecibels(result.peak), 1)
                << " dB), so the loudest parts are distorted in the file. Turn the Mains or track volumes down, "
                   "or tick \"Normalize\" when exporting.";
        msg << extra;
        const auto file = target;
        juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::InfoIcon, "Export finished", msg,
#if JUCE_MAC
            "Show in Finder",
#else
            "Show file",
#endif
            "OK", nullptr, juce::ModalCallbackFunction::create([file](int r) { if (r == 1) file.revealToUser(); }));
    }

    std::unique_ptr<audio::RenderSession> session;
    audio::ExportOptions opts;
    juce::File target, encoder;
    audio::ExportResult result;
    std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
    double progress = 0.0;
    juce::CriticalSection lock;
    juce::String message, shown;
    juce::AlertWindow window;
};

// ---------------------------------------------------------------- steps after the dialog
void chooseFileAndExport(AppContext& app, const audio::ExportOptions& o, const juce::File& encoder)
{
    const auto& info = audio::formatInfo(o.format);
    const juce::String ext = info.extension;
    const auto initial = app.settings.lastDirectory().getChildFile(juce::File::createLegalFileName(o.title) + "." + ext);
    auto& chooser = fileChooser();
    chooser = std::make_unique<juce::FileChooser>("Export song as " + juce::String(info.name), initial, "*." + ext);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                         [&app, o, encoder, ext](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension(ext)) f = f.withFileExtension(ext);
        app.settings.setLastDirectory(f.getParentDirectory());
        juce::String err;
        if (o.format == ExportFormat::Midi) {
            if (!app.exportMidi(f, err))
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export failed", err);
            return;
        }
        auto session = app.createRenderSession(o.sampleRate, o.loopOnly, err);
        if (!session) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export song", err);
            return;
        }
        new ExportJob(std::move(session), o, f, encoder); // deletes itself when done
    });
}

void locateEncoder(AppContext& app, const audio::ExportOptions& o)
{
    auto& chooser = fileChooser();
#if JUCE_WINDOWS
    const juce::String pattern = "*.exe";
#else
    const juce::String pattern = "*";
#endif
    chooser = std::make_unique<juce::FileChooser>(o.format == ExportFormat::Mp3 ? "Choose lame or ffmpeg" : "Choose ffmpeg",
                                                  juce::File(), pattern);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [&app, o](const juce::FileChooser& fc) {
        const auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!audio::isUsableEncoder(o.format, f)) {
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export song",
                "\"" + f.getFileName() + "\" cannot be used for this format. Choose "
                + (o.format == ExportFormat::Mp3 ? juce::String("lame or ffmpeg.") : juce::String("ffmpeg.")));
            return;
        }
        app.settings.setEncoderPath(f);
        juce::MessageManager::callAsync([&app, o, f] { chooseFileAndExport(app, o, f); }); // after this chooser has closed
    });
}

void startExport(AppContext& app, const audio::ExportOptions& o)
{
    juce::File encoder;
    if (audio::needsExternalEncoder(o.format)) {
        encoder = audio::findEncoder(o.format, app.settings.encoderPath());
        if (!encoder.existsAsFile()) {
            juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::InfoIcon,
                juce::String(audio::formatInfo(o.format).name) + " encoder needed", audio::encoderInstallHint(o.format),
                "Locate encoder...", "Cancel", nullptr,
                juce::ModalCallbackFunction::create([&app, o](int r) { if (r == 1) locateEncoder(app, o); }));
            return;
        }
    }
    chooseFileAndExport(app, o, encoder);
}

void styleLabel(juce::Label& l)
{
    l.setFont(theme::uiFont(14.0f, true));
    l.setColour(juce::Label::textColourId, theme::col::text);
    l.setJustificationType(juce::Justification::centredRight);
}
} // namespace

// ---------------------------------------------------------------- running exports
bool ExportDialog::isExportRunning() { return !runningJobs().empty(); }

void ExportDialog::cancelRunningExports()
{
    while (!runningJobs().empty()) runningJobs().back()->abort(); // abort() removes the job from the list
}

// ---------------------------------------------------------------- dialog
void ExportDialog::show(AppContext& app)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new ExportDialog(app));
    o.dialogTitle = "Export song";
    o.dialogBackgroundColour = theme::col::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

ExportDialog::ExportDialog(AppContext& a) : app(a)
{
    for (auto* l : {&formatLabel, &rateLabel, &qualityLabel, &rangeLabel}) { styleLabel(*l); addAndMakeVisible(l); }
    note.setFont(theme::uiFont(13.0f));
    note.setColour(juce::Label::textColourId, theme::col::textDim);
    note.setJustificationType(juce::Justification::topLeft);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&format, &rate, &quality, &range, &tail, &normalize, &note, &exportBtn, &cancelBtn})
        addAndMakeVisible(c);

    const auto& formats = audio::exportFormats();
    for (size_t i = 0; i < formats.size(); ++i) format.addItem(formats[i].name, (int)i + 1);
    const auto chosen = audio::formatFromKey(app.settings.exportSetting("format", "wav"));
    for (size_t i = 0; i < formats.size(); ++i)
        if (formats[i].format == chosen) format.setSelectedId((int)i + 1, juce::dontSendNotification);
    format.onChange = [this] { formatChanged(); };

    range.addItem("Whole song", 1);
    const auto& p = app.project;
    const auto bar = std::max<model::Tick>(1, seq::ticksPerBar(p.timeSig));
    const bool hasLoop = p.loopEnd > p.loopStart;
    juce::String loopText = "Loop region";
    if (hasLoop)
        loopText << " (bar " << juce::String((double)p.loopStart / (double)bar + 1.0, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".")
                 << " to " << juce::String((double)p.loopEnd / (double)bar + 1.0, 1).trimCharactersAtEnd("0").trimCharactersAtEnd(".") << ")";
    range.addItem(loopText, 2);
    range.setItemEnabled(2, hasLoop);
    range.setSelectedId(1, juce::dontSendNotification);
    range.setTooltip("Export the whole song, or only the part between the loop markers");

    tail.setToggleState(app.settings.exportSetting("tail", "1") == "1", juce::dontSendNotification);
    tail.setTooltip("Keeps rendering after the last note until reverb and note releases have died away");
    normalize.setToggleState(app.settings.exportSetting("normalize", "0") == "1", juce::dontSendNotification);
    normalize.setTooltip("Makes the song as loud as possible without distortion: the loudest moment is set to -1 dB");
    rate.setTooltip("44.1 kHz is CD quality and right for most uses; 48 kHz is standard for video");
    rate.onChange = [this] { updateNote(); };
    quality.onChange = [this] { updateNote(); };

    exportBtn.onClick = [this] { exportClicked(); };
    cancelBtn.onClick = [this] { if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState(0); };
    formatChanged();
    setSize(580, 420);
}

void ExportDialog::formatChanged()
{
    const auto f = audio::exportFormats()[(size_t)std::max(0, format.getSelectedId() - 1)].format;
    const auto& info = audio::formatInfo(f);
    const juce::String key = info.key;

    // sample rates the format supports
    const auto oldRate = rate.getSelectedId() > 0 ? rate.getSelectedId() : app.settings.exportSetting("rate", "44100").getIntValue();
    rate.clear(juce::dontSendNotification);
    for (auto r : kRates)
        if (r <= audio::maxSampleRate(f)) rate.addItem(juce::String(r / 1000.0, 1) + " kHz", (int)r);
    rate.setSelectedId(rate.indexOfItemId(oldRate) >= 0 ? oldRate : 44100, juce::dontSendNotification);

    // bit depth (lossless) or bit rate (lossy)
    quality.clear(juce::dontSendNotification);
    if (info.lossy) {
        qualityLabel.setText("Bit rate", juce::dontSendNotification);
        for (int k : audio::bitRates(f))
            quality.addItem(f == ExportFormat::Ogg ? juce::String::fromUTF8("\xe2\x89\x88 ") + juce::String(k) + " kbps (variable)"
                                                   : juce::String(k) + " kbps", k);
        const int def = f == ExportFormat::Mp3 ? 320 : (f == ExportFormat::M4a ? 256 : 192);
        const int want = app.settings.exportSetting("kbps." + key, juce::String(def)).getIntValue();
        quality.setSelectedId(quality.indexOfItemId(want) >= 0 ? want : def, juce::dontSendNotification);
    } else if (info.audio) {
        qualityLabel.setText("Bit depth", juce::dontSendNotification);
        for (int b : audio::bitDepths(f))
            quality.addItem(b == 16 ? "16-bit (CD quality)" : b == 24 ? "24-bit (studio quality)" : "32-bit float (for further mixing)", b);
        const int want = app.settings.exportSetting("bits." + key, "24").getIntValue();
        quality.setSelectedId(quality.indexOfItemId(want) >= 0 ? want : 24, juce::dontSendNotification);
    } else {
        qualityLabel.setText("Quality", juce::dontSendNotification);
    }

    for (juce::Component* c : std::initializer_list<juce::Component*>{&rate, &quality, &range, &tail, &normalize, &rateLabel, &qualityLabel, &rangeLabel})
        c->setEnabled(info.audio);
    updateNote();
}

void ExportDialog::updateNote()
{
    const auto o = currentOptions();
    juce::String s;
    switch (o.format) {
        case ExportFormat::Wav: s = "Uncompressed: the best quality, for CDs, video and further work in other programs. Large files."; break;
        case ExportFormat::Flac: s = "Lossless compression: exactly the same sound as WAV in about half the size."; break;
        case ExportFormat::Mp3: s = "Plays everywhere. Small files; 320 kbps sounds practically like the original."; break;
        case ExportFormat::Ogg: s = "Free, open format. Small files with very good sound."; break;
        case ExportFormat::M4a: s = "AAC, as used by Apple Music and iTunes. Small files with very good sound."; break;
        case ExportFormat::Midi: s = "Notes only, no sound: every instrument track becomes a MIDI track. "
                                     "Opens in any DAW or notation program."; break;
    }
    if (audio::needsExternalEncoder(o.format)) {
        const auto enc = audio::findEncoder(o.format, app.settings.encoderPath());
        s << "\n\nEncoder: " << (enc.existsAsFile() ? enc.getFullPathName()
                                                    : juce::String(o.format == ExportFormat::Mp3 ? "LAME or FFmpeg" : "FFmpeg")
                                                          + " is needed (not found). You will be shown how to get it.");
    }
    if (o.format != ExportFormat::Midi) {
        const auto len = (o.loopOnly ? app.project.loopEnd - app.project.loopStart
                                     : seq::SequenceSnapshot::build(app.project).lengthTicks);
        s << "\n\nLength: " << formatTime(seq::ticksToSeconds(std::max<model::Tick>(0, len), app.project.tempoBpm))
          << (o.includeTail ? " plus the ending tail" : "");
    }
    note.setText(s, juce::dontSendNotification);
}

audio::ExportOptions ExportDialog::currentOptions() const
{
    audio::ExportOptions o;
    o.format = audio::exportFormats()[(size_t)std::max(0, format.getSelectedId() - 1)].format;
    o.sampleRate = rate.getSelectedId() > 0 ? (double)rate.getSelectedId() : 44100.0;
    const auto& info = audio::formatInfo(o.format);
    if (info.lossy) o.bitrateKbps = std::max(64, quality.getSelectedId());
    else if (info.audio) o.bitDepth = quality.getSelectedId() > 0 ? quality.getSelectedId() : 24;
    o.loopOnly = range.getSelectedId() == 2;
    o.includeTail = tail.getToggleState();
    o.normalize = normalize.getToggleState();
    o.title = songTitle(app);
    return o;
}

void ExportDialog::remember(const audio::ExportOptions& o)
{
    auto& s = app.settings;
    const auto& info = audio::formatInfo(o.format);
    const juce::String key = info.key;
    s.setExportSetting("format", key);
    if (!info.audio) return;
    s.setExportSetting("rate", juce::String((int)o.sampleRate));
    if (info.lossy) s.setExportSetting("kbps." + key, juce::String(o.bitrateKbps));
    else s.setExportSetting("bits." + key, juce::String(o.bitDepth));
    s.setExportSetting("tail", o.includeTail ? "1" : "0");
    s.setExportSetting("normalize", o.normalize ? "1" : "0");
}

void ExportDialog::exportClicked()
{
    const auto o = currentOptions();
    remember(o);
    auto& a = app;
    juce::MessageManager::callAsync([&a, o] { startExport(a, o); }); // after this dialog has closed
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState(1); // deletes this dialog
}

void ExportDialog::paint(juce::Graphics& g) { g.fillAll(theme::col::panel); }

void ExportDialog::resized()
{
    auto r = getLocalBounds().reduced(theme::gapSection);
    auto row = [&](juce::Label& l, juce::Component& c) {
        auto line = r.removeFromTop(28);
        l.setBounds(line.removeFromLeft(100));
        line.removeFromLeft(theme::gap);
        c.setBounds(line);
        r.removeFromTop(theme::gap);
    };
    row(formatLabel, format);
    row(rateLabel, rate);
    row(qualityLabel, quality);
    row(rangeLabel, range);
    const int indent = 100 + theme::gap;
    tail.setBounds(r.removeFromTop(24).withTrimmedLeft(indent));
    normalize.setBounds(r.removeFromTop(24).withTrimmedLeft(indent));
    auto buttons = r.removeFromBottom(30);
    cancelBtn.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(theme::gap);
    exportBtn.setBounds(buttons.removeFromRight(120));
    r.removeFromTop(theme::gap);
    r.removeFromBottom(theme::gap);
    note.setBounds(r.withTrimmedLeft(indent - 4));
}

} // namespace mc::gui
