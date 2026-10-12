#pragma once
// Peak level meter (with falloff and peak hold) and the dB fader mapping
// shared by the mixer and the track cards.
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace mc::gui {

namespace fader {
inline constexpr double kMinDb = -60.0, kMaxDb = 6.0;
inline double gainToDb(double g) { return g <= 0.001 ? kMinDb : juce::jlimit(kMinDb, kMaxDb, juce::Decibels::gainToDecibels(g)); }
inline double dbToGain(double db) { return db <= kMinDb + 0.05 ? 0.0 : juce::Decibels::decibelsToGain(db); }
inline juce::String text(double gain)
{
    if (gain <= 0.001) return "-inf";
    const double db = juce::Decibels::gainToDecibels(gain);
    return (db > 0.05 ? "+" : "") + juce::String(db, 1);
}
// Slider in dB (-60 = off .. +6), double-click = 0 dB.
void setup(juce::Slider&);
} // namespace fader

class LevelMeter final : public juce::Component, public juce::SettableTooltipClient {
public:
    explicit LevelMeter(bool vertical = true, bool stereo = true) : vertical_(vertical), stereo_(stereo) {
        setInterceptsMouseClicks(true, false);
        setTooltip("Level. A red mark means the signal clipped (reached 0 dB) - click the meter to reset it");
    }
    // Peak levels (linear) since the previous call; called by a ~30 Hz UI timer.
    void push(float left, float right);
    void setInputStyle(bool in) { if (input_ != in) { input_ = in; repaint(); } } // red-ish tint for input levels
    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { ch[0].clip = ch[1].clip = false; repaint(); } // reset the clip LED

private:
    struct Channel { float level = 0, hold = 0; int holdFrames = 0; bool clip = false; };
    static float toNorm(float gain);
    void update(Channel&, float v);
    bool vertical_, stereo_, input_ = false;
    Channel ch[2];
};

} // namespace mc::gui
