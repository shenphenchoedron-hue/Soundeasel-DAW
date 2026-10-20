#include "audio/AudioEngine.h"
#include "sequencer/Timing.h"

namespace mc::audio {

namespace {
constexpr int kClipFade = 64; // samples of fade at clip edges (avoids clicks)
}

std::optional<PlaybackSnapshot::Clip> PlaybackSnapshot::makeClip(const model::AudioClip& c, std::shared_ptr<const AudioFileData> data)
{
    if (!data) return std::nullopt;
    Clip pc;
    pc.offset = (juce::int64)std::llround(c.offset * data->sampleRate);
    pc.length = std::min<juce::int64>((juce::int64)std::llround(c.length * data->sampleRate), data->audio.getNumSamples() - pc.offset);
    if (pc.length <= 0) return std::nullopt;
    pc.data = std::move(data);
    pc.start = c.start;
    pc.gain = c.gain;
    return pc;
}

AudioEngine::AudioEngine(juce::AudioDeviceManager& d) : devices(&d)
{
    mixBuffer.setSize(2, 4096);
    busBuffer.setSize(2, 4096);
    silence.setSize(2, 4096);
    silence.clear();
    devices->addAudioCallback(this);
}

AudioEngine::AudioEngine(double sr, int bs)
{
    sampleRate_ = sr > 0 ? sr : 44100.0;
    blockSize_ = juce::jlimit(16, 4096, bs);
    numOutputs_ = 2;
    running_ = true;
    mixBuffer.setSize(2, 4096);
    busBuffer.setSize(2, 4096);
    silence.setSize(2, 4096);
    silence.clear();
}

AudioEngine::~AudioEngine()
{
    if (devices) devices->removeAudioCallback(this);
    stopAudioRecording();
    const juce::ScopedLock l(slotLock);
    slots.clear();
}

void AudioEngine::setSnapshot(std::shared_ptr<const PlaybackSnapshot> s)
{
    {
        const juce::SpinLock::ScopedLockType l(snapshotLock);
        if (pendingSnapshot) retired.push_back(pendingSnapshot);
        pendingSnapshot = std::move(s);
    }
    // keep a few old snapshots alive so the audio thread never frees memory
    while (retired.size() > 4) retired.erase(retired.begin());
}

void AudioEngine::setLoop(bool enabled, model::Tick start, model::Tick end)
{
    loopStart_ = start;
    loopEnd_ = end;
    loopEnabled_ = enabled && end > start;
}

AudioEngine::Slot* AudioEngine::findSlot(model::TrackId id) const
{
    for (auto& s : slots) if (s->id == id) return s.get();
    return nullptr;
}

std::unique_ptr<plugins::InstrumentPlugin> AudioEngine::setInstrument(model::TrackId id, std::unique_ptr<plugins::InstrumentPlugin> p)
{
    if (p) p->prepare(sampleRate(), blockSize());
    const juce::ScopedLock l(slotLock);
    auto* s = findSlot(id);
    if (!s) {
        auto ns = std::make_unique<Slot>();
        ns->id = id;
        ns->midi.ensureSize(8192);
        ns->eq.prepare(sampleRate());
        ns->comp.prepare(sampleRate());
        ns->reverb.prepare(sampleRate()); // allocates: never on the audio thread
        s = ns.get();
        slots.push_back(std::move(ns));
    }
    std::swap(s->plugin, p);
    s->held.reset();
    s->pedalDown.reset();
    s->chasePedal = true; // a new instrument gets the sustain line's pedal position
    return p;
}

// `slots` and each Slot::plugin are only ever modified on the message thread
// (under slotLock, so the audio thread never sees a half-done change). Reading
// them from the message thread therefore needs no lock. Taking the lock here
// would make every UI action wait for the audio thread to finish rendering
// (which can take most of a block with heavy plugins like Kontakt), and would
// make the audio thread drop blocks whenever the UI held the lock.
plugins::InstrumentPlugin* AudioEngine::instrument(model::TrackId id) const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    auto* s = findSlot(id);
    return s ? s->plugin.get() : nullptr;
}

void AudioEngine::setBypassed(model::TrackId id, bool b)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    if (auto* s = findSlot(id)) s->bypassed = b; // atomic, read by the audio thread
}

