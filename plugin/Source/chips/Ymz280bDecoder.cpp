#include "Ymz280bDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <map>

#include "Pitch.h"
#include "Ymz280bData.h"

namespace toybox::chips
{
namespace
{
	constexpr uint32_t kDefaultClock = 16934400;

	/** **The chip's own rate is the clock over 384**: `0x172` steps of fnum span 0.172 to 44.1 kHz in
		256 steps (`ymz280b.c:18`), so 16 934 400 / 384 = 44 100 exactly. */
	constexpr double kClockPerRate = 384.0;

	/** How many steps one cycle of the drawn window holds. A streamed slice has no cycle; this is
		the amount the row shows while the voice plays. */
	constexpr int kWindowSteps = 256;

	/** One voice's rate in steps per second: `clock / 384 * (fnum + 1) / 256`.

		The ADPCM mode has a nine-bit fnum but only its low eight bits count (`ymz280b.c:259-262`),
		the PCM modes use all nine. */
	double rateOf (uint32_t clock, int fnum, int mode)
	{
		const double steps = (double) ((mode == 1) ? (fnum & 0xFF) : (fnum & 0x1FF)) + 1.0;

		return ((double) clock / kClockPerRate) * steps / 256.0;
	}
}

struct Ymz280bDecoder::Impl
{
	struct Voice
	{
		int fnum = 0;
		int mode = 0;
		bool keyon = false;
		bool looping = false;
		bool playing = false;
		int level = 0;
		int pan = 8;
		uint32_t start = 0;			// nibbles
		uint32_t stop = 0;
		uint32_t loopEnd = 0;
		uint32_t keyTime = 0;		// the sample time the voice last started, or its range last moved
	};

	Voice voices[numChannels];
	std::vector<uint8_t> rom;
	uint32_t clock = kDefaultClock;
	double timeBase = 44100.0;
	uint32_t now = 0;
	bool keyonEnable = false;

	/** **The start register *is* the position, and nothing is extrapolated from the clock.**

		The first version estimated it as `start + elapsed × rate`, which is wrong here in a way that
		looks like a broken decoder: a streamed voice has its start rewritten every few seconds, so
		the elapsed time since key-on piles up and the estimate marches straight past `stop`. Past the
		range the ROM is zero, all-zero nibbles decode to a constant, and the row draws a flat line -
		while the exported file, which is cut from the block's own start, is perfectly fine.

		The drivers in this collection rewrite start (and stop) as they feed the next section, so the
		register already says where the voice is. Reading from it needs no clock and cannot drift.
	*/
	uint32_t positionOf (const Voice& voice) const
	{
		const double elapsed = (double) (now - voice.keyTime) / timeBase;
		const double steps = elapsed * ymz280b::rateOf (clock, voice.fnum, voice.mode);
		const uint32_t span = (voice.stop > voice.start) ? (voice.stop - voice.start) : 0;
		uint32_t offset = (uint32_t) (steps > 0.0 ? steps : 0.0);

		// **Advance inside the range, and only inside it.** The baseline (`keyTime`) is reset by
		// every write that moves start or stop, so `elapsed` never spans two sections; clamping then
		// keeps a slow or stalled driver from reading past the data into the zeros after it.
		if (span != 0)
		{
			if (voice.looping && voice.loopEnd > voice.start && (voice.loopEnd - voice.start) <= span)
			{
				offset %= (voice.loopEnd - voice.start);
			}
			else if (offset > span)
			{
				offset = span;
			}
		}

		return voice.start + offset;
	}

