#include "Okim6295Decoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>`r`n#include <cstdlib>`r`n#include <cstring>

#include "Pitch.h"

namespace toybox::chips
{
	namespace
	{
		constexpr int kVoices = 4;

		/** The DAC rate: `clock / 165`, or `clock / 132` with pin 7 high (`okim6295.c:444`). */
		constexpr double kDivisorPin7Low = 165.0;
		constexpr double kDivisorPin7High = 132.0;

		/** A voice that cannot be measured still reports a marker note, like the OPN's ADPCM-A. */
		constexpr int kOneShotNote = 60;

		/** How much of a sample is decoded for the measurement, and the window it is measured in:
			the same shape the RF5C164 uses, because an OKI sample also opens with an attack. */
		constexpr uint32_t kScanNibbles = 4096;
		constexpr std::size_t kWindow = 512;
		constexpr std::size_t kScanStep = 256;

		// MSM6295's 4-bit delta codec: a step table indexed by a running state, the nibble's
		// sign-magnitude giving the delta. Only the *shape* matters here - the period of the
		// decoded waveform is what a note comes from, and a scale factor does not move it.
		constexpr int kStepTable[49] = {
			16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
			118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
			598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552 };

		constexpr int kIndexShift[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
	}

	struct Okim6295Decoder::Impl
	{
		struct Voice
		{
			bool playing = false;
			bool melodic = false;
			uint32_t start = 0;
			uint32_t stop = 0;
			uint32_t gateEnd = 0;
			int keyOns = 0;
			int volume = 0;
			int command = -1;
			double cycleLength = 0.0;
		};

		/** One table per voice: the note is the sample's own cycle at the chip's fixed rate, so the
			voice itself is the table's only input. */
		class NoteTableImpl final : public NoteTable
		{
		public:
			NoteTableImpl (const Impl* owner, int index, double voiceRate)
				: impl (owner), voice (index), rate (voiceRate) {}

			int noteFor (uint32_t, int, int* finePitch) const override
			{
				const Voice& state = impl->voices[voice];
			return noteFromSampleCycle (rate, state.cycleLength, state.melodic, kOneShotNote, finePitch);
			}

		private:
			const Impl* impl;
			int voice;
			double rate;
		};

		uint32_t clock = 1000000;
		uint32_t now = 0;
		double outRate = 44100.0;
		bool pin7High = false;
		int pendingCommand = -1;
		std::vector<uint8_t> rom;
		std::map<uint32_t, double> cycleCache;
		Voice voices[kVoices];
		std::vector<std::unique_ptr<NoteTableImpl>> tables;

		double sampleRate (void) const
		{
			return (double) clock / (pin7High ? kDivisorPin7High : kDivisorPin7Low);
		}

		uint8_t byteAt (uint32_t address) const
		{
			return (address < rom.size()) ? rom[address] : 0xFF;
		}

		/** The nibble stream decoded into a waveform: MSM6295's delta codec, as `okim6295.c` runs it. */
		std::vector<double> decode (uint32_t start, uint32_t nibbles) const
		{
			std::vector<double> waveform;
			waveform.reserve (nibbles);

			int signal = 0;
			int stepIndex = 0;
			for (uint32_t i = 0; i < nibbles; ++i)
			{
				const uint8_t byte = byteAt (start + i / 2);
				const int nibble = (i % 2 == 0) ? (byte >> 4) : (byte & 0x0F);

				const int magnitude = nibble & 0x07;
				const int delta = kStepTable[stepIndex] * magnitude / 2;
				signal += (nibble & 0x08) != 0 ? -delta : delta;
				signal = std::max (-2048, std::min (2047, signal));

				stepIndex += kIndexShift[nibble];
				stepIndex = std::max (0, std::min (48, stepIndex));

				waveform.push_back ((double) signal);
			}

			return waveform;
		}

