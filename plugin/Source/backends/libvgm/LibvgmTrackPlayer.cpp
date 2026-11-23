#include "LibvgmTrackPlayer.h"

#include "player/droplayer.hpp"
#include "player/gymplayer.hpp"
#include "player/s98player.hpp"
#include "player/vgmplayer.hpp"
#include "utils/MemoryLoader.h"

#include "LibvgmMuteIsolation.h"
#include "emu/SoundDevs.h"

#include <algorithm>
#include <cmath>

#include "playback/ChannelFromChip.h"
#include "settings/SettingsStore.h"

#include "playback/NoteWaveform.h"

#include "FmVoiceDriver.h"

namespace toybox::backends::libvgm
{
	namespace
	{
		/** How many audio blocks a hit is kept available to the panel.

			A block is the engine's 512 samples, about twelve milliseconds, and the panel reads once
			per display frame - so this has to outlast one frame plus a skipped capture. Sixteen
			blocks is roughly a fifth of a second: long enough that no hit is missed, short enough
			that a row does not stay lit on its own.
		*/
		constexpr int kHitBlocks = 16;
	}


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

	// The libvgm side of the note renderer. Created up front rather than on first use: it holds
	// no resources until it is loaded, and a lazily created unique_ptr on the audio thread would
	// need a guard for no gain.
	voiceDriver = makeFmVoiceDriver ();

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

	// **A streamed voice is moved by its start address, not by replaying writes.** libvgm's seek
	// replays the register writes, and these drivers have none after their opening second: the
	// playhead moved while the audio did not (probe: identical RMS at 0 s, 5 s, 30 s). What the
	// driver does to advance its stream is rewrite `start`, so a seek does the same thing - put the
	// voice at the byte the target time corresponds to, then key it on again.
	// **Refuse, because every alternative measured worse.** libvgm's own seek replays register
	// writes, and these drivers have none after their opening second, so the playhead moved while
	// the audio repeated the opening. Repositioning the voice was tried twice: to an arbitrary byte
	// (four-bit ADPCM carries a predictor the chip resets on key-on, so entering mid-sample plays
	// noise) and then to the section starts the driver itself wrote (still noise on `06 Sonic
	// Diver`, with the untouched stream audible underneath). Neither is acceptable, so a streamed
	// song reports the seek as unsupported and the position stays where it is. `02 Spirit of Fang`
	// already behaves this way, and the ear confirms it is the only clean outcome.
	if (! seekable)
	{
		return false;
	}

	// **[TOYBOX NOTE] The seek itself is libvgm's, and it does not work for every file.**
	//
	// `VGMPlayer::Seek` (`vgmplayer.cpp:1939`) only parses **forward**: it calls `Reset()` when the
	// target is behind the current tick and otherwise asks `ParseFile(delta)` to advance. `ParseFile`
	// opens with `if (_playState & PLAYSTATE_END) return;` (`vgmplayer.cpp:2052-2057`), so once a song
	// has run to its end, every forward seek is a no-op - and a backward seek resets and re-parses
	// from `dataOfs`, which for a stream that *starts* with one huge inline ROM block (the YMZ280B
	// rips here: 536 659 bytes) never reaches the target.
	//
	// A reset-then-seek was tried here and made both directions land at zero, so it was reverted
	// rather than left in: the real fix belongs in the library - skip the ROM re-write while
	// `PLAYSTATE_SEEK` and clear `PLAYSTATE_END` for a seek - and it needs a headless seek test to
	// verify, which the harness does not have yet.
	playerEngine.Seek (PLAYPOS_SAMPLE, (uint32_t) (juce::jmax (0.0, seconds) * sampleRate));
	finished = false;

	// The decoders describe the position before the jump. Marked rather than rewound here: this is
	// the message thread and the replayer belongs to the audio thread, which will do it before it
	// captures the next frame.
	pendingRewind.store (true, std::memory_order_relaxed);
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

	// For captureState, which is const and so cannot ask the player itself. Same thread, one call
	// later - the engine renders a block and then captures, inside the same lock.
	lastPosition.store (positionSeconds (), std::memory_order_relaxed);

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
	else if (evtType == PLREVT_LOOP)
	{
		// The song went back to its loop point, so the decoders have to as well: their registers
		// still describe the position before the jump, and a snapshot taken now would show the
		// state of the previous pass while the music plays the next one.
		pendingRewind.store (true, std::memory_order_relaxed);
	}

	return 0x00;
}

std::vector<playback::DecodedSample> LibvgmTrackPlayer::samples (void) const
{
	// A copy rather than a reference: the list is shared and the caller may keep it past this
	// player's life (an export dialog holds it while the song keeps playing).
	return sampleList != nullptr ? *sampleList : std::vector<playback::DecodedSample> ();
}

