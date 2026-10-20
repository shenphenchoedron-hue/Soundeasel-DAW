#include "AppContext.h"
#include <unordered_map>
#include "io/MidiFile.h"
#include "io/MusicXml.h"
#include "io/ProjectSerializer.h"
#include "plugins/BasicSynth.h"
#include "plugins/BuiltinInstruments.h"
#include "plugins/SoundFontPlayer.h"

namespace mc {

namespace {
class PluginEditorWindow final : public juce::DocumentWindow {
public:
    PluginEditorWindow(const juce::String& title, juce::Component* editor, std::function<void()> onClose)
        : DocumentWindow(title, juce::Colour(0xffe9edf2), DocumentWindow::closeButton), closeFn(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
        setContentOwned(editor, true);
        if (auto* ape = dynamic_cast<juce::AudioProcessorEditor*>(editor)) setResizable(ape->isResizable(), false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }
    void closeButtonPressed() override { juce::MessageManager::callAsync(closeFn); }
private:
    std::function<void()> closeFn;
};

model::PluginReference defaultInstrument()
{
    return plugins::PluginManager::referenceFor(plugins::InternalPluginHost::defaultInstrument());
}
} // namespace

AppContext::AppContext()
{
    // The user's saved device comes first. Two inputs for audio tracks if the device can open
    // them; otherwise the same device with outputs only, so instruments always make sound.
    // Only if the saved device cannot be opened at all, the system default is used - and that
    // fallback is never written back over the user's choice.
    auto state = settings.audioDeviceState();
    auto opened = [this] { return deviceManager.getCurrentAudioDevice() != nullptr; };
    auto err = deviceManager.initialise(2, 2, state.get(), false);
    if (err.isNotEmpty() || !opened()) {
        DBG("Audio init: " << err);
        err = deviceManager.initialise(0, 2, state.get(), false);
    }
    if (err.isNotEmpty() || !opened()) {
        DBG("Audio init (output only): " << err);
        deviceFellBack = state != nullptr; // the saved device is not available right now
        err = deviceManager.initialise(2, 2, nullptr, true);
        if (err.isNotEmpty() || !opened()) err = deviceManager.initialise(0, 2, nullptr, true);
        if (err.isNotEmpty()) { DBG("Audio init (default device): " << err); }
    }

    engine = std::make_unique<audio::AudioEngine>(deviceManager);
    audioFiles.onLoaded = [this] { syncEngine(); sendChangeMessage(); };
    ensureStereoOutput();
    deviceManager.addChangeListener(this);
    diskThread.startThread();
    plugins = std::make_unique<plugins::PluginManager>(settings.dataDirectory());
    midiDevices = std::make_unique<midi::MidiDeviceManager>(deviceManager, [this](const juce::MidiMessage& m) { handleMidi(m); });
    midiDevices->selectInput(settings.midiInputIdentifier());

    chordInput.setMapping(settings.controllerMapping());
    project.controllerMapping = chordInput.mapping();
    metronomeOn = settings.metronome();
    metronomeLvl = settings.metronomeLevel();
    countIn = settings.countInBars();
    engine->setMetronome(metronomeOn);
    engine->setMetronomeLevel(metronomeLvl);

    listenerHandle = project.addListener([this] { onModelChanged(); });
    newProject();
}

AppContext::~AppContext()
{
    project.removeListener(listenerHandle);
    deviceManager.removeChangeListener(this);
    engine->stopAudioRecording();
    diskThread.stopThread(2000);
    editorWindows.clear();
    insertEditors.clear();
    midiDevices.reset();
    if (!deviceFellBack)
        if (auto xml = deviceManager.createStateXml()) settings.setAudioDeviceState(xml.get());
    engine.reset();
    inserts.clear(); // after the engine: nothing processes them any more
}

// ---------------------------------------------------------------- model sync
void AppContext::onModelChanged()
{
    // Step sequencer lines are edited in the step sequencer panel, not the piano roll:
    // keep a piano roll track active (e.g. after adding a line, or undo/redo).
    if (auto* a = project.midiTrack(project.activeTrack); project.activeTrack != 0 && (!a || a->isStepLine())) {
        model::TrackId pick = 0;
        if (auto* l = project.midiTrack(lastPianoTrack); l && !l->isStepLine()) pick = lastPianoTrack;
        else for (auto& t : project.tracks())
            if (auto* m = dynamic_cast<model::MidiTrack*>(t.get()); m && !m->isStepLine()) { pick = m->id(); break; }
        project.activeTrack = pick;
    }
    if (project.midiTrack(project.activeTrack)) lastPianoTrack = project.activeTrack;
    if (!suppressDirty) dirty = true;
    syncEngine();
    sendChangeMessage();
}

void AppContext::syncEngine()
{
    // remove engine slots for deleted tracks
    for (auto id : engine->trackIds())
        if (!project.track(id)) {
            closeEditor(id);
            prepareToRemove(id);
            engine->removeTrack(id);
            clearInserts(id);
            loadedRefs.erase(id);
            missing.erase(id);
            errors.erase(id);
        }

    for (auto& t : project.tracks()) {
        if (t->kind() == model::TrackKind::Audio && !engine->hasTrack(t->id())) engine->setInstrument(t->id(), nullptr);
        auto* mt = dynamic_cast<model::MidiTrack*>(t.get());
        if (!mt) continue;
        auto it = loadedRefs.find(mt->id());
        const bool same = it != loadedRefs.end() && it->second.format == mt->plugin.format && it->second.identifier == mt->plugin.identifier;
        if (!same) {
            closeEditor(mt->id());
            std::unique_ptr<plugins::InstrumentPlugin> inst;
            missing.erase(mt->id());
            errors.erase(mt->id());
            if (!mt->plugin.empty()) {
                juce::String err;
                inst = plugins->instantiate(mt->plugin, engine->sampleRate(), engine->blockSize(), err);
                if (!inst) { missing.insert(mt->id()); errors[mt->id()] = err; }
            }
            if (auto* old = engine->instrument(mt->id())) old->prepareToRemove();
            engine->setInstrument(mt->id(), std::move(inst)); // old instance destroyed here, on the message thread
            loadedRefs[mt->id()] = mt->plugin;
        }
        engine->setBypassed(mt->id(), mt->plugin.bypassed);
    }
    for (auto& t : project.tracks()) syncInserts(*t);

    activeTrackAtomic = project.activeTrack;
    if (auto* a = activeMidiTrack()) activeChannelAtomic = a->channel;
    engine->setTempo(project.tempoBpm);
    engine->setMeter(seq::ticksPerBeat(project.timeSig), seq::ticksPerBar(project.timeSig));
    engine->setLoop(project.loopEnabled, project.loopStart, project.loopEnd);
    auto snap = std::make_shared<audio::PlaybackSnapshot>();
    snap->seq = seq::SequenceSnapshot::build(project);
    const double sr = engine->sampleRate();
    snap->clips.resize(snap->seq.tracks.size());
    std::set<std::string> used;
    for (size_t i = 0; i < snap->seq.tracks.size(); ++i)
        for (auto& c : snap->seq.tracks[i].clips) {
            used.insert(c.file);
            auto data = audioFiles.find(c.file, sr);
            if (!data) { audioFiles.request(c.file, sr); continue; } // decoded in the background, then synced again
            if (auto pc = audio::PlaybackSnapshot::makeClip(c, std::move(data))) snap->clips[i].push_back(std::move(*pc));
        }
    engine->setSnapshot(std::move(snap));
    audioFiles.retainOnly(used);
}

void AppContext::changeListenerCallback(juce::ChangeBroadcaster*)
{
    // Device or sample rate changed: audio files are resampled for the new rate.
    ensureStereoOutput();
    syncEngine();
    sendChangeMessage();
}

juce::String AppContext::pluginError(model::TrackId id) const
{
    auto it = errors.find(id);
    return it == errors.end() ? juce::String() : it->second;
}

void AppContext::captureAllPluginStates()
{
    for (auto& t : project.tracks()) captureTrackPluginStates(*t);
}

void AppContext::captureTrackPluginStates(model::Track& t)
{
    captureInsertStates(t);
    if (auto* mt = dynamic_cast<model::MidiTrack*>(&t))
        if (!isPluginMissing(mt->id()))
            if (auto* inst = engine->instrument(mt->id())) {
                auto state = inst->getState();
                mt->plugin.stateBase64 = state.getSize() ? state.toBase64Encoding().toStdString() : std::string();
            }
}

// ---------------------------------------------------------------- files
void AppContext::newProject()
{
    stop();
    suppressDirty = true;
    model::Project p;
    p.controllerMapping = chordInput.mapping();
    auto& t = p.addMidiTrack("Piano");
    t.plugin = defaultInstrument();
    project = p;
    undo.clear();
    currentFile = juce::File();
    setHarmony(project.harmony.root, project.harmony.scaleId, project.harmony.chordMode);
    engine->setPosition(0);
    project.notifyChanged();
    suppressDirty = false;
    dirty = false;
}

bool AppContext::openProject(const juce::File& f, juce::String& error)
{
    model::Project p;
    auto r = io::ProjectSerializer::loadFile(f.getFullPathName().toStdString(), p);
    if (!r.ok) { error = r.error; return false; }
    stop();
    suppressDirty = true;
    // Mapping belongs to the user's keyboard: settings win over the project copy.
    p.controllerMapping = chordInput.mapping();
    loadedRefs.clear(); // force re-instantiation with stored state
    for (auto id : engine->trackIds()) { closeEditor(id); prepareToRemove(id); engine->removeTrack(id); clearInserts(id); }
    project = p;
    undo.clear();
    currentFile = f;
    setHarmony(project.harmony.root, project.harmony.scaleId, project.harmony.chordMode);
    engine->setPosition(project.playhead);
    project.notifyChanged();
    suppressDirty = false;
    dirty = false;
    if (r.newerThanApp) error = "Project was saved by a newer version; some data may be ignored.";
    return true;
}

bool AppContext::saveProject(const juce::File& f, juce::String& error)
{
    captureAllPluginStates();
    collectAudioFiles(f);
    project.playhead = (model::Tick)engine->position();
    std::string e;
    if (!io::ProjectSerializer::saveFile(project, f.getFullPathName().toStdString(), &e)) { error = e; return false; }
    currentFile = f;
    dirty = false;
    sendChangeMessage();
    return true;
}

bool AppContext::importMidi(const juce::File& f, juce::String& error)
{
    model::Project imported;
    auto r = io::MidiFile::readFile(f.getFullPathName().toStdString(), imported, true);
    if (!r.ok) { error = r.error; return false; }
    addImportedTracks(imported);
    return true;
}

namespace {
// .mxl is a zip; META-INF/container.xml names the score, otherwise take the first .musicxml/.xml.
bool readMxl(const juce::File& f, std::string& xml, juce::String& error)
{
    juce::ZipFile zip(f);
    if (zip.getNumEntries() == 0) { error = "Not a valid .mxl (zip) file"; return false; }
    auto readEntry = [&](const juce::String& name, juce::String& out) {
        const int i = zip.getIndexOfFileName(name);
        if (i < 0) return false;
        std::unique_ptr<juce::InputStream> in(zip.createStreamForEntry(i));
        if (!in) return false;
        juce::MemoryBlock mb;
        in->readIntoMemoryBlock(mb);
        out = juce::String::fromUTF8((const char*)mb.getData(), (int)mb.getSize());
        return true;
    };
    juce::String rootPath, container;
    if (readEntry("META-INF/container.xml", container))
        if (auto doc = juce::parseXML(container))
            if (auto* rf = doc->getChildByName("rootfiles"))
                if (auto* first = rf->getChildByName("rootfile")) rootPath = first->getStringAttribute("full-path");
    if (rootPath.isEmpty())
        for (int i = 0; i < zip.getNumEntries(); ++i) {
            const auto name = zip.getEntry(i)->filename;
            if (!name.startsWith("META-INF") && (name.endsWithIgnoreCase(".musicxml") || name.endsWithIgnoreCase(".xml"))) { rootPath = name; break; }
        }
    juce::String content;
    if (rootPath.isEmpty() || !readEntry(rootPath, content)) { error = "No score found inside the .mxl file"; return false; }
    xml = content.toStdString();
    return true;
}
} // namespace

bool AppContext::importMusicXml(const juce::File& f, juce::String& error)
{
    std::string xml;
    if (f.hasFileExtension("mxl")) {
        if (!readMxl(f, xml, error)) return false;
    } else {
        juce::MemoryBlock mb;
        if (!f.loadFileAsData(mb)) { error = "Cannot read " + f.getFullPathName(); return false; }
        xml.assign((const char*)mb.getData(), mb.getSize());
    }
    model::Project imported;
    auto r = io::MusicXml::read(xml, imported, true);
    if (!r.ok) { error = r.error; return false; }
    addImportedTracks(imported);
    if (r.hasKey) setHarmony(r.harmony.root, r.harmony.scaleId, project.harmony.chordMode);
    return true;
}

void AppContext::openSoundsFolder()
{
    const auto folder = plugins->userSoundsFolder();
    folder.createDirectory();
    folder.startAsProcess();
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Add your own sounds",
        "Copy SoundFont files (.sf2) into the folder that just opened:\n\n" + folder.getFullPathName() +
        "\n\nThen choose Options > Rescan plugins. The sounds appear under SoundFonts in each track's instrument menu.");
}

void AppContext::openManual()
{
    // Same places as the bundled SoundFont: app bundle Resources (macOS),
    // next to the executable (Windows/Linux tarball), /usr/share/doc (Debian).
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    const auto app = juce::File::getSpecialLocation(juce::File::currentApplicationFile);
    const juce::String name = "Soundeasel-DAW-Manual.pdf";
    for (auto& f : {app.getChildFile("Contents/Resources").getChildFile(name),
                    exe.getParentDirectory().getChildFile(name),
                    juce::File("/usr/share/doc/soundeasel-daw").getChildFile(name)})
        if (f.existsAsFile() && f.startAsProcess()) return;
    juce::URL("https://github.com/shenphenchoedron-hue/Soundeasel-DAW/releases/latest").launchInDefaultBrowser();
}

void AppContext::showAbout()
{
    juce::String text;
    text << "Soundeasel DAW " << JUCE_APPLICATION_VERSION_STRING << "\n"
         << "Instruments, step sequencers and audio tracks with a mixer.\n\n"
         << "Built-in instruments: GeneralUser GS by S. Christian Collins\n"
         << "https://www.schristiancollins.com/generaluser\n"
         << "Violin: Valiant Violin v2 by Giovanni Valentino with Mike77154 (WTFPL)\n\n"
         << "SoundFont playback: TinySoundFont by Bernhard Schelling (MIT license)\n"
         << "https://github.com/schellingb/TinySoundFont\n\n"
         << "Made with JUCE.\n\n"
         << "Manual: Options > User manual (English). A Danish manual is available on the\n"
         << "Releases page: https://github.com/shenphenchoedron-hue/Soundeasel-DAW/releases";
    if (auto f = plugins::soundfont::builtinFile(); f.existsAsFile())
        text << "\n\nLicense of the built-in sounds:\n" << f.getSiblingFile(plugins::kBuiltinSoundFontLicense).getFullPathName();
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "About Soundeasel DAW", text);
}