		/** The period of a window, or 0 when it has none: a lag whose correlation troughs below
			`-0.5` and then peaks above `0.5`, taken from the trough so continuity at lag 2 cannot pass
			for a period. */
		static double bestPeriod (const std::vector<double>& waveform)
		{
			const std::size_t size = waveform.size();
			if (size < 64)
			{
				return 0.0;
			}

			double mean = 0.0;
			for (double sample : waveform)
			{
				mean += sample;
			}

			mean /= (double) size;

			const std::size_t maxLag = std::min<std::size_t> (size / 2, kWindow / 2);
			std::vector<double> correlation (maxLag + 1, 0.0);
			bool sawTrough = false;
			std::size_t troughLag = 0;
			double best = 0.0;

			for (std::size_t lag = 2; lag <= maxLag; ++lag)
			{
				double product = 0.0;
				double energyA = 0.0;
				double energyB = 0.0;
				for (std::size_t i = 0; i + lag < size; ++i)
				{
					const double a = waveform[i] - mean;
					const double b = waveform[i + lag] - mean;
					product += a * b;
					energyA += a * a;
					energyB += b * b;
				}

				if (energyA <= 0.0 || energyB <= 0.0)
				{
					continue;
				}

				correlation[lag] = product / std::sqrt (energyA * energyB);
				if (! sawTrough)
				{
					sawTrough = correlation[lag] <= -0.5;
					troughLag = lag;
				}
				else
				{
					best = std::max (best, correlation[lag]);
				}
			}

			if (! sawTrough || best < 0.5)
			{
				return 0.0;
			}

			for (std::size_t lag = troughLag; lag <= maxLag; ++lag)
			{
				if (correlation[lag] >= best * 0.9 && correlation[lag] >= 0.5)
				{
					return (double) lag;
				}
			}

			return 0.0;
		}

		/** The sample's cycle in nibbles, measured once per sample number: the ROM table is
			immutable, so a cache keyed by the sample is exact. */
		double cycleFor (int command)
		{
			const auto cached = cycleCache.find ((uint32_t) command);
			if (cached != cycleCache.end())
			{
				return cached->second;
			}

			double cycle = 0.0;
			const uint32_t base = (uint32_t) command * 8;
			const uint32_t start =
				((uint32_t) byteAt (base) << 16 | (uint32_t) byteAt (base + 1) << 8
				 | (uint32_t) byteAt (base + 2)) & 0x3FFFFu;
			const uint32_t stop =
				((uint32_t) byteAt (base + 3) << 16 | (uint32_t) byteAt (base + 4) << 8
				 | (uint32_t) byteAt (base + 5)) & 0x3FFFFu;

			if (start < stop && ! rom.empty())
			{
				const uint32_t nibbles = std::min<uint32_t> (2 * (stop - start + 1),
																	   kScanNibbles);
				const std::vector<double> waveform = decode (start, nibbles);
				for (std::size_t offset = 0; cycle <= 0.0 && offset + 64 <= waveform.size();
					 offset += kScanStep)
				{
					const std::size_t length = std::min<std::size_t> (kWindow, waveform.size() - offset);
					cycle = bestPeriod (std::vector<double> (waveform.begin() + (long) offset,
															 waveform.begin() + (long) (offset + length)));
				}
			}

			cycleCache[(uint32_t) command] = cycle;
			return cycle;
		}

		void startVoices (uint8_t data, uint32_t time)
		{
			const int command = pendingCommand;
			pendingCommand = -1;
			if (command < 0)
			{
				return;
			}

			const uint32_t base = (uint32_t) command * 8;
			const uint32_t start =
				((uint32_t) byteAt (base) << 16 | (uint32_t) byteAt (base + 1) << 8
				 | (uint32_t) byteAt (base + 2)) & 0x3FFFFu;
			const uint32_t stop =
				((uint32_t) byteAt (base + 3) << 16 | (uint32_t) byteAt (base + 4) << 8
				 | (uint32_t) byteAt (base + 5)) & 0x3FFFFu;

			const double rate = sampleRate();
			const uint32_t nibbles = (start < stop) ? 2 * (stop - start + 1) : 0;
			const double cycle = (start < stop) ? cycleFor (command) : 0.0;

			for (int voice = 0; voice < kVoices; ++voice)
			{
				if ((data & (0x10 << voice)) == 0)
				{
					continue;
				}

				Voice& state = voices[voice];
				if (state.playing || nibbles == 0)
				{
					continue;		// the manual: one voice at a time, and invalid samples do nothing
				}

				state.playing = true;
				state.command = command;
				state.start = start;
				state.stop = stop;
				state.volume = data & 0x0F;
				state.cycleLength = cycle;
				state.melodic = cycle > 0.0;
				++state.keyOns;

				// the length is exact: `2 * (stop - start + 1)` nibbles at the fixed DAC rate
				state.gateEnd = time + (uint32_t) std::llround ((double) nibbles * outRate / rate);
			}
		}
	};

