#pragma once
// Immutable, flattened playback data built from the Project on the message
// thread and handed to the audio thread (which never touches the Project).
#include "model/Project.h"
#include <cstdint>
#include <vector>

namespace mc::seq {

using model::Tick;

struct SeqEvent {
    Tick tick;
    bool noteOn;
    std::uint8_t channel, pitch, velocity;
    bool controller = false; // true: control change, pitch = controller number, velocity = value
};

inline constexpr int kSustainController = 64;
inline constexpr Tick kSustainRampStep = model::kPPQ / 32; // ramps send a new pedal value at most every 1/128 note

struct ControllerValue {
    Tick tick;
    int value; // 0..127
    bool operator==(const ControllerValue&) const = default;
};
// Pedal values (CC 64) of a sustain line, only where the value changes: the first value at
// tick 0, steps at their tick, ramps sampled every kSustainRampStep. Empty line: none.
std::vector<ControllerValue> sustainValues(const std::vector<model::AutomationPoint>&);
// Pedal value (0..127) of a sustain line at `tick` (0 for an empty line).
int sustainValueAt(const std::vector<model::AutomationPoint>&, double tick);

// Note-on/off events of a track, sorted (offs, then ons at the same tick). Overlapping
// notes on the same key and channel are made safe for every synth: a note that starts
// while the key is still sounding first ends the sounding note (note-off, then note-on at
// the same tick), and only the last note-off of the overlapping notes is kept. So every
// note-on gets exactly one note-off and no voice is left hanging.
std::vector<SeqEvent> noteEvents(const model::MidiTrack&);

struct TrackSequence {
    model::TrackId trackId = 0;
    bool isAudio = false;
    bool audible = true;   // mute/solo resolved
    float volume = 0.8f, pan = 0.0f;
    int channel = 0;
    std::vector<SeqEvent> events; // sorted; note-offs, then controllers, then note-ons at equal tick
    std::vector<model::AutomationPoint> sustain; // MIDI tracks: pedal line (the engine restores it after jumps)
    model::EqSettings eq;                  // channel strip effects
    model::CompressorSettings compressor;
    model::ReverbSettings reverb;
    std::vector<char> insertBypassed;      // per effect plugin (same order as the track's inserts)
    bool insertsAfterBuiltins = false;

    // audio tracks
    int input = -1;
    bool stereoInput = false, armed = false, monitor = false;
    std::vector<model::AudioClip> clips; // sorted by start
    std::vector<model::AutomationPoint> volumeAutomation; // extra gain over time (empty = none)

    // Visit events with from <= tick < to.
    template <typename F> void forEachInRange(Tick from, Tick to, F&& f) const
    {
        auto it = lowerBound(from);
        for (; it != events.end() && it->tick < to; ++it) f(*it);
    }
    std::vector<SeqEvent>::const_iterator lowerBound(Tick t) const;
    // Whether a note on this key sounds just before `at` (its last note event before `at` is a note-on).
    bool noteSoundingAt(int channel, int pitch, Tick at) const;
};

struct SequenceSnapshot {
    std::vector<TrackSequence> tracks;   // in project order
    Tick lengthTicks = 0;
    float masterVolume = 1.0f;          // mute resolved (0 when muted)
    float masterBalance = 0.0f;         // -1 left .. 1 right
    static SequenceSnapshot build(const model::Project&);
};

} // namespace mc::seq
