#include "dsp/Compressor.h"
#include <algorithm>
#include <cmath>

namespace mc::dsp {

namespace {
float coeff(double ms, double sr) { return ms <= 0.0 ? 0.0f : (float)std::exp(-1.0 / (ms * 0.001 * sr)); }
constexpr float kRmsWindowMs = 10.0f;
} // namespace

void Compressor::prepare(double sampleRate)
{
    sr_ = sampleRate > 0 ? sampleRate : 44100.0;
    setSettings(s_);
    reset();
}

void Compressor::reset()
{
    env_ = 0.0f;
    meanSq_ = 0.0f;
}

void Compressor::setSettings(const model::CompressorSettings& s)
{
    s_ = s;
    s_.ratio = std::clamp(s_.ratio, 1.0f, 100.0f);
    s_.kneeDb = std::clamp(s_.kneeDb, 0.0f, 48.0f);
    att_ = coeff(std::clamp(s_.attackMs, 0.01f, 1000.0f), sr_);
    rel_ = coeff(std::clamp(s_.releaseMs, 1.0f, 5000.0f), sr_);
    rmsCoeff_ = coeff(kRmsWindowMs, sr_);
    makeup_ = std::pow(10.0f, s_.makeupDb / 20.0f);
}

float Compressor::curve(float x, float t, float ratio, float knee)
{
    const float slope = 1.0f / ratio - 1.0f;
    const float over = x - t;
    if (knee > 0.0f && 2.0f * std::abs(over) <= knee) {
        const float k = over + knee * 0.5f;
        return x + slope * k * k / (2.0f * knee);
    }
    return over <= 0.0f ? x : t + over / ratio;
}

void Compressor::process(float* l, float* r, int n)
{
    const float t = s_.thresholdDb, ratio = s_.ratio, knee = s_.kneeDb;
    for (int i = 0; i < n; ++i) {
        const float a = l[i], b = r ? r[i] : a;
        float level;
        if (s_.rms) {
            meanSq_ = rmsCoeff_ * meanSq_ + (1.0f - rmsCoeff_) * 0.5f * (a * a + b * b);
            level = std::sqrt(meanSq_);
        } else {
            level = std::max(std::abs(a), std::abs(b));
        }
        const float inDb = 20.0f * std::log10(std::max(level, 1.0e-6f));
        const float target = inDb - curve(inDb, t, ratio, knee); // wanted reduction, >= 0
        const float c = target > env_ ? att_ : rel_;
        env_ = c * env_ + (1.0f - c) * target;
        const float g = makeup_ * std::pow(10.0f, -env_ / 20.0f);
        l[i] = a * g;
        if (r) r[i] = b * g;
    }
}

} // namespace mc::dsp
