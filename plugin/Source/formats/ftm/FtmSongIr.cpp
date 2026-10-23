#include "FtmSongIr.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "formats/ftm/FtmSongLoader.h"

namespace toybox::formats::ftm
{
namespace
{
	constexpr int kAudioRate = 44100;

	/** APU register addresses for the two pulse channels, triangle and noise. */
	constexpr std::uint8_t kPulseControl[2] = { 0x00, 0x04 };
	constexpr std::uint8_t kPulseSweep[2] = { 0x01, 0x05 };
	constexpr std::uint8_t kPulsePeriodLow[2] = { 0x02, 0x06 };
	constexpr std::uint8_t kPulsePeriodHigh[2] = { 0x03, 0x07 };
	constexpr std::uint8_t kTriangleLinear = 0x08;
	constexpr std::uint8_t kTrianglePeriodLow = 0x0A;
	constexpr std::uint8_t kTrianglePeriodHigh = 0x0B;
	constexpr std::uint8_t kNoiseControl = 0x0C;
	constexpr std::uint8_t kNoisePeriod = 0x0E;
	constexpr std::uint8_t kNoiseLength = 0x0F;
	constexpr std::uint8_t kStatus = 0x15;
	constexpr std::uint8_t kFrameCounter = 0x17;

	/** The 2A03's noise periods, shortest first. A note is mapped onto them by index, the way the
		driver's period table does; the mapping is coarse by nature - noise has sixteen pitches, not
		a hundred and twenty-eight. */
	constexpr std::uint16_t kNoisePeriods[16] = { 4, 8, 16, 32, 64, 96, 128, 160,
												  202, 254, 380, 508, 762, 1016, 2034, 4068 };

	int pulsePeriod (int note, std::uint32_t clock)
	{
		const double frequency = 440.0 * std::pow (2.0, (note - 69) / 12.0);
		const double period = (double) clock / (16.0 * frequency) - 1.0;
		return (int) std::lround (std::clamp (period, 8.0, 2047.0));
	}

	int trianglePeriod (int note, std::uint32_t clock)
	{
		const double frequency = 440.0 * std::pow (2.0, (note - 69) / 12.0);
		const double period = (double) clock / (32.0 * frequency) - 1.0;
		return (int) std::lround (std::clamp (period, 2.0, 2047.0));
	}

	int noiseIndex (int note)
	{
		// Every four semitones is one noise period: a whole tone per two steps would run out of
		// table at the bottom of the range, which is where percussion mostly sits.
		return std::clamp ((note - 24) / 4, 0, 15);
	}

	/** The write emitter, with the one behaviour the driver has: a register written with the value
		it already holds costs a tick in the stream and nothing in the sound. Those are dropped, so a
		long steady note is a handful of writes rather than one per frame. */
	class Emitter
	{
	public:
		explicit Emitter (midi::SongIr& song) : ir (song) {}

		void write (int device, std::uint8_t reg, int value, std::uint32_t time)
		{
			const auto key = std::make_pair (device, reg);
			const auto found = last.find (key);
			if (found != last.end() && found->second == value)
			{
				return;
			}

			last[key] = value;

			chips::RegisterWrite entry;
			entry.deviceId = (std::uint32_t) device;
			entry.reg = reg;
			entry.value = (std::uint16_t) (value & 0xFF);
			entry.sampleTime = time;
			ir.writes.push_back (entry);
			ir.totalTicks = std::max (ir.totalTicks, time);
		}