std::unique_ptr<plugins::InstrumentPlugin> AudioEngine::removeTrack(model::TrackId id)
{
    std::unique_ptr<Slot> removed;
    {
        const juce::ScopedLock l(slotLock);
        for (auto it = slots.begin(); it != slots.end(); ++it)
            if ((*it)->id == id) { removed = std::move(*it); slots.erase(it); break; }
    }
    return removed ? std::move(removed->plugin) : nullptr; // a recording writer is flushed and closed here
}

void AudioEngine::setInserts(model::TrackId id, std::vector<plugins::InstrumentPlugin*> list)
{
    const juce::ScopedLock l(slotLock);
    if (auto* s = findSlot(id)) s->inserts = std::move(list);
}

void AudioEngine::runInserts(Slot& s, const seq::TrackSequence* ts, juce::AudioBuffer<float>& buf)
{
    for (size_t k = 0; k < s.inserts.size(); ++k) {
        auto* p = s.inserts[k];
        if (!p) continue;
        if (ts && k < ts->insertBypassed.size() && ts->insertBypassed[k]) continue;
        s.fxMidi.clear();
        p->process(buf, s.fxMidi);
    }
}

std::vector<model::TrackId> AudioEngine::trackIds() const
{
    jassert(juce::MessageManager::existsAndIsCurrentThread()); // see instrument()
    std::vector<model::TrackId> ids;
    for (auto& s : slots) ids.push_back(s->id);
    return ids;
}

AudioEngine::Levels AudioEngine::takeTrackLevels(model::TrackId id)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    auto* s = findSlot(id);
    if (!s) return {};
    return {s->peakL.exchange(0.0f), s->peakR.exchange(0.0f), s->peakIn.exchange(0.0f), s->gainReduction.exchange(0.0f)};
}

AudioEngine::Levels AudioEngine::takeMasterLevels()
{
    return {masterPeakL.exchange(0.0f), masterPeakR.exchange(0.0f), 0.0f};
}

void AudioEngine::startAudioRecording(std::vector<std::pair<model::TrackId, std::unique_ptr<Writer>>> writers)
{
    jassert(juce::MessageManager::existsAndIsCurrentThread());
    stopAudioRecording();
    recordStartPos_ = -1.0;
    for (auto& [id, w] : writers)
        if (auto* s = findSlot(id)) {
            s->writerOwner = std::move(w);
            s->writer = s->writerOwner.get();
        }
    audioRecording_ = true;
}

double AudioEngine::stopAudioRecording()
{
    audioRecording_ = false;
    for (auto& s : slots) s->writer = nullptr;
    {
        const juce::ScopedLock l(slotLock); // waits for a running callback: no writer is in use after this
    }
    for (auto& s : slots) s->writerOwner.reset(); // flushes the remaining samples and closes the file
    return recordStartPos_.load();
}

void AudioEngine::pushLive(model::TrackId track, const juce::MidiMessage& m)
{
    if (m.getRawDataSize() > 3) return;
    const juce::SpinLock::ScopedLockType l(livePushLock);
    int s1, n1, s2, n2;
    liveFifo.prepareToWrite(1, s1, n1, s2, n2);
    if (n1 + n2 < 1) return; // full: drop
    auto& e = liveBuffer[(size_t)(n1 > 0 ? s1 : s2)];
    e.track = track;
    e.size = m.getRawDataSize();
    std::memcpy(e.data, m.getRawData(), (size_t)e.size);
    liveFifo.finishedWrite(1);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* d)
{
    sampleRate_ = d->getCurrentSampleRate();
    blockSize_ = d->getCurrentBufferSizeSamples();
    numInputs_ = d->getActiveInputChannels().countNumberOfSetBits();
    numOutputs_ = d->getActiveOutputChannels().countNumberOfSetBits();
    latency_ = d->getInputLatencyInSamples() + d->getOutputLatencyInSamples();
    const int size = std::max(4096, blockSize_.load());
    mixBuffer.setSize(2, size);
    busBuffer.setSize(2, size);
    silence.setSize(2, size);
    silence.clear();
    running_ = true;
    const juce::ScopedLock l(slotLock);
    for (auto& s : slots) {
        if (s->plugin) s->plugin->prepare(sampleRate_, blockSize_);
        for (auto* p : s->inserts) if (p) p->prepare(sampleRate_, blockSize_);
        s->eq.prepare(sampleRate_);
        s->comp.prepare(sampleRate_);
        s->reverb.prepare(sampleRate_);
    }
}

