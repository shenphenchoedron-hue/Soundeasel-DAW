#include "Test.h"
#include "dsp/Compressor.h"
#include "dsp/Reverb.h"
#include <cmath>
#include <vector>

using namespace mc;

static bool near(float a, float b, float eps) { return std::abs(a - b) <= eps; }

TEST(compressor_curve_hard_and_soft_knee)
{
    // hard knee: below threshold unchanged, above divided by the ratio
    CHECK(near(dsp::Compressor::curve(-30.0f, -20.0f, 4.0f, 0.0f), -30.0f, 1e-4f));
    CHECK(near(dsp::Compressor::curve(-8.0f, -20.0f, 4.0f, 0.0f), -17.0f, 1e-4f));
    // soft knee: identical outside the knee, smooth and in between inside it
    CHECK(near(dsp::Compressor::curve(-30.0f, -20.0f, 4.0f, 10.0f), -30.0f, 1e-4f));
    CHECK(near(dsp::Compressor::curve(-8.0f, -20.0f, 4.0f, 10.0f), -17.0f, 1e-4f));
    const float atT = dsp::Compressor::curve(-20.0f, -20.0f, 4.0f, 10.0f);
    CHECK(atT < -20.0f && atT > -20.0f - 2.0f);             // gentle reduction already at the threshold
    CHECK(near(dsp::Compressor::curve(-25.0f, -20.0f, 4.0f, 10.0f), -25.0f, 1e-4f)); // knee start
    CHECK(near(dsp::Compressor::curve(-15.0f, -20.0f, 4.0f, 10.0f), -18.75f, 1e-4f)); // knee end
}

static float settle(dsp::Compressor& c, float amp, int n = 48000)
{
    std::vector<float> l((size_t)n, amp), r((size_t)n, amp);
    c.process(l.data(), r.data(), n);
    return l.back();
}

TEST(compressor_reduces_loud_signal_peak_and_rms)
{
    for (bool rms : {false, true}) {
        dsp::Compressor c;
        c.prepare(48000);
        model::CompressorSettings s;
        s.rms = rms; s.thresholdDb = -20; s.ratio = 4; s.kneeDb = 0; s.attackMs = 1; s.releaseMs = 50;
        c.setSettings(s);
        // 0 dBFS DC: -20 + 20/4 = -15 dB out
        const float out = settle(c, 1.0f);
        CHECK(near(20.0f * std::log10(out), -15.0f, 0.2f));
        CHECK(near(c.gainReductionDb(), 15.0f, 0.2f));
        // quiet signal: no reduction after release
        const float quiet = settle(c, 0.01f);
        CHECK(near(quiet, 0.01f, 0.0005f));
    }
}

TEST(compressor_makeup_and_attack_time)
{
    dsp::Compressor c;
    c.prepare(48000);
    model::CompressorSettings s;
    s.thresholdDb = -20; s.ratio = 4; s.kneeDb = 0; s.attackMs = 50; s.releaseMs = 100; s.makeupDb = 6;
    c.setSettings(s);
    std::vector<float> l(480, 1.0f), r(480, 1.0f); // 10 ms: attack not finished yet
    c.process(l.data(), r.data(), 480);
    CHECK(c.gainReductionDb() > 1.0f && c.gainReductionDb() < 10.0f);
    const float out = settle(c, 1.0f);
    CHECK(near(20.0f * std::log10(out), -9.0f, 0.2f)); // -15 + 6 makeup
}

TEST(reverb_impulse_tail_is_stereo_finite_and_decays)
{
    dsp::Reverb rv;
    rv.prepare(48000);
    model::ReverbSettings s;
    s.enabled = true; s.mix = 0.5f; s.size = 0.7f; s.preDelayMs = 10;
    rv.setSettings(s);
    const int n = 48000 * 4;
    std::vector<float> l((size_t)n, 0.0f), r((size_t)n, 0.0f);
    l[0] = r[0] = 1.0f;
    rv.process(l.data(), r.data(), n);
    auto energy = [&](const std::vector<float>& v, int from, int to) { double e = 0; for (int i = from; i < to; ++i) e += (double)v[(size_t)i] * v[(size_t)i]; return e; };
    const double early = energy(l, 480, 48000), late = energy(l, 3 * 48000, 4 * 48000);
    CHECK(early > 1e-4);           // there is a tail
    CHECK(late < early * 0.01);    // and it decays
    bool finite = true, differ = false;
    for (int i = 0; i < n; ++i) {
        finite = finite && std::isfinite(l[(size_t)i]) && std::abs(l[(size_t)i]) < 4.0f;
        differ = differ || std::abs(l[(size_t)i] - r[(size_t)i]) > 1e-4f;
    }
    CHECK(finite);
    CHECK(differ);                 // decorrelated left/right
    // nothing before the pre-delay (except the dry impulse)
    CHECK(energy(l, 1, 400) < 1e-12);
}