void AppContext::addImportedTracks(const model::Project& imported)
{
    // Imported tracks are added through undoable commands.
    project.tempoBpm = imported.tempoBpm;
    project.timeSig = imported.timeSig;
    for (auto& t : imported.tracks()) {
        auto* src = dynamic_cast<model::MidiTrack*>(t.get());
        auto cmd = std::make_unique<model::AddTrackCommand>(src->name);
        auto* raw = cmd.get();
        undo.perform(std::move(cmd));
        if (auto* dst = project.midiTrack(raw->trackId())) {
            dst->channel = src->channel;
            dst->plugin = defaultInstrument();
            std::vector<model::Note> notes(src->notes().begin(), src->notes().end());
            for (auto& n : notes) n.id = 0;
            undo.perform(std::make_unique<model::AddNotesCommand>(dst->id(), notes, "Import"));
        }
    }
}

bool AppContext::exportMidi(const juce::File& f, juce::String& error)
{
    std::string e;
    if (!io::MidiFile::writeFile(project, f.getFullPathName().toStdString(), &e)) { error = e; return false; }
    return true;
}

bool AppContext::exportMusicXml(const juce::File& f, juce::String& error)
{
    std::string e;
    if (!io::MusicXml::writeFile(project, f.getFullPathName().toStdString(), &e)) { error = e; return false; }
    return true;
}

