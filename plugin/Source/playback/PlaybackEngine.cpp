#include "PlaybackEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include "playback/BackendRegistry.h"

namespace vgmplay
{

PlaybackEngine::PlaybackEngine (void)
{
}

PlaybackEngine::~PlaybackEngine (void)
{
	const juce::ScopedLock sl (lock);
	resetState();
}

//==============================================================================
// message thread

void PlaybackEngine::prepare (double newSampleRate, int maxBlock)
{
	const juce::ScopedLock sl (lock);

	const double newRate = (newSampleRate > 0.0) ? newSampleRate : 44100.0;
	const int newBlockSize = juce::jmax (1, maxBlock);
	const bool settingsChanged = (newRate != sampleRate) || (newBlockSize != maxBlockSize);

	sampleRate = newRate;
	maxBlockSize = newBlockSize;

	// the scratch the mix renders each track into (allocated here, never on the audio thread)
	mixLeft.assign ((size_t) maxBlockSize, 0.0f);
	mixRight.assign ((size_t) maxBlockSize, 0.0f);

	if (! loaded.load() || ! settingsChanged)
	{
		return;
	}

	// The sound cores are created at the output sample rate, so a loaded song has to be
	// re-created when it changes - otherwise the emulation would keep running at the old rate
	// (wrong pitch and playback speed).
	juce::String errorMessage;
	rebuildPlayers (errorMessage);
	if (! errorMessage.isEmpty())
	{
		juce::Logger::writeToLog ("[VGMPlay] reload after sample rate change failed: " + errorMessage);
	}
}

bool PlaybackEngine::loadFile (const juce::File& file, juce::String& errorMessage)
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

	// the backend unpacks a compressed container (.vgz) itself - the image stays as it was read
	fileData.swapWith (data);
	currentFile = file;

	if (! createPlayers (errorMessage))
	{
		resetState();
		return false;
	}

	positionSeconds = 0.0;
	loaded = true;
	finished = false;
	playing = true;

	return true;
}

void PlaybackEngine::unloadFile (void)
{
	const juce::ScopedLock sl (lock);
	resetState();
}

void PlaybackEngine::setMixerEnabled (bool shouldBeEnabled)
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

void PlaybackEngine::play (void)
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

void PlaybackEngine::pause (void)
{
	playing = false;
}

void PlaybackEngine::stop (void)
{
	playing = false;
	finished = false;

	const juce::ScopedLock sl (lock);
	for (auto& player : trackPlayers)
	{
		player->seekSeconds (0.0);
	}
	positionSeconds = 0.0;
}

void PlaybackEngine::setPositionSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	if (trackPlayers.empty())
	{
		return;
	}

	const double duration = getPlayLengthSeconds();
	const double target = juce::jlimit (0.0, juce::jmax (0.0, duration), seconds);

	// every track seeks to the very same sample, which is what keeps them in sync
	for (auto& player : trackPlayers)
	{
		player->seekSeconds (target);
	}

	positionSeconds = target;
	finished = false;
}

void PlaybackEngine::setLoopCount (int loops)
{
	const juce::ScopedLock sl (lock);
	loopCount = loops;
	updatePlayerTimeline();
	refreshCachedTimes();	// the cached duration includes the loop repetitions
}

void PlaybackEngine::setFadeSeconds (double seconds)
{
	const juce::ScopedLock sl (lock);
	fadeSeconds = seconds;
	updatePlayerTimeline();
	refreshCachedTimes();	// the cached duration includes the fade time
}

void PlaybackEngine::setMasterVolume (float linear)
{
	// The output volume is applied by render() itself (the last gain stage), so no player has
	// to be touched and no lock is needed.
	masterVolume.store (juce::jmax (0.0f, linear));
}

void PlaybackEngine::setMixerMasterGain (float linear)
{
	// the mixer master is a mix-time gain like the track gains: one atomic, read by render()
	mixerMasterGain.store (juce::jmax (0.0f, linear));
}

void PlaybackEngine::setChipMuted (size_t chipIndex, bool shouldBeMuted)
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

	// The players only need to know which devices are audible - the backend decides what that
	// means for its own state (libvgm suspends the device, which also saves the CPU).
	applyChipMutes();
}

//==============================================================================
// mixer parameters (plain atomics - the audio thread reads them without the lock)

void PlaybackEngine::setTrackGain (size_t trackIndex, float linearGain)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->gain.store (juce::jmax (0.0f, linearGain));
	}
}

float PlaybackEngine::getTrackGain (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->gain.load() : 0.0f;
}

void PlaybackEngine::setTrackPan (size_t trackIndex, float pan)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->pan.store (juce::jlimit (-1.0f, 1.0f, pan));
	}
}

float PlaybackEngine::getTrackPan (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) ? tracks[trackIndex]->pan.load() : 0.0f;
}

void PlaybackEngine::setTrackMute (size_t trackIndex, bool shouldBeMuted)
{
	if (trackIndex < tracks.size())
	{
		tracks[trackIndex]->mute.store (shouldBeMuted);
	}
}

bool PlaybackEngine::isTrackMuted (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) && tracks[trackIndex]->mute.load();
}

