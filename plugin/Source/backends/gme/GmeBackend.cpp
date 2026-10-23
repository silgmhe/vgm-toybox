#include "GmeBackend.h"

#include <atomic>
#include <cstring>
#include <vector>

#include "Nsf_Emu.h"

namespace toybox::backends::gme
{

namespace
{

/** The chip flags an NSF header carries. libgme emulates three of the six expansions and refuses
	any image that asks for one it does not have, so the flags are read here to say which chip is
	missing rather than leaving "could not read the image" as the only message. */
struct NsfChips
{
	std::uint8_t flags = 0;
	bool nsfe = false;
	bool readable = false;

	bool has (std::uint8_t bit) const { return (flags & bit) != 0; }
	bool hasUnsupported (void) const { return has (0x02) || has (0x04) || has (0x08); }
};

NsfChips readNsfChips (const std::vector<char>& bytes)
{
	NsfChips chips;

	if (bytes.size() < 0x7B)
	{
		return chips;
	}

	const auto* data = reinterpret_cast<const unsigned char*> (bytes.data());

	if (std::memcmp (data, "NSFE", 4) == 0)
	{
		chips.nsfe = true;
		chips.readable = true;
		return chips;
	}

	if (std::memcmp (data, "NESM", 4) != 0)
	{
		return chips;
	}

	chips.flags = data[0x7A];
	chips.readable = true;
	return chips;
}

juce::String unsupportedChipName (const NsfChips& chips)
{
	if (chips.has (0x02)) { return "VRC7"; }
	if (chips.has (0x04)) { return "FDS"; }
	if (chips.has (0x08)) { return "MMC5"; }
	return {};
}

/** libgme renders a song as a whole - it has no per-channel mute and no isolation - so the
	instance offers a single track and says so through its capabilities. */
constexpr playback::TrackId kSingleTrack = 1;

std::unique_ptr<Nsf_Emu> makeEmulator (const void* data, std::size_t size, long sampleRate,
									   juce::String& errorMessage)
{
	auto emulator = std::make_unique<Nsf_Emu>();

	if (emulator->set_sample_rate (sampleRate) != nullptr)
	{
		errorMessage = "libgme rejected the sample rate";
		return nullptr;
	}

	if (emulator->load_mem (data, (long) size) != nullptr)
	{
		errorMessage = "libgme could not read the image";
		return nullptr;
	}

	return emulator;
}

class GmeTrackPlayer final : public playback::ITrackPlayer
{
public:
	GmeTrackPlayer (const void* data, std::size_t size, double sampleRate, int blockSize,
					juce::String& errorMessage)
		: emu (makeEmulator (data, size, (long) sampleRate, errorMessage)),
		  rate (sampleRate > 0.0 ? sampleRate : 44100.0),
		  scratch ((std::size_t) juce::jmax (1, blockSize) * 2, 0)
	{
		if (emu != nullptr)
		{
			emu->start_track (0);
		}
	}

	bool isReady (void) const { return emu != nullptr; }

	/** The five oscillators a 2A03 has, in libgme's order: two squares, triangle, noise, DMC. */
	static constexpr int oscCount = 5;

	/** libgme gives each oscillator its own output, and NULL mutes it. Muting is one-way through
		that call, so a player is built for a single track and never restored: the core asks for
		one player per track, which is what IsolationMode::multiInstance means here. */
	void isolateTo (int index)
	{
		if (emu == nullptr || soloIndex >= 0 || index < 0 || index >= oscCount)
		{
			return;
		}

		soloIndex = index;

		for (int i = 0; i < oscCount; ++i)
		{
			if (i != index)
			{
				emu->apu_()->osc_output (i, nullptr);
			}
		}
	}

	void start (void) override
	{
		if (emu != nullptr)
		{
			emu->start_track (0);
			seconds = 0.0;
			finished = false;
		}
	}

	void pause (void) override {}

	/** libgme seeks by running the driver forward from the start of the track, so a backwards
		seek costs as much as the distance from the beginning - the capability is real, not free. */
	bool seekSeconds (double target) override
	{
		if (emu == nullptr || target < 0.0)
		{
			return false;
		}

		if (emu->seek ((long) (target * 1000.0)) != nullptr)
		{
			return false;
		}

		seconds = target;
		finished = false;
		return true;
	}
	void setTimeline (int, double) override {}
	void setChipAudible (playback::DeviceId, bool) override {}

	void setTrackAudible (playback::TrackId track, bool audible) override
	{
		if (audible && track >= 1 && track <= (playback::TrackId) oscCount)
		{
			isolateTo ((int) track - 1);
		}
	}

	int render (float* left, float* right, int numSamples) override
	{
		if (emu == nullptr || numSamples <= 0)
		{
			return 0;
		}

		const int frames = juce::jmin (numSamples, (int) (scratch.size() / 2));

		if (emu->play ((long) frames * 2, scratch.data()) != nullptr)
		{
			finished = true;
			return 0;
		}

		for (int i = 0; i < frames; ++i)
		{
			const float l = (float) scratch[(std::size_t) i * 2 + 0] / 32768.0f;
			const float r = (float) scratch[(std::size_t) i * 2 + 1] / 32768.0f;

			if (left != nullptr)
			{
				left[i] = l;
			}

			if (right != nullptr)
			{
				right[i] = r;
			}
		}

		seconds += (double) frames / rate;

		if (emu->track_ended())
		{
			finished = true;
		}

		return frames;
	}

