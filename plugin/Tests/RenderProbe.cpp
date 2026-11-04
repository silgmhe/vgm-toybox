/**
 *	The measurement probe: what a channel renders, and what its row would hand back.

 *	It began as an RF5C164 question - does the exported sample match the one the channel plays - and
 *	that question is still open for that chip (`docs/memo/2026-11-04-the-sample-that-was-never-the-one-playing.md`).
 *	It outlived the question, because the same four measurements keep answering for other chips:
 *
 *		-2   the core driven alone, with a calibration that must read `-3776 3776 -3776 3776`.
 *		     Written because a harness that cannot prove it hears anything will report silence for a
 *		     chip that is playing - which this one did, for a whole session.
 *		-3   what capturing the display state costs the audio thread, on the same span twice.
 *		-4   the rows as the UI would draw them, captured while playing: the wave each channel holds
 *		     and the rate it declares. This is what a drag writes out.
 *		<n>  one track in detail, or -1 for all of them: a render, its note, and how it compares with
 *		     the slot the export would take.
 *
 *	`-4` is the one that settled the OKIM6295 export: the row held 3714 levels where the File menu
 *	wrote 1858, and the two numbers came from different arithmetic on the same ROM table.
 *
 *	Build it explicitly - it is `EXCLUDE_FROM_ALL` and never ships:
 *
 *		cmake --build build --target vgm_render_probe
 *		vgm_render_probe <song> -4 3.0
 *		vgm_render_probe <song> -1 3.0
 *		vgm_render_probe <song> 4 1.0
 *
 *	Nothing in the product calls it.
 */
#include <juce_core/juce_core.h>

#include "backends/libvgm/LibvgmVgz.h"
#include "backends/libvgm/VgmCommandReader.h"

#include "emu/EmuStructs.h"
#include "emu/SoundDevs.h"
#include "emu/SoundEmu.h"

extern "C" DEV_DEF devDef_RF5C68_Gens;	// scd_pcm.c: the Gens core, taken directly
#include "playback/ChipState.h"
#include "playback/PlaybackEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <vector>

/**
 * Renders one channel of a song and compares it, note by note, with the sample the display
 * believes that channel plays.
 *
 * The point of this program is that it is **not** a unit test. Everything else in this branch
 * argues from the register stream: which `0x06` write set the start, which `0x08` edge opened the
 * gate, what rate the `0x02`/`0x03` pair asks for. Those arguments have been wrong about this chip
 * three times. This one asks the emulator instead: solo a channel, render it, and measure what
 * actually came out - then measure the wave the drag export would write and put the two numbers
 * next to each other. If they disagree, the disagreement is a fact, not an inference.
 *
 *   build <dir>/plugin/vgm_render_probe <song.vgm|vgz> [trackIndex] [seconds]
 *
 * `trackIndex` of -1 (the default) walks every track.
 */

namespace
{
constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 512;

/** One stretched stretch of audio above the noise floor. */
struct NoteWindow
{
	int start = 0;			// first sample
	int length = 0;			// samples
	double peak = 0.0;
	double pitch = 0.0;		// Hz, 0 when the autocorrelation found nothing usable
};

/** `seconds` of the engine's stereo output, left channel only. */
std::vector<float> renderSeconds (toybox::PlaybackEngine& engine, double seconds)
{
	std::vector<float> out;
	out.reserve ((size_t) (seconds * kSampleRate));

	std::vector<float> blockL ((size_t) kBlockSize), blockR ((size_t) kBlockSize);
	toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };

	const size_t total = (size_t) (seconds * kSampleRate);

	while (out.size () < total && ! engine.isFinished ())
	{
		std::fill (blockL.begin (), blockL.end (), 0.0f);
		std::fill (blockR.begin (), blockR.end (), 0.0f);
		engine.render (buses, 1, kBlockSize);

		for (int i = 0; i < kBlockSize && out.size () < total; ++i)
		{
			out.push_back (blockL[(size_t) i]);
		}
	}

	return out;
}

/** Normalised autocorrelation over a musical range of lags; returns the strongest period in Hz. */
double pitchOf (const std::vector<float>& samples, int from, int length, double minHz, double maxHz)
{
	const int minLag = (int) std::floor ((double) kSampleRate / maxHz);
	const int maxLag = (int) std::floor ((double) kSampleRate / minHz);
	const int span = juce::jmin (length, 8192);

	if (span < minLag * 4)
	{
		return 0.0;
	}

	double best = -2.0;
	int bestLag = 0;

	for (int lag = minLag; lag <= maxLag && lag < span / 2; ++lag)
	{
		double num = 0.0;
		double d1 = 0.0;
		double d2 = 0.0;

		for (int i = 0; i < span - lag; ++i)
		{
			const double a = samples[(size_t) (from + i)];
			const double b = samples[(size_t) (from + i + lag)];
			num += a * b;
			d1 += a * a;
			d2 += b * b;
		}

		if (d1 > 0.0 && d2 > 0.0)
		{
			const double r = num / std::sqrt (d1 * d2);

			if (r > best)
			{
				best = r;
				bestLag = lag;
			}
		}
	}

	return (bestLag > 0) ? (double) kSampleRate / bestLag : 0.0;
}

/** The first stretch that rises above the floor and then falls back under it. */
NoteWindow firstNote (const std::vector<float>& samples)
{
	NoteWindow window;

	constexpr int kWindow = 256;
	const int windows = (int) samples.size () / kWindow;

	std::vector<double> energy ((size_t) windows, 0.0);
	double loudest = 0.0;

	for (int w = 0; w < windows; ++w)
	{
		double sum = 0.0;

		for (int i = 0; i < kWindow; ++i)
		{
			const double v = samples[(size_t) (w * kWindow + i)];
			sum += v * v;
		}

		energy[(size_t) w] = std::sqrt (sum / kWindow);
		loudest = juce::jmax (loudest, energy[(size_t) w]);
	}

	if (loudest <= 0.0)
	{
		return window;
	}

	const double floorLevel = loudest * 0.02;
	int first = -1;

	for (int w = 0; w < windows; ++w)
	{
		if (energy[(size_t) w] > floorLevel)
		{
			first = w;
			break;
		}
	}

	if (first < 0)
	{
		return window;
	}

	int last = first;

	for (int w = first; w < windows; ++w)
	{
		if (energy[(size_t) w] > floorLevel)
		{
			last = w;
		}
		// Sixty-four milliseconds of quiet ends a note. Three windows (16 ms) was too eager: a
		// percussive channel retriggered every 124 ms came out as one 491 ms note, and the export was
		// then blamed for being a quarter of its length.
		else if (w - last >= 12)
		{
			break;
		}
	}

	window.start = first * kWindow;
	window.length = (last - first + 1) * kWindow;
	window.peak = loudest;
	window.pitch = pitchOf (samples, window.start, window.length, 30.0, 3000.0);

	return window;
}