std::unique_ptr<audio::RenderSession> AppContext::createRenderSession(double sampleRate, bool loopOnly, juce::String& error)
{
    auto s = std::make_unique<audio::RenderSession>();
    s->sampleRate = sampleRate;
    s->bpm = project.tempoBpm;
    s->snapshot = std::make_shared<audio::PlaybackSnapshot>();
    s->snapshot->seq = seq::SequenceSnapshot::build(project);
    if (loopOnly) {
        s->start = project.loopStart;
        s->end = project.loopEnd;
        if (s->end <= s->start) { error = "The loop region is empty."; return nullptr; }
    } else {
        s->end = s->snapshot->seq.lengthTicks;
        if (s->end <= 0) { error = "There is nothing to export yet: the song has no notes or audio clips."; return nullptr; }
    }

    // Fresh instances get the settings the live ones have right now.
    captureAllPluginStates();
    constexpr int kBlock = 512;
    s->engine = std::make_unique<audio::AudioEngine>(sampleRate, kBlock);
    auto load = [&](const model::PluginReference& ref, const std::string& track) -> std::unique_ptr<plugins::InstrumentPlugin> {
        juce::String err;
        auto p = plugins->instantiate(ref, sampleRate, kBlock, err);
        if (!p) {
            s->warnings.add(juce::String(track) + ": " + juce::String(ref.name) + " could not be loaded"
                            + (err.isNotEmpty() ? " (" + err + ")" : juce::String()));
            return nullptr;
        }
        p->setNonRealtime(true); // takes effect with the next prepare()
        return p;
    };
    for (auto& t : project.tracks()) {
        std::unique_ptr<plugins::InstrumentPlugin> inst;
        if (auto* mt = dynamic_cast<model::MidiTrack*>(t.get()); mt && !mt->plugin.empty()) {
            const bool audible = std::any_of(s->snapshot->seq.tracks.begin(), s->snapshot->seq.tracks.end(),
                                             [&](const seq::TrackSequence& ts) { return ts.trackId == mt->id() && ts.audible; });
            if (audible && !mt->plugin.bypassed) inst = load(mt->plugin, mt->name); // muted tracks need no instrument
        }
        s->engine->setInstrument(t->id(), std::move(inst)); // prepares it (non-realtime)
        std::vector<plugins::InstrumentPlugin*> fx;
        for (auto& ref : t->inserts) {
            auto p = load(ref, t->name);
            if (p) p->prepare(sampleRate, kBlock); // setInserts() does not prepare
            fx.push_back(p.get()); // nullptr entries are skipped, like in the live engine
            if (p) s->effects.push_back(std::move(p));
        }
        if (!fx.empty()) s->engine->setInserts(t->id(), std::move(fx));
    }

    // Audio clips: decoded on the worker thread; files already in memory at this rate are reused.
    const bool sameRate = std::abs(sampleRate - engine->sampleRate()) < 0.5;
    for (size_t i = 0; i < s->snapshot->seq.tracks.size(); ++i)
        for (auto& c : s->snapshot->seq.tracks[i].clips) {
            s->clips.push_back({i, c});
            if (sameRate && !s->cachedAudio.count(c.file))
                if (auto d = audioFiles.find(c.file, sampleRate)) s->cachedAudio[c.file] = std::move(d);
        }
    return s;
}

// ---------------------------------------------------------------- tracks
void AppContext::addTrack()
{
    auto cmd = std::make_unique<model::AddTrackCommand>();
    auto* raw = cmd.get();
    undo.perform(std::move(cmd));
    if (auto* t = project.midiTrack(raw->trackId()); t && t->plugin.empty()) {
        t->plugin = defaultInstrument();
        project.notifyChanged();
    }
}

// ---------------------------------------------------------------- step sequencer
namespace {
struct DrumDefault { const char* name; int pitch; };
constexpr DrumDefault kDrumDefaults[] = {{"Kick", 36}, {"Snare", 38}, {"Closed Hat", 42}, {"Open Hat", 46},
                                         {"Clap", 39}, {"Low Tom", 45}, {"High Tom", 50}, {"Crash", 49}, {"Ride", 51}};
}

std::uint64_t AppContext::activeStepGroup() const
{
    auto* t = activeMidiTrack();
    return t ? t->stepGroup : 0;
}

std::vector<model::MidiTrack*> AppContext::stepLines(std::uint64_t group) const
{
    std::vector<model::MidiTrack*> v;
    if (group == 0) return v;
    for (auto& t : project.tracks())
        if (auto* m = dynamic_cast<model::MidiTrack*>(t.get()); m && m->stepGroup == group) v.push_back(m);
    return v;
}

void AppContext::addStepSequencer()
{
    std::uint64_t group = 0;
    for (auto& t : project.tracks())
        if (auto* m = dynamic_cast<model::MidiTrack*>(t.get())) group = std::max(group, m->stepGroup);
    addStepLine(group + 1);
}

void AppContext::addStepLine(std::uint64_t group)
{
    if (group == 0) return;
    const auto existing = stepLines(group);
    model::StepPattern pat;
    if (!existing.empty()) {
        const auto& f = existing.front()->step;
        pat.stepTicks = f.stepTicks; pat.barTicks = f.barTicks; pat.bars = f.bars; pat.startTick = f.startTick;
        pat.title = f.title;
    } else {
        pat.barTicks = seq::ticksPerBar(project.timeSig);
        pat.bars = 1;
    }
    const auto& def = kDrumDefaults[existing.size() % std::size(kDrumDefaults)];
    pat.pitch = def.pitch;
    pat.normalise();

    auto cmd = std::make_unique<model::AddTrackCommand>(def.name);
    auto* raw = cmd.get();
    undo.perform(std::move(cmd));
    auto* t = project.midiTrack(raw->trackId());
    if (!t) return;
    t->stepGroup = group;
    t->step = pat;
    // Each line inherits the instrument of the previous line; the first one gets a drum kit.
    if (!existing.empty()) t->plugin = existing.back()->plugin;
    else {
        t->plugin = defaultInstrument();
        for (auto& i : plugins->plugins())
            if (i.identifier == std::string(plugins::soundfont::kBuiltinPrefix) + "pop-drums") { t->plugin = plugins::PluginManager::referenceFor(i); break; }
    }
    t->plugin.stateBase64.clear();
    t->colour = existing.empty() ? 0xffe08f4fu : existing.front()->colour;
    t->regenerateStepNotes();
    project.notifyChanged();
}

