#pragma once

#include <memory>

#include "IChipDecoder.h"

namespace toybox::chips
{

/** The Yamaha YMF271 (OPX): twelve groups of four operator slots, where the group's *sync* mode
	decides what those four slots are.

	The chip is not one chip but four arrangements behind the same register file (ymf271.c:
	`ymf271_write_fm` and `ymf271_update`):

	- **sync 0** - a four-operator FM voice. Bank 0 is the key-on bank: its writes to the synced
	  registers (0x0, 0x9, 0xA, 0xC, 0xD, 0xE) are copied to all four slots, which is why a driver
	  only addresses the first bank. The algorithm picks the carriers.
	- **sync 1** - two two-operator voices per group: slots 1/3 (banks 0/2) and slots 2/4
	  (banks 1/3), each pair with its own key-on bank and its own algorithm.
	- **sync 2** - a three-operator voice on banks 0-2 plus a PCM slot on bank 3. Only the FM half
	  is decoded; the PCM slot would need a sample length that the registers do not describe.
	- **sync 3** - the group is four independent PCM slots. A slot has no pitch and no end register
	  we can read, so a key-on is a fixed 80 ms one-shot, the same marker the X1-010 and the OPN's
	  ADPCM use.

	Register addressing (`ymf271_w`): the VGM command's port selects the bank - 0-3 are the FM
	banks, 4 is the PCM register file, 6 the timer/sync registers - and the address byte is
	`register << 4 | group`, with group numbers 3, 7, 11 and 15 unused (fm_tab). So the FM total
	level of group 0 slot 1 is address 0x40 written to port 0.

	Pitch comes from the core's step arithmetic (ymf271.c `calculate_step`): one cycle is 1024
	table entries, the phase advances by `2 * fns * pow[block] * multiple / 8` entries per output
	sample, and the core renders at `clock / 384`. That reduces to

	    hz = clock / 384 * fns * pow[block] * multiple / 2^28

	so the note table carries fns in the low twelve bits of the period and the operator's multiple
	above them - the carrier's multiple is part of the pitch, not a timbre setting. */
class Ymf271Decoder final : public IChipDecoder
{
public:
	enum Channel
	{
		channel0 = 0,
		channel1,
		channel2,
		channel3,
		channel4,
		channel5,
		channel6,
		channel7,
		channel8,
		channel9,
		channel10,
		channel11,
		numChannels
	};

	Ymf271Decoder();
	~Ymf271Decoder() override;

	void reset (const ChipConfig& config) override;
	void write (const RegisterWrite& write) override;
	void advance (std::uint32_t sampleTime) override;

	int channelCount() const override { return channelCount_; }
	ChannelState channelState (int channel) const override;
	const NoteTable& noteTable (int channel) const override;
	Family family (int channel) const override;
	bool isDrum (int channel) const override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl;
	int channelCount_ = (int) Ymf271Decoder::numChannels;
};

} // namespace toybox::chips
