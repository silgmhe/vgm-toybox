#pragma once

#include "chips/VoicePatch.h"

namespace toybox::playback
{

/** **A chip instance that can be told what to play, in samples.**

	One implementation per engine, and that is the whole point: the libvgm backend drives the core the
	plugin plays through, the NSF backend drives the OPLL its own player uses. Neither knows the other
	exists, and both give the caller the same five calls.

	**`load` rather than a constructor**, because a driver outlives a patch. One instance per backend
	serves every channel in turn: load a patch, key it, read the samples, move on. Starting a chip core
	costs tens of kilobytes and a fair amount of initialisation, and doing that per note is what made
	opening a file slow once already.

	**The samples are mono and unnormalised.** Whatever the chip produced, scaled to -1..1 by its own
	full scale - not by its peak. Peak normalisation and quantisation belong to the display, which is
	the layer that knows what the picture is for; a driver that decided them would be deciding them
	twice.
*/
class IVoiceDriver
{
public:
	virtual ~IVoiceDriver () = default;

	/** Take on this patch. False means the chip cannot play it - an OPN patch handed to an OPLL, or
		a driver that failed to start its core. A caller that gets false has nothing to draw. */
	virtual bool load (const chips::VoicePatch& patch) = 0;

	/** Key on at this frequency. Hz rather than a MIDI note: a chip has a frequency, and the note is
		the playback layer's idea of one. */
	virtual void noteOn (double hz) = 0;

	/** Key off. The release is the chip's own; this only stops holding the note. */
	virtual void noteOff (void) = 0;

	/** Fill `frames` mono samples, in -1..1. Advances the chip by exactly that many samples. */
	virtual void render (float* out, int frames) = 0;

	/** The rate `render` produces at. Valid once a patch has loaded; before that, the chip's nominal
		rate for whatever model the driver was built for. */
	virtual double rate (void) const = 0;

	/** **How many bits of magnitude this chip's core produces**, which is what `render` scales its
		samples by and what the caller quantises to.

		Asked here rather than looked up from the patch, because there is one right answer and it
		belongs to the core that made the samples. `chips::fmOutputBits` was read at both ends - once
		in the driver keyed on the model it had loaded, once in `noteWave` keyed on the patch's model -
		and those two are equal only by construction.
	*/
	virtual int noteWidth (void) const { return 16; }
};

} // namespace toybox::playback