std::vector<std::uint64_t> AppContext::stepGroups() const
{
    std::vector<std::uint64_t> v;
    for (auto& t : project.tracks())
        if (auto* m = dynamic_cast<model::MidiTrack*>(t.get()); m && m->isStepLine() && std::find(v.begin(), v.end(), m->stepGroup) == v.end())
            v.push_back(m->stepGroup);
    return v;
}

void AppContext::deleteStepSequencer(std::uint64_t group)
{
    std::vector<std::unique_ptr<model::Command>> cmds;
    for (auto* t : stepLines(group)) cmds.push_back(std::make_unique<model::RemoveTrackCommand>(t->id()));
    if (!cmds.empty()) undo.perform(std::make_unique<model::CompoundCommand>(std::move(cmds), "Delete step sequencer"));
}

void AppContext::previewOnTrack(model::TrackId track, int pitch, int velocity, int ms)
{
    auto* t = project.midiTrack(track);
    if (!t) return;
    const int ch = t->channel + 1;
    engine->pushLive(track, juce::MidiMessage::noteOn(ch, pitch, (juce::uint8)velocity));
    auto* eng = engine.get();
    juce::Timer::callAfterDelay(ms, [eng, track, ch, pitch] { eng->pushLive(track, juce::MidiMessage::noteOff(ch, pitch)); });
}

void AppContext::setStepPatterns(std::vector<model::SetStepPatternsCommand::Entry> after, const std::string& name)
{
    std::vector<model::SetStepPatternsCommand::Entry> before;
    for (auto& [id, pat] : after) {
        auto* t = project.midiTrack(id);
        if (!t) return;
        before.push_back({id, t->step});
        pat.normalise();
    }
    if (before == after) return;
    undo.perform(std::make_unique<model::SetStepPatternsCommand>(std::move(before), std::move(after), name));
}

void AppContext::deleteTrack(model::TrackId id)
{
    if (project.track(id)) undo.perform(std::make_unique<model::RemoveTrackCommand>(id));
}

void AppContext::selectTrack(model::TrackId id)
{
    if (project.activeTrack == id) return;
    if (auto* t = project.midiTrack(id); !t || t->isStepLine()) return; // only piano roll tracks are edited in the piano roll
    for (auto& e : chordInput.allNotesOff()) engine->pushLive(activeTrackAtomic, juce::MidiMessage::noteOff(activeChannelAtomic + 1, e.pitch));
    project.activeTrack = id;
    suppressDirty = true;
    project.notifyChanged();
    suppressDirty = false;
}

void AppContext::setTrackPlugin(model::TrackId id, const plugins::PluginInfo* info)
{
    auto* t = project.track(id);
    if (!t) return;
    auto before = model::TrackProperties::from(*t);
    auto after = before;
    after.plugin = info ? plugins::PluginManager::referenceFor(*info) : model::PluginReference{};
    undo.perform(std::make_unique<model::SetTrackPropertiesCommand>(id, before, after));
}

void AppContext::changeTrack(model::TrackId id, const std::function<void(model::TrackProperties&)>& edit)
{
    auto* t = project.track(id);
    if (!t) return;
    auto before = model::TrackProperties::from(*t);
    auto after = before;
    edit(after);
    undo.perform(std::make_unique<model::SetTrackPropertiesCommand>(id, before, after));
}

void AppContext::beginTrackEdit(model::TrackId id)
{
    if (auto* t = project.track(id); t && !trackEdits.count(id)) trackEdits.emplace(id, model::TrackProperties::from(*t));
}

void AppContext::liveTrackVolume(model::TrackId id, float gain)
{
    if (auto* t = project.track(id)) { t->volume = std::clamp(gain, 0.0f, 2.0f); modelChangedWithoutUndo(); }
}

void AppContext::liveTrackPan(model::TrackId id, float pan)
{
    if (auto* t = project.track(id)) { t->pan = std::clamp(pan, -1.0f, 1.0f); modelChangedWithoutUndo(); }
}

void AppContext::endTrackEdit(model::TrackId id)
{
    auto it = trackEdits.find(id);
    if (it == trackEdits.end()) return;
    const auto before = it->second;
    trackEdits.erase(it);
    auto* t = project.track(id);
    if (!t || (t->volume == before.volume && t->pan == before.pan)) return; // nothing changed: no undo step
    auto after = before;
    after.volume = t->volume;
    after.pan = t->pan;
    t->volume = before.volume;
    t->pan = before.pan;
    undo.perform(std::make_unique<model::SetTrackPropertiesCommand>(id, before, after));
}

// ---------------------------------------------------------------- copy & paste tracks
std::vector<std::unique_ptr<model::Track>> AppContext::snapshotTracks(model::TrackId id)
{
    std::vector<std::unique_ptr<model::Track>> v;
    auto* t = project.track(id);
    if (!t) return v;
    std::vector<model::Track*> sources;
    if (auto* m = dynamic_cast<model::MidiTrack*>(t); m && m->isStepLine())
        for (auto* line : stepLines(m->stepGroup)) sources.push_back(line); // the whole step sequencer
    else
        sources.push_back(t);
    for (auto* s : sources) {
        captureTrackPluginStates(*s); // the copy gets the plugins' settings as they are right now
        v.push_back(s->clone());
    }
    return v;
}

void AppContext::copyTrack(model::TrackId id)
{
    auto snap = snapshotTracks(id);
    if (snap.empty()) return;
    trackClipboard = std::move(snap);
    sendChangeMessage(); // menus and buttons that offer "Paste track"
}

void AppContext::duplicateTrack(model::TrackId id)
{
    pasteTracks(snapshotTracks(id), id); // the clipboard is left alone
}

juce::String AppContext::copiedTrackName() const
{
    if (trackClipboard.empty()) return {};
    const auto& first = *trackClipboard.front();
    if (auto* m = dynamic_cast<const model::MidiTrack*>(&first); m && m->isStepLine()) {
        const juce::String title = m->step.title.empty() ? juce::String("Step sequencer") : juce::String(m->step.title);
        return title + " (" + juce::String((int)trackClipboard.size()) + (trackClipboard.size() == 1 ? " line)" : " lines)");
    }
    return juce::String(first.name);
}

void AppContext::pasteTrack(model::TrackId after) { pasteTracks(trackClipboard, after); }

