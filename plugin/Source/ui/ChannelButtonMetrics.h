#pragma once

namespace toybox::ui
{
	/** **The mixer's M/S buttons, in one place.**

		`ChannelStrip` lays a strip's pair out from these, and the keyboard's rows follow so that the
		two views draw their buttons at the same size. They used to be two `constexpr` in
		`ChannelStrip.cpp` and two plain fields in `KeyboardRowLayout.h`, and the fields had drifted to
		30 and 8 while the comment beside them still claimed they agreed with the mixer.

		A header of its own rather than one of the two owners: `KeyboardRowLayout.h` is pure arithmetic
		and must not pull in `ChannelStrip.h` (JUCE widgets and two playback headers), while
		`ChannelStrip.cpp` should not reach into a layout header for a number about buttons.

		The height is the button's face plus the shadow room each one reserves - `ButtonPaint.h`.
	*/
	inline constexpr int channelButtonHeight = 28;
	inline constexpr int channelButtonGap = 6;
}