void AudioEngine::audioDeviceStopped()
{
    running_ = false;
    const juce::ScopedLock l(slotLock);
    for (auto& s : slots) {
        if (s->plugin) s->plugin->release();
        for (auto* p : s->inserts) if (p) p->release();
    }
}

void AudioEngine::releaseHeld(Slot& s, int offset)
{
    // Stop, jump, loop or mute: release the sustain pedal too, and restore the
    // sustain line's position when the sequencer plays this track again.
    s.chasePedal = true;
    if (s.pedalDown.any()) {
        for (int ch = 0; ch < 16; ++ch)
            if (s.pedalDown[(size_t)ch]) s.midi.addEvent(juce::MidiMessage::controllerEvent(ch + 1, seq::kSustainController, 0), offset);
        s.pedalDown.reset();
    }
    if (s.held.none()) return;
    for (int i = 0; i < 16 * 128; ++i)
        if (s.held[(size_t)i]) s.midi.addEvent(juce::MidiMessage::noteOff(i / 128 + 1, i % 128), offset);
    s.held.reset();
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* in, int numIn, float* const* out, int numOut, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext&)
{
    const juce::ScopedNoDenormals noDenormals; // reverb tails and compressor envelopes
    for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear(out[c], numSamples);

    {
        const juce::SpinLock::ScopedTryLockType l(snapshotLock);
        if (l.isLocked() && pendingSnapshot != audioSnapshot) { audioSnapshot = pendingSnapshot; ++snapshotGeneration; }
    }

    const juce::ScopedTryLock l(slotLock);
    if (!l.isLocked()) return; // instrument being swapped: skip one block rather than block

    const int maxChunk = std::min(mixBuffer.getNumSamples(), std::max(1, blockSize_.load()));
    for (int start = 0; start < numSamples; start += maxChunk)
        renderChunk(in, numIn, out, numOut, start, std::min(maxChunk, numSamples - start));
}

void AudioEngine::renderOffline(float* const* out, int numOut, int numSamples)
{
    jassert(devices == nullptr); // a device engine is driven by its audio callback
    const juce::ScopedNoDenormals noDenormals;
    for (int c = 0; c < numOut; ++c) if (out[c]) juce::FloatVectorOperations::clear(out[c], numSamples);
    {
        const juce::SpinLock::ScopedLockType l(snapshotLock);
        if (pendingSnapshot != audioSnapshot) { audioSnapshot = pendingSnapshot; ++snapshotGeneration; }
    }
    const juce::ScopedLock l(slotLock); // offline: wait instead of skipping a block
    const int maxChunk = std::min(mixBuffer.getNumSamples(), std::max(1, blockSize_.load()));
    for (int start = 0; start < numSamples; start += maxChunk)
        renderChunk(nullptr, 0, out, numOut, start, std::min(maxChunk, numSamples - start));
}