/** The wave the drag export would write: period in samples, and the pitch that implies. */
struct ExportShape
{
	bool present = false;
	size_t length = 0;
	double rate = 0.0;
	int period = 0;
	double impliedPitch = 0.0;
};

ExportShape shapeOf (const toybox::playback::ChannelState& channel)
{
	ExportShape shape;

	if (channel.wave == nullptr || channel.wave->size () < 8)
	{
		return shape;
	}

	shape.present = true;
	shape.length = channel.wave->size ();
	shape.rate = (double) channel.sampleRate;

	// The wave's own period, in its own samples, by autocorrelation.
	double best = -2.0;

	for (int lag = 2; lag < (int) shape.length / 2; ++lag)
	{
		double num = 0.0;
		double d1 = 0.0;
		double d2 = 0.0;

		for (size_t i = 0; i + (size_t) lag < shape.length; ++i)
		{
			const double a = (double) (*channel.wave)[i];
			const double b = (double) (*channel.wave)[i + (size_t) lag];
			num += a * b;
			d1 += a * a;
			d2 += b * b;
		}

		if (d1 > 0.0 && d2 > 0.0)
		{
			const double r = num / std::sqrt (d1 * d2);

			if (r > best)
			{
				best = r;
				shape.period = lag;
			}
		}
	}

	if (shape.period > 0 && shape.rate > 0.0)
	{
		shape.impliedPitch = shape.rate / (double) shape.period;
	}

	return shape;
}

/** The sixty-four kilobytes the chip addresses, from the `0x02` blocks of the file. */
std::vector<uint8_t> ramImage (const juce::File& file)
{
	juce::MemoryBlock raw;

	if (! file.loadFileAsData (raw))
	{
		return {};
	}

	// The plugin's own inflater: the probe must not grow a dependency the plugin lacks.
	juce::MemoryBlock plain;

	if (! toybox::backends::libvgm::flattenCompressedImage (raw.getData (), raw.getSize (), plain))
	{
		return {};
	}

	const auto* begin = static_cast<const uint8_t*> (plain.getData());
	std::vector<uint8_t> image (begin, begin + plain.getSize ());

	if (image.size () < 0x40)
	{
		return {};
	}

	toybox::backends::libvgm::VgmCommandReader reader (image.data (), image.size ());
	toybox::backends::libvgm::VgmCommand command;

	while (reader.next (command))
	{
	}

	// The bank, laid out by the file's own `0x68` copies rather than assumed at zero.
	//
	// The emulator's memory was measured to hold `chip[0x8073] == bank[0x1473]`, i.e. the bank was
	// written starting at chip address **0x6C00** - which is what a `0x68` command says. Cutting the
	// bank at 64 KB (what this used to do) answered `99 19 83 97` at 0x8000, a different sample from
	// the one the chip plays there, and that difference is the "drum instead of piano" report.
	std::vector<uint8_t> bank;
	std::vector<uint8_t> ram (64 * 1024, 0x80);

	for (const auto& block : reader.dataBlocks())
	{
		if (block.type == 0x02)
		{
			bank.insert (bank.end (), block.data, block.data + block.size);
		}
	}

	// **The layout the emulator actually produces.** Measured from the chip's own memory: its voice
	// was reading `8A 8A 87 83` at 0x8073, and those bytes sit at 0x1473 in the bank - so the bank
	// was written starting at chip address 0x6C00 and wrapped, leaving the last write at each
	// address. For this file 0x6C00 = 0x26C00 - 2 * 0x10000, the bank's size past two whole windows.
	//
	// The `0x68` commands that should have said this are unusable: the reader finds 25447 of them,
	// the first reporting `type=0x66, addr=0, len=0x100000` and then every address a megabyte apart -
	// it is walking into `0x67` block payloads and reading them as commands. Deriving the layout from
	// the bank's size is what the measurement supports; the parser is a separate defect.
	const std::size_t base = bank.size () % (2 * ram.size ());

	// **Written from `base` and cut at the top of the chip's memory - not wrapped.** The channel
	// reads `bank[0x1473]` at chip 0x8073, which is `base + 0x1473`; a wraparound also fits that one
	// address but then overwrites it with `bank[0x11473]`, and the emulator's byte there is the
	// first one. Cutting is what matches.
	for (std::size_t i = 0; i + base < ram.size () && i < bank.size (); ++i)
	{
		ram[base + i] = bank[i];
	}

	std::printf ("   layout  : bank %u B from chip 0x%zX, copies %d; bank[0x1473]=%02X ours[0x8073]=%02X\n",
				 (unsigned) bank.size (), base, (int) reader.ramCopies ().size (),
				 (bank.size () > 0x1474) ? bank[0x1473] : 0, ram[0x8073]);

	return ram;
}

