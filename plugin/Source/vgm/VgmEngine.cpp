#include "VgmEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "emu/SoundDevs.h"	// for DEVID_*
#include "emu/SoundEmu.h"
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

	juce::String fccToString (UINT32 fcc)
	{
		char text[5] = { 0, 0, 0, 0, 0 };
		text[0] = (char) ((fcc >> 24) & 0xFF);
		text[1] = (char) ((fcc >> 16) & 0xFF);
		text[2] = (char) ((fcc >> 8) & 0xFF);
		text[3] = (char) (fcc & 0xFF);

		juce::String result;
		for (int i = 0; i < 4; i ++)
		{
			if (text[i] == 0)
			{
				break;
			}
			result += juce::String::charToString ((juce::juce_wchar) (unsigned char) text[i]);
		}
		return result.trim();
	}

	void logCallback (void*, PlayerBase* player, UINT8 level, UINT8 srcType, const char* srcTag, const char* message)
	{
		if (level > PLRLOG_WARN)
		{
			return;
		}
		const juce::String tag = (srcType == PLRLOGSRC_PLR) ? juce::String (player->GetPlayerName())
															: juce::String (srcTag);
		juce::Logger::writeToLog ("[VGMPlay/" + tag + "] " + juce::String (message).trim());
	}
}

VgmEngine::VgmEngine()
{
	player.RegisterPlayerEngine (new VGMPlayer);
	player.RegisterPlayerEngine (new S98Player);
	player.RegisterPlayerEngine (new DROPlayer);
	player.RegisterPlayerEngine (new GYMPlayer);
	player.SetEventCallback (eventCallback, this);
	player.SetLogCallback (logCallback, nullptr);
	renderBuffer.resize ((size_t) maxBlockSize);
}

VgmEngine::~VgmEngine()
{
	{
		const juce::ScopedLock sl (lock);
		resetState();
	}
	player.UnregisterAllPlayers();
}

//==============================================================================
// message thread API

void VgmEngine::prepare (double newSampleRate, int maxBlock)
{
	const juce::ScopedLock sl (lock);

	const double newRate = (newSampleRate > 0.0) ? newSampleRate : 44100.0;
	const int newBlockSize = juce::jmax (1, maxBlock);
	const bool settingsChanged = (newRate != sampleRate) || (newBlockSize != maxBlockSize);

	sampleRate = newRate;
	maxBlockSize = newBlockSize;
	renderBuffer.assign ((size_t) maxBlockSize, WAVE_32BS());

	if (loaded.load() && settingsChanged)
	{
		// The sound cores are created at the output sample rate, and PlayerA::SetSampleRate()
		// deliberately skips the player that is currently playing - so a loaded song has to be
		// re-loaded, otherwise the emulation would keep running at the old rate (wrong pitch
		// and playback speed).
		const juce::File file = currentFile;
		const bool wasPlaying = playing.load();
		const double position = wasPlaying ? getPositionSeconds() : 0.0;

		resetState();
		sampleRate = newRate;
		maxBlockSize = newBlockSize;

		if (file.existsAsFile())
		{
			juce::String errorMessage;
			if (loadFile (file, errorMessage))
			{
				playing = wasPlaying;
				if (wasPlaying)
				{
					setPositionSeconds (position);
				}
			}
			else
			{
				juce::Logger::writeToLog ("[VGMPlay] reload after sample rate change failed: " + errorMessage);
			}
		}
		return;
	}

	applyOutputSettings();
	updatePlayerConfig();
}

bool VgmEngine::loadFile (const juce::File& file, juce::String& errorMessage)
{
	// The lock is held for the whole operation: the audio thread will output silence
	// (it uses a try-lock) while a file is being loaded.
	const juce::ScopedLock sl (lock);

	resetState();	// stop playback and release the previous file first

	if (! file.existsAsFile())
	{
		errorMessage = "The file does not exist.";
		return false;
	}

	juce::MemoryBlock data;
	if (! file.loadFileAsData (data) || data.getSize() == 0)
	{
		errorMessage = "Could not read the file.";
		return false;
	}

	fileData.swapWith (data);
	currentFile = file;

	// MemoryLoader handles plain and gzip-compressed (.vgz) files
	dataLoader = MemoryLoader_Init ((const UINT8*) fileData.getData(), (UINT32) fileData.getSize());
	if (dataLoader == NULL)
	{
		errorMessage = "Out of memory.";
		resetState();
		return false;
	}

	UINT8 retVal = DataLoader_Load (dataLoader);
	if (retVal)
	{
		errorMessage = juce::String::formatted ("Could not load the file (error 0x%02X).", (unsigned) retVal);
		resetState();
		return false;
	}

	applyOutputSettings();
	retVal = player.LoadFile (dataLoader);
	if (retVal)
	{
		errorMessage = juce::String::formatted ("Unsupported file format (error 0x%02X).", (unsigned) retVal);
		resetState();
		return false;
	}

	enumerateTags();
	updatePlayerConfig();
	player.Start();			// the sound cores only exist after starting playback
	enumerateChips();

	loaded = true;
	finished = false;
	playing = true;

	return true;
}

