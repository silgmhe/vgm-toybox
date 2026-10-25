#ifndef __EMU_PANNING_H__
#define __EMU_PANNING_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include "../stdtype.h"

#define PANNING_BITS	16	// 16.16 fixed point
#define PANNING_NORMAL	(1 << PANNING_BITS)

// apply panning value to x, x should be within +-16384
#define APPLY_PANNING_S(x, panval)	((x * panval) >> PANNING_BITS)
// apply panning value to x, version for larger values
#define APPLY_PANNING_L(x, panval)	(int32_t)(((int64_t)x * panval) >> PANNING_BITS)

void Panning_Calculate(int32_t channels[2], int16_t position);
void Panning_Centre(int32_t channels[2]);

#ifdef __cplusplus
}
#endif

#endif	// __EMU_PANNING_H__
