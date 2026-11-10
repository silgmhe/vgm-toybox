#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <cstddef>
#include <vector>

namespace toybox::midi
{
struct SongIr;
}

namespace toybox::backends::libvgm::dac
{

/** **A pause this long ends a hit**: 512 samples at 44.1 kHz = 11.6 ms.

	Measured on `02 Green Hill Zone`: the gaps inside one drum are shorter and the gaps between two
	hits are longer - and most hits are cut by the driver's own `0xE0` anyway, so this only separates
	the ones that are not. Two milliseconds was tried once and shattered decays into fragments (916
	hits / 18 kinds); this value is the reference implementation's (`vgm2fur/chips/sampler.py:4`),
	which is also what makes our files match its `.fur`.
*/
constexpr uint32_t kSeparationSamples = 512;

/** One hit: the bytes the driver played between two boundaries, and how long it took.

	`beginTime`/`endTime` are ticks (44.1 kHz samples) and exist so a *row* can ask "which hit is
	playing now"; the export only needs `start`, `length` and `duration`.
*/
struct Run
{
	uint32_t beginTime = 0;		// tick of the run's first byte
	uint32_t endTime = 0;		// tick of its last byte
	uint32_t start = 0;			// bank offset
	uint32_t length = 0;		// bytes played
	uint32_t duration = 0;		// ticks spent playing, pauses shorter than 11.6 ms included
};

/** **The sampler both the export and the panel use**, in ticks rather than seconds.

	The YM2612 has no sample table: a hit is the bytes the driver actually played. This is
	`vgm2fur/chips/sampler.py` line for line, because the export means to be the same sample set that
	tool puts in a Furnace module and the row means to show the same bytes - same boundaries, same
	lengths, same rates.

	Three rules carry all of it:

	  1. `0xE0` while a hit is in progress (or before any byte) starts a new hit; a `0xE0` that only
		 moves the pointer through silence does not, and drops the silent bytes it skipped;
	  2. a pause of 512 samples or more between two bytes ends the hit - the next byte begins at
		 `start + length`, which is where the driver continued;
	  3. the rate is `tickRate * length / duration`, where `duration` counts the small pauses inside
		 the hit but not the long one that ended it.

	**The region model this replaces was wrong in a way the numbers only now show**: the `0xE0` offsets
	are seek targets, not sample boundaries. Marble's `0x0A80` is played 1 693 bytes straight through
	two later offsets, and Labyrinth plays 3 808 bytes from 0 across the offset at 1 904.
*/
class Sampler
{
public:
	explicit Sampler (uint32_t tickRate) : tickRate_ (tickRate) {}

	void seek (uint32_t time, uint32_t offset)
	{
		if (duration_ > 0 || idle_)
		{
			close ();
			start_ = offset;
			length_ = 0;
			duration_ = 0;
			idle_ = false;
		}
		else
		{
			// Only silence has been played since the last hit; move the pointer and forget the
			// bytes, rather than exporting a hit that is nothing but silence.
			start_ = offset;
			length_ = 0;
			idle_ = false;
		}

		pauseBase_ = time;
		haveBase_ = true;
	}

	void byte (uint32_t time)
	{
		if (length_ > 0 && haveBase_)
		{
			const uint32_t pause = time > pauseBase_ ? time - pauseBase_ : 0;

			if (pause > kSeparationSamples)
			{
				if (duration_ > 0)
				{
					close ();
					start_ += length_;
					length_ = 0;
					duration_ = 0;
				}
				else
				{
					start_ += length_;
					length_ = 0;
				}
			}
			else
			{
				duration_ += pause;
			}
		}

		if (length_ == 0)
		{
			beginTime_ = time;
		}

		++length_;
		lastTime_ = time;
		pauseBase_ = time;
		haveBase_ = true;
		idle_ = false;
	}

	void finish (void)
	{
		close ();
	}

	const std::vector<Run>& runs (void) const
	{
		return runs_;
	}

	uint32_t tickRate (void) const
	{
		return tickRate_;
	}

private:
	void close (void)
	{
		if (length_ > 0 && duration_ > 0)
		{
			runs_.push_back (Run { beginTime_, lastTime_, start_, length_, duration_ });
		}
	}

	uint32_t tickRate_ = 44100;
	std::vector<Run> runs_;
	uint32_t beginTime_ = 0;
	uint32_t lastTime_ = 0;
	uint32_t start_ = 0;
	uint32_t length_ = 0;
	uint32_t duration_ = 0;
	uint32_t pauseBase_ = 0;
	bool haveBase_ = false;
	bool idle_ = true;
};

/** **A song's DAC table: the bank and the hits, built once and shared.**

	It is built by whoever owns the song (`LibvgmSongInstance`) and handed to every track player and
	to the export, because building it costs one pass over every `0x8n` byte - 459 546 of them on
	`02 Green Hill Zone` - and there are a dozen track players. Built per player, that pass and its
	sort cost about 200 ms of load time for nothing.
*/
struct Table
{
	uint32_t tickRate = 44100;
	std::size_t device = 0;		// which chip slot the runs came from (the only YM2612, in practice)
	std::vector<uint8_t> bank;
	std::vector<Run> runs;		// that chip's hits; `0xE0` names no chip, and there is one
};

/** Builds the table in one pass over the song. */
std::shared_ptr<const Table> buildTable (const midi::SongIr& ir, std::size_t maxBankBytes);

/** **The hit that is playing at `samples`, or the last one that played before it.**

	"Or before" on purpose: every other sample row keeps the sample it last played, so a stopped DAC
	row shows the last drum rather than going blank. Null when nothing has played yet.
*/
inline const Run* hitAt (const std::vector<Run>& runs, uint32_t samples)
{
	const auto after = std::upper_bound (runs.begin (), runs.end (), samples,
										 [] (uint32_t value, const Run& run)
										 { return value < run.beginTime; });

	return (after == runs.begin ()) ? nullptr : &*(after - 1);
}

/** The PCM bank: every `0x00`/`0x80` block, concatenated in stream order.

	The reference takes one block, the last one, because its input files carry one. A VGM bank can be
	uploaded in pieces - the uploads concatenate, that is what the format says - so this is the one
	place the two implementations deliberately differ.
*/
std::vector<uint8_t> bankFor (const midi::SongIr& ir, std::size_t maxBytes);

/** One device's hits, in the order they were played.

	Two inputs, one timeline: the seeks say where the driver moved the pointer and the `0x8n` bytes
	say what it fed it. `0x52 0x2A` pokes are left out on purpose - they are stops and level steps,
	and counting them would cut samples the driver played as one.
*/
std::vector<Run> runsFor (const midi::SongIr& ir, std::size_t device, uint32_t tickRate);

} // namespace toybox::backends::libvgm::dac
