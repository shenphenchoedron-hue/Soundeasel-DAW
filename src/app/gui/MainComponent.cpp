#include "gui/MainComponent.h"
#include "gui/PluginBrowser.h"
#include "gui/SettingsComponent.h"
#include "gui/Theme.h"

namespace mc::gui {

namespace {
enum MenuId {
    New = 1, Open, Save, SaveAs, ImportMidi, ImportMusicXml, ExportMidi, ExportMusicXml, Quit, ImportAudio,
    Undo = 100, Redo, Cut, Copy, Paste, Delete, SelectAll, AddTrack, DeleteTrack, AddStepSeq, AddAudioTrack,
    Settings = 200, Plugins, Rescan, SoundsFolder, Manual, About, TestSound,
    ShowMixer = 300, ShowPianoRoll
};
const char* kProjectExt = "*.mcproj";
}

MainComponent::MainComponent(AppContext& a)
    : app(a), transport(a), chordPanel(a), trackList(a), pianoRoll(a), stepSeq(a, pianoRoll), audioTracks(a, pianoRoll), midiOverview(a, pianoRoll), mixer(a)
{
    for (juce::Component* c : std::initializer_list<juce::Component*>{&transport, &chordPanel, &page})
        addAndMakeVisible(c);
    addChildComponent(mixer);
    addAndMakeVisible(transport.statusButton());
    mixer.onClose = [this] { setMixerVisible(false); };
    transport.onToggleMixer = [this] { setMixerVisible(!mixerVisible); };
    for (juce::Component* c : std::initializer_list<juce::Component*>{&trackList, &pianoRoll, &stepSeq, &audioTracks,
                                                                      &zoomLabel, &zoomOutH, &zoomInH, &zoomOutV, &zoomInV,
                                                                      &gridLabel, &gridBox, &snapBtn, &rollBtn, &midiOverview})
        pageContent.addAndMakeVisible(c);
    // Note grid + snap for the piano roll (extendable with triplet/dotted variants; GridValue supports them).
    gridLabel.setFont(theme::uiFont(13.5f, true));
    gridLabel.setColour(juce::Label::textColourId, theme::col::textDim);
    gridLabel.setJustificationType(juce::Justification::centredRight);
    gridBox.addItem("1/4", 4); gridBox.addItem("1/8", 8); gridBox.addItem("1/16", 16); gridBox.addItem("1/32", 32);
    gridBox.addItem("1/8 T", 108); gridBox.addItem("1/16 T", 116);
    gridBox.setSelectedId(app.grid.triplet ? app.grid.denominator + 100 : app.grid.denominator, juce::dontSendNotification);
    gridBox.setTooltip("Note grid of the piano roll");
    gridBox.onChange = [this] {
        const int id = gridBox.getSelectedId();
        app.grid = id > 100 ? seq::GridValue{id - 100, true} : seq::GridValue{id};
        app.sendChangeMessage();
    };
    snapBtn.setToggleState(app.snapEnabled, juce::dontSendNotification);
    snapBtn.setTooltip("Snap notes to the grid");
    snapBtn.onClick = [this] { app.snapEnabled = snapBtn.getToggleState(); };
    rollBtn.onClick = [this] { setPianoRollVisible(!rollVisible); };
    page.setViewedComponent(&pageContent, false);
    page.setScrollBarsShown(true, false);
    page.setScrollBarThickness(12);
    pianoRoll.onViewChanged = [this] { stepSeq.viewChanged(); audioTracks.viewChanged(); midiOverview.viewChanged(); };
    stepSeq.onHeightChanged = [this] { resized(); };
    audioTracks.onHeightChanged = [this] { resized(); };
    midiOverview.onHeightChanged = [this] { resized(); };
    midiOverview.onOpenTrack = [this](model::TrackId id) { app.selectTrack(id); setPianoRollVisible(true); };
    zoomLabel.setFont(theme::uiFont(13.5f, true));
    zoomLabel.setColour(juce::Label::textColourId, theme::col::textDim);
    zoomLabel.setJustificationType(juce::Justification::centredRight);
    zoomOutH.setTooltip("Zoom out in time (-, Ctrl+wheel)");
    zoomInH.setTooltip("Zoom in in time (+, Ctrl+wheel)");
    zoomOutV.setTooltip("Smaller rows and keys (Alt+wheel)");
    zoomInV.setTooltip("Taller rows and keys (Alt+wheel)");
    zoomOutH.onClick = [this] { pianoRoll.zoomOutH(); };
    zoomInH.onClick = [this] { pianoRoll.zoomInH(); };
    zoomOutV.onClick = [this] { pianoRoll.zoomOutV(); };
    zoomInV.onClick = [this] { pianoRoll.zoomInV(); };
#if !JUCE_MAC
    addAndMakeVisible(menuBar);
#else
    juce::MenuBarModel::setMacMainMenu(this);
#endif
    setWantsKeyboardFocus(true);
    app.addChangeListener(this);
    startTimerHz(30);
    setSize(1500, 900);
    setMixerVisible(app.settings.mixerVisible());
    setPianoRollVisible(app.settings.pianoRollVisible());

    if (!app.plugins->hasCache()) // first start: offer a scan
        juce::MessageManager::callAsync([this] {
            juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::QuestionIcon, "Instrument plugins",
                "Scan for installed instrument plugins now? (Can be done later via Options)", "Scan", "Later", this,
                juce::ModalCallbackFunction::create([this](int r) { if (r == 1) PluginBrowser::rescanWithProgress(app, {}); }));
        });
}