	double positionSeconds (void) override { return seconds; }
	bool isFinished (void) const override { return finished.load(); }

private:
	std::unique_ptr<Nsf_Emu> emu;
	double rate = 44100.0;
	std::vector<short> scratch;
	double seconds = 0.0;
	std::atomic<bool> finished { false };
	int soloIndex = -1;
};

class GmeSongInstance final : public playback::ISongInstance
{
public:
	explicit GmeSongInstance (const playback::SongMemory& memory) : bytes (memory.size)
	{
		std::memcpy (bytes.data(), memory.data, memory.size);
	}

	bool prepare (double hostSampleRate, int maxBlockSize, juce::String& errorMessage) override
	{
		rate = hostSampleRate > 0.0 ? hostSampleRate : 44100.0;
		blockSize = maxBlockSize > 0 ? maxBlockSize : 512;

		// A throwaway emulator reads the metadata and proves the image is readable. Players own
		// their own emulator afterwards, because libgme keeps no state shared between them.
		auto emulator = makeEmulator (bytes.data(), bytes.size(), (long) rate, errorMessage);

		if (emulator == nullptr)
		{
			return false;
		}

		track_info_t info {};
		if (emulator->track_info (&info, 0) == nullptr)
		{
			if (info.song != nullptr && *info.song != 0) { info_.tags["title"] = info.song; }
			if (info.game != nullptr && *info.game != 0) { info_.tags["game"] = info.game; }
			if (info.author != nullptr && *info.author != 0) { info_.tags["artist"] = info.author; }
			if (info.copyright != nullptr && *info.copyright != 0) { info_.tags["copyright"] = info.copyright; }

			if (info.length > 0)
			{
				info_.durationSeconds = (double) info.length / 1000.0;
				info_.singlePassSeconds = info_.durationSeconds;
			}
		}

		info_.formatName = "libgme";
		info_.chips.clear();
		info_.chips.push_back (playback::ChipInfo { "2A03", "libgme", 0 });

		// The header says which expansion the driver expects; libgme emulates three of them.
		const auto chips = readNsfChips (bytes);

		if (chips.has (0x01)) { info_.chips.push_back (playback::ChipInfo { "VRC6", "libgme", 1 }); }
		if (chips.has (0x10)) { info_.chips.push_back (playback::ChipInfo { "N163", "libgme", 1 }); }
		if (chips.has (0x20)) { info_.chips.push_back (playback::ChipInfo { "S5B", "libgme", 1 }); }

		tracks_.clear();

		// One track per oscillator. libgme can point each of them at its own output, which is the
		// whole basis of isolating them; the core provides the gain and pan.
		static const char* const names[] = { "Pulse 1", "Pulse 2", "Triangle", "Noise", "DMC" };

		for (int i = 0; i < 5; ++i)
		{
			tracks_.push_back (playback::TrackInfo { (playback::TrackId) (i + 1), 0, 0, i, true,
													 names[i], "2A03",
													 i < 2 ? playback::MidiCapability::melodic
														   : playback::MidiCapability::unavailable });
		}

		return true;
	}

	const playback::SongInfo& getInfo (void) const override { return info_; }
	const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks_; }

	playback::BackendCapabilities getCapabilities (void) const override
	{
		playback::BackendCapabilities caps;
		caps.canSeek = true;
		caps.canLoop = false;
		caps.canFade = false;
		caps.hasLoopPoint = false;
		caps.canMuteChips = false;
		caps.isolation = playback::IsolationMode::multiInstance;
		caps.rendersAtHostRate = true;
		caps.nativeSampleRate = 0.0;
		return caps;
	}

	void setTimeline (int, double) override {}

	std::unique_ptr<playback::ITrackPlayer> createPlayer (juce::String& errorMessage) override
	{
		auto player = std::make_unique<GmeTrackPlayer> (bytes.data(), bytes.size(), rate,
														blockSize, errorMessage);

		if (! player->isReady())
		{
			return nullptr;
		}

		return player;
	}

private:
	std::vector<char> bytes;
	double rate = 44100.0;
	int blockSize = 512;
	playback::SongInfo info_;
	std::vector<playback::TrackInfo> tracks_;
};

} // namespace

bool GmeBackend::canOpen (const playback::SongMemory& memory) const
{
	if (! memory.isValid() || memory.size < 4)
	{
		return false;
	}

	const auto* bytes = static_cast<const std::uint8_t*> (memory.data);

	return std::memcmp (bytes, "NESM", 4) == 0 || std::memcmp (bytes, "NSFE", 4) == 0;
}

std::unique_ptr<playback::ISongInstance> GmeBackend::open (const playback::SongMemory& memory,
														   juce::String& errorMessage)
{
	if (! canOpen (memory))
	{
		errorMessage = "not an NSF image";
		return nullptr;
	}

	const std::vector<char> bytes (static_cast<const char*> (memory.data),
								   static_cast<const char*> (memory.data) + memory.size);
	const auto chips = readNsfChips (bytes);

	if (chips.hasUnsupported())
	{
		errorMessage = "this NSF needs the " + unsupportedChipName (chips)
					 + " expansion chip, which libgme does not emulate";
		return nullptr;
	}

	return std::make_unique<GmeSongInstance> (memory);
}

} // namespace toybox::backends::gme
