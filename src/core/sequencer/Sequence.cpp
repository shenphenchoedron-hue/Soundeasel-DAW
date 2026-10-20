#include "sequencer/Sequence.h"
#include <algorithm>
#include <cmath>

namespace mc::seq {

std::vector<SeqEvent>::const_iterator TrackSequence::lowerBound(Tick t) const
{
    return std::lower_bound(events.begin(), events.end(), t, [](const SeqEvent& e, Tick v) { return e.tick < v; });
}

bool TrackSequence::noteSoundingAt(int channel, int pitch, Tick at) const
{
    for (auto it = lowerBound(at); it != events.begin();) {
        --it;
        if (!it->controller && it->channel == channel && it->pitch == pitch) return it->noteOn;
    }
    return false;
}

int sustainValueAt(const std::vector<model::AutomationPoint>& pts, double tick)
{
    if (pts.empty()) return 0;
    return std::clamp((int)std::lround(model::automationAt(pts, tick) * 127.0f), 0, 127);
}

std::vector<ControllerValue> sustainValues(const std::vector<model::AutomationPoint>& pts)
{
    std::vector<ControllerValue> v;
    if (pts.empty()) return v;
    auto emit = [&](Tick t, float g) {
        const int val = std::clamp((int)std::lround(g * 127.0f), 0, 127);
        if (v.empty() || v.back().value != val) v.push_back({t, val});
    };
    emit(0, pts.front().gain); // flat before the first point
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const auto& a = pts[i];
        const auto& b = pts[i + 1];
        if (b.tick > a.tick && a.gain != b.gain)
            for (Tick t = a.tick + kSustainRampStep; t < b.tick; t += kSustainRampStep)
                emit(t, model::automationAt(pts, (double)t));
        emit(b.tick, b.gain);
    }
    return v;
}

std::vector<SeqEvent> noteEvents(const model::MidiTrack& mt)
{
    std::vector<SeqEvent> raw;
    raw.reserve(mt.notes().size() * 2);
    for (auto& n : mt.notes()) {
        const auto ch = (std::uint8_t)((n.channel >= 0 ? n.channel : mt.channel) & 15);
        raw.push_back({n.start, true, ch, (std::uint8_t)n.pitch, (std::uint8_t)n.velocity});
        raw.push_back({n.end(), false, ch, (std::uint8_t)n.pitch, 0});
    }
    std::stable_sort(raw.begin(), raw.end(), [](const SeqEvent& a, const SeqEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return !a.noteOn && b.noteOn; // offs first
    });
    std::vector<SeqEvent> out;
    out.reserve(raw.size());
    std::vector<int> depth(16 * 128, 0); // notes currently overlapping on each key
    for (auto& e : raw) {
        auto& d = depth[(size_t)(e.channel * 128 + e.pitch)];
        if (e.noteOn) {
            if (d > 0) out.push_back({e.tick, false, e.channel, e.pitch, 0}); // end the sounding note first
            out.push_back(e);
            ++d;
        } else if (d > 0 && --d == 0) {
            out.push_back(e); // the last of the overlapping notes ends
        }
    }
    return out;
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
        ts.events = noteEvents(*mt);
        for (auto& n : mt->notes()) s.lengthTicks = std::max(s.lengthTicks, n.end());
        ts.sustain = mt->sustain;
        for (auto& cv : sustainValues(mt->sustain)) {
            ts.events.push_back({cv.tick, false, (std::uint8_t)mt->channel, (std::uint8_t)kSustainController, (std::uint8_t)cv.value, true});
            if (!mt->notes().empty()) s.lengthTicks = std::max(s.lengthTicks, cv.tick); // the song ends when the pedal is released
        }
        std::stable_sort(ts.events.begin(), ts.events.end(), [](const SeqEvent& a, const SeqEvent& b) {
            if (a.tick != b.tick) return a.tick < b.tick;
            auto rank = [](const SeqEvent& e) { return e.controller ? 1 : e.noteOn ? 2 : 0; }; // offs, controllers, ons
            return rank(a) < rank(b);
        });
        s.tracks.push_back(std::move(ts));
    }
    return s;
}

} // namespace mc::seq