MainComponent::~MainComponent()
{
#if JUCE_MAC
    juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
    app.removeChangeListener(this);
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
#if !JUCE_MAC
    menuBar.setBounds(r.removeFromTop(24));
#endif
    transport.setBounds(r.removeFromTop(44));
    {
        auto row = r.removeFromTop(40);
        chordPanel.setBounds(row);
        // sound status ("does my sound work?") at the right end of the chord bar
        const int sw = std::min(460, row.getWidth() - 1130);
        transport.statusButton().setBounds(sw >= 200 ? row.removeFromRight(sw).reduced(theme::gap, 7) : juce::Rectangle<int>());
    }
    r.removeFromTop(1);
    if (mixerVisible) {
        const int mh = std::min(MixerComponent::kPreferredHeight, std::max(240, r.getHeight() * 3 / 5));
        mixer.setBounds(r.removeFromBottom(mh));
    }
    page.setBounds(r);
    const int viewH = r.getHeight();
    const int audioH = audioTracks.preferredHeight();
    const int stepsH = stepSeq.preferredHeight();
    const int belowH = 1 + audioH + (stepsH > 0 ? 1 + stepsH : 0);
    // Closed piano roll: the bar plus a compact lane per instrument track (mini notes).
    const int overviewH = midiOverview.preferredHeight();
    int topH = kBarHeight + 1 + overviewH, totalH = std::max(viewH, topH + belowH);
    if (rollVisible) {
        // The piano roll height never depends on the content below it: a fixed strip below it shows the start
        // of the audio tracks and step sequencers; more tracks are added further down (scroll to them).
        const bool onlyHeader = stepsH == 0 && audioH <= AudioTracksComponent::kHeaderHeight;
        const int peek = onlyHeader ? audioH + 1 : std::min(StepSequencerComponent::kPeekHeight, (int)(viewH * 0.3)) + 1;
        topH = std::max(260, viewH - peek);
        totalH = std::max(topH + belowH, topH + peek);
    }
    const int w = totalH > viewH ? r.getWidth() - page.getScrollBarThickness() : r.getWidth();
    pageContent.setSize(w, totalH);

    auto top = juce::Rectangle<int>(0, 0, w, topH);
    audioTracks.cardWidth = 290 + 1;
    audioTracks.setBounds(0, topH + 1, w, audioH);
    stepSeq.cardWidth = 290 + 1;
    stepSeq.setBounds(stepsH > 0 ? juce::Rectangle<int>(0, topH + 1 + audioH + 1, w, stepsH) : juce::Rectangle<int>());
    pageContent.darkFrom = topH + belowH;

    trackList.setVisible(rollVisible);
    pianoRoll.setVisible(rollVisible);
    zoomOutV.setVisible(rollVisible);
    zoomInV.setVisible(rollVisible);
    midiOverview.setVisible(!rollVisible);
    midiOverview.cardWidth = 290 + 1;
    if (!rollVisible) midiOverview.setBounds(0, kBarHeight + 1, w, overviewH);
    // The bar is always the top row of the page, so the piano roll button and the zoom/grid
    // controls never move when the piano roll is opened or closed.
    auto bar = top.removeFromTop(kBarHeight);
    top.removeFromTop(1);
    {
        auto b = bar.reduced(theme::gap, 5);
        rollBtn.setBounds(b.removeFromLeft(160));
        b.removeFromLeft(theme::gapSection);
        zoomLabel.setBounds(b.removeFromLeft(48));
        b.removeFromLeft(theme::gapS);
        for (auto* z : {&zoomOutH, &zoomInH, &zoomOutV, &zoomInV}) { z->setBounds(b.removeFromLeft(84)); b.removeFromLeft(theme::gapS); }
        b.removeFromLeft(theme::gapSection);
        gridLabel.setBounds(b.removeFromLeft(40));
        b.removeFromLeft(theme::gapS);
        gridBox.setBounds(b.removeFromLeft(84));
        b.removeFromLeft(theme::gap);
        snapBtn.setBounds(b.removeFromLeft(70));
    }
    if (rollVisible) {
        trackList.setBounds(top.removeFromLeft(290));
        top.removeFromLeft(1);
    }
    pageContent.toolbar = bar;
    if (rollVisible) pianoRoll.setBounds(top);
}

