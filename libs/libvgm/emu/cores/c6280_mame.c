// license:BSD-3-Clause
// copyright-holders:Charles MacDonald
/*
    HuC6280 sound chip emulator
    by Charles MacDonald
    E-mail: cgfm2@hotmail.com
    WWW: http://cgfm2.emuviews.com

    Thanks to:

    - Paul Clifford for his PSG documentation.
    - Richard Bannister for the TGEmu-specific sound updating code.
    - http://www.uspto.gov for the PSG patents.
    - All contributors to the tghack-list.

    Changes:

    (03/30/2003)
    - Removed TGEmu specific code and added support functions for MAME.
    - Modified setup code to handle multiple chips with different clock and
      volume settings.

    Missing features / things to do:

    - Verify LFO frequency from real hardware.

    - Add shared index for waveform playback and sample writes. Almost every
      game will reset the index prior to playback so this isn't an issue.

    - While the noise emulation is complete, the data for the pseudo-random
      bitstream is calculated by machine().rand() and is not a representation of what
      the actual hardware does.

    For some background on Hudson Soft's C62 chipset:

    - http://www.hudsonsoft.net/ww/about/about.html
    - http://www.hudson.co.jp/corp/eng/coinfo/history.html

*/

#include <stdlib.h>	// for rand()
#include <string.h>	// for memset()
#include <math.h>	// for pow()

#include "../../stdtype.h"
#include "../snddef.h"
#include "../EmuStructs.h"
#include "../EmuCores.h"
#include "../EmuHelper.h"
#include "c6280_mame.h"


static void c6280mame_w(void *chip, uint8_t offset, uint8_t data);
static uint8_t c6280mame_r(void* chip, uint8_t offset);

static void c6280mame_update(void* param, uint32_t samples, DEV_SMPL **outputs);
static uint8_t device_start_c6280_mame(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);
static void* device_start_c6280mame(uint32_t clock, uint32_t rate);
static void device_stop_c6280mame(void* chip);
static void device_reset_c6280mame(void* chip);

static void c6280mame_set_mute_mask(void* chip, uint32_t MuteMask);


static DEVDEF_RWFUNC devFunc[] =
{
	{RWF_REGISTER | RWF_WRITE, DEVRW_A8D8, 0, c6280mame_w},
	{RWF_REGISTER | RWF_READ, DEVRW_A8D8, 0, c6280mame_r},
	{RWF_CHN_MUTE | RWF_WRITE, DEVRW_ALL, 0, c6280mame_set_mute_mask},
	{0x00, 0x00, 0, NULL}
};
DEV_DEF devDef_C6280_MAME =
{
	"HuC6280", "MAME", FCC_MAME,
	
	device_start_c6280_mame,
	device_stop_c6280mame,
	device_reset_c6280mame,
	c6280mame_update,
	
	NULL,	// SetOptionBits
	c6280mame_set_mute_mask,
	NULL,	// SetPanning
	NULL,	// SetSampleRateChangeCallback
	NULL,	// SetLoggingCallback
	NULL,	// LinkDevice
	
	devFunc,	// rwFuncs
};


typedef struct {
	uint16_t frequency;
	uint8_t control;
	uint8_t balance;
	uint8_t waveform[32];
	uint8_t index;
	int16_t dda;
	uint8_t noise_control;
	uint32_t noise_counter;
	uint32_t noise_seed;
	uint32_t counter;
	uint8_t Muted;
} t_channel;

typedef struct {
	DEV_DATA _devData;
	
	uint8_t select;
	uint8_t balance;
	uint8_t lfo_frequency;
	uint8_t lfo_control;
	t_channel channel[8];	// is 8, because: p->select = data & 0x07;
	int16_t volume_table[32];
	uint32_t noise_freq_tab[32];
	uint32_t wave_freq_tab[4096];
} c6280_t;


//-------------------------------------------------
//  calculate_clocks - (re)calculate clock-derived
//  members
//-------------------------------------------------

