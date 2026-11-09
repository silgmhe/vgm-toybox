#include "MidiSequenceBuilder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "ChipMemoryAssembly.h"
#include "ChipWriteRouter.h"
#include "VgmCommandReader.h"
#include "formats/vgm/VgmSongLoader.h"
#include "midi/NoteStream.h"
#include "midi/SongIr.h"

#include "chips/Ay8910Decoder.h"
#include "chips/C140Decoder.h"
#include "chips/C352Decoder.h"
#include "chips/GbcApuDecoder.h"
#include "chips/HuC6280Decoder.h"
#include "chips/K051649Decoder.h"
#include "chips/K053260Decoder.h"
#include "chips/K054539Decoder.h"
#include "chips/QSoundDecoder.h"
#include "chips/Es5503Decoder.h"
#include "chips/Ga20Decoder.h"
#include "chips/SegaPcmDecoder.h"
#include "chips/NesApuDecoder.h"
#include "chips/NoteExtractor.h"
#include "chips/OplDecoder.h"
#include "chips/OpmDecoder.h"
#include "chips/OpnDecoder.h"
#include "chips/PokeyDecoder.h"
#include "chips/K007232Decoder.h"
#include "chips/VsuDecoder.h"
#include "chips/K005289Decoder.h"
#include "chips/Msm5232Decoder.h"
#include "chips/MultiPcmDecoder.h"
#include "chips/MikeyDecoder.h"
#include "chips/Okim6295Decoder.h"
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

	uint64_t deviceChannelKey (playback::DeviceId device, int channel)
	{
		return ((uint64_t) device << 8) | (uint64_t) (channel & 0xFF);
	}

	class Builder
	{
	public:
		Builder (const std::vector<PLR_DEV_INFO>& deviceList,
				 const std::vector<playback::TrackInfo>& trackList, bool transposeBySampleRoot)
			: router (deviceList), tracks (trackList), useSampleRoot (transposeBySampleRoot)
		{
			// The three chips that keep a buffer of their own keep it here rather than in the
			// router: the reason they are separate is that their notes need the whole song before
			// they mean anything, and that is this layer's concern. The router only says which
			// writes cannot go through the registry.
			router.onNesWrite = [this] (const PLR_DEV_INFO* device, uint8_t reg, uint8_t value, uint32_t time)
			{
				if (device == nullptr)
				{
					return;
				}

				chips::RegisterWrite write;
				write.deviceId = device->id;
				write.reg = reg;
				write.value = value;
				write.sampleTime = time;
				nesWrites[device->id].push_back (write);
				nesDevices[device->id] = device;
			};

			router.onGbWrite = [this] (const PLR_DEV_INFO* device, uint8_t reg, uint8_t value, uint32_t time)
			{
				if (device == nullptr)
				{
					return;
				}

				chips::RegisterWrite write;
				write.deviceId = device->id;
				write.reg = reg;
				write.value = value;
				write.sampleTime = time;
				gbWrites[device->id].push_back (write);
				gbDevices[device->id] = device;
			};

			router.onSsgWrite = [this] (const PLR_DEV_INFO* device, uint8_t reg, uint8_t value, uint32_t time)
			{
				ssgWrites.queue (device, reg, value, time, 0);
			};

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
			// The stream is read by the format's loader; everything below is assembly. This function
			// no longer knows a single VGM command number, which is what lets a second format reuse it.
			midi::SongIr ir;
			std::string error;
			formats::vgm::VgmSongLoader loader;
			if (! loader.load (data, size, ir, error))
			{
				return nullptr;
			}

			// the song declares its own time base; everything below converts once, here
			tickRate = ir.tickRate > 0.0 ? ir.tickRate : 44100.0;

			for (const auto& write : ir.writes)
			{
				dispatch (ir, write);
			}

			// A -> B, then B -> MIDI. Two stages, one direction: the encoder below cannot see a
			// register write, and this stage cannot see a MIDI event.
			return encode (inferNotes (ir));
		}

		/** A -> B: the register writes become notes.

			This is the only place where a note is *guessed* - every decoder's pitch model, every
			measured sample, every one-shot decision ends up here as a `NoteEvent`. A module loader
			that knows its notes outright produces the same `NoteStream` without calling this. */
		midi::NoteStream inferNotes (const midi::SongIr& ir)
		{
			stream = midi::NoteStream {};
			stream.tickRate = tickRate;

			const uint32_t lastSampleTime = ir.totalTicks;

			// Chips that need the finished stream before they can place a note. The order matters: at
			// equal timestamps the stable sort keeps whichever was appended first, so the flush order
			// is part of the output.
			flushNes (lastSampleTime);
			// ADPCM-B's pitch only exists in its DELTA-T ROM, which is why the OPN family is flushed
			// through its own entry point: it also splits ports and drums, and the block that carries
			// that ROM depends on the variant of each device.
			flushOpn (ir, lastSampleTime);
			flushSsg (ir, lastSampleTime);
			flushGb (lastSampleTime);
			flushOpl (lastSampleTime);

			flushDecoded (router.decoder<chips::OpmDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::HuC6280Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::K051649Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Saa1099Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::PokeyDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::WswanDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Sn76489Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Ay8910Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Rf5c68Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::K007232Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::VsuDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::K005289Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Msm5232Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::MultiPcmDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::MikeyDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Okim6295Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::X1_010Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::C140Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::C352Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::K053260Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::K054539Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::QSoundDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Ga20Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::SegaPcmDecoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Es5503Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Ymf271Decoder> (), ir, lastSampleTime);
			flushDecoded (router.decoder<chips::Ymz280bDecoder> (), ir, lastSampleTime);

			return std::move (stream);
		}

	private:
		/** Routes one IR write to the decoder that owns its kind.

			The chip's own protocol is applied here rather than in the loader: the Gb/Nes helpers split
			a write into the several the chip needs, the C140 falls back to the 219 ASIC when the running
			device is the cut-down one, and the two RF5C variants share a libvgm id. Those are facts
			about the devices this runtime started, which is exactly what this layer knows and the
			format layer does not. */

		void dispatch (const midi::SongIr& ir, const chips::RegisterWrite& w)
		{
			router.route (ir, w);
		}

		/** B -> MIDI. The track is already resolved on every note, so nothing here knows a device,
			a chip or a format - only notes, bends and the unit their times are in. */
		std::unique_ptr<playback::MidiSequence> encode (const midi::NoteStream& in)
		{
			events.clear();
			// With the sample-root preference on, the note moves by whatever the sample's own root
			// says (design doc section 14); the cents left over ride on the bend that already
			// belongs to the note's own start, so vibrato after it is untouched.
			std::map<std::pair<int, uint32_t>, int> rootRemainder;

			for (const auto& entry : in.notes)
			{
				const auto& note = entry.note;
				const int track = entry.track;

				int midiNote = note.note;
				if (useSampleRoot && note.rootCents != 0)
				{
					const int total = note.note * 100 + note.rootCents;
					midiNote = (int) std::lround ((double) total / 100.0);
					rootRemainder[{ track, note.startSample }] = total - midiNote * 100;
				}

				midiNote = std::max (0, std::min (127, midiNote));

				const double start = (double) note.startSample / tickRate;
				const double end = (double) note.endSample / tickRate;
				noteOn (track, start, midiNote, note.velocity);

				// the report needs to know where the pitch came from; `data1` already carries it
				events.back().transposeCents = (int16_t) note.transposeCents;
				events.back().rootCents = (int16_t) note.rootCents;
				events.back().pitchSource = (uint8_t) note.pitchSource;

				noteOff (track, end, midiNote);
			}

			for (const auto& entry : in.bends)
			{
				const auto& bend = entry.bend;
				const int track = entry.track;

				int cents = bend.cents;
				const auto extra = rootRemainder.find ({ track, bend.sampleTime });
				if (extra != rootRemainder.end())
				{
					cents += extra->second;
				}

				pitchBend (track, (double) bend.sampleTime / tickRate, (double) cents / 100.0);
			}

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

		/** Collects one decoder's notes, with the MIDI track each chip channel maps to already
			resolved - the last thing that needs a device pointer before the encoder takes over. */
		void appendNotes (const PLR_DEV_INFO* device, const chips::ExtractedPerformance& performance,
						  int channelOffset = 0)
		{
			for (const auto& note : performance.notes)
			{
				const int track = trackFor (device, note.logicalChannel + channelOffset);
				if (track >= 0)
				{
					stream.notes.push_back ({ track, note });
				}
			}

			for (const auto& bend : performance.bends)
			{
				const int track = trackFor (device, bend.logicalChannel + channelOffset);
				if (track >= 0)
				{
					stream.bends.push_back ({ track, bend });
				}
			}
		}

		/** The SSG behind an OPN chip: its own buffer, its own clock divisor, and its notes shifted
			past the FM channels because that is where its tracks are. */
		void flushSsg (const midi::SongIr& ir, uint32_t totalSamples)
		{
			flushDecoded (ssgWrites, ir, totalSamples, DecoderScope::opnSsg);
		}

		void flushNes (uint32_t totalSamples)
		{
			for (auto& entry : nesWrites)
			{
				const auto device = nesDevices.find (entry.first);
				if (device == nesDevices.end() || device->second == nullptr)
				{
					continue;
				}

				const uint32_t clock = (device->second->devCfg != nullptr
											 && device->second->devCfg->clock > 0)
												? device->second->devCfg->clock
												: 1789773u;

				chips::NesApuDecoder decoder;
				decoder.reset (chips::ChipConfig { clock, tickRate });

				appendNotes (device->second,
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
		void flushOpn (const midi::SongIr& ir, uint32_t totalSamples)
		{
			for (auto& entry : router.decoder<chips::OpnDecoder> ().writes)
			{
				const auto device = router.decoder<chips::OpnDecoder> ().devices.find (entry.first);
				if (device == router.decoder<chips::OpnDecoder> ().devices.end() || device->second == nullptr)
				{
					continue;
				}

				const PLR_DEV_INFO& info = *device->second;
				uint32_t fallbackClock = 7670453u;
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

				const uint32_t clock = (info.devCfg != nullptr && info.devCfg->clock > 0)
												? info.devCfg->clock
												: fallbackClock;

				const chips::OpnDecoder::Variant variant = opnVariantFor (info);
				chips::OpnDecoder decoder (variant);
				decoder.reset (chips::ChipConfig { clock, tickRate });

				// **Delta-T's ROM, chosen per device.** The OPNB keeps it in block 0x83; the OPNA in
				// 0x81 and often ships only the ADPCM-A block, which its two families share. This used
				// to be one image built from 0x82 for the whole family, so on an OPNB the sample's
				// cycle - and with it the root correction - was measured from the wrong bytes.
				const auto blocks = chips::OpnDecoder::sampleBlocksFor (variant);
				const uint8_t types[2] = { blocks.deltaT, blocks.deltaTFallback };

				for (const uint8_t type : types)
				{
					if (type == 0)
					{
						continue;
					}

					const std::vector<uint8_t> rom =
						memory::rom (ir, type, chips::OpnDecoder::kAdpcmBImageBytes, true);

					if (! rom.empty())
					{
						chips::ChipMemory attached;
						attached.bytes = &rom;
						attached.type = type;
						decoder.attachSecondaryMemory (attached);
						break;
					}
				}

				appendNotes (&info,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			router.decoder<chips::OpnDecoder> ().writes.clear();
			router.decoder<chips::OpnDecoder> ().devices.clear();
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
		void flushOpl (uint32_t totalSamples)
		{
			for (auto& entry : router.decoder<chips::OplDecoder> ().writes)
			{
				const auto device = router.decoder<chips::OplDecoder> ().devices.find (entry.first);
				if (device == router.decoder<chips::OplDecoder> ().devices.end() || device->second == nullptr)
				{
					continue;
				}

				const PLR_DEV_INFO& info = *device->second;
				bool adpcm = false;
				const chips::OplDecoder::Variant variant = oplVariantFor (info, adpcm);

				const uint32_t fallbackClock = variant == chips::OplDecoder::Variant::opl3
														? 14318180u
														: 3579545u;
				uint32_t clock = (info.devCfg != nullptr && info.devCfg->clock > 0)
										  ? info.devCfg->clock
										  : fallbackClock;

				// the OPL4 clocks its linked OPL3 at 8/19 of its own clock (libvgm's
				// init_opl3_devinfo does the same), so the pitch table needs that clock
				if (info.type == DEVID_YMF278B)
				{
					clock = clock * 8u / 19u;
				}

				chips::OplDecoder decoder (variant, adpcm);
				decoder.reset (chips::ChipConfig { clock, tickRate });

				appendNotes (&info,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			router.decoder<chips::OplDecoder> ().writes.clear();
			router.decoder<chips::OplDecoder> ().devices.clear();
		}

		/** What a decoder is reading: a chip that stands alone, or the voice set linked behind one.
			The latter runs at a fraction of the parent's clock and numbers its channels after it.
			(Not to be confused with LinkedDeviceKind, which is how a linked device is muted.) */
		enum class DecoderScope { standalone, opnSsg };
		/** Runs one decoder over its whole write stream and hands the notes to the stream.

			Everything chip-specific comes from the decoder: `kRequirements` says which clock to assume
			and which memory image to assemble, `attachMemory` receives it. This used to be one call per
			chip with those facts written out at the call site - twenty-five lines of "0x8D, 2 MB" in
			the layer that is meant to know nothing about chips. */
		template <typename Decoder>
		void flushDecoded (ChipBuffer<Decoder>& buffer, const midi::SongIr& ir,
						   uint32_t totalSamples, DecoderScope scope = DecoderScope::standalone)
		{
			constexpr auto requirements = Decoder::kRequirements;

			// The three block shapes and which one a chip wants live in memory::forRequirements, so
			// that the display's replay builds the same image this does.
			std::vector<uint8_t> image;
			chips::ChipMemory memory;

			if (! buffer.writes.empty())
			{
				uint8_t banking = 0;
				image = memory::forRequirements (ir, requirements, banking);

				if (! image.empty())
				{
					memory.bytes = &image;
					memory.banking = banking;
				}
			}

			for (auto& entry : buffer.writes)
			{
				const auto device = buffer.devices.find (entry.first);
				if (device == buffer.devices.end() || device->second == nullptr)
				{
					continue;
				}

				uint32_t clock = (device->second->devCfg != nullptr
									   && device->second->devCfg->clock > 0)
										  ? device->second->devCfg->clock
										  : requirements.fallbackClock;

				// The SSG inside an OPN chip is clocked from the master: the YM2608 and YM2610 hand
				// it half, the YM2203 the whole thing (libvgm's init_ssg_devinfo clock divisors).
				if (scope == DecoderScope::opnSsg && device->second->type != DEVID_YM2203)
				{
					clock /= 2;
				}

				Decoder decoder;
				decoder.reset (chips::ChipConfig { clock, tickRate });
				if (requirements.hasMemory)
				{
					decoder.attachMemory (memory);
				}

				const int channelOffset = (scope == DecoderScope::opnSsg
											  && device->second->devDecl != nullptr
											  && device->second->devDecl->channelCount != nullptr)
											 ? (int) device->second->devDecl->channelCount (device->second->devCfg)
											 : 0;

				appendNotes (device->second,
							 chips::extractPerformance (decoder, entry.second, totalSamples),
							 channelOffset);
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
		/** The C140's ROM banking travels in the slot's flags, the way the header byte did before. */
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
			event.data1 = (uint8_t) note;
			event.data2 = (uint8_t) juce::jlimit (1, 127, velocity);
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
			event.data1 = (uint8_t) note;
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
			event.bendCents = (int16_t) std::lround (
				juce::jlimit (-(double) playback::maxPitchBendRange, (double) playback::maxPitchBendRange,
							  semitones) * 100.0);
			event.sourceTrack = tracks[(std::size_t) track].id;
			events.push_back (event);
		}

		void flushGb (uint32_t totalSamples)
		{
			for (auto& entry : gbWrites)
			{
				const auto device = gbDevices.find (entry.first);
				if (device == gbDevices.end() || device->second == nullptr)
				{
					continue;
				}

				const uint32_t clock = (device->second->devCfg != nullptr
											 && device->second->devCfg->clock > 0)
												? device->second->devCfg->clock
												: 4194304u;

				chips::GbcApuDecoder decoder;
				decoder.reset (chips::ChipConfig { clock, tickRate });

				appendNotes (device->second,
								  chips::extractPerformance (decoder, entry.second, totalSamples));
			}

			gbWrites.clear();
			gbDevices.clear();
		}

		/** YM2608: ADPCM-A on port 0 registers 0x10..0x1F, ADPCM-B on port 1 0x00..0x0F,
			everything else is the normal OPN FM register file. */
		/** Where each write goes. Shared with the visualisation's replay: the mapping from a chip
			kind to a decoder and a device is knowledge, not a preference, and two copies of it would
			drift into "one row looks merely wrong". */
		ChipWriteRouter router;

		const std::vector<playback::TrackInfo>& tracks;
		std::map<uint64_t, int> trackByDeviceChannel;

		// 2A03 writes, kept until the stream ends so the decoder can run its frame sequencer
		std::map<uint32_t, std::vector<chips::RegisterWrite>> nesWrites;
		std::map<uint32_t, const PLR_DEV_INFO*> nesDevices;
		// DMG writes: same reason as the 2A03, its frame sequencer owns the note ends
		std::map<uint32_t, std::vector<chips::RegisterWrite>> gbWrites;
		std::map<uint32_t, const PLR_DEV_INFO*> gbDevices;
		// the OPN family: algorithm and total level decide whether a key-on is a note, and the
		// ADPCM/DAC channels are one-shots the chip itself ends, so the whole stream is needed
		// the PSGs whose registers describe the whole voice: a write only matters in context

		// The OKIM6295 plays every voice at one fixed rate, so its note comes from the sample
		// alone - the ROM (0x8B) is what the decoder measures

		// Konami K005289: two wavetable voices whose pitch is clock / (32 * (freq + 1)) - no pitch
		// register to transpose and no ROM to measure, so nothing has to be handed to it

		// OKI MSM5232: eight voices whose register is a key index into an 88-entry ROM (note = 21 + index)

		// Yamaha YMW-258: 28 sampled voices, ratio = (1024 + pitch) / 1024 * 2^octave

		// Atari Lynx MIKEY: four timer-driven squares, clock / (2 * (backup + 1) * 2^(shift + 4))

		// Konami K007232: two PCM voices whose step register is a real pitch, clock / (4 * (4096 - step))

		// Virtual Boy VSU: five wavetable voices plus noise, clock / (32 * (2048 - freq))
		// the C140's note length comes from its end address, but only the whole stream says when
		// a voice was keyed on, so its writes wait for the end as well
		// the C352 is the same reader with an explicit key-on write, and its notes are measured
		// from the ROM its 0x92 block carries
		// the K053260 keys from a shared bitmask register and takes a note's length from its own
		// length register, so it too needs the whole stream before it can place a single note
		// the K054539 ends a note on a marker inside the sample, so its length - and its pitch -
		// come from the ROM its 0x8C block carries
		// QSound has no key bit at all: a voice sounds while its rate and volume are non-zero, so
		// its notes - and its sixteen voices - only make sense read from the whole stream too

		// The GA20 has a key bit but no length register the core reads: a sample ends where its
		// ROM says 0x00, so the decoder needs the whole stream - and the ROM - to know the gate

		// The Sega PCM's registers are its playback state - address, pitch, end, volume and the
		// control bit that starts it - so its notes only exist in the whole stream

		// The ES5503 is a wavetable chip: a voice walks a table it points at, so the note depends on
		// the table's own cycle length (measured from the RAM its 0xE1 blocks upload) as well as on
		// the frequency register - and its key is the falling edge of a halt bit


	/** The SSG behind an OPN chip: same decoder as a standalone AY, different clock and numbering. */
	ChipBuffer<chips::Ay8910Decoder> ssgWrites;

		/** The unit of RegisterWrite::sampleTime and of every note time below, as the
			loader declared it. VGM says 44100; a CPU-rate format would say 1.79 MHz and
			the conversion happens here, once. */
		double tickRate = 44100.0;

		/** A -> B's output; `encode` is the only reader. */
		midi::NoteStream stream;

		std::vector<playback::MidiEvent> events;

		/** The user's preference at load time: whether a sampled note also carries its sample's
			own root (design doc section 14). A preference rather than a live control, because it
			changes the note numbers themselves - switching it reloads the song. */
		bool useSampleRoot = false;
	};
}

std::unique_ptr<playback::MidiSequence> buildMidiSequence (
	const void* data, std::size_t size, const std::vector<PLR_DEV_INFO>& devices,
	const std::vector<playback::TrackInfo>& tracks, bool transposeBySampleRoot)
{
	if (data == nullptr || size < 0x40)
	{
		return nullptr;
	}

	Builder builder (devices, tracks, transposeBySampleRoot);
	return builder.parse (static_cast<const uint8_t*> (data), size);
}

} // namespace toybox::backends::libvgm
