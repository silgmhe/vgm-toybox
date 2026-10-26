#include "LibvgmSongInstance.h"

#include "LibvgmMuteIsolation.h"

#include <map>

#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

#include "LibvgmVgz.h"
#include "MidiSequenceBuilder.h"
#include "settings/SettingsStore.h"

namespace toybox::backends::libvgm
{

namespace
{
	/** The emulation core is reported as a four character code - show it as text. */
	juce::String fccToString (uint32_t code)
	{
		const auto fcc = (const char*) &code;
		juce::String result;
		for (int i = 0; i < 4; i ++)
		{
			if (fcc[i] == 0)
			{
				break;
			}
			result += juce::String::charToString ((juce::juce_wchar) (unsigned char) fcc[i]);
		}
		return result.trim();
	}

	/** Linked devices are an implementation detail of the cores (they are configured through
		their parent's options), so they never become tracks of their own. */
	bool isTrackDevice (const PLR_DEV_INFO& info)
	{
		return info.parentIdx == (uint32_t) -1;
	}

	/** What the MIDI decoder will be able to do for one channel of a device. This is deliberately
		kept here, next to the device enumeration: the UI and the engine see only the capability. */
	playback::MidiCapability midiCapabilityFor (const PLR_DEV_INFO& device, int channelIndex)
	{
		const auto type = device.type;
		/** Whether this channel is one of the linked SSG's: they are numbered after the parent's
		channels, which is where the layout puts their tracks. */
	const auto isSsgChannel = [] (const PLR_DEV_INFO& parent, int channel)
	{
		const int first = channelCountOf (parent);
		return channel >= first && channel < first + kSsgChannels;
	};

	const auto fmChannel = [channelIndex] (int count)
		{
			return channelIndex >= 0 && channelIndex < count ? playback::MidiCapability::melodic
															 : playback::MidiCapability::unavailable;
		};

		switch (type)
		{
		case DEVID_YM2610:
			if (channelIndex < 0)
			{
				return playback::MidiCapability::unavailable;
			}
			if (channelIndex < 6)
			{
				// The non-B variant only emits CH[1], CH[2], CH[4], CH[5].
				const bool isB = device.devCfg != nullptr && (device.devCfg->flags & 0x01) != 0;
				return (isB || (channelIndex != 0 && channelIndex != 3))
						   ? playback::MidiCapability::melodic
						   : playback::MidiCapability::unavailable;
			}
			// 6-11 are ADPCM-A (a trigger with no pitch of its own), 12 is ADPCM-B, whose DELTA-N
			// is a pitch register - the same one the reference implementation turns into a note
			// (`form/KB/OPN/frmYM2610.cs` -> `searchYM2608Adpcm`), so it is melodic without needing
			// the sample to be measurable.
			if (isSsgChannel (device, channelIndex))
			{
				return playback::MidiCapability::melodic;
			}

			return (channelIndex == 12) ? playback::MidiCapability::melodic
										: (channelIndex < 12 ? playback::MidiCapability::oneShot
															 : playback::MidiCapability::unavailable);
		case DEVID_YM2608:
			if (channelIndex < 6)
			{
				return playback::MidiCapability::melodic;
			}
			// the same split as the YM2610: ADPCM-A triggers, ADPCM-B carries a pitch, and the SSG
			// follows the parent's channels
			if (isSsgChannel (device, channelIndex))
			{
				return playback::MidiCapability::melodic;
			}

			return (channelIndex == 12) ? playback::MidiCapability::melodic
										: (channelIndex < 12 ? playback::MidiCapability::oneShot
															 : playback::MidiCapability::unavailable);
		case DEVID_YM2612:
			return (channelIndex == 6) ? playback::MidiCapability::oneShot : fmChannel (6);
		case DEVID_YM2203:
			// its SSG is numbered after the three FM channels
			return isSsgChannel (device, channelIndex) ? playback::MidiCapability::melodic
													   : fmChannel (3);
		case DEVID_YM2151:
			return fmChannel (8);
		case DEVID_YMF271:
			// twelve groups, and a group is FM or PCM depending on the sync mode the song writes.
			// The mode is runtime state, so the strip keeps the default: the PCM groups then just
			// report their one-shot notes on a channel the UI calls melodic.
			return fmChannel (12);
		case DEVID_YM2413:
			return (channelIndex >= 0 && channelIndex < 9) ? playback::MidiCapability::melodic
														   : (channelIndex < 14
																  ? playback::MidiCapability::oneShot
																  : playback::MidiCapability::unavailable);
		case DEVID_YM3526:
		case DEVID_YM3812:
			return (channelIndex >= 0 && channelIndex < 9) ? playback::MidiCapability::melodic
														   : (channelIndex < 14
																  ? playback::MidiCapability::oneShot
																  : playback::MidiCapability::unavailable);
		case DEVID_Y8950:	// 9 melodic, 5 rhythm, and the Delta-T ADPCM channel on top
			return (channelIndex >= 0 && channelIndex < 9) ? playback::MidiCapability::melodic
														   : (channelIndex < 15
																  ? playback::MidiCapability::oneShot
																  : playback::MidiCapability::unavailable);
		case DEVID_YMF262:
		case DEVID_YMF278B:	// its FM half is an OPL3; the wavetable half is not decoded
			return (channelIndex >= 0 && channelIndex < 18) ? playback::MidiCapability::melodic
															: (channelIndex < 23
																   ? playback::MidiCapability::oneShot
																   : playback::MidiCapability::unavailable);
		case DEVID_AY8910:
			// Three melodic channels. The comment here used to say that nothing in this backend
			// decodes the SSG, which was true when it was written and stopped being true at cbaf588,
			// where Ay8910Decoder arrived - a decoder whose own header names this chip and its
			// clones, and which computes pitch from the twelve-bit tone divider. The capability was
			// never revisited, so the three channels were offered as unavailable while their notes
			// came out all along: ay8910.vgm plays A4, C5 and E5 and all three were discarded by the
			// mixer for want of a route.
			//
			// A linked SSG does not come through here: one behind an OPN chip is numbered after the
			// parent's channels and is caught by isSsgChannel() above. A standalone chip is a device
			// of its own, and its channels are 0..2 like any other three-channel part.
			return (channelIndex >= 0 && channelIndex < kSsgChannels)
					   ? playback::MidiCapability::melodic
					   : playback::MidiCapability::unavailable;
		case DEVID_SN76496:
			return (channelIndex >= 0 && channelIndex < 3) ? playback::MidiCapability::melodic
														   : (channelIndex == 3
																  ? playback::MidiCapability::oneShot
																  : playback::MidiCapability::unavailable);
		case DEVID_RF5C68:	// also RF5C164 / RF5C105
			// Eight PCM voices with a step register and the sample in the chip's own RAM, so the note
			// is the step against the chip's rate (one byte per sample) and its own octave comes from
			// the measured cycle - section 14 of the design doc. A voice whose sample has no period
			// still reports its C4 trigger, so the capability stays melodic.
			return fmChannel (8);
		case DEVID_VBOY_VSU:
			// Five wavetable voices (clock / (32 * (2048 - freq))) plus one noise voice, which reports
			// an unpitched trigger.
			return fmChannel (6);
		case DEVID_K007232:
			// Two PCM voices with a step register: clock / (4 * (4096 - step)) is exact, so both are
			// melodic without a root layer.
			return fmChannel (2);
		case DEVID_MIKEY:
			// Four timer-driven square waves: the reload value and the clock shift give the pitch
			// exactly, so all four channels are melodic.
			return fmChannel (4);
		case DEVID_YMW258:
			// Twenty-eight sampled voices. The register pair is a ratio against unity, so every channel is
			// melodic with the usual caveat that the absolute octave comes from the sample's own rate.
			return fmChannel (28);
		case DEVID_MSM5232:
			// Eight tone generators whose register is a key index into an 88-entry ROM: the note is the
			// index itself, so all eight are melodic (a value of 0xD8 or more is the noise mode).
			return fmChannel (8);
		case DEVID_K005289:
			// Two wavetable voices on a 32-step PROM: the pitch is clock / (32 * (freq + 1)), exact from
			// the register, so both channels are melodic.
			return fmChannel (2);
		case DEVID_C219:
			// Namco's 219 ASIC (NA-1/NA-2): a cut-down C140 with sixteen PCM voices rather than
			// twenty-four. Same register layout, so `chips/C140Decoder` reports its notes.
			return fmChannel (16);
		case DEVID_MSM6295:
			// Four ADPCM voices at one fixed DAC rate (`clock / 165`): there is no pitch register,
			// so a note can only come from the sample's own cycle - the ES5503's case, and likewise
			// melodic, with a sample that has no period reporting its C4 trigger.
			return fmChannel (4);
		// Three chips are deliberately *not* represented, and they are named here so that the
		// absence reads as a decision rather than an oversight (design doc section 13, 2026-10-22):
		//
		//   PWM (32X)  - a streaming DAC. What VGM logs as "register writes" is the audio itself
		//                (`0xB2` to the left/right/mono output registers); the cycle register sets
		//                the DAC's sample rate, not a pitch. There is no note event to report.
		//   OKIM6258   - the same shape: `0x01` is a data port fed byte by byte, `0x0C` only
		//                selects a sample-rate divider. No start address, no sample number.
		//   uPD7759    - a speech synthesiser. A start edge plus a sample number is a real event,
		//                but the chip has no pitch at all, so the best a MIDI note could say is
		//                "a C4 happened" - which turns speech into an instrument.
		//
		// A MIDI note needs a pitch; these three have none, so the honest output is nothing.
		case DEVID_32X_PWM:
		case DEVID_MSM6258:
		case DEVID_uPD7759:
			return playback::MidiCapability::unavailable;
		case DEVID_YMZ280B:
			// Eight PCM voices. Their pitch register is a playback rate, not a note, so the decoder
			// reports one-shot triggers; see chips/Ymz280bDecoder.h and M4 in the design doc.
			return (channelIndex >= 0 && channelIndex < 8) ? playback::MidiCapability::oneShot
														   : playback::MidiCapability::unavailable;
		case DEVID_C140:
			// Twenty-four sampled voices. Their frequency register is a playback rate rather than a
			// note, so the note comes from that rate and the ROM's root rate (chips/Pitch.h); the
			// end address gives the voice a length, which is what makes it melodic and not a
			// one-shot guess. A zero-volume voice reports nothing, the same way the chip is silent.
			return fmChannel (24);
		case DEVID_C352:
			// Thirty-two sampled voices, the C140's closest relative: the frequency register is a
			// playback rate, the note comes from that rate and the ROM sample's own cycle length,
			// and the length comes from the end address or from a key-off. Its key-on is a write
			// of its own (offset 0x202), which is why the decoder cannot report a note at the flag
			// write - but the promise is the same one the C140 makes: melodic, not a trigger.
			return fmChannel (32);
		case DEVID_QSOUND:
			// Sixteen sampled voices plus three ADPCM ones. There is no key bit: a voice sounds
			// while its rate and volume are non-zero and the driver keys a note by writing the rate
			// from zero, so the decoder keys on that edge and gates on the same condition. The
			// drums share these voices, and an unmeasurable sample is the C4 trigger.
			return fmChannel (19);
		case DEVID_K054539:
			// Eight sampled voices, 8-bit PCM / 16-bit PCM / 4-bit DPCM, with a 24-bit pitch and a
			// volume register that attenuates. A sample ends on a marker byte, so a non-looping
			// note has an exact length and no gate guess; the drums share these eight voices with
			// the music, and the decoder reports an unmeasurable sample as a C4 trigger.
			return fmChannel (8);
		case DEVID_K053260:
			// Four sampled voices, and the first chip whose note length is a register: a
			// non-looping voice lasts `length` bytes at its pitch, so a note needs no gate guess and
			// no end address. The drums on these boards share the four voices with the music; the
			// decoder marks an unmeasurable sample as a C4 trigger rather than inventing a pitch, so
			// the capability stays melodic - a voice can be either.
			return fmChannel (4);
		case DEVID_SEGAPCM:
			// Sixteen voices playing 8-bit PCM. The registers are the playback state - current
			// address, pitch, end address, volumes and one control bit - and the boards put drums
			// and melody on the same voices, so the decoder decides per key-on and a drum still
			// reports its C4 trigger.
			return fmChannel (16);
		case DEVID_GA20:
			// Four voices playing 8-bit PCM, a rate divider and a sample that ends on a 0x00 byte.
			// These boards (R-Type, Undercover Cops) put drums and melody on the same four voices,
			// and one voice can play both inside one song - the decoder decides per key-on from the
			// sample it latched, so the capability stays melodic and a drum still reports a note.
			return fmChannel (4);
		case DEVID_ES5503:
			// Thirty-two wavetable oscillators (the Ensoniq DOC). Each one walks a table it points
			// at, and its note is that table's repeat rate: the frequency register is a real pitch
			// once it is divided by the table's own cycle length, which the decoder measures from
			// the chip's RAM. A one-shot table with no period - a drum - still reports its C4
			// trigger, so the capability stays melodic; a looping table always has a pitch.
			return fmChannel (32);
		case DEVID_X1_010:
			// Sixteen channels, and the same channel is either a wavetable voice or a PCM player
			// depending on one status bit. The capability is static, so it states what the channel
			// can be at its best - a wavetable voice with a real pitch. A PCM hit still reports a
			// note (the decoder gives it the one-shot marker), so the promise holds in both modes.
			return fmChannel (16);
		case DEVID_NES_APU:
			return (channelIndex >= 0 && channelIndex < 3) ? playback::MidiCapability::melodic
														   : (channelIndex < 5
																  ? playback::MidiCapability::oneShot
																  : playback::MidiCapability::unavailable);
		case DEVID_GB_DMG:
			return (channelIndex >= 0 && channelIndex < 3) ? playback::MidiCapability::melodic
														   : (channelIndex == 3
																  ? playback::MidiCapability::oneShot
																  : playback::MidiCapability::unavailable);
		case DEVID_C6280:		// six wavetable channels, the noise generator shared with the last two
			return fmChannel (6);
		case DEVID_K051649:		// SCC1 / SCC+: five wavetable channels, no noise
			return fmChannel (5);
		case DEVID_SAA1099:		// six square channels; the noise and envelope generators feed them
			return fmChannel (6);
		case DEVID_POKEY:		// four dividers; a joined pair is one voice, handled by the decoder
			return fmChannel (4);
		case DEVID_WSWAN:		// four waveform channels; noise and the voice stream ride on them
			return fmChannel (4);
		default:
			return playback::MidiCapability::unavailable;
		}
	}
}

LibvgmSongInstance::LibvgmSongInstance (const playback::SongMemory& image)
	: memory (image), playerMemory (image)
{
}

bool LibvgmSongInstance::unpackImage (juce::String& errorMessage)
{
	if (! isCompressedImage (memory.data, memory.size))
	{
		return true;	// plain image - the players read the core's bytes directly
	}

	if (! flattenCompressedImage (memory.data, memory.size, ownedImage) || ownedImage.getSize() == 0)
	{
		errorMessage = "Could not read the file.";
		return false;
	}

	playerMemory.data = ownedImage.getData();
	playerMemory.size = ownedImage.getSize();
	return true;
}

bool LibvgmSongInstance::prepare (double hostSampleRate, int newMaxBlockSize, juce::String& errorMessage)
{
	sampleRate = (hostSampleRate > 0.0) ? hostSampleRate : 44100.0;
	maxBlockSize = juce::jmax (1, newMaxBlockSize);

	probe = std::make_unique<LibvgmTrackPlayer>();
	probe->prepare (sampleRate, maxBlockSize);
	if (! probe->load (playerMemory, errorMessage))
	{
		probe.reset();
		return false;
	}

	applyTimelineTo (*probe);

	// The probe is started like every rendering player: libvgm only reports the complete device
	// list (and the final loop time) once the player has walked the file once, and the engine
	// reads both from here. The probe is never rendered, so it costs no CPU.
	probe->start();

	enumerate();
	midiSequence = buildMidiSequence (playerMemory.data, playerMemory.size, deviceInfo, tracks,
									  toybox::settings::SettingsStore::getInstance().getTransposeBySampleRoot());
	if (midiSequence == nullptr)
	{
		// The builder only understands VGM today; do not offer a MIDI channel that can never
		// produce an event for S98/DRO/GYM.
		for (auto& track : tracks)
		{
			track.midiCapability = playback::MidiCapability::unavailable;
		}
	}
	probe->setTrackList (&tracks);	// setTrackAudible() resolves the track through this list
	refreshTimelineFacts();
	return true;
}

void LibvgmSongInstance::applyTimelineTo (LibvgmTrackPlayer& player) const
{
	player.setTimeline (loopCount, fadeSeconds);
}

void LibvgmSongInstance::setTimeline (int loops, double fade)
{
	loopCount = juce::jmax (0, loops);
	fadeSeconds = juce::jmax (0.0, fade);

	if (probe != nullptr)
	{
		applyTimelineTo (*probe);
	}

	// the cached facts depend on the loop setting, so they are re-read here - the core asks
	// for getInfo() right after this call
	refreshTimelineFacts();
}

void LibvgmSongInstance::refreshTimelineFacts (void)
{
	if (probe == nullptr || probe->player().GetPlayer() == NULL)
	{
		info.durationSeconds = 0.0;
		info.singlePassSeconds = 0.0;
		info.loopLengthSeconds = 0.0;
		info.loopPointSeconds = -1.0;
		info.loopsForever = false;
		return;
	}

	PlayerA& player = probe->player();

	// libvgm reports a negative length while the song loops forever, which would make the seek
	// bar unwritable (the position would be clamped to 0 on every refresh) - measure one
	// playthrough instead and let the position jump back at every repetition
	const double total = player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
	const double singlePass = juce::jmax (0.0, player.GetTotalTime (PLAYTIME_TIME_PBK));
	info.loopsForever = (total < 0.0);
	info.singlePassSeconds = singlePass;
	info.durationSeconds = info.loopsForever ? singlePass : total;

	// GetLoopTime() is the *length* of the loop, so the loop point is the length of a single
	// playthrough minus that length - the position the seek bar jumps back to. It has to be
	// derived from the *single* pass, not from `duration`: with two loops `duration` already
	// contains one repeat, and the marker would sit a whole loop too late.
	const double loopLength = juce::jmax (0.0, player.GetLoopTime());
	info.loopLengthSeconds = loopLength;
	info.loopPointSeconds = (loopLength > 0.0) ? juce::jmax (0.0, singlePass - loopLength) : -1.0;
}

/** What the probe knows about a song, in this project's vocabulary.

	libvgm answers in its own: a device list of DEVID/core/config triples, channel counts obtained by
	calling into the core declaration, names from SndEmu_GetDevName. This is where that becomes
	ChipInfo/TrackInfo, which the engine, the mixer and the MIDI side all speak. */
struct SongDescription
{
	playback::SongInfo info;
	std::vector<playback::TrackInfo> tracks;
	std::vector<PLR_DEV_INFO> devices;
};

SongDescription describeSong (LibvgmTrackPlayer* probe)
{
	SongDescription result;

	if (probe == nullptr)
	{
		return result;
	}

	if (probe == nullptr)
	{
		return result;
	}

	PlayerBase* pBase = probe->player().GetPlayer();
	if (pBase == NULL)
	{
		return result;
	}

	result.info.formatName = juce::String (pBase->GetPlayerName());

	// --- tags: the flat tag/value list the player offers ------------------------------
	const char* const* tagList = pBase->GetTags();
	if (tagList != NULL)
	{
		for (const char* const* tag = tagList; *tag != NULL; tag += 2)
		{
			if (tag[1] == NULL || tag[1][0] == '\0')
			{
				continue;
			}
			result.info.tags[juce::String (tag[0])] = juce::String::fromUTF8 (tag[1]);
		}

		// prefer the untranslated tags over the Japanese ones
		for (auto it = result.info.tags.begin(); it != result.info.tags.end();)
		{
			if (it->first.endsWith ("-JPN") && result.info.tags.count (it->first.dropLastCharacters (4)) > 0)
			{
				it = result.info.tags.erase (it);
			}
			else
			{
				++it;
			}
		}
	}

	// --- devices and their channels ---------------------------------------------------
	std::vector<PLR_DEV_INFO> devices;
	pBase->GetSongDeviceInfo (devices);
	result.devices = devices;

	std::map<uint32_t, int> instancesByType;
	for (const auto& device : devices)
	{
		if (isTrackDevice (device))
		{
			++instancesByType[(uint32_t) device.type];
		}
	}

	// the chips card: one entry per device (the second half of a T6W28 would only repeat its
	// group, so it stays hidden here - the mixer *must* keep it, its channels carry audio)
	for (const auto& device : devices)
	{
		if (! isTrackDevice (device))
		{
			continue;
		}
		if (device.type == DEVID_SN76496 && (device.devCfg->flags & 0x01) && (device.instance & 0x01))
		{
			continue;
		}

		playback::ChipInfo chip;
		const char* chipName = SndEmu_GetDevName (device.type, 0x01, device.devCfg);
		chip.name = juce::String (chipName != NULL ? chipName : "?");
		chip.core = fccToString (device.core);
		chip.deviceId = device.id;
		result.info.chips.push_back (chip);
	}

	// the mixer tracks: one per channel, or one for the whole device when the core the song
	// picked cannot mute single channels (without it the channels cannot be isolated, and
	// duplicating the device's audio across tracks would be wrong)
	int deviceIndex = 0;

	/** Whether a device's core can mute single channels - and therefore whether its voices can be
		tracks at all. */
	const auto isIsolatable = [] (const PLR_DEV_INFO& info)
	{
		if (info.devDecl == nullptr)
		{
			return false;
		}

		for (const DEV_DEF* const* core = info.devDecl->cores; *core != nullptr; ++core)
		{
			if ((*core)->coreID == info.core)
			{
				return (*core)->SetMuteMask != nullptr;
			}
		}

		return false;
	};

	for (size_t headIndex = 0; headIndex < devices.size(); ++headIndex)
	{
		const auto& device = devices[headIndex];

		if (! isTrackDevice (device))
		{
			continue;
		}

		const DEV_DECL* decl = device.devDecl;
		const int channelCount = channelCountOf (device);
		const char** channelNames = (decl != nullptr && decl->channelNames != nullptr)
									  ? decl->channelNames (device.devCfg) : nullptr;

		juce::String groupName = [&device]
		{
			const char* deviceName = SndEmu_GetDevName (device.type, 0x01, device.devCfg);
			return (deviceName != nullptr) ? juce::String (deviceName) : juce::String ("Device");
		}();
		if (instancesByType[(uint32_t) device.type] > 1)
		{
			const int instanceNumber = (device.instance == 0xFFFF) ? 1 : (int) device.instance + 1;
			groupName << " #" << instanceNumber;
		}

		const bool isolatable = isIsolatable (device);

		const auto addTrack = [&result, &device, deviceIndex, &groupName] (int channelIndex,
																		const juce::String& name,
																		bool trackIsolatable)
		{
			playback::TrackInfo track;
			track.id = (playback::TrackId) result.tracks.size();	// the handle the core passes back
			track.deviceId = device.id;
			track.deviceIndex = deviceIndex;
			track.channelIndex = channelIndex;
			track.isolatable = trackIsolatable;
			track.name = name;
			track.groupName = groupName;
			track.midiCapability = midiCapabilityFor (device, channelIndex);
			result.tracks.push_back (std::move (track));
		};

		if (! isolatable)
		{
			addTrack (-1, "All", false);
		}
		else
		{
			for (int channel = 0; channel < channelCount; ++channel)
			{
				const juce::String name = (channelNames != nullptr && channelNames[channel] != nullptr)
											? juce::String (channelNames[channel])
											: ("Ch " + juce::String (channel + 1));
				addTrack (channel, name, true);
			}
		}

		// A linked device that is a voice set of its own - the SSG behind an OPN chip - gets its
		// channels as tracks, numbered after the parent's so the parent's indices do not move: a
		// session saved before this change keeps its per-track gains and mutes.
		//
		// Leaving the SSG out of the layout and trusting the mask to keep it quiet is what this
		// replaces, and the mask does not (docs/memo/2026-10-25-linked-devices-and-channel-numbering.md).
		int linkedBase = channelCount;

		for (const auto& child : devices)
		{
			if (child.parentIdx != (uint32_t) headIndex
				|| linkedDeviceKindFor (child.type) != LinkedDeviceKind::extraVoices)
			{
				continue;
			}

			const int childChannels = channelCountOf (child);
			const bool childIsolatable = isIsolatable (child);

			for (int channel = 0; channel < childChannels; ++channel)
			{
				addTrack (linkedBase + channel, "SSG " + juce::String (channel + 1), childIsolatable);
			}

			linkedBase += childChannels;
		}

		++deviceIndex;
	}
	return result;
}
void LibvgmSongInstance::enumerate (void)
{
	const SongDescription described = describeSong (probe.get());

	info = described.info;
	tracks = described.tracks;
	deviceInfo = described.devices;
}

playback::BackendCapabilities LibvgmSongInstance::getCapabilities (void) const
{
	playback::BackendCapabilities caps;
	caps.canSeek = true;
	caps.canLoop = true;
	caps.canFade = true;
	caps.hasLoopPoint = (info.loopPointSeconds >= 0.0);
	caps.canMuteChips = ! info.chips.empty();
	caps.canEmitMidi = true;		// the register stream is parsed into a sequence at load
	caps.isolation = playback::IsolationMode::multiInstance;
	caps.rendersAtHostRate = true;		// the players are built at the host rate (and rebuilt when it changes)
	caps.nativeSampleRate = 0.0;
	return caps;
}

std::unique_ptr<playback::ITrackPlayer> LibvgmSongInstance::createPlayer (juce::String& errorMessage)
{
	auto player = std::make_unique<LibvgmTrackPlayer>();
	player->prepare (sampleRate, maxBlockSize);

	if (! player->load (playerMemory, errorMessage))
	{
		return nullptr;
	}

	player->setTimeline (loopCount, fadeSeconds);
	player->setTrackList (&tracks);
	return player;
}

} // namespace toybox::backends::libvgm
