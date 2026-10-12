#include "audio/MacAacEncoder.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <cctype>
#include <vector>

namespace mc::audio {

namespace {
std::string describe(const char* what, OSStatus s)
{
    // Core Audio errors are often four printable characters ('fmt?', 'wht?', ...).
    const auto u = (std::uint32_t)s;
    const char c[4] = {(char)(u >> 24), (char)(u >> 16), (char)(u >> 8), (char)u};
    bool printable = true;
    for (char ch : c) printable = printable && std::isprint((unsigned char)ch);
    std::string code = printable ? "'" + std::string(c, 4) + "'" : std::to_string((long)s);
    return std::string(what) + " failed (Core Audio error " + code + ")";
}
} // namespace

struct MacAacEncoder::Impl {
    ExtAudioFileRef file = nullptr;
    int channels = 2;
    std::vector<std::uint8_t> listMemory; // AudioBufferList with `channels` buffers
};

MacAacEncoder::~MacAacEncoder()
{
    std::string ignored;
    close(ignored);
}

std::unique_ptr<MacAacEncoder> MacAacEncoder::create(const std::string& path, double sampleRate, int numChannels,
                                                     int bitrate, std::string& error)
{
    if (numChannels < 1 || numChannels > 2) { error = "AAC export supports mono and stereo only"; return nullptr; }
    if (sampleRate > 48000.0) { error = "AAC (M4A) export supports sample rates up to 48 kHz"; return nullptr; }

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.data(), (CFIndex)path.size(), false);
    if (!url) { error = "Invalid file name: " + path; return nullptr; }

    AudioStreamBasicDescription dst{};
    dst.mSampleRate = sampleRate;
    dst.mFormatID = kAudioFormatMPEG4AAC;
    dst.mChannelsPerFrame = (UInt32)numChannels;
    UInt32 size = sizeof(dst);
    AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, 0, nullptr, &size, &dst); // fills in the rest

    ExtAudioFileRef file = nullptr;
    OSStatus s = ExtAudioFileCreateWithURL(url, kAudioFileM4AType, &dst, nullptr, kAudioFileFlags_EraseFile, &file);
    CFRelease(url);
    if (s != noErr || !file) { error = describe("Creating the M4A file", s); return nullptr; }

    // What we hand over: non-interleaved 32-bit float.
    AudioStreamBasicDescription client{};
    client.mSampleRate = sampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved | kAudioFormatFlagsNativeEndian;
    client.mBitsPerChannel = 32;
    client.mChannelsPerFrame = (UInt32)numChannels;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = 4; // per channel (non-interleaved)
    client.mBytesPerPacket = 4;
    s = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(client), &client);
    if (s != noErr) { ExtAudioFileDispose(file); error = describe("Setting up the AAC encoder", s); return nullptr; }

    // Bit rate + best quality. Unsupported values are ignored (the encoder keeps its default).
    AudioConverterRef conv = nullptr;
    size = sizeof(conv);
    if (ExtAudioFileGetProperty(file, kExtAudioFileProperty_AudioConverter, &size, &conv) == noErr && conv) {
        UInt32 br = (UInt32)bitrate;
        AudioConverterSetProperty(conv, kAudioConverterEncodeBitRate, sizeof(br), &br);
        UInt32 quality = kAudioConverterQuality_Max;
        AudioConverterSetProperty(conv, kAudioConverterCodecQuality, sizeof(quality), &quality);
        CFArrayRef config = nullptr; // tells ExtAudioFile to pick up the converter changes
        ExtAudioFileSetProperty(file, kExtAudioFileProperty_ConverterConfig, sizeof(config), &config);
    }

    std::unique_ptr<MacAacEncoder> e(new MacAacEncoder());
    e->impl = std::make_unique<Impl>();
    e->impl->file = file;
    e->impl->channels = numChannels;
    e->impl->listMemory.resize(offsetof(AudioBufferList, mBuffers) + sizeof(AudioBuffer) * (size_t)numChannels);
    return e;
}

bool MacAacEncoder::write(const float* const* channels, int numSamples, std::string& error)
{
    if (!impl || !impl->file) { error = "The M4A file is closed"; return false; }
    if (numSamples <= 0) return true;
    auto* list = reinterpret_cast<AudioBufferList*>(impl->listMemory.data());
    list->mNumberBuffers = (UInt32)impl->channels;
    for (int c = 0; c < impl->channels; ++c) {
        list->mBuffers[c].mNumberChannels = 1;
        list->mBuffers[c].mDataByteSize = (UInt32)numSamples * sizeof(float);
        list->mBuffers[c].mData = const_cast<float*>(channels[c]); // only read by the encoder
    }
    const OSStatus s = ExtAudioFileWrite(impl->file, (UInt32)numSamples, list);
    if (s != noErr) { error = describe("Encoding AAC", s); return false; }
    return true;
}

bool MacAacEncoder::close(std::string& error)
{
    if (!impl || !impl->file) return true;
    const OSStatus s = ExtAudioFileDispose(impl->file);
    impl->file = nullptr;
    if (s != noErr) { error = describe("Finishing the M4A file", s); return false; }
    return true;
}

} // namespace mc::audio
