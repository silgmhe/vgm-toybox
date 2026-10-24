#include "backends/nsf/NsfBackend.h"

#include <cstring>
#include <memory>
#include <vector>

#include "backends/nsf/NsfPlayer.h"
#include "formats/nsf/NsfFile.h"

namespace toybox::backends::nsf
{

namespace
{

/** One rendered instance of the song. The emulation is the player's; this only adapts it. */
class NsfTrackPlayer final : public playback::ITrackPlayer
{
public:
	NsfTrackPlayer (const formats::nsf::NsfFile& file, double sampleRate)
		: player (file, sampleRate)
	{
		player.start ();
	}

	void start (void) override {}
	void pause (void) override {}
	bool seekSeconds (double) override { return false; }
	void setTimeline (int, double) override {}
	void setTrackAudible (playback::TrackId track, bool audible) override
	{
		if (audible && track >= 1 && track <= 5)
		{
			player.setTrackAudible ((int) track - 1);
		}
	}
	void setChipAudible (playback::DeviceId, bool) override {}

	int render (float* left, float* right, int numSamples) override
	{
		return player.render (left, right, numSamples);
	}

	double positionSeconds (void) override { return player.positionSeconds (); }

	bool isFinished (void) const override
	{
		// An NSF has no end: the driver loops until it is stopped.
		return false;
	}

private:
	NsfPlayer player;
};

class NsfSongInstance final : public playback::ISongInstance
{
public:
	NsfSongInstance (formats::nsf::NsfFile image, std::vector<char> bytes)
		: file (std::move (image)), image (std::move (bytes))
	{
		info_.formatName = "NSF (NSFPlay core)";

		if (! file.name.empty ()) { info_.tags["title"] = file.name.c_str (); }
		if (! file.artist.empty ()) { info_.tags["artist"] = file.artist.c_str (); }
		if (! file.copyright.empty ()) { info_.tags["copyright"] = file.copyright.c_str (); }

		info_.chips.push_back (playback::ChipInfo { "2A03", "NSFPlay", 0 });

		switch (file.chip ())
		{
			case formats::nsf::NsfChip::vrc6: info_.chips.push_back ({ "VRC6", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::vrc7: info_.chips.push_back ({ "VRC7", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::fds:  info_.chips.push_back ({ "FDS",  "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::mmc5: info_.chips.push_back ({ "MMC5", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::n163: info_.chips.push_back ({ "N163", "NSFPlay", 1 }); break;
			case formats::nsf::NsfChip::fme7: info_.chips.push_back ({ "S5B",  "NSFPlay", 1 }); break;
			default: break;
		}

		// Five channels, because the two chip objects expose exactly that many outputs between them:
		// the APU renders the two pulses, and the DMC object renders triangle, noise and DPCM.
		static const char* const names[] = { "Pulse 1", "Pulse 2", "Triangle", "Noise", "DMC" };

		for (int i = 0; i < 5; ++i)
		{
			tracks_.push_back (playback::TrackInfo { (playback::TrackId) (i + 1), 0, 0, i, true,
													 names[i], "2A03",
													 i < 2 ? playback::MidiCapability::melodic
														   : playback::MidiCapability::unavailable });
		}
	}

	const playback::SongInfo& getInfo (void) const override { return info_; }
	const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks_; }

	playback::BackendCapabilities getCapabilities (void) const override
	{
		playback::BackendCapabilities caps;
		caps.isolation = playback::IsolationMode::multiInstance;
		caps.rendersAtHostRate = true;
		return caps;
	}

	bool prepare (double hostSampleRate, int maxBlockSize, juce::String& errorMessage) override
	{
		rate = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
		blockSize = maxBlockSize > 0 ? maxBlockSize : 512;
		errorMessage.clear ();
		return true;
	}

	void setTimeline (int, double) override {}

	std::unique_ptr<playback::ITrackPlayer> createPlayer (juce::String& errorMessage) override
	{
		errorMessage.clear ();
		return std::make_unique<NsfTrackPlayer> (file, rate);
	}

private:
	formats::nsf::NsfFile file;
	std::vector<char> image;
	playback::SongInfo info_;
	std::vector<playback::TrackInfo> tracks_;
	double rate = 44100.0;
	int blockSize = 512;
};

} // namespace

bool NsfBackend::canOpen (const playback::SongMemory& memory) const
{
	return memory.isValid () && formats::nsf::NsfFile::looksLikeNsf (memory.data, memory.size);
}

std::unique_ptr<playback::ISongInstance> NsfBackend::open (const playback::SongMemory& memory,
														   juce::String& errorMessage)
{
	if (! memory.isValid ())
	{
		errorMessage = "empty song image";
		return nullptr;
	}

	formats::nsf::NsfFile file;
	std::string error;

	if (! formats::nsf::NsfFile::parse (memory.data, memory.size, file, error))
	{
		errorMessage = juce::String (error);
		return nullptr;
	}

	// The image is copied because the core's memory block outlives the caller's buffer, and the
	// players of a song read from the same copy.
	std::vector<char> bytes (static_cast<const char*> (memory.data),
							 static_cast<const char*> (memory.data) + memory.size);

	return std::make_unique<NsfSongInstance> (std::move (file), std::move (bytes));
}

} // namespace toybox::backends::nsf