	/** One window of the ROM, decoded the way the chip plays it.

		**The ADPCM state starts from zero at the window's first step**, which is not what the chip
		holds mid-stream - it has been accumulating since key-on. For a shape on screen that is
		enough (the envelope of the data is what you see); a file should never be cut this way, which
		is why the collector decodes from the range's own start. */
	void windowOf (const Voice& voice, std::vector<float>& out) const
	{
		out.clear ();

		if (rom.empty ())
		{
			return;
		}

		ymz280b::decodeRange (rom, voice.mode, positionOf (voice), (uint32_t) kWindowSteps, out);

		// **Normalised for the eye, not for the ear.** An ADPCM run started from zero state ramps up
		// from silence, so the first 256 steps of a slice sit around a few hundredths of full scale -
		// measured peaks of 0.047 on these rips, which draws as a flat line even though every one of
		// those 256 steps is a different value. The file keeps the absolute level (the collector
		// decodes from the range's own start), so nothing is lost where it matters.
		float peak = 0.0f;

		for (const float point : out)
		{
			peak = std::max (peak, std::fabs (point));
		}

		if (peak > 1.0e-4f)
		{
			for (auto& point : out)
			{
				point /= peak;
			}
		}
	}
};

Ymz280bDecoder::Ymz280bDecoder() = default;
Ymz280bDecoder::~Ymz280bDecoder() = default;

void Ymz280bDecoder::reset (const ChipConfig& config)
{
	std::vector<uint8_t> rom;

	if (impl != nullptr)
	{
		rom = impl->rom;		// the sample ROM belongs to the song, not to the run
	}

	impl = std::make_unique<Impl> ();
	impl->clock = (config.clockHz != 0) ? config.clockHz : kDefaultClock;
	impl->timeBase = (config.timeBaseHz > 0.0) ? config.timeBaseHz : 44100.0;
	impl->rom = std::move (rom);

	if (std::getenv ("TOYBOX_YMZ_ROM") != nullptr)
	{
		std::size_t firstNonZero = impl->rom.size ();
		for (std::size_t i = 0; i < impl->rom.size (); ++i) { if (impl->rom[i] != 0) { firstNonZero = i; break; } }
		std::fprintf (stderr, "    YMZ rom size=%u firstNonZero=0x%X\n",
					  (unsigned) impl->rom.size (), (unsigned) firstNonZero);
	}
}

void Ymz280bDecoder::setRom (std::vector<uint8_t> rom)
{
	if (impl == nullptr)
	{
		impl = std::make_unique<Impl> ();
	}

	impl->rom = std::move (rom);

	if (std::getenv ("TOYBOX_YMZ_ROM") != nullptr)
	{
		std::size_t firstNonZero = impl->rom.size ();
		for (std::size_t i = 0; i < impl->rom.size (); ++i) { if (impl->rom[i] != 0) { firstNonZero = i; break; } }
		std::fprintf (stderr, "    YMZ rom size=%u firstNonZero=0x%X\n",
					  (unsigned) impl->rom.size (), (unsigned) firstNonZero);
	}
}

void Ymz280bDecoder::write (const RegisterWrite& w)
{
	if (impl == nullptr)
	{
		return;
	}

	// **The global half first.** `0xFF` bit 7 is the key-on enable, and its rising edge starts every
	// voice that is configured with key-on and looping (`ymz280b.c:985-992`) - these songs start
	// their stream exactly this way, before the voice's own key-on is ever written.
	if (w.reg == 0xFF)
	{
		const bool enable = (w.value & 0x80) != 0;

		if (enable && ! impl->keyonEnable)
		{
			for (auto& voice : impl->voices)
			{
				if (voice.keyon && voice.looping)
				{
					voice.playing = true;
					voice.keyTime = w.sampleTime;
				}
			}
		}
		else if (! enable && impl->keyonEnable)
		{
			for (auto& voice : impl->voices)
			{
				voice.playing = false;
			}
		}

		impl->keyonEnable = enable;
		return;
	}

	if (w.reg >= 0x80)
	{
		return;		// DSP, ROM readback and IRQ registers say nothing about a voice
	}

	const int index = (w.reg >> 2) & 7;
	Impl::Voice& voice = impl->voices[index];

	// **A write that moves the range also moves the position baseline.** The streamed drivers here
	// rewrite start and stop as they feed the next section; without this the elapsed clock keeps
	// counting from key-on and the estimate is immediately past the new range - which reads zeros
	// and draws a flat line, the very symptom that took two rounds to chase down.
	{
		const int field = w.reg & 0xE3;
		const bool isAddress = (field >= 0x20 && field <= 0x23)
							   || (field >= 0x40 && field <= 0x43)
							   || (field >= 0x60 && field <= 0x63);

		if (isAddress)
		{
			voice.keyTime = w.sampleTime;
		}
	}

	switch (w.reg & 0xE3)
	{
	case 0x00:
		voice.fnum = (voice.fnum & 0x100) | w.value;
		break;

	case 0x01:
	{
		voice.fnum = (voice.fnum & 0xFF) | ((w.value & 0x01) << 8);
		voice.looping = (w.value & 0x10) != 0;
		voice.mode = (w.value & 0x60) >> 5;

		const bool on = (w.value & 0x80) != 0;

		if (on && ! voice.keyon)
		{
			voice.playing = true;
			voice.keyTime = w.sampleTime;
		}
		else if (! on && voice.keyon)
		{
			voice.playing = false;
		}

		voice.keyon = on;
		break;
	}

	case 0x02:
		voice.level = w.value;
		break;

	case 0x03:
		voice.pan = w.value & 0x0F;
		break;

	case 0x20: case 0x21: case 0x22: case 0x23:
	{
		// Address blocks: high writes bits 16-23, middle 8-15, low 0-7, all in nibbles.
		const int which = (w.reg & 0xE3) - 0x20;
		uint32_t& target = (which == 0) ? voice.start
						 : (which == 1) ? voice.start		// loop start is where playback comes back to
						 : (which == 2) ? voice.loopEnd
										: voice.stop;
		target = (target & 0x00FFFF) | ((uint32_t) w.value << 16);
		break;
	}

	case 0x40: case 0x41: case 0x42: case 0x43:
	{
		const int which = (w.reg & 0xE3) - 0x40;
		uint32_t& target = (which == 0) ? voice.start
						 : (which == 1) ? voice.start
						 : (which == 2) ? voice.loopEnd
										: voice.stop;
		target = (target & 0xFF00FF) | ((uint32_t) w.value << 8);
		break;
	}

	case 0x60: case 0x61: case 0x62: case 0x63:
	{
		const int which = (w.reg & 0xE3) - 0x60;
		uint32_t& target = (which == 0) ? voice.start
						 : (which == 1) ? voice.start
						 : (which == 2) ? voice.loopEnd
										: voice.stop;
		target = (target & 0xFFFF00) | (uint32_t) w.value;
		break;
	}

	default:
		break;
	}
}

void Ymz280bDecoder::advance (uint32_t sampleTime)
{
	if (impl != nullptr)
	{
		impl->now = sampleTime;
	}
}

ChannelState Ymz280bDecoder::channelState (int channel) const
{
	ChannelState state;

	if (impl == nullptr || channel < 0 || channel >= channelCount_)
	{
		return state;
	}

	Impl::Voice& voice = impl->voices[channel];

	if (std::getenv ("TOYBOX_YMZ_STATE") != nullptr && channel == 0)
	{
		static int seen = 0;

		if (seen++ % 60 == 0)
		{
			const std::size_t at = impl->rom.empty () ? 0 : (std::size_t) (voice.start % (uint32_t) impl->rom.size ());
			std::fprintf (stderr, "    YMZ ch0 start=0x%X stop=0x%X mode=%d lvl=%d playing=%d size=%u rom[at]=0x%02X\n",
						  (unsigned) voice.start, (unsigned) voice.stop, voice.mode, voice.level,
						  voice.playing ? 1 : 0, (unsigned) impl->rom.size (),
						  impl->rom.empty () ? 0xFF : impl->rom[at]);
		}
	}

	state.present = true;
	state.gate = voice.playing && voice.level > 0;
	state.volume = (int) voice.level * 15 / 255;
	state.period = 0x80000000u;			// a stream has no pitch: the note table says one-shot

	if (! state.gate)
	{
		return state;
	}

	// **A sample, not a shape**: the row shows the slice being read, and the rate is the rate the
	// chip steps through it. `tableWave` stays false so the export writes it as audio at that rate.
	auto points = std::make_shared<std::vector<float>> ();
	impl->windowOf (voice, *points);

	if (! points->empty ())
	{
		state.wave = points;
		state.tableWave = false;		// a recording, not a cycle
		state.sampleRate = (float) ymz280b::rateOf (impl->clock, voice.fnum, voice.mode);
	}

	return state;
}

const NoteTable& Ymz280bDecoder::noteTable (int channel) const
{
	// **A stream has no pitch**, so the table answers only what the row needs: this is a one-shot,
	// drawn as a drum rather than placed on the keyboard.
	class OneShot final : public NoteTable
	{
	public:
		int noteFor (uint32_t period, int, int* finePitch) const override
		{
			if (finePitch != nullptr)
			{
				*finePitch = 0;
			}

			return (period & 0x80000000u) != 0 ? 60 : -1;
		}
	};

	static const OneShot table;
	(void) channel;
	return table;
}

ChannelKind Ymz280bDecoder::channelKind (int channel) const
{
	(void) channel;
	return ChannelKind::adpcmDrum;
}

} // namespace toybox::chips