void AppContext::pasteTracks(const std::vector<std::unique_ptr<model::Track>>& sources, model::TrackId after)
{
    if (sources.empty()) return;
    int index = -1; // end
    if (auto* t = project.track(after)) {
        index = project.indexOf(after) + 1;
        if (auto* m = dynamic_cast<model::MidiTrack*>(t); m && m->isStepLine())
            for (auto* line : stepLines(m->stepGroup)) index = std::max(index, project.indexOf(line->id()) + 1);
    }
    auto nameTaken = [&](const std::string& n) {
        for (auto& t : project.tracks()) if (t->name == n) return true;
        return false;
    };
    const bool stepSeq = [&] { auto* m = dynamic_cast<const model::MidiTrack*>(sources.front().get()); return m && m->isStepLine(); }();
    std::vector<std::unique_ptr<model::Track>> copies;
    for (auto& src : sources) {
        auto c = src->clone();
        if (auto* m = dynamic_cast<model::MidiTrack*>(c.get()); m && m->isStepLine()) {
            // Lines keep their names (Kick, Snare, ...); a named step sequencer is marked as a copy.
            if (!m->step.title.empty()) m->step.title += " (copy)";
        } else {
            std::string n = c->name + " (copy)";
            for (int i = 2; nameTaken(n); ++i) n = c->name + " (copy " + std::to_string(i) + ")";
            c->name = n;
        }
        copies.push_back(std::move(c));
    }
    undo.perform(std::make_unique<model::PasteTracksCommand>(std::move(copies), index, stepSeq ? "Paste step sequencer" : "Paste track"));
}

// ---------------------------------------------------------------- Human Touch
int AppContext::humanizeTrack(model::TrackId id, const model::HumanizeSettings& s)
{
    auto* t = project.midiTrack(id);
    if (!t || t->isStepLine()) return 0; // step sequencer notes are generated from the pattern
    const auto seed = (std::uint32_t)juce::Random::getSystemRandom().nextInt();
    auto after = model::humanizeNotes(t->notes(), s, seed);
    if (after.empty()) return 0;
    std::vector<model::Note> before;
    before.reserve(after.size());
    std::unordered_map<model::NoteId, const model::Note*> byId;
    for (auto& n : t->notes()) byId.emplace(n.id, &n);
    for (auto& n : after) before.push_back(*byId.at(n.id));
    const int count = (int)after.size();
    undo.perform(std::make_unique<model::ModifyNotesCommand>(id, std::move(before), std::move(after), "Human Touch"));
    return count;
}

// ---------------------------------------------------------------- audio tracks
std::vector<model::AudioTrack*> AppContext::audioTracks() const
{
    std::vector<model::AudioTrack*> v;
    for (auto& t : project.tracks())
        if (auto* a = dynamic_cast<model::AudioTrack*>(t.get())) v.push_back(a);
    return v;
}

model::TrackId AppContext::addAudioTrack(const std::string& name)
{
    auto cmd = std::make_unique<model::AddTrackCommand>(name, model::TrackKind::Audio);
    auto* raw = cmd.get();
    undo.perform(std::move(cmd));
    if (auto* a = project.audioTrack(raw->trackId())) {
        // A fresh track listens to the first input (if the device has one), so pressing
        // record just works. The colour sets audio tracks apart from instrument tracks.
        a->input = engine->numInputChannels() > 0 ? 0 : -1;
        a->colour = 0xff5aae84u;
        a->volume = 0.8f;
        project.notifyChanged();
    }
    return raw->trackId();
}

bool AppContext::importAudio(const juce::File& f, model::TrackId track, model::Tick at, juce::String& error)
{
    if (!audioFiles.canRead(f)) { error = "Cannot read \"" + f.getFileName() + "\" (supported: WAV, AIFF, FLAC, Ogg Vorbis, MP3)."; return false; }
    const double len = audioFiles.fileLength(f);
    if (len <= 0) { error = "\"" + f.getFileName() + "\" contains no audio."; return false; }
    if (!project.audioTrack(track)) track = addAudioTrack(f.getFileNameWithoutExtension().toStdString());
    auto* a = project.audioTrack(track);
    if (!a) return false;
    auto clips = a->clips();
    model::AudioClip c;
    c.file = f.getFullPathName().toStdString();
    c.name = f.getFileNameWithoutExtension().toStdString();
    c.start = std::max<model::Tick>(0, at);
    c.length = len;
    clips.push_back(c);
    setAudioClips(track, std::move(clips), "Import audio");
    return true;
}

void AppContext::setAudioClips(model::TrackId id, std::vector<model::AudioClip> after, const std::string& name)
{
    auto* a = project.audioTrack(id);
    if (!a) return;
    auto before = a->clips();
    if (before == after) return;
    undo.perform(std::make_unique<model::SetAudioClipsCommand>(id, std::move(before), std::move(after), name));
}

void AppContext::setTrackArmed(model::TrackId id, bool on)
{
    if (auto* a = project.audioTrack(id); a && a->armed != on) {
        a->armed = on;
        if (on && a->input < 0 && engine->numInputChannels() > 0) a->input = 0;
        project.notifyChanged();
    }
}

void AppContext::setTrackMonitor(model::TrackId id, bool on)
{
    if (auto* a = project.audioTrack(id); a && a->monitor != on) { a->monitor = on; project.notifyChanged(); }
}

void AppContext::setVolumeAutomation(model::TrackId id, std::vector<model::AutomationPoint> after)
{
    auto* a = project.audioTrack(id);
    if (!a) return;
    model::normalizeAutomation(after);
    if (after == a->volumeAutomation) return;
    undo.perform(std::make_unique<model::SetVolumeAutomationCommand>(id, a->volumeAutomation, std::move(after)));
}

void AppContext::setSustain(model::TrackId id, std::vector<model::AutomationPoint> after)
{
    auto* t = project.midiTrack(id);
    if (!t) return;
    model::normalizeAutomation(after, 1.0f);
    if (after == t->sustain) return;
    undo.perform(std::make_unique<model::SetSustainCommand>(id, t->sustain, std::move(after)));
}

void AppContext::setShowVolumeLine(model::TrackId id, bool on)
{
    if (auto* a = project.audioTrack(id); a && a->showVolumeLine != on) { a->showVolumeLine = on; project.notifyChanged(); }
}

void AppContext::setTrackInput(model::TrackId id, int input, bool stereo)
{
    changeTrack(id, [&](model::TrackProperties& p) { p.input = input; p.stereoInput = stereo; });
}

juce::StringArray AppContext::inputNames() const
{
    juce::StringArray names;
    if (auto* d = deviceManager.getCurrentAudioDevice()) {
        const auto all = d->getInputChannelNames();
        const auto active = d->getActiveInputChannels();
        for (int i = 0; i < all.size(); ++i)
            if (active[i]) names.add(all[i]);
    }
    return names;
}

std::shared_ptr<const audio::AudioFileData> AppContext::audioFile(const std::string& path)
{
    // Never blocks: files are decoded in the background (the lanes show "Loading...").
    auto d = audioFiles.find(path, engine->sampleRate());
    if (!d) audioFiles.request(path, engine->sampleRate());
    return d;
}

juce::File AppContext::audioFolder() const
{
    if (currentFile != juce::File()) return currentFile.getSiblingFile(currentFile.getFileNameWithoutExtension() + " Audio");
    return settings.dataDirectory().getChildFile("Recordings");
}

