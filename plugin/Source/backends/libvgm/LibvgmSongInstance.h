#pragma once

#include <memory>
#include <vector>

#include <juce_core/juce_core.h>

#include "playback/SongBackend.h"

#include "LibvgmTrackPlayer.h"

namespace vgmplay::backends::libvgm
{

/**
	One open libvgm song - the adapter's ISongInstance.

	It owns a *probe* player used for everything the core asks about the song (tags, devices,
	tracks, timeline facts) and as the factory for the players the mixer renders with. The
	probe is never rendered, so it costs no CPU - it only holds one player state (and it is how
	the mixer keeps its "one player per track, all over the same image" model).

	All of this is message-thread work; the object is created and destroyed while the engine
	holds its lock.
*/
class LibvgmSongInstance final : public playback::ISongInstance
{
public:
	explicit LibvgmSongInstance (const playback::SongMemory& image);

	/** Unpacks a compressed image (.vgz) if needed - call right after construction. */
	bool unpackImage (juce::String& errorMessage);

	// --- playback::ISongInstance ----------------------------------------------------
	bool prepare (double hostSampleRate, int maxBlockSize, juce::String& errorMessage) override;
	const playback::SongInfo& getInfo (void) const override { return info; }
	const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks; }
	playback::BackendCapabilities getCapabilities (void) const override;
	void setTimeline (int loopCount, double fadeSeconds) override;
	std::unique_ptr<playback::ITrackPlayer> createPlayer (juce::String& errorMessage) override;

private:
	void enumerate (void);
	void refreshTimelineFacts (void);
	void applyTimelineTo (LibvgmTrackPlayer& player) const;

	playback::SongMemory memory;
	juce::MemoryBlock ownedImage;			// only filled when the image had to be unpacked
	playback::SongMemory playerMemory;		// the bytes every player loads (owned or the core's)

	std::unique_ptr<LibvgmTrackPlayer> probe;
	double sampleRate = 44100.0;
	int maxBlockSize = 512;
	int loopCount = 1;
	double fadeSeconds = 0.0;

	playback::SongInfo info;
	std::vector<playback::TrackInfo> tracks;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LibvgmSongInstance)
};

} // namespace vgmplay::backends::libvgm
