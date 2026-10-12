#include "dsp/Reverb.h"
#include <algorithm>
#include <cmath>

namespace mc::dsp {

namespace {
// Delay lengths in samples at 44.1 kHz (mutually prime-ish, as in Schroeder/Moorer designs).
constexpr int kCombTuning[Reverb::kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
constexpr int kAllpassTuning[Reverb::kAllpasses] = {556, 441, 341, 225};
constexpr int kStereoSpread = 23;
// Early reflections (ms after the pre-delay) and gains, after Moorer's tapped delay line.
constexpr float kTapMsL[Reverb::kTaps] = {4.3f, 21.5f, 26.8f, 29.8f, 45.8f, 57.2f, 61.2f, 70.7f};
constexpr float kTapMsR[Reverb::kTaps] = {5.1f, 22.5f, 27.0f, 32.4f, 48.8f, 58.7f, 63.5f, 72.6f};
constexpr float kTapGain[Reverb::kTaps] = {0.841f, 0.504f, 0.491f, 0.379f, 0.380f, 0.346f, 0.289f, 0.272f};
constexpr float kMaxPreDelayMs = 100.0f, kMaxTapMs = 80.0f;
constexpr float kInputGain = 0.015f; // combs sum to a large gain
} // namespace

void Reverb::prepare(double sampleRate)
{
    sr_ = sampleRate > 0 ? sampleRate : 44100.0;
    const double scale = sr_ / 44100.0;
    for (int c = 0; c < 2; ++c) {
        auto& ch = ch_[c];
        const int spread = c == 0 ? 0 : kStereoSpread;
        for (int i = 0; i < kCombs; ++i) ch.combs[(size_t)i].buf.assign((size_t)std::max(1.0, (kCombTuning[i] + spread) * scale), 0.0f);
        for (int i = 0; i < kAllpasses; ++i) ch.allpasses[(size_t)i].buf.assign((size_t)std::max(1.0, (kAllpassTuning[i] + spread) * scale), 0.0f);
        for (int i = 0; i < kTaps; ++i) {
            ch.taps[(size_t)i] = (int)std::lround((c == 0 ? kTapMsL[i] : kTapMsR[i]) * 0.001 * sr_);
            ch.gains[(size_t)i] = kTapGain[i];
        }
    }
    delay_.assign((size_t)std::ceil((kMaxPreDelayMs + kMaxTapMs) * 0.001 * sr_) + 2, 0.0f);
    setSettings(s_);
    reset();
}

void Reverb::reset()
{
    for (auto& ch : ch_) {
        for (auto& c : ch.combs) { std::fill(c.buf.begin(), c.buf.end(), 0.0f); c.store = 0; c.pos = 0; }
        for (auto& a : ch.allpasses) { std::fill(a.buf.begin(), a.buf.end(), 0.0f); a.pos = 0; }
    }
    std::fill(delay_.begin(), delay_.end(), 0.0f);
    delayPos_ = 0;
}

void Reverb::setSettings(const model::ReverbSettings& s)
{
    s_ = s;
    const float size = std::clamp(s.size, 0.0f, 1.0f);
    const float mix = std::clamp(s.mix, 0.0f, 1.0f);
    const float width = std::clamp(s.width, 0.0f, 1.0f);
    feedback_ = 0.70f + size * 0.28f;               // longer decay for bigger rooms
    damp_ = std::clamp(s.damping, 0.0f, 1.0f) * 0.4f;
    const float wet = std::min(1.0f, 2.0f * mix) * 3.0f;
    dry_ = std::min(1.0f, 2.0f * (1.0f - mix));
    wet1_ = wet * (width * 0.5f + 0.5f);
    wet2_ = wet * ((1.0f - width) * 0.5f);
    early_ = 0.35f * (1.0f - 0.5f * size);          // small rooms: more early reflections
    preDelay_ = (int)std::lround(std::clamp(s.preDelayMs, 0.0f, kMaxPreDelayMs) * 0.001 * sr_);
}

void Reverb::process(float* l, float* r, int n)
{
    if (delay_.empty()) return;
    const size_t len = delay_.size();
    auto tap = [&](int samplesAgo) { return delay_[(delayPos_ + len - (size_t)samplesAgo % len) % len]; };
    for (int i = 0; i < n; ++i) {
        const float inL = l[i], inR = r[i];
        delay_[delayPos_] = (inL + inR) * 0.5f;
        float out[2];
        for (int c = 0; c < 2; ++c) {
            auto& ch = ch_[c];
            float er = 0.0f;
            for (int k = 0; k < kTaps; ++k) er += tap(preDelay_ + ch.taps[(size_t)k]) * ch.gains[(size_t)k];
            const float late_in = (tap(preDelay_) + er * 0.5f) * kInputGain;
            float acc = 0.0f;
            for (auto& comb : ch.combs) acc += comb.process(late_in, feedback_, damp_);
            for (auto& ap : ch.allpasses) acc = ap.process(acc);
            out[c] = acc + er * early_ * 0.25f;
        }
        l[i] = inL * dry_ + out[0] * wet1_ + out[1] * wet2_;
        r[i] = inR * dry_ + out[1] * wet1_ + out[0] * wet2_;
        if (++delayPos_ >= len) delayPos_ = 0;
    }
}

} // namespace mc::dsp