void AudioEngine::renderChunk(const float* const* in, int numIn, float* const* out, int numOut, int start, int n)
{
    // transport commands
    numPendingClicks = 0;
    numSegments = 0;
    if (stopRequested_.exchange(false)) {
        playing_ = false;
        countInLeft_ = 0;
        for (auto& s : slots) releaseHeld(*s, 0);
    }
    const double seek = pendingSeek_.exchange(-1.0);
    double pos = position_.load();
    if (seek >= 0) {
        pos = seek;
        for (auto& s : slots) releaseHeld(*s, 0);
    }

    // live MIDI
    {
        int s1, n1, s2, n2;
        liveFifo.prepareToRead(liveFifo.getNumReady(), s1, n1, s2, n2);
        auto take = [&](int from, int count) {
            for (int i = 0; i < count; ++i) {
                auto& e = liveBuffer[(size_t)(from + i)];
                if (auto* s = findSlot(e.track)) s->midi.addEvent(e.data, e.size, 0);
            }
        };
        take(s1, n1);
        take(s2, n2);
        liveFifo.finishedRead(n1 + n2);
    }

    // count-in: metronome only, the song position does not move
    int seqStart = 0;
    if (playing_ && countInLeft_.load() > 0) {
        const double tps = seq::ticksPerSample(tempo_.load(), sampleRate_.load());
        const double total = countInTotal_.load(), left = countInLeft_.load();
        const double elapsed = total - left, end = elapsed + n * tps;
        scheduleClicks(elapsed, std::min(end, total), tps, 0, n, elapsed);
        if (end >= total) { // count-in ends inside this chunk: the song starts at that sample
            seqStart = juce::jlimit(0, n, (int)std::ceil(left / tps));
            countInLeft_ = 0;
        } else {
            countInLeft_ = total - end;
            seqStart = n;
        }
    }

    const auto* snap = audioSnapshot.get();
    const bool songRolling = playing_ && seqStart < n;
    const double recordPos = pos; // song position at sample seqStart

    // sequencer
    if (songRolling && snap) {
        const double tps = seq::ticksPerSample(tempo_.load(), sampleRate_.load());
        // While audio is recorded the song plays straight through (no loop jumps), so takes stay continuous.
        const bool loop = loopEnabled_ && !audioRecording_;
        int sampleAt = seqStart;
        while (sampleAt < n) {
            int len = n - sampleAt;
            double segEnd = pos + len * tps;
            bool wrap = false;
            const auto le = (double)loopEnd_.load();
            if (loop && pos < le && segEnd >= le) {
                len = std::max(1, (int)std::ceil((le - pos) / tps));
                len = std::min(len, n - sampleAt);
                segEnd = le;
                wrap = true;
            }
            if (numSegments < (int)segments.size()) segments[(size_t)numSegments++] = {sampleAt, len, pos};
            const auto fromT = (model::Tick)std::ceil(pos);
            const auto toT = (model::Tick)std::ceil(segEnd);
            if (metronomeOn_) scheduleClicks(pos, segEnd, tps, sampleAt, n, pos);
            for (auto& s : slots) {
                const seq::TrackSequence* ts = nullptr;
                for (auto& t : snap->seq.tracks) if (t.trackId == s->id) { ts = &t; break; }
                if (!ts || ts->isAudio) continue;
                if (!ts->audible) { releaseHeld(*s, sampleAt); continue; }
                if (s->seqGeneration != snapshotGeneration) {
                    // The song was edited while playing: a held note whose note was deleted, moved or shortened
                    // would never get its note-off - end it now. Put the pedal where the (edited) line is.
                    s->seqGeneration = snapshotGeneration;
                    s->chasePedal = true;
                    if (s->held.any()) {
                        const auto at = (model::Tick)std::ceil(pos);
                        for (int i = 0; i < 16 * 128; ++i)
                            if (s->held[(size_t)i] && !ts->noteSoundingAt(i / 128, i % 128, at)) {
                                s->midi.addEvent(juce::MidiMessage::noteOff(i / 128 + 1, i % 128), juce::jlimit(0, n - 1, sampleAt));
                                s->held.reset((size_t)i);
                            }
                    }
                }
                if (s->chasePedal) {
                    // after start, jumps and loops (and edits): put the pedal where the sustain line is now
                    s->chasePedal = false;
                    if (!ts->sustain.empty()) {
                        const int v = seq::sustainValueAt(ts->sustain, pos);
                        const auto ch = (size_t)juce::jlimit(0, 15, ts->channel);
                        if (v > 0 || s->pedalDown[ch]) {
                            s->midi.addEvent(juce::MidiMessage::controllerEvent((int)ch + 1, seq::kSustainController, v), juce::jlimit(0, n - 1, sampleAt));
                            s->pedalDown.set(ch, v > 0);
                        }
                    }
                }
                ts->forEachInRange(fromT, toT, [&](const seq::SeqEvent& e) {
                    const int off = juce::jlimit(0, n - 1, sampleAt + (int)(((double)e.tick - pos) / tps));
                    if (e.controller) {
                        s->midi.addEvent(juce::MidiMessage::controllerEvent(e.channel + 1, e.pitch, e.velocity), off);
                        if (e.pitch == seq::kSustainController) s->pedalDown.set(e.channel & 15, e.velocity > 0);
                        return;
                    }
                    const auto idx = (size_t)(e.channel * 128 + e.pitch);
                    if (e.noteOn) {
                        // never two voices on one key: synths may end only one of them (hanging note)
                        if (s->held[idx]) s->midi.addEvent(juce::MidiMessage::noteOff(e.channel + 1, e.pitch), off);
                        s->midi.addEvent(juce::MidiMessage::noteOn(e.channel + 1, e.pitch, (juce::uint8)e.velocity), off);
                        s->held.set(idx);
                    } else if (s->held[idx]) {
                        s->midi.addEvent(juce::MidiMessage::noteOff(e.channel + 1, e.pitch), off);
                        s->held.reset(idx);
                    }
                });
            }
            sampleAt += len;
            pos = segEnd;
            if (wrap) {
                pos = (double)loopStart_.load();
                for (auto& s : slots) releaseHeld(*s, std::min(sampleAt, n - 1));
            }
        }
    } else if (songRolling) {
        pos += (n - seqStart) * seq::ticksPerSample(tempo_.load(), sampleRate_.load());
    }
    position_ = pos;

    // render every track through its channel strip into the Mains bus
    const double sr = sampleRate_.load(), bpm = tempo_.load();
    juce::AudioBuffer<float> bus(busBuffer.getArrayOfWritePointers(), 2, n); // no allocation: refers to existing data
    bus.clear();
    bool wroteAudio = false;
    for (size_t si = 0; si < slots.size(); ++si) {
        auto& s = slots[si];
        const seq::TrackSequence* ts = nullptr;
        const std::vector<PlaybackSnapshot::Clip>* clips = nullptr;
        if (snap)
            for (size_t i = 0; i < snap->seq.tracks.size(); ++i)
                if (snap->seq.tracks[i].trackId == s->id) {
                    ts = &snap->seq.tracks[i];
                    if (i < snap->clips.size()) clips = &snap->clips[i];
                    break;
                }

        juce::AudioBuffer<float> buf(mixBuffer.getArrayOfWritePointers(), 2, n);
        buf.clear();
        if (s->plugin && !s->bypassed) s->plugin->process(buf, s->midi);
        s->midi.clear();

        if (ts && ts->isAudio) {
            if (clips && !clips->empty() && numSegments > 0) renderClips(*clips, buf, bpm, sr);
            // input: meter, monitoring, recording
            const int ch = ts->input;
            const bool hasIn = ch >= 0 && ch < numIn && in[ch] != nullptr;
            const bool hasIn2 = ts->stereoInput && ch + 1 < numIn && in[ch + 1] != nullptr;
            if (hasIn && (ts->armed || ts->monitor)) {
                const float* l = in[ch] + start;
                const float* r = hasIn2 ? in[ch + 1] + start : l;
                float m = 0.0f;
                for (int i = 0; i < n; ++i) m = std::max(m, std::max(std::abs(l[i]), std::abs(r[i])));
                storePeak(s->peakIn, m);
                if (ts->monitor) {
                    buf.addFrom(0, 0, l, n);
                    buf.addFrom(1, 0, r, n);
                }
            }
            if (auto* w = s->writer.load(); w && audioRecording_ && songRolling) {
                const int count = n - seqStart;
                const float* chans[2] = {silence.getReadPointer(0), silence.getReadPointer(1)};
                if (hasIn) chans[0] = in[ch] + start + seqStart;
                if (hasIn2) chans[1] = in[ch + 1] + start + seqStart;
                w->write(chans, count);
                wroteAudio = true;
            }
        }

        // channel strip: effect plugins (before or after the built-in effects), EQ -> compressor -> reverb
        const bool insertsLast = ts && ts->insertsAfterBuiltins;
        if (!s->inserts.empty() && !insertsLast) runInserts(*s, ts, buf);
        if (ts) {
            float* L = buf.getWritePointer(0);
            float* R = buf.getWritePointer(1);
            if (ts->eq.enabled) {
                // Snapshots are immutable: new settings always come with a new snapshot.
                if (s->eqGeneration != snapshotGeneration) { s->eq.setSettings(ts->eq); s->eqGeneration = snapshotGeneration; }
                if (!s->eqOn) { s->eq.reset(); s->eqOn = true; }
                s->eq.process(L, R, n);
            } else {
                s->eqOn = false;
            }
            if (ts->compressor.enabled) {
                if (!(ts->compressor == s->compSettings)) { s->comp.setSettings(ts->compressor); s->compSettings = ts->compressor; }
                if (!s->compOn) { s->comp.reset(); s->compOn = true; }
                s->comp.process(L, R, n);
                storePeak(s->gainReduction, s->comp.gainReductionDb());
            } else {
                s->compOn = false;
            }
            if (ts->reverb.enabled) {
                if (!(ts->reverb == s->reverbSettings)) { s->reverb.setSettings(ts->reverb); s->reverbSettings = ts->reverb; }
                if (!s->reverbOn) { s->reverb.reset(); s->reverbOn = true; } // no old tail when switched on again
                s->reverb.process(L, R, n);
            } else {
                s->reverbOn = false;
            }
        }

        if (!s->inserts.empty() && insertsLast) runInserts(*s, ts, buf);

        // volume line (automation): gain ramps between the values at the segment ends
        if (ts && !ts->volumeAutomation.empty()) {
            const auto& va = ts->volumeAutomation;
            if (numSegments == 0) {
                buf.applyGain(0, n, model::automationAt(va, position_.load()));
            } else {
                const double tps = seq::ticksPerSample(bpm, sr);
                if (segments[0].sampleAt > 0) buf.applyGain(0, segments[0].sampleAt, model::automationAt(va, segments[0].pos));
                for (int k = 0; k < numSegments; ++k) {
                    const auto& sg = segments[(size_t)k];
                    const float g0 = model::automationAt(va, sg.pos), g1 = model::automationAt(va, sg.pos + sg.len * tps);
                    for (int c = 0; c < 2; ++c) buf.applyGainRamp(c, sg.sampleAt, sg.len, g0, g1);
                }
            }
        }

        // channel strip: volume + pan (smoothed), mute/solo, meter
        float tl = 0.0f, tr = 0.0f;
        if (ts && ts->audible) {
            tl = ts->volume * std::min(1.0f, 1.0f - ts->pan);
            tr = ts->volume * std::min(1.0f, 1.0f + ts->pan);
        } else if (!ts) {
            tl = tr = 0.8f; // track not yet in the snapshot (just added)
        }
        buf.applyGainRamp(0, 0, n, s->gainL, tl);
        buf.applyGainRamp(1, 0, n, s->gainR, tr);
        s->gainL = tl;
        s->gainR = tr;
        storePeak(s->peakL, buf.getMagnitude(0, 0, n));
        storePeak(s->peakR, buf.getMagnitude(1, 0, n));
        bus.addFrom(0, 0, buf, 0, 0, n);
        bus.addFrom(1, 0, buf, 1, 0, n);
    }
    if (wroteAudio && recordStartPos_.load() < 0) recordStartPos_ = recordPos;

    // Mains: stereo bus with balance
    const float mg = snap ? snap->seq.masterVolume : 1.0f;
    const float bal = snap ? snap->seq.masterBalance : 0.0f;
    const float ml = mg * std::min(1.0f, 1.0f - bal), mr = mg * std::min(1.0f, 1.0f + bal);
    bus.applyGainRamp(0, 0, n, masterGainL, ml);
    bus.applyGainRamp(1, 0, n, masterGainR, mr);
    masterGainL = ml;
    masterGainR = mr;
    storePeak(masterPeakL, bus.getMagnitude(0, 0, n));
    storePeak(masterPeakR, bus.getMagnitude(1, 0, n));
    // Mains goes to the first two active outputs (the stereo pair chosen for Mains);
    // a device with a single output gets a mono sum.
    if (numOut == 1 && out[0]) {
        juce::FloatVectorOperations::addWithMultiply(out[0] + start, bus.getReadPointer(0), 0.5f, n);
        juce::FloatVectorOperations::addWithMultiply(out[0] + start, bus.getReadPointer(1), 0.5f, n);
    } else {
        if (numOut > 0 && out[0]) juce::FloatVectorOperations::add(out[0] + start, bus.getReadPointer(0), n);
        if (numOut > 1 && out[1]) juce::FloatVectorOperations::add(out[1] + start, bus.getReadPointer(1), n);
    }
    renderClicks(out, numOut, start, n);
    renderTestTone(out, numOut, start, n);
}

