#include "LibvgmTrackPlayer.h"

#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/s98player.hpp"
#include "player/vgmplayer.hpp"
#include "utils/MemoryLoader.h"

#include "LibvgmMuteIsolation.h"
#include "emu/SoundDevs.h"

namespace toybox::backends::libvgm
{

namespace
{
	// libvgm renders 24-bit samples internally and shifts them left by 8 bit when packing to
	// 32-bit output, so full scale is +/- 2^31.
	constexpr float kInt32ToFloat = 1.0f / 2147483648.0f;
	constexpr int kBytesPerSample = 8;					// 32 bit * 2 channels

	// Players are constructed on the message thread - remember it, so the log callback can
	// stay silent on the audio thread (building the strings or entering the logger allocates).
	juce::Thread::ThreadID messageThreadId = {};

	void logCallback (void*, PlayerBase* player, uint8_t level, uint8_t srcType, const char* srcTag, const char* message)
	{
		if (level > PLRLOG_WARN)
		{
			return;
		}
		if (juce::Thread::getCurrentThreadId() != messageThreadId)
		{
			return;
		}

		const juce::String tag = (srcType == PLRLOGSRC_PLR) ? juce::String (player->GetPlayerName())
															: juce::String (srcTag);
		juce::Logger::writeToLog ("[VGMToybox/" + tag + "] " + juce::String (message).trim());
	}
}

LibvgmTrackPlayer::LibvgmTrackPlayer (void)
{
	messageThreadId = juce::Thread::getCurrentThreadId();
	playerEngine.RegisterPlayerEngine (new VGMPlayer);
	playerEngine.RegisterPlayerEngine (new S98Player);
	playerEngine.RegisterPlayerEngine (new DROPlayer);
	playerEngine.RegisterPlayerEngine (new GYMPlayer);
	playerEngine.SetEventCallback (eventCallback, this);
	playerEngine.SetLogCallback (logCallback, nullptr);
}

LibvgmTrackPlayer::~LibvgmTrackPlayer (void)
{
	unload();
	playerEngine.UnregisterAllPlayers();
}

void LibvgmTrackPlayer::prepare (double hostSampleRate, int newMaxBlockSize)
{
	sampleRate = (hostSampleRate > 0.0) ? hostSampleRate : 44100.0;
	maxBlockSize = juce::jmax (1, newMaxBlockSize);

	renderBuffer.assign ((size_t) maxBlockSize, WAVE_32BS());

	// 32-bit integer stereo output (libvgm's highest precision integer format)
	playerEngine.SetOutputSettings ((uint32_t) sampleRate, 2, 32, (uint32_t) maxBlockSize);
}

bool LibvgmTrackPlayer::load (const playback::SongMemory& memory, juce::String& errorMessage)
{
	unload();

	if (! memory.isValid())
	{
		errorMessage = "Could not read the file.";
		return false;
	}

	dataLoader = MemoryLoader_Init ((const uint8_t*) memory.data, (uint32_t) memory.size);
	if (dataLoader == NULL)
	{
		errorMessage = "Out of memory.";
		return false;
	}

	uint8_t retVal = DataLoader_Load (dataLoader);
	if (retVal)
	{
		errorMessage = juce::String::formatted ("Could not load the file (error 0x%02X).", (unsigned) retVal);
		unload();
		return false;
	}

	retVal = playerEngine.LoadFile (dataLoader);
	if (retVal)
	{
		errorMessage = juce::String::formatted ("Unsupported file format (error 0x%02X).", (unsigned) retVal);
		unload();
		return false;
	}

	return true;
}

void LibvgmTrackPlayer::unload (void)
{
	if (playerEngine.GetPlayer() != NULL)
	{
		playerEngine.UnloadFile();
	}
	if (dataLoader != NULL)
	{
		DataLoader_Deinit (dataLoader);
		dataLoader = NULL;
	}

	finished = false;
}

void LibvgmTrackPlayer::start (void)
{
	playerEngine.Start();
}

void LibvgmTrackPlayer::pause (void)
{
	// A libvgm player only advances inside Render(), so pausing *is* "the engine stops calling
	// render()" - there is no separate pause state to set. (The port keeps the method for
	// backends that do need it, e.g. one that emulates on its own thread.)
}

bool LibvgmTrackPlayer::seekSeconds (double seconds)
{
	if (playerEngine.GetPlayer() == NULL)
	{
		return false;
	}

	playerEngine.Seek (PLAYPOS_SAMPLE, (uint32_t) (juce::jmax (0.0, seconds) * sampleRate));
	finished = false;
	return true;
}

void LibvgmTrackPlayer::setTimeline (int loops, double fade)
{
	loopCount = juce::jmax (0, loops);
	fadeSeconds = juce::jmax (0.0, fade);

	PlayerA::Config cfg = playerEngine.GetConfiguration();
	cfg.masterVol = 0x10000;	// unity - the engine applies its own volume stages
	cfg.loopCount = (uint32_t) loopCount;
	cfg.fadeSmpls = (uint32_t) (fadeSeconds * sampleRate);
	cfg.endSilenceSmpls = 0;	// a plugin should not render trailing silence
	cfg.pbSpeed = 1.0;
	playerEngine.SetConfiguration (cfg);

	PlayerBase* pBase = playerEngine.GetPlayer();
	if (pBase == NULL)
	{
		return;
	}

	// VGM files without a loop point would otherwise be faded out immediately
	if (pBase->GetPlayerType() == FCC_VGM)
	{
		if (VGMPlayer* vgmPlayer = dynamic_cast<VGMPlayer*> (pBase))
		{
			playerEngine.SetLoopCount (vgmPlayer->GetModifiedLoopCount ((uint32_t) loopCount));
		}
	}

	// While looping forever the absolute position keeps growing, so the position queries use
	// the single-playthrough mode instead (libvgm reports an infinite runtime with a negative
	// length there) - the same mode the engine measures the seek bar in.
	loopsForever = (playerEngine.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK) < 0.0);
}

