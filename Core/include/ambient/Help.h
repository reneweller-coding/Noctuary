/**
 * @file Help.h
 * @brief The help texts: one or two sentences for every parameter, and the manual by topic.
 *
 * They live in the core so every shell (plugin, standalone, the Quest app, the render
 * tool's --list) tells the same story; the plugin shows them as tooltips, as the line in the
 * header that follows the mouse, and as the Help page.
 *
 * The texts themselves are tables in Help.cpp, keyed by the parameter's key string (Params.h):
 * the slot, LFO, envelope and delay families share one text per family, so "src2_pos" is looked
 * up as "srcN_pos" and the four slots can never drift apart. Everything here reads static
 * storage, is safe from any thread, and never hands back a null pointer.
 */
#pragma once
#include "Params.h"

namespace ambient {

/**
 * @brief What a parameter does, for the person turning it.
 *
 * Never null; "" if nothing is known.
 *
 * @param id  the parameter whose text is wanted
 * @return    one or two sentences from the table in Help.cpp, "" when it has no entry
 */
const char* paramHelp(ParamId id);

/**
 * @name The manual, by topic
 * The manual, by topic (overview and signal flow, sources, filters, space, effects, cosmos,
 * conductor, modulation, morph and performance, presets, clock, control, shortcuts).
 * The topics are a fixed table in Help.cpp; a shell lists the titles and shows the text of the
 * one that is picked. An index outside the table reads as "".
 * @{ */

/**
 * @brief How many topics the manual has.
 * @return the number of entries; the valid indices are 0 .. that - 1
 */
int         numHelpTopics();

/**
 * @brief The heading of one topic, as the Help page lists it.
 * @param index  0 .. numHelpTopics() - 1
 * @return       the title, "" for an index outside the table
 */
const char* helpTopicTitle(int index);

/**
 * @brief The body of one topic.
 * @param index  0 .. numHelpTopics() - 1
 * @return       the text, "" for an index outside the table
 */
const char* helpTopicText(int index);
/** @} */

/**
 * @brief What a block of the panel is for, by the name on its tab ("SOURCE 2", "CLOUD + FAR REVERB",
 *        "MATRIX"), a section without a tab ("SPACE", "MASTER") or a source type ("TYPE Stretch").
 *
 * "" when nothing is written for it.
 *
 * @param name  the label exactly as the panel prints it
 * @return      the block's text from the table in Help.cpp, "" when there is none
 */
const char* tabHelp(const char* name);

} // namespace ambient