void MainComponent::setPianoRollVisible(bool b)
{
    rollVisible = b;
    app.settings.setPianoRollVisible(b);
    rollBtn.setButtonText(juce::String::fromUTF8(b ? "\xe2\x96\xbe  Hide piano roll" : "\xe2\x96\xb8  Show piano roll")); // ▾ / ▸
    rollBtn.setTooltip(b ? "Close the piano roll and instrument track list to make room (F2)" : "Open the piano roll (F2)");
    resized();
}

void MainComponent::Page::paint(juce::Graphics& g)
{
    g.fillAll(theme::col::border); // 1px seams between sections
    g.setColour(theme::col::panel);
    g.fillRect(toolbar);
    g.setColour(theme::col::rollBg);
    g.fillRect(0, darkFrom, getWidth(), getHeight() - darkFrom);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::col::border); // 1px seams between sections
}

void MainComponent::setMixerVisible(bool b)
{
    mixerVisible = b;
    mixer.setVisible(b);
    transport.setMixerShown(b);
    app.settings.setMixerVisible(b);
    resized();
}

void MainComponent::timerCallback()
{
    app.pollLevels();
    trackList.refreshMeters();
    audioTracks.refresh();
    midiOverview.refresh();
    if (mixerVisible) mixer.refreshMeters();
    transport.refreshPosition();
    chordPanel.refreshDisplay();
    pianoRoll.refreshPlayhead();
    stepSeq.refreshPlayhead();
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    menuItemsChanged();
    if (onTitleChanged) {
        juce::String t = app.currentFile.existsAsFile() ? app.currentFile.getFileNameWithoutExtension() : juce::String("Untitled");
        if (app.hasUnsavedChanges()) t += " *";
        onTitleChanged(t + " - Soundeasel DAW");
    }
}

bool MainComponent::keyPressed(const juce::KeyPress& k)
{
    const bool cmd = k.getModifiers().isCommandDown(), shift = k.getModifiers().isShiftDown();
    if (k == juce::KeyPress::spaceKey) { app.engine->isPlaying() ? app.stop() : app.play(); return true; }
    if (k == juce::KeyPress::homeKey) { app.returnToStart(); return true; }
    if (k == juce::KeyPress::F3Key) { setMixerVisible(!mixerVisible); return true; }
    if (k == juce::KeyPress::F2Key) { setPianoRollVisible(!rollVisible); return true; }
    if (cmd && k.getKeyCode() == 'Z') { shift ? app.undo.redo() : app.undo.undo(); return true; }
    if (cmd && k.getKeyCode() == 'Y') { app.undo.redo(); return true; }
    if (cmd && k.getKeyCode() == 'S') { shift ? saveDialog() : save(); return true; }
    if (cmd && k.getKeyCode() == 'O') { openDialog(); return true; }
    if (cmd && k.getKeyCode() == 'N') { menuItemSelected(New, 0); return true; }
    if (!cmd && k.getKeyCode() == 'R') { app.toggleRecord(); return true; }
    if (!cmd && k.getKeyCode() == 'K') { app.setMetronome(!app.metronome()); return true; }
    if (!cmd && k.getKeyCode() == 'L') { app.setLoop(!app.project.loopEnabled, app.project.loopStart, app.project.loopEnd); return true; }
    if (!cmd && k.getKeyCode() == 'C') { auto& h = app.project.harmony; app.setHarmony(h.root, h.scaleId, !h.chordMode); return true; }
    return false;
}

