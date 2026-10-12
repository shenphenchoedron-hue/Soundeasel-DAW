#include "dsp/Equalizer.h"
#include <algorithm>
#include <cmath>
#include <complex>

namespace mc::dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Equalizer::prepare(double sampleRate)
{
    sr_ = sampleRate > 0 ? sampleRate : 44100.0;
    for (int i = 0; i < numBands_; ++i) coeffs_[(size_t)i] = design(bands_[(size_t)i], sr_);
    numActive_ = numBands_;
    reset();
}

void Equalizer::reset()
{
    for (auto& b : state_) for (auto& c : b) c = {};
}

Equalizer::Coeffs Equalizer::design(const model::EqBand& band, double sr)
{
    using T = model::EqBand::Type;
    const double f = std::clamp((double)band.freq, 10.0, sr * 0.49);
    const double q = std::clamp((double)band.q, 0.05, 40.0);
    const double A = std::pow(10.0, std::clamp((double)band.gainDb, -48.0, 48.0) / 40.0);
    const double w0 = 2.0 * kPi * f / sr, cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (band.type) {
        case T::Bell:
            b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
            a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
            break;
        case T::LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s); b1 = 2 * A * ((A - 1) - (A + 1) * cw); b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;       a1 = -2 * ((A - 1) + (A + 1) * cw);   a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case T::HighShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s); b1 = -2 * A * ((A - 1) + (A + 1) * cw); b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;       a1 = 2 * ((A - 1) - (A + 1) * cw);     a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        case T::LowCut:
            b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
        case T::HighCut:
            b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2;
            a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
            break;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void Equalizer::setSettings(const model::EqSettings& s)
{
    numBands_ = 0;
    for (auto& b : s.bands) {
        if (numBands_ >= model::EqSettings::kMaxBands) break;
        if (!b.enabled) continue;
        bands_[(size_t)numBands_] = b;
        coeffs_[(size_t)numBands_] = design(b, sr_);
        ++numBands_;
    }
    numActive_ = numBands_;
}

void Equalizer::process(float* l, float* r, int n)
{
    for (int k = 0; k < numActive_; ++k) {
        const auto& c = coeffs_[(size_t)k];
        float* chans[2] = {l, r};
        for (int ch = 0; ch < 2; ++ch) {
            auto& st = state_[(size_t)k][(size_t)ch];
            float* x = chans[ch];
            double z1 = st.z1, z2 = st.z2;
            for (int i = 0; i < n; ++i) { // transposed direct form II
                const double in = x[i];
                const double out = c.b0 * in + z1;
                z1 = c.b1 * in - c.a1 * out + z2;
                z2 = c.b2 * in - c.a2 * out;
                x[i] = (float)out;
            }
            st.z1 = z1;
            st.z2 = z2;
        }
    }
}

double Equalizer::bandResponseDb(const model::EqBand& band, double freq, double sr)
{
    const auto c = design(band, sr);
    const double w = 2.0 * kPi * std::clamp(freq, 1.0, sr * 0.5) / sr;
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = std::polar(1.0, -2.0 * w);
    const auto h = (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    return 20.0 * std::log10(std::max(1e-9, std::abs(h)));
}

double Equalizer::responseDb(const model::EqSettings& s, double freq, double sr)
{
    double db = 0;
    int n = 0;
    for (auto& b : s.bands) {
        if (n >= model::EqSettings::kMaxBands) break;
        if (!b.enabled) continue;
        db += bandResponseDb(b, freq, sr);
        ++n;
    }
    return db;
}

} // namespace mc::dsp
