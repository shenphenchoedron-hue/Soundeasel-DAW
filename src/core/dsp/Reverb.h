#pragma once
// Algorithmic stereo reverb after Schroeder and Moorer:
//   pre-delay -> early reflections (tapped delay line, Moorer)
//             -> 8 parallel comb filters with a low-pass in the feedback loop (Moorer)
//             -> 4 series all-pass filters (Schroeder)
// Left and right use slightly different delay lengths for a wide, decorrelated tail.
// prepare() allocates; reset() and process() are realtime safe.
#include "model/Effects.h"
#include <array>
#include <cstddef>
#include <vector>

namespace mc::dsp {

class Reverb {
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const model::ReverbSettings&);
    // In place, stereo.
    void process(float* left, float* right, int numSamples);

    static constexpr int kCombs = 8, kAllpasses = 4, kTaps = 8;

private:
    struct Comb {
        std::vector<float> buf;
        std::size_t pos = 0;
        float store = 0;
        float process(float in, float feedback, float damp)
        {
            const float out = buf[pos];
            store = out * (1.0f - damp) + store * damp;  // one-pole low-pass in the loop
            buf[pos] = in + store * feedback;
            if (++pos >= buf.size()) pos = 0;
            return out;
        }
    };
    struct Allpass {
        std::vector<float> buf;
        std::size_t pos = 0;
        float process(float in)
        {
            const float b = buf[pos];
            buf[pos] = in + b * 0.5f;
            if (++pos >= buf.size()) pos = 0;
            return b - in;
        }
    };
    struct Channel {
        std::array<Comb, kCombs> combs;
        std::array<Allpass, kAllpasses> allpasses;
        std::array<int, kTaps> taps{};     // early reflection delays (samples, after the pre-delay)
        std::array<float, kTaps> gains{};
    };

    double sr_ = 44100.0;
    model::ReverbSettings s_;
    Channel ch_[2];
    std::vector<float> delay_;  // shared input delay line (pre-delay + early reflections), mono
    std::size_t delayPos_ = 0;
    int preDelay_ = 0;
    float feedback_ = 0.84f, damp_ = 0.2f, wet1_ = 0, wet2_ = 0, dry_ = 1, early_ = 0.3f;
};

} // namespace mc::dsp