static void c6280_calculate_clocks(c6280_t *p, double clk, double rate)
{
	int i;
	double step;
	//double rate = clock / 16;

	/* Make waveform frequency table */
	for (i = 0; i < 4096; i += 1)
	{
		step = ((clk / rate) * 4096) / (i+1);
		p->wave_freq_tab[(1 + i) & 0xFFF] = (uint32_t)step;
	}

	/* Make noise frequency table */
	for (i = 0; i < 32; i += 1)
	{
		step = ((clk / rate) * 32) / (i+1);
		p->noise_freq_tab[i] = (uint32_t)step;
	}
}


/* Write Register Layout

       76543210
    00 -----xxx Channel Select
       -----000 Channel 0
       -----001 Channel 1
       -----010 Channel 2
       -----011 Channel 3
       -----100 Channel 4
       -----101 Channel 5

    01 xxxx---- Overall Left Volume
       ----xxxx Overall Right Volume

    02 xxxxxxxx Frequency (Low 8 bits)

    03 ----xxxx Frequency (High 4 bits)

    04 x------- Channel Enable
       -x------ Direct D/A
       00------ Write Data
       01------ Reset Counter
       10------ Mixing (Sound Output)
       11------ Direct D/A
       ---xxxxx Channel Master Volume

    05 xxxx---- Channel Left Volume
       ----xxxx Channel Right Volume

    06 ---xxxxx Waveform Data

    07 x------- Noise Enable (channel 5, 6 only)
       ---xxxxx Noise Frequency ^ 0x1f

    08 xxxxxxxx LFO Frequency

    09 x------- LFO Reset
       ------xx LFO Control
       ------00 LFO off
       ------01 Direct Addition
       ------10 2 bit Shift Addition
       ------11 4 bit Shift Addition
*/

static void c6280mame_w(void *chip, uint8_t offset, uint8_t data)
{
	c6280_t *p = (c6280_t *)chip;
	t_channel *chan = &p->channel[p->select];

	//m_cpudevice->io_set_buffer(data);

	switch(offset & 0x0F)
	{
		case 0x00: /* Channel select */
			p->select = data & 0x07;
			break;

		case 0x01: /* Global balance */
			p->balance  = data;
			break;

		case 0x02: /* Channel frequency (LSB) */
			chan->frequency = (chan->frequency & 0x0F00) | data;
			chan->frequency &= 0x0FFF;
			break;

		case 0x03: /* Channel frequency (MSB) */
			chan->frequency = (chan->frequency & 0x00FF) | (data << 8);
			chan->frequency &= 0x0FFF;
			break;

		case 0x04: /* Channel control (key-on, DDA mode, volume) */

			/* 1-to-0 transition of DDA bit resets waveform index */
			if ((chan->control & 0x40) && ((data & 0x40) == 0))
			{
				chan->index = 0;
			}
			if (((chan->control & 0x80) == 0) && (data & 0x80))
			{
				chan->counter = 0;
			}
			chan->control = data;
			break;

		case 0x05: /* Channel balance */
			chan->balance = data;
			break;

		case 0x06: /* Channel waveform data */

			switch(chan->control & 0x40)
			{
				case 0x00: /* Waveform */
					chan->waveform[chan->index & 0x1F] = data & 0x1F;
					if (!(chan->control & 0x80)) // TODO : wave pointer is increased at writing data when sound playback is off??
						chan->index = (chan->index + 1) & 0x1F;
					break;

				case 0x40: /* Direct D/A */
					chan->dda = data & 0x1F;
					break;
			}

			break;

		case 0x07: /* Noise control (enable, frequency) */
			chan->noise_control = data;
			break;

		case 0x08: /* LFO frequency */
			p->lfo_frequency = data;
			break;

		case 0x09: /* LFO control (enable, mode) */
			p->lfo_control = data;
			break;

		default:
			break;
	}
}