namespace
{
	/** How many points a DAC row draws. The longest hit in the four Sonic files is 5 398 bytes and
		the row paints 128 columns, so anything past 8 192 is decode cost with no picture to show for
		it; the file still gets the whole run (`writeSamples` has its own, larger limits). */
	constexpr std::size_t kDacDisplayPoints = 8192;

	/** **A device the song reports under its parent's identity.**

		libvgm gives a linked voice set - the SSG behind a YM2203, a YM2608 or a YM2610 - the
		parent's device id *and* its type, so neither field separates them. Measured on
		`82 Ruins.vgz`, the two instances of one YM2608 both carry `type=0x07` and `id=0`; only
		their channel counts differ, thirteen against three.

		**Which is why this must not be a test on one `DEVID_`.** It used to be, naming the YM2610,
		and that is the chip it was written for - so a YM2608's SSG answered as FM channels 1-3 on
		the keyboard (its state looked up an FM track) while the FM rows were drawn as squares (the
		kind table's SSG slot overwrote theirs). One device type, three failures below it.

		**The decoder answers it now** (`IChipDecoder::isLinkedVoiceSet`), which is where the
		knowledge belongs: the SSG is an `Ay8910Decoder`, and a YM2203's FM - three channels as well -
		is not. Not a `typeid` here, because that puts a class name in playback code and goes quietly
		wrong the day a subclass appears; not a channel count either, because `DEV_CHN_COUNT_YM2203`
		is three.

		One definition, because two copies drift: the copy in the kind table answered `channelKind
		(0..2)` from the FM instance, whose FM count is above those indices, so the SSG's three rows
		drew as `fm` while the keyboard drew them as squares.
	*/
	template <typename Instance>
	bool isLinkedVoiceSet (const Instance& instance)
	{
		if (instance.device == nullptr || instance.decoder == nullptr)
		{
			return false;
		}

		// **The parent test keeps a standalone AY8910 out**: there the device type is the AY itself,
		// and its decoder answers true for the same reason a linked one does.
		const bool opnParent = instance.device->type == DEVID_YM2203
							   || instance.device->type == DEVID_YM2608
							   || instance.device->type == DEVID_YM2610;

		return opnParent && instance.decoder->isLinkedVoiceSet ();
	}
}

void LibvgmTrackPlayer::applyDacHit (playback::ChannelState& state, uint32_t samples) const
{
	if (dacTable == nullptr || dacTable->runs.empty () || dacTable->bank.empty ())
	{
		return;			// no table: the decoder's own window is the honest answer
	}

	// The lookup lives in the sampler header so a test can pin it without a song: "the hit that is
	// playing, or the last one that played" is the whole rule, and it has one definition.
	const dac::Run* foundRun = dac::hitAt (dacTable->runs, samples);

	if (foundRun == nullptr)
	{
		return;			// nothing has played yet
	}

	const dac::Run& run = *foundRun;

	if (run.start >= dacTable->bank.size () || run.duration == 0)
	{
		return;
	}

	const auto key = std::make_pair (run.start, run.length);
	auto found = dacWaves.find (key);

	if (found == dacWaves.end ())
	{
		const std::size_t length
			= (std::min<std::size_t>) (run.length, dacTable->bank.size () - run.start);
		const std::size_t capped = (std::min<std::size_t>) (length, kDacDisplayPoints);

		auto wave = std::make_shared<std::vector<float>> ();
		wave->reserve (capped);

		for (std::size_t i = 0; i < capped; ++i)
		{
			// A run longer than the row can draw is **sampled evenly**, not truncated: the shape of
			// a long hit is its envelope, and drawing only the attack would read as a different drum.
			const std::size_t index = (capped == length) ? i : (i * length) / capped;
			wave->push_back ((float) (((int) dacTable->bank[run.start + index] - 0x80) / 128.0));
		}

		if (dacWaves.size () > 128)
		{
			// A driver with hundreds of distinct entries would grow this without bound; the table
			// belongs to one song, so dropping it is cheap and cannot show the wrong shape.
			dacWaves.clear ();
		}

		found = dacWaves.emplace (key, std::move (wave)).first;
	}

	state.wave = found->second;
	state.sampleRate
		= (float) ((uint64_t) run.length * (uint64_t) dacTable->tickRate / run.duration);

	// **The note names the sample**, not a pitch: C4 plus its index, wrapping every five octaves.
	// The index is the one the export puts in a file name, so the row and the file agree; a drum row
	// that always said C4 could not tell a kick from a snare while transcribing, which is the row's
	// whole purpose. `state.note` is what the piano roll draws, and the MIDI path applies the same
	// rule in `MidiSequenceBuilder::flushOpn`.
	const int note = dac::noteForSample (run.sample);
	state.note = note;
	state.frequency = 440.0f * std::pow (2.0f, (float) (note - 69) / 12.0f);
}