// Adds the clips' audio for every segment where the song plays in this chunk.
void AudioEngine::renderClips(const std::vector<PlaybackSnapshot::Clip>& clips, juce::AudioBuffer<float>& buf, double bpm, double sr)
{
    for (int si = 0; si < numSegments; ++si) {
        const auto& seg = segments[(size_t)si];
        const auto songSample = (juce::int64)std::llround(seg.pos / model::kPPQ * 60.0 / bpm * sr);
        for (auto& c : clips) {
            if (!c.data || c.length <= 0) continue;
            const auto clipStart = (juce::int64)std::llround((double)c.start / model::kPPQ * 60.0 / bpm * sr);
            const juce::int64 rel0 = songSample - clipStart;               // position inside the clip at the segment start
            const auto from = (int)std::max<juce::int64>(0, -rel0);
            const auto to = (int)std::min<juce::int64>(seg.len, c.length - rel0);
            if (from >= to) continue;
            const auto& src = c.data->audio;
            const int srcCh = src.getNumChannels();
            const juce::int64 srcLen = src.getNumSamples();
            for (int ch = 0; ch < 2; ++ch) {
                const float* s = src.getReadPointer(std::min(ch, srcCh - 1));
                float* d = buf.getWritePointer(ch, seg.sampleAt);
                for (int i = from; i < to; ++i) {
                    const juce::int64 rel = rel0 + i;
                    const juce::int64 idx = c.offset + rel;
                    if (idx < 0 || idx >= srcLen) continue;
                    float g = c.gain;
                    if (rel < kClipFade) g *= (float)rel / kClipFade;
                    else if (c.length - rel < kClipFade) g *= (float)(c.length - rel) / kClipFade;
                    d[i] += s[idx] * g;
                }
            }
        }
    }
}

