#pragma once
// Realtime engine: runs the sequencer inside the audio callback, routes
// sequenced + live MIDI to each track's instrument, plays audio clips, takes
// audio input (monitoring + recording) and mixes every track through its
// channel strip (volume, pan, mute/solo, meter) into the Mains bus.
// The audio thread never touches the Project; it consumes immutable snapshots,
// atomics and a lock-free FIFO. No allocations happen on the audio thread.
#include "audio/AudioFileCache.h"
#include "dsp/Compressor.h"
#include "dsp/Equalizer.h"
#include "dsp/Reverb.h"
#include "plugins/InstrumentPlugin.h"
#include "sequencer/Sequence.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <array>
#include <bitset>
#include <optional>

namespace mc::audio {

// Everything the audio thread needs to play the project.
struct PlaybackSnapshot {
    struct Clip {
        std::shared_ptr<const AudioFileData> data;
        model::Tick start = 0;
        juce::int64 offset = 0, length = 0; // samples at the device rate
        float gain = 1.0f;
    };
    seq::SequenceSnapshot seq;
    std::vector<std::vector<Clip>> clips; // parallel to seq.tracks (empty for MIDI tracks)

    // Engine clip for a project clip, from its audio decoded at the engine rate. Shared by
    // playback and song export so both cut clips identically. nullopt: nothing to play.
    static std::optional<Clip> makeClip(const model::AudioClip&, std::shared_ptr<const AudioFileData>);
};

class AudioEngine final : private juce::AudioIODeviceCallback {
public:
    explicit AudioEngine(juce::AudioDeviceManager&);
    // Offline engine (song export): not attached to a sound device. It is driven by
    // renderOffline() from one thread, as fast as the computer can render.
    AudioEngine(double sampleRate, int blockSize);
    ~AudioEngine() override;

    // ---- offline rendering (offline engines only). Renders the next numSamples of the
    // song exactly like the device callback would, into numOut (1 or 2) channels.
    void renderOffline(float* const* out, int numOut, int numSamples);

    // ---- message thread
    void setSnapshot(std::shared_ptr<const PlaybackSnapshot>);
    // Creates the track's channel strip if needed. Returns the previous instrument
    // so it is destroyed on the caller's thread. nullptr = no instrument (audio tracks).
    std::unique_ptr<plugins::InstrumentPlugin> setInstrument(model::TrackId, std::unique_ptr<plugins::InstrumentPlugin>);
    plugins::InstrumentPlugin* instrument(model::TrackId) const;
    bool hasTrack(model::TrackId id) const { return findSlot(id) != nullptr; }
    void setBypassed(model::TrackId, bool);
    std::unique_ptr<plugins::InstrumentPlugin> removeTrack(model::TrackId);
    std::vector<model::TrackId> trackIds() const;
    // Effect plugins of a track's channel strip (owned by the caller, which must keep them
    // alive until they are replaced or the track is removed). nullptr entries are skipped.
    void setInserts(model::TrackId, std::vector<plugins::InstrumentPlugin*>);

    double sampleRate() const { return sampleRate_.load(); }
    int blockSize() const { return blockSize_.load(); }
    int numInputChannels() const { return numInputs_.load(); }   // active device inputs
    int numOutputChannels() const { return numOutputs_.load(); }
    bool isDeviceRunning() const { return running_.load(); }

    // ---- meters (message thread). Peak levels since the previous call (linear).
    struct Levels { float left = 0, right = 0, input = 0, gainReduction = 0; }; // gain reduction in dB (compressor)
    Levels takeTrackLevels(model::TrackId);
    Levels takeMasterLevels();

    // ---- audio recording (message thread). Writers belong to armed audio tracks;
    // samples are written from the moment the song position moves (after the count-in).
    using Writer = juce::AudioFormatWriter::ThreadedWriter;
    void startAudioRecording(std::vector<std::pair<model::TrackId, std::unique_ptr<Writer>>>);
    // Stops writing and flushes/closes all files. Returns the song position (ticks)
    // where the recording started, or -1 if nothing was written.
    double stopAudioRecording();
    bool isRecordingAudio() const { return audioRecording_.load(); }
    double audioRecordStart() const { return recordStartPos_.load(); } // -1 until the first sample is written
    int roundTripLatency() const { return latency_.load(); }          // input + output latency, samples

    // ---- transport (any thread)
    void play() { playing_ = true; }
    // Plays `ticks` of metronome count-in (song position stays put), then starts the transport.
    void playWithCountIn(model::Tick ticks)
    {
        countInTotal_ = (double)std::max<model::Tick>(0, ticks);
        countInLeft_ = countInTotal_.load();
        playing_ = true;
    }
    bool isCountingIn() const { return playing_.load() && countInLeft_.load() > 0; }
    double countInRemaining() const { return countInLeft_.load(); } // ticks
    void stop() { stopRequested_ = true; }
    bool isPlaying() const { return playing_.load(); }
    void setPosition(model::Tick t) { pendingSeek_ = (double)std::max<model::Tick>(0, t); }
    double position() const { return position_.load(); }
    void setTempo(double bpm) { tempo_ = juce::jlimit(10.0, 999.0, bpm); }
    void setLoop(bool enabled, model::Tick start, model::Tick end);