TEST(reverb_reset_clears_tail)
{
    dsp::Reverb rv;
    rv.prepare(44100);
    model::ReverbSettings s;
    s.mix = 1.0f;
    rv.setSettings(s);
    std::vector<float> l(4410, 0.0f), r(4410, 0.0f);
    l[0] = 1.0f;
    rv.process(l.data(), r.data(), 4410);
    rv.reset();
    std::vector<float> z(4410, 0.0f), z2(4410, 0.0f);
    rv.process(z.data(), z2.data(), 4410);
    double e = 0;
    for (float v : z) e += v * v;
    CHECK(e == 0.0);
}

#include "dsp/Equalizer.h"

TEST(eq_band_responses)
{
    using T = model::EqBand::Type;
    const double sr = 48000;
    model::EqBand bell{T::Bell, 1000, 6, 1, true};
    CHECK(near((float)dsp::Equalizer::bandResponseDb(bell, 1000, sr), 6.0f, 0.05f));   // full gain at the centre
    CHECK(std::abs(dsp::Equalizer::bandResponseDb(bell, 50, sr)) < 0.2);               // flat far away
    model::EqBand ls{T::LowShelf, 200, -9, 0.707f, true};
    CHECK(near((float)dsp::Equalizer::bandResponseDb(ls, 20, sr), -9.0f, 0.3f));
    CHECK(std::abs(dsp::Equalizer::bandResponseDb(ls, 10000, sr)) < 0.2);
    model::EqBand hs{T::HighShelf, 5000, 4, 0.707f, true};
    CHECK(near((float)dsp::Equalizer::bandResponseDb(hs, 18000, sr), 4.0f, 0.4f));
    model::EqBand lc{T::LowCut, 100, 0, 0.707f, true};
    CHECK(near((float)dsp::Equalizer::bandResponseDb(lc, 100, sr), -3.0f, 0.1f));     // -3 dB at the cutoff
    CHECK(dsp::Equalizer::bandResponseDb(lc, 25, sr) < -20.0);                         // 12 dB/octave
    model::EqBand hc{T::HighCut, 2000, 0, 0.707f, true};
    CHECK(dsp::Equalizer::bandResponseDb(hc, 8000, sr) < -20.0);
    // bands add up; disabled bands do nothing
    model::EqSettings s;
    s.bands = {bell, bell};
    CHECK(near((float)dsp::Equalizer::responseDb(s, 1000, sr), 12.0f, 0.1f));
    s.bands[1].enabled = false;
    CHECK(near((float)dsp::Equalizer::responseDb(s, 1000, sr), 6.0f, 0.1f));
}

TEST(eq_process_matches_response)
{
    // a 1 kHz sine through a +6 dB bell at 1 kHz comes out about twice as loud
    const double sr = 48000;
    dsp::Equalizer eq;
    eq.prepare(sr);
    model::EqSettings s;
    s.enabled = true;
    s.bands = {{model::EqBand::Type::Bell, 1000, 6, 1, true}};
    eq.setSettings(s);
    const int n = 48000;
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int i = 0; i < n; ++i) l[(size_t)i] = r[(size_t)i] = 0.25f * (float)std::sin(2 * 3.14159265358979 * 1000 * i / sr);
    eq.process(l.data(), r.data(), n);
    float peak = 0;
    for (int i = n / 2; i < n; ++i) peak = std::max(peak, std::abs(l[(size_t)i]));
    CHECK(near(20.0f * std::log10(peak / 0.25f), 6.0f, 0.1f));
    CHECK(near(l[(size_t)n - 1], r[(size_t)n - 1], 1e-6f));
}
