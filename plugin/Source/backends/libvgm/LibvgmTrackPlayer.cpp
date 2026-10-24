#include "LibvgmTrackPlayer.h"

#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/s98player.hpp"
#include "player/vgmplayer.hpp"
#include "utils/MemoryLoader.h"

#include "LibvgmMuteIsolation.h"

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

	void logCallback (void*, PlayerBase* player, UINT8 level, UINT8 srcType, const char* srcTag, const char* message)
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
	playerEngine.SetOutputSettings ((UINT32) sampleRate, 2, 32, (UINT32) maxBlockSize);
}

bool LibvgmTrackPlayer::load (const playback::SongMemory& memory, juce::String& errorMessage)
{
	unload();

	if (! memory.isValid())
	{
		errorMessage = "Could not read the file.";
		return false;
	}

	dataLoader = MemoryLoader_Init ((const UINT8*) memory.data, (UINT32) memory.size);
	if (dataLoader == NULL)
	{
		errorMessage = "Out of memory.";
		return false;
	}

	UINT8 retVal = DataLoader_Load (dataLoader);
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

	playerEngine.Seek (PLAYPOS_SAMPLE, (UINT32) (juce::jmax (0.0, seconds) * sampleRate));
	finished = false;
	return true;
}

void LibvgmTrackPlayer::setTimeline (int loops, double fade)
{
	loopCount = juce::jmax (0, loops);
	fadeSeconds = juce::jmax (0.0, fade);

	PlayerA::Config cfg = playerEngine.GetConfiguration();
	cfg.masterVol = 0x10000;	// unity - the engine applies its own volume stages
	cfg.loopCount = (UINT32) loopCount;
	cfg.fadeSmpls = (UINT32) (fadeSeconds * sampleRate);
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
			playerEngine.SetLoopCount (vgmPlayer->GetModifiedLoopCount ((UINT32) loopCount));
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

void LibvgmTrackPlayer::applyMuting (UINT32 deviceId, const PLR_MUTE_OPTS& muteOpts)
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
	auto applyMuteGroup = [&devices, this] (const PLR_DEV_INFO& device, const PLR_MUTE_OPTS& options)
	{
		applyMuting (device.id, options);

		for (const auto& child : devices)
		{
			if (child.parentIdx == device.id)
			{
				applyMuting (child.id, options);
			}
		}
	};

	for (const auto& info : devices)
	{
		if (info.parentIdx != (UINT32) -1)
		{
			continue;	// linked devices are configured through their parent
		}


		if (! isolated)
		{
			// the plain, un-isolated player: every device plays, only the chips card mutes
			applyMuteGroup (info, chipMuteOptions (! isChipAudible (info.id), false));
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
			applyMuteGroup (info, suspendedMuteOptions());
			continue;
		}

		// the track's own device: every channel muted except the track's own one - unless the
		// core cannot mute single channels, in which case the whole device is the track
		applyMuteGroup (info, singleChannelMuteOptions (myTrackInfo->channelIndex,
													   myTrackInfo->isolatable));
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
	const UINT32 renderedBytes = playerEngine.Render ((UINT32) samplesToRender * (UINT32) kBytesPerSample,
													  renderBuffer.data());
	const int outSamples = juce::jmin ((int) (renderedBytes / (UINT32) kBytesPerSample), samplesToRender);

	for (int i = 0; i < outSamples; ++i)
	{
		left[i] = (float) renderBuffer[(size_t) i].L * kInt32ToFloat;
		right[i] = (float) renderBuffer[(size_t) i].R * kInt32ToFloat;
	}

	return outSamples;
}

UINT8 LibvgmTrackPlayer::eventCallback (PlayerBase* /*player*/, void* userParam, UINT8 evtType, void* evtParam)
{
	LibvgmTrackPlayer* track = static_cast<LibvgmTrackPlayer*> (userParam);
	return (track != nullptr) ? track->handleEvent (evtType, evtParam) : 0x00;
}

UINT8 LibvgmTrackPlayer::handleEvent (UINT8 evtType, void* /*evtParam*/)
{
	if (evtType == PLREVT_END)
	{
		finished = true;	// the engine stops rendering after this block
	}
	return 0x00;
}

} // namespace toybox::backends::libvgm