/** Writes mono float samples as a 16-bit PCM wav.

	The point is a file someone can listen to. Every number in this branch has been argued about at
	least twice; a wav of what the engine actually rendered settles "what does this channel sound
	like" without an instrument in between.
*/
bool writeWav (const juce::File& file, const std::vector<float>& mono, double rate)
{
	if (file.existsAsFile ())
	{
		file.deleteFile ();
	}

	juce::FileOutputStream out (file);

	if (! out.openedOk ())
	{
		return false;
	}

	const uint32_t dataBytes = (uint32_t) mono.size () * 2u;
	const uint32_t rate32 = (uint32_t) rate;

	out.write ("RIFF", 4);
	out.writeInt ((int) (36u + dataBytes));
	out.write ("WAVEfmt ", 8);
	out.writeInt (16);					// fmt chunk size
	out.writeShort (1);					// PCM
	out.writeShort (1);					// mono
	out.writeInt ((int) rate32);
	out.writeInt ((int) (rate32 * 2u));	// byte rate
	out.writeShort (2);					// block align
	out.writeShort (16);				// bits
	out.write ("data", 4);
	out.writeInt ((int) dataBytes);

	for (const auto sample : mono)
	{
		const auto scaled = (int) juce::jlimit (-32768.0f, 32767.0f, sample * 32767.0f);
		out.writeShort ((short) scaled);
	}

	return true;
}

/** Loudness in `bins` equal slices, normalised so the shape - not the gain - is what is compared. */
std::vector<double> envelopeOf (const std::vector<float>& samples, int from, int count, int bins)
{
	std::vector<double> envelope ((size_t) bins, 0.0);
	double loudest = 0.0;

	for (int b = 0; b < bins; ++b)
	{
		const int begin = from + count * b / bins;
		const int end = from + count * (b + 1) / bins;

		double sum = 0.0;
		int n = 0;

		for (int i = begin; i < end && i < (int) samples.size (); ++i)
		{
			sum += (double) samples[(size_t) i] * samples[(size_t) i];
			++n;
		}

		envelope[(size_t) b] = (n > 0) ? std::sqrt (sum / n) : 0.0;
		loudest = juce::jmax (loudest, envelope[(size_t) b]);
	}

	if (loudest > 0.0)
	{
		for (auto& v : envelope)
		{
			v /= loudest;
		}
	}

	return envelope;
}

/** The same shape, from sign-magnitude bytes. */
std::vector<double> envelopeOfBytes (const uint8_t* bytes, int count, int bins)
{
	std::vector<double> envelope ((size_t) bins, 0.0);
	double loudest = 0.0;

	for (int b = 0; b < bins; ++b)
	{
		const int begin = count * b / bins;
		const int end = count * (b + 1) / bins;

		double sum = 0.0;
		int n = 0;

		for (int i = begin; i < end; ++i)
		{
			const double level = (double) (bytes[i] & 0x7F) / 128.0;
			sum += level * level;
			++n;
		}

		envelope[(size_t) b] = (n > 0) ? std::sqrt (sum / n) : 0.0;
		loudest = juce::jmax (loudest, envelope[(size_t) b]);
	}

	if (loudest > 0.0)
	{
		for (auto& v : envelope)
		{
			v /= loudest;
		}
	}

	return envelope;
}

/** Sign changes per second - the cheapest way to tell a tone from noise.

	A hi-hat is broadband: it crosses zero thousands of times a second and its waveform looks like
	noise at any rate. A piano or a bass crosses at roughly twice its fundamental and looks smooth.
	The number does not depend on the rate the bytes are read at in the way a waveform comparison
	does, which is why it can settle "is this slot the same instrument" without knowing the rate.
*/
double crossingRate (const std::vector<float>& samples, int from, int count)
{
	int crossings = 0;

	for (int i = from + 1; i < from + count && i < (int) samples.size (); ++i)
	{
		if ((samples[(size_t) i] >= 0.0f) != (samples[(size_t) (i - 1)] >= 0.0f))
		{
			++crossings;
		}
	}

	const double seconds = (double) count / (double) kSampleRate;

	return (seconds > 0.0) ? (double) crossings / seconds : 0.0;
}

/** The same for sign-magnitude bytes, at a nominal rate - the shape, not the speed. */
double crossingRateOfBytes (const uint8_t* bytes, int count)
{
	int crossings = 0;
	bool previous = (bytes[0] & 0x80) != 0;

	for (int i = 1; i < count; ++i)
	{
		const bool current = (bytes[i] & 0x80) != 0;

		if (current != previous)
		{
			++crossings;
		}

		previous = current;
	}

	// Per byte rather than per second: the rate is exactly what is in doubt, so the answer is
	// reported as "sign changes in N bytes" and compared with the render's own period.
	return (double) crossings / (double) count;
}

/** Pearson correlation of two envelopes: 1 is the same shape, 0 unrelated, -1 inverted. */
double shapeScore (const std::vector<double>& a, const std::vector<double>& b)
{
	const size_t n = juce::jmin (a.size (), b.size ());

	if (n == 0)
	{
		return 0.0;
	}

	double meanA = 0.0;
	double meanB = 0.0;

	for (size_t i = 0; i < n; ++i)
	{
		meanA += a[i];
		meanB += b[i];
	}

	meanA /= (double) n;
	meanB /= (double) n;

	double num = 0.0;
	double d1 = 0.0;
	double d2 = 0.0;

	for (size_t i = 0; i < n; ++i)
	{
		const double x = a[i] - meanA;
		const double y = b[i] - meanB;
		num += x * y;
		d1 += x * x;
		d2 += y * y;
	}

	return (d1 > 0.0 && d2 > 0.0) ? num / std::sqrt (d1 * d2) : 0.0;
}
} // namespace