	// the pimpl's destructor has to live where Impl is complete
	Okim6295Decoder::Okim6295Decoder() = default;
	Okim6295Decoder::~Okim6295Decoder() = default;

	void Okim6295Decoder::reset (const ChipConfig& config)
{
	[[maybe_unused]] const uint32_t clock = config.clockHz;
	[[maybe_unused]] const double sampleRate = config.timeBaseHz;
		impl = std::make_unique<Impl>();
		impl->clock = clock > 0 ? clock : 1000000;
		impl->outRate = sampleRate > 0.0 ? sampleRate : 44100.0;

		for (int voice = 0; voice < kVoices; ++voice)
		{
			impl->tables.push_back (std::make_unique<Impl::NoteTableImpl> (impl.get(), voice,
																		 impl->sampleRate()));
		}
	}

	void Okim6295Decoder::setRom (std::vector<uint8_t> rom)
	{
		impl->rom = std::move (rom);
		impl->cycleCache.clear();
	}

	void Okim6295Decoder::write (const RegisterWrite& write)
	{
		if (write.port != 0)
		{
			return;		// the second chip of a pair is a separate decoder instance
		}

		switch (write.reg)
		{
		case 0x00:
			// A pending sample number means *this* byte starts voices, bit 7 included: the mask is
			// bits 4-7, so voice 3 is only reachable through a byte that also has bit 7 set. Testing
			// bit 7 first (as if it always meant "sample select") lost that voice in every song.
			if (impl->pendingCommand >= 0)
			{
				impl->startVoices (write.value, write.sampleTime);
			}
			else if ((write.value & 0x80) != 0)
			{
				impl->pendingCommand = write.value & 0x7F;
			}
			else
			{
				// silence: bits 3-6 name the voices
				for (int voice = 0; voice < kVoices; ++voice)
				{
					if ((write.value & (0x08 << voice)) != 0)
					{
						impl->voices[voice].playing = false;
					}
				}
			}
			break;
		case 0x0C:
			impl->pin7High = (write.value & 0x01) != 0;
			break;
		default:
			break;		// clock / NMK bank registers do not move a note
		}
	}

	void Okim6295Decoder::advance (uint32_t sampleTime)
	{
		// every note's end is an absolute sample, so the clock only has to be remembered for the
		// gate comparison in channelState()
		impl->now = sampleTime;
	}

	int Okim6295Decoder::channelCount() const
	{
		return kVoices;
	}

	ChannelState Okim6295Decoder::channelState (int channel) const
	{
		ChannelState state;
		if (channel < 0 || channel >= kVoices)
		{
			return state;
		}

		const Impl::Voice& voice = impl->voices[channel];
		state.present = true;
		state.gate = voice.playing && (voice.gateEnd == 0 || impl->now < voice.gateEnd);
		state.keyOns = voice.keyOns;
		state.period = (voice.command >= 0) ? (uint32_t) voice.command : 0;
		state.volume = 15 - voice.volume;		// the field is an attenuation index: 0 is the loudest

		// no L1 at all: the chip plays at one fixed rate, so the whole pitch comes from the
		// sample's own cycle - the ES5503's situation (design doc section 14.3)
		state.hz = voice.melodic ? impl->sampleRate() / voice.cycleLength : 0.0;
		state.transposeCents = 0;
		state.rootCents = 0;
		state.pitchSource = voice.melodic ? PitchSource::sampleInferred : PitchSource::unpitched;

		return state;
	}

	const NoteTable& Okim6295Decoder::noteTable (int channel) const
	{
		// a channel out of range has no state and therefore no pitch: the C4 marker
		struct Fallback final : NoteTable
		{
			int noteFor (uint32_t, int, int* finePitch) const override
			{
				if (finePitch != nullptr)
				{
					*finePitch = 0;
				}

				return kOneShotNote;
			}
		};

		static const Fallback fallback;
		if (channel < 0 || channel >= (int) impl->tables.size())
		{
			return fallback;
		}

		return *impl->tables[(std::size_t) channel];
	}

	ChannelKind Okim6295Decoder::channelKind (int) const
	{
		return ChannelKind::okimAdpcm;
	}

	bool Okim6295Decoder::isDrum (int channel) const
	{
		return channel < 0 || channel >= kVoices || ! impl->voices[channel].melodic;
	}
} // namespace toybox::chips