juce::StringArray MainComponent::getMenuBarNames() { return {"File", "Edit", "View", "Options"}; }

juce::PopupMenu MainComponent::getMenuForIndex(int idx, const juce::String&)
{
    juce::PopupMenu m;
    if (idx == 0) {
        m.addItem(New, "New project");
        m.addItem(Open, "Open project...");
        m.addItem(Save, "Save");
        m.addItem(SaveAs, "Save as...");
        m.addSeparator();
        m.addItem(ImportAudio, "Import audio file (WAV, AIFF, FLAC, Ogg, MP3)...");
        m.addItem(ImportMidi, "Import MIDI file...");
        m.addItem(ImportMusicXml, "Import MusicXML (MuseScore)...");
        m.addItem(ExportMidi, "Export MIDI file...");
        m.addItem(ExportMusicXml, "Export MusicXML (MuseScore)...");
        m.addSeparator();
        m.addItem(Quit, "Quit");
    } else if (idx == 1) {
        m.addItem(Undo, "Undo " + juce::String(app.undo.undoName()), app.undo.canUndo());
        m.addItem(Redo, "Redo", app.undo.canRedo());
        m.addSeparator();
        m.addItem(Cut, "Cut notes");
        m.addItem(Copy, "Copy notes");
        m.addItem(Paste, "Paste notes at playhead", !app.clipboard.empty());
        m.addItem(Delete, "Delete notes");
        m.addItem(SelectAll, "Select all notes");
        m.addSeparator();
        m.addItem(AddTrack, "Add instrument track (piano roll)");
        m.addItem(AddStepSeq, "Add step sequencer track");
        m.addItem(AddAudioTrack, "Add audio track");
        m.addItem(DeleteTrack, "Delete active track", app.project.track(app.project.activeTrack) != nullptr);
    } else if (idx == 2) {
        m.addItem(ShowPianoRoll, "Piano roll  (F2)", true, rollVisible);
        m.addItem(ShowMixer, "Mixer  (F3)", true, mixerVisible);
    } else {
        m.addItem(Settings, "Settings (sound device, MIDI, modifier keys)...");
        m.addItem(TestSound, "Play test sound");
        m.addItem(Plugins, "Plugin browser...");
        m.addItem(Rescan, "Rescan plugins");
        m.addItem(SoundsFolder, "Open Sounds folder (add .sf2 files)...");
        m.addSeparator();
        m.addItem(Manual, "User manual");
        m.addItem(About, "About Soundeasel DAW...");
    }
    return m;
}

void MainComponent::menuItemSelected(int id, int)
{
    switch (id) {
        case New: confirmDiscard([this] { app.newProject(); }); break;
        case Open: openDialog(); break;
        case Save: save(); break;
        case SaveAs: saveDialog(); break;
        case ImportMidi: importDialog(); break;
        case ImportMusicXml: importMusicXmlDialog(); break;
        case ExportMidi: exportDialog(); break;
        case ExportMusicXml: exportMusicXmlDialog(); break;
        case Quit: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
        case Undo: app.undo.undo(); break;
        case Redo: app.undo.redo(); break;
        case Cut: pianoRoll.copySelection(); pianoRoll.deleteSelection(); break;
        case Copy: pianoRoll.copySelection(); break;
        case Paste: pianoRoll.paste(); break;
        case Delete: pianoRoll.deleteSelection(); break;
        case SelectAll: pianoRoll.selectAll(); break;
        case AddTrack: app.addTrack(); break;
        case AddStepSeq: app.addStepSequencer(); break;
        case AddAudioTrack: app.addAudioTrack(); break;
        case ImportAudio: audioTracks.importDialog(0, (model::Tick)app.engine->position()); break;
        case ShowMixer: setMixerVisible(!mixerVisible); break;
        case ShowPianoRoll: setPianoRollVisible(!rollVisible); break;
        case TestSound: app.playTestTone(); break;
        case DeleteTrack: app.deleteTrack(app.project.activeTrack); break;
        case Settings: SettingsComponent::show(app); break;
        case Plugins: {
            const auto tid = app.project.activeTrack;
            PluginBrowser::show(app, [this, tid](const plugins::PluginInfo& i) { app.setTrackPlugin(tid, &i); });
            break;
        }
        case Rescan: PluginBrowser::rescanWithProgress(app, {}); break;
        case SoundsFolder: app.openSoundsFolder(); break;
        case Manual: app.openManual(); break;
        case About: app.showAbout(); break;
        default: break;
    }
}

