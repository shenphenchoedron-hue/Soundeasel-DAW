#pragma once
// Settings of the built-in channel strip effects (parametric EQ, compressor and
// reverb per track). Pure data: saved with the project, edited through commands.
#include <vector>

namespace mc::model {

struct EqBand {
    enum class Type { Bell, LowShelf, HighShelf, LowCut, HighCut };
    Type type = Type::Bell;
    float freq = 1000.0f;   // Hz, 20 .. 20000
    float gainDb = 0.0f;    // -24 .. 24 (not used by the cut filters)
    float q = 1.0f;         // 0.1 .. 18 (resonance for the cut filters)
    bool enabled = true;
    bool operator==(const EqBand&) const = default;
};

struct EqSettings {
    static constexpr int kMaxBands = 8;
    bool enabled = false;
    std::vector<EqBand> bands;  // at most kMaxBands
    bool operator==(const EqSettings&) const = default;
};

struct CompressorSettings {
    bool enabled = false;
    bool rms = false;            // detector: false = peak, true = RMS
    float thresholdDb = -18.0f;  // -60 .. 0
    float ratio = 4.0f;          // 1 .. 20 (:1)
    float kneeDb = 6.0f;         // 0 .. 24, soft knee width
    float attackMs = 10.0f;      // 0.1 .. 200
    float releaseMs = 150.0f;    // 5 .. 2000
    float makeupDb = 0.0f;       // 0 .. 24
    bool operator==(const CompressorSettings&) const = default;
};

struct ReverbSettings {
    bool enabled = false;
    float mix = 0.25f;           // 0 .. 1, wet amount (dry stays at full level up to 0.5, then fades)
    float size = 0.6f;           // 0 .. 1, room size / decay time
    float damping = 0.5f;        // 0 .. 1, high frequency damping in the tail
    float width = 1.0f;          // 0 .. 1, stereo width of the tail
    float preDelayMs = 12.0f;    // 0 .. 100
    bool operator==(const ReverbSettings&) const = default;
};

} // namespace mc::model
