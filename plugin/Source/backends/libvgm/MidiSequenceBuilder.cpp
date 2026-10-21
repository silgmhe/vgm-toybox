#include "MidiSequenceBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "VgmCommandReader.h"

#include "chips/Ay8910Decoder.h"
#include "chips/C140Decoder.h"
#include "chips/C352Decoder.h"
#include "chips/GbcApuDecoder.h"
#include "chips/HuC6280Decoder.h"
#include "chips/K051649Decoder.h"
#include "chips/K053260Decoder.h"
#include "chips/K054539Decoder.h"
#include "chips/QSoundDecoder.h"
#include "chips/Ga20Decoder.h"
#include "chips/SegaPcmDecoder.h"
#include "chips/NesApuDecoder.h"
#include "chips/NoteExtractor.h"
#include "chips/OplDecoder.h"
#include "chips/OpmDecoder.h"
#include "chips/OpnDecoder.h"
#include "chips/PokeyDecoder.h"
#include "chips/Rf5c68Decoder.h"
#include "chips/X1_010Decoder.h"
#include "chips/Ymf271Decoder.h"
#include "chips/Ymz280bDecoder.h"
#include "chips/Saa1099Decoder.h"
#include "chips/Sn76489Decoder.h"
#include "chips/WswanDecoder.h"

#include "emu/SoundDevs.h"

