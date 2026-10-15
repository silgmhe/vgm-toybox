#include "LibvgmSongInstance.h"

#include <map>

#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

#include "LibvgmVgz.h"

namespace toybox::backends::libvgm
{

namespace
{
	/** The emulation core is reported as a four character code - show it as text. */
	juce::String fccToString (UINT32 code)
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
		return info.parentIdx == (UINT32) -1;
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
		info.loopPointSeconds = -1.0;
		info.loopsForever = false;
		return;
	}

	PlayerA& player = probe->player();

	// libvgm reports a negative length while the song loops forever, which would make the seek
	// bar unwritable (the position would be clamped to 0 on every refresh) - measure one
	// playthrough instead and let the position jump back at every repetition
	const double total = player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
	info.loopsForever = (total < 0.0);
	info.durationSeconds = info.loopsForever ? juce::jmax (0.0, player.GetTotalTime (PLAYTIME_TIME_PBK))
											 : total;

	// GetLoopTime() is the *length* of the loop, so the loop point is the length of a single
	// playthrough minus that length - the position the seek bar jumps back to
	const double loopLength = player.GetLoopTime();
	info.loopPointSeconds = (loopLength > 0.0) ? juce::jmax (0.0, info.durationSeconds - loopLength)
											   : -1.0;
}

void LibvgmSongInstance::enumerate (void)
{
	info.tags.clear();
	info.chips.clear();
	tracks.clear();

	if (probe == nullptr)
	{
		return;
	}

	PlayerBase* pBase = probe->player().GetPlayer();
	if (pBase == NULL)
	{
		return;
	}

	info.formatName = juce::String (pBase->GetPlayerName());

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
			info.tags[juce::String (tag[0])] = juce::String::fromUTF8 (tag[1]);
		}

		// prefer the untranslated tags over the Japanese ones
		for (auto it = info.tags.begin(); it != info.tags.end();)
		{
			if (it->first.endsWith ("-JPN") && info.tags.count (it->first.dropLastCharacters (4)) > 0)
			{
				it = info.tags.erase (it);
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

	std::map<UINT32, int> instancesByType;
	for (const auto& device : devices)
	{
		if (isTrackDevice (device))
		{
			++instancesByType[(UINT32) device.type];
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
		info.chips.push_back (chip);
	}

	// the mixer tracks: one per channel, or one for the whole device when the core the song
	// picked cannot mute single channels (without it the channels cannot be isolated, and
	// duplicating the device's audio across tracks would be wrong)
	int deviceIndex = 0;
	for (const auto& device : devices)
	{
		if (! isTrackDevice (device))
		{
			continue;
		}

		const DEV_DECL* decl = device.devDecl;
		const int channelCount = (decl != nullptr && decl->channelCount != nullptr)
								   ? (int) decl->channelCount (device.devCfg) : 0;
		const char** channelNames = (decl != nullptr && decl->channelNames != nullptr)
									  ? decl->channelNames (device.devCfg) : nullptr;

		juce::String groupName = [&device]
		{
			const char* deviceName = SndEmu_GetDevName (device.type, 0x01, device.devCfg);
			return (deviceName != nullptr) ? juce::String (deviceName) : juce::String ("Device");
		}();
		if (instancesByType[(UINT32) device.type] > 1)
		{
			const int instanceNumber = (device.instance == 0xFFFF) ? 1 : (int) device.instance + 1;
			groupName << " #" << instanceNumber;
		}

		bool isolatable = false;
		if (decl != nullptr)
		{
			for (const DEV_DEF* const* core = decl->cores; *core != nullptr; ++core)
			{
				if ((*core)->coreID == device.core)
				{
					isolatable = ((*core)->SetMuteMask != nullptr);
					break;
				}
			}
		}

		const auto addTrack = [this, &device, deviceIndex, &groupName] (int channelIndex,
																		const juce::String& name,
																		bool trackIsolatable)
		{
			playback::TrackInfo track;
			track.id = (playback::TrackId) tracks.size();	// the handle the core passes back
			track.deviceId = device.id;
			track.deviceIndex = deviceIndex;
			track.channelIndex = channelIndex;
			track.isolatable = trackIsolatable;
			track.name = name;
			track.groupName = groupName;
			tracks.push_back (std::move (track));
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

		++deviceIndex;
	}
}

playback::BackendCapabilities LibvgmSongInstance::getCapabilities (void) const
{
	playback::BackendCapabilities caps;
	caps.canSeek = true;
	caps.canLoop = true;
	caps.canFade = true;
	caps.hasLoopPoint = (info.loopPointSeconds >= 0.0);
	caps.canMuteChips = ! info.chips.empty();
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
