#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace toybox::chips
{

/** Which chip a voice belongs to.

	**Its own enum, not libvgm's `DEVID_*`.** Those name a device *instance* inside one backend, and
	a chip that arrives from an NSF file has no such handle at all. This is the model - "what is
	this sound made by" - which both backends can answer, and which a saved patch can carry.
*/
enum class ChipModel : uint8_t
{
	unknown = 0,
	ym2203,		// OPN, three FM channels and an SSG
	ym2612,		// OPN2, six FM channels and a DAC
	ym2608,		// OPNA, the PC-98 part
	ym2610,		// OPNB, the Neo Geo part
	ym2151,		// OPM, eight four-operator channels with a different envelope layout
	ym3812,		// OPL2, two operators, eight registers per voice
	vrc7		// the NES expansion chip, an OPL2 relative
};

/** **The registers behind one channel's voice**, as the chip itself addresses them.

	**Offsets are 16 bits because the chip has two ports.** The OPN's second port carries channels
	3-5, and its registers are written with the port in bit 8. An 8-bit offset would make channel 4
	indistinguishable from channel 0 - the low two bits of both are zero - which is a patch that
	loads somewhere else.

	`(offset, value)` pairs rather than a struct of named fields, and that is deliberate:

	- it is what a replayer needs: `load()` writes them back in order, with no encoding step in
	  between;
	- it assumes no layout - the YM2612's patch is 31 bytes over four operators, the VRC7's is 8
	  over two, and one array holds either;
	- a struct of named fields would have to be turned *back* into register writes before anything
	  could be played, and that encoder would be a second authority over the same numbers. That is
	  the shape this project has already paid for twice (`tableWave`, the SSG's channel count).

	The offsets are the chip's own (0x30..0xB0 for the OPN family), so an empty `regs` and a
	`model` of `unknown` are the two ways of saying "no patch". A decoder that cannot serialise
	its voice says so by returning false from `capturePatch()` rather than inventing one.
*/
/** **The rate a *picture* of a note is sampled at.**

	Low on purpose: the row is about ninety pixels wide, and 1.6 seconds at 8 kHz is already more
	points than that width can show. It lives here rather than beside the renderer because
	`playback/` may not include an engine header - the build enforces that.

	**Export does not use this.** A dragged file keeps the chip's own rate, because above 4 kHz the
	corners of a waveform are what make its timbre recognisable - see `chips::noteRateFor`.

	**The two were one constant until this rename**, and that is why the name matters: "the rate of
	a note" read as if it applied to both, so when a YM2151 stutter forced the visual rate down the
	export came down with it - seven times too fast, and no test looked at the file's header.
*/
constexpr double kVisualNoteRate = 8000.0;

/** **A note rendered for export: the samples and the rate they are at.**

	The rate travels with the samples because they are one fact. Keeping them apart is what let a
	YM2151 export declare 55 930 over samples made at 8 000 - seven times too fast, and invisible to
	every test that only looked at the length of the vector.

	Here rather than beside `ITrackPlayer`, because `backends/nsf/NsfPlayer.h` renders notes too and
	may not include a JUCE-bearing header. This struct is standard library only.
*/
struct RenderedNote
{
	std::shared_ptr<const std::vector<float>> wave;
	double rate = 0.0;
};

/** **How long a rendered note is, and whether it is normalised**, read once where the settings
	live and handed to whoever renders.

	**Read on the message thread, not where the render happens.** `renderNoteAt` runs on the export
	pool or on a drag, and reading `SettingsStore` there is a second source of the same three values
	- the same shape this project has paid for more than once. It is also a read of a file-backed
	`PropertiesFile` from a worker while the message thread may be writing one.
*/
struct NoteRenderShape
{
	double sustainSeconds = 0.0;
	double releaseSeconds = 0.0;
	bool normalise = false;

	/** **The rate the file will be written at**, or 0 to render at the chip's own rate.

		An export renders at this rate rather than resampling down to it afterwards. The player starts
		every core at the host's rate for the same reason: the core times itself from `freqbase`, so
		pitch and timbre are unchanged, while resampling costs the top of the spectrum and adds the
		step that `heldAt` would otherwise have to paper over.
	*/
	double rate = 0.0;
};

/** **The rate a note falls back to when nothing names the chip**, for a caller holding samples and
	no idea what produced them. Never a substitute for `chips::noteRateFor` when the patch does say.
*/
constexpr double kExportNoteRateFallback = 8000.0;

/** The two stages of a rendered note, in seconds. Both are settings; these are the fallbacks.

	They are separate because they answer different questions. The sustain is how long the sound is
	held before the key comes up - a choice, since the chip's "S" is a level and it decays through
	it rather than holding. The release is the tail after the key, which the patch does define, but
	its rate code reaches four minutes at RR = 1, so the length is capped here rather than read.
*/
constexpr double kDefaultSustainSeconds = 1.5;
constexpr double kDefaultReleaseSeconds = 0.1;

/** **How many bits of magnitude a chip's FM core actually produces.**

	Not the DAC's width. An eight-bit converter fed by a fourteen-bit sum still leaves all fourteen
	bits in `DEV_SMPL`, and quantising to the converter discards detail the emulator produces - a
	different sound, not a more authentic one.

	**Every figure here is measured, and four of them differ from what the first version assumed.**
	Each core was rendered at `normalise = false` and its peak compared against full scale: a value
	one bit too wide costs 6 dB of level *and* moves the quantiser off the core's own LSB, and neither
	shows up in a test that only counts samples.

	| model | peak when declared sixteen | measured width |
	|---|---|---|
	| YM2612 | 0.250 | 14 |
	| YM2203 / YM2608 | 0.997 | 16 |
	| YM2610 (MAME core) | - | 15, by ear |
	| YM2151 | 0.249 | 14 |

	Adding a chip means measuring it, not copying its neighbour: the two six-channel OPNs do not even
	share a core, and one needed a width the other did not.
*/
constexpr int fmOutputBits (ChipModel model)
{
	switch (model)
	{
	// **Fourteen for the OPN2, sixteen for the rest of the family.** One core serves all four, but it
	// does not accumulate to the same width: measured off `normalise = false` renders, a YM2612 peaks
	// at ±0x2000 while a YM2203 and YM2608 reach ±0x8000. Saying fourteen for all of them made the
	// YM2612's exports 12 dB quiet and shifted its quantiser off the core's own LSB.
	case ChipModel::ym2612:		return 14;

	case ChipModel::ym2608:
	case ChipModel::ym2203:		return 16;

	// **Fifteen, and it is the one value here that a fixture cannot check.** The YM2610's only core is
	// the MAME one, which renders `peak 0.000` from a standalone `SndEmu_Start` even with silent PCM
	// banks attached through `rwFuncs` - so its width was settled by comparing an export against
	// playback, where sixteen was audibly quiet and fifteen matches.
	//
	// **Two sibling parts, two different widths, and that is not a mistake**: `sndDev_YM2610` declares
	// only the MAME core while the YM2608 gets fmopn, and the two do not accumulate to the same scale.
	// Whoever adds a part here should measure it, not copy its neighbour's number.
	case ChipModel::ym2610:		return 15;

	// **Fourteen for the OPM too, measured the same way as the OPN2.** `nukedopm` reaches ±0x2000,
	// so a declared sixteen left every YM2151 export at a quarter of the chip's level - `peak
	// 0.249` against a real full scale. Same failure as the YM2612's, same fix.
	case ChipModel::ym2151:		return 14;

	// **The OPL parts and the VRC7 are still sixteen, and neither has been measured.** They are left
	// at the value they were written with rather than guessed at - a wrong number here is 6 dB of
	// level per bit, so it is worth a fixture before anyone reports it.
	case ChipModel::ym3812:
	case ChipModel::vrc7:		return 16;

	case ChipModel::unknown:	break;
	}

	// Same rule as `noteRateFor` below: this is `unknown`'s answer, not a guess for a chip that has
	// no case. Sixteen is what the OPL and OPM cores accumulate, so an unfilled model silently reads
	// as "wide" and its quantisation would be wrong by up to four bits.
	return 16;
}

/** **The rate a chip's FM channels actually produce samples at.**

	`clock / 144` for the OPN family, straight out of `ym2612.c` (`Rate = Clock / 144`, with the 144
	being 12 * prescale-6 * 2). Rendering slower than this and holding the samples back up to the
	export rate is what gives a dragged file its wide horizontal steps - a real property of a real
	chip, but not the one an eight-bit converter has.

	The figures are the nominal clock divided by its divider; a song may name a different clock, and
	the core is started with whatever the patch's chip is normally clocked at.
*/
constexpr double noteRateFor (ChipModel model)
{
	switch (model)
	{
	case ChipModel::ym2612:		return 7670453.0 / 144.0;		// 53 267 Hz, the Mega Drive
	case ChipModel::ym2203:		return 3993600.0 / 144.0;		// 27 733 Hz, the three-channel OPN
	case ChipModel::ym2608:
	case ChipModel::ym2610:		return 8000000.0 / 144.0;		// 55 556 Hz
	case ChipModel::ym2151:		return 3579545.0 / 64.0;		// 55 930 Hz
	case ChipModel::ym3812:		return 3579545.0 / 72.0;		// 49 716 Hz

	case ChipModel::vrc7:
	case ChipModel::unknown:	break;
	}

	// **The OPN's figure is for `unknown`, not for "a chip nobody filled in yet".** A model added to
	// the enum without a case here lands on this number, which is plausible enough to survive review
	// and wrong for anything that is not an OPN. `ymf271` is in that state today: it has no
	// `capturePatch`, so nothing calls this - see
	// `docs/issue/2026-11-22-the-drag-that-kept-the-panels-shape.md` §5.
	return 7670453.0 / 144.0;
}

struct VoicePatch
{
	ChipModel model = ChipModel::unknown;
	std::vector<std::pair<uint16_t, uint16_t>> regs;
};

} // namespace toybox::chips