void VgmEngine::unloadFile (void)
{
	const juce::ScopedLock sl (lock);
	resetState();
}

void VgmEngine::resetState (void)
{
	playing = false;
	finished = false;
	loaded = false;

	if (player.GetPlayer() != NULL)
	{
		player.UnloadFile();
	}
	if (dataLoader != NULL)
	{
		DataLoader_Deinit (dataLoader);
		dataLoader = NULL;
	}
	fileData.reset();
	currentFile = juce::File();
	chips.clear();
	tags.clear();
}

void VgmEngine::applyOutputSettings (void)
{
	// 32-bit integer stereo output (libvgm's highest precision integer format)
	player.SetOutputSettings ((UINT32) sampleRate, 2, 32, (UINT32) maxBlockSize);
}

void VgmEngine::updatePlayerConfig (void)
{
	PlayerA::Config cfg = player.GetConfiguration();

	cfg.masterVol = (INT32) (masterVolume * 0x10000);
	cfg.loopCount = (UINT32) juce::jmax (0, loopCount);
	cfg.fadeSmpls = (UINT32) (juce::jmax (0.0, fadeSeconds) * sampleRate);
	cfg.endSilenceSmpls = 0;	// a plugin should not render trailing silence
	cfg.pbSpeed = 1.0;

	player.SetConfiguration (cfg);

	// VGM files without a loop point would otherwise be faded out immediately
	PlayerBase* pBase = player.GetPlayer();
	if (pBase != NULL && pBase->GetPlayerType() == FCC_VGM)
	{
		VGMPlayer* vgmPlayer = dynamic_cast<VGMPlayer*> (pBase);
		if (vgmPlayer != NULL)
		{
			player.SetLoopCount (vgmPlayer->GetModifiedLoopCount ((UINT32) juce::jmax (0, loopCount)));
		}
	}
}

void VgmEngine::play (void)
{
	if (! loaded.load())
	{
		return;
	}
	if (finished.load())
	{
		setPositionSeconds (0.0);	// restart a finished song
	}
	playing = true;
}

void VgmEngine::pause (void)
{
	playing = false;
}

void VgmEngine::stop (void)
{
	playing = false;
	finished = false;

	const juce::ScopedLock sl (lock);
	if (player.GetPlayer() != NULL)
	{
		player.Seek (PLAYPOS_SAMPLE, 0);
	}
}

void VgmEngine::setPositionSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	if (player.GetPlayer() == NULL)
	{
		return;
	}

	const double duration = player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
	const double target = juce::jlimit (0.0, juce::jmax (0.0, duration), seconds);

	player.Seek (PLAYPOS_SAMPLE, (UINT32) (target * sampleRate));
	finished = false;
}

void VgmEngine::setLoopCount (int loops)
{
	const juce::ScopedLock sl (lock);
	loopCount = loops;
	updatePlayerConfig();
}

void VgmEngine::setFadeSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	fadeSeconds = seconds;
	updatePlayerConfig();
}

void VgmEngine::setMasterVolume (float linear)
{
	const juce::ScopedLock sl (lock);
	masterVolume = juce::jmax (0.0f, linear);
	player.SetMasterVolume ((INT32) (masterVolume * 0x10000));
}

void VgmEngine::setChipMuted (size_t chipIndex, bool shouldBeMuted)
{
	const juce::ScopedLock sl (lock);
	if (chipIndex >= chips.size())
	{
		return;
	}

	ChipEntry& chip = chips[chipIndex];
	chip.muted = shouldBeMuted;

	PlayerBase* pBase = player.GetPlayer();
	if (pBase == NULL)
	{
		return;
	}

	PLR_MUTE_OPTS muteOpts = {};
	muteOpts.disable = shouldBeMuted ? 0x03 : 0x00;	// 0x03 = main + linked devices
	for (size_t i = 0; i < 2; i ++)
	{
		muteOpts.chnMute[i] = 0x00;
	}
	pBase->SetDeviceMuting (chip.devID, muteOpts);
}