/**
 * Drives the GENS RF5C164 core by itself: no player, no mixer, no mute masks.

	Everything this branch measured went through the engine, and the engine kept producing results
	that contradicted the core's own memory. This path removes every layer between the bytes and the
	output: give the chip our 64 KB image and one note's registers, ask it for half a second of audio,
	and look at what comes out. If the core plays the sample, the image is right and the engine is the
	thing to look at; if it plays silence, the image is wrong and no amount of engine work will fix it.
*/
int runCoreProbe (const juce::File& file, const std::vector<uint8_t>& ram)
{
	// **Calibration first, and with a waveform whose answer is known by hand.** The harness has to
	// be able to say "the chip is silent" rather than "my harness is silent", and the way to earn
	// that is to make it reproduce something predictable: `0xC0` is +0.5 and `0x40` is -0.5 in
	// sign-magnitude, so alternating the two and stepping one byte per output sample must come out
	// as a square wave at half of full scale.
	{
		DEV_GEN_CFG cfg = {};
		cfg.emuCore = devDef_RF5C68_Gens.coreID;
		cfg.srMode = DEVRI_SRMODE_NATIVE;
		cfg.flags = 1;
		cfg.clock = 12500000;
		cfg.smplRate = 48000;

		DEV_INFO info = {};

		if (SndEmu_Start2 (DEVID_RF5C68, &cfg, &info, nullptr, 0) == 0x00 && info.dataPtr != nullptr)
		{
			DEVFUNC_WRITE_A8D8 w = nullptr;
			DEVFUNC_WRITE_A16D8 m = nullptr;
			SndEmu_GetDeviceFunc (info.devDef, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, (void**) &w);
			SndEmu_GetDeviceFunc (info.devDef, RWF_MEMORY | RWF_WRITE, DEVRW_A16D8, 0, (void**) &m);

			for (uint32_t i = 0; i < 0x10000; ++i)
			{
				m (info.dataPtr, (uint16_t) i, ((i & 1u) != 0) ? 0x40 : 0xC0);
			}

			w (info.dataPtr, 0x07, 0xC0);			// master on, channel 0
			w (info.dataPtr, 0x06, 0x00);			// start 0x0000
			w (info.dataPtr, 0x04, 0x00);			// loop 0x0000
			w (info.dataPtr, 0x05, 0x00);
			w (info.dataPtr, 0x02, 0x00);			// step 0x0800 = one byte per output sample
			w (info.dataPtr, 0x03, 0x08);
			// Pan **before** envelope: the core recomputes its per-side multipliers when the envelope is
			// written, so an envelope written first against pan 0 stays at zero. With either left at zero the
			// core multiplies its output by zero, which reads as silence.
			w (info.dataPtr, 0x01, 0xFF);			// pan 15/15
			w (info.dataPtr, 0x00, 0x7F);			// envelope: full scale
			w (info.dataPtr, 0x00, 0x7F);			// again, now that pan is set
			w (info.dataPtr, 0x08, 0xFE);			// channel 0 on

			std::vector<int32_t> l (16), r (16);
			DEV_SMPL* out[2] = { l.data (), r.data () };
			devDef_RF5C68_Gens.Update (info.dataPtr, 16, out);

			std::printf ("\ncalibration (0xC0/0x40 alternating, step 2048):");
			for (int i = 0; i < 8; ++i)
			{
				std::printf (" %d", (int) l[(size_t) i]);
			}
			std::printf ("  <- expect alternating signs of equal size (the core is not int32-scaled)\n");

			// Which core actually started, next to the one being pumped: if these differ, every
			// "the chip is silent" reading is a reading of the wrong structure.
			std::printf ("   core started: id=0x%X | pumped: id=0x%X | same=%d\n",
						 (unsigned) info.devDef->coreID,
						 (unsigned) devDef_RF5C68_Gens.coreID,
						 (info.devDef == &devDef_RF5C68_Gens) ? 1 : 0);

			SndEmu_Stop (&info);
		}
	}

	DEV_GEN_CFG cfg = {};
	// Forced, not left to the device: this probe pumps the core with `devDef_RF5C68_Gens.Update`, and
	// if the device started the MAME core instead, that call would be reading another core's state -
	// which produces silence rather than an error, and would have been read as "the image is empty".
	cfg.emuCore = devDef_RF5C68_Gens.coreID;
	cfg.srMode = DEVRI_SRMODE_NATIVE;
	cfg.flags = 1;				// RF5C164
	cfg.clock = 12500000;
	cfg.smplRate = 48000;

	DEV_INFO info = {};

	if (SndEmu_Start2 (DEVID_RF5C68, &cfg, &info, nullptr, 0) != 0x00 || info.dataPtr == nullptr)
	{
		std::printf ("core did not start\n");
		return 1;
	}

	DEVFUNC_WRITE_A8D8 writeReg = nullptr;
	DEVFUNC_WRITE_A16D8 writeMem = nullptr;
	DEVFUNC_WRITE_BLOCK writeBlock = nullptr;
	DEVFUNC_UPDATE update = nullptr;

	SndEmu_GetDeviceFunc (info.devDef, RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, (void**) &writeReg);
	SndEmu_GetDeviceFunc (info.devDef, RWF_MEMORY | RWF_WRITE, DEVRW_A16D8, 0, (void**) &writeMem);
	SndEmu_GetDeviceFunc (info.devDef, RWF_MEMORY | RWF_WRITE, DEVRW_BLOCK, 0, (void**) &writeBlock);
	update = devDef_RF5C68_Gens.Update;

	if (writeReg == nullptr || writeMem == nullptr || update == nullptr)
	{
		std::printf ("core has no register/memory/update function\n");
		SndEmu_Stop (&info);
		return 1;
	}

	// Our image, through the same entry the player uses for a bank.
	if (writeBlock != nullptr && ! ram.empty ())
	{
		writeBlock (info.dataPtr, 0, (uint32_t) ram.size (), ram.data ());
	}
	else
	{
		for (uint32_t offset = 0; offset < ram.size (); ++offset)
		{
			writeMem (info.dataPtr, (uint16_t) offset, ram[offset]);
		}
	}

	// One note on voice 4, the way the song does it: start 0x8000, step 657, bit 4 cleared.
	writeReg (info.dataPtr, 0x07, 0xC4);			// master on, select channel 4
	writeReg (info.dataPtr, 0x06, 0x80);			// start 0x8000, in 256-byte units
	writeReg (info.dataPtr, 0x02, (uint8_t) (657 & 0xFF));
	writeReg (info.dataPtr, 0x03, (uint8_t) (657 >> 8));
	writeReg (info.dataPtr, 0x01, 0xFF);			// pan 15/15 - without it the multipliers stay zero
	writeReg (info.dataPtr, 0x00, 0x7F);			// envelope, full scale
	writeReg (info.dataPtr, 0x00, 0x7F);			// again, after pan (see the note above)
	writeReg (info.dataPtr, 0x08, (uint8_t) ~(1u << 4));

	const int blocks = 64;
	const int blockSize = 512;
	std::vector<int32_t> left ((size_t) blockSize), right ((size_t) blockSize);
	DEV_SMPL* outputs[2] = { left.data (), right.data () };

	double peak = 0.0;
	int crossings = 0;
	double previous = 0.0;
	std::vector<float> mono;

	for (int b = 0; b < blocks; ++b)
	{
		std::fill (left.begin (), left.end (), 0);
		std::fill (right.begin (), right.end (), 0);
		update (info.dataPtr, blockSize, outputs);

		for (int i = 0; i < blockSize; ++i)
		{
			const double v = (double) left[(size_t) i];
			mono.push_back ((float) v);
			peak = juce::jmax (peak, std::abs (v));

			if ((v >= 0.0) != (previous >= 0.0))
			{
				++crossings;
			}

			previous = v;
		}
	}

	const double seconds = (double) mono.size () / 48000.0;

	// What the core was actually given, next to what came out: silence from an image that holds a
	// sample at that address means this harness is wrong, not the image.
	std::printf ("\n   image at 0x8000: %02X %02X %02X %02X\n",
				 ram[0x8000], ram[0x8001], ram[0x8002], ram[0x8003]);

	std::printf ("\ncore alone: %d samples (%.3f s), peak %.0f (raw), %.0f crossings/s\n",
				 (int) mono.size (), seconds, peak, (double) crossings / seconds);

	// The first sixteen output samples, so a silent chip and a quiet one are told apart.
	std::printf ("   first samples:");
	for (int i = 0; i < 16 && i < (int) mono.size (); ++i)
	{
		std::printf (" %.0f", mono[(size_t) i]);
	}
	std::printf ("\n");

	SndEmu_Stop (&info);
	return 0;
}