void LibvgmTrackPlayer::setReplaySource (std::shared_ptr<const midi::SongIr> ir,
										 const std::vector<PLR_DEV_INFO>& devices,
										 std::shared_ptr<const dac::Table> table,
										 RomCache* romCache)
{
	replayIr = std::move (ir);
	replayDevices = devices;
	replayRomCache = romCache;

	// **A replayer built before this call has no cache.** `probe` is started before it is handed
	// its source, and starting is enough to build the replayer - so the first note waveform asked
	// for would assemble sixteen megabytes per device for the rest of the session. Dropping it
	// here costs one advance and makes the next build pick the cache up.
	replayer.reset ();

	// **The table is built once by the backend that owns the song**, not here: this call happens once
	// per track player (a dozen of them), and building it meant a pass over every `0x8n` byte each
	// time - about 200 ms of load time on `02 Green Hill Zone` for eleven tables nobody reads.
	dacTable = std::move (table);
	dacWaves.clear ();

	// **The kind table belongs to the song this player is about to describe.** It is built once and
	// never invalidated, which was safe only because a player is constructed per song - an implicit
	// contract. Reusing a player would leave the previous song's kinds in place: no crash, no red
	// test, just the wrong marks on every strip.
	kindsCollected = false;
	kindTable.clear ();

	// A streamed sample player cannot be seeked - see the note in seekSeconds.
	seekable = true;

	if (replayIr != nullptr)
	{
		for (const auto& slot : replayIr->chips)
		{
			if (slot.kind == midi::ChipKind::ymz280b)
			{
				seekable = false;
			}
		}
	}

	// A new song has played nothing yet: the remembered samples belong to the old one.
	ledger.lastPlayed.clear ();

	if (replayIr != nullptr)
	{
		ledger.buildNotes (*replayIr);
	}
}

const LibvgmTrackPlayer::DpcmLookup* LibvgmTrackPlayer::Ledger::lookup (
	uint32_t sampleId, const std::vector<playback::DecodedSample>* collection) const
{
	if (sampleId == 0)
	{
		return nullptr;
	}

	const auto cached = samples.find (sampleId);
	if (cached != samples.end ())
	{
		return &cached->second;
	}

	if (collection == nullptr)
	{
		return nullptr;
	}

	for (const auto& sample : *collection)
	{
		if (sample.sampleId != sampleId)
		{
			continue;
		}

		DpcmLookup entry;
		entry.wave = std::make_shared<const std::vector<float>> (sample.values);
		entry.rate = (float) sample.rate;

		return &samples.emplace (sampleId, std::move (entry)).first->second;
	}

	// Not an error: the collector and the decoder name samples with the same arithmetic, so a miss
	// means this hit's bytes were never collected - a file that carried no ROM for them. The row
	// then draws the rate and no wave, which is the truth about that hit.
	return nullptr;
}

void LibvgmTrackPlayer::Ledger::buildNotes (const midi::SongIr& ir)
{
	notes.clear ();

	// A sample is named by the two registers that point at it, and a hit happens when `$4015`
	// enables the channel with both already written. Scanning the song's writes in order keeps the
	// three together - the registers may be written on different frames, which is why this waits
	// for the enable instead of recording on the address.
	uint8_t page = 0;
	int length = 0;
	int nextNote = 60;		// middle C: the note a drum part is written at

	for (const auto& write : ir.writes)
	{
		if (write.deviceId >= ir.chips.size ()
			|| ir.chips[write.deviceId].kind != midi::ChipKind::nesApu)
		{
			continue;
		}

		// The 2A03's registers are the low five bits of the address the stream carries.
		const uint8_t reg = (uint8_t) (write.reg & 0x1F);
		const uint8_t value = (uint8_t) write.value;

		if (reg == 0x12)
		{
			page = (uint8_t) (value & 0x3F);
			continue;
		}

		if (reg == 0x13)
		{
			length = (value == 0) ? 1 : ((int) value * 16 + 1);
			continue;
		}

		if (reg != 0x15 || (value & 0x10) == 0)
		{
			continue;
		}

		// Same identity the decoder reports: page and length, not the bytes.
		const uint32_t id = ((uint32_t) page << 16) | (uint32_t) length;

		if (notes.find (id) == notes.end () && nextNote <= 127)
		{
			notes[id] = nextNote++;
		}
	}
}