bool AppContext::startAudioRecording()
{
    const int inputs = engine->numInputChannels();
    if (inputs == 0) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "No audio input",
            "Your sound device has no active input, so audio cannot be recorded.\n\n"
            "Open Options > Settings and choose a device (or input channels) for audio input.");
        return false;
    }
    const auto folder = audioFolder();
    if (!folder.createDirectory()) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Cannot record", "Cannot create the folder\n" + folder.getFullPathName());
        return false;
    }
    takes.clear();
    std::vector<std::pair<model::TrackId, std::unique_ptr<audio::AudioEngine::Writer>>> writers;
    juce::WavAudioFormat wav;
    const double sr = engine->sampleRate();
    for (auto* a : audioTracks()) {
        if (!a->armed) continue;
        if (a->input < 0 || a->input >= inputs) a->input = 0;
        const int channels = a->stereoInput && a->input + 1 < inputs ? 2 : 1;
        const auto file = folder.getNonexistentChildFile(juce::File::createLegalFileName(a->name) + " take", ".wav", false);
        std::unique_ptr<juce::OutputStream> os(file.createOutputStream());
        if (!os) continue;
#if JUCE_VERSION >= 0x80009
        auto w = wav.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(channels).withBitsPerSample(24));
        if (!w) continue;
        auto* writer = w.release();
#else
        auto* writer = wav.createWriterFor(os.get(), sr, (unsigned)channels, 24, {}, 0);
        if (!writer) continue;
        os.release(); // owned by the writer
#endif
        writers.push_back({a->id(), std::make_unique<audio::AudioEngine::Writer>(writer, diskThread, (int)sr * 4)});
        takes.push_back({a->id(), file});
    }
    if (writers.empty()) return false;
    engine->startAudioRecording(std::move(writers));
    return true;
}

void AppContext::finishAudioRecording(std::vector<std::unique_ptr<model::Command>>& cmds)
{
    const double startPos = engine->stopAudioRecording(); // files are complete after this
    const double sr = engine->sampleRate();
    const double latency = sr > 0 ? engine->roundTripLatency() / sr : 0.0;
    for (auto& take : takes) {
        auto* a = project.audioTrack(take.track);
        const double len = audioFiles.fileLength(take.file);
        if (!a || startPos < 0 || len <= latency + 0.01) { take.file.deleteFile(); continue; }
        auto before = a->clips();
        auto after = before;
        model::AudioClip c;
        c.file = take.file.getFullPathName().toStdString();
        c.name = take.file.getFileNameWithoutExtension().toStdString();
        c.start = (model::Tick)std::llround(startPos);
        // What was recorded arrives late by the device latency: skip that part.
        c.offset = latency;
        c.length = len - latency;
        after.push_back(c);
        audioFiles.forget(c.file);
        cmds.push_back(std::make_unique<model::SetAudioClipsCommand>(take.track, std::move(before), std::move(after), "Record audio"));
    }
    takes.clear();
}

// Copies every audio file the project uses into "<project> Audio" next to the
// project file, so the project folder can be moved or backed up as a whole.
void AppContext::collectAudioFiles(const juce::File& projectFile)
{
    const auto folder = projectFile.getSiblingFile(projectFile.getFileNameWithoutExtension() + " Audio");
    std::map<std::string, std::string> moved;
    bool changed = false;
    for (auto* a : audioTracks()) {
        auto clips = a->clips();
        bool trackChanged = false;
        for (auto& c : clips) {
            const juce::File src(juce::String::fromUTF8(c.file.c_str()));
            if (src.isAChildOf(folder) || !src.existsAsFile()) continue;
            // Only real, readable audio files are copied: a project file could name any file on the disk.
            if (!audioFiles.canRead(src) || audioFiles.fileLength(src) <= 0.0) continue;
            auto it = moved.find(c.file);
            if (it == moved.end()) {
                folder.createDirectory();
                auto dst = folder.getChildFile(src.getFileName());
                // An existing file is reused only if it is the very same audio (same name and size is not enough).
                if (dst.existsAsFile() && !dst.hasIdenticalContentTo(src))
                    dst = folder.getNonexistentChildFile(src.getFileNameWithoutExtension(), src.getFileExtension(), false);
                if (!dst.existsAsFile() && !src.copyFileTo(dst)) continue;
                it = moved.emplace(c.file, dst.getFullPathName().toStdString()).first;
            }
            c.file = it->second;
            trackChanged = true;
        }
        if (trackChanged) { a->setClips(std::move(clips)); changed = true; }
    }
    if (changed) { suppressDirty = true; project.notifyChanged(); suppressDirty = false; }
}

// ---------------------------------------------------------------- Mains
void AppContext::setMaster(const model::MasterBus& before, const model::MasterBus& after)
{
    if (before == after) return;
    project.master = before;
    undo.perform(std::make_unique<model::SetMasterCommand>(before, after));
}

void AppContext::setMasterLive(const model::MasterBus& m)
{
    project.master = m;
    project.notifyChanged();
}

// ---------------------------------------------------------------- meters
void AppContext::pollLevels()
{
    lastLevels.clear();
    for (auto& t : project.tracks()) lastLevels[t->id()] = engine->takeTrackLevels(t->id());
    lastLevels[0] = engine->takeMasterLevels();
}

audio::AudioEngine::Levels AppContext::levels(model::TrackId id) const
{
    auto it = lastLevels.find(id);
    return it == lastLevels.end() ? audio::AudioEngine::Levels{} : it->second;
}

// ---------------------------------------------------------------- device status
bool AppContext::audioOutputOk() const
{
    return deviceManager.getCurrentAudioDevice() != nullptr && engine->isDeviceRunning() && engine->numOutputChannels() > 0;
}

// Opens a stereo pair for Mains if the device has two or more outputs but fewer are active
// (e.g. only one output channel ticked in the settings).
void AppContext::ensureStereoOutput()
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (!d || changingDevice) return;
    const int total = d->getOutputChannelNames().size();
    const auto active = d->getActiveOutputChannels();
    if (total < 2 || active.countNumberOfSetBits() >= 2) return;
    const int first = std::max(0, active.findNextSetBit(0));
    setMainsOutputPair(std::min(first / 2, total / 2 - 1), false);
}

juce::StringArray AppContext::mainsOutputPairs() const
{
    juce::StringArray pairs;
    if (auto* d = deviceManager.getCurrentAudioDevice()) {
        const int outs = d->getOutputChannelNames().size();
        for (int i = 0; i + 1 < outs; i += 2) {
            pairs.add("Out " + juce::String(i + 1) + "+" + juce::String(i + 2));
        }
    }
    return pairs;
}

int AppContext::mainsOutputPair() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (!d) return -1;
    const int first = d->getActiveOutputChannels().findNextSetBit(0);
    return first < 0 ? -1 : first / 2;
}

void AppContext::setMainsOutputPair(int pair, bool userChoice)
{
    if (userChoice) deviceFellBack = false;
    auto setup = deviceManager.getAudioDeviceSetup();
    setup.useDefaultOutputChannels = false;
    setup.outputChannels.clear();
    setup.outputChannels.setRange(pair * 2, 2, true);
    changingDevice = true;
    const auto err = deviceManager.setAudioDeviceSetup(setup, true);
    changingDevice = false;
    if (err.isNotEmpty()) { DBG("Mains output: " << err); }
    if (!deviceFellBack) // never replace the user's saved device with a fallback setup
        if (auto xml = deviceManager.createStateXml()) settings.setAudioDeviceState(xml.get());
    sendChangeMessage();
}

juce::String AppContext::audioStatusText() const
{
    auto* d = deviceManager.getCurrentAudioDevice();
    if (!audioOutputOk() || !d) return "No sound - click to choose your sound device";
    juce::String s = d->getName() + "  " + juce::String(d->getCurrentSampleRate() / 1000.0, 1) + " kHz  |  "
                   + (isStereoOutput() ? "stereo out" : "MONO out");
    const int in = engine->numInputChannels();
    s << "  |  " << (in > 0 ? juce::String(in) + (in == 1 ? " input" : " inputs") : juce::String("no inputs"));
    return s;
}