// Clicks on every beat in [fromTick, toTick); the bar's first beat is accented.
// originTick is the tick at sample `sampleAt` of this chunk.
void AudioEngine::scheduleClicks(double fromTick, double toTick, double tps, int sampleAt, int n, double originTick)
{
    const auto beat = (double)beatTicks_.load(), bar = (double)barTicks_.load();
    for (double t = std::ceil(fromTick / beat) * beat; t < toTick; t += beat) {
        if (numPendingClicks >= (int)pendingClicks.size()) break;
        const int off = juce::jlimit(0, n - 1, sampleAt + (int)((t - originTick) / tps));
        const bool accent = std::fmod(t, bar) < 0.5;
        pendingClicks[(size_t)numPendingClicks++] = {off, accent};
    }
}

void AudioEngine::renderClicks(float* const* out, int numOut, int start, int n)
{
    if (numPendingClicks == 0 && clickLeft <= 0) return;
    const double sr = sampleRate_.load();
    const float level = metronomeLevel_.load();
    const int len = (int)(sr * 0.045);              // 45 ms click
    const double decay = std::exp(-1.0 / (sr * 0.009));
    int next = 0;
    for (int i = 0; i < n; ++i) {
        while (next < numPendingClicks && pendingClicks[(size_t)next].offset == i) {
            const bool accent = pendingClicks[(size_t)next].accent;
            clickFreq = accent ? 1760.0 : 1175.0;
            clickGain = (accent ? 0.75 : 0.5) * level;
            clickPhase = 0;
            clickLeft = len;
            ++next;
        }
        if (clickLeft <= 0) continue;
        const float v = (float)(std::sin(clickPhase) * clickGain);
        clickPhase += juce::MathConstants<double>::twoPi * clickFreq / sr;
        clickGain *= decay;
        --clickLeft;
        for (int c = 0; c < std::min(numOut, 2); ++c) if (out[c]) out[c][start + i] += v;
    }
}

void AudioEngine::renderTestTone(float* const* out, int numOut, int start, int n)
{
    int left = testToneLeft_.load();
    if (left <= 0) return;
    const double sr = sampleRate_.load();
    const int total = (int)(sr * 0.8), fade = (int)(sr * 0.02);
    for (int i = 0; i < n && left > 0; ++i, --left) {
        const int pos = total - left;
        float g = 0.2f;
        if (pos < fade) g *= (float)pos / (float)fade;
        if (left < fade) g *= (float)left / (float)fade;
        const float v = (float)std::sin(testPhase) * g;
        testPhase += juce::MathConstants<double>::twoPi * 440.0 / sr;
        for (int c = 0; c < std::min(numOut, 2); ++c) if (out[c]) out[c][start + i] += v;
    }
    testToneLeft_ = left;
}

} // namespace mc::audio
