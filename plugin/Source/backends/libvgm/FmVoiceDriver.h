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
struct Core;

/** **What one FM family does differently**, and nothing else.

	Everything that costs is shared: the libvgm device, `Core`, `render`, the rate decision. The OPN and
	the OPM differ in five places, four of them code - which is why this is a struct of functions rather
	than a table of numbers.

	**Not a second driver class.** `LibvgmTrackPlayer` holds one `voiceDriver` for the whole song and a
	song can carry both chips, so a facade would have to dispatch on the model - which is exactly the
	branch this replaces, moved one level up.

	Declared here rather than in the .cpp because the driver holds a pointer to one; `Core` is only
	forward-declared, and the four functions that take it live in the .cpp with its definition.
*/
struct Dialect
{
	/** The channel is these bits of the patch's addresses: two on the OPN (operator stride four),
		three on the OPM (stride eight). Reading one as the other puts channel 4's patch on channel 0. */
	uint16_t channelMask;

	/** Bring a freshly reset core to the point where it will make a sound. */
	void ( *init ) ( Core& core, int channel, chips::ChipModel model );

	/** Write the pitch and key the voice.

		**Not split into pitch and key-on**: the OPN has to write 0xA4 before 0xA0 for the f-number to
		latch, and the key follows in the same sequence; the OPM's KC/KF and key are equally one run.
	*/
	void ( *noteOn ) ( Core& core, int port, int channel, chips::ChipModel model, double hz );

	void ( *noteOff ) ( Core& core, int port, int channel );

	/** After the patch's registers are written. The OPM's pan has to come last, because the patch
		carries `0x20|ch` and a song that never sets it leaves both bits clear - which is silence. */
	void ( *afterPatch ) ( Core& core, int channel );
};

class FmVoiceDriver final : public playback::IVoiceDriver
{
public:
	/** @param exportQuality true to run the core at the chip's own rate rather than the rate a
		panel draws at. The two differ by a factor of seven on a YM2151, and the reason is in
		`chips::kVisualNoteRate`.
	*/
	explicit FmVoiceDriver (bool exportQuality = false);
	~FmVoiceDriver (void) override;

	FmVoiceDriver (const FmVoiceDriver&) = delete;
	FmVoiceDriver& operator= (const FmVoiceDriver&) = delete;

	bool load (const chips::VoicePatch& patch) override;
	void noteOn (double hz) override;
	void noteOff (void) override;
	void render (float* out, int frames) override;
	double rate (void) const override;

private:
	/** Starts a core for `model` at `rate`, replacing whatever was running. */
	bool startCore (chips::ChipModel model, double rate);

	/** The namespace-level `Core`, not a nested one: `Dialect`'s functions take it, and those are
		declared above the class. */
	std::unique_ptr<Core> core;
	chips::ChipModel loadedModel = chips::ChipModel::unknown;

	/** The rate the running core was started at. Kept because `load` has to rebuild when the
		requested rate changes, not only when the model does - otherwise a panel opened after an
		export would keep the high-rate core.
	*/
	double loadedRate = 0.0;
	bool exportQuality = false;

	/** Which family's registers this driver is speaking. Set by `load`, from the patch's model. */
	const Dialect* dialect = nullptr;

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
std::unique_ptr<playback::IVoiceDriver> makeFmVoiceDriver (bool exportQuality = false);

} // namespace toybox::backends::libvgm
