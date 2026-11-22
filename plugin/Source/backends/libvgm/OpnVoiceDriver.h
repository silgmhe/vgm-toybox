#pragma once

#include <cstdint>
#include <memory>

#include "chips/VoicePatch.h"
#include "playback/VoiceDriver.h"

namespace toybox::backends::libvgm
{

/** **The libvgm side of `IVoiceDriver`**: it starts the core the plugin plays through.

	`SndEmu_Start` with `emuCore = 0` picks the default implementation of whatever `ChipModel` the
	patch names, which for this project is `fmopn.c` - the same code `VGMPlayer` runs. That is the
	reason a drawn waveform and a playing one cannot disagree about a patch: they are not two models
	of the same thing, they are the same model.

	One instance serves every channel of a backend in turn: `load` rewrites the registers, `noteOn`
	keys the voice, and the previous note is gone. Starting a core per note costs tens of kilobytes
	and the initialisation that goes with it, which is why the core is kept across calls and rebuilt
	only when the chip model changes.
*/
class OpnVoiceDriver final : public playback::IVoiceDriver
{
public:
	/** @param exportQuality true to run the core at the chip's own rate rather than the rate a
		panel draws at. The two differ by a factor of seven on a YM2151, and the reason is in
		`chips::kVisualNoteRate`.
	*/
	explicit OpnVoiceDriver (bool exportQuality = false);
	~OpnVoiceDriver (void) override;

	OpnVoiceDriver (const OpnVoiceDriver&) = delete;
	OpnVoiceDriver& operator= (const OpnVoiceDriver&) = delete;

	bool load (const chips::VoicePatch& patch) override;
	void noteOn (double hz) override;
	void noteOff (void) override;
	void render (float* out, int frames) override;
	double rate (void) const override;

private:
	struct Core;

	/** Starts a core for `model` at `rate`, replacing whatever was running. */
	bool startCore (chips::ChipModel model, double rate);

	std::unique_ptr<Core> core;
	chips::ChipModel loadedModel = chips::ChipModel::unknown;

	/** The rate the running core was started at. Kept because `load` has to rebuild when the
		requested rate changes, not only when the model does - otherwise a panel opened after an
		export would keep the high-rate core.
	*/
	double loadedRate = 0.0;
	bool exportQuality = false;

	/** Where this patch's channel lives: the port is bit 8 of the offsets, and the chip numbers the
		channel key as `(bank << 2) | slot` rather than 0..5. Both are recovered in `load`. */
	int port = 0;
	int channelInPort = 0;
};

/** **Creates a driver, rather than letting callers construct one.**

	`Core` is defined in the .cpp because it holds `DEV_INFO` and the engine's function pointers -
	the headers this file exists to keep out of everyone else. A class whose only member is a
	pointer to an incomplete type cannot be created or destroyed outside that .cpp, so the factory
	is the door: callers get a `unique_ptr` to the interface and never need the definition.
*/
std::unique_ptr<playback::IVoiceDriver> makeOpnVoiceDriver (bool exportQuality = false);

} // namespace toybox::backends::libvgm