double VgmEngine::getPositionSeconds (void) const
{
	const juce::ScopedLock sl (lock);
	if (player.GetPlayer() == NULL)
	{
		return 0.0;
	}
	return player.GetCurTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
}

double VgmEngine::getDurationSeconds (void) const
{
	const juce::ScopedLock sl (lock);
	if (player.GetPlayer() == NULL)
	{
		return 0.0;
	}
	return player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
}

juce::String VgmEngine::getTag (const juce::String& tagName) const
{
	const juce::ScopedLock sl (lock);
	auto it = tags.find (tagName);
	return (it == tags.end()) ? juce::String() : it->second;
}

void VgmEngine::enumerateTags (void)
{
	tags.clear();

	PlayerBase* pBase = player.GetPlayer();
	if (pBase == NULL)
	{
		return;
	}

	const char* const* tagList = pBase->GetTags();
	if (tagList == NULL)
	{
		return;
	}

	for (const char* const* tag = tagList; *tag != NULL; tag += 2)
	{
		if (tag[1] == NULL || tag[1][0] == '\0')
		{
			continue;
		}
		tags[juce::String (tag[0])] = juce::String::fromUTF8 (tag[1]);
	}

	// prefer the untranslated tags over the Japanese ones
	for (auto it = tags.begin(); it != tags.end();)
	{
		if (it->first.endsWith ("-JPN") && tags.count (it->first.dropLastCharacters (4)) > 0)
		{
			it = tags.erase (it);
		}
		else
		{
			++it;
		}
	}
}

void VgmEngine::enumerateChips (void)
{
	chips.clear();

	PlayerBase* pBase = player.GetPlayer();
	if (pBase == NULL)
	{
		return;
	}

	std::vector<PLR_DEV_INFO> devList;
	pBase->GetSongDeviceInfo (devList);

	for (const PLR_DEV_INFO& info : devList)
	{
		if (info.parentIdx != (UINT32) -1)
		{
			continue;	// linked devices are an implementation detail
		}
		if (info.type == DEVID_SN76496 && (info.devCfg->flags & 0x01) && (info.instance & 0x01))
		{
			continue;	// second half of a T6W28
		}

		ChipEntry chip;
		const char* chipName = SndEmu_GetDevName (info.type, 0x01, info.devCfg);
		chip.name = juce::String (chipName != NULL ? chipName : "?");
		chip.core = fccToString (info.core);
		chip.devID = info.id;
		chips.push_back (chip);
	}
}

//==============================================================================
// audio thread API

void VgmEngine::render (float* const* outputs, int numSamples)
{
	const juce::ScopedTryLock stl (lock);

	if (! stl.isLocked() || ! playing.load() || ! loaded.load())
	{
		for (int ch = 0; ch < 2; ++ch)
		{
			if (outputs[ch] != nullptr)
			{
				juce::FloatVectorOperations::clear (outputs[ch], numSamples);
			}
		}
		return;
	}

	const int samplesToRender = juce::jmin (numSamples, (int) renderBuffer.size());
	// note: PlayerA::Render() returns the number of *bytes* that were written
	const UINT32 renderedBytes = player.Render ((UINT32) samplesToRender * (UINT32) kBytesPerSample,
												renderBuffer.data());
	const int outSamples = juce::jmin ((int) (renderedBytes / (UINT32) kBytesPerSample), samplesToRender);

	for (int i = 0; i < outSamples; ++i)
	{
		outputs[0][i] = (float) renderBuffer[(size_t) i].L * kInt32ToFloat;
		outputs[1][i] = (float) renderBuffer[(size_t) i].R * kInt32ToFloat;
	}
	if (outSamples < numSamples)
	{
		const int tail = numSamples - outSamples;
		juce::FloatVectorOperations::clear (outputs[0] + outSamples, tail);
		juce::FloatVectorOperations::clear (outputs[1] + outSamples, tail);
	}

	if (finished.load())
	{
		playing = false;	// the song is over -> silence
	}
}

UINT8 VgmEngine::eventCallback (PlayerBase* /*player*/, void* userParam, UINT8 evtType, void* evtParam)
{
	VgmEngine* engine = static_cast<VgmEngine*> (userParam);
	return (engine != nullptr) ? engine->handleEvent (evtType, evtParam) : 0x00;
}

UINT8 VgmEngine::handleEvent (UINT8 evtType, void* /*evtParam*/)
{
	if (evtType == PLREVT_END)
	{
		finished = true;	// the audio thread stops rendering after this block
	}
	return 0x00;
}

} // namespace vgmplay
