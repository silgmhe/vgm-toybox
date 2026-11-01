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

void LibvgmTrackPlayer::setReplaySource (std::shared_ptr<const midi::SongIr> ir,
										 const std::vector<PLR_DEV_INFO>& devices)
{
	replayIr = std::move (ir);
	replayDevices = devices;

	if (replayIr != nullptr)
	{
		buildDpcmNotes (*replayIr);
	}
}

const LibvgmTrackPlayer::DpcmLookup* LibvgmTrackPlayer::lookupDpcmSample (uint32_t sampleId) const
{
	if (sampleId == 0)
	{
		return nullptr;
	}

	const auto cached = ledger.samples.find (sampleId);
	if (cached != ledger.samples.end ())
	{
		return &cached->second;
	}

	if (sampleList == nullptr)
	{
		return nullptr;
	}

	for (const auto& sample : *sampleList)
	{
		if (sample.sampleId != sampleId)
		{
			continue;
		}

		DpcmLookup entry;
		entry.wave = std::make_shared<const std::vector<float>> (sample.values);
		entry.rate = (float) sample.rate;

		return &ledger.samples.emplace (sampleId, std::move (entry)).first->second;
	}

	// Not an error: the collector and the decoder name samples with the same arithmetic, so a miss
	// means this hit's bytes were never collected - a file that carried no ROM for them. The row
	// then draws the rate and no wave, which is the truth about that hit.
	return nullptr;
}

void LibvgmTrackPlayer::buildDpcmNotes (const midi::SongIr& ir)
{
	ledger.notes.clear ();

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

		if (ledger.notes.find (id) == ledger.notes.end () && nextNote <= 127)
		{
			ledger.notes[id] = nextNote++;
		}
	}
}

int LibvgmTrackPlayer::trackFor (uint32_t deviceId, int channel) const
{
	if (tracks == nullptr)
	{
		return -1;
	}

	for (std::size_t i = 0; i < tracks->size (); ++i)
	{
		const auto& track = (*tracks)[i];

		if (track.deviceId != deviceId)
		{
			continue;
		}

		// A whole-device track (channelIndex -1) owns every channel of that device: the core the
		// song picked cannot isolate its voices, so there is nothing finer to point at.
		if (track.channelIndex < 0 || track.channelIndex == channel)
		{
			return (int) i;
		}
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
		replayer = std::make_unique<VgmReplayer> (replayIr, replayDevices);
	}

	if (pendingRewind.exchange (false, std::memory_order_relaxed))
	{
		replayer->rewind ();
	}

	const double seconds = lastPosition.load (std::memory_order_relaxed);
	const auto samples = (uint32_t) std::max (0.0, seconds * replayer->tickRate ());

	replayer->advanceTo (samples);

	snapshot.chips.clear ();
	snapshot.channels.clear ();
	snapshot.positionSeconds = seconds;
	snapshot.valid = true;

	for (const auto& instance : replayer->instances ())
	{
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
			const auto chipState = instance.decoder->channelState (channel);

			if (! chipState.present)
			{
				continue;
			}

			const auto kind = instance.decoder->channelKind (channel);

			playback::ChipShapeHints hints;
			hints.waveKind = playback::waveKindForChannelKind (kind);
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
				channelName = "Ch " + std::to_string (channel + 1);
			}

			auto state = playback::channelFromChip (
				chipState, instance.decoder->noteTable (channel), hints, chipName, channelName);

			// Set here rather than inside the conversion: which engine track a channel belongs to is
			// a fact about this runtime, not about the chip, and the conversion has no engine to ask.
			state.trackIndex = trackFor (instance.deviceId, channel);

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
				if (const auto* sample = lookupDpcmSample (chipState.sampleId))
				{
					state.wave = sample->wave;
					state.sampleRate = sample->rate;
				}
			}

			// A hit is an event, and a snapshot is a sample: the two do not line up for anything
			// that lasts milliseconds. Held here until a snapshot has carried it.
			const uint32_t hitKey = (instance.deviceId << 8) ^ (uint32_t) channel;

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

	return true;
}

} // namespace toybox::backends::libvgm
