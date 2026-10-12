#pragma once
// Volume (dB) and pan sliders of a track, wherever they are shown (track list,
// audio track cards, mixer strips): live while dragging, one undo step per
// gesture, and each slider only writes its own property.
#include "AppContext.h"
#include "gui/LevelMeter.h"

namespace mc::gui {

inline void connectTrackSliders(AppContext& app, model::TrackId id, juce::Slider& volume, juce::Slider& pan,
                                std::function<void()> afterChange = {})
{
    auto connect = [&app, id, afterChange](juce::Slider& s, bool isVolume) {
        s.onDragStart = [&app, id] { app.beginTrackEdit(id); };
        s.onValueChange = [&app, id, &s, isVolume, afterChange] {
            // wheel / double-click / typed values arrive without a drag: they are one gesture each
            const bool single = !app.isEditingTrack(id);
            if (single) app.beginTrackEdit(id);
            if (isVolume) app.liveTrackVolume(id, (float)fader::dbToGain(s.getValue()));
            else app.liveTrackPan(id, (float)s.getValue());
            if (single) app.endTrackEdit(id);
            if (afterChange) afterChange();
        };
        s.onDragEnd = [&app, id] { app.endTrackEdit(id); };
    };
    connect(volume, true);
    connect(pan, false);
}

} // namespace mc::gui