void AppContext::setTrackBypass(model::TrackId id, bool b)
{
    auto* t = project.track(id);
    if (!t) return;
    auto before = model::TrackProperties::from(*t);
    auto after = before;
    after.plugin.bypassed = b;
    undo.perform(std::make_unique<model::SetTrackPropertiesCommand>(id, before, after));
}

void AppContext::closeEditor(model::TrackId id) { editorWindows.erase(id); }

void AppContext::openPluginEditor(model::TrackId id)
{
    if (auto it = editorWindows.find(id); it != editorWindows.end()) { it->second->toFront(true); return; }
    auto* inst = engine->instrument(id);
    if (!inst || !inst->hasEditor()) return;
    if (auto* ed = inst->createEditor()) {
        auto* t = project.track(id);
        editorWindows[id] = std::make_unique<PluginEditorWindow>(
            juce::String(t ? t->name : "") + " - " + inst->name(), ed, [this, id] { closeEditor(id); });
    }
}

// ---------------------------------------------------------------- effect plugins
// Keeps the live plugin instances of a track's insert chain in step with the model.
// Instances are matched by their slot id (not just the plugin type, so two copies of
// the same plugin keep their own settings) and reused when plugins are moved.
void AppContext::syncInserts(model::Track& t)
{
    model::assignInsertIds(t.inserts);
    auto& cur = inserts[t.id()];
    const auto& want = t.inserts;
    auto matches = [](const model::PluginReference& a, const model::PluginReference& b) { return a.uid == b.uid && a.sameType(b); };
    bool same = cur.size() == want.size();
    for (size_t i = 0; same && i < want.size(); ++i) same = matches(cur[i].ref, want[i]);
    if (same) {
        for (size_t i = 0; i < want.size(); ++i) cur[i].ref = want[i];
        return;
    }
    std::vector<InsertInstance> next;
    std::vector<bool> used(cur.size(), false);
    for (auto& r : want) {
        InsertInstance inst;
        inst.ref = r;
        for (size_t j = 0; j < cur.size(); ++j)
            if (!used[j] && matches(cur[j].ref, r)) { used[j] = true; inst.plugin = std::move(cur[j].plugin); inst.error = cur[j].error; break; }
        if (!inst.plugin && inst.error.isEmpty()) {
            juce::String err;
            inst.plugin = plugins->instantiate(r, engine->sampleRate(), engine->blockSize(), err);
            if (!inst.plugin) inst.error = err.isNotEmpty() ? err : juce::String("Could not load the plugin");
        }
        next.push_back(std::move(inst));
    }
    for (size_t j = 0; j < cur.size(); ++j)
        if (!used[j] && cur[j].plugin) cur[j].plugin->prepareToRemove(); // still in the engine: stops on the audio thread
    std::vector<plugins::InstrumentPlugin*> ptrs;
    for (auto& i : next) ptrs.push_back(i.plugin.get());
    engine->setInserts(t.id(), std::move(ptrs)); // the audio thread no longer sees removed plugins
    for (size_t j = 0; j < cur.size(); ++j)
        if (!used[j] && cur[j].plugin) insertEditors.erase(cur[j].plugin.get()); // close its window first
    cur = std::move(next); // removed plugins are destroyed here, on the message thread
}

void AppContext::prepareToRemove(model::TrackId id)
{
    if (auto* inst = engine->instrument(id)) inst->prepareToRemove();
    if (auto it = inserts.find(id); it != inserts.end())
        for (auto& i : it->second) if (i.plugin) i.plugin->prepareToRemove();
}

void AppContext::clearInserts(model::TrackId id)
{
    auto it = inserts.find(id);
    if (it == inserts.end()) return;
    for (auto& i : it->second) if (i.plugin) insertEditors.erase(i.plugin.get());
    inserts.erase(it);
}

void AppContext::captureInsertStates(model::Track& t)
{
    auto it = inserts.find(t.id());
    if (it == inserts.end()) return;
    for (size_t i = 0; i < it->second.size() && i < t.inserts.size(); ++i)
        if (auto* p = it->second[i].plugin.get()) {
            const auto state = p->getState();
            t.inserts[i].stateBase64 = state.getSize() ? state.toBase64Encoding().toStdString() : std::string();
        }
}

void AppContext::editInserts(model::TrackId id, const std::function<void(std::vector<model::PluginReference>&)>& f)
{
    auto* t = project.track(id);
    if (!t) return;
    captureInsertStates(*t); // so undo/redo bring plugins back with their current settings
    changeTrack(id, [&](model::TrackProperties& p) { f(p.inserts); model::assignInsertIds(p.inserts); });
}

void AppContext::addInsert(model::TrackId id, const plugins::PluginInfo& info)
{
    editInserts(id, [&](auto& v) { v.push_back(plugins::PluginManager::referenceFor(info)); });
}

void AppContext::replaceInsert(model::TrackId id, int index, const plugins::PluginInfo& info)
{
    // the replacement gets a new slot id (uid 0 -> assigned), so it is a new instance even for the same plugin
    editInserts(id, [&](auto& v) { if (index >= 0 && index < (int)v.size()) v[(size_t)index] = plugins::PluginManager::referenceFor(info); });
}

void AppContext::removeInsert(model::TrackId id, int index)
{
    editInserts(id, [&](auto& v) { if (index >= 0 && index < (int)v.size()) v.erase(v.begin() + index); });
}

void AppContext::moveInsert(model::TrackId id, int from, int to)
{
    editInserts(id, [&](auto& v) {
        if (from < 0 || from >= (int)v.size() || to < 0 || to >= (int)v.size() || from == to) return;
        auto r = v[(size_t)from];
        v.erase(v.begin() + from);
        v.insert(v.begin() + to, r);
    });
}

void AppContext::setInsertBypass(model::TrackId id, int index, bool b)
{
    editInserts(id, [&](auto& v) { if (index >= 0 && index < (int)v.size()) v[(size_t)index].bypassed = b; });
}

void AppContext::setInsertsAfterBuiltins(model::TrackId id, bool after)
{
    changeTrack(id, [&](model::TrackProperties& p) { p.insertsAfterBuiltins = after; });
}

juce::String AppContext::insertError(model::TrackId id, int index) const
{
    auto it = inserts.find(id);
    if (it == inserts.end() || index < 0 || index >= (int)it->second.size()) return {};
    return it->second[(size_t)index].error;
}

void AppContext::openInsertEditor(model::TrackId id, int index)
{
    auto it = inserts.find(id);
    if (it == inserts.end() || index < 0 || index >= (int)it->second.size()) return;
    auto* p = it->second[(size_t)index].plugin.get();
    if (!p) return;
    if (auto w = insertEditors.find(p); w != insertEditors.end()) { w->second->toFront(true); return; }
    if (auto* ed = p->createEditor()) {
        auto* t = project.track(id);
        insertEditors[p] = std::make_unique<PluginEditorWindow>(juce::String(t ? t->name : "") + " - " + p->name(), ed,
                                                                [this, p] { insertEditors.erase(p); });
    }
}

// ---------------------------------------------------------------- transport
void AppContext::play() { engine->play(); sendChangeMessage(); }

