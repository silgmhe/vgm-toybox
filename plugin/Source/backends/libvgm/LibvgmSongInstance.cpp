#include "LibvgmSongInstance.h"

#include "NesDpcmBank.h"

#include "LibvgmMuteIsolation.h"

#include <algorithm>
#include <map>

#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

#include "LibvgmVgz.h"
#include "MidiSequenceBuilder.h"
#include "midi/ChipPlans.h"
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
	/** Which MIDI chip a libvgm device id names.

		This is the seam SongIr.h describes: the MIDI side knows `ChipKind` and nothing about file
		formats, and every backend translates its own numbering once - here, for VGM. Two ids can
		name one kind (the 219 ASIC is a cut-down C140 and reports through the same decoder), and a
		device with no pitch at all maps to `unknown`, whose plan is empty.

		This switch is a translation, not a table: the left-hand values belong to libvgm and exist
		nowhere else in this project, so there is nothing for the two sides to drift apart into.
	*/
	midi::ChipKind chipKindForDevice (uint32_t deviceId)
	{
		using midi::ChipKind;

		switch (deviceId)
		{
		case DEVID_AY8910:      return ChipKind::ay8910;
		case DEVID_SN76496:     return ChipKind::sn76489;
		case DEVID_SAA1099:     return ChipKind::saa1099;
		case DEVID_POKEY:       return ChipKind::pokey;
		case DEVID_YM2413:      return ChipKind::ym2413;
		case DEVID_YM3812:      return ChipKind::ym3812;
		case DEVID_YM3526:      return ChipKind::ym3526;
		case DEVID_Y8950:       return ChipKind::y8950;
		case DEVID_YMF262:      return ChipKind::ymf262;
		case DEVID_YMF278B:     return ChipKind::ymf278b;
		case DEVID_YM2612:      return ChipKind::ym2612;
		case DEVID_YM2203:      return ChipKind::ym2203;
		case DEVID_YM2608:      return ChipKind::ym2608;
		case DEVID_YM2610:      return ChipKind::ym2610;
		case DEVID_YM2151:      return ChipKind::ym2151;
		case DEVID_YMF271:      return ChipKind::ymf271;
		case DEVID_YMZ280B:     return ChipKind::ymz280b;
		case DEVID_C140:        return ChipKind::c140;
		case DEVID_C219:        return ChipKind::c140;      // the 219 ASIC is a cut-down C140
		case DEVID_C352:        return ChipKind::c352;
		case DEVID_ES5503:      return ChipKind::es5503;
		case DEVID_GA20:        return ChipKind::ga20;
		case DEVID_K053260:     return ChipKind::k053260;
		case DEVID_K054539:     return ChipKind::k054539;
		case DEVID_YMW258:      return ChipKind::multiPcm;
		case DEVID_MSM6295:     return ChipKind::okim6295;
		case DEVID_QSOUND:      return ChipKind::qsound;
		case DEVID_RF5C68:      return ChipKind::rf5c68;
		case DEVID_SEGAPCM:     return ChipKind::segapcm;
		case DEVID_GB_DMG:      return ChipKind::gameBoy;
		case DEVID_C6280:       return ChipKind::huc6280;
		case DEVID_K005289:     return ChipKind::k005289;
		case DEVID_K007232:     return ChipKind::k007232;
		case DEVID_K051649:     return ChipKind::k051649;
		case DEVID_MIKEY:       return ChipKind::mikey;
		case DEVID_MSM5232:     return ChipKind::msm5232;
		case DEVID_NES_APU:     return ChipKind::nesApu;
		case DEVID_VBOY_VSU:    return ChipKind::vsu;
		case DEVID_WSWAN:       return ChipKind::wonderswan;
		case DEVID_X1_010:      return ChipKind::x1_010;

		// 32X PWM, MSM6258 and uPD7759 have no pitch to report at all. The reasoning is kept in
		// docs/issue; here it is enough that `unknown` carries an empty plan.
		default:                return ChipKind::unknown;
		}
	}

	/** What the MIDI decoder will be able to do for one channel of a device.

		The answer comes from midi/ChipPlans, which is keyed on the chip's kind rather than on
		libvgm's device id. That is what this function needed. Capability used to be a 255-line
		switch over `DEVID_*`, and adding Ay8910Decoder left its entry saying "nothing in this
		backend decodes the SSG" - the table and the decoders were keyed on different vocabularies,
		so nothing connected them. Now a chip whose decoder exists is one edit away from a plan,
		and a test holds every plan to the promise its decoder makes.
	*/
	playback::MidiCapability midiCapabilityFor (const PLR_DEV_INFO& device, int channelIndex)
	{
		if (channelIndex < 0)
		{
			return playback::MidiCapability::unavailable;
		}

		// The one device whose plan depends on a configuration bit rather than on its kind: the
		// non-B YM2610 leaves channels 0 and 3 unwired. Everything else - including the SSG an OPN
		// chip links behind itself, which libvgm numbers after the parent's channels - is a run of
		// channels, and lives in the table.
		if (device.type == DEVID_YM2610 && channelIndex < 6)
		{
			const bool isB = device.devCfg != nullptr && (device.devCfg->flags & 0x01) != 0;
			return (isB || (channelIndex != 0 && channelIndex != 3))
					   ? playback::MidiCapability::melodic
					   : playback::MidiCapability::unavailable;
		}

		return midi::channelPlanFor (chipKindForDevice (device.type)).at (channelIndex);
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
	// The samples this song starts, taken from the same bytes the players load. A VGM says what it
	// plays in its command stream, so this is one walk with no emulation - unlike the NSF path,
	// which has to run the song to find out.
	//
	// Only when the file actually has a 2A03: the walk reads the whole command stream, and for a
	// file built on other chips that read can only come back empty. `enumerate()` above has
	// already filled deviceInfo.
	const bool hasNes = std::any_of (deviceInfo.begin (), deviceInfo.end (),
									 [] (const PLR_DEV_INFO& device) { return device.type == DEVID_NES_APU; });

	if (hasNes)
	{
		dpcmSamples = std::make_shared<const std::vector<playback::DecodedSample>> (
			collectNesDpcmSamples (playerMemory.data, playerMemory.size));
	}

	probe->setSamples (dpcmSamples);
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
	player->setSamples (dpcmSamples);
	player->setTrackList (&tracks);
	return player;
}

} // namespace toybox::backends::libvgm
