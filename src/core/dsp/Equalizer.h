#pragma once
// Parametric EQ for a channel strip: up to 8 bands of second-order (biquad) filters
// after the RBJ "Audio EQ Cookbook" (bell, low/high shelf, low/high cut).
// setSettings() computes coefficients; process() is realtime safe (no allocation).
#include "model/Effects.h"
#include <array>

namespace mc::dsp {

class Equalizer {
public:
    void prepare(double sampleRate);
    void reset();
    void setSettings(const model::EqSettings&);
    void process(float* left, float* right, int numSamples); // in place, stereo

    // Frequency response in dB of all enabled bands at `freq` (for drawing the curve).
    static double responseDb(const model::EqSettings&, double freq, double sampleRate);
    static double bandResponseDb(const model::EqBand&, double freq, double sampleRate);

private:
    struct Coeffs { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
    static Coeffs design(const model::EqBand&, double sampleRate);
    struct State { double z1 = 0, z2 = 0; };

    double sr_ = 44100.0;
    std::array<model::EqBand, model::EqSettings::kMaxBands> bands_{}; // enabled bands (fixed size: no allocation)
    int numBands_ = 0;
    std::array<Coeffs, model::EqSettings::kMaxBands> coeffs_{};
    std::array<std::array<State, 2>, model::EqSettings::kMaxBands> state_{};
    int numActive_ = 0;
};

} // namespace mc::dsp
