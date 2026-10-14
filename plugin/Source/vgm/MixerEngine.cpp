#include "MixerEngine.h"

#include <cmath>

#include <juce_audio_basics/juce_audio_basics.h>

#include "emu/SoundDevs.h"	// for DEVID_*
#include "emu/SoundEmu.h"
#include "player/vgmplayer.hpp"
#include "utils/MemoryLoader.h"

namespace vgmplay
{

namespace
{
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

	/** .vgz is a gzip wrapper around a plain VGM image. Inflate it once with libvgm's own
		MemoryLoader, so that every track's DataLoader works on the same plain bytes instead of
		holding a decompressed copy of its own - a mixer track list easily reaches a dozen
		players. Plain files pass through unchanged. */
	bool flattenCompressedImage (juce::MemoryBlock& data)
	{
		const auto* raw = static_cast<const unsigned char*> (data.getData());
		if (data.getSize() < 2 || raw[0] != 0x1f || raw[1] != 0x8b)
		{
			return true;
		}

		DATA_LOADER* loader = MemoryLoader_Init (raw, (UINT32) data.getSize());
		if (loader == NULL)
		{
			return false;
		}

		bool success = false;
		if (DataLoader_Load (loader) == 0x00)
		{
			const UINT32 plainSize = DataLoader_GetSize (loader);
			const UINT8* plainData = DataLoader_GetData (loader);
			if (plainData != nullptr && plainSize > 0)
			{
				juce::MemoryBlock plain (plainData, (size_t) plainSize);
				data.swapWith (plain);
				success = true;
			}
		}
		DataLoader_Deinit (loader);
		return success;
	}
}

MixerEngine::MixerEngine (void)
{
}

MixerEngine::~MixerEngine (void)
{
	const juce::ScopedLock sl (lock);
	resetState();
}

//==============================================================================
// message thread

void MixerEngine::prepare (double newSampleRate, int maxBlock)
{
	const juce::ScopedLock sl (lock);

	const double newRate = (newSampleRate > 0.0) ? newSampleRate : 44100.0;
	const int newBlockSize = juce::jmax (1, maxBlock);
	const bool settingsChanged = (newRate != sampleRate) || (newBlockSize != maxBlockSize);

	sampleRate = newRate;
	maxBlockSize = newBlockSize;

	if (! loaded.load() || ! settingsChanged)
	{
		return;
	}

	// The sound cores are created at the output sample rate, and PlayerA::SetSampleRate()
	// deliberately skips the player that is currently playing - so a loaded song has to be
	// re-created, otherwise the emulation would keep running at the old rate (wrong pitch
	// and playback speed).
	juce::String errorMessage;
	rebuildPlayers (errorMessage);
	if (! errorMessage.isEmpty())
	{
		juce::Logger::writeToLog ("[VGMPlay] reload after sample rate change failed: " + errorMessage);
	}
}

bool MixerEngine::loadFile (const juce::File& file, juce::String& errorMessage)
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

	if (! flattenCompressedImage (data))
	{
		errorMessage = "Could not read the file.";
		return false;
	}

	fileData.swapWith (data);
	currentFile = file;

	if (! createPlayers (errorMessage))
	{
		resetState();
		return false;
	}

	refreshCachedTimes();
	positionSeconds = 0.0;
	loaded = true;
	finished = false;
	playing = true;

	return true;
}

void MixerEngine::unloadFile (void)
{
	const juce::ScopedLock sl (lock);
	resetState();
}

void MixerEngine::setMixerEnabled (bool shouldBeEnabled)
{
	const juce::ScopedLock sl (lock);
	if (shouldBeEnabled == mixerEnabled)
	{
		return;
	}

	mixerEnabled = shouldBeEnabled;
	if (! loaded.load())
	{
		return;
	}

	// rebuild the players for the new mode; the track settings stay, they belong to the layout
	juce::String errorMessage;
	rebuildPlayers (errorMessage);
	if (! errorMessage.isEmpty())
	{
		juce::Logger::writeToLog ("[VGMPlay] switching the mixer failed: " + errorMessage);
	}
}

void MixerEngine::play (void)
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

void MixerEngine::pause (void)
{
	playing = false;
}