/**
 *	Real-time factor, with state capture on and with it off.

	`captureState` runs on the audio thread on every block while the keyboard page is visible, and
	it is not cheap: it advances the replayer cursor and asks every decoder in the song for its
	channel state. The design doc has carried "performance not measured" since the first commit,
	and it is the only item on that list that can turn a correct feature into a stuttering one.

	Two passes over the same span, differing only in the capture flag, so the difference between
	them is the cost being asked about. A factor well above 1 means the block budget is not at
	risk; a factor near 1 means a busy machine will drop blocks.
*/
int runPerf (toybox::PlaybackEngine& engine, double seconds)
{
	const size_t total = (size_t) (seconds * kSampleRate);

	std::vector<float> blockL ((size_t) kBlockSize), blockR ((size_t) kBlockSize);
	toybox::playback::OutputBusBuffers buses[1] = { { blockL.data(), blockR.data() } };

	// **Warm-up first, then alternate.** Comparing one pass against the next measures the caches:
	// an earlier version of this ran capture-off and then capture-on and concluded that capturing
	// made rendering faster. Three rounds each way, alternating, and the best of each is the CPU cost
	// without the allocator and the cache still filling in.
	const auto once = [&] (bool capture) -> double
	{
		engine.setStateCaptureWanted (capture);
		engine.setPositionSeconds (0.0);
		engine.play ();

		size_t frames = 0;
		size_t blocks = 0;

		const auto start = juce::Time::getHighResolutionTicks ();

		while (frames < total && ! engine.isFinished ())
		{
			std::fill (blockL.begin (), blockL.end (), 0.0f);
			std::fill (blockR.begin (), blockR.end (), 0.0f);
			engine.render (buses, 1, kBlockSize);
			frames += (size_t) kBlockSize;
			++blocks;
		}

		const auto stop = juce::Time::getHighResolutionTicks ();
		const double wall = juce::Time::highResolutionTicksToSeconds (stop - start);

		engine.stop ();
		return wall;
	};

	once (true);		// allocations, JIT'd dispatch, first-touch pages - not measured

	double bestWith = 1.0e9;
	double bestWithout = 1.0e9;

	for (int round = 0; round < 3; ++round)
	{
		bestWithout = juce::jmin (bestWithout, once (false));
		bestWith = juce::jmin (bestWith, once (true));
	}

	const double audio = (double) total / kSampleRate;

	for (int pass = 0; pass < 2; ++pass)
	{
		const bool capture = (pass == 1);
		const double wall = capture ? bestWith : bestWithout;

		std::printf ("  capture=%d  %.1f s audio in %.3f s wall  RTF %.1fx  %.1f us/block\n",
					 capture ? 1 : 0, audio, wall, (wall > 0.0) ? audio / wall : 0.0,
					 (wall > 0.0) ? wall * 1.0e6 / (audio * kSampleRate / kBlockSize) : 0.0);
	}

	if (bestWith > 0.0 && bestWithout > 0.0)
	{
		std::printf ("  capture costs %.2fx the render it sits on (%.1f%% of the block)\n",
					 bestWith / bestWithout, 100.0 * (bestWith - bestWithout) / bestWith);
	}

	engine.setStateCaptureWanted (false);
	return 0;
}

/**
 *	The rows as the UI would draw them, captured **while playing**.

	The per-track reports capture after `stop()`, and a stopped engine has no snapshot - which is
	why their channel state comes back empty and every `wave` size reads zero. This plays, captures
	inside playback, and prints what each channel is holding: the whole point is to compare that
	against what the sample collector hands the File menu, because the two are separate producers of
	"this channel's bytes" and a disagreement between them is invisible from either side alone.
*/
int runLiveRows (toybox::PlaybackEngine& engine, double seconds, int onlyTrack)
{
	engine.setStateCaptureWanted (true);
	engine.setPositionSeconds (0.0);
	engine.play ();

	const auto audio = renderSeconds (engine, seconds);

	toybox::playback::ChipStateSnapshot snapshot;
	engine.captureChipState (snapshot);

	const auto& tracks = engine.getTracks ();

	std::printf ("  audio %d samples, snapshot valid=%d, %d channels\n",
				 (int) audio.size (), snapshot.valid ? 1 : 0, (int) snapshot.channels.size ());

	for (size_t i = 0; i < snapshot.channels.size (); ++i)
	{
		const auto& channel = snapshot.channels[i];

		if (onlyTrack >= 0 && (int) i != onlyTrack)
		{
			continue;
		}

		const std::string name = (i < tracks.size ()) ? tracks[i].name.toStdString () : std::string ();

		std::printf ("    [%2d] %-14s wave=%5d pts rate=%8.1f note=%3d hz=%8.1f active=%d\n", (int) i,
					 name.c_str (), (channel.wave != nullptr) ? (int) channel.wave->size () : 0,
					 (double) channel.sampleRate, channel.note, (double) channel.frequency,
					 channel.active ? 1 : 0);
	}

	engine.stop ();
	engine.setStateCaptureWanted (false);
	return 0;
}