void AppContext::stop()
{
    if (!engine) return;
    const bool wasPlaying = engine->isPlaying();
    engine->stop();
    if (isRecording()) {
        std::vector<std::unique_ptr<model::Command>> cmds;
        if (recorder.isRecording()) {
            auto notes = recorder.finish((model::Tick)engine->position());
            if (!notes.empty() && project.midiTrack(project.activeTrack))
                cmds.push_back(std::make_unique<model::AddNotesCommand>(project.activeTrack, notes, "Record"));
        }
        if (engine->isRecordingAudio()) finishAudioRecording(cmds);
        if (cmds.size() == 1) undo.perform(std::move(cmds.front()));
        else if (!cmds.empty()) undo.perform(std::make_unique<model::CompoundCommand>(std::move(cmds), "Record"));
    } else if (!wasPlaying) {
        engine->setPosition(0); // second Stop returns to start
    }
    sendChangeMessage();
}

void AppContext::toggleRecord()
{
    if (isRecording()) { stop(); return; }
    bool anyArmed = false;
    for (auto* a : audioTracks()) anyArmed = anyArmed || a->armed;
    if (!activeMidiTrack() && !anyArmed) {
        juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Nothing to record",
            "Add a piano roll track to record MIDI, or add an audio track and press its record button (R) to record audio.");
        return;
    }
    if (anyArmed && !startAudioRecording()) return;
    recorder.start(project.loopStart, project.loopEnd, project.loopEnabled);
    if (countIn > 0 && !engine->isPlaying())
        engine->playWithCountIn((model::Tick)countIn * seq::ticksPerBar(project.timeSig));
    else
        engine->play();
    sendChangeMessage();
}

void AppContext::setMetronome(bool on)
{
    metronomeOn = on;
    engine->setMetronome(on);
    settings.setMetronome(on);
    sendChangeMessage();
}

void AppContext::setMetronomeLevel(float v)
{
    metronomeLvl = juce::jlimit(0.0f, 1.0f, v);
    engine->setMetronomeLevel(metronomeLvl);
    settings.setMetronomeLevel(metronomeLvl);
}

void AppContext::setCountInBars(int n)
{
    countIn = juce::jlimit(0, 8, n);
    settings.setCountInBars(countIn);
    sendChangeMessage();
}

void AppContext::returnToStart() { engine->setPosition(project.loopEnabled ? project.loopStart : 0); sendChangeMessage(); }
void AppContext::setPosition(model::Tick t) { engine->setPosition(t); sendChangeMessage(); }

void AppContext::setTempo(double bpm)
{
    project.tempoBpm = juce::jlimit(10.0, 999.0, bpm);
    project.notifyChanged();
}

void AppContext::setTimeSignature(int num, int den)
{
    project.timeSig = {juce::jlimit(1, 32, num), den};
    project.notifyChanged();
}

void AppContext::setLoop(bool enabled, model::Tick start, model::Tick end)
{
    project.loopEnabled = enabled;
    project.loopStart = std::max<model::Tick>(0, start);
    project.loopEnd = std::max(project.loopStart + 1, end);
    project.notifyChanged();
}

// ---------------------------------------------------------------- harmony
void AppContext::setHarmony(int root, const std::string& scaleId, bool chordMode)
{
    {
        std::lock_guard l(chordMutex);
        if (chordInput.chordMode() != chordMode)
            for (auto& e : chordInput.allNotesOff()) engine->pushLive(activeTrackAtomic, juce::MidiMessage::noteOff(activeChannelAtomic + 1, e.pitch));
        chordInput.setScale(root, scaleId);
        chordInput.setChordMode(chordMode);
    }
    project.harmony = {root, scaleId, chordMode};
    project.notifyChanged();
}

void AppContext::setModifierLatched(input::Modifier m, bool on)
{
    { std::lock_guard l(chordMutex); chordInput.setLatched(m, on); }
    sendChangeMessage();
}

bool AppContext::isModifierLatched(input::Modifier m) const
{
    std::lock_guard l(chordMutex);
    return chordInput.isLatched(m);
}

theory::ChordRequest AppContext::chordRequestFor(int pitch) const
{
    std::lock_guard l(chordMutex);
    return chordInput.requestFor(pitch);
}

input::ChordDisplayState AppContext::chordDisplay() const
{
    std::lock_guard l(chordMutex);
    return chordInput.displayState();
}

void AppContext::beginMidiLearn(input::Modifier m)
{
    { std::lock_guard l(chordMutex); chordInput.beginLearn(m); }
    sendChangeMessage();
}

std::optional<input::Modifier> AppContext::learningModifier() const
{
    std::lock_guard l(chordMutex);
    return chordInput.learning();
}

void AppContext::setControllerMapping(const input::ControllerMapping& m)
{
    { std::lock_guard l(chordMutex); chordInput.setMapping(m); }
    settings.setControllerMapping(m);
    project.controllerMapping = m;
    sendChangeMessage();
}

// ---------------------------------------------------------------- live input
void AppContext::sendLive(const std::vector<input::OutputEvent>& events)
{
    const auto track = activeTrackAtomic.load();
    const int ch = activeChannelAtomic.load() + 1;
    const auto now = (model::Tick)engine->position();
    bool rec = recorder.isRecording();
    // During the count-in only notes played just before the downbeat are recorded (at the start).
    if (rec && engine->isCountingIn() && engine->countInRemaining() > (double)engine->beatTicks() / 2) rec = false;
    for (auto& e : events) {
        engine->pushLive(track, e.noteOn ? juce::MidiMessage::noteOn(ch, e.pitch, (juce::uint8)e.velocity)
                                         : juce::MidiMessage::noteOff(ch, e.pitch));
        if (rec) { if (e.noteOn) recorder.noteOn(e.pitch, e.velocity, now); else recorder.noteOff(e.pitch, now); }
    }
}

void AppContext::handleKeyboardNote(bool on, int pitch, int velocity)
{
    std::vector<input::OutputEvent> out;
    std::optional<input::Modifier> learned;
    input::ControllerMapping mapping;
    {
        std::lock_guard l(chordMutex);
        out = on ? chordInput.noteOn(pitch, velocity) : chordInput.noteOff(pitch);
        learned = chordInput.takeLearnedModifier();
        mapping = chordInput.mapping();
    }
    sendLive(out);
    if (learned)
        juce::MessageManager::callAsync([this, mapping] { setControllerMapping(mapping); });
    // The chord display is refreshed by MainComponent's 30 Hz timer. Broadcasting a
    // change per note would rebuild the menu bar, track list and piano roll on
    // every key press, which makes playing laggy with heavy plugins open.
}

void AppContext::handleMidi(const juce::MidiMessage& m)
{
    if (m.isNoteOn()) handleKeyboardNote(true, m.getNoteNumber(), m.getVelocity());
    else if (m.isNoteOff()) handleKeyboardNote(false, m.getNoteNumber(), 0);
    else if (m.isController() || m.isPitchWheel() || m.isChannelPressure() || m.isAftertouch()) {
        auto copy = m;
        copy.setChannel(activeChannelAtomic.load() + 1);
        engine->pushLive(activeTrackAtomic, copy);
    }
}

void AppContext::previewNotes(const std::vector<int>& pitches, int velocity, int ms)
{
    const auto track = activeTrackAtomic.load();
    const int ch = activeChannelAtomic.load() + 1;
    for (int p : pitches) engine->pushLive(track, juce::MidiMessage::noteOn(ch, p, (juce::uint8)velocity));
    auto* eng = engine.get();
    juce::Timer::callAfterDelay(ms, [eng, track, ch, pitches] {
        for (int p : pitches) eng->pushLive(track, juce::MidiMessage::noteOff(ch, p));
    });
}

} // namespace mc