static void c6280mame_update(void* param, uint32_t samples, DEV_SMPL **outputs)
{
	static const uint8_t scale_tab[16] =
	{
		0x00, 0x03, 0x05, 0x07, 0x09, 0x0b, 0x0d, 0x0f,
		0x10, 0x13, 0x15, 0x17, 0x19, 0x1b, 0x1d, 0x1f
	};

	int ch;
	uint32_t i;
	c6280_t *p = (c6280_t *)param;

	uint8_t lmal = scale_tab[(p->balance >> 4) & 0x0F];
	uint8_t rmal = scale_tab[(p->balance >> 0) & 0x0F];

	/* Clear buffer */
	memset(outputs[0], 0x00, samples * sizeof(DEV_SMPL));
	memset(outputs[1], 0x00, samples * sizeof(DEV_SMPL));

	for (ch = 0; ch < 6; ch++)
	{
		/* Only look at enabled channels */
		if((p->channel[ch].control & 0x80) && ! p->channel[ch].Muted)
		{
			t_channel* chan = &p->channel[ch];
			uint8_t lal = scale_tab[(chan->balance >> 4) & 0x0F];
			uint8_t ral = scale_tab[(chan->balance >> 0) & 0x0F];
			uint8_t al  = chan->control & 0x1F;
			int16_t vll, vlr;

			// verified from both patent and manual
			vll = (0x1f - lmal) + (0x1f - al) + (0x1f - lal);
			if (vll > 0x1f) vll = 0x1f;

			vlr = (0x1f - rmal) + (0x1f - al) + (0x1f - ral);
			if (vlr > 0x1f) vlr = 0x1f;

			vll = p->volume_table[vll];
			vlr = p->volume_table[vlr];

			/* Check channel mode */
			if ((ch >= 4) && (chan->noise_control & 0x80))
			{
				/* Noise mode */
				uint32_t step = p->noise_freq_tab[(chan->noise_control & 0x1F) ^ 0x1F];
				for(i = 0; i < samples; i++)
				{
					int16_t data = (chan->noise_seed & 1) ? 0x1F : 0;
					chan->noise_counter += step;
					if(chan->noise_counter >= 0x800)
					{
						uint32_t seed = chan->noise_seed;
						// based on Charles MacDonald's research
						uint32_t feedback = ((seed) ^ (seed >> 1) ^ (seed >> 11) ^ (seed >> 12) ^ (seed >> 17)) & 1;
						chan->noise_seed = (chan->noise_seed >> 1) ^ (feedback << 17);
					}
					chan->noise_counter &= 0x7FF;
					outputs[0][i] += (vll * (data - 16));
					outputs[1][i] += (vlr * (data - 16));
				}
			}
			else if (chan->control & 0x40)
			{
				/* DDA mode */
				for(i = 0; i < samples; i++)
				{
					outputs[0][i] += (vll * (chan->dda - 16));
					outputs[1][i] += (vlr * (chan->dda - 16));
				}
			}
			else
			{
				if ((p->lfo_control & 3) && (ch < 2))
				{
					if (ch == 0) // CH 0 only, CH 1 is muted
					{
						/* Waveform mode with LFO */
						t_channel* lfo_srcchan = &p->channel[1];
						t_channel* lfo_dstchan = &p->channel[0];
						uint16_t lfo_step = lfo_srcchan->frequency;
						for (i = 0; i < samples; i++)
						{
							uint32_t step = lfo_dstchan->frequency;
							uint8_t offset;
							int16_t data;
							if (p->lfo_control & 0x80) // reset LFO
							{
								lfo_srcchan->counter = 0;
							}
							else
							{
								int lfooffset = (lfo_srcchan->counter >> 12) & 0x1F;
								int16_t lfo_data = lfo_srcchan->waveform[lfooffset];
								lfo_srcchan->counter += p->wave_freq_tab[(lfo_step * p->lfo_frequency) & 0xfff]; // verified from manual
								lfo_srcchan->counter &= 0x1FFFF;
								lfo_srcchan->index = (lfo_srcchan->counter >> 12) & 0x1F;
								step += ((lfo_data - 16) << (((p->lfo_control & 3) - 1) << 1)); // verified from manual
							}
							offset = (lfo_dstchan->counter >> 12) & 0x1F;
							data = lfo_dstchan->waveform[offset];
							lfo_dstchan->counter += p->wave_freq_tab[step & 0xfff];
							lfo_dstchan->counter &= 0x1FFFF;
							lfo_dstchan->index = (lfo_dstchan->counter >> 12) & 0x1F;
							outputs[0][i] += (vll * (data - 16));
							outputs[1][i] += (vlr * (data - 16));
						}
					}
				}
				else
				{
					/* Waveform mode */
					uint32_t step = p->wave_freq_tab[chan->frequency];
					for (i = 0; i < samples; i++)
					{
						uint8_t offset = (chan->counter >> 12) & 0x1F;
						int16_t data = chan->waveform[offset];
						chan->counter += step;
						chan->counter &= 0x1FFFF;
						chan->index = (chan->counter >> 12) & 0x1F;
						outputs[0][i] += (vll * (data - 16));
						outputs[1][i] += (vlr * (data - 16));
					}
				}
			}
		}
	}
}


