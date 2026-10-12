#pragma once
// Application controller: owns the model and all services and wires them
// together. GUI components talk to this, never to each other's data.
#include "audio/AudioEngine.h"
#include "audio/AudioExport.h"
#include "audio/AudioFileCache.h"
#include "input/ChordInputProcessor.h"
#include "midi/MidiDeviceManager.h"
#include "model/Commands.h"
#include "plugins/PluginManager.h"
#include "recording/Recorder.h"
#include "sequencer/Timing.h"
#include "settings/Settings.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <set>

namespace mc {

class AppContext final : public juce::ChangeBroadcaster, private juce::ChangeListener {
public:
    AppContext();
    ~AppContext() override;

    model::Project project;
    model::UndoStack undo{project};
    settings::Settings settings;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<audio::AudioEngine> engine;
    std::unique_ptr<plugins::PluginManager> plugins;
    std::unique_ptr<midi::MidiDeviceManager> midiDevices;
    recording::Recorder recorder;
    audio::AudioFileCache audioFiles;
    std::vector<model::Note> clipboard; // starts relative to 0

    // ---- project files
    void newProject();
    bool openProject(const juce::File&, juce::String& error);
    bool saveProject(const juce::File&, juce::String& error);
    bool importMidi(const juce::File&, juce::String& error);
    bool exportMidi(const juce::File&, juce::String& error);
    bool exportMusicXml(const juce::File&, juce::String& error);
    // Song export: private copies of every instrument and effect (with their current
    // settings) plus the song, ready to be rendered offline by audio::renderSong on a
    // worker thread. Message thread; destroy the session on the message thread too.
    // nullptr + error if there is nothing to export.
    std::unique_ptr<audio::RenderSession> createRenderSession(double sampleRate, bool loopOnly, juce::String& error);
    bool importMusicXml(const juce::File&, juce::String& error); // .musicxml, .xml, .mxl
    void openSoundsFolder();   // reveals the user's .sf2 folder in Finder/Explorer
    void showAbout();          // version + third-party credits
    void openManual();         // bundled English PDF manual; falls back to the GitHub Releases page
    juce::File currentFile;
    bool hasUnsavedChanges() const { return dirty; }

    // ---- tracks
    void addTrack();
    void deleteTrack(model::TrackId);
    void selectTrack(model::TrackId);
    model::MidiTrack* activeMidiTrack() const { return project.midiTrack(project.activeTrack); }
    void setTrackPlugin(model::TrackId, const plugins::PluginInfo*); // nullptr = remove
    // Undoable change of a track's properties (name, mute, solo, volume, pan, input, ...).
    void changeTrack(model::TrackId, const std::function<void(model::TrackProperties&)>& edit);
    // Live volume/pan gestures (fader drags): heard immediately, one undo step at the end,
    // none if nothing changed.
    void beginTrackEdit(model::TrackId);
    void liveTrackVolume(model::TrackId, float gain);
    void liveTrackPan(model::TrackId, float pan);
    void endTrackEdit(model::TrackId);
    bool isEditingTrack(model::TrackId id) const { return trackEdits.count(id) > 0; }

    // ---- audio tracks
    model::TrackId addAudioTrack(const std::string& name = {});  // returns the new track id
    // Imports an audio file as a clip at `at`. track == 0 creates a new audio track.
    bool importAudio(const juce::File&, model::TrackId track, model::Tick at, juce::String& error);
    void setAudioClips(model::TrackId, std::vector<model::AudioClip> after, const std::string& name = "Edit audio");
    void setTrackArmed(model::TrackId, bool);     // record enable (not undoable)
    void setTrackMonitor(model::TrackId, bool);   // hear the input (not undoable)
    void setVolumeAutomation(model::TrackId, std::vector<model::AutomationPoint> after); // undoable
    void setShowVolumeLine(model::TrackId, bool); // show/edit the volume line in the lane
    void setTrackInput(model::TrackId, int input, bool stereo);
    juce::StringArray inputNames() const;         // active inputs of the audio device, index = AudioTrack::input
    std::shared_ptr<const audio::AudioFileData> audioFile(const std::string& path); // nullptr while loading or missing
    bool isAudioFileLoading(const std::string& path) const { return audioFiles.isLoading(path); }
    juce::File audioFolder() const;               // new recordings go here ("<project> Audio" next to the project)
    std::vector<model::AudioTrack*> audioTracks() const;

    // ---- Mains (master bus)
    void setMaster(const model::MasterBus& before, const model::MasterBus& after); // undoable
    void setMasterLive(const model::MasterBus&);  // while dragging, no undo

    // ---- audio device status ("does my sound work?")
    bool audioOutputOk() const;
    // Mains always plays on a stereo pair of the sound device.
    bool isStereoOutput() const { return engine->numOutputChannels() >= 2; }
    juce::StringArray mainsOutputPairs() const;   // "Out 1+2", "Out 3+4", ... (index = pair)
    int mainsOutputPair() const;                  // -1 if unknown
    void setMainsOutputPair(int pair, bool userChoice = true);
    // The user picked the sound device (settings window): from now on it is saved again.
    void userChoseAudioDevice() { deviceFellBack = false; }
    juce::String audioStatusText() const;
    void playTestTone() { engine->playTestTone(); }

    // ---- meters: polled once per UI frame, read by every view (track cards, mixer)
    void pollLevels();
    audio::AudioEngine::Levels levels(model::TrackId id) const; // id 0 = Mains

