/**
 * @file Rate.h
 * @brief The sample rate, written once (11.11.2026).
 *
 * The instrument runs at the host's rate: prepare() hands it to every stage. This is the rate they stand at until
 * then, the rate taken when a host names none, and the rate of what has no host -- the render tools and the bank's
 * measurements. Nothing else in the instrument's code writes the number (the tests name their rates themselves:
 * they run at several on purpose).
 * @author Rene Weller
 */
#pragma once

namespace ambient
{

/** @brief The sample rate before prepare() names the host's, and the tools' own, Hz. */
constexpr double DefaultSampleRate = 48000.0;

}   // namespace ambient