chips::ChannelKind LibvgmTrackPlayer::channelKindFor (playback::DeviceId deviceId, uint16_t sourceType,
													   int deviceChannel) const
{
	// A whole-device row has no single channel, so no single kind either.
	if (deviceChannel < 0 || replayIr == nullptr || replayDevices.empty ())
	{
		return chips::ChannelKind::unknown;
	}

	// On demand, the way captureState() creates it: enumerateTracks() runs before a block has been
	// **The instances appear as their first command is replayed**, so asking at time zero answers
	// with an empty list. The table is therefore built once, from a replayer of its own pushed a few
	// seconds in, and thrown away - **not** the one captureState() drives, whose playhead must stay
	// where the audio thread left it. One pass, not one per track.
	if (! kindsCollected)
	{
		kindsCollected = true;

		VgmReplayer probe (replayIr, replayDevices);
		probe.advanceTo ((uint32_t) (probe.tickRate () * 2.0));

		for (const auto& instance : probe.instances ())
		{
			if (instance.device == nullptr)
			{
				continue;
			}

			const int count = instance.decoder->channelCount ();
			const bool isSsg = isLinkedVoiceSet (instance);

			std::vector<chips::ChannelKind> kinds;
			kinds.reserve ((size_t) juce::jmax (0, count));

			for (int channel = 0; channel < count; ++channel)
			{
				kinds.push_back (instance.decoder->channelKind (channel));
			}

			kindTable[{ instance.device->id, isSsg }] = std::move (kinds);
		}
	}

	// **The YM2610 answers with two instances under one handle.** Its SSG is a child device that
	// libvgm reports with the parent's id *and* type, so the handle cannot separate them - three
	// channels is what does, the same call captureState() makes at `voiceType`. A track whose
	// `sourceDeviceType` is the AY is the SSG one.
	const bool wantSsg = (sourceType == DEVID_AY8910);

	const auto found = kindTable.find ({ (uint32_t) deviceId, wantSsg });

	if (found == kindTable.end () || deviceChannel >= (int) found->second.size ())
	{
		return chips::ChannelKind::unknown;
	}

	return found->second[(size_t) deviceChannel];
}

int LibvgmTrackPlayer::trackForVoice (uint16_t deviceType, int deviceChannel) const
{
	if (tracks == nullptr)
	{
		return -1;
	}

	int wholeDevice = -1;

	for (std::size_t i = 0; i < tracks->size (); ++i)
	{
		const auto& track = (*tracks)[i];

		if (track.sourceDeviceType != deviceType)
		{
			continue;
		}

		if (track.deviceChannel == deviceChannel)
		{
			return (int) i;
		}

		if (track.deviceChannel < 0)
		{
			wholeDevice = (int) i;		// the core cannot isolate this device any further
		}
	}

	return wholeDevice;
}

int LibvgmTrackPlayer::trackFor (int channel) const
{
	if (tracks == nullptr)
	{
		return -1;
	}

	int wholeDeviceTrack = -1;

	for (std::size_t i = 0; i < tracks->size (); ++i)
	{
		const auto& track = (*tracks)[i];

		// A whole-device track (channelIndex -1) owns every channel of that device: the core the
		// song picked cannot isolate its voices, so there is nothing finer to point at.
		//
		// **Exact matches win.** The two conditions were one `||`, so a device with a whole-device
		// track claimed *every* channel that shared its id - and a linked voice set shares its
		// parent's, which attached the SSG's channels to whichever track came first.
		if (track.channelIndex == channel)
		{
			return (int) i;
		}

		if (track.channelIndex < 0 && wholeDeviceTrack == -1)
		{
			wholeDeviceTrack = (int) i;
		}
	}

	if (wholeDeviceTrack != -1)
	{
		return wholeDeviceTrack;
	}

	return -1;
}