namespace toybox::backends::libvgm
{
namespace
{
	constexpr double kVgmRate = 44100.0;

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
					sn.queue (deviceFor (DEVID_SN76496, command.port), command.addr,
							  command.data, command.sampleTime);
					break;
				case 0x51:
					opl.queue (deviceFor (DEVID_YM2413, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0x52:
				case 0x53:
					opn.queue (deviceFor (DEVID_YM2612, 0), command.addr, command.data,
							   command.sampleTime, command.command == 0x53 ? 1 : 0);
					break;
				case 0x54:
					opm.queue (deviceFor (DEVID_YM2151, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0x55:
					opn.queue (deviceFor (DEVID_YM2203, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0x56:
				case 0x57:
					opn.queue (deviceFor (DEVID_YM2608, 0), command.addr, command.data,
							   command.sampleTime, command.command == 0x57 ? 1 : 0);
					break;
				case 0x58:
				case 0x59:
					opn.queue (deviceFor (DEVID_YM2610, 0), command.addr, command.data,
							   command.sampleTime, command.command == 0x59 ? 1 : 0);
					break;
				case 0x5A:
					opl.queue (deviceFor (DEVID_YM3812, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0x5D:
					ymz.queue (deviceFor (DEVID_YMZ280B, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xC8:
					// X1-010: the 16-bit offset arrived split across addr (low) and port (high).
					x1.queue (deviceFor (DEVID_X1_010, 0), command.addr, command.data,
							  command.sampleTime, command.port);
					break;
				case 0xD4:
					// C140: bit 7 of the port byte is the chip instance, the rest is the high byte
					// of the register offset - a voice's registers are sixteen apart, so the note
					// registers only line up when both halves arrive.
					c140.queue (deviceFor (DEVID_C140, (command.port >> 7) & 1), command.addr,
								command.data, command.sampleTime,
								(std::uint8_t) (command.port & 0x7F));
					break;
				case 0xD1:
					// YMF271: bit 7 of the port byte is the chip instance, the rest is the bank the
					// write goes to - a bank-0 write is what copies a group's synced registers.
					opx.queue (deviceFor (DEVID_YMF271, (command.port >> 7) & 1), command.addr,
							   command.data, command.sampleTime,
							   (std::uint8_t) (command.port & 0x7F));
					break;
				case 0xE1:
					// C352: a 16-bit offset and a 16-bit value, so unlike every other chip the
					// register write is a whole word - the reader split it across `data` (high) and
					// `data2` (low), and bit 7 of the port byte picks the chip.
					c352.queue (deviceFor (DEVID_C352, (command.port >> 7) & 1), command.addr,
								command.data, command.sampleTime,
								(std::uint8_t) (command.port & 0x7F), command.data2);
					break;



				case 0x5B:
					opl.queue (deviceFor (DEVID_YM3526, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0x5C:
					opl.queue (deviceFor (DEVID_Y8950, 0), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0x5E:
				case 0x5F:
					opl.queue (deviceFor (DEVID_YMF262, 0), command.addr, command.data,
							   command.sampleTime, command.command == 0x5F ? 1 : 0);
					break;
				case 0xA0:
					ay.queue (deviceFor (DEVID_AY8910, command.port), command.addr, command.data,
							  command.sampleTime);
					break;
				case 0xA1:
					opl.queue (deviceFor (DEVID_YM2413, 1), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xA4:
					opm.queue (deviceFor (DEVID_YM2151, 1), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xA5:
					opn.queue (deviceFor (DEVID_YM2203, 1), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xA6:
				case 0xA7:
					opn.queue (deviceFor (DEVID_YM2608, 1), command.addr, command.data,
							   command.sampleTime, command.command == 0xA7 ? 1 : 0);
					break;
				case 0xAA:
					opl.queue (deviceFor (DEVID_YM3812, 1), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xAB:
					opl.queue (deviceFor (DEVID_YM3526, 1), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xAC:
					opl.queue (deviceFor (DEVID_Y8950, 1), command.addr, command.data,
							   command.sampleTime);
					break;
				case 0xAE:
				case 0xAF:
					opl.queue (deviceFor (DEVID_YMF262, 1), command.addr, command.data,
							   command.sampleTime, command.command == 0xAF ? 1 : 0);
					break;
				case 0xB0:	// RF5C68
					rf5c.queue (rf5cDeviceFor (false, command.port), command.addr, command.data,
								command.sampleTime);
					break;
				case 0xB1:	// RF5C164
					rf5c.queue (rf5cDeviceFor (true, command.port), command.addr, command.data,
								command.sampleTime);
					break;
				case 0xB3:
					queueGbWrite (deviceFor (DEVID_GB_DMG, command.port),
								  command.addr, command.data, command.sampleTime);
					break;
				case 0xB4:
					queueNesWrite (deviceFor (DEVID_NES_APU, command.port),
								   command.addr, command.data, command.sampleTime);
					break;
				case 0xB9:	// HuC6280
					c6280.queue (deviceFor (DEVID_C6280, command.port), command.addr,
								 command.data, command.sampleTime);
					break;
				case 0xBB:	// POKEY
					pokey.queue (deviceFor (DEVID_POKEY, command.port), command.addr,
								 command.data, command.sampleTime);
					break;
				case 0xBC:	// WonderSwan
					wswan.queue (deviceFor (DEVID_WSWAN, command.port), command.addr,
								 command.data, command.sampleTime);
					break;
				case 0xBD:	// SAA1099
					saa.queue (deviceFor (DEVID_SAA1099, command.port), command.addr,
							   command.data, command.sampleTime);
					break;
				case 0xBA:	// K053260: one chip per instance, a 6-bit offset and one data byte
					k053260.queue (deviceFor (DEVID_K053260, 0), command.addr, command.data,
								   command.sampleTime);
					break;
				case 0xBF:	// GA20: four voices, offset = channel * 8 + register
					ga20.queue (deviceFor (DEVID_GA20, 0), command.addr, command.data,
								command.sampleTime);
					break;
				case 0xC0:	// Sega PCM: a byte of the register RAM at a 16-bit offset
					segaPcm.queue (deviceFor (DEVID_SEGAPCM, 0), command.addr, command.data,
								   command.sampleTime, command.port);
					break;
				case 0xD3:	// K054539: a 16-bit offset, so its high byte travels in `port`
					k054539.queue (deviceFor (DEVID_K054539, 0), command.addr, command.data,
								   command.sampleTime, command.port);
					break;
				case 0xC4:	// QSound: the value is 16 bits wide and comes before the offset
					qsound.queue (deviceFor (DEVID_QSOUND, 0), command.addr, command.data,
								  command.sampleTime, 0, command.data2);
					break;
				case 0xD0:	// YMF278B (OPL4): ports 0/1 are the OPL3 banks, ports 2..5 the
							// wavetable half, which has no decoder yet; bit 7 selects the chip
					if ((command.port & 0x7F) < 2)
					{
						opl.queue (deviceFor (DEVID_YMF278B, (command.port & 0x80) != 0 ? 1 : 0),
								   command.addr, command.data, command.sampleTime,
								   (std::uint8_t) (command.port & 0x7F));
					}
					break;
				case 0xD2:	// K051649 (SCC): one command, two chip writes
					if (command.kind == VgmCommand::Kind::registerWritePair)
					{
						scc.queuePair (deviceFor (DEVID_K051649, command.port), command.addr,
									   command.data, command.data2, command.sampleTime, command.port);
					}
					break;
				case 0xA2:
				case 0xA3:
					opn.queue (deviceFor (DEVID_YM2612, 1), command.addr, command.data,
							   command.sampleTime, command.command == 0xA3 ? 1 : 0);
					break;
				case 0xA8:
				case 0xA9:
					opn.queue (deviceFor (DEVID_YM2610, 1), command.addr, command.data,
							   command.sampleTime, command.command == 0xA9 ? 1 : 0);
					break;
				default:
					break;	// waits and stream end are handled by the reader
				}
			}

			// chips with a real state machine are decoded in one pass after the stream is read:
			// their note boundaries come from the chip's own counters, not from a register write
			flushNes (lastSampleTime);

			// ADPCM-B's pitch only exists in its DELTA-T ROM (block 0x82): the sample's period has
			// to be measured, and DELTA-N turns it into the note the driver asked for
			flushOpn (opn.writes.empty() ? std::vector<std::uint8_t>()
										 : assembleRom (reader, 0x82, 16 * 1024 * 1024, true),
					  lastSampleTime);
			flushGb (lastSampleTime);
			flushOpl (lastSampleTime);
			flushDecoded (opm, 3579545u, lastSampleTime);
			flushDecoded (c6280, 3579545u, lastSampleTime);
			flushDecoded (scc, 3579545u, lastSampleTime);
			flushDecoded (saa, 8000000u, lastSampleTime);
			flushDecoded (pokey, 1789772u, lastSampleTime);
			flushDecoded (wswan, 3072000u, lastSampleTime);
			flushDecoded (sn, 3579545u, lastSampleTime);
			flushDecoded (ay, 1789773u, lastSampleTime);
			flushDecoded (rf5c, 12500000u, lastSampleTime);
			flushDecoded (x1, 16000000u, lastSampleTime);

			// The C140's note is the playback rate divided by the sample's own cycle length, and
			// only the ROM holds that length: without it every voice is quantised against a fixed
			// 8 kHz root and a song recorded at another rate comes out at the wrong octave (this
			// is the bug that made "Submerged City" send 59/62 where the chip plays 32/56). The
			// ROM is the song's 0x8D blocks, and the banking that folds a voice's bank into a ROM
			// offset is the header's byte 0x96 - both are part of the same fact, so they travel
			// together and the decoder cannot read one without the other.
			const std::vector<std::uint8_t> c140Rom =
				c140.writes.empty() ? std::vector<std::uint8_t>()
									: assembleRom (reader, 0x8D, 2 * 1024 * 1024);
			const auto c140Banking = chips::C140Decoder::bankingFromHeaderByte (reader.c140Banking());
			flushDecoded (c140, 8000000u, lastSampleTime,
						  [&c140Rom, c140Banking] (chips::C140Decoder& decoder)
						  {
							  if (! c140Rom.empty())
							  {
								  decoder.setRom (c140Rom, c140Banking);
							  }
						  });

			// The C352 needs its ROM for the same reason, from its own block type (0x92), and its
			// sample address is `bank << 16 | start`, so the image is the whole ROM the song
			// declares - the chip masks it to a power of two rather than clipping at 2 MB.
			const std::vector<std::uint8_t> c352Rom =
				c352.writes.empty() ? std::vector<std::uint8_t>()
									: assembleRom (reader, 0x92, 32 * 1024 * 1024);
			flushDecoded (c352, 12288000u, lastSampleTime,
						  [&c352Rom] (chips::C352Decoder& decoder)
						  {
							  if (! c352Rom.empty())
							  {
								  decoder.setRom (c352Rom);
							  }
						  });

			// The K053260 needs its ROM for the same reason, from its own block type (0x8E). Its
			// start address is 21 bits with no bank register, so the image is the declared ROM and
			// the chip masks it to a power of two.
			const std::vector<std::uint8_t> k053260Rom =
				k053260.writes.empty() ? std::vector<std::uint8_t>()
									   : assembleRom (reader, 0x8E, 2 * 1024 * 1024);
			flushDecoded (k053260, 3579545u, lastSampleTime,
						  [&k053260Rom] (chips::K053260Decoder& decoder)
						  {
							  if (! k053260Rom.empty())
							  {
								  decoder.setRom (k053260Rom);
							  }
						  });
			// The K054539 needs its ROM to know where a sample ends - the marker byte is what
			// keys the voice off in the core - and to measure the cycle between the start and that
			// marker. Block type 0x8C, 24-bit addresses.
			const std::vector<std::uint8_t> k054539Rom =
				k054539.writes.empty() ? std::vector<std::uint8_t>()
									   : assembleRom (reader, 0x8C, 16 * 1024 * 1024);
			flushDecoded (k054539, 18432000u, lastSampleTime,
						  [&k054539Rom] (chips::K054539Decoder& decoder)
						  {
							  if (! k054539Rom.empty())
							  {
								  decoder.setRom (k054539Rom);
							  }
						  });

			// QSound's samples are its 0x8F blocks: 16-bit addresses inside a 64 KB bank, so the
			// image is the declared ROM and the chip masks it to a power of two.
			const std::vector<std::uint8_t> qsoundRom =
				qsound.writes.empty() ? std::vector<std::uint8_t>()
									  : assembleRom (reader, 0x8F, 8 * 1024 * 1024);
			flushDecoded (qsound, 60000000u, lastSampleTime,
						  [&qsoundRom] (chips::QSoundDecoder& decoder)
						  {
							  if (! qsoundRom.empty())
							  {
								  decoder.setRom (qsoundRom);
							  }
						  });

			// GA20's samples are its 0x93 blocks, addressed in 16-byte units inside 1 MB.
			const std::vector<std::uint8_t> ga20Rom =
				ga20.writes.empty() ? std::vector<std::uint8_t>()
									: assembleRom (reader, 0x93, 1024 * 1024);
			// the GA20's byte rate is `clock / 4 / (256 - rate)`: the decoder needs the clock the
			// device was started with, which `flushDecoded` takes from the device config (the
			// header's 0xE0); the constant is only the fallback when there is no device
			flushDecoded (ga20, 3579545u, lastSampleTime,
						  [&ga20Rom] (chips::Ga20Decoder& decoder)
						  {
							  if (! ga20Rom.empty())
							  {
								  decoder.setRom (ga20Rom);
							  }
						  });

			// Sega PCM's samples are its 0x80 blocks, indexed by the 24-bit position's byte part.
			const std::vector<std::uint8_t> segaPcmRom =
				segaPcm.writes.empty() ? std::vector<std::uint8_t>()
									   : assembleRom (reader, 0x80, 8 * 1024 * 1024);
			flushDecoded (segaPcm, 8000000u, lastSampleTime,
						  [&segaPcmRom] (chips::SegaPcmDecoder& decoder)
						  {
							  if (! segaPcmRom.empty())
							  {
								  decoder.setRom (segaPcmRom);
							  }
						  });

			flushDecoded (opx, 16934400u, lastSampleTime);
			flushDecoded (ymz, 16934400u, lastSampleTime);

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

		/** Emits the notes a decoder produced, through the track each chip channel maps to.

			The bends go in after the notes so that, at equal timestamps, the stable sort keeps
			every note-on ahead of the bend that belongs to it. */
		void emitDecodedNotes (const PLR_DEV_INFO* device,
							   const chips::ExtractedPerformance& performance)
		{
			for (const auto& note : performance.notes)
			{
				const int track = trackFor (device, note.logicalChannel);
				if (track < 0)
				{
					continue;
				}

				const double start = (double) note.startSample / kVgmRate;
				const double end = (double) note.endSample / kVgmRate;
				noteOn (track, start, note.note, note.velocity);
				noteOff (track, end, note.note);
			}

			for (const auto& bend : performance.bends)
			{
				const int track = trackFor (device, bend.logicalChannel);
				if (track < 0)
				{
					continue;
				}

				pitchBend (track, (double) bend.sampleTime / kVgmRate, (double) bend.cents / 100.0);
			}
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

				emitDecodedNotes (device->second,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			nesWrites.clear();
			nesDevices.clear();
		}

		/** Which OPN the device is: the register map differs, and so does the channel layout the
			notes are reported on. A YM2610 with config flag bit 0 set is the B variant, which has
			all six FM channels - the same bit order the chip capability table uses. */
		static chips::OpnDecoder::Variant opnVariantFor (const PLR_DEV_INFO& device)
		{
			switch (device.type)
			{
			case DEVID_YM2203:
				return chips::OpnDecoder::Variant::ym2203;

			case DEVID_YM2608:
				return chips::OpnDecoder::Variant::ym2608;

			case DEVID_YM2610:
				return (device.devCfg != nullptr && (device.devCfg->flags & 0x01) != 0)
						   ? chips::OpnDecoder::Variant::ym2610b
						   : chips::OpnDecoder::Variant::ym2610;

			case DEVID_YM2612:
			default:
				return chips::OpnDecoder::Variant::ym2612;
			}
		}

		/** Decodes every OPN write once the stream is read. One pass per device, because the
			decoder needs the whole register history: algorithm changes, a total level written
			before the key-on, ADPCM start bits and the DAC's 0x2A stream. */
		void flushOpn (const std::vector<std::uint8_t>& adpcmBRom, std::uint32_t totalSamples)
		{
			for (auto& entry : opn.writes)
			{
				const auto device = opn.devices.find (entry.first);
				if (device == opn.devices.end() || device->second == nullptr)
				{
					continue;
				}

				const PLR_DEV_INFO& info = *device->second;
				std::uint32_t fallbackClock = 7670453u;
				switch (info.type)
				{
				case DEVID_YM2203:
					fallbackClock = 3993600u;
					break;
				case DEVID_YM2608:
					fallbackClock = 7987200u;
					break;
				case DEVID_YM2610:
					fallbackClock = 8000000u;
					break;
				default:
					break;
				}

				const std::uint32_t clock = (info.devCfg != nullptr && info.devCfg->clock > 0)
												? info.devCfg->clock
												: fallbackClock;

				chips::OpnDecoder decoder (opnVariantFor (info));
				decoder.reset (clock, (double) kVgmRate);
				if (! adpcmBRom.empty())
				{
					decoder.setAdpcmBRom (adpcmBRom);
				}

				emitDecodedNotes (&info,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			opn.writes.clear();
			opn.devices.clear();
		}

		/** Which OPL the device is. Only the Y8950 carries Delta-T, an OPL2 with a sample
			player on its own channel. */
		static chips::OplDecoder::Variant oplVariantFor (const PLR_DEV_INFO& device, bool& adpcm)
		{
			adpcm = device.type == DEVID_Y8950;
			switch (device.type)
			{
			case DEVID_YM2413:
				return chips::OplDecoder::Variant::opll;

			case DEVID_YMF262:
			case DEVID_YMF278B:	// the OPL4's FM half is register-compatible with an OPL3
				return chips::OplDecoder::Variant::opl3;

			default:
				return chips::OplDecoder::Variant::opl2;
			}
		}

		/** One pass per OPL device: OPLL and OPL2 have different pitch registers, and the
			rhythm mode borrows channels as it is entered, so the whole stream is needed. */
		void flushOpl (std::uint32_t totalSamples)
		{
			for (auto& entry : opl.writes)
			{
				const auto device = opl.devices.find (entry.first);
				if (device == opl.devices.end() || device->second == nullptr)
				{
					continue;
				}

				const PLR_DEV_INFO& info = *device->second;
				bool adpcm = false;
				const chips::OplDecoder::Variant variant = oplVariantFor (info, adpcm);

				const std::uint32_t fallbackClock = variant == chips::OplDecoder::Variant::opl3
														? 14318180u
														: 3579545u;
				std::uint32_t clock = (info.devCfg != nullptr && info.devCfg->clock > 0)
										  ? info.devCfg->clock
										  : fallbackClock;

				// the OPL4 clocks its linked OPL3 at 8/19 of its own clock (libvgm's
				// init_opl3_devinfo does the same), so the pitch table needs that clock
				if (info.type == DEVID_YMF278B)
				{
					clock = clock * 8u / 19u;
				}

				chips::OplDecoder decoder (variant, adpcm);
				decoder.reset (clock, (double) kVgmRate);

				emitDecodedNotes (&info,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			opl.writes.clear();
			opl.devices.clear();
		}

		/** Register writes of one chip, kept per device until the stream ends.

			The PSG decoders below (HuC6280, SCC, SAA1099, POKEY, WonderSwan) all need the
			finished stream: which note is playing at a given moment follows from the chip's own
			state - a divider that was set earlier, a key mask that was switched on, a sweep that
			has been running - and not from the write that happens to arrive. */
		template <typename Decoder>
		struct ChipBuffer
		{
			void queue (const PLR_DEV_INFO* device, std::uint8_t reg, std::uint8_t value,
						std::uint32_t sampleTime, std::uint8_t port = 0, std::uint8_t value2 = 0)
			{
				if (device == nullptr)
				{
					return;
				}

				chips::RegisterWrite write;
				write.deviceId = device->id;
				write.reg = reg;
				write.value = value;
				write.port = port;
				write.value2 = value2;
				write.sampleTime = sampleTime;
				writes[device->id].push_back (write);
				devices[device->id] = device;
			}

			/** The SCC command carries a latch and the value that follows it: the chip sees two
				writes, and so does the decoder. */
			void queuePair (const PLR_DEV_INFO* device, std::uint8_t reg, std::uint8_t latch,
							std::uint8_t value, std::uint32_t sampleTime, std::uint8_t port)
			{
				queue (device, reg, latch, sampleTime, port);
				queue (device, (std::uint8_t) (reg | 1), value, sampleTime, port);
			}

			std::map<std::uint32_t, std::vector<chips::RegisterWrite>> writes;
			std::map<std::uint32_t, const PLR_DEV_INFO*> devices;
		};

		template <typename Decoder>
		void flushDecoded (ChipBuffer<Decoder>& buffer, std::uint32_t fallbackClock,
						   std::uint32_t totalSamples)
		{
			flushDecoded (buffer, fallbackClock, totalSamples, [] (Decoder&) {});
		}

		/** The same pass, with a hook that hands the decoder whatever the stream carried besides
			register writes - a sampled chip needs its ROM before it can say what pitch a sample is. */
		template <typename Decoder, typename Configure>
		void flushDecoded (ChipBuffer<Decoder>& buffer, std::uint32_t fallbackClock,
						   std::uint32_t totalSamples, Configure configure)
		{
			for (auto& entry : buffer.writes)
			{
				const auto device = buffer.devices.find (entry.first);
				if (device == buffer.devices.end() || device->second == nullptr)
				{
					continue;
				}

				// the clock decides the pitch of every note on the chip, so a missing one is not
				// something to guess per write: the header's value, or the chip's usual clock
				const std::uint32_t clock = (device->second->devCfg != nullptr
											 && device->second->devCfg->clock > 0)
												? device->second->devCfg->clock
												: fallbackClock;

				Decoder decoder;
				decoder.reset (clock, (double) kVgmRate);
				configure (decoder);

				emitDecodedNotes (device->second,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			buffer.writes.clear();
			buffer.devices.clear();
		}

		/** Assembles one ROM from the stream's blocks of `type` (0x8D is the C140's sample ROM,
			0x92 the C352's).

			A ROM block carries the size of the whole ROM and where its payload belongs in it, so
			the image is the union of the blocks at their own offsets - a song can split a megabyte
			over many blocks ("Submerged City" fills 0x100000 bytes from sixteen of them, with gaps
			the ripper could not be bothered to upload; those stay zero, which reads as silence and
			measures as "no period", so a voice on one of them keeps the root rate). The declared
			size comes from the file, so it is clipped to what the chip can address instead of
			trusted with an allocation.

			The ROM is what makes a sampled player's notes mean something: its playback rate is a
			register, but the sample's own cycle length - the number the rate has to be divided by -
			only exists in these bytes. See chips/C140Decoder.h and chips/C352Decoder.h for the
			measurement (shared through chips/Pitch.h) and the reason it cannot be skipped. */
		static std::vector<std::uint8_t> assembleRom (const VgmCommandReader& reader,
													  std::uint8_t type,
													  std::size_t maxRomBytes,
													  bool clipToContent = false)
		{
			// The declared size is what the chip can address; the content is what a mask is built
			// from. They differ on the Neo Geo: the header says 8 MB but only 2.8 MB of blocks are
			// present, so libvgm masks the ADPCM-B address with 4 MB and the driver's 0x6B9000 is
			// really 0x2B9000, inside its block. Clipping to the content reproduces that.
			std::size_t romSize = 0;
			for (const auto& block : reader.dataBlocks())
			{
				if (block.type != type)
				{
					continue;
				}

				const std::size_t end = (std::size_t) block.romOffset + block.size;
				romSize = std::max (romSize, clipToContent ? end : (std::size_t) block.romSize);
			}

			std::vector<std::uint8_t> rom;
			romSize = std::min (romSize, maxRomBytes);
			if (romSize == 0)
			{
				return rom;
			}

			rom.assign (romSize, 0);
			for (const auto& block : reader.dataBlocks())
			{
				if (block.type != type || block.romOffset >= rom.size())
				{
					continue;
				}

				const std::size_t count = std::min (block.size, rom.size() - block.romOffset);
				std::memcpy (rom.data() + block.romOffset, block.data, count);
			}

			return rom;
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

		/** Pitch bend, stored as cents rather than as MIDI bytes: the 14-bit scale depends on the
			receiving instrument's range, which is a preference the engine applies when it sends
			(playback/MidiSequence.h). Clamped to the widest range the setting offers, so a broken
			chip value cannot wrap around. */
		void pitchBend (int track, double time, double semitones)
		{
			if (track < 0 || track >= (int) tracks.size())
			{
				return;
			}

			playback::MidiEvent event;
			event.timeSeconds = time;
			event.type = playback::MidiEventType::pitchBend;
			event.bendCents = (std::int16_t) std::lround (
				juce::jlimit (-(double) playback::maxPitchBendRange, (double) playback::maxPitchBendRange,
							  semitones) * 100.0);
			event.sourceTrack = tracks[(std::size_t) track].id;
			events.push_back (event);
		}

		/** Buffers one DMG APU register write. A Game Boy note is not a register write either: the
			length counters, the envelopes and the sweep run off the chip's frame sequencer, which
			only the decoder models, so the stream is decoded once it has been read. */
		void queueGbWrite (const PLR_DEV_INFO* device, std::uint8_t reg, std::uint8_t value,
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
			gbWrites[device->id].push_back (write);
			gbDevices[device->id] = device;
		}

		void flushGb (std::uint32_t totalSamples)
		{
			for (auto& entry : gbWrites)
			{
				const auto device = gbDevices.find (entry.first);
				if (device == gbDevices.end() || device->second == nullptr)
				{
					continue;
				}

				const std::uint32_t clock = (device->second->devCfg != nullptr
											 && device->second->devCfg->clock > 0)
												? device->second->devCfg->clock
												: 4194304u;

				chips::GbcApuDecoder decoder;
				decoder.reset (clock, (double) kVgmRate);

				emitDecodedNotes (device->second,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			gbWrites.clear();
			gbDevices.clear();
		}

		/** YM2608: ADPCM-A on port 0 registers 0x10..0x1F, ADPCM-B on port 1 0x00..0x0F,
			everything else is the normal OPN FM register file. */
		const std::vector<PLR_DEV_INFO>& devices;
		const std::vector<playback::TrackInfo>& tracks;
		std::map<std::uint64_t, const PLR_DEV_INFO*> deviceByType;
		std::map<std::uint64_t, int> trackByDeviceChannel;

		// 2A03 writes, kept until the stream ends so the decoder can run its frame sequencer
		std::map<std::uint32_t, std::vector<chips::RegisterWrite>> nesWrites;
		std::map<std::uint32_t, const PLR_DEV_INFO*> nesDevices;
		// DMG writes: same reason as the 2A03, its frame sequencer owns the note ends
		std::map<std::uint32_t, std::vector<chips::RegisterWrite>> gbWrites;
		std::map<std::uint32_t, const PLR_DEV_INFO*> gbDevices;
		// the OPN family: algorithm and total level decide whether a key-on is a note, and the
		// ADPCM/DAC channels are one-shots the chip itself ends, so the whole stream is needed
		ChipBuffer<chips::OpnDecoder> opn;
		ChipBuffer<chips::OpmDecoder> opm;
		ChipBuffer<chips::OplDecoder> opl;
		// the PSGs whose registers describe the whole voice: a write only matters in context
		ChipBuffer<chips::HuC6280Decoder> c6280;
		ChipBuffer<chips::K051649Decoder> scc;
		ChipBuffer<chips::Saa1099Decoder> saa;
		ChipBuffer<chips::PokeyDecoder> pokey;
		ChipBuffer<chips::WswanDecoder> wswan;
		ChipBuffer<chips::Sn76489Decoder> sn;
		ChipBuffer<chips::Ay8910Decoder> ay;
		ChipBuffer<chips::Rf5c68Decoder> rf5c;
		ChipBuffer<chips::X1_010Decoder> x1;
		ChipBuffer<chips::Ymf271Decoder> opx;
		ChipBuffer<chips::Ymz280bDecoder> ymz;
		// the C140's note length comes from its end address, but only the whole stream says when
		// a voice was keyed on, so its writes wait for the end as well
		ChipBuffer<chips::C140Decoder> c140;
		// the C352 is the same reader with an explicit key-on write, and its notes are measured
		// from the ROM its 0x92 block carries
		ChipBuffer<chips::C352Decoder> c352;
		// the K053260 keys from a shared bitmask register and takes a note's length from its own
		// length register, so it too needs the whole stream before it can place a single note
		ChipBuffer<chips::K053260Decoder> k053260;
		// the K054539 ends a note on a marker inside the sample, so its length - and its pitch -
		// come from the ROM its 0x8C block carries
		ChipBuffer<chips::K054539Decoder> k054539;
		// QSound has no key bit at all: a voice sounds while its rate and volume are non-zero, so
		// its notes - and its sixteen voices - only make sense read from the whole stream too
		ChipBuffer<chips::QSoundDecoder> qsound;

		// The GA20 has a key bit but no length register the core reads: a sample ends where its
		// ROM says 0x00, so the decoder needs the whole stream - and the ROM - to know the gate
		ChipBuffer<chips::Ga20Decoder> ga20;

		// The Sega PCM's registers are its playback state - address, pitch, end, volume and the
		// control bit that starts it - so its notes only exist in the whole stream
		ChipBuffer<chips::SegaPcmDecoder> segaPcm;
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
