#pragma once
// Parametric EQ editor for a channel strip (call-out next to the mixer strip):
// frequency response graph with draggable band points, plus controls for the
// selected band. Changes are heard live; closing records one undo step.
#include "AppContext.h"

namespace mc::gui {

void showEqEditor(AppContext&, model::TrackId, juce::Component& target);

} // namespace mc::gui
