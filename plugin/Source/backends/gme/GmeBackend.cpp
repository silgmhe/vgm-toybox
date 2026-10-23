#include "GmeBackend.h"

#include <atomic>
#include <cstdio>
#include <set>
#include <cstring>
#include <vector>

#include "Nes_Fme7_Apu.h"
#include "Nes_Namco_Apu.h"
#include "Nes_Vrc6_Apu.h"
#include "Nsf_Emu.h"

namespace toybox::backends::gme
{

namespace
{

/** One frame of the driver's output, hashed. Two frames with the same hash are the same musical
	moment, which is what makes a loop point findable in a program that never ends. */
struct LoopPoint
{
	bool found = false;
	double introSeconds = 0.0;
	double loopSeconds = 0.0;
};

LoopPoint findLoop (Nsf_Emu& emu, double rate)
{
	LoopPoint result;

	// 735 samples is one NTSC frame at 44.1 kHz; a frame is the driver's natural unit.
	const int frameSamples = 735;
	const int maxFrames = 60 * 180;

	std::vector<short> buffer ((std::size_t) frameSamples * 2, 0);
	std::vector<unsigned long> hashes;
	hashes.reserve (maxFrames);

	emu.start_track (0);

	for (int frame = 0; frame < maxFrames && ! emu.track_ended(); ++frame)
	{
		if (emu.play (frameSamples * 2, buffer.data()) != nullptr)
		{
			break;
		}

		hashes.push_back (emu.reg_hash_());
	}

	// A single matching frame proves nothing - silence and sustained notes repeat constantly - so
	// eight consecutive matches are required before a repeat is believed.
	constexpr int confirm = 20;

	// Every earlier occurrence is kept, not just the most recent: a frame with no writes has the
	// same hash as the frame before it, so keeping only the latest would overwrite the candidate
	// that matters with an adjacent duplicate.
	std::map<unsigned long, std::vector<int>> seen;

	for (int k = 0; k < (int) hashes.size(); ++k)
	{
		auto& candidates = seen[hashes[(std::size_t) k]];

		if (k + confirm < (int) hashes.size())
		{
			for (auto it = candidates.rbegin(); it != candidates.rend() && *it < k; ++it)
			{
				const int j = *it;

				if (k - j <= confirm)
				{
					continue;
				}

				bool same = true;

				for (int n = 0; n < confirm && same; ++n)
				{
					same = hashes[(std::size_t) (j + n)] == hashes[(std::size_t) (k + n)];
				}

				// A window where nothing is written repeats trivially - two long rests look
				// identical - so a candidate has to contain some activity of its own.
				std::set<unsigned long> within (hashes.begin() + k, hashes.begin() + k + confirm);

				if (same && within.size() > 12)
				{
					const double framesPerSecond = rate / (double) frameSamples;
					result.found = true;
					result.introSeconds = (double) j / framesPerSecond;
					result.loopSeconds = (double) (k - j) / framesPerSecond;
					return result;
				}
			}
		}

		candidates.push_back (k);
	}

	return result;
}

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
		if (emu == nullptr || soloIndex >= 0 || index < 0)
		{
			return;
		}

		soloIndex = index;

		// The enumeration order has to match the track list built in prepare(): the five 2A03
		// oscillators first, then the expansion chip's, in the order VRC6, N163, S5B.
		int position = 0;

		for (int i = 0; i < oscCount; ++i)
		{
			if (position++ != index)
			{
				emu->apu_()->osc_output (i, nullptr);
			}
		}

		if (auto* chip = emu->vrc6_())
		{
			for (int i = 0; i < 3; ++i)
			{
				if (position++ != index) { chip->osc_output (i, nullptr); }
			}
		}

		if (auto* chip = emu->namco_())
		{
			for (int i = 0; i < 8; ++i)
			{
				if (position++ != index) { chip->osc_output (i, nullptr); }
			}
		}

		if (auto* chip = emu->fme7_())
		{
			for (int i = 0; i < 3; ++i)
			{
				if (position++ != index) { chip->osc_output (i, nullptr); }
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

		// One pass through the song to find where it repeats: the only way to get a length out of
		// an NSF, whose header has none and whose driver never reports an end.
		const auto loop = findLoop (*emulator, rate);

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

		if (loop.found)
		{
			info_.singlePassSeconds = loop.introSeconds + loop.loopSeconds;
			info_.durationSeconds = info_.singlePassSeconds;
			info_.loopPointSeconds = loop.introSeconds;
			info_.loopLengthSeconds = loop.loopSeconds;
			info_.loopsForever = false;
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

		playback::TrackId nextId = 1;

		for (int i = 0; i < 5; ++i)
		{
			tracks_.push_back (playback::TrackInfo { nextId++, 0, 0, i, true, names[i], "2A03",
													 i < 2 ? playback::MidiCapability::melodic
														   : playback::MidiCapability::unavailable });
		}

		// The expansion oscillators are addressable too, in the same way as the 2A03's, so they
		// become tracks rather than being folded into the mix unannounced.
		auto addExpansion = [this, &nextId] (bool present, int count, const char* chip,
											 const char* prefix, int device)
		{
			if (! present)
			{
				return;
			}

			for (int i = 0; i < count; ++i)
			{
				tracks_.push_back (playback::TrackInfo {
					nextId++, (playback::DeviceId) device, 0, i, true,
					juce::String (prefix) + " " + juce::String (i + 1), chip,
					playback::MidiCapability::unavailable });
			}
		};

		// Driven by the header flags rather than by the emulator's pointers: the flags are what the
		// chips list is built from, and the two must agree or the mixer would show tracks for a
		// chip it never named.
		if (chips.has (0x01)) { addExpansion (true, 3, "VRC6", "VRC6", 1); }
		if (chips.has (0x10)) { addExpansion (true, 8, "N163", "N163", 1); }
		if (chips.has (0x20)) { addExpansion (true, 3, "S5B", "S5B", 1); }

		return true;
	}

	const playback::SongInfo& getInfo (void) const override { return info_; }
	const std::vector<playback::TrackInfo>& getTracks (void) const override { return tracks_; }

	playback::BackendCapabilities getCapabilities (void) const override
	{
		playback::BackendCapabilities caps;
		caps.canSeek = true;
		caps.hasLoopPoint = true;
		caps.canLoop = false;
		caps.canFade = false;
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
