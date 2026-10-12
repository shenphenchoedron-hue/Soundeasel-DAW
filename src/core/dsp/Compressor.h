#pragma once
// Feed-forward compressor for a channel strip.
// - detector: peak or RMS (stereo linked: both channels get the same gain)
// - static curve with a soft knee (quadratic interpolation over the knee width)
// - gain reduction smoothed in the log domain with separate attack/release times
// prepare() allocates nothing; process() is realtime safe.
#include "model/Effects.h"

namespace mc::dsp {

class Compressor {
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const model::CompressorSettings&);
    // In place. `right` may be nullptr (mono).
    void process(float* left, float* right, int numSamples);
    float gainReductionDb() const { return env_; } // current reduction (>= 0 dB)

    // Static curve: output level (dB) for an input level (dB).
    static float curve(float inDb, float thresholdDb, float ratio, float kneeDb);

private:
    double sr_ = 44100.0;
    model::CompressorSettings s_;
    float att_ = 0, rel_ = 0, rmsCoeff_ = 0, makeup_ = 1.0f;
    float env_ = 0.0f;     // smoothed gain reduction in dB
    float meanSq_ = 0.0f;  // RMS detector state
};

} // namespace mc::dsp