void PlaybackEngine::setTrackSolo (size_t trackIndex, bool shouldBeSoloed)
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

bool PlaybackEngine::isTrackSoloed (size_t trackIndex) const
{
	return (trackIndex < tracks.size()) && tracks[trackIndex]->solo.load();
}

float PlaybackEngine::getTrackPeak (size_t trackIndex, int channel) const
{
	if (trackIndex >= tracks.size())
	{
		return 0.0f;
	}

	const TrackState& state = *tracks[trackIndex];
	return (channel == 1) ? state.peakR.load() : state.peakL.load();
}

float PlaybackEngine::getOutputPeak (int channel) const
{
	return (channel == 1) ? outputPeakR.load() : outputPeakL.load();
}

double PlaybackEngine::getTrackPositionSeconds (size_t trackIndex) const
{
	const juce::ScopedLock sl (lock);
	if (trackIndex >= trackPlayers.size())
	{
		return -1.0;
	}

	return trackPlayers[trackIndex]->positionSeconds();
}

//==============================================================================
// song facts

void PlaybackEngine::refreshCachedTimes (void)
{
	if (song == nullptr)
	{
		cachedDurationSeconds = 0.0;
		cachedLoopPointSeconds = -1.0;
		loopsForever = false;
		return;
	}

	const playback::SongInfo& info = song->getInfo();
	cachedDurationSeconds = info.durationSeconds;
	cachedLoopPointSeconds = info.loopPointSeconds;
	loopsForever = info.loopsForever;
}

double PlaybackEngine::getPlayLengthSeconds (void) const
{
	return (song != nullptr) ? song->getInfo().durationSeconds : 0.0;
}

juce::String PlaybackEngine::getTag (const juce::String& tagName) const
{
	// no lock: the map is only ever mutated from the message thread (load/reset) and the UI
	// polls it at 15 Hz - taking the render lock here would make the audio thread's try-lock
	// fail every now and then, dropping a whole block to silence
	auto it = tags.find (tagName);
	return (it == tags.end()) ? juce::String() : it->second;
}

void PlaybackEngine::enumerateTags (void)
{
	tags.clear();

	if (song != nullptr)
	{
		tags = song->getInfo().tags;
	}
}

void PlaybackEngine::enumerateChips (void)
{
	// keep the mute toggles when the same song is rebuilt (sample-rate change, mode switch)
	std::map<playback::DeviceId, bool> previousMutes;
	for (const auto& chip : chips)
	{
		previousMutes[chip.deviceId] = chip.muted;
	}

	chips.clear();

	if (song == nullptr)
	{
		return;
	}

	for (const auto& chipInfo : song->getInfo().chips)
	{
		ChipEntry chip;
		chip.name = chipInfo.name;
		chip.core = chipInfo.core;
		chip.deviceId = chipInfo.deviceId;

		const auto it = previousMutes.find (chip.deviceId);
		if (it != previousMutes.end())
		{
			chip.muted = it->second;
		}

		chips.push_back (std::move (chip));
	}
}

//==============================================================================
// player bookkeeping (the lock is held by the caller)

void PlaybackEngine::updatePlayerTimeline (void)
{
	if (song != nullptr)
	{
		song->setTimeline (loopCount, fadeSeconds);		// the facts follow immediately
	}
	for (auto& player : trackPlayers)
	{
		player->setTimeline (loopCount, fadeSeconds);
	}
}

void PlaybackEngine::applyChipMutes (void)
{
	for (auto& player : trackPlayers)
	{
		for (const auto& chip : chips)
		{
			player->setChipAudible (chip.deviceId, ! chip.muted);
		}
	}
}

void PlaybackEngine::applyTrackIsolation (void)
{
	if (! mixerEnabled)
	{
		return;
	}

	for (size_t t = 0; t < trackPlayers.size(); ++t)
	{
		if (t < trackLayout.size())
		{
			trackPlayers[t]->setTrackAudible (trackLayout[t].id, true);
		}
	}
}

bool PlaybackEngine::createPlayers (juce::String& errorMessage)
{
	destroyPlayers();

	playback::SongMemory image;
	image.data = fileData.getData();
	image.size = fileData.getSize();
	image.file = currentFile;

	// Compile-time registration (idempotent): the plugin ships its backends inside the same
	// binary, so there is nothing to load at runtime (ADR-008).
	playback::registerBuiltInBackends();
	auto* backend = playback::BackendRegistry::getInstance().findFor (image);
	if (backend == nullptr)
	{
		errorMessage = "Unsupported file format.";
		return false;
	}

	song = backend->open (image, errorMessage);
	if (song == nullptr)
	{
		return false;
	}

	if (! song->prepare (sampleRate, maxBlockSize, errorMessage))
	{
		song.reset();
		return false;
	}

	song->setTimeline (loopCount, fadeSeconds);

	enumerateTags();
	enumerateChips();
	refreshCachedTimes();

	if (trackLayout.empty())
	{
		// a new song: build the track list and one control state per track (a mode switch
		// keeps both - the layout of a song never changes)
		trackLayout = song->getTracks();

		tracks.clear();
		soloCount = 0;
		for (size_t i = 0; i < trackLayout.size(); ++i)
		{
			tracks.push_back (std::make_unique<TrackState>());
		}
	}

	// One player per track when the backend can isolate them, a single un-isolated one
	// otherwise (plain player mode always uses a single player).
	const playback::BackendCapabilities caps = song->getCapabilities();
	const bool perTrack = mixerEnabled
						   && caps.isolation == playback::IsolationMode::multiInstance
						   && ! trackLayout.empty();
	const size_t playersWanted = perTrack ? trackLayout.size() : 1;

	for (size_t i = 0; i < playersWanted; ++i)
	{
		auto player = song->createPlayer (errorMessage);
		if (player == nullptr)
		{
			destroyPlayers();
			song.reset();
			return false;
		}

		player->start();
		trackPlayers.push_back (std::move (player));
	}

	if (perTrack)
	{
		applyTrackIsolation();
	}
	applyChipMutes();

	return true;
}

