#include "TrackLayout.h"

#include <map>

#include "emu/SoundEmu.h"

namespace vgmplay
{

std::vector<TrackLayoutEntry> buildTrackLayout (const PlayerBase& player)
{
	std::vector<TrackLayoutEntry> layout;

	std::vector<PLR_DEV_INFO> devices;
	player.GetSongDeviceInfo (devices);

	// Linked devices are an implementation detail of the cores (they are configured through
	// their parent's options), so they never become tracks of their own.
	const auto isTrackDevice = [] (const PLR_DEV_INFO& info) -> bool
	{
		return info.parentIdx == (UINT32) -1;
	};

	// how often each device type appears in the song - only then a "#2" suffix is useful
	std::map<UINT32, int> instancesByType;
	for (const auto& info : devices)
	{
		if (isTrackDevice (info))
		{
			++instancesByType[(UINT32) info.type];
		}
	}

	int deviceIndex = 0;
	for (const auto& info : devices)
	{
		if (! isTrackDevice (info))
		{
			continue;
		}

		const DEV_DECL* decl = info.devDecl;
		const int channelCount = (decl != nullptr && decl->channelCount != nullptr)
								   ? (int) decl->channelCount (info.devCfg) : 0;
		const char** channelNames = (decl != nullptr && decl->channelNames != nullptr)
									  ? decl->channelNames (info.devCfg) : nullptr;

		const char* deviceName = SndEmu_GetDevName (info.type, 0x01, info.devCfg);
		juce::String groupName = (deviceName != nullptr) ? juce::String (deviceName)
														 : juce::String ("Device");
		if (instancesByType[(UINT32) info.type] > 1)
		{
			const int instanceNumber = (info.instance == 0xFFFF) ? 1 : (int) info.instance + 1;
			groupName << " #" << instanceNumber;
		}

		for (int channel = 0; channel < channelCount; ++channel)
		{
			TrackLayoutEntry entry;
			entry.deviceID = info.id;
			entry.deviceIndex = deviceIndex;
			entry.channelIndex = channel;
			entry.name = (channelNames != nullptr && channelNames[channel] != nullptr)
						   ? juce::String (channelNames[channel])
						   : ("Ch " + juce::String (channel + 1));
			entry.groupName = groupName;
			layout.push_back (std::move (entry));
		}

		++deviceIndex;
	}

	return layout;
}

} // namespace vgmplay
