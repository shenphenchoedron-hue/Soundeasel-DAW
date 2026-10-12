#include "sequencer/Sequence.h"
#include <algorithm>
#include <cmath>

namespace mc::seq {

std::vector<SeqEvent>::const_iterator TrackSequence::lowerBound(Tick t) const
{
    return std::lower_bound(events.begin(), events.end(), t, [](const SeqEvent& e, Tick v) { return e.tick < v; });
}

SequenceSnapshot SequenceSnapshot::build(const model::Project& p)
{
    SequenceSnapshot s;
    const bool anySolo = std::any_of(p.tracks().begin(), p.tracks().end(), [](auto& t) { return t->solo; });
    s.masterVolume = p.master.mute ? 0.0f : p.master.volume;
    s.masterBalance = p.master.balance;
    for (auto& t : p.tracks()) {
        if (auto* at = dynamic_cast<const model::AudioTrack*>(t.get())) {
            TrackSequence ts;
            ts.trackId = at->id();
            ts.isAudio = true;
            ts.audible = !at->mute && (!anySolo || at->solo);
            ts.volume = at->volume;
            ts.pan = at->pan;
            ts.eq = at->eq;
            ts.compressor = at->compressor;
            ts.reverb = at->reverb;
            for (auto& i : at->inserts) ts.insertBypassed.push_back(i.bypassed ? 1 : 0);
            ts.insertsAfterBuiltins = at->insertsAfterBuiltins;
            ts.input = at->input;
            ts.stereoInput = at->stereoInput;
            ts.armed = at->armed;
            ts.monitor = at->monitor;
            ts.clips = at->clips();
            ts.volumeAutomation = at->volumeAutomation;
            for (auto& c : ts.clips) s.lengthTicks = std::max(s.lengthTicks, (Tick)std::ceil(c.end(p.tempoBpm)));
            s.tracks.push_back(std::move(ts));
            continue;
        }
        auto* mt = dynamic_cast<const model::MidiTrack*>(t.get());
        if (!mt) continue;
        TrackSequence ts;
        ts.trackId = mt->id();
        ts.audible = !mt->mute && (!anySolo || mt->solo);
        ts.volume = mt->volume;
        ts.pan = mt->pan;
        ts.eq = mt->eq;
        ts.compressor = mt->compressor;
        ts.reverb = mt->reverb;
        for (auto& i : mt->inserts) ts.insertBypassed.push_back(i.bypassed ? 1 : 0);
        ts.insertsAfterBuiltins = mt->insertsAfterBuiltins;
        ts.channel = mt->channel;
        for (auto& n : mt->notes()) {
            const auto ch = (std::uint8_t)(n.channel >= 0 ? n.channel : mt->channel);
            ts.events.push_back({n.start, true, ch, (std::uint8_t)n.pitch, (std::uint8_t)n.velocity});
            ts.events.push_back({n.end(), false, ch, (std::uint8_t)n.pitch, 0});
            s.lengthTicks = std::max(s.lengthTicks, n.end());
        }
        std::stable_sort(ts.events.begin(), ts.events.end(), [](const SeqEvent& a, const SeqEvent& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            return !a.noteOn && b.noteOn; // offs first
        });
        s.tracks.push_back(std::move(ts));
    }
    return s;
}

} // namespace mc::seq