void MixerEngine::stop (void)
{
	playing = false;
	finished = false;

	const juce::ScopedLock sl (lock);
	for (auto& track : trackPlayers)
	{
		if (track->player().GetPlayer() != NULL)
		{
			track->player().Seek (PLAYPOS_SAMPLE, 0);
		}
		track->clearFinished();
	}
	positionSeconds = 0.0;
}

void MixerEngine::setPositionSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	if (trackPlayers.empty() || trackPlayers[0]->player().GetPlayer() == NULL)
	{
		return;
	}

	const double duration = getPlayLengthSeconds();
	const double target = juce::jlimit (0.0, juce::jmax (0.0, duration), seconds);

	// every track seeks to the very same sample, which is what keeps them in sync
	for (auto& track : trackPlayers)
	{
		track->player().Seek (PLAYPOS_SAMPLE, (UINT32) (target * sampleRate));
		track->clearFinished();
	}

	positionSeconds = target;
	finished = false;
}

void MixerEngine::setLoopCount (int loops)
{
	const juce::ScopedLock sl (lock);
	loopCount = loops;
	updatePlayerConfig();
	refreshCachedTimes();	// the cached duration includes the loop repetitions
}

void MixerEngine::setFadeSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	fadeSeconds = seconds;
	updatePlayerConfig();
	refreshCachedTimes();	// the cached duration includes the fade time
}

void MixerEngine::setMasterVolume (float linear)
{
	const juce::ScopedLock sl (lock);
	masterVolume = juce::jmax (0.0f, linear);

	for (auto& track : trackPlayers)
	{
		track->player().SetMasterVolume ((INT32) (masterVolume * 0x10000));
	}
}

void MixerEngine::setChipMuted (size_t chipIndex, bool shouldBeMuted)
{
	const juce::ScopedLock sl (lock);
	if (chipIndex >= chips.size())
	{
		return;
	}

	ChipEntry& chip = chips[chipIndex];
	chip.muted = shouldBeMuted;

	if (trackPlayers.empty())
	{
		return;
	}

	if (mixerEnabled)
	{
		applyTrackIsolation();	// the device is suspended on every track
		return;
	}

	// plain player: the single instance mutes the whole device
	PLR_MUTE_OPTS muteOpts = {};
	muteOpts.disable = shouldBeMuted ? 0x03 : 0x00;	// 0x03 = main + linked devices
	for (size_t i = 0; i < 2; i ++)
	{
		muteOpts.chnMute[i] = 0x00;
	}
	trackPlayers[0]->applyMuting (chip.devID, muteOpts);
}

//==============================================================================
// mixer parameters (plain atomics - the audio thread reads them without the lock)

void MixerEngine::setTrackGain (size_t trackIndex, float linearGain)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->gain.store (juce::jmax (0.0f, linearGain));
	}
}

float MixerEngine::getTrackGain (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->gain.load() : 0.0f;
}

void MixerEngine::setTrackPan (size_t trackIndex, float pan)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->pan.store (juce::jlimit (-1.0f, 1.0f, pan));
	}
}

float MixerEngine::getTrackPan (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->pan.load() : 0.0f;
}

void MixerEngine::setTrackMute (size_t trackIndex, bool shouldBeMuted)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->mute.store (shouldBeMuted);
	}
}

bool MixerEngine::isTrackMuted (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) && tracks[trackIndex]->mute.load();
}

void MixerEngine::setTrackSolo (size_t trackIndex, bool shouldBeSoloed)
{
	if (trackIndex >= tracks.size())
	{
		return;
	}

	TrackState& state = *tracks[trackIndex];
	if (state.solo.load() == shouldBeSoloed)
	{
		return;
	}

	state.solo.store (shouldBeSoloed);
	soloCount.fetch_add (shouldBeSoloed ? 1 : -1);
}

bool MixerEngine::isTrackSoloed (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) && tracks[trackIndex]->solo.load();
}

float MixerEngine::getTrackPeak (size_t trackIndex, int channel) const
{
	if (trackIndex >= tracks.size())
	{
		return 0.0f;
	}

	const TrackState& state = *tracks[trackIndex];
	return (channel == 1) ? state.peakR.load() : state.peakL.load();
}

