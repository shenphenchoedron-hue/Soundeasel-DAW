#pragma once
#include "input/ControllerMapping.h"
#include "model/Track.h"
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mc::model {

struct TimeSignature {
    int numerator = 4;
    int denominator = 4;
    bool operator==(const TimeSignature&) const = default;
};

// The main output bus ("Mains") every track is mixed into.
struct MasterBus {
    float volume = 1.0f;   // linear gain 0..2
    float balance = 0.0f;  // stereo balance -1 (left) .. 1 (right)
    bool mute = false;
    bool operator==(const MasterBus&) const = default;
};

struct HarmonySettings {
    int root = 9;                         // pitch class, A
    std::string scaleId = "natural_minor";
    bool chordMode = false;
};

class Project {
public:
    Project();
    Project(const Project&);
    Project& operator=(const Project&);

    // ---- tracks
    const std::vector<std::unique_ptr<Track>>& tracks() const { return tracks_; }
    Track* track(TrackId id) const;
    MidiTrack* midiTrack(TrackId id) const;
    int indexOf(TrackId id) const;
    MidiTrack& addMidiTrack(const std::string& name = {});
    AudioTrack& addAudioTrack(const std::string& name = {});
    AudioTrack* audioTrack(TrackId id) const;
    void insertTrack(int index, std::unique_ptr<Track> t);  // used by undo
    std::unique_ptr<Track> removeTrack(TrackId id);
    TrackId nextTrackId() const { return nextTrackId_; }
    TrackId allocateTrackId() { return nextTrackId_++; }  // id for a track created outside add*Track (pasted copies)
    std::uint64_t unusedStepGroup() const;                // step sequencer group id no track uses yet

    TrackId activeTrack = 0;

    // ---- global song settings
    double tempoBpm = 120.0;
    TimeSignature timeSig;
    Tick playhead = 0;
    bool loopEnabled = false;
    Tick loopStart = 0;
    Tick loopEnd = 4 * 4 * kPPQ;
    HarmonySettings harmony;
    MasterBus master;

    // Instrument plugins ticked in the plugin browser ("format|identifier"): the instrument menus of
    // this project only list these (plus the built-in instruments and each track's current one).
    // Empty = the menus list every scanned instrument.
    std::set<std::string> instrumentMenu;
    static std::string pluginMenuKey(const std::string& format, const std::string& identifier) { return format + "|" + identifier; }
    bool instrumentMenuFiltered() const { return !instrumentMenu.empty(); }
    bool inInstrumentMenu(const std::string& format, const std::string& identifier) const
    {
        return instrumentMenu.empty() || instrumentMenu.count(pluginMenuKey(format, identifier)) > 0;
    }
    input::ControllerMapping controllerMapping;

    // ---- change notification (UI observes the model; model never knows the UI)
    using Listener = std::function<void()>;
    int addListener(Listener l);
    void removeListener(int handle);
    void notifyChanged() const;

private:
    std::vector<std::unique_ptr<Track>> tracks_;
    TrackId nextTrackId_ = 1;
    std::vector<std::pair<int, Listener>> listeners_;
    int nextListener_ = 1;
};

} // namespace mc::model
