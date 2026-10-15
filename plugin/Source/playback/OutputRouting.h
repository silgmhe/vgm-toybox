#pragma once

#include <cstdint>

#include <juce_core/juce_core.h>

/**
	Output routing: how many stereo buses the plugin declares and where a track can send its audio.

	The count is a **build-time** constant because a VST3 host fixes the bus layout when it loads
	the plugin - it cannot change while the plugin runs. Override it on the command line:

	    cmake -S . -B build -DVGMPLAY_OUTPUT_BUSES=16

	Bus 0 is `Out 1/2` (the main bus, activated by default); the other buses are `Out 3/4`,
	`Out 5/6`, ... and are **inactive by default** - the user enables the ones they need in the
	host (the same model Kontakt and every other multi-output instrument uses).
*/
#ifndef VGMPLAY_OUTPUT_BUSES
	#define VGMPLAY_OUTPUT_BUSES 64
#endif

namespace vgmplay::playback
{

/** Number of stereo output buses this build declares. */
inline constexpr int kOutputBusCount = VGMPLAY_OUTPUT_BUSES;

/** Destination of a track (or of the master bus): -1 = the internal master bus, >= 0 = a real bus. */
using OutputTarget = int;

/** The internal master bus - not a real output, it has a target bus of its own. */
inline constexpr OutputTarget kMasterOutput = -1;

/** True when the target names one of the declared output buses. */
inline bool isRealOutputBus (OutputTarget target)
{
	return target >= 0 && target < kOutputBusCount;
}

/** One stereo pair per output bus, as the host hands it to our render().
	A bus the host has not connected has both pointers null (nothing is rendered into it). */
struct OutputBusBuffers
{
	float* left = nullptr;
	float* right = nullptr;

	bool isConnected (void) const { return left != nullptr && right != nullptr; }
};

/** Display name of a bus ("Out 1/2", "Out 3/4", ...) - the host shows the same names. */
inline juce::String outputBusName (int busIndex)
{
	const int first = busIndex * 2 + 1;
	return "Out " + juce::String (first) + "/" + juce::String (first + 1);
}

/** Display name of a target, including the internal master bus. */
inline juce::String outputTargetName (OutputTarget target)
{
	return (target == kMasterOutput) ? juce::String ("Master") : outputBusName (target);
}

} // namespace vgmplay::playback