double MixerEngine::getTrackPositionSeconds (size_t trackIndex) const
{
	const juce::ScopedLock sl (lock);
	if (trackIndex >= trackPlayers.size())
	{
		return -1.0;
	}

	PlayerA& player = trackPlayers[trackIndex]->player();
	if (player.GetPlayer() == NULL)
	{
		return -1.0;
	}

	return player.GetCurTime (loopsForever ? PLAYTIME_TIME_PBK
										   : (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK));
}

//==============================================================================
// song facts

void MixerEngine::refreshCachedTimes (void)
{
	if (trackPlayers.empty() || trackPlayers[0]->player().GetPlayer() == NULL)
	{
		cachedDurationSeconds = 0.0;
		cachedLoopPointSeconds = -1.0;
		loopsForever = false;
		return;
	}

	PlayerA& player = trackPlayers[0]->player();
	cachedDurationSeconds = getPlayLengthSeconds();

	// While looping forever the absolute position keeps growing, so report the position that
	// jumps back to the loop start on every repetition - the same mode getPlayLengthSeconds()
	// measures the duration in.
	loopsForever = (player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK) < 0.0);

	// GetLoopTime() is the *length* of the loop, so the loop point is the length of a single
	// playthrough minus that length - the position the seek bar jumps back to on every repetition.
	const double loopLength = player.GetLoopTime();
	cachedLoopPointSeconds = (loopLength > 0.0) ? juce::jmax (0.0, cachedDurationSeconds - loopLength)
												: -1.0;
}

double MixerEngine::getPlayLengthSeconds (void) const
{
	if (trackPlayers.empty() || trackPlayers[0]->player().GetPlayer() == NULL)
	{
		return 0.0;
	}

	PlayerA& player = trackPlayers[0]->player();
	const double total = player.GetTotalTime (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK);
	if (total > 0.0)
	{
		return total;
	}

	// "loop forever": PlayerA::GetTotalTime() returns a negative value to indicate an infinite
	// runtime (see its comment "indicates infinite runtime"), which would make the seek bar
	// unwritable - the position would be clamped to 0 on every refresh. Use the length of a
	// single playthrough instead and let the position jump back at every repetition.
	return juce::jmax (0.0, player.GetTotalTime (PLAYTIME_TIME_PBK));
}

juce::String MixerEngine::getTag (const juce::String& tagName) const
{
	// no lock: the map is only ever mutated from the message thread (load/reset) and the UI
	// polls it at 20 Hz - taking the render lock here would make the audio thread's try-lock
	// fail every now and then, dropping a whole block to silence
	auto it = tags.find (tagName);
	return (it == tags.end()) ? juce::String() : it->second;
}