    // ---- metronome (any thread). Clicks are synthesized here; no instrument needed.
    void setMetronome(bool on) { metronomeOn_ = on; }
    void setMetronomeLevel(float g) { metronomeLevel_ = juce::jlimit(0.0f, 1.0f, g); }
    model::Tick beatTicks() const { return beatTicks_.load(); }
    void setMeter(model::Tick beat, model::Tick bar) { beatTicks_ = std::max<model::Tick>(1, beat); barTicks_ = std::max<model::Tick>(1, bar); }

    // A short test tone straight to the output ("Is my sound working?").
    void playTestTone() { testToneLeft_ = (int)(sampleRate_.load() * 0.8); }

    // ---- live input (any thread, lock-free for the audio thread)
    void pushLive(model::TrackId, const juce::MidiMessage&);

private:
    struct Slot {
        model::TrackId id = 0;
        std::unique_ptr<plugins::InstrumentPlugin> plugin;
        std::atomic<bool> bypassed{false};
        juce::MidiBuffer midi;
        std::vector<plugins::InstrumentPlugin*> inserts; // effect plugins (not owned)
        juce::MidiBuffer fxMidi;                          // always empty: effects get no notes
        std::bitset<16 * 128> held;    // notes sent by the sequencer
        std::bitset<16> pedalDown;     // channels whose sustain pedal the sequencer holds down
        bool chasePedal = true;        // send the sustain line's pedal position at the next sequencer step
        std::uint64_t seqGeneration = 0; // snapshot the held notes and pedal were last checked against
        float gainL = 0, gainR = 0;    // audio thread: current (smoothed) strip gains
        std::atomic<float> peakL{0}, peakR{0}, peakIn{0}, gainReduction{0};
        // channel strip effects (audio thread; prepared on the message/device thread)
        dsp::Equalizer eq;
        std::uint64_t eqGeneration = 0; // snapshot generation the EQ coefficients were made from
        bool eqOn = false;
        dsp::Compressor comp;
        dsp::Reverb reverb;
        model::CompressorSettings compSettings;
        model::ReverbSettings reverbSettings;
        bool compOn = false, reverbOn = false;
        std::atomic<Writer*> writer{nullptr};
        std::unique_ptr<Writer> writerOwner; // message thread
    };
    struct LiveEvent { model::TrackId track; std::uint8_t data[3]; int size; };
    struct Segment { int sampleAt, len; double pos; }; // song position at sampleAt (chunk-relative)

    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int, int,
                                          const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void renderChunk(const float* const* in, int numIn, float* const* out, int numOut, int start, int n);
    void runInserts(Slot&, const seq::TrackSequence*, juce::AudioBuffer<float>&);
    void renderClips(const std::vector<PlaybackSnapshot::Clip>&, juce::AudioBuffer<float>& buf, double bpm, double sr);
    void scheduleClicks(double fromTick, double toTick, double tps, int sampleAt, int n, double originTick);
    void renderClicks(float* const* out, int numOut, int start, int n);
    void renderTestTone(float* const* out, int numOut, int start, int n);
    static void releaseHeld(Slot&, int sampleOffset);
    static void storePeak(std::atomic<float>& a, float v) { if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed); }
    Slot* findSlot(model::TrackId) const;

    juce::AudioDeviceManager* devices = nullptr; // nullptr: offline engine
    mutable juce::CriticalSection slotLock;
    std::vector<std::unique_ptr<Slot>> slots;

    juce::SpinLock snapshotLock;
    std::shared_ptr<const PlaybackSnapshot> pendingSnapshot, audioSnapshot;
    std::uint64_t snapshotGeneration = 1; // audio thread: +1 for every new snapshot
    std::vector<std::shared_ptr<const PlaybackSnapshot>> retired; // freed on message thread

    juce::SpinLock livePushLock;
    juce::AbstractFifo liveFifo{2048};
    std::array<LiveEvent, 2048> liveBuffer{};

    std::atomic<double> sampleRate_{44100.0};
    std::atomic<int> blockSize_{512}, numInputs_{0}, numOutputs_{0}, latency_{0};
    std::atomic<bool> running_{false};
    std::atomic<bool> playing_{false}, stopRequested_{false};
    std::atomic<double> position_{0}, pendingSeek_{-1}, tempo_{120};
    std::atomic<bool> loopEnabled_{false};
    std::atomic<model::Tick> loopStart_{0}, loopEnd_{0};
    juce::AudioBuffer<float> mixBuffer, busBuffer, silence;

    // master
    float masterGainL = 1.0f, masterGainR = 1.0f; // audio thread, smoothed
    std::atomic<float> masterPeakL{0}, masterPeakR{0};

    // recording
    std::atomic<bool> audioRecording_{false};
    std::atomic<double> recordStartPos_{-1};

    // audio thread: segments of the current chunk where the song plays
    std::array<Segment, 16> segments{};
    int numSegments = 0;

    std::atomic<bool> metronomeOn_{false};
    std::atomic<float> metronomeLevel_{0.6f};
    std::atomic<model::Tick> beatTicks_{960}, barTicks_{3840};
    std::atomic<double> countInTotal_{0}, countInLeft_{0};
    // audio-thread only: click voice + clicks scheduled in the current chunk
    struct PendingClick { int offset; bool accent; };
    std::array<PendingClick, 8> pendingClicks{};
    int numPendingClicks = 0;
    double clickPhase = 0, clickFreq = 0, clickGain = 0;
    int clickLeft = 0;
    std::atomic<int> testToneLeft_{0};
    double testPhase = 0;
};

} // namespace mc::audio
