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
};

struct TrackSequence {
    model::TrackId trackId = 0;
    bool isAudio = false;
    bool audible = true;   // mute/solo resolved
    float volume = 0.8f, pan = 0.0f;
    int channel = 0;
    std::vector<SeqEvent> events; // sorted; note-offs before note-ons at equal tick
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
};

struct SequenceSnapshot {
    std::vector<TrackSequence> tracks;   // in project order
    Tick lengthTicks = 0;
    float masterVolume = 1.0f;          // mute resolved (0 when muted)
    float masterBalance = 0.0f;         // -1 left .. 1 right
    static SequenceSnapshot build(const model::Project&);
};

} // namespace mc::seq