void MixerEngine::enumerateTags (void)
{
	tags.clear();

	if (trackPlayers.empty())
	{
		return;
	}

	PlayerBase* pBase = trackPlayers[0]->player().GetPlayer();
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

void MixerEngine::enumerateChips (void)
{
	// keep the mute toggles when the same song is rebuilt (sample-rate change, mode switch)
	std::map<UINT32, bool> previousMutes;
	for (const auto& chip : chips)
	{
		previousMutes[chip.devID] = chip.muted;
	}

	chips.clear();

	if (trackPlayers.empty())
	{
		return;
	}

	PlayerBase* pBase = trackPlayers[0]->player().GetPlayer();
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

	for (auto& chip : chips)
	{
		const auto it = previousMutes.find (chip.devID);
		if (it != previousMutes.end())
		{
			chip.muted = it->second;
		}
	}
}

//==============================================================================
// player bookkeeping (the lock is held by the caller)

void MixerEngine::applyConfigToPlayer (TrackPlayer& track)
{
	PlayerA::Config cfg = track.player().GetConfiguration();

	cfg.masterVol = (INT32) (masterVolume * 0x10000);
	cfg.loopCount = (UINT32) juce::jmax (0, loopCount);
	cfg.fadeSmpls = (UINT32) (juce::jmax (0.0, fadeSeconds) * sampleRate);
	cfg.endSilenceSmpls = 0;	// a plugin should not render trailing silence
	cfg.pbSpeed = 1.0;

	track.player().SetConfiguration (cfg);

	// VGM files without a loop point would otherwise be faded out immediately
	PlayerBase* pBase = track.player().GetPlayer();
	if (pBase != NULL && pBase->GetPlayerType() == FCC_VGM)
	{
		VGMPlayer* vgmPlayer = dynamic_cast<VGMPlayer*> (pBase);
		if (vgmPlayer != NULL)
		{
			track.player().SetLoopCount (vgmPlayer->GetModifiedLoopCount ((UINT32) juce::jmax (0, loopCount)));
		}
	}
}

void MixerEngine::updatePlayerConfig (void)
{
	for (auto& track : trackPlayers)
	{
		applyConfigToPlayer (*track);
	}
}

void MixerEngine::applyTrackIsolation (void)
{
	if (! mixerEnabled)
	{
		return;
	}

	const size_t numTracks = juce::jmin (trackPlayers.size(), trackLayout.size());
	for (size_t t = 0; t < numTracks; ++t)
	{
		TrackPlayer& player = *trackPlayers[t];
		const TrackLayoutEntry& entry = trackLayout[t];

		PlayerBase* pBase = player.player().GetPlayer();
		if (pBase == NULL)
		{
			continue;
		}

		std::vector<PLR_DEV_INFO> devices;
		pBase->GetSongDeviceInfo (devices);

		// every device is suspended (`disable` skips the emulation, which really saves the CPU)
		// and has all of its channels muted - only the track's own device is woken up below
		PLR_MUTE_OPTS suspend = {};
		suspend.disable = 0xFF;
		for (size_t i = 0; i < 2; i ++)
		{
			suspend.chnMute[i] = 0xFFFFFFFF;
		}

		for (const auto& info : devices)
		{
			if (info.parentIdx != (UINT32) -1)
			{
				continue;	// linked devices are configured through their parent
			}

			if (info.id != entry.deviceID)
			{
				player.applyMuting (info.id, suspend);
				continue;
			}

			// the track's device: every channel muted except the track's own one
			PLR_MUTE_OPTS isolate = {};
			isolate.disable = 0x00;
			for (int half = 0; half < 2; ++half)
			{
				UINT32 mask = 0xFFFFFFFF;
				if (channelMuteHalf (entry.channelIndex) == half)
				{
					mask &= ~channelMuteBit (entry.channelIndex);
				}
				isolate.chnMute[half] = mask;
			}

			// muted through the chips card -> suspend the whole device as well
			for (const auto& chip : chips)
			{
				if (chip.devID == info.id && chip.muted)
				{
					isolate.disable = 0x01;
					break;
				}
			}

			player.applyMuting (info.id, isolate);
		}
	}
}

bool MixerEngine::createPlayers (juce::String& errorMessage)
{
	destroyPlayers();

	// The first player always exists: it is the only one in plain player mode, and it also
	// carries the song metadata (tags, device list, track layout) in mixer mode.
	auto first = std::make_unique<TrackPlayer>();
	first->prepare (sampleRate, maxBlockSize);
	if (! first->loadFile (fileData, errorMessage))
	{
		destroyPlayers();
		return false;
	}
	applyConfigToPlayer (*first);
	first->start();
	trackPlayers.push_back (std::move (first));

	enumerateTags();
	enumerateChips();

	if (trackLayout.empty())
	{
		// a new song: build the track list and one control state per track (a mode switch
		// keeps both - the layout of a song never changes)
		PlayerBase* pBase = trackPlayers[0]->player().GetPlayer();
		if (pBase != NULL)
		{
			trackLayout = buildTrackLayout (*pBase);
		}

		tracks.clear();
		soloCount = 0;
		for (size_t i = 0; i < trackLayout.size(); ++i)
		{
			tracks.push_back (std::make_unique<TrackState>());
		}
	}

	if (mixerEnabled)
	{
		for (size_t i = 1; i < trackLayout.size(); ++i)
		{
			auto track = std::make_unique<TrackPlayer>();
			track->prepare (sampleRate, maxBlockSize);
			if (! track->loadFile (fileData, errorMessage))
			{
				destroyPlayers();
				return false;
			}
			applyConfigToPlayer (*track);
			track->start();
			trackPlayers.push_back (std::move (track));
		}
	}

	applyTrackIsolation();
	return true;
}

void MixerEngine::destroyPlayers (void)
{
	trackPlayers.clear();
}

void MixerEngine::rebuildPlayers (juce::String& errorMessage)
{
	const bool wasPlaying = playing.load();
	const double position = getPositionSeconds();

	destroyPlayers();
	if (! createPlayers (errorMessage))
	{
		resetState();
		return;
	}

	refreshCachedTimes();
	positionSeconds = 0.0;
	finished = false;
	playing = true;
	if (position > 0.0)
	{
		setPositionSeconds (position);
	}
	if (! wasPlaying)
	{
		playing = false;	// stay paused where we were
	}
}

void MixerEngine::resetState (void)
{
	playing = false;
	finished = false;
	loaded = false;

	destroyPlayers();
	trackLayout.clear();
	tracks.clear();
	soloCount = 0;

	fileData.reset();
	currentFile = juce::File();
	chips.clear();
	tags.clear();

	positionSeconds = 0.0;
	cachedDurationSeconds = 0.0;
	cachedLoopPointSeconds = -1.0;
	loopsForever = false;
}

//==============================================================================
// audio thread

void MixerEngine::render (float* const* outputs, int numSamples)
{
	// clear first: the early exits and the mix path both leave the rest of the block silent
	for (int ch = 0; ch < 2; ++ch)
	{
		if (outputs[ch] != nullptr)
		{
			juce::FloatVectorOperations::clear (outputs[ch], numSamples);
		}
	}

	// check the cheap atomics first: while paused or idle the audio thread must not touch
	// the lock at all (a failed try-lock costs a whole silent block)
	if (! playing.load() || ! loaded.load())
	{
		return;
	}

	// The lock guards the player objects against the (rare, user-initiated) commands and
	// against (re)building the players - the 20 Hz UI polling reads cached values instead,
	// so this try-lock succeeds while the song plays.
	const juce::ScopedTryLock stl (lock);
	if (! stl.isLocked() || trackPlayers.empty())
	{
		return;
	}

	if (! mixerEnabled)
	{
		TrackPlayer& track = *trackPlayers[0];
		track.render (numSamples);

		const int rendered = track.getRenderedSamples();
		const float* srcL = track.getLeft();
		const float* srcR = track.getRight();
		for (int i = 0; i < rendered; ++i)
		{
			outputs[0][i] = srcL[i];
			outputs[1][i] = srcR[i];
		}
	}
	else
	{
		const bool anySolo = (soloCount.load() > 0);
		const size_t numTracks = juce::jmin (trackPlayers.size(), tracks.size());

		for (size_t t = 0; t < numTracks; ++t)
		{
			TrackPlayer& player = *trackPlayers[t];
			TrackState& state = *tracks[t];

			player.render (numSamples);
			const int rendered = player.getRenderedSamples();
			if (rendered <= 0)
			{
				state.peakL.store (0.0f);
				state.peakR.store (0.0f);
				continue;
			}

			const float gain = (state.mute.load() || (anySolo && ! state.solo.load()))
								 ? 0.0f : state.gain.load();
			const float pan = juce::jlimit (-1.0f, 1.0f, state.pan.load());
			// balance law: the centre keeps both sides at unity, panning attenuates the far side
			const float gainL = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
			const float gainR = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);

			const float* srcL = player.getLeft();
			const float* srcR = player.getRight();
			float peakL = 0.0f;
			float peakR = 0.0f;
			for (int i = 0; i < rendered; ++i)
			{
				const float l = srcL[i] * gainL;
				const float r = srcR[i] * gainR;
				outputs[0][i] += l;
				outputs[1][i] += r;
				peakL = juce::jmax (peakL, std::abs (l));
				peakR = juce::jmax (peakR, std::abs (r));
			}
			state.peakL.store (peakL);
			state.peakR.store (peakR);
		}
	}

	// publish the position for the lock-free UI getter (an O(1) query, still under the lock);
	// track 0 carries the song timeline in both modes
	PlayerA& primary = trackPlayers[0]->player();
	if (primary.GetPlayer() != NULL)
	{
		const double position = primary.GetCurTime (loopsForever ? PLAYTIME_TIME_PBK
																 : (PLAYTIME_LOOP_INCL | PLAYTIME_TIME_PBK));
		if (position >= 0.0)
		{
			positionSeconds.store (position);
		}
	}

	if (trackPlayers[0]->hasFinished())
	{
		finished = true;
		playing = false;	// the song is over -> silence
	}
}

} // namespace vgmplay
