#include "audio/AudioExport.h"
#include "sequencer/Timing.h"
#if JUCE_MAC
 #include "audio/MacAacEncoder.h"
#endif

namespace mc::audio {

namespace {
// Order as offered in the dialog.
const std::vector<ExportFormatInfo> kFormats = {
    {ExportFormat::Wav, "wav", "WAV (uncompressed)", "wav", true, false},
    {ExportFormat::Mp3, "mp3", "MP3", "mp3", true, true},
    {ExportFormat::Ogg, "ogg", "OGG Vorbis", "ogg", true, true},
    {ExportFormat::Flac, "flac", "FLAC (lossless)", "flac", true, false},
    {ExportFormat::Midi, "midi", "MIDI (notes only)", "mid", false, false},
    {ExportFormat::M4a, "m4a", "M4A (AAC)", "m4a", true, true},
};

constexpr float kSilence = 1.0e-4f;        // -80 dBFS: the tail has died away
constexpr double kMaxTailSeconds = 20.0;   // never render more tail than this
constexpr double kQuietTailSeconds = 1.0;  // this much silence ends the tail
constexpr double kTailFadeSeconds = 0.05;  // kept (and faded out) after the last audible sample

// Progress shares of the stages
constexpr double kRenderShare = 0.80, kTailShare = 0.05; // encoding gets the rest

const juce::String kWriteFailed = "Could not write the file. Is the disk full?";

// ---------------------------------------------------------------- sinks
class Sink {
public:
    virtual ~Sink() = default;
    virtual bool write(const float* const* channels, int numSamples, juce::String& error) = 0;
    virtual bool finish(juce::String& error) = 0;
};

class JuceSink final : public Sink {
public:
    explicit JuceSink(std::unique_ptr<juce::AudioFormatWriter> w) : writer(std::move(w)) {}
    bool write(const float* const* channels, int n, juce::String& error) override
    {
        if (writer && writer->writeFromFloatArrays(channels, 2, n)) return true;
        error = kWriteFailed;
        return false;
    }
    bool finish(juce::String&) override
    {
        writer.reset(); // flushes, writes the header and closes the file
        return true;
    }
private:
    std::unique_ptr<juce::AudioFormatWriter> writer;
};

#if JUCE_MAC
class AacSink final : public Sink {
public:
    explicit AacSink(std::unique_ptr<MacAacEncoder> e) : enc(std::move(e)) {}
    bool write(const float* const* channels, int n, juce::String& error) override
    {
        std::string e;
        if (enc->write(channels, n, e)) return true;
        error = e;
        return false;
    }
    bool finish(juce::String& error) override
    {
        std::string e;
        if (enc->close(e)) return true;
        error = e;
        return false;
    }
private:
    std::unique_ptr<MacAacEncoder> enc;
};
#endif

// Stereo writer for any JUCE format. Handles both writer APIs (JUCE 8.0.9 changed it).
std::unique_ptr<juce::AudioFormatWriter> createWriter(juce::AudioFormat& format, const juce::File& file, double sampleRate,
                                                      int bits, int quality, const juce::String& title, juce::String& error)
{
    file.deleteFile(); // FileOutputStream appends to an existing file
    std::unique_ptr<juce::OutputStream> os(file.createOutputStream());
    if (!os) { error = "Cannot write to " + file.getFullPathName(); return nullptr; }
#if JUCE_VERSION >= 0x80009
    std::unordered_map<juce::String, juce::String> meta;
    if (title.isNotEmpty() && dynamic_cast<juce::OggVorbisAudioFormat*>(&format)) meta[juce::OggVorbisAudioFormat::id3title] = title;
    auto w = format.createWriterFor(os, juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(2)
                                            .withBitsPerSample(bits).withQualityOptionIndex(quality).withMetadataValues(meta));
#else
    juce::StringPairArray meta;
    if (title.isNotEmpty() && dynamic_cast<juce::OggVorbisAudioFormat*>(&format)) meta.set(juce::OggVorbisAudioFormat::id3title, title);
    std::unique_ptr<juce::AudioFormatWriter> w(format.createWriterFor(os.get(), sampleRate, 2u, bits, meta, quality));
    if (w) os.release(); // owned by the writer
#endif
    if (!w) error = "Cannot create a " + format.getFormatName() + " file with these settings ("
                    + juce::String(sampleRate / 1000.0, 1) + " kHz, " + juce::String(bits) + " bit).";
    return w;
}

int oggQualityIndex(int kbps)
{
    // JUCE's options are "64 kbps" ... "500 kbps"; pick the nearest.
    const auto options = juce::OggVorbisAudioFormat().getQualityOptions();
    int best = 0, bestDiff = 1 << 30;
    for (int i = 0; i < options.size(); ++i)
        if (const int d = std::abs(options[i].getIntValue() - kbps); d < bestDiff) { best = i; bestDiff = d; }
    return best;
}

// ---------------------------------------------------------------- external encoders
bool isLame(const juce::File& f) { return f.getFileNameWithoutExtension().containsIgnoreCase("lame"); }

juce::File findProgram(const juce::String& name)
{
#if JUCE_WINDOWS
    const juce::String exe = name + ".exe", sep = ";";
#else
    const juce::String exe = name, sep = ":";
#endif
    juce::StringArray dirs;
    dirs.addTokens(juce::SystemStats::getEnvironmentVariable("PATH", {}), sep, "\"");
    // Apps started from the Finder/desktop get a minimal PATH: also look where installers put these.
#if JUCE_MAC
    for (auto* d : {"/opt/homebrew/bin", "/usr/local/bin", "/opt/local/bin", "/usr/bin"}) dirs.add(d);
#elif JUCE_WINDOWS
    for (auto* v : {"ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const auto base = juce::SystemStats::getEnvironmentVariable(v, {});
        if (base.isNotEmpty()) { dirs.add(base + "\\ffmpeg\\bin"); dirs.add(base + "\\lame"); }
    }
    dirs.add("C:\\ffmpeg\\bin");
#else
    for (auto* d : {"/usr/bin", "/usr/local/bin", "/snap/bin", "/var/lib/flatpak/exports/bin"}) dirs.add(d);
#endif
    for (auto d : dirs) {
        d = d.trim().unquoted();
        if (d.isEmpty() || !juce::File::isAbsolutePath(d)) continue;
        const auto f = juce::File(d).getChildFile(exe);
        if (f.existsAsFile()) return f;
    }
    return {};
}

// Runs the encoder on a finished 24-bit WAV. Polls so the export can be cancelled.
bool runEncoder(const juce::File& tool, const ExportOptions& o, const juce::File& in, const juce::File& out,
                const std::function<bool()>& keepGoing, bool& cancelled, juce::String& error)
{
    juce::StringArray args;
    auto add = [&](std::initializer_list<juce::String> l) { for (auto& s : l) args.add(s); };
    const juce::String kbps(o.bitrateKbps);
    args.add(tool.getFullPathName());
    if (isLame(tool)) {
        add({"--silent", "--cbr", "-b", kbps, "-q", "2"});
        if (o.title.isNotEmpty()) add({"--tt", o.title});
        add({in.getFullPathName(), out.getFullPathName()});
    } else { // FFmpeg
        add({"-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-i", in.getFullPathName(), "-vn"});
        if (o.format == ExportFormat::Mp3) add({"-c:a", "libmp3lame", "-b:a", kbps + "k", "-id3v2_version", "3"});
        else add({"-c:a", "aac", "-b:a", kbps + "k", "-movflags", "+faststart"});
        if (o.title.isNotEmpty()) add({"-metadata", "title=" + o.title});
        add({"-f", o.format == ExportFormat::Mp3 ? "mp3" : "ipod", out.getFullPathName()}); // "ipod" = the .m4a flavour of MP4
    }
    juce::ChildProcess p;
    if (!p.start(args)) { error = "Could not start the encoder:\n" + tool.getFullPathName(); return false; }
    while (p.isRunning()) {
        if (!keepGoing()) { p.kill(); cancelled = true; return false; }
        juce::Thread::sleep(40);
    }
    const auto output = p.readAllProcessOutput().trim();
    if (p.getExitCode() != 0 || !out.existsAsFile() || out.getSize() == 0) {
        error = tool.getFileName() + " could not encode the file";
        error << (output.isNotEmpty() ? ":\n\n" + output.substring(0, 800) : juce::String("."));
        if (!isLame(tool) && output.containsIgnoreCase("libmp3lame"))
            error << "\n\nThis FFmpeg was built without MP3 support. Install LAME, or an FFmpeg build that includes libmp3lame.";
        return false;
    }
    return true;
}
} // namespace

// ---------------------------------------------------------------- formats
const std::vector<ExportFormatInfo>& exportFormats() { return kFormats; }

const ExportFormatInfo& formatInfo(ExportFormat f)
{
    for (auto& i : kFormats) if (i.format == f) return i;
    return kFormats.front();
}

ExportFormat formatFromKey(const juce::String& key, ExportFormat fallback)
{
    for (auto& i : kFormats) if (key == i.key) return i.format;
    return fallback;
}

double maxSampleRate(ExportFormat f)
{
    return f == ExportFormat::Mp3 || f == ExportFormat::M4a ? 48000.0 : 192000.0;
}

std::vector<int> bitDepths(ExportFormat f)
{
    if (f == ExportFormat::Wav) return {16, 24, 32};
    if (f == ExportFormat::Flac) return {16, 24};
    return {};
}

std::vector<int> bitRates(ExportFormat f)
{
    if (f == ExportFormat::Mp3 || f == ExportFormat::M4a) return {128, 160, 192, 256, 320};
    if (f == ExportFormat::Ogg) return {96, 128, 160, 192, 256, 320, 500};
    return {};
}

bool needsExternalEncoder(ExportFormat f)
{
#if JUCE_MAC
    return f == ExportFormat::Mp3;
#else
    return f == ExportFormat::Mp3 || f == ExportFormat::M4a;
#endif
}

bool isUsableEncoder(ExportFormat f, const juce::File& tool)
{
    if (!tool.existsAsFile()) return false;
    return f == ExportFormat::Mp3 || !isLame(tool); // LAME only writes MP3
}

juce::File findEncoder(ExportFormat f, const juce::File& userChoice)
{
    if (isUsableEncoder(f, userChoice)) return userChoice;
    if (f == ExportFormat::Mp3)
        if (auto lame = findProgram("lame"); lame.existsAsFile()) return lame;
    return findProgram("ffmpeg");
}

juce::String encoderInstallHint(ExportFormat f)
{
    const bool mp3 = f == ExportFormat::Mp3;
    juce::String s;
    s << (mp3 ? "MP3" : "M4A (AAC)") << " files are made by a free encoder program that is installed separately: "
      << (mp3 ? "LAME or FFmpeg." : "FFmpeg.") << "\n\n";
#if JUCE_MAC
    s << "Install it with Homebrew (https://brew.sh) in Terminal:\n    brew install " << (mp3 ? "lame" : "ffmpeg");
#elif JUCE_WINDOWS
    s << "Download FFmpeg from https://ffmpeg.org/download.html, unpack it, then click \"Locate encoder...\" "
         "and choose ffmpeg.exe in its bin folder.";
#else
    s << "Install it with your package manager, for example:\n    sudo apt install " << (mp3 ? "lame" : "ffmpeg");
#endif
    s << "\n\nIf it is installed somewhere else, click \"Locate encoder...\" and choose the program.";
    return s;
}

// ---------------------------------------------------------------- rendering
ExportResult renderSong(RenderSession& s, const ExportOptions& o, const juce::File& target, const juce::File& encoder,
                        const std::function<bool(double, const juce::String&)>& progress)
{
    ExportResult r;
    auto cancel = [&r] { r.cancelled = true; r.error = "Export cancelled."; return r; };
    if (!s.engine || !s.snapshot || s.end <= s.start) { r.error = "There is nothing to export."; return r; }
    const auto& info = formatInfo(o.format);
    const double sr = s.sampleRate;

    // 1. audio clips, decoded at the export rate
    if (!progress(0.0, "Loading audio files...")) return cancel();
    s.snapshot->clips.resize(s.snapshot->seq.tracks.size());
    for (auto& pending : s.clips) {
        const auto& file = pending.clip.file;
        auto it = s.cachedAudio.find(file);
        if (it == s.cachedAudio.end()) it = s.cachedAudio.emplace(file, AudioFileCache::decode(file, sr)).first;
        if (!it->second) {
            const auto msg = "Audio file missing or unreadable: " + juce::File(juce::String::fromUTF8(file.c_str())).getFileName();
            s.warnings.addIfNotAlreadyThere(msg);
            continue;
        }
        if (pending.track >= s.snapshot->clips.size()) continue;
        if (auto pc = PlaybackSnapshot::makeClip(pending.clip, it->second)) s.snapshot->clips[pending.track].push_back(std::move(*pc));
    }
    s.engine->setSnapshot(s.snapshot);

    // 2. render the mix into a 32-bit float file (lossless, no clipping, peak measured)
    juce::TemporaryFile mixFile(".wav");
    juce::WavAudioFormat wav;
    juce::String error;
    auto mixWriter = createWriter(wav, mixFile.getFile(), sr, 32, 0, {}, error);
    if (!mixWriter) { r.error = error; return r; }

    auto& eng = *s.engine;
    eng.setTempo(s.bpm);
    eng.setLoop(false, 0, 0);
    eng.setMetronome(false);
    eng.setPosition(s.start);
    const int block = eng.blockSize();
    juce::AudioBuffer<float> buf(2, block);
    // One block with the transport stopped: volume/pan ramps reach their values and plugins
    // warm up, so the first note is not faded in.
    eng.renderOffline(buf.getArrayOfWritePointers(), 2, block);
    eng.play();

    const double tps = seq::ticksPerSample(s.bpm, sr);
    const auto songLen = std::max<juce::int64>(1, (juce::int64)std::ceil((double)(s.end - s.start) / tps));
    float peak = 0.0f;
    juce::int64 total = 0;
    for (juce::int64 done = 0; done < songLen;) {
        const int n = (int)std::min<juce::int64>(block, songLen - done);
        eng.renderOffline(buf.getArrayOfWritePointers(), 2, n);
        peak = std::max(peak, buf.getMagnitude(0, n));
        const float* ch[2] = {buf.getReadPointer(0), buf.getReadPointer(1)};
        if (!mixWriter->writeFromFloatArrays(ch, 2, n)) { r.error = kWriteFailed; return r; }
        done += n;
        if (!progress(kRenderShare * (double)done / (double)songLen, "Rendering the song...")) return cancel();
    }
    total = songLen;
    eng.stop(); // note-offs: instruments release, reverbs ring out

    if (o.includeTail) {
        const auto maxLen = (juce::int64)(kMaxTailSeconds * sr), quiet = (juce::int64)(kQuietTailSeconds * sr);
        juce::AudioBuffer<float> tail(2, (int)maxLen + block);
        juce::int64 have = 0, audibleEnd = 0; // audibleEnd: just after the last audible sample
        while (have < maxLen && have - audibleEnd < quiet) {
            const int n = (int)std::min<juce::int64>(block, maxLen - have);
            float* ch[2] = {tail.getWritePointer(0, (int)have), tail.getWritePointer(1, (int)have)};
            eng.renderOffline(ch, 2, n);
            for (int i = 0; i < n; ++i)
                if (std::abs(ch[0][i]) > kSilence || std::abs(ch[1][i]) > kSilence) audibleEnd = have + i + 1;
            have += n;
            if (!progress(kRenderShare + kTailShare * (double)have / (double)maxLen, "Rendering the reverb tail...")) return cancel();
        }
        const auto keep = (int)std::min(have, audibleEnd + (juce::int64)(kTailFadeSeconds * sr));
        if (keep > audibleEnd)
            for (int c = 0; c < 2; ++c) tail.applyGainRamp(c, (int)audibleEnd, keep - (int)audibleEnd, 1.0f, 0.0f);
        if (keep > 0) {
            peak = std::max(peak, tail.getMagnitude(0, keep));
            const float* ch[2] = {tail.getReadPointer(0), tail.getReadPointer(1)};
            if (!mixWriter->writeFromFloatArrays(ch, 2, keep)) { r.error = kWriteFailed; return r; }
            total += keep;
        }
    }
    mixWriter.reset(); // complete the file
    r.seconds = (double)total / sr;
    r.peak = peak;

    // 3. encode: level, then the chosen format
    const float gain = o.normalize && peak > 1.0e-6f ? juce::Decibels::decibelsToGain(-1.0f) / peak : 1.0f;
    const bool floatOut = o.format == ExportFormat::Wav && o.bitDepth == 32;
    r.clipped = !floatOut && peak * gain > 1.0f;

    juce::TemporaryFile out(target); // next to the target; replaces it only when everything worked
    std::unique_ptr<juce::TemporaryFile> pcm; // 24-bit WAV for an external encoder
    std::unique_ptr<Sink> sink;
    auto juceSink = [&](juce::AudioFormat& fmt, const juce::File& f, int bits, int quality) {
        if (auto w = createWriter(fmt, f, sr, bits, quality, o.title, error)) sink = std::make_unique<JuceSink>(std::move(w));
    };
    juce::FlacAudioFormat flac;
    juce::OggVorbisAudioFormat ogg;
    const bool external = needsExternalEncoder(o.format);
    if (external) {
        if (!isUsableEncoder(o.format, encoder)) { r.error = "No " + juce::String(info.name) + " encoder found.\n\n" + encoderInstallHint(o.format); return r; }
        pcm = std::make_unique<juce::TemporaryFile>(".wav");
        juceSink(wav, pcm->getFile(), 24, 0);
    } else {
        switch (o.format) {
            case ExportFormat::Wav: juceSink(wav, out.getFile(), o.bitDepth == 16 || o.bitDepth == 32 ? o.bitDepth : 24, 0); break;
            case ExportFormat::Flac: juceSink(flac, out.getFile(), o.bitDepth == 16 ? 16 : 24, juce::jlimit(0, 8, o.flacLevel)); break;
            case ExportFormat::Ogg: juceSink(ogg, out.getFile(), 32, oggQualityIndex(o.bitrateKbps)); break;
            case ExportFormat::M4a: {
#if JUCE_MAC
                std::string e;
                if (auto enc = MacAacEncoder::create(out.getFile().getFullPathName().toStdString(), sr, 2, o.bitrateKbps * 1000, e))
                    sink = std::make_unique<AacSink>(std::move(enc));
                else error = e;
#endif
                break;
            }
            case ExportFormat::Mp3:
            case ExportFormat::Midi: break;
        }
    }
    if (!sink) { r.error = error.isNotEmpty() ? error : "Cannot export this format."; return r; }

    const auto encodeTo = external ? 0.90 : 1.0;
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(mixFile.getFile()));
        if (!reader) { r.error = "Could not read the rendered audio back."; return r; }
        const int chunk = 16384;
        juce::AudioBuffer<float> b(2, chunk);
        const auto len = reader->lengthInSamples;
        for (juce::int64 pos = 0; pos < len; pos += chunk) {
            const int n = (int)std::min<juce::int64>(chunk, len - pos);
            reader->read(&b, 0, n, pos, true, true);
            if (gain != 1.0f) b.applyGain(0, n, gain);
            const float* ch[2] = {b.getReadPointer(0), b.getReadPointer(1)};
            if (!sink->write(ch, n, error)) { r.error = error; return r; }
            const double f = kRenderShare + kTailShare + (encodeTo - kRenderShare - kTailShare) * (double)(pos + n) / (double)len;
            if (!progress(f, "Writing " + juce::String(info.name) + "...")) return cancel();
        }
    }
    if (!sink->finish(error)) { r.error = error; return r; }
    sink.reset();

    if (external) {
        if (!progress(encodeTo, "Encoding " + juce::String(info.name) + " with " + encoder.getFileNameWithoutExtension() + "...")) return cancel();
        bool cancelled = false;
        if (!runEncoder(encoder, o, pcm->getFile(), out.getFile(), [&] { return progress(encodeTo, {}); }, cancelled, error)) {
            if (cancelled) return cancel();
            r.error = error;
            return r;
        }
    }
    if (!out.overwriteTargetFileWithTemporary()) { r.error = "Could not save " + target.getFullPathName(); return r; }
    progress(1.0, "Done");
    r.ok = true;
    return r;
}

} // namespace mc::audio
