#pragma once

#include <cstdint>
#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The OPL family: YM2413 (OPLL), YM3526/YM3812/Y8950 (OPL2), YMF262 (OPL3) and the FM half
	of the YMF278B (OPL4), which is register-compatible with an OPL3.

	Two software-compatible register files with different pitch registers:

	  - OPLL: FNUM low byte in 0x10..0x18, block and FNUM bit 8 in 0x20..0x28, key-on in bit 4
		of that register and key-off in bit 5 of 0x30..0x38. Its FNUM-to-frequency reference is
		one octave lower than the OPL2 one, so the table carries the reference.
	  - OPL2/OPL3: 0xA0..0xA8 and 0xB0..0xB8 hold FNUM/block and key-on. An OPL3 has two banks,
		the second addressed through the second register port.

	Frequency follows f = FNUM * clock / 72 * 2^(block - 20) on the 3.58 MHz parts; the OPL3
	divides by 288 because its clock is four times higher. The pitch is clock-calibrated for the
	same reason the OPN and OPM tables are.

	Channel indices (must match midiCapabilityFor() in LibvgmSongInstance.cpp):

	  - OPLL:     9 melodic (0..8) + 5 rhythm (9..13).
	  - OPL2:     9 melodic (0..8) + 5 rhythm (9..13) + the Y8950's Delta-T on 14.
	  - OPL3:    18 melodic (0..8 and 9..17) + 5 rhythm (18..22).

	Rhythm instruments are level-triggered: only the rising edge of a drum bit is a hit, which is
	why they are reported as one-shot pulses rather than held notes. */
class OplDecoder final : public IChipDecoder
{
public:
	enum class Variant
	{
		opll,	// YM2413: nine channels, rhythm on 0x0E
		opl2,	// YM3526 / YM3812 / Y8950: nine channels, rhythm on 0xBD
		opl3	// YMF262 / YMF278B FM: two nine-channel banks, rhythm on 0xBD
	};

	/** **The value `ChipConfig::flags` carries for one of these variants**, in bits 1-2.

		Bit 0 is taken by the Y8950's Delta-T unit (`deltaTFlag`), so the variant sits above it.

		**Why it has to travel at all.** The decoder registry makes its instances with the default
		constructor, and that constructor is `opl2` - correct for the OPL2s, wrong for everything
		else that shares this decoder. A YM2413 therefore had its `0x10..0x18` / `0x20..0x28` pitch
		and key-on registers parsed as an OPL2's operator registers: no period, no block, no gate. The
		row showed no note and no key highlight, while its MIDI was right - `MidiSequenceBuilder`
		builds a decoder of its own, with the variant it worked out. The OPL3s lost their second
		nine-channel bank the same way.

		Same shape as `opnSlotFlag` in `VgmReplayer`, and read back in `reset`.
	*/
	static constexpr uint32_t slotFlag (Variant variant)
	{
		return ((uint32_t) variant + 1u) << 1;
	}

	/** Bit 0: this device is a Y8950, whose Delta-T unit adds a fifteenth row. */
	static constexpr uint32_t deltaTFlag = 1u;

	/** @param adpcm true for the Y8950, whose Delta-T unit is a sample player on channel 14 */
	explicit OplDecoder (Variant variant = Variant::opl2, bool adpcm = false);
	~OplDecoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (uint32_t sampleTime) override;

	int channelCount() const override;
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	ChannelKind channelKind (int channel) const override;
	bool isDrum (int channel) const override;

	/** **The OPLL's voice, as the byte the chip takes.** An OPLL patch is not a set of operator
		registers - it is an instrument number pointing into the chip's own ROM table - so this
		returns one register for a preset and nine for the user voice (eight global operator
		registers plus the selector). See `docs/desgin/2026-11-22-opl-visualisation.md`.

		Always captured for **channel 0**: the patch is rendered by a core of its own, and the driver's
		OPLL dialect writes `0x10`/`0x20`, so the registers and the key land together.

		The OPL2 and the OPL3 have real operator registers and would need their own case; they return
		false, which is what they did before.
	*/
	bool capturePatch (int channel, VoicePatch& out) const override;

	/** **The Y8950's Delta-T ROM, in a block of its own.** `0x88` is VGM's "Y8950 DELTA-T ROM data";
		the unit addresses it as pages of 32 bytes (or 4, when control2 asks for a DRAM part), so an
		image is at most `0x10000 << 5` = 2 MB. The fallback clock is the 3.58 MHz part the unit was
		designed around - a song that names no clock still has to be measured against something. */
	static constexpr ChipRequirements kRequirements { 3579545u, true, 0x88, 1u << 24, false, false };

	void attachMemory (const ChipMemory& memory) override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
