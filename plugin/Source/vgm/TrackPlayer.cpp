#include "TrackPlayer.h"

#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/s98player.hpp"
#include "player/vgmplayer.hpp"
#include "utils/MemoryLoader.h"

namespace vgmplay
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
		juce::Logger::writeToLog ("[VGMPlay/" + tag + "] " + juce::String (message).trim());
	}
}

TrackPlayer::TrackPlayer (void)
{
	messageThreadId = juce::Thread::getCurrentThreadId();
	playerEngine.RegisterPlayerEngine (new VGMPlayer);
	playerEngine.RegisterPlayerEngine (new S98Player);
	playerEngine.RegisterPlayerEngine (new DROPlayer);
	playerEngine.RegisterPlayerEngine (new GYMPlayer);
	playerEngine.SetEventCallback (eventCallback, this);
	playerEngine.SetLogCallback (logCallback, nullptr);
}

TrackPlayer::~TrackPlayer (void)
{
	unload();
	playerEngine.UnregisterAllPlayers();
}

void TrackPlayer::prepare (double newSampleRate, int newMaxBlockSize)
{
	sampleRate = (newSampleRate > 0.0) ? newSampleRate : 44100.0;
	maxBlockSize = juce::jmax (1, newMaxBlockSize);

	renderBuffer.assign ((size_t) maxBlockSize, WAVE_32BS());
	left.assign ((size_t) maxBlockSize, 0.0f);
	right.assign ((size_t) maxBlockSize, 0.0f);

	// 32-bit integer stereo output (libvgm's highest precision integer format)
	playerEngine.SetOutputSettings ((UINT32) sampleRate, 2, 32, (UINT32) maxBlockSize);
}

bool TrackPlayer::loadFile (const juce::MemoryBlock& fileData, juce::String& errorMessage)
{
	unload();

	dataLoader = MemoryLoader_Init ((const UINT8*) fileData.getData(), (UINT32) fileData.getSize());
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

void TrackPlayer::unload (void)
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
	renderedSamples = 0;
}

void TrackPlayer::start (void)
{
	playerEngine.Start();
}

void TrackPlayer::seekToSample (UINT32 sample)
{
	playerEngine.Seek (PLAYPOS_SAMPLE, sample);
	finished = false;
}

void TrackPlayer::applyConfig (const PlayerA::Config& config)
{
	playerEngine.SetConfiguration (config);
}

void TrackPlayer::applyMuting (UINT32 deviceID, const PLR_MUTE_OPTS& muteOpts)
{
	PlayerBase* pBase = playerEngine.GetPlayer();
	if (pBase != NULL)
	{
		pBase->SetDeviceMuting (deviceID, muteOpts);
	}
}

void TrackPlayer::render (int numSamples)
{
	renderedSamples = 0;

	if (playerEngine.GetPlayer() == NULL)
	{
		return;
	}

	const int samplesToRender = juce::jmin (numSamples, maxBlockSize);
	// note: PlayerA::Render() returns the number of *bytes* that were written
	const UINT32 renderedBytes = playerEngine.Render ((UINT32) samplesToRender * (UINT32) kBytesPerSample,
													  renderBuffer.data());
	const int outSamples = juce::jmin ((int) (renderedBytes / (UINT32) kBytesPerSample), samplesToRender);

	for (int i = 0; i < outSamples; ++i)
	{
		left[(size_t) i] = (float) renderBuffer[(size_t) i].L * kInt32ToFloat;
		right[(size_t) i] = (float) renderBuffer[(size_t) i].R * kInt32ToFloat;
	}
	renderedSamples = outSamples;
}

UINT8 TrackPlayer::eventCallback (PlayerBase* /*player*/, void* userParam, UINT8 evtType, void* evtParam)
{
	TrackPlayer* track = static_cast<TrackPlayer*> (userParam);
	return (track != nullptr) ? track->handleEvent (evtType, evtParam) : 0x00;
}

UINT8 TrackPlayer::handleEvent (UINT8 evtType, void* /*evtParam*/)
{
	if (evtType == PLREVT_END)
	{
		finished = true;	// the engine stops rendering after this block
	}
	return 0x00;
}

} // namespace vgmplay