void MainComponent::showError(const juce::String& msg)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Soundeasel DAW", msg);
}

void MainComponent::confirmDiscard(std::function<void()> proceed)
{
    if (!app.hasUnsavedChanges()) { proceed(); return; }
    juce::AlertWindow::showYesNoCancelBox(juce::MessageBoxIconType::QuestionIcon, "Unsaved changes",
        "Save changes to the current project?", "Save", "Discard", "Cancel", this,
        juce::ModalCallbackFunction::create([this, proceed](int r) {
            if (r == 1) save(proceed);
            else if (r == 2) proceed();
        }));
}

void MainComponent::requestQuit(std::function<void()> quit) { confirmDiscard(std::move(quit)); }

void MainComponent::openDialog()
{
    confirmDiscard([this] {
        chooser = std::make_unique<juce::FileChooser>("Open project", app.settings.lastDirectory(), kProjectExt);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            app.settings.setLastDirectory(f.getParentDirectory());
            juce::String err;
            if (!app.openProject(f, err)) showError("Could not open project:\n" + err);
            else if (err.isNotEmpty()) showError(err);
            for (auto& t : app.project.tracks())
                if (app.isPluginMissing(t->id())) { showError("Some instrument plugins are missing. Their tracks are marked \"Plugin missing\"; MIDI data is intact."); break; }
        });
    });
}

void MainComponent::save(std::function<void()> after)
{
    if (!app.currentFile.existsAsFile()) { saveDialog(std::move(after)); return; }
    juce::String err;
    if (!app.saveProject(app.currentFile, err)) { showError("Save failed:\n" + err); return; }
    if (after) after();
}

void MainComponent::saveDialog(std::function<void()> after)
{
    chooser = std::make_unique<juce::FileChooser>("Save project", app.settings.lastDirectory().getChildFile("Untitled.mcproj"), kProjectExt);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, after](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("mcproj")) f = f.withFileExtension("mcproj");
        app.settings.setLastDirectory(f.getParentDirectory());
        juce::String err;
        if (!app.saveProject(f, err)) { showError("Save failed:\n" + err); return; }
        if (after) after();
    });
}

void MainComponent::importDialog()
{
    chooser = std::make_unique<juce::FileChooser>("Import MIDI file", app.settings.lastDirectory(), "*.mid;*.midi");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        app.settings.setLastDirectory(f.getParentDirectory());
        juce::String err;
        if (!app.importMidi(f, err)) showError("Import failed:\n" + err);
    });
}

void MainComponent::exportDialog()
{
    chooser = std::make_unique<juce::FileChooser>("Export MIDI file", app.settings.lastDirectory().getChildFile("export.mid"), "*.mid");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("mid")) f = f.withFileExtension("mid");
        juce::String err;
        if (!app.exportMidi(f, err)) showError("Export failed:\n" + err);
    });
}

void MainComponent::importMusicXmlDialog()
{
    chooser = std::make_unique<juce::FileChooser>("Import MusicXML", app.settings.lastDirectory(), "*.musicxml;*.mxl;*.xml");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        app.settings.setLastDirectory(f.getParentDirectory());
        juce::String err;
        if (!app.importMusicXml(f, err)) showError("Import failed:\n" + err);
    });
}

void MainComponent::exportMusicXmlDialog()
{
    const auto name = (app.currentFile.existsAsFile() ? app.currentFile.getFileNameWithoutExtension() : juce::String("export")) + ".musicxml";
    chooser = std::make_unique<juce::FileChooser>("Export MusicXML", app.settings.lastDirectory().getChildFile(name), "*.musicxml");
    chooser->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        if (!f.hasFileExtension("musicxml")) f = f.withFileExtension("musicxml");
        app.settings.setLastDirectory(f.getParentDirectory());
        juce::String err;
        if (!app.exportMusicXml(f, err)) showError("Export failed:\n" + err);
    });
}

} // namespace mc::gui