int main (int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf ("usage: %s <song.vgm|vgz> [trackIndex] [seconds]\n", argv[0]);
		return 1;
	}

	toybox::PlaybackEngine engine;
	engine.prepare ((double) kSampleRate, kBlockSize);

	juce::String error;

	if (! engine.loadFile (juce::File (juce::String (argv[1])), error))
	{
		std::printf ("load failed: %s\n", error.toRawUTF8 ());
		return 1;
	}

	// Mixer mode, or every solo renders the plain player and the rows never publish state.
	engine.setMixerEnabled (true);

	const auto& tracks = engine.getTracks ();
	const int wanted = (argc > 2) ? std::atoi (argv[2]) : -1;
	const double seconds = (argc > 3) ? std::atof (argv[3]) : 4.0;
	const std::vector<uint8_t> ram = ramImage (juce::File (juce::String (argv[1])));

	std::printf ("ram    : %d bytes\n", (int) ram.size ());

	// -2 asks for the core on its own: no player, no mixer, no mute masks.
	if (wanted == -2)
	{
		return runCoreProbe (juce::File (juce::String (argv[1])), ram);
	}

	// -3 asks what the display costs the audio thread, which the design doc has never measured.
	if (wanted == -3)
	{
		std::printf ("song: %s   tracks: %d   span: %.1f s\n", argv[1], (int) tracks.size (), seconds);
		return runPerf (engine, seconds);
	}

	// -4 prints the rows captured while playing, which is what a drag would export.
	if (wanted == -4)
	{
		const int only = (argc > 4) ? std::atoi (argv[4]) : -1;
		std::printf ("song: %s   tracks: %d   at %.1f s\n", argv[1], (int) tracks.size (), seconds);
		return runLiveRows (engine, seconds, only);
	}

	std::printf ("song: %s   tracks: %d   rendered: %.1f s\n", argv[1], (int) tracks.size (), seconds);

	for (size_t track = 0; track < tracks.size (); ++track)
	{
		if (wanted >= 0 && (int) track != wanted)
		{
			continue;
		}

		/** Renders one track, isolated by solo or by muting the others.

			**Two paths on purpose.** Solo and mute reach the same mixer through different calls, and
			one of them may not be applying the single-channel mask at all: a track rendered under
			solo came out six seconds long and at the loudest peak of the song, which is what the whole
			mix looks like, while another came out as a single quiet note. If the two paths disagree,
			the isolation is the bug and every "this channel sounds like X" report on this chip has
			been about the wrong channel.
		*/
		const auto renderTrack = [&] (bool bySolo) -> std::vector<float>
		{
			engine.setPositionSeconds (0.0);
			engine.play ();

			// **Set after `play()`, not before.** Starting the transport rebuilds the players, and
			// the rebuild resets `soloCount` while the per-track flags survive: `isTrackSoloed` then
			// returns true, the mixer's `anySolo` is false, and every unmuted track passes through.
			// That is what made a "soloed" render six seconds of the whole song, and later a 0.49 s
			// note from a chip whose memory is silence.
			for (size_t other = 0; other < tracks.size (); ++other)
			{
				engine.setTrackSolo (other, bySolo && other == track);
				engine.setTrackMute (other, ! bySolo && other != track);
			}

			if (bySolo)
			{
				const size_t other = (track == 0) ? 1 : 0;

				std::printf ("     before : soloed=%d other-soloed=%d other-muted=%d\n",
							 engine.isTrackSoloed (track) ? 1 : 0,
							 (tracks.size () > 1 && engine.isTrackSoloed (other)) ? 1 : 0,
							 (tracks.size () > 1 && engine.isTrackMuted (other)) ? 1 : 0);
			}

			auto rendered = renderSeconds (engine, seconds);

			engine.stop ();

			for (size_t other = 0; other < tracks.size (); ++other)
			{
				engine.setTrackSolo (other, false);
				engine.setTrackMute (other, false);
			}

			return rendered;
		};

		const std::vector<float> audio = renderTrack (true);
		const std::vector<float> muted = renderTrack (false);

		// Before stop() tears the players down - which it has already done - the snapshot is taken
		// from the last published block by the audio thread, so it is read while playing instead.
		toybox::playback::ChipStateSnapshot snapshot;
		engine.captureChipState (snapshot);

		// A listenable artifact, next to the numbers.
		{
			char name[64] = {};
			std::snprintf (name, sizeof (name), "build/render_ch%d.wav", (int) track + 1);
			writeWav (juce::File::getCurrentWorkingDirectory ().getChildFile (name), audio, kSampleRate);
		}

		const NoteWindow note = firstNote (audio);

		const toybox::playback::ChannelState* channel = nullptr;

		for (size_t i = 0; i < snapshot.channels.size (); ++i)
		{
			if ((int) i == (int) track)
			{
				channel = &snapshot.channels[i];
			}
		}

		std::printf ("\n[%2d] %-20s device=%u channel=%d isolatable=%d\n",
					 (int) track, tracks[track].name.toRawUTF8 (),
					 (unsigned) tracks[track].deviceId, (int) tracks[track].channelIndex,
					 tracks[track].isolatable ? 1 : 0);

		// What the decoder handed the row: a sampled channel draws these points, and the rate is
		// what a dragged-out file has to declare. Zero points with a non-zero rate is a decoder
		// that knows the sample exists but cannot reach its bytes.
		if (channel != nullptr)
		{
			std::printf ("     wave   : %d points, rate %.0f Hz, active %d\n",
						 (channel->wave != nullptr) ? (int) channel->wave->size () : 0,
						 (double) channel->sampleRate, channel->active ? 1 : 0);
		}

		{
			const NoteWindow byMute = firstNote (muted);

			std::printf ("     solo   : %.3f s, peak %.3f, %.0f cross/s  [soloed=%d other-also-soloed=%d other-muted=%d]\n",
						 (double) note.length / kSampleRate, note.peak,
						 crossingRate (audio, note.start, juce::jmax (1, note.length)),
						 engine.isTrackSoloed (track) ? 1 : 0,
						 (tracks.size () > 1 && engine.isTrackSoloed (track == 0 ? 1 : 0)) ? 1 : 0,
						 (tracks.size () > 1 && engine.isTrackMuted (track == 0 ? 1 : 0)) ? 1 : 0);
			std::printf ("     mute   : %.3f s, peak %.3f, %.0f cross/s\n",
						 (double) byMute.length / kSampleRate, byMute.peak,
						 crossingRate (muted, byMute.start, juce::jmax (1, byMute.length)));
		}

		if (note.length <= 0)
		{
			std::printf ("     render : silence\n");
		}
		else
		{
			std::printf ("     render : note at %.3f s, %.3f s long, peak %.3f, pitch %.1f Hz\n",
						 (double) note.start / kSampleRate, (double) note.length / kSampleRate,
						 note.peak, note.pitch);
		}

		if (note.length > 0 && ! ram.empty ())
		{
			// **Which slot, at which rate, is the sound that just came out of the speaker.**
			// The register arguments have been wrong three times; this asks the bytes directly.
			//
			// **Known limitation, and it makes the numbers below useless as a verdict**: a periodic
			// signal decorrelates completely once the rate is off by more than about one percent over
			// the window (thirty-six cycles of a 71 Hz tone in half a second), and this grid steps in
			// tens of percent. Every r here is therefore near zero whether or not the bytes are right.
			// What it does show is which slots are not obviously unrelated; a rate has to be found by
			// measuring a period, not by searching a grid this coarse.
			static const double kRates[] = { 2000, 3000, 4000, 5000, 6500, 8000, 10000, 12500,
											 15000, 18000, 22000, 26000, 30000, 32552 };

			struct Match { double r; uint32_t slot; double rate; };
			std::vector<Match> matches;

			const std::vector<double> want = envelopeOf (audio, note.start, note.length, 32);
			const double seconds = (double) note.length / (double) kSampleRate;

			// **The shape, not the waveform.** A periodic signal decorrelates the moment the rate is
			// off by a percent, so comparing samples only ever compared rates. The loudness envelope
			// over the note is rate-blind: it says piano (attack, long decay) or drum (one burst) or
			// loop (repeats) without caring how fast the bytes were read. The length that wins also
			// gives the rate, because the note's duration is known: bytes / seconds.
			static const int kSpans[] = { 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096, 6144, 8192 };

			for (uint32_t slot = 0; slot + 0x2000 <= ram.size (); slot += 0x2000)
			{
				for (int span : kSpans)
				{
					const std::vector<double> have = envelopeOfBytes (ram.data () + slot, span, 32);
					const double score = shapeScore (want, have);

					matches.push_back ({ score, slot, (double) span / seconds });
				}
			}

			std::sort (matches.begin (), matches.end (),
					   [] (const Match& a, const Match& b) { return a.r > b.r; });

			// A tone crosses zero about twice per cycle; noise crosses far more often. Printing both
			// sides says whether the bank holds anything tonal at all - and if it does not, no
			// address mapping can explain a piano.
			std::printf ("     tone   : render %.0f/s", crossingRate (audio, note.start, note.length));

			// **One fixed rate for every slot**, so the column is comparable with the render. The
			// first version multiplied each slot by whatever rate the shape search had liked for it,
			// which made the numbers look like they measured the slot when they measured the guess.
			constexpr double kCommonRate = 10442.7;

			for (uint32_t slot = 0; slot + 0x2000 <= ram.size (); slot += 0x2000)
			{
				const double perByte = crossingRateOfBytes (ram.data () + slot, 0x2000);

				std::printf ("  0x%04X %.0f/s", (unsigned) slot, perByte * kCommonRate);
			}

			std::printf ("\n");

			std::printf ("     shape  :");

			for (int i = 0; i < 3 && i < (int) matches.size (); ++i)
			{
				std::printf ("  0x%04X@%.0fB/s r=%.2f", (unsigned) matches[(size_t) i].slot,
							 matches[(size_t) i].rate, matches[(size_t) i].r);
			}

			std::printf ("\n");

			// **The slot's own pitch, in the same units as the render's.**
			//
			// Autocorrelation over the bytes, in the byte domain, then divided into the rate they
			// would be played at: a period of 146 bytes at 10442.7 B/s is 71.5 Hz, which is what the
			// channel renders. This is the comparison that had never been made - the render was
			// measured and the export was measured, but never by the same arithmetic, and the two
			// numbers that disagreed (14 bytes against 71.5 Hz) came from two different scripts.
			{
				constexpr uint32_t kSlot = 0x8000;
				constexpr double kSlotRate = 10442.7;

				if (kSlot + 4096 <= ram.size ())
				{
					const uint8_t* bytes = ram.data () + kSlot;
					const int count = 4096;

					std::vector<double> level ((size_t) count);

					for (int i = 0; i < count; ++i)
					{
						const double magnitude = (double) (bytes[i] & 0x7F) / 128.0;
						level[(size_t) i] = ((bytes[i] & 0x80) != 0) ? magnitude : -magnitude;
					}

					double best = -2.0;
					int bestLag = 0;

					for (int lag = 8; lag < count / 2; ++lag)
					{
						double num = 0.0;
						double d1 = 0.0;
						double d2 = 0.0;

						for (int i = 0; i + lag < count; ++i)
						{
							const double a = level[(size_t) i];
							const double b = level[(size_t) (i + lag)];
							num += a * b;
							d1 += a * a;
							d2 += b * b;
						}

						const double r = (d1 > 0.0 && d2 > 0.0) ? num / std::sqrt (d1 * d2) : 0.0;

						if (r > best)
						{
							best = r;
							bestLag = lag;
						}
					}

					std::printf ("     0x8000 : period %d bytes -> %.1f Hz at %.0f B/s (r=%.2f)\n",
								 bestLag, (bestLag > 0) ? kSlotRate / (double) bestLag : 0.0,
								 kSlotRate, best);

					// The same bytes as the export, so the two files can be compared by ear: one is
					// what the engine rendered, the other what the row would hand back.
					std::vector<float> exported (level.begin (), level.end ());
					writeWav (juce::File::getCurrentWorkingDirectory ().getChildFile ("build/slot_8000.wav"),
							  exported, kSlotRate);
				}
			}

			if (! matches.empty () && matches[0].r > 0.9)
			{
				std::printf ("     verdict: MATCH 0x%04X at %.0f bytes/s (%.3f s)\n",
							 (unsigned) matches[0].slot, matches[0].rate, seconds);
			}
			else
			{
				std::printf ("     verdict: no slot matches this note\n");
			}
		}

		if (channel == nullptr)
		{
			std::printf ("     export : no state\n");
			continue;
		}

		const ExportShape shape = shapeOf (*channel);

		if (! shape.present)
		{
			std::printf ("     export : no wave\n");
			continue;
		}

		std::printf ("     export : %zu samples at %.1f Hz = %.3f s, period %d -> %.1f Hz\n",
					 shape.length, shape.rate, (double) shape.length / shape.rate,
					 shape.period, shape.impliedPitch);

		if (note.pitch > 0.0 && shape.impliedPitch > 0.0)
		{
			std::printf ("     ratio  : export pitch / rendered pitch = %.3f\n",
						 shape.impliedPitch / note.pitch);
		}

		if (note.length > 0 && shape.rate > 0.0)
		{
			const double renderedSeconds = (double) note.length / kSampleRate;
			const double exportSeconds = (double) shape.length / shape.rate;
			std::printf ("     length : render %.3f s vs export %.3f s (ratio %.2f)\n",
						 renderedSeconds, exportSeconds, exportSeconds / renderedSeconds);
		}

	}

	// **Does muting do anything at all?** A track whose voice was never given a start is silent
	// whether or not the mixer isolates it, so "Ch7 is quiet" proves nothing. Muting every track and
	// rendering asks the question where there is no second reason for silence: if the bus is not
	// quiet, the per-track measurements above are measurements of the whole song and every
	// conclusion drawn from them has to be thrown away.
	{
		for (size_t other = 0; other < tracks.size (); ++other)
		{
			engine.setTrackMute (other, true);
		}

		engine.setPositionSeconds (0.0);
		engine.play ();

		const std::vector<float> allMuted = renderSeconds (engine, 2.0);

		engine.stop ();

		for (size_t other = 0; other < tracks.size (); ++other)
		{
			engine.setTrackMute (other, false);
		}

		double peak = 0.0;

		for (const auto sample : allMuted)
		{
			peak = juce::jmax (peak, (double) std::abs (sample));
		}

		std::printf ("\nall tracks muted: %d samples, peak %.4f %s\n", (int) allMuted.size (), peak,
					 (peak > 0.001) ? "<- MUTE IS NOT APPLIED" : "(silent, so muting works)");
	}

	// **Where the chip's bytes sit in the bank.** If the emulator loads the `0x02` bank with a different
	// layout than `memory::pcmBank`, the same file address holds different samples in the two - which
	// is exactly what the user hears: the row exports a drum while the channel plays a piano. This
	// searches the dumped memory for the bank instead of assuming an offset.
	{
		const juce::File dump = juce::File::getCurrentWorkingDirectory ().getChildFile ("build/hot_4.bin");

		if (dump.existsAsFile () && ! ram.empty ())
		{
			juce::MemoryBlock bytes;

			if (dump.loadFileAsData (bytes) && bytes.getSize () == 0x10000)
			{
				const auto* emu = static_cast<const uint8_t*> (bytes.getData ());
				constexpr int kNeedle = 24;
				constexpr uint32_t kFrom = 0x8073;

				std::vector<std::size_t> found;

				for (std::size_t i = 0; i + kNeedle < ram.size () && found.size () < 3; ++i)
				{
					bool same = true;

					for (int k = 0; k < kNeedle; ++k)
					{
						if (ram[i + (std::size_t) k] != emu[kFrom + (uint32_t) k])
						{
							same = false;
							break;
						}
					}

					if (same)
					{
						found.push_back (i);
					}
				}

				std::printf ("\nchip memory: %u bytes, its 0x%04X reads %02X %02X %02X %02X (ours: %02X %02X %02X %02X)\n",
							 (unsigned) bytes.getSize (), (unsigned) kFrom, emu[kFrom], emu[kFrom + 1],
							 emu[kFrom + 2], emu[kFrom + 3]);

				if (found.empty ())
				{
					std::printf ("   those bytes do not appear in the bank at any offset\n");
				}
				else
				{
					// The shift the emulator uses, next to what the export reads at the same address.
					const std::size_t shift = (std::size_t) (kFrom - found[0]);

					std::printf ("   found in the bank at 0x%zX (shift 0x%zX)\n", found[0], shift);
					std::printf ("   chip 0x%04X == bank 0x%04X == %02X %02X %02X %02X"
								 "   |  our image 0x8000 = %02X %02X %02X %02X\n",
								 (unsigned) kFrom, (unsigned) (kFrom - shift),
								 ram[kFrom - shift], ram[kFrom - shift + 1],
								 ram[kFrom - shift + 2], ram[kFrom - shift + 3],
								 ram[0x8000], ram[0x8001], ram[0x8002], ram[0x8003]);
				}
			}
		}
	}

	std::printf ("\n");
	return 0;
}
