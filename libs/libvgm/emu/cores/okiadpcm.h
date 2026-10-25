// license:BSD-3-Clause
// copyright-holders:Andrew Gardner,Aaron Giles
/***************************************************************************

    okiadpcm.h

    OKI ADCPM emulation.

***************************************************************************/

#ifndef __OKIADPCM_H__
#define __OKIADPCM_H__


// Internal ADPCM state, used by external ADPCM generators with compatible specs to the OKIM 6295.
typedef struct _oki_adpcm_state
{
	int16_t   signal;
	int16_t   step;

	const int8_t* index_shift;
	const int16_t* diff_lookup;
} oki_adpcm_state;

oki_adpcm_state* oki_adpcm_create(const int8_t* custom_index_shift, const int16_t* custom_diff_lookup);
void oki_adpcm_destroy(oki_adpcm_state* adpcm);
void oki_adpcm_init(oki_adpcm_state* adpcm, const int8_t* custom_index_shift, const int16_t* custom_diff_lookup);
void oki_adpcm_reset(oki_adpcm_state* adpcm);
int16_t oki_adpcm_clock(oki_adpcm_state* adpcm, uint8_t nibble);


#endif	// __OKIADPCM_H__