/*--------------------------------------------------------------------------*/
/* MAME specific code                                                       */
/*--------------------------------------------------------------------------*/

static uint8_t device_start_c6280_mame(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf)
{
	void* chip;
	DEV_DATA* devData;
	uint32_t rate;
	
	rate = cfg->clock / 16;
	SRATE_CUSTOM_HIGHEST(cfg->srMode, rate, cfg->smplRate);
	
	chip = device_start_c6280mame(cfg->clock, rate);
	if (chip == NULL)
		return 0xFF;
	
	devData = (DEV_DATA*)chip;
	devData->chipInf = chip;
	INIT_DEVINF(retDevInf, devData, rate, &devDef_C6280_MAME);
	return 0x00;
}

static void* device_start_c6280mame(uint32_t clock, uint32_t rate)
{
	c6280_t *info;
	int i;
	double step;
	/* Loudest volume level for table */
	double level = 65536.0 / 6.0 / 32.0;

	info = (c6280_t*)calloc(1, sizeof(c6280_t));
	if (info == NULL)
		return NULL;

	c6280_calculate_clocks(info, clock, rate);

	/* Make volume table */
	/* PSG has 48dB volume range spread over 32 steps */
	step = 48.0 / 32.0;
	for (i = 0; i < 31; i++)
	{
		info->volume_table[i] = (uint16_t)level;
		level /= pow(10.0, step / 20.0);
	}
	info->volume_table[31] = 0;

	c6280mame_set_mute_mask(info, 0x00);

	return info;
}

static void device_stop_c6280mame(void* chip)
{
	c6280_t *info = (c6280_t *)chip;
	
	free(info);
	
	return;
}

static void device_reset_c6280mame(void* chip)
{
	c6280_t *info = (c6280_t *)chip;
	uint8_t CurChn;
	t_channel* TempChn;
	
	info->select = 0x00;
	info->balance = 0x00;
	info->lfo_frequency = 0x00;
	info->lfo_control = 0x00;
	
	for (CurChn = 0; CurChn < 6; CurChn ++)
	{
		TempChn = &info->channel[CurChn];
		
		TempChn->frequency = 0x00;
		TempChn->control = 0x00;
		TempChn->balance = 0x00;
		memset(TempChn->waveform, 0x00, 0x20);
		TempChn->index = 0x00;
		TempChn->dda = 0x00;
		TempChn->noise_control = 0x00;
		TempChn->noise_counter = 0x00;
		TempChn->noise_seed = 1;
		TempChn->counter = 0x00;
	}
	
	return;
}

static uint8_t c6280mame_r(void* chip, uint8_t offset)
{
	c6280_t *info = (c6280_t *)chip;
	//return m_cpudevice->io_get_buffer();
	if (offset == 0)
		return info->select;
	return 0;
}


static void c6280mame_set_mute_mask(void* chip, uint32_t MuteMask)
{
	c6280_t *info = (c6280_t *)chip;
	uint8_t CurChn;
	
	for (CurChn = 0; CurChn < 6; CurChn ++)
		info->channel[CurChn].Muted = (MuteMask >> CurChn) & 0x01;
	
	return;
}