void PlaybackEngine::destroyPlayers (void)
{
	trackPlayers.clear();
}

void PlaybackEngine::rebuildPlayers (juce::String& errorMessage)
{
	const bool wasPlaying = playing.load();
	const double position = getPositionSeconds();

	destroyPlayers();
	if (! createPlayers (errorMessage))
	{
		resetState();
		return;
	}

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

void PlaybackEngine::resetState (void)
{
	playing = false;
	finished = false;
	loaded = false;

	destroyPlayers();
	song.reset();
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

void PlaybackEngine::render (float* const* outputs, int numSamples)
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
	if (! playing.load() || ! loaded.load() || outputs[0] == nullptr || outputs[1] == nullptr)
	{
		return;
	}

	// The lock guards the players against the (rare, user-initiated) commands and against
	// (re)building them - the UI polling reads cached values instead, so this try-lock
	// succeeds while the song plays.
	const juce::ScopedTryLock stl (lock);
	if (! stl.isLocked() || trackPlayers.empty())
	{
		return;
	}

	const int usable = juce::jmin (numSamples, (int) mixLeft.size());
	if (usable <= 0)
	{
		return;
	}

	if (! mixerEnabled)
	{
		playback::ITrackPlayer& player = *trackPlayers[0];
		const int rendered = player.render (mixLeft.data(), mixRight.data(), usable);

		for (int i = 0; i < rendered; ++i)
		{
			outputs[0][i] = mixLeft[(size_t) i];
			outputs[1][i] = mixRight[(size_t) i];
		}
	}
	else
	{
		const bool anySolo = (soloCount.load() > 0);
		const size_t numTracks = juce::jmin (trackPlayers.size(), tracks.size());

		for (size_t t = 0; t < numTracks; ++t)
		{
			playback::ITrackPlayer& player = *trackPlayers[t];
			TrackState& state = *tracks[t];

			const int rendered = player.render (mixLeft.data(), mixRight.data(), usable);
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

			float peakL = 0.0f;
			float peakR = 0.0f;
			for (int i = 0; i < rendered; ++i)
			{
				const float l = mixLeft[(size_t) i] * gainL;
				const float r = mixRight[(size_t) i] * gainR;
				outputs[0][i] += l;
				outputs[1][i] += r;
				peakL = juce::jmax (peakL, std::abs (l));
				peakR = juce::jmax (peakR, std::abs (r));
			}
			state.peakL.store (peakL);
			state.peakR.store (peakR);
		}
	}

	// the mixer master (the master strip's fader): a mix-time gain on the finished sum
	const float mixerMaster = mixerMasterGain.load();
	if (mixerMaster != 1.0f)
	{
		juce::FloatVectorOperations::multiply (outputs[0], mixerMaster, numSamples);
		juce::FloatVectorOperations::multiply (outputs[1], mixerMaster, numSamples);
	}

	// the master meter: the level of the finished mix (both modes), read lock-free by the UI
	float peakL = 0.0f;
	float peakR = 0.0f;
	for (int i = 0; i < numSamples; ++i)
	{
		peakL = juce::jmax (peakL, std::abs (outputs[0][i]));
		peakR = juce::jmax (peakR, std::abs (outputs[1][i]));
	}
	outputPeakL.store (peakL);
	outputPeakR.store (peakR);

	// The output volume (the transport card's slider) is the very last stage - after the master
	// meter, so the mixer meters keep showing the mixer's own level while it is pulled down.
	const float outputVolume = masterVolume.load();
	if (outputVolume != 1.0f)
	{
		juce::FloatVectorOperations::multiply (outputs[0], outputVolume, numSamples);
		juce::FloatVectorOperations::multiply (outputs[1], outputVolume, numSamples);
	}

	// publish the position for the lock-free UI getter (an O(1) query, still under the lock);
	// the first player carries the song timeline in both modes
	const double position = trackPlayers[0]->positionSeconds();
	if (position >= 0.0)
	{
		positionSeconds.store (position);
	}

	if (trackPlayers[0]->isFinished())
	{
		finished = true;
		playing = false;	// the song is over -> silence
	}
}

} // namespace vgmplay