bool LibvgmTrackPlayer::captureState (playback::ChipStateSnapshot& snapshot) const
{
	if (replayIr == nullptr || replayIr->writes.empty ())
	{
		return false;		// nothing to describe; the panel drawing nothing is the honest answer
	}

	if (replayer == nullptr)
	{
		replayer = std::make_unique<VgmReplayer> (replayIr, replayDevices, replayRomCache);
	}

	if (pendingRewind.exchange (false, std::memory_order_relaxed))
	{
		replayer->rewind ();

		// The remembered samples belong to the position the playhead just left. Keeping them would
		// offer a drag of something that never played here.
		ledger.lastPlayed.clear ();
	}

	const double seconds = lastPosition.load (std::memory_order_relaxed);
	const auto samples = (uint32_t) std::max (0.0, seconds * replayer->tickRate ());

	replayer->advanceTo (samples);

	snapshot.chips.clear ();
	snapshot.channels.clear ();
	snapshot.positionSeconds = seconds;
	snapshot.valid = true;

	// **A voice is found by the pair `describeSong` wrote down, not by where it sits in a container.**
	// `TrackInfo::sourceDeviceId` names the device a row really describes (a linked voice set keeps the
	// child's id there, not its parent's) and `deviceChannel` names the channel inside it. Matching on
	// that pair is what makes the walk order irrelevant: instances appear in the order commands reached
	// them, tracks in the order the header lists devices, and those two orders differ - the Mega Drive's
	// PSG is written first but listed after the YM2612, which shifted four rows.
	auto instances = replayer->instances ();

	// No ordering is imposed on the instances: the lookup above is by identity, so the order they are
	// walked in cannot matter. A sort used to stand here - widest device first - which held only while
	// the instance order happened to agree with the track numbering.
	for (std::size_t instanceIndex = 0; instanceIndex < instances.size (); ++instanceIndex)
	{
		const auto& instance = instances[instanceIndex];
		const auto* device = instance.device;
		const bool hasDeclaration = device != nullptr && device->devDecl != nullptr;

		std::string chipName = "Chip";

		if (hasDeclaration && device->devDecl->name != nullptr)
		{
			if (const char* name = device->devDecl->name (device->devCfg))
			{
				chipName = name;
			}
		}

		// The chip list says what is playing here even when a channel cannot be described, so it is
		// filled once per chip rather than once per device.
		if (std::find (snapshot.chips.begin (), snapshot.chips.end (), chipName) == snapshot.chips.end ())
		{
			snapshot.chips.push_back (chipName);
		}

		const char* const* channelNames
			= (hasDeclaration && device->devDecl->channelNames != nullptr)
				  ? device->devDecl->channelNames (device->devCfg)
				  : nullptr;

		const int count = instance.decoder->channelCount ();


		for (int channel = 0; channel < count; ++channel)
		{
			auto chipState = instance.decoder->channelState (channel);

			if (! chipState.present)
			{
				continue;
			}


			const auto kind = instance.decoder->channelKind (channel);

			playback::ChipShapeHints hints;
			hints.waveKind = playback::waveKindForChannelKind (kind);
			hints.channelKind = kind;
			hints.isDrum = instance.decoder->isDrum (channel);

			// The chips that select a pulse width by a two-bit register. The others are always
			// square, and their `patch` means something else - a noise period, a sample number -
			// so reading it as a duty here would draw a shape the chip does not make.
			hints.patchIsDuty = (kind == chips::ChannelKind::nesPulse
								 || kind == chips::ChannelKind::gbPulse
								 || kind == chips::ChannelKind::vrc6Pulse
								 || kind == chips::ChannelKind::mmc5Pulse);
			std::string channelName;
			if (channelNames != nullptr && channelNames[channel] != nullptr)
			{
				channelName = channelNames[channel];
			}
			else
			{
				// **A chip without a name table still gets a useful name.** `kind` is the
				// decoder's own answer about what this channel is, so the fallback can use it
				// rather than numbering everything `Ch n`. The Nuked OPM ships no `channelNames`,
				// and its eight rows read `Ch 1`-`Ch 8` for exactly that reason.
				//
				// Keyed on the kind, not on a device id: the OPL family and the OPX will arrive
				// the same way, and each of them should say `FM n` without another edit here.
				const bool fm = kind == chips::ChannelKind::fm || kind == chips::ChannelKind::fmDrum;
				channelName = (fm ? "FM " : "Ch ") + std::to_string (channel + 1);
			}

			auto state = playback::channelFromChip (
				chipState, instance.decoder->noteTable (channel), hints, chipName, channelName);

			// **Rendered here, not in channelState()**: this runs while the keyboard page is on
			// screen, and `channelState()` runs while a song is being scanned. The cache is keyed
			// on the patch and the pitch, which is everything `render()` reads.
			//
			// **No pitch test on the row.** `state.frequency` comes from `GetFreqHz()`, and a
			// driver writes the pitch register only when it starts a note - so a channel resting at
			// this instant reports zero, and requiring one left its row drawing a single cycle
			// instead of the note. Measured on a YM2203 song over forty-five blocks: `freq > 0` held
			// for three quarters of them, and the rows that carried a note matched that count
			// exactly - a quarter of the frames drew a cycle rather than the note.
			//
			// The NSF side dropped the same condition for the same reason - the shape is drawn at a
			// fixed pitch, so whether this voice has one right now is not what decides it.
			if (state.voicePatch != nullptr && voiceDriver != nullptr)
			{
				NoteWaveEntry& entry = noteWaves[channel];

				// **A fixed pitch, not the note that happens to be playing.** Keyed on the playing
				// note, a timbre that slides or vibratos re-rendered on almost every frame - and at
				// the chip's own rate that is 424 KB and four milliseconds each time, thrown away as
				// fast as it was made. What the row shows is the timbre, and a timbre is a shape: it
				// has no pitch of its own until something plays it.
				const auto& store = settings::SettingsStore::getInstance ();
				const double hz = playback::noteWave::frequencyOf (store.getFmFixedNote ());

				// Both lengths are settings, so both belong in the cache key: changing either has to
				// invalidate every cached note, or the slider would appear to do nothing.
				const double sustain = (double) juce::jmax (1, store.getFmSustainMs ()) / 1000.0;
				const double release = (double) juce::jmax (1, store.getFmReleaseMs ()) / 1000.0;
				const bool normalise = store.getFmNormalise ();

				if (entry.wave == nullptr || entry.patch != state.voicePatch
					|| entry.hz != hz || entry.sustain != sustain || entry.release != release
					|| entry.normalise != normalise)
				{
					entry.patch = state.voicePatch;
					entry.hz = hz;
					entry.sustain = sustain;
					entry.release = release;
					entry.normalise = normalise;
					entry.wave = std::make_shared<const std::vector<float>> (
						playback::noteWave::render (*voiceDriver, *state.voicePatch, hz, sustain,
													release, normalise));
				}

								state.noteWaveform = entry.wave;

				// **This is the panel's copy, so it carries the panel's rate.** Export renders its
				// own through `renderNoteAt`, at the chip's rate, and says so in `noteWaveformRate`
				// when it hands the channel over - see `RenderedNote`.
				state.noteWaveformRate = (float) chips::kVisualNoteRate;
			}



			// **The YM2612's DAC row answers for the hit, not for the stream.** The decoder can only
			// see bytes arrive; which hit they belong to is a fact about the driver's table, built
			// from `SongIr` (see `buildDacTable`). Channel 6 is the DAC on that chip.
			if (device != nullptr && device->type == DEVID_YM2612 && channel == 6)
			{
				applyDacHit (state, samples);
			}

			// Set here rather than inside the conversion: which engine track a channel belongs to is
			// a fact about this runtime, not about the chip, and the conversion has no engine to ask.
			// **Matched by number, not by device.** A linked voice set's tracks carry its parent's
			// `deviceId` - `describeSong` creates them inside the head's loop - so filtering by id can
			// never find them. That is the whole reason the SSG and PSG rows sat empty while the mixer,
			// which does no such matching, drew them: it was not the block arithmetic at all.
			uint16_t voiceType = (device != nullptr) ? (uint16_t) device->type : 0;

			if (isLinkedVoiceSet (instance))	// the SSG answers under the YM2610's identity
			{
				voiceType = DEVID_AY8910;
			}

			state.trackIndex = trackForVoice (voiceType, channel);

			// **The name of a row is the name of the track it belongs to.** The chip's own table is
			// not the same thing: a linked voice set shares its parent's table, so the SSG's three
			// channels were drawn as `FM 1..3` while the mixer called those same tracks `SSG 1..3`.
			// Taking the name from the track removes the second naming rule instead of correcting it.
			if (tracks != nullptr && state.trackIndex >= 0
				&& (std::size_t) state.trackIndex < tracks->size ())
			{
				const auto& track = (*tracks)[(std::size_t) state.trackIndex];
				state.name = track.name.toStdString ();
				state.chipName = track.groupName.toStdString ();
			}

			// And so is this: which sample is playing is the chip's answer, but what note that
			// sample gets is the song's. Without it every drum hit shares the one trigger note and
			// the grid cannot say which hit it was - the conversion sees a single channel at a
			// single moment, and the mapping only exists across the whole file.
			if (chipState.sampleId != 0)
			{
				const auto found = ledger.notes.find (chipState.sampleId);

				if (found != ledger.notes.end ())
				{
					state.note = found->second;
					state.frequency = 440.0f * std::pow (2.0f, (float) (state.note - 69) / 12.0f);
				}

				// The bytes come from the same collection the export writes, not from a second
				// reading of the chip's memory. That second reading is what drew a flat line while
				// the same file exported correctly; one source, one answer.
				if (const auto* sample = ledger.lookup (chipState.sampleId, sampleList.get ()))
				{
					state.wave = sample->wave;
					state.sampleRate = sample->rate;
				}
			}

			// A hit is an event, and a snapshot is a sample: the two do not line up for anything
			// that lasts milliseconds. Held here until a snapshot has carried it.
			const uint32_t hitKey = (instance.deviceId << 8) ^ (uint32_t) channel;

			// **One place, every sampled chip: keep the last sample a channel played.**
			//
			// A row draws what the chip reports *now*, and between samples that is nothing - which
			// is most of the time for a short one. `11 Redtailed Cat`'s drums are two thousand bytes
			// at 34 kHz, sixty milliseconds each, so the cell was empty far more often than not and
			// there was nothing to drag. Latching it here rather than in each decoder means one rule
			// for every chip, and the drag reads the same field and gets it for free.
			//
			// Both halves are kept together: a wave with a zero rate passes the eye and fails the
			// export, which asks `sampleRate > 0` before it will write anything.
			{
				DpcmLookup& held = ledger.lastPlayed[hitKey];

				const bool playing = state.wave != nullptr && state.wave->size () >= 2
								  && state.sampleRate > 0.0f;

				// **A channel that is sounding without a wave is not a channel between samples.**
				// A sampled voice goes quiet because its hit ended; a noise generator goes quiet
				// with no shape at all, and it is loud the whole time. Handing it the previous
				// tone's wave made a Mikey noise passage draw the melody's duty cycle - the row said
				// "PWM" while the ear said "noise", and both were right about different frames.
				const bool sounding = chipState.gate || chipState.released;

				if (playing)
				{
					held.wave = state.wave;
					held.rate = state.sampleRate;
				}
				else if (held.wave != nullptr && ! sounding)
				{
					// **The pair, or nothing.** A decoder that has keyed a voice but closed its gate
					// reports a wave with a rate of zero - a cell that draws and cannot be dragged,
					// because the export asks `sampleRate > 0` before it will write anything. Rather
					// than let a half-pair through, the remembered one replaces it.
					//
					// Dim, not lit: `active` is left alone, so the row draws the remembered shape in
					// the quiet colour and nothing pretends the channel is sounding.
					state.wave = held.wave;
					state.sampleRate = held.rate;
				}
			}

			if (chipState.hitCount != ledger.lastHits[hitKey])
			{
				ledger.lastHits[hitKey] = chipState.hitCount;
				ledger.pending[hitKey] = PendingHit { state.note, kHitBlocks };
			}

			auto& hit = ledger.pending[hitKey];

			if (! state.active && hit.blocksLeft > 0)
			{
				state.active = true;

				if (hit.note >= 0)
				{
					state.note = hit.note;
					state.frequency = 440.0f * std::pow (2.0f, (float) (state.note - 69) / 12.0f);
				}

				--hit.blocksLeft;
			}

			snapshot.channels.push_back (std::move (state));
		}
	}

	// **A row exists because a track exists, not because the device was written to.** libvgm creates a
	// device instance the first time a command addresses it, so a channel with nothing to say yet had
	// no instance and no row: the SSG appeared only at its first note, and every row below it moved
	// down by three when it did. Laying the rows out from `tracks` fixes the count before the first
	// note; a track with no state keeps an empty row, which is what the mixer shows for it too.
	if (tracks != nullptr)
	{
		std::vector<playback::ChannelState> rows;
		rows.reserve (tracks->size () + snapshot.channels.size ());

		for (const auto& track : *tracks)
		{
			// **One row per track, and the fullest state wins.** A device that libvgm hands over twice
			// produces two states for the same voice - one of them empty, and not always the same one -
			// so choosing by track means choosing between them rather than emitting both. The score
			// prefers a shape over a bare note, and a note over nothing.
			const playback::ChannelState* best = nullptr;
			int bestScore = -1;

			for (const auto& state : snapshot.channels)
			{
				if (state.trackIndex != (int) track.id)
				{
					continue;
				}

				const int score = (state.wave != nullptr ? 4 : 0) + (state.active ? 2 : 0)
								  + (state.note >= 0 ? 1 : 0);

				if (score > bestScore)
				{
					bestScore = score;
					best = &state;
				}
			}

			if (best != nullptr)
			{
				rows.push_back (*best);
				continue;
			}

			// A voice that has not sounded yet: a row with a name and nothing else, so the page does
			// not gain and lose rows while the song plays.
			playback::ChannelState empty;
			empty.name = track.name.toStdString ();
			empty.chipName = track.groupName.toStdString ();
			empty.trackIndex = (int) track.id;
			empty.note = -1;
			rows.push_back (std::move (empty));
		}

		// **A state no track describes is a layout disagreement, not a row.** Appending it showed the
		// same device twice - the second copy carries its own name, so the page grew a group that the
		// mixer does not have. It is dropped here instead: one row per track, which is the contract.
		snapshot.channels = std::move (rows);
	}

	return true;
}