    // ---- step sequencer (a group of MidiTracks, one per line)
    void addStepSequencer();                       // new group with one line, becomes active
    void addStepLine(std::uint64_t group);         // new line (track) in an existing group
    void deleteStepSequencer(std::uint64_t group); // all its lines, one undo step
    std::vector<std::uint64_t> stepGroups() const; // in track order
    std::uint64_t activeStepGroup() const;         // group of the active track, 0 if none
    std::vector<model::MidiTrack*> stepLines(std::uint64_t group) const;
    void setStepPatterns(std::vector<model::SetStepPatternsCommand::Entry> after, const std::string& name = "Edit steps");
    void setTrackBypass(model::TrackId, bool);
    void openPluginEditor(model::TrackId);

    // ---- effect plugins on channel strips (undoable)
    void addInsert(model::TrackId, const plugins::PluginInfo&);
    void replaceInsert(model::TrackId, int index, const plugins::PluginInfo&);
    void removeInsert(model::TrackId, int index);
    void moveInsert(model::TrackId, int from, int to);
    void setInsertBypass(model::TrackId, int index, bool);
    void setInsertsAfterBuiltins(model::TrackId, bool);
    void openInsertEditor(model::TrackId, int index);
    juce::String insertError(model::TrackId, int index) const; // empty: loaded fine
    bool isPluginMissing(model::TrackId id) const { return missing.count(id) > 0; }
    juce::String pluginError(model::TrackId id) const;

    // ---- transport
    void play();
    void stop();
    void toggleRecord();
    bool isRecording() const { return recorder.isRecording() || (engine && engine->isRecordingAudio()); }
    void returnToStart();
    void setPosition(model::Tick);
    void setTempo(double bpm);
    void setTimeSignature(int num, int den);
    void setLoop(bool enabled, model::Tick start, model::Tick end);

    // ---- metronome / count-in (count-in is used when recording)
    bool metronome() const { return metronomeOn; }
    void setMetronome(bool);
    float metronomeLevel() const { return metronomeLvl; }
    void setMetronomeLevel(float);
    int countInBars() const { return countIn; }
    void setCountInBars(int);
    bool isCountingIn() const { return engine && engine->isCountingIn(); }

    // ---- harmony / chord input
    void setHarmony(int root, const std::string& scaleId, bool chordMode);
    void setModifierLatched(input::Modifier, bool on);
    bool isModifierLatched(input::Modifier m) const;
    theory::ChordRequest chordRequestFor(int pitch) const; // honours latched + held modifiers
    input::ChordDisplayState chordDisplay() const;
    void beginMidiLearn(input::Modifier);
    std::optional<input::Modifier> learningModifier() const;
    void setControllerMapping(const input::ControllerMapping&);

    // ---- live note input (any thread): from MIDI keyboard or on-screen keyboard
    void handleKeyboardNote(bool on, int pitch, int velocity);
    void previewNotes(const std::vector<int>& pitches, int velocity, int durationMs); // GUI: audition inserted notes
    void previewOnTrack(model::TrackId, int pitch, int velocity, int durationMs);       // audition a step sequencer line

    // Call after modifying the model without a command (e.g. live drag).
    void modelChangedWithoutUndo() { project.notifyChanged(); }

    // Snaps for UI
    seq::GridValue grid;
    bool snapEnabled = true;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override; // audio device changed
    bool startAudioRecording();
    void finishAudioRecording(std::vector<std::unique_ptr<model::Command>>& cmds);
    void collectAudioFiles(const juce::File& projectFile);
    void ensureStereoOutput();
    bool changingDevice = false;
    bool deviceFellBack = false; // the saved device could not be opened: the default device is used, not saved
    void onModelChanged();
    void syncEngine();
    void addImportedTracks(const model::Project& imported); // undoable; adopts tempo + time signature
    void captureAllPluginStates();
    void handleMidi(const juce::MidiMessage&);
    void sendLive(const std::vector<input::OutputEvent>&);
    void closeEditor(model::TrackId);
    void syncInserts(model::Track&);
    void clearInserts(model::TrackId);
    void prepareToRemove(model::TrackId); // instrument + effects of a track, before its engine slot goes   // after the engine slot is gone or its inserts were reset
    void captureInsertStates(model::Track&);
    void editInserts(model::TrackId, const std::function<void(std::vector<model::PluginReference>&)>&);

    std::map<model::TrackId, audio::AudioEngine::Levels> lastLevels;
    std::map<model::TrackId, model::TrackProperties> trackEdits; // volume/pan gestures in progress
    struct Take { model::TrackId track; juce::File file; };
    std::vector<Take> takes;
    juce::TimeSliceThread diskThread{"Audio recording"};

    mutable std::mutex chordMutex;
    input::ChordInputProcessor chordInput;

    std::atomic<model::TrackId> activeTrackAtomic{0};
    std::atomic<int> activeChannelAtomic{0};
    std::map<model::TrackId, model::PluginReference> loadedRefs;
    std::set<model::TrackId> missing;
    std::map<model::TrackId, juce::String> errors;
    std::map<model::TrackId, std::unique_ptr<juce::DocumentWindow>> editorWindows;
    struct InsertInstance {
        model::PluginReference ref;
        std::unique_ptr<plugins::InstrumentPlugin> plugin; // nullptr: missing / failed (see error)
        juce::String error;
    };
    std::map<model::TrackId, std::vector<InsertInstance>> inserts;
    std::map<plugins::InstrumentPlugin*, std::unique_ptr<juce::DocumentWindow>> insertEditors;
    int listenerHandle = 0;
    model::TrackId lastPianoTrack = 0;
    bool metronomeOn = false;
    float metronomeLvl = 0.6f;
    int countIn = 1; // the piano roll never edits step sequencer lines
    bool dirty = false;
    bool suppressDirty = false;
};

} // namespace mc
