#pragma once
// Song export ("mixdown"): renders the whole song (or the loop region) offline through
// private instances of every instrument, effect and channel strip, exactly as it plays,
// and writes it as WAV, FLAC, Ogg Vorbis, MP3 or M4A (AAC).
//
//   WAV, FLAC, Ogg Vorbis  encoded by JUCE (built in)
//   M4A (AAC)              macOS: the system's AAC encoder; elsewhere FFmpeg
//   MP3                    LAME or FFmpeg (external programs; MP3 encoders cannot be bundled freely)
//
// MIDI export is listed here too, so the dialog can offer every format in one place;
// it is written by io::MidiFile.
#include "audio/AudioEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>
#include <map>
#include <vector>

namespace mc::audio {

enum class ExportFormat { Wav, Mp3, Ogg, Flac, Midi, M4a };

struct ExportFormatInfo {
    ExportFormat format;
    const char* key;        // stable id for the settings ("wav", "mp3", ...)
    const char* name;       // shown in the dialog
    const char* extension;  // without the dot
    bool audio;             // false: MIDI
    bool lossy;             // bit rate instead of bit depth
};
const std::vector<ExportFormatInfo>& exportFormats();
const ExportFormatInfo& formatInfo(ExportFormat);
ExportFormat formatFromKey(const juce::String& key, ExportFormat fallback = ExportFormat::Wav);

// Highest sample rate a format can be written at (MP3 and AAC stop at 48 kHz).
double maxSampleRate(ExportFormat);
// Bit depths a lossless format offers (empty for lossy formats and MIDI). WAV 32 = float.
std::vector<int> bitDepths(ExportFormat);
// Bit rates (kbps) a lossy format offers.
std::vector<int> bitRates(ExportFormat);

struct ExportOptions {
    ExportFormat format = ExportFormat::Wav;
    double sampleRate = 44100.0;
    int bitDepth = 24;        // WAV: 16, 24, 32 (float); FLAC: 16, 24
    int bitrateKbps = 256;    // MP3, M4A, Ogg Vorbis
    int flacLevel = 5;        // FLAC compression 0 (fastest) .. 8 (smallest)
    bool loopOnly = false;    // only the loop region
    bool includeTail = true;  // let reverb and note releases ring out after the end
    bool normalize = false;   // loudest peak at -1 dBFS
    juce::String title;       // tag, where the format has one
};

// Everything needed to render the project offline. Built on the message thread
// (AppContext::createRenderSession), rendered on a worker thread, destroyed on the
// message thread (plugins must be created and deleted there).
struct RenderSession {
    struct PendingClip {
        size_t track = 0;                 // index into snapshot->seq.tracks / clips
        model::AudioClip clip;
    };
    // Declared before the engine, so they are destroyed after it (the engine only borrows them).
    std::vector<std::unique_ptr<plugins::InstrumentPlugin>> effects;
    std::unique_ptr<AudioEngine> engine;
    std::shared_ptr<PlaybackSnapshot> snapshot; // handed to the engine once the clips are loaded
    std::vector<PendingClip> clips;
    // Decoded audio already in memory at the export rate (saves decoding the file again).
    std::map<std::string, std::shared_ptr<const AudioFileData>> cachedAudio;
    model::Tick start = 0, end = 0;
    double bpm = 120.0, sampleRate = 44100.0;
    juce::StringArray warnings; // instruments/effects that could not be loaded, unreadable audio files
};

struct ExportResult {
    bool ok = false, cancelled = false;
    juce::String error;
    double seconds = 0;          // length of the written audio
    float peak = 0;              // loudest sample before normalising (linear)
    bool clipped = false;        // peaks above 0 dBFS were cut off (fixed-point and lossy formats)
};

// Worker thread. `progress(fraction 0..1, stage)` returns false to cancel.
// `encoder` is the external program for MP3 (and M4A outside macOS); see findEncoder().
ExportResult renderSong(RenderSession&, const ExportOptions&, const juce::File& target, const juce::File& encoder,
                        const std::function<bool(double, const juce::String&)>& progress);

// External encoders --------------------------------------------------------------------
bool needsExternalEncoder(ExportFormat);
// Usable encoder for the format: the user's choice if it fits, else LAME (MP3 only) or
// FFmpeg from the PATH and the usual install folders. Empty if none is installed.
juce::File findEncoder(ExportFormat, const juce::File& userChoice);
bool isUsableEncoder(ExportFormat, const juce::File&);
juce::String encoderInstallHint(ExportFormat); // how to get one, per platform

} // namespace mc::audio