chips::RenderedNote LibvgmTrackPlayer::renderNoteAt (int track, double hz,
													 const chips::VoicePatch* patch,
													 const chips::NoteRenderShape& shape) const
{
	if (hz <= 0.0)
	{
		return {};
	}

	// **The caller's patch, always.** It already holds the `ChannelState` it wants rendered, so
	// asking this function to find the same voice again was a second lookup over a second numbering
	// scheme - chip channels against mixer rows - and when it missed, the empty answer was
	// indistinguishable from "nothing here". The exporter then kept the panel's 8 kHz shape, and a
	// dragged YM2610 voice came out as a staircase while the same row exported correctly from the
	// batch path.
	//
	// **The fallback lookup is gone.** It read `noteWaves`, which `captureState` writes on the
	// message thread, and this runs on the export pool - a data race nobody could reproduce, kept
	// alive for a caller that does not exist: both export paths now pass the patch.
	(void) track;

	if (patch == nullptr)
	{
		return {};
	}

	const chips::VoicePatch* chosen = patch;

	// **One export at a time.** A batch export runs on `ContentSlot::exportPool` and a drag on the
	// message thread; both arrive here, and the driver below is a single mutable core. Without the
	// lock they would rewrite the same registers from two threads, and the symptom - a garbled
	// waveform, an occasional crash - is not reproducible on demand.
	const juce::ScopedLock lock (exportLock);

	// **Its own core.** The driver `captureState` uses is being advanced by the audio thread; an
	// export that re-rendered through it would rewrite the registers under that thread's feet, and
	// both the picture and the sound would change mid-note.
	//
	// **Per chip, because "what the player does" is not one thing.**
	//
	// `nukedopm` has a fractional resampler of its own: asked for a rate other than `clock / 64` it
	// interpolates **linearly, with no filter** (`nukedopm.c:2367`), so 48 kHz folds the top band in
	// before we see the samples. That is the hiss this chip was fixed for once already
	// (`docs/issue/2026-11-22-the-opm-resamples-itself.md`), and the fix is the one the player uses:
	// `PlayerA` runs the core at its **native** rate and converts with its own filtered resampler. So
	// the export does the same - native here, the low-pass in `WaveExport` after.
	//
	// **The other two were put on the file's rate on purpose**, and both for a listening reason rather
	// than a numerical one: with the FIR in the path the OPL2 came out 1.15 dB quieter and darker at
	// the top than the timeline (`peak 1.000` against `0.874` direct), so the file did not sound like
	// what had just been heard. Their cores interpolate too, but no one has reported hiss on them; if
	// one does, it belongs here beside the OPM.
	//
	// Zero still means the chip's own rate: that is the export setting that asks for it, and the
	// driver reads it as "no conversion".
	const double wantedRate = (chosen->model == chips::ChipModel::ym2151) ? 0.0 : shape.rate;

	// **Rebuilt whenever the requested rate changes, and `0` is a rate.** This compared `> 0.0`
	// before, which read 0 as "do not care" - so a YM2151 export after any other model kept the
	// 48 kHz core and got `nukedopm`'s unfiltered interpolation back, and switching the export-rate
	// setting to `sampleRateChip` was silently ignored. Both are the same mistake: one value with
	// two meanings. `exportRateRequested` keeps the request itself, so 0 compares like any other.
	if (exportDriver == nullptr || exportRateRequested != wantedRate)
	{
		exportDriver = makeFmVoiceDriver (true, wantedRate);
		exportRateRequested = wantedRate;
	}

	// **The shape arrives, it is not looked up.** This runs on the export pool or on a drag;
	// reading `SettingsStore` here would be a second source of the same three values, and a read of
	// a file-backed store from a worker while the message thread may be writing it.
	auto rendered = playback::noteWave::render (*exportDriver, *chosen, hz, shape.sustainSeconds,
											  shape.releaseSeconds, shape.normalise);

	// **The OPM's filtered path loses about 4 dB, and the timeline is the reference.** Measured on the
	// same patch: `rms 0.176` rendered at the chip's rate, `0.145` after `WaveExport`'s low-pass - the
	// band above `0.40 * targetRate` is what the filter is there to remove - and decimation costs the
	// rest. A DAW then put the file 4 dB under the channel, which is this number.
	//
	// **Only when the filter will actually run**: exporting at the chip's own rate bypasses it, and
	// making that case louder would overshoot by the same amount.
	if (chosen->model == chips::ChipModel::ym2151 && wantedRate == 0.0 && shape.rate > 0.0
		&& shape.rate < chips::noteRateFor (chosen->model))
	{
		constexpr float kOpmFilterMakeup = 1.585f;		// +4.0 dB

		for (auto& sample : rendered)
		{
			sample *= kOpmFilterMakeup;
		}
	}

	auto wave = std::make_shared<const std::vector<float>> (std::move (rendered));

	// **The rate travels with the samples.** `exportDriver` was created with `exportQuality`, so
	// its rate is the chip's own - reading it back here is what keeps the file's header honest
	// instead of a second guess at what the samples were made at.
	return { std::move (wave), exportDriver->rate () };
}

} // namespace toybox::backends::libvgm