double LibvgmTrackPlayer::positionSeconds (void)
{
	if (playerEngine.GetPlayer() == NULL)
	{
		return -1.0;
	}

	return playerEngine.GetCurTime (loopsForever ? PLAYTIME_TIME_PBK
												: (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK));
}

void LibvgmTrackPlayer::applyMuting (uint32_t deviceId, const PLR_MUTE_OPTS& muteOpts)
{
	PlayerBase* pBase = playerEngine.GetPlayer();
	if (pBase != NULL)
	{
		pBase->SetDeviceMuting (deviceId, muteOpts);
	}
}

bool LibvgmTrackPlayer::isChipAudible (playback::DeviceId device) const
{
	for (const auto& entry : chipAudible)
	{
		if (entry.first == device)
		{
			return entry.second;
		}
	}

	return true;	// devices are audible until the chips card mutes them
}

const playback::TrackInfo* LibvgmTrackPlayer::findMyTrack (void) const
{
	if (tracks == nullptr || myTrack >= tracks->size())
	{
		return nullptr;
	}

	return &(*tracks)[myTrack];
}

void LibvgmTrackPlayer::setTrackAudible (playback::TrackId track, bool audible)
{
	isolated = true;
	myTrack = track;
	myTrackAudible = audible;
	applyIsolation();
}

void LibvgmTrackPlayer::setChipAudible (playback::DeviceId device, bool audible)
{
	for (auto& entry : chipAudible)
	{
		if (entry.first == device)
		{
			entry.second = audible;
			applyIsolation();
			return;
		}
	}

	chipAudible.emplace_back (device, audible);
	applyIsolation();
}