	private:
		midi::SongIr& ir;
		std::map<std::pair<int, int>, int> last;
	};
}

std::uint32_t clockForMachine (int machine)
{
	return (machine == 1) ? 1662607u : nesApuClockHz;
}

bool loadSongIr (const void* data, std::size_t size, midi::SongIr& out, std::string& error)
{
	Module module;
	if (! parseModule (data, size, module, error))
	{
		return false;
	}

	if (module.channels <= 0 || module.frameCount <= 0)
	{
		error = "module has no channels or no frames";
		return false;
	}

	out.chips.clear();
	out.writes.clear();
	out.roms.clear();
	out.tickRate = (double) kAudioRate;

	// PAL runs at 1,662,607 Hz: the same note table is about 7% flatter and the frame lasts
	// 20% longer, so both the period arithmetic and the sample clock follow the machine.
	const std::uint32_t apuClock = clockForMachine (module.machine);

	midi::ChipSlot slot;
	slot.kind = midi::ChipKind::nesApu;
	slot.clockHz = apuClock;
	out.chips.push_back (slot);

	Emitter emitter (out);
	emitter.write (0, kFrameCounter, 0x40, 0);		// four-step sequence, as the driver sets it

	// Patterns by (channel, index), as the loader builds them.
	std::map<std::pair<int, int>, const ModulePattern*> lookup;
	for (const auto& pattern : module.patterns)
	{
		lookup[{ pattern.channel, pattern.index }] = &pattern;
	}

	std::vector<int> channels;
	for (int channel = 0; channel < module.channels; ++channel)
	{
		if (! module.order[(std::size_t) channel].empty())
		{
			channels.push_back (channel);
		}
	}

	std::vector<ApuChannelState> states ((std::size_t) module.channels);
	const double machineRate = (module.machine == 1) ? 50.0069789082 : 60.0988141404;
	const double framesPerSecond = module.framesPerSecond() * (machineRate / 60.0988141404);
	const double samplesPerFrame = (double) kAudioRate / (framesPerSecond > 0.0 ? framesPerSecond : 60.0);

	std::uint32_t clock = (std::uint32_t) std::max (1, module.speed);
	std::uint32_t baseFrame = 0;
	int frame = 0;
	const int maxSteps = module.frameCount * 4 + 64;
	int sounding[2] = { -1, -1 };

	for (int step = 0; frame >= 0 && frame < module.frameCount && step < maxSteps; ++step)
	{
		int nextFrame = frame + 1;

		// The frame's row clock: an Fxx on a row already applies to that row, as in the loader.
		std::vector<std::uint32_t> offsets ((std::size_t) module.patternLength + 1, 0);
		for (int row = 0; row < module.patternLength; ++row)
		{
			for (const int channel : channels)
			{
				const auto found = lookup.find ({ channel, module.order[(std::size_t) channel][(std::size_t) frame] });
				if (found == lookup.end())
				{
					continue;
				}

				for (const auto& entry : found->second->rows)
				{
					if (entry.row != row)
					{
						continue;
					}

					for (const auto& effect : entry.effects)
					{
						if (effect.first == 1 && effect.second > 0)		// Fxx
						{
							clock = (std::uint32_t) effect.second;
						}
					}
				}
			}

			offsets[(std::size_t) row + 1] = offsets[(std::size_t) row] + clock;
		}

		// Every frame of every row, in order.
		for (int row = 0; row < module.patternLength; ++row)
		{
			const std::uint32_t rowStart = baseFrame + offsets[(std::size_t) row];
			const std::uint32_t rowLength = offsets[(std::size_t) row + 1] - offsets[(std::size_t) row];

			for (std::uint32_t tick = 0; tick < rowLength; ++tick)
			{
				const std::uint32_t when = (std::uint32_t) std::lround ((double) (rowStart + tick) * samplesPerFrame);

				for (const int channel : channels)
				{
					ApuChannelState& state = states[(std::size_t) channel];
					const int type = module.channelTypes[(std::size_t) channel];
					const bool pulse = (type == 0 || type == 1);
					const bool triangle = (type == 2);
					const bool noise = (type == 3);

					if (tick == 0)
					{
						const auto found = lookup.find ({ channel,
														 module.order[(std::size_t) channel][(std::size_t) frame] });
						if (found != lookup.end())
						{
							for (const auto& entry : found->second->rows)
							{
								if (entry.row != row)
								{
									continue;
								}

								if (entry.hasNote())
								{
									state.note = 12 + entry.octave * 12 + (entry.note - 1);
									state.instrument = entry.instrument;
									state.macroStep = 0;
									state.effectFrame = 0;
									state.periodOffset = 0.0;
									state.silenced = false;
									state.level = entry.volume > 0 ? entry.volume : 15;
									state.enabled = true;
									sounding[0] = channel;
								}

								if (entry.note == 14)		// cut
								{
									state.note = -1;
									state.enabled = false;
								}
								else if (entry.note == 13)	// release
								{
									state.note = -1;
								}

								for (const auto& effect : entry.effects)
								{
									switch (effect.first)
									{
									case 10:		// 0xy arpeggio: three steps, one per frame
										state.arpeggioX = (effect.second >> 4) & 0x0F;
										state.arpeggioY = effect.second & 0x0F;
										break;

									case 11:		// 4xy vibrato
										state.vibratoSpeed = (effect.second >> 4) & 0x0F;
										state.vibratoDepth = effect.second & 0x0F;
										break;

									case 16:		// 1xx porta up: the period shrinks
										state.portaUnits = effect.second & 0xFF;
										break;

									case 17:		// 2xx porta down
										state.portaUnits = -(effect.second & 0xFF);
										break;

									case 6:			// 3xx tone portamento
										state.tonePortaUnits = effect.second & 0xFF;
										if (entry.hasNote() && state.lastPeriod > 0)
										{
											const int target = 12 + entry.octave * 12 + (entry.note - 1);
											const int goal = triangle ? trianglePeriod (target, apuClock)
																	  : pulsePeriod (target, apuClock);
											state.periodOffset = (double) (state.lastPeriod - goal);
										}
										break;

									case 23:		// Sxx cut: silence now, not a note-off to schedule
										// Silenced rather than disabled: a channel that stops being
										// written keeps its last period and volume, so the cut has to
										// reach the register as level 0.
										state.silenced = true;
										break;

									default:
										break;
									}
								}
							}
						}
					}

					// The instrument's macros run once per frame, not once per row.
					int level = state.level;
					int duty = state.duty;
					int bend = 0;

					if (state.instrument > 0 && state.instrument < (int) module.instruments.size())
					{
						const auto& instrument = module.instruments[(std::size_t) state.instrument];

						if (instrument.volume >= 0 && instrument.volume < (int) module.volumeSequences.size())
						{
							const auto& items = module.volumeSequences[(std::size_t) instrument.volume];
							if (! items.empty())
							{
								level = items[(std::size_t) state.macroStep % items.size()];
							}
						}

						if (instrument.duty >= 0 && instrument.duty < (int) module.dutySequences.size())
						{
							const auto& items = module.dutySequences[(std::size_t) instrument.duty];
							if (! items.empty())
							{
								duty = items[(std::size_t) state.macroStep % items.size()] & 0x03;
							}
						}

						if (instrument.arpeggio >= 0
							&& instrument.arpeggio < (int) module.arpeggioSequences.size())
						{
							const auto& items = module.arpeggioSequences[(std::size_t) instrument.arpeggio];
							if (! items.empty())
							{
								bend = (std::int8_t) items[(std::size_t) state.macroStep % items.size()];
							}
						}
					}

					if (! state.enabled || state.note < 0)
					{
						continue;
					}

					// The row effects move the pitch every frame, which is exactly why the MIDI side
					// turns them into bend events: here they are just numbers to add.
					const int arpSteps[3] = { 0, state.arpeggioX, state.arpeggioY };
					const int arp = arpSteps[(std::size_t) (state.effectFrame % 3)];

					double vibratoCents = 0.0;
					if (state.vibratoSpeed > 0 && state.vibratoDepth > 0)
					{
						const double phase = (double) ((state.effectFrame * state.vibratoSpeed) % 64);
						vibratoCents = 100.0 * ((double) state.vibratoDepth / 16.0)
									   * std::sin (6.283185307179586 * phase / 64.0);
					}

					if (state.portaUnits != 0)
					{
						state.periodOffset += (double) state.portaUnits;
					}

					if (state.tonePortaUnits > 0)
					{
						// 3xx walks the period towards the note's own, and stops on arrival rather
						// than stepping past it.
						if (std::abs (state.periodOffset) <= (double) state.tonePortaUnits)
						{
							state.periodOffset = 0.0;
						}
						else
						{
							state.periodOffset += (state.periodOffset > 0.0)
													  ? -(double) state.tonePortaUnits
													  : (double) state.tonePortaUnits;
						}
					}

					const int pitch = std::clamp (state.note + bend + arp, 0, 127);

					if (pulse)
					{
						const int index = (type == 0) ? 0 : 1;
						const double base = (double) pulsePeriod (pitch, apuClock) + state.periodOffset;
						const int period = (int) std::lround (std::clamp (
							base * std::pow (2.0, -vibratoCents / 1200.0), 8.0, 2047.0));
						emitter.write (0, kPulseControl[index],
									   (duty << 6) | 0x30 | ((state.silenced ? 0 : level) & 0x0F), when);
						emitter.write (0, kPulsePeriodLow[index], period & 0xFF, when);
						emitter.write (0, kPulsePeriodHigh[index], (period >> 8) & 0x07, when);
						emitter.write (0, kStatus, 1 << index, when);
						state.lastPeriod = period;
					}
					else if (triangle)
					{
						const double base = (double) trianglePeriod (pitch, apuClock) + state.periodOffset;
						const int period = (int) std::lround (std::clamp (
							base * std::pow (2.0, -vibratoCents / 1200.0), 2.0, 2047.0));
						emitter.write (0, kTriangleLinear, state.silenced ? 0x00 : 0x7F, when);
						emitter.write (0, kTrianglePeriodLow, period & 0xFF, when);
						emitter.write (0, kTrianglePeriodHigh, (period >> 8) & 0x07, when);
						emitter.write (0, kStatus, 1 << 2, when);
						state.lastPeriod = period;
					}
					else if (noise)
					{
						emitter.write (0, kNoiseControl,
									   0x30 | ((state.silenced ? 0 : level) & 0x0F), when);
						emitter.write (0, kNoisePeriod, noiseIndex (pitch), when);
						emitter.write (0, kStatus, 1 << 3, when);
					}

					++state.macroStep;
					++state.effectFrame;
				}
			}
		}

		// Structure effects, read after the frame has played, as the loader does.
		for (const int channel : channels)
		{
			const auto found = lookup.find ({ channel, module.order[(std::size_t) channel][(std::size_t) frame] });
			if (found == lookup.end())
			{
				continue;
			}

			for (const auto& entry : found->second->rows)
			{
				for (const auto& effect : entry.effects)
				{
					switch (effect.first)
					{
					case 2: nextFrame = effect.second; break;					// Bxx
					case 3: nextFrame = frame + 1 + effect.second; break;		// Dxx
					case 4: nextFrame = -1; break;								// halt
					default: break;
					}
				}
			}
		}

		if (nextFrame <= frame)
		{
			nextFrame = -1;			// the song's loop: one pass, as everywhere else in this loader
		}

		baseFrame += offsets[(std::size_t) module.patternLength];
		frame = nextFrame;
	}

	(void) sounding;
	return true;
}

bool buildVgm (const void* data, std::size_t size, std::vector<std::uint8_t>& out, std::string& error)
{
	midi::SongIr song;
	if (! loadSongIr (data, size, song, error))
	{
		return false;
	}

	out.assign (0x100, 0);
	out[0] = 'V';
	out[1] = 'g';
	out[2] = 'm';
	out[3] = ' ';

	const auto put32 = [&out] (std::size_t at, std::uint32_t value)
	{
		for (int i = 0; i < 4; ++i)
		{
			out[at + (std::size_t) i] = (std::uint8_t) ((value >> (8 * i)) & 0xFF);
		}
	};

	put32 (0x08, 0x00000151);					// version 1.51, which has the NES clock field
	put32 (0x18, song.totalTicks);				// total samples
	// The data offset is relative to 0x34, not absolute: libvgm and this project's own reader both
	// compute 0x34 + field, so writing 0x100 here would put the command stream at 0x134 - past the
	// end of a small image, which the reader rejects as "not a VGM stream".
	put32 (0x34, 0x00000100 - 0x34);
	put32 (0x84, song.chips.empty() ? nesApuClockHz : song.chips[0].clockHz);				// NES APU clock

	// Writes are already ordered by time; a wait is only emitted when the clock actually moves,
	// because zero-wait commands are what the format is designed around.
	std::uint32_t clock = 0;
	for (const auto& write : song.writes)
	{
		if (write.sampleTime > clock)
		{
			std::uint32_t gap = write.sampleTime - clock;
			while (gap > 0)
			{
				const std::uint32_t chunk = std::min<std::uint32_t> (gap, 65535);
				out.push_back (0x61);
				out.push_back ((std::uint8_t) (chunk & 0xFF));
				out.push_back ((std::uint8_t) ((chunk >> 8) & 0xFF));
				gap -= chunk;
				clock += chunk;
			}
		}

		out.push_back (0xB4);
		out.push_back (write.reg);
		out.push_back ((std::uint8_t) (write.value & 0xFF));
	}

	out.push_back (0x66);						// end of stream

	put32 (0x04, (std::uint32_t) out.size() - 4);	// EOF offset, relative to 0x04
	return true;
}

} // namespace toybox::formats::ftm
