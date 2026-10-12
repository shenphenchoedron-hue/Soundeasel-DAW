#pragma once
// Editors for the built-in channel strip effects (compressor, reverb), shown in a
// call-out next to the mixer strip. Changes are heard live; closing the editor
// records one undo step.
#include "AppContext.h"

namespace mc::gui {

enum class StripEffect { Compressor, Reverb };
void showEffectEditor(AppContext&, model::TrackId, StripEffect, juce::Component& target);

} // namespace mc::gui
