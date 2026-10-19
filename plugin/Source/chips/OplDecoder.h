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

	/** @param adpcm true for the Y8950, whose Delta-T unit is a sample player on channel 14 */
	explicit OplDecoder (Variant variant = Variant::opl2, bool adpcm = false);
	~OplDecoder() override;

	void reset (std::uint32_t clock, double sampleRate) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override;
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
};

} // namespace toybox::chips
