#include "MidiSequenceBuilder.h"

#include <algorithm>
#include <cmath>
#include <map>

#include "VgmCommandReader.h"

#include "chips/NesApuDecoder.h"
#include "chips/NoteExtractor.h"

#include "emu/SoundDevs.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr double kVgmRate = 44100.0;

	// One-shot channels have no reliable pitch.  C4 is only a trigger marker, not a
	// transcription of the sample; users can map it with a MIDI effect in the DAW.
	constexpr int kOneShotNote = 60;

	struct SnState
	{
		int period[3] = { 0, 0, 0 };
		int periodLow[3] = { 0, 0, 0 };
		int volume[4] = { 15, 15, 15, 15 };
		int note[4] = { -1, -1, -1, -1 };
		bool active[4] = { false, false, false, false };
		int latch = 0;
		bool latchIsVolume = false;
	};

	struct AyState
	{
		UINT8 regs[16] = {};
		int note[3] = { -1, -1, -1 };
		bool active[3] = { false, false, false };
	};

	struct FmChannel
	{
		int fnum = 0;
		int block = 0;
		int note = -1;
		bool active = false;
		bool keyHeld = false;
	};

	struct FmState
	{
		FmChannel channels[6];
	};

	/** YM2151 has no F-Number: KC (block + note) and KF (fraction) are the pitch source. */
	struct OpmChannel
	{
		int kc = 0;
		int kf = 0;
		int note = -1;
		bool active = false;
		bool keyHeld = false;
		bool havePitch = false;
	};

	struct OpmState
	{
		OpmChannel channels[8];
	};

	enum class OplKind
	{
		opll,	// YM2413: 9 melodic channels, rhythm on 0x0E
		opl2,	// YM3526/YM3812/Y8950: 9 melodic, rhythm on 0xBD
		opl3	// YMF262: two 9-channel banks, rhythm on 0xBD
	};

	struct OplChannel
	{
		int fnum = 0;
		int block = 0;
		int note = -1;
		bool active = false;
		bool keyHeld = false;
	};

	struct OplState
	{
		OplChannel channels[18];
		UINT8 rhythmBits = 0;
		bool rhythmEnabled = false;
		bool adpcmHeld = false;		// Y8950 Delta-T start bit
	};

	/** The YM2612 DAC is a stream of sample bytes, not notes; rate-limit it to drum markers. */
	struct DacState
	{
		bool enabled = false;
		double lastTrigger = -100.0;
	};

	struct AdpcmState
	{
		bool activeA[6] = { false, false, false, false, false, false };
		bool activeB = false;
	};

	struct GbState
	{
		UINT8 regs[0x30] = {};
		int note[4] = { -1, -1, -1, -1 };
		bool active[4] = { false, false, false, false };
		bool powered = true;
	};

	struct Rf5cState
	{
		bool active[8] = {};
		bool master = true;			// 0x07 bit 7: sound output enable
		UINT8 channelMask = 0xFF;	// 0x08, a cleared bit enables the channel
	};

	/** A one-shot note is closed by a timer, but a retrigger before that timer expires must not
		be cut off by the previous note's scheduled note-off.  The pending event is moved to the
		retrigger instant instead. */
	struct PendingOneShot
	{
		std::size_t eventIndex = 0;
		double time = -1.0;
		bool valid = false;
	};

	std::uint64_t deviceChannelKey (playback::DeviceId device, int channel)
	{
		return ((std::uint64_t) device << 8) | (std::uint64_t) (channel & 0xFF);
	}

	std::uint64_t typeInstanceKey (DEV_ID type, int instance)
	{
		return ((std::uint64_t) type << 16) | (std::uint64_t) (instance & 0xFFFF);
	}

	class Builder
	{
	public:
		Builder (const std::vector<PLR_DEV_INFO>& deviceList,
				 const std::vector<playback::TrackInfo>& trackList)
			: devices (deviceList), tracks (trackList)
		{
			for (const auto& device : devices)
			{
				if (device.parentIdx != (UINT32) -1)
				{
					continue;
				}

				deviceByType[typeInstanceKey (device.type, device.instance)] = &device;
				if (deviceByType.count (typeInstanceKey (device.type, 0)) == 0)
				{
					deviceByType[typeInstanceKey (device.type, 0)] = &device;
				}
			}

			for (std::size_t i = 0; i < tracks.size(); ++i)
			{
				if (tracks[i].channelIndex >= 0)
				{
					trackByDeviceChannel[deviceChannelKey (tracks[i].deviceId, tracks[i].channelIndex)] = (int) i;
				}
			}
		}

		std::unique_ptr<playback::MidiSequence> parse (const void* data, std::size_t size)
		{
			VgmCommandReader reader (data, size);
			if (! reader.isValid())
			{
				return nullptr;
			}

			VgmCommand command;
			std::uint32_t lastSampleTime = 0;
			while (reader.next (command))
			{
				const double time = (double) command.sampleTime / kVgmRate;
				lastSampleTime = command.sampleTime;

				switch (command.command)
				{
				case 0x50:
					writeSn (deviceFor (DEVID_SN76496, 0), command.addr, time);
					break;
				case 0x51:
					writeOpl (deviceFor (DEVID_YM2413, 0), false, command.addr, command.data,
							  time, OplKind::opll);
					break;
				case 0x52:
				case 0x53:
					writeFm (deviceFor (DEVID_YM2612, 0), command.command == 0x53, command.addr,
							 command.data, time, false, 6);
					break;
				case 0x54:
					writeOpm (deviceFor (DEVID_YM2151, 0), command.addr, command.data, time);
					break;
				case 0x55:
					writeFm (deviceFor (DEVID_YM2203, 0), false, command.addr, command.data,
							 time, false, 3);
					break;
				case 0x56:
				case 0x57:
					writeYm2608 (deviceFor (DEVID_YM2608, 0), command.command == 0x57, command.addr,
								 command.data, time);
					break;
				case 0x58:
				case 0x59:
					writeYm2610 (deviceFor (DEVID_YM2610, 0), command.command == 0x59, command.addr,
								 command.data, time);
					break;
				case 0x5A:
					writeOpl (deviceFor (DEVID_YM3812, 0), false, command.addr, command.data,
							  time, OplKind::opl2);
					break;
				case 0x5B:
					writeOpl (deviceFor (DEVID_YM3526, 0), false, command.addr, command.data,
							  time, OplKind::opl2);
					break;
				case 0x5C:
					writeOpl (deviceFor (DEVID_Y8950, 0), false, command.addr, command.data,
							  time, OplKind::opl2);
					break;
				case 0x5E:
				case 0x5F:
					writeOpl (deviceFor (DEVID_YMF262, 0), command.command == 0x5F, command.addr,
							  command.data, time, OplKind::opl3);
					break;
				case 0xA0:
					writeAy (deviceFor (DEVID_AY8910, 0), command.addr, command.data, time);
					break;
				case 0xA1:
					writeOpl (deviceFor (DEVID_YM2413, 1), false, command.addr, command.data,
							  time, OplKind::opll);
					break;
				case 0xA4:
					writeOpm (deviceFor (DEVID_YM2151, 1), command.addr, command.data, time);
					break;
				case 0xA5:
					writeFm (deviceFor (DEVID_YM2203, 1), false, command.addr, command.data,
							 time, false, 3);
					break;
				case 0xA6:
				case 0xA7:
					writeYm2608 (deviceFor (DEVID_YM2608, 1), command.command == 0xA7, command.addr,
								 command.data, time);
					break;
				case 0xAA:
					writeOpl (deviceFor (DEVID_YM3812, 1), false, command.addr, command.data,
							  time, OplKind::opl2);
					break;
				case 0xAB:
					writeOpl (deviceFor (DEVID_YM3526, 1), false, command.addr, command.data,
							  time, OplKind::opl2);
					break;
				case 0xAC:
					writeOpl (deviceFor (DEVID_Y8950, 1), false, command.addr, command.data,
							  time, OplKind::opl2);
					break;
				case 0xAE:
				case 0xAF:
					writeOpl (deviceFor (DEVID_YMF262, 1), command.command == 0xAF, command.addr,
							  command.data, time, OplKind::opl3);
					break;
				case 0xB0:	// RF5C68
					writeRf5c (rf5cDeviceFor (false, command.port),
							   command.addr, command.data, time);
					break;
				case 0xB1:	// RF5C164
					writeRf5c (rf5cDeviceFor (true, command.port),
							   command.addr, command.data, time);
					break;
				case 0xB3:
					writeGb (deviceFor (DEVID_GB_DMG, command.port),
							 command.addr, command.data, time);
					break;
				case 0xB4:
					queueNesWrite (deviceFor (DEVID_NES_APU, command.port),
								   command.addr, command.data, command.sampleTime);
					break;
				case 0xA2:
				case 0xA3:
					writeFm (deviceFor (DEVID_YM2612, 1), command.command == 0xA3, command.addr,
							 command.data, time, false, 6);
					break;
				case 0xA8:
				case 0xA9:
					writeYm2610 (deviceFor (DEVID_YM2610, 1), command.command == 0xA9, command.addr,
								 command.data, time);
					break;
				default:
					break;	// waits and stream end are handled by the reader
				}
			}

			// chips with a real state machine are decoded in one pass after the stream is read:
			// their note boundaries come from the chip's own counters, not from a register write
			flushNes (lastSampleTime);

			if (events.empty())
			{
				return nullptr;
			}

			std::stable_sort (events.begin(), events.end(),
							  [] (const playback::MidiEvent& a, const playback::MidiEvent& b)
							  {
								  return a.timeSeconds < b.timeSeconds;
							  });

			auto sequence = std::make_unique<playback::MidiSequence>();
			sequence->events = std::move (events);
			return sequence;
		}

	private:
		/** Buffers one 2A03 register write. The chip's notes cannot be read off a single write:
			they end when the length counter or the envelope says so, which only the decoder's own
			frame sequencer knows. */
		void queueNesWrite (const PLR_DEV_INFO* device, std::uint8_t reg, std::uint8_t value,
							std::uint32_t sampleTime)
		{
			if (device == nullptr)
			{
				return;
			}

			chips::RegisterWrite write;
			write.deviceId = device->id;
			write.reg = reg;
			write.value = value;
			write.sampleTime = sampleTime;
			nesWrites[device->id].push_back (write);
			nesDevices[device->id] = device;
		}

		void flushNes (std::uint32_t totalSamples)
		{
			for (auto& entry : nesWrites)
			{
				const auto device = nesDevices.find (entry.first);
				if (device == nesDevices.end() || device->second == nullptr)
				{
					continue;
				}

				const std::uint32_t clock = (device->second->devCfg != nullptr
											 && device->second->devCfg->clock > 0)
												? device->second->devCfg->clock
												: 1789773u;

				chips::NesApuDecoder decoder;
				decoder.reset (clock, (double) kVgmRate);

				const auto notes = chips::extractNotes (decoder, entry.second, totalSamples);
				for (const auto& note : notes)
				{
					const int track = trackFor (device->second, note.logicalChannel);
					if (track < 0)
					{
						continue;
					}

					const double start = (double) note.startSample / kVgmRate;
					const double end = (double) note.endSample / kVgmRate;
					noteOn (track, start, note.note, note.velocity);
					noteOff (track, end, note.note);
				}
			}

			nesWrites.clear();
			nesDevices.clear();
		}

		const PLR_DEV_INFO* deviceFor (DEV_ID type, int instance) const
		{
			const auto exact = deviceByType.find (typeInstanceKey (type, instance));
			if (exact != deviceByType.end())
			{
				return exact->second;
			}

			const auto first = deviceByType.find (typeInstanceKey (type, 0));
			return first == deviceByType.end() ? nullptr : first->second;
		}

		// Both the RF5C68 and the RF5C164 report DEVID_RF5C68; only the config flags tell them
		// apart, so 0xB0/0xB1 writes must find the matching variant, not just the first one.
		const PLR_DEV_INFO* rf5cDeviceFor (bool is164, int instance) const
		{
			for (const auto& device : devices)
			{
				if (device.parentIdx != (UINT32) -1 || device.type != DEVID_RF5C68
					|| device.instance != instance || device.devCfg == nullptr)
				{
					continue;
				}

				if (((device.devCfg->flags & 0x01) != 0) == is164)
				{
					return &device;
				}
			}

			return deviceFor (DEVID_RF5C68, instance);
		}

		int trackFor (const PLR_DEV_INFO* device, int channel) const
		{
			if (device == nullptr)
			{
				return -1;
			}

			const auto it = trackByDeviceChannel.find (deviceChannelKey (device->id, channel));
			return it == trackByDeviceChannel.end() ? -1 : it->second;
		}

		void noteOn (int track, double time, int note, int velocity)
		{
			if (track < 0 || track >= (int) tracks.size() || note < 0 || note > 127)
			{
				return;
			}

			playback::MidiEvent event;
			event.timeSeconds = time;
			event.type = playback::MidiEventType::noteOn;
			event.data1 = (std::uint8_t) note;
			event.data2 = (std::uint8_t) juce::jlimit (1, 127, velocity);
			event.sourceTrack = tracks[(std::size_t) track].id;
			events.push_back (event);
		}

		void noteOff (int track, double time, int note)
		{
			if (track < 0 || track >= (int) tracks.size() || note < 0 || note > 127)
			{
				return;
			}

			playback::MidiEvent event;
			event.timeSeconds = time;
			event.type = playback::MidiEventType::noteOff;
			event.data1 = (std::uint8_t) note;
			event.data2 = 0;
			event.sourceTrack = tracks[(std::size_t) track].id;
			events.push_back (event);
		}

		/** Pitch bend with the MIDI default range of +/- 2 semitones. The value is clamped so a
			large slide cannot wrap around; the caller retriggers instead of exceeding the range. */
		void pitchBend (int track, double time, double semitones)
		{
			if (track < 0 || track >= (int) tracks.size())
			{
				return;
			}

			const int value = (int) std::lround (juce::jlimit (-2.0, 2.0, semitones) * 4096.0);
			const int biased = juce::jlimit (0, 16383, value + 8192);

			playback::MidiEvent event;
			event.timeSeconds = time;
			event.type = playback::MidiEventType::pitchBend;
			event.data1 = (std::uint8_t) (biased & 0x7F);
			event.data2 = (std::uint8_t) ((biased >> 7) & 0x7F);
			event.sourceTrack = tracks[(std::size_t) track].id;
			events.push_back (event);
		}

		/** Starts a melodic note at its exact pitch and resets any bend left by the previous note. */
		void startMelodicNote (int track, double time, int note, int velocity)
		{
			pitchBend (track, time, 0.0);
			noteOn (track, time, note, velocity);
		}

		/** Follows a continuously changing pitch. Small changes become pitch bend; only a
			change outside the +/- 2 semitone MIDI range retriggers the note. */
		void updateMelodicPitch (int track, double time, int& note, bool& active,
								 double pitch, int velocity)
		{
			if (pitch < 0.0 || ! std::isfinite (pitch))
			{
				if (active)
				{
					noteOff (track, time, note);
					active = false;
					note = -1;
				}
				return;
			}

			const int nearest = juce::jlimit (1, 127, (int) std::lround (pitch));
			if (! active)
			{
				note = nearest;
				startMelodicNote (track, time, nearest, velocity);
				active = true;
				return;
			}

			const double delta = pitch - (double) note;
			if (std::abs (delta) <= 2.0)
			{
				pitchBend (track, time, delta);
				return;
			}

			noteOff (track, time, note);
			note = nearest;
			startMelodicNote (track, time, note, velocity);
			pitchBend (track, time, pitch - (double) note);
			active = true;
		}

		/** Emits a C4 trigger marker for a channel that has no usable pitch.  The note is closed
			after a short default gate, or earlier if the same track retriggers. */
		void triggerOneShot (int track, double time, double gate = 0.08)
		{
			if (track < 0)
			{
				return;
			}

			PendingOneShot& pending = oneShotPending[track];
			if (pending.valid && pending.eventIndex < events.size()
				&& events[pending.eventIndex].timeSeconds > time)
			{
				events[pending.eventIndex].timeSeconds = time;
			}

			noteOn (track, time, kOneShotNote, 100);
			noteOff (track, time + gate, kOneShotNote);
			pending.eventIndex = events.size() - 1;
			pending.time = time + gate;
			pending.valid = true;
		}

		static double noteFromFrequencyF (double frequency)
		{
			if (! (frequency > 0.0) || ! std::isfinite (frequency))
			{
				return -1.0;
			}

			return 69.0 + 12.0 * std::log2 (frequency / 440.0);
		}

		static int noteFromFrequency (double frequency)
		{
			const double note = noteFromFrequencyF (frequency);
			return note < 0.0 ? -1 : juce::jlimit (1, 127, (int) std::lround (note));
		}

		void writeSn (const PLR_DEV_INFO* device, UINT8 data, double time)
		{
			if (device == nullptr)
			{
				return;
			}

			SnState& state = snStates[device->id];
			if (data & 0x80)
			{
				state.latch = (data >> 5) & 0x03;
				state.latchIsVolume = (data & 0x10) != 0;
				if (state.latchIsVolume)
				{
					state.volume[state.latch] = data & 0x0F;
				}
				else
				{
					state.periodLow[state.latch] = data & 0x0F;
				}
			}
			else if (! state.latchIsVolume && state.latch < 3)
			{
				state.period[state.latch] = state.periodLow[state.latch] | ((data & 0x3F) << 4);
			}

			refreshSnChannel (device, state, state.latch, time);
		}

		void refreshSnChannel (const PLR_DEV_INFO* device, SnState& state, int channel, double time)
		{
			if (channel < 0 || channel > 3)
			{
				return;
			}

			const int track = trackFor (device, channel);
			const bool audible = state.volume[channel] < 15;
			if (channel == 3)
			{
				if (audible && ! state.active[channel])
				{
					state.note[channel] = kOneShotNote;
					noteOn (track, time, kOneShotNote, 100);
					state.active[channel] = true;
				}
				else if (! audible && state.active[channel])
				{
					noteOff (track, time, state.note[channel]);
					state.active[channel] = false;
				}
				return;
			}

			const int period = state.period[channel];
			const double clock = device->devCfg != nullptr ? (double) device->devCfg->clock : 0.0;
			const double frequency = (period > 0) ? clock / (32.0 * period) : 0.0;
			const double pitch = audible ? noteFromFrequencyF (frequency) : -1.0;
			updateMelodicPitch (track, time, state.note[channel], state.active[channel], pitch, 100);
		}

		void writeAy (const PLR_DEV_INFO* device, UINT8 addr, UINT8 data, double time)
		{
			if (device == nullptr || addr >= 16)
			{
				return;
			}

			AyState& state = ayStates[device->id];
			state.regs[addr] = data;
			for (int channel = 0; channel < 3; ++channel)
			{
				const int period = state.regs[channel * 2]
								   | ((state.regs[channel * 2 + 1] & 0x0F) << 8);
				const int amplitude = state.regs[0x08 + channel] & 0x1F;
				const bool toneEnabled = (state.regs[0x07] & (1 << channel)) == 0;
				const double clock = device->devCfg != nullptr ? (double) device->devCfg->clock : 0.0;
				const double frequency = (period > 0) ? clock / (16.0 * period) : 0.0;
				const double pitch = (amplitude > 0 && toneEnabled) ? noteFromFrequencyF (frequency) : -1.0;
				const int track = trackFor (device, channel);
				updateMelodicPitch (track, time, state.note[channel], state.active[channel], pitch,
									juce::jlimit (1, 127, amplitude * 4));
			}
		}

		void writeGb (const PLR_DEV_INFO* device, UINT8 addr, UINT8 data, double time)
		{
			if (device == nullptr || addr >= 0x30)
			{
				return;
			}

			GbState& state = gbStates[device->id];
			state.regs[addr] = data;

			if (addr == 0x16)	// NR52
			{
				if ((data & 0x80) == 0)
				{
					for (int channel = 0; channel < 4; ++channel)
					{
						if (state.active[channel])
						{
							noteOff (trackFor (device, channel), time, state.note[channel]);
							state.active[channel] = false;
						}
					}
				}
				state.powered = (data & 0x80) != 0;
				return;
			}

			if (addr == 0x14 || addr == 0x15)	// NR50 / NR51
			{
				return;
			}

			int channel = -1;
			UINT8 base = 0;
			// `base` is the register of the channel's first tone register minus four, so that
			// base + 2/3/4 are the volume, frequency low and frequency high registers.  Pulse 1
			// starts at NR10 (sweep), pulse 2 at 0x06 with no sweep - for it base stays 0x05.
			if (addr <= 0x04)						{ channel = 0; base = 0x00; }
			else if (addr >= 0x06 && addr <= 0x09)	{ channel = 1; base = 0x05; }
			else if (addr >= 0x0A && addr <= 0x0E)	{ channel = 2; base = 0x0A; }
			else if (addr >= 0x10 && addr <= 0x13)	{ channel = 3; base = 0x10; }
			else
			{
				return;	// wave RAM
			}

			const double clock = (device->devCfg != nullptr && device->devCfg->clock > 0)
									 ? (double) device->devCfg->clock
									 : 4194304.0;

			const auto frequencyReg = [&] (void)
			{
				switch (channel)
				{
				case 2:		return (int) state.regs[0x0D] | (((int) state.regs[0x0E] & 0x07) << 8);
				case 3:		return (int) state.regs[0x12] | (((int) state.regs[0x13] & 0x07) << 8);
				default:	return (int) state.regs[base + 3] | (((int) state.regs[base + 4] & 0x07) << 8);
				}
			};

			const auto audible = [&] (void)
			{
				switch (channel)
				{
				case 2:
					return (state.regs[0x0A] & 0x80) != 0 && ((state.regs[0x0C] >> 5) & 0x03) != 0;
				case 3:
					return (state.regs[0x11] & 0xF8) != 0;
				default:
					return (state.regs[base + 2] & 0xF8) != 0;
				}
			};

			const auto velocity = [&] (void)
			{
				switch (channel)
				{
				case 2:		return 100;
				case 3:		return juce::jlimit (1, 127, (int) (state.regs[0x11] >> 4) * 8);
				default:	return juce::jlimit (1, 127, (int) (state.regs[base + 2] >> 4) * 8);
				}
			};

			const int track = trackFor (device, channel);
			const bool triggerAddr = (channel == 2 && addr == 0x0E)
									 || (channel == 3 && addr == 0x13)
									 || ((channel == 0 || channel == 1) && addr == base + 4);

			if (triggerAddr && (data & 0x80) != 0)
			{
				if (state.active[channel])
				{
					noteOff (track, time, state.note[channel]);
					state.active[channel] = false;
				}

				if (! state.powered || ! audible())
				{
					return;
				}

				if (channel == 3)
				{
					triggerOneShot (track, time);
					return;
				}

				const int f = frequencyReg();
				if (f >= 2048)
				{
					return;
				}

				const double frequency = (channel == 2 ? clock / 64.0 : clock / 32.0)
										 / (double) (2048 - f);
				const int note = noteFromFrequency (frequency);
				if (note >= 0)
				{
					startMelodicNote (track, time, note, velocity());
					state.note[channel] = note;
					state.active[channel] = true;
				}
				return;
			}

			if (! state.active[channel])
			{
				return;
			}

			if (! audible())
			{
				noteOff (track, time, state.note[channel]);
				state.active[channel] = false;
				return;
			}

			const bool frequencyWrite = (channel == 2 && (addr == 0x0D || addr == 0x0E))
										|| (channel == 0 || channel == 1) && (addr == base + 3 || addr == base + 4)
										|| (channel == 0 && addr == 0x00);	// sweep
			if (frequencyWrite)
			{
				const int f = frequencyReg();
				const double frequency = f < 2048
											 ? (channel == 2 ? clock / 64.0 : clock / 32.0)
												   / (double) (2048 - f)
											 : 0.0;
				updateMelodicPitch (track, time, state.note[channel], state.active[channel],
									noteFromFrequencyF (frequency), velocity());
			}
		}

		void writeRf5c (const PLR_DEV_INFO* device, UINT8 addr, UINT8 data, double time)
		{
			if (device == nullptr || addr > 0x08)
			{
				return;
			}

			Rf5cState& state = rf5cStates[device->id];

			// Bit i cleared enables channel i; the sample runs until the driver disables it.
			// The fixed gate only closes the MIDI note so a lone sample cannot hold forever.
			const auto applyChannelMask = [&] (void)
			{
				if (! state.master)
				{
					return;
				}

				for (int channel = 0; channel < 8; ++channel)
				{
					const bool on = ((state.channelMask >> channel) & 1) == 0;
					if (on && ! state.active[channel])
					{
						triggerOneShot (trackFor (device, channel), time);
					}
					state.active[channel] = on;
				}
			};

			if (addr == 0x07)
			{
				const bool master = (data & 0x80) != 0;
				if (master && ! state.master)
				{
					// The core resumes from the channel's current address; retriggering the
					// one-shot marker is the closest MIDI approximation.
					for (bool& active : state.active)
					{
						active = false;
					}
					state.master = true;
					applyChannelMask();
				}
				else if (! master)
				{
					for (bool& active : state.active)
					{
						active = false;
					}
					state.master = false;
				}
				return;
			}

			if (addr == 0x08)
			{
				state.channelMask = data;
				applyChannelMask();
			}
		}

		void writeYm2610 (const PLR_DEV_INFO* device, bool port1, UINT8 addr, UINT8 data, double time)
		{
			if (device == nullptr)
			{
				return;
			}

			if (port1 && addr < 0x30)
			{
				if (addr == 0x00)
				{
					// ADPCM-A control: bit 7 stops every channel, bits 0..5 start one each.
					AdpcmState& state = adpcmStates[device->id];
					const bool keyOffAll = (data & 0x80) != 0;
					for (int channel = 0; channel < 6; ++channel)
					{
						const bool on = ! keyOffAll && (data & (1u << channel)) != 0;
						if (on && ! state.activeA[channel])
						{
							triggerOneShot (trackFor (device, 6 + channel), time);
						}
						state.activeA[channel] = on;
					}
				}
				return;
			}

			if (! port1 && addr >= 0x10 && addr < 0x20)
			{
				if (addr == 0x10)
				{
					// Delta-T (ADPCM-B) control: bit 7 starts, bit 0 resets.
					AdpcmState& state = adpcmStates[device->id];
					const bool start = (data & 0x80) != 0;
					if (start && ! state.activeB)
					{
						triggerOneShot (trackFor (device, 12), time, 0.10);
						state.activeB = true;
					}
					else if ((data & 0x01) != 0)
					{
						state.activeB = false;
					}
				}
				return;
			}

			writeFm (device, port1, addr, data, time, true, 6);
		}

		void writeFm (const PLR_DEV_INFO* device, bool port1, UINT8 addr, UINT8 data,
					  double time, bool isYm2610, int channelCount)
		{
			if (device == nullptr)
			{
				return;
			}

			// The YM2612 DAC is written through port 0 registers 0x2A/0x2B. It is a sample
			// stream rather than an FM operator, so it has its own marker track.
			if (device->type == DEVID_YM2612 && ! port1 && (addr == 0x2A || addr == 0x2B))
			{
				writeDac (device, addr, data, time);
				return;
			}

			FmState& state = fmStates[device->id];
			const bool isB = device->devCfg != nullptr && (device->devCfg->flags & 0x01) != 0;
			const bool fourChannel = isYm2610 && ! isB;

			// Key on/off lives in port 0 only; port 1 register 0x28 is not an OPN key register.
			if (addr == 0x28 && ! port1)
			{
				int channel = data & 0x03;
				if (channel == 3)
				{
					return;
				}
				if ((data & 0x04) != 0)
				{
					channel += 3;
				}
				if (channel >= channelCount || (fourChannel && (channel == 0 || channel == 3)))
				{
					return;
				}

				FmChannel& voice = state.channels[channel];
				const bool keyOn = (data & 0xF0) != 0;
				const int track = trackFor (device, channel);
				if (! keyOn)
				{
					voice.keyHeld = false;
					if (voice.active)
					{
						noteOff (track, time, voice.note);
						voice.active = false;
					}
					return;
				}

				voice.keyHeld = true;
				const double pitch = fmPitch (device, voice);
				updateMelodicPitch (track, time, voice.note, voice.active, pitch, 100);
				return;
			}

			// 0xA0-0xA6 (and port 1's 0xA0-0xA6) hold FNUM/BLOCK; 0xA8-0xAE are the
			// 3-slot/CSM registers of a single-port OPN and carry no channel pitch.
			if ((addr & 0xF0) != 0xA0 || (addr & 0x08) != 0)
			{
				return;
			}

			const int channel = (addr & 0x03) + (port1 ? 3 : 0);
			if (channel >= channelCount || (fourChannel && (channel == 0 || channel == 3)))
			{
				return;
			}

			FmChannel& voice = state.channels[channel];
			if ((addr & 0x04) == 0)
			{
				voice.fnum = (voice.fnum & 0x700) | data;
			}
			else
			{
				voice.fnum = (voice.fnum & 0x0FF) | ((data & 0x07) << 8);
				voice.block = (data >> 3) & 0x07;
			}

			if (! voice.keyHeld)
			{
				return;
			}

			const int track = trackFor (device, channel);
			const double pitch = fmPitch (device, voice);
			updateMelodicPitch (track, time, voice.note, voice.active, pitch, 100);
		}

		static double fmPitch (const PLR_DEV_INFO* device, const FmChannel& voice)
		{
			if (device == nullptr || device->devCfg == nullptr || voice.fnum <= 0
				|| voice.block < 0 || voice.block > 7)
			{
				return -1.0;
			}

			// OPN FNUM/BLOCK to frequency: the hardware relation is
			// FNUM = 144 * f * 2^20 / (clock * 2^(block-1)), so
			// f = FNUM * clock / (144 * 2^(21-block)).  144 is the FM sample-clock divisor
			// shared by the OPN family; the exact value is calibrated by the VGM's chip clock.
			const double clock = (double) device->devCfg->clock;
			const double frequency = clock / 144.0 * (double) voice.fnum
									 / (double) (1u << (21 - voice.block));
			return noteFromFrequencyF (frequency);
		}

		/** YM2151 pitch: KC holds block (bits 6..4) and note (bits 3..0), KF adds 1/64 of a
			semitone.  The note codes 3/7/11/15 are unused and repeat the previous semitone, so
			code - (code >> 2) is the semitone step inside the octave.  Code 0 is C#, which makes
			KC 0x4A (block 4) A4 = 440 Hz - the pitch the codes were laid out for at 3.579545 MHz. */
		static double opmPitch (const PLR_DEV_INFO* device, const OpmChannel& voice)
		{
			if (! voice.havePitch || device == nullptr || device->devCfg == nullptr)
			{
				return -1.0;
			}

			const int block = (voice.kc >> 4) & 0x07;
			const int noteCode = voice.kc & 0x0F;
			double semitone = 13.0 + 12.0 * block + (double) (noteCode - (noteCode >> 2))
							  + (double) voice.kf / 64.0;

			// The output frequency follows the chip clock, so an overclocked OPM plays sharp.
			const double clock = (double) device->devCfg->clock;
			if (clock > 0.0)
			{
				semitone += 12.0 * std::log2 (clock / 3579545.0);
			}

			return semitone;
		}

		void writeOpm (const PLR_DEV_INFO* device, UINT8 addr, UINT8 data, double time)
		{
			if (device == nullptr)
			{
				return;
			}

			OpmState& state = opmStates[device->id];
			if (addr == 0x08)
			{
				// Bits 2..0 select the channel, bits 6..3 are the operator key mask
				// (M1 = 0x08, C1 = 0x10, M2 = 0x20, C2 = 0x40), so 0x78 is "all four".
				const int channel = data & 0x07;
				OpmChannel& voice = state.channels[channel];
				const bool keyOn = (data & 0x78) != 0;
				const int track = trackFor (device, channel);
				if (! keyOn)
				{
					voice.keyHeld = false;
					if (voice.active)
					{
						noteOff (track, time, voice.note);
						voice.active = false;
					}
					return;
				}

				voice.keyHeld = true;
				updateMelodicPitch (track, time, voice.note, voice.active,
									opmPitch (device, voice), 100);
				return;
			}

			if (addr >= 0x28 && addr < 0x30)
			{
				OpmChannel& voice = state.channels[addr & 0x07];
				voice.kc = data & 0x7F;
				voice.havePitch = true;
				if (voice.keyHeld)
				{
					updateMelodicPitch (trackFor (device, addr & 0x07), time, voice.note,
										voice.active, opmPitch (device, voice), 100);
				}
				return;
			}

			if (addr >= 0x30 && addr < 0x38)
			{
				OpmChannel& voice = state.channels[addr & 0x07];
				voice.kf = (data >> 2) & 0x3F;
				if (voice.keyHeld)
				{
					updateMelodicPitch (trackFor (device, addr & 0x07), time, voice.note,
										voice.active, opmPitch (device, voice), 100);
				}
			}
		}

		/** OPL/OPLL/OPL3 pitch: FNUM * (clock / divisor) * 2^(block - reference).
			The OPL3 divisor is four times the OPL2 one because its clock is four times higher. */
		static double oplPitch (const PLR_DEV_INFO* device, const OplChannel& voice,
								OplKind kind)
		{
			if (device == nullptr || device->devCfg == nullptr || voice.fnum <= 0)
			{
				return -1.0;
			}

			const double clock = (double) device->devCfg->clock;
			if (! (clock > 0.0))
			{
				return -1.0;
			}

			const double divisor = kind == OplKind::opl3 ? 288.0 : 72.0;
			const int reference = kind == OplKind::opll ? 19 : 20;
			const double frequency = (double) voice.fnum * (clock / divisor)
									 * std::pow (2.0, (double) (voice.block - reference));
			return noteFromFrequencyF (frequency);
		}

		void applyOplKey (const PLR_DEV_INFO* device, OplChannel& voice, int channel,
						  bool keyOn, double time, OplKind kind)
		{
			const int track = trackFor (device, channel);
			if (! keyOn)
			{
				voice.keyHeld = false;
				if (voice.active)
				{
					noteOff (track, time, voice.note);
					voice.active = false;
				}
				return;
			}

			voice.keyHeld = true;
			updateMelodicPitch (track, time, voice.note, voice.active,
								oplPitch (device, voice, kind), 100);
		}

		/** Rhythm instruments are level-triggered; only the rising edge is a hit. */
		void applyOplRhythm (const PLR_DEV_INFO* device, OplState& state, UINT8 data,
							 bool enabled, int firstTrack, int bank, OplKind kind, double time)
		{
			if (! enabled)
			{
				state.rhythmBits = 0;
				state.rhythmEnabled = false;
				return;
			}

			if (! state.rhythmEnabled)
			{
				// Rhythm mode borrows channels 6..8 of the first bank, so a melodic note still
				// held there is dropped by the chip - and its key-ons stop being melodic.
				state.rhythmEnabled = true;
				state.rhythmBits = 0;
				for (int channel = 6; channel < 9; ++channel)
				{
					applyOplKey (device, state.channels[bank + channel], bank + channel,
								 false, time, kind);
				}
			}

			const UINT8 bits = data & 0x1F;
			const UINT8 rising = bits & (UINT8) ~state.rhythmBits;
			for (int i = 0; i < 5; ++i)
			{
				if ((rising & (0x10u >> i)) != 0)
				{
					triggerOneShot (trackFor (device, firstTrack + i), time);
				}
			}
			state.rhythmBits = bits;
		}

		void writeOpl (const PLR_DEV_INFO* device, bool port1, UINT8 addr, UINT8 data,
					   double time, OplKind kind)
		{
			if (device == nullptr)
			{
				return;
			}

			OplState& state = oplStates[device->id];
			const int bank = (kind == OplKind::opl3 && port1) ? 9 : 0;

			// The Y8950's Delta-T unit is the same idea as the YM2608 one: bit 7 of the
			// control register starts a sample, so only its rising edge is a hit.
			if (device->type == DEVID_Y8950 && addr == 0x07)
			{
				const bool start = (data & 0x80) != 0;
				if (start && ! state.adpcmHeld)
				{
					triggerOneShot (trackFor (device, 14), time, 0.12);
				}
				state.adpcmHeld = start;
				return;
			}

			if (kind == OplKind::opll)
			{
				if (addr >= 0x10 && addr < 0x19)
				{
					OplChannel& voice = state.channels[addr - 0x10];
					voice.fnum = (voice.fnum & 0x100) | data;
					if (voice.keyHeld)
					{
						updateMelodicPitch (trackFor (device, addr - 0x10), time, voice.note,
											voice.active, oplPitch (device, voice, kind), 100);
					}
					return;
				}

				if (addr >= 0x20 && addr < 0x29)
				{
					const int channel = addr - 0x20;
					OplChannel& voice = state.channels[channel];
					voice.fnum = (voice.fnum & 0x0FF) | ((data & 0x01) << 8);
					voice.block = (data >> 1) & 0x07;
					if (state.rhythmEnabled && channel >= 6)
					{
						return;		// borrowed by the rhythm instruments
					}
					applyOplKey (device, voice, channel, (data & 0x10) != 0, time, kind);
					return;
				}

				if (addr == 0x0E)
				{
					applyOplRhythm (device, state, data, (data & 0x20) != 0, 9, 0, kind, time);
				}
				return;
			}

			if (addr >= 0xA0 && addr < 0xA9)
			{
				const int channel = bank + (addr - 0xA0);
				OplChannel& voice = state.channels[channel];
				voice.fnum = (voice.fnum & 0x300) | data;
				if (voice.keyHeld)
				{
					updateMelodicPitch (trackFor (device, channel), time, voice.note,
										voice.active, oplPitch (device, voice, kind), 100);
				}
				return;
			}

			if (addr >= 0xB0 && addr < 0xB9)
			{
				const int channel = bank + (addr - 0xB0);
				OplChannel& voice = state.channels[channel];
				voice.fnum = (voice.fnum & 0x0FF) | ((data & 0x03) << 8);
				voice.block = (data >> 2) & 0x07;
				if (state.rhythmEnabled && bank == 0 && channel >= 6 && channel < 9)
				{
					return;		// borrowed by the rhythm instruments
				}
				applyOplKey (device, voice, channel, (data & 0x20) != 0, time, kind);
				return;
			}

			if (addr == 0xBD && ! port1)
			{
				applyOplRhythm (device, state, data, (data & 0x20) != 0,
								kind == OplKind::opl3 ? 18 : 9, 0, kind, time);
			}
		}

		/** YM2608: ADPCM-A on port 0 registers 0x10..0x1F, ADPCM-B on port 1 0x00..0x0F,
			everything else is the normal OPN FM register file. */
		void writeYm2608 (const PLR_DEV_INFO* device, bool port1, UINT8 addr, UINT8 data,
						  double time)
		{
			if (device == nullptr)
			{
				return;
			}

			AdpcmState& state = adpcmStates[device->id];
			if (! port1 && addr >= 0x10 && addr < 0x20)
			{
				if (addr == 0x10)
				{
					// Bit 7 is "key off all"; bits 0..5 start one channel each.
					const bool keyOffAll = (data & 0x80) != 0;
					for (int channel = 0; channel < 6; ++channel)
					{
						const bool on = ! keyOffAll && (data & (1u << channel)) != 0;
						if (on && ! state.activeA[channel])
						{
							triggerOneShot (trackFor (device, 6 + channel), time);
						}
						state.activeA[channel] = on;
					}
				}
				return;
			}

			if (port1 && addr < 0x10)
			{
				if (addr == 0x00)
				{
					const bool start = (data & 0x80) != 0;
					const bool reset = (data & 0x01) != 0;
					if (start && ! state.activeB)
					{
						triggerOneShot (trackFor (device, 12), time);
						state.activeB = true;
					}
					else if (reset)
					{
						state.activeB = false;
					}
				}
				return;
			}

			writeFm (device, port1, addr, data, time, false, 6);
		}

		/** The YM2612 DAC is written one sample byte at a time; a marker per byte would be
			thousands of notes, so only a drum-rate pulse is kept. */
		void writeDac (const PLR_DEV_INFO* device, UINT8 addr, UINT8 data, double time)
		{
			DacState& state = dacStates[device->id];
			if (addr == 0x2B)
			{
				state.enabled = (data & 0x80) != 0;
				return;
			}

			if (addr != 0x2A || ! state.enabled || data == 0x80)
			{
				return;
			}

			if (time - state.lastTrigger < 0.05)
			{
				return;
			}

			state.lastTrigger = time;
			triggerOneShot (trackFor (device, 6), time, 0.05);
		}

		const std::vector<PLR_DEV_INFO>& devices;
		const std::vector<playback::TrackInfo>& tracks;
		std::map<std::uint64_t, const PLR_DEV_INFO*> deviceByType;
		std::map<std::uint64_t, int> trackByDeviceChannel;

		// 2A03 writes, kept until the stream ends so the decoder can run its frame sequencer
		std::map<std::uint32_t, std::vector<chips::RegisterWrite>> nesWrites;
		std::map<std::uint32_t, const PLR_DEV_INFO*> nesDevices;
		std::map<std::uint32_t, SnState> snStates;
		std::map<std::uint32_t, AyState> ayStates;
		std::map<std::uint32_t, FmState> fmStates;
		std::map<std::uint32_t, OpmState> opmStates;
		std::map<std::uint32_t, OplState> oplStates;
		std::map<std::uint32_t, DacState> dacStates;
		std::map<std::uint32_t, AdpcmState> adpcmStates;
		std::map<std::uint32_t, GbState> gbStates;
		std::map<std::uint32_t, Rf5cState> rf5cStates;
		std::map<int, PendingOneShot> oneShotPending;
		std::vector<playback::MidiEvent> events;
	};
}

std::unique_ptr<playback::MidiSequence> buildMidiSequence (
	const void* data, std::size_t size, const std::vector<PLR_DEV_INFO>& devices,
	const std::vector<playback::TrackInfo>& tracks)
{
	if (data == nullptr || size < 0x40)
	{
		return nullptr;
	}

	Builder builder (devices, tracks);
	return builder.parse (static_cast<const UINT8*> (data), size);
}

} // namespace toybox::backends::libvgm
