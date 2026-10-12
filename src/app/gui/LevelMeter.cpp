#include "gui/LevelMeter.h"
#include "gui/Theme.h"
#include <tuple>

namespace mc::gui {

void fader::setup(juce::Slider& s)
{
    s.getProperties().set("fader", true); // fills from the bottom, not from 0 dB
    s.setRange(kMinDb, kMaxDb, 0.1);
    s.setSkewFactorFromMidPoint(-12.0);
    s.setDoubleClickReturnValue(true, 0.0);
    s.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
    s.textFromValueFunction = [](double db) { return db <= kMinDb + 0.05 ? juce::String("-inf dB") : juce::String(db, 1) + " dB"; };
    s.setPopupDisplayEnabled(true, true, nullptr);
}

float LevelMeter::toNorm(float gain)
{
    if (gain <= 0.0f) return 0.0f;
    const float db = juce::Decibels::gainToDecibels(gain, -100.0f);
    return juce::jlimit(0.0f, 1.0f, (db - (float)fader::kMinDb) / (float)(fader::kMaxDb - fader::kMinDb));
}

void LevelMeter::update(Channel& c, float v)
{
    const float n = toNorm(v);
    c.level = std::max(n, c.level - 0.035f); // smooth falloff
    if (n >= c.hold) { c.hold = n; c.holdFrames = 45; }
    else if (--c.holdFrames <= 0) c.hold = std::max(c.level, c.hold - 0.02f);
    if (v >= 1.0f && !c.clip) { c.clip = true; repaint(); } // stays lit until the meter is clicked
}

void LevelMeter::push(float left, float right)
{
    const auto before = std::make_tuple(ch[0].level, ch[1].level, ch[0].hold, ch[1].hold);
    update(ch[0], left);
    update(ch[1], stereo_ ? right : left);
    if (before != std::make_tuple(ch[0].level, ch[1].level, ch[0].hold, ch[1].hold)) repaint();
}

void LevelMeter::paint(juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour(juce::Colour(0xff1b2027));
    g.fillRoundedRectangle(r, 2.0f);
    const int n = stereo_ ? 2 : 1;
    const float zeroDb = (float)((0.0 - fader::kMinDb) / (fader::kMaxDb - fader::kMinDb));
    for (int i = 0; i < n; ++i) {
        juce::Rectangle<float> bar = vertical_
            ? juce::Rectangle<float>(r.getX() + 1 + i * (r.getWidth() - 2) / n, r.getY() + 1, (r.getWidth() - 2) / n - (n > 1 ? 1 : 0), r.getHeight() - 2)
            : juce::Rectangle<float>(r.getX() + 1, r.getY() + 1 + i * (r.getHeight() - 2) / n, r.getWidth() - 2, (r.getHeight() - 2) / n - (n > 1 ? 1 : 0));
        const auto& c = ch[i];
        const auto lo = input_ ? juce::Colour(0xffe08f4f) : juce::Colour(0xff3fae94);
        juce::ColourGradient grad = vertical_
            ? juce::ColourGradient(lo, bar.getX(), bar.getBottom(), juce::Colour(0xffe45b5b), bar.getX(), bar.getY(), false)
            : juce::ColourGradient(lo, bar.getX(), bar.getY(), juce::Colour(0xffe45b5b), bar.getRight(), bar.getY(), false);
        grad.addColour(zeroDb * 0.82, lo);
        grad.addColour(zeroDb * 0.95, juce::Colour(0xffe5c442));
        g.setGradientFill(grad);
        if (vertical_) {
            const float h = bar.getHeight() * c.level;
            g.fillRect(bar.withTop(bar.getBottom() - h));
            if (c.hold > 0.01f) {
                g.setColour(c.hold >= zeroDb ? juce::Colour(0xffe45b5b) : juce::Colours::white.withAlpha(0.8f));
                g.fillRect(bar.getX(), bar.getBottom() - bar.getHeight() * c.hold, bar.getWidth(), 1.5f);
            }
        } else {
            g.fillRect(bar.withWidth(bar.getWidth() * c.level));
            if (c.hold > 0.01f) {
                g.setColour(c.hold >= zeroDb ? juce::Colour(0xffe45b5b) : juce::Colours::white.withAlpha(0.8f));
                g.fillRect(bar.getX() + bar.getWidth() * c.hold - 1.5f, bar.getY(), 1.5f, bar.getHeight());
            }
        }
    }
    // 0 dB mark
    g.setColour(juce::Colours::white.withAlpha(0.25f));
    if (vertical_) g.fillRect(r.getX(), r.getBottom() - 1 - (r.getHeight() - 2) * zeroDb, r.getWidth(), 1.0f);
    else g.fillRect(r.getX() + 1 + (r.getWidth() - 2) * zeroDb, r.getY(), 1.0f, r.getHeight());
    // clip LED: the signal reached 0 dBFS (click the meter to reset)
    if (ch[0].clip || ch[1].clip) {
        g.setColour(juce::Colour(0xffff3b3b));
        if (vertical_) g.fillRect(r.getX(), r.getY(), r.getWidth(), 4.0f);
        else g.fillRect(r.getRight() - 4.0f, r.getY(), 4.0f, r.getHeight());
    }
}

} // namespace mc::gui