void LibvgmTrackPlayer::applyIsolation (void)
{
	PlayerBase* pBase = playerEngine.GetPlayer();
	if (pBase == NULL)
	{
		return;
	}

	std::vector<PLR_DEV_INFO> devices;
	pBase->GetSongDeviceInfo (devices);

	const playback::TrackInfo* myTrackInfo = findMyTrack();

	// One chip can be more than one device: OPL4 is the OPL3 half plus a PCM half that libvgm
	// reports as a linked child, and a mute sent only to the parent left the PCM playing. Every
	// mute therefore goes to the device and to whatever is linked under it.
	// Children are matched by the parent's *index* in the device list: a linked device shares its
	// parent's id - both are 0 on a YM2610 - so comparing ids finds the child of the wrong chain
	// as soon as a song has two chips of the same kind.
	auto applyMuteGroup = [&devices, this] (size_t parentIndex, const PLR_MUTE_OPTS& options)
	{
		applyMuting (devices[parentIndex].id, options);

		for (size_t i = 0; i < devices.size(); ++i)
		{
			if (devices[i].parentIdx == (uint32_t) parentIndex)
			{
				applyMuting (devices[i].id, options);
			}
		}
	};

	/** The kind of the devices linked behind one chain, if it has any. */
	const auto linkedKindOf = [&devices] (size_t parentIndex)
	{
		for (const auto& child : devices)
		{
			if (child.parentIdx == (uint32_t) parentIndex)
			{
				return linkedDeviceKindFor (child.type);
			}
		}

		return LinkedDeviceKind::mirror;	// nothing linked, or nothing that cares
	};

	for (size_t deviceIndex = 0; deviceIndex < devices.size(); ++deviceIndex)
	{
		const auto& info = devices[deviceIndex];

		if (info.parentIdx != (uint32_t) -1)
		{
			continue;	// linked devices are configured through their parent
		}

		if (! isolated)
		{
			// the plain, un-isolated player: every device plays, only the chips card mutes
			applyMuteGroup (deviceIndex, chipMuteOptions (! isChipAudible (info.id), false));
			continue;
		}

		// Not this track's device, the track is switched off, or the chip it belongs to is muted
		// on the chips card: suspend it completely.
		//
		// The muted case used to keep the single-channel options and only set `disable`, which
		// left the one channel this player exists for audible - so the checkbox did nothing on a
		// mixer-enabled player while the plain player honoured it. A suspended device is what
		// "this chip is muted" means for an isolated player.
		if (myTrackInfo == nullptr || info.id != myTrackInfo->deviceId || ! myTrackAudible
			|| ! isChipAudible (info.id))
		{
			applyMuteGroup (deviceIndex, suspendedMuteOptions());
			continue;
		}

		// A track can live on a device linked behind this one - the SSG's three channels are
		// numbered after the parent's - and then the second half of the mask is the one that
		// counts, against the linked device's own numbering.
		const int parentChannels = channelCountOf (info);

		if (parentChannels > 0 && myTrackInfo->channelIndex >= parentChannels)
		{
			// The parent device contributes nothing to this track, so it is suspended rather than
			// muted channel by channel - and the mask below is read against the linked device's own
			// numbering, starting at zero.
			PLR_MUTE_OPTS linked = {};
			linked.disable = kSuspendMainDevice;

			const int localChannel = myTrackInfo->channelIndex - parentChannels;
			linked.chnMute[1] = myTrackInfo->isolatable
									? (0xFFFFFFFF & ~channelMuteBit (localChannel))
									: 0x00000000;

			applyMuteGroup (deviceIndex, linked);
			continue;
		}

		// the track's own device: every channel muted except the track's own one - unless the
		// core cannot mute single channels, in which case the whole device is the track
		PLR_MUTE_OPTS isolate = singleChannelMuteOptions (myTrackInfo->channelIndex,
														 myTrackInfo->isolatable);

		// A linked device with channel numbers of its own does not belong in this track's render:
		// the mask's second half is read against *its* numbering, so a track for FM channel 2 left
		// SSG channel 2 audible - measured at 32% of that strip's energy on a song where the SSG
		// is loud, and heard as crosstalk and a noise floor under the FM. The same mistake is in
		// every OPN chip: YM2203 leaks all three of its SSG channels into its three FM tracks, and
		// YM2608 leaks them into tracks 0-2.
		//
		// The SSG has no track of its own yet, so suspending it costs nothing that is mixed. A
		// mirror - an OPL4's OPL3 - must keep the mask in both halves: there the linked device is
		// where the chip's audio is, and suspending it silences the whole chip.
		if (linkedKindOf (deviceIndex) == LinkedDeviceKind::extraVoices)
		{
			isolate.disable |= kSuspendLinkedDevices;
		}

		applyMuteGroup (deviceIndex, isolate);
	}
}

int LibvgmTrackPlayer::render (float* left, float* right, int numSamples)
{
	if (playerEngine.GetPlayer() == NULL || left == nullptr || right == nullptr || numSamples <= 0)
	{
		return 0;
	}

	const int samplesToRender = juce::jmin (numSamples, maxBlockSize);
	// note: PlayerA::Render() returns the number of *bytes* that were written
	const uint32_t renderedBytes = playerEngine.Render ((uint32_t) samplesToRender * (uint32_t) kBytesPerSample,
													  renderBuffer.data());
	const int outSamples = juce::jmin ((int) (renderedBytes / (uint32_t) kBytesPerSample), samplesToRender);

	for (int i = 0; i < outSamples; ++i)
	{
		left[i] = (float) renderBuffer[(size_t) i].L * kInt32ToFloat;
		right[i] = (float) renderBuffer[(size_t) i].R * kInt32ToFloat;
	}

	return outSamples;
}

uint8_t LibvgmTrackPlayer::eventCallback (PlayerBase* /*player*/, void* userParam, uint8_t evtType, void* evtParam)
{
	LibvgmTrackPlayer* track = static_cast<LibvgmTrackPlayer*> (userParam);
	return (track != nullptr) ? track->handleEvent (evtType, evtParam) : 0x00;
}

uint8_t LibvgmTrackPlayer::handleEvent (uint8_t evtType, void* /*evtParam*/)
{
	if (evtType == PLREVT_END)
	{
		finished = true;	// the engine stops rendering after this block
	}
	return 0x00;
}

} // namespace toybox::backends::libvgm
