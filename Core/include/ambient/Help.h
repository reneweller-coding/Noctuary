// Noctuary -- the help texts: one or two sentences for every parameter, and the manual by
// topic. They live in the core so every shell (plugin, standalone, the Quest app, the render
// tool's --list) tells the same story; the plugin shows them as tooltips, as the line in the
// header that follows the mouse, and as the Help page.
#pragma once
#include "Params.h"

namespace ambient {

// What a parameter does, for the person turning it. Never null; "" if nothing is known.
const char* paramHelp(ParamId id);

// The manual, by topic (overview and signal flow, sources, filters, space, effects, cosmos,
// conductor, modulation, morph and performance, presets, clock, control, shortcuts).
int         numHelpTopics();
const char* helpTopicTitle(int index);
const char* helpTopicText(int index);

// What a block of the panel is for, by the name on its tab ("SOURCE 2", "CLOUD + FAR REVERB",
// "MATRIX"), a section without a tab ("SPACE", "MASTER") or a source type ("TYPE Stretch").
// "" when nothing is written for it.
const char* tabHelp(const char* name);

} // namespace ambient
