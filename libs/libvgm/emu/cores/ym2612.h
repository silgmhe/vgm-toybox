#ifndef __YM2612_H__
#define __YM2612_H__

#include "../../stdtype.h"

typedef struct ym2612__ ym2612_;

ym2612_ *YM2612_Init(uint32_t clock, uint32_t rate, uint8_t interpolation);
void YM2612_End(ym2612_ *YM2612);
void YM2612_Reset(ym2612_ *YM2612);
uint8_t YM2612_Read(ym2612_ *YM2612, uint8_t adr);
void YM2612_Write(ym2612_ *YM2612, uint8_t adr, uint8_t data);
void YM2612_ClearBuffer(DEV_SMPL **buffer, uint32_t length);
void YM2612_Update(ym2612_ *YM2612, DEV_SMPL **buf, uint32_t length);

uint32_t YM2612_GetMute(ym2612_ *YM2612);
void YM2612_SetMute(ym2612_ *YM2612, uint32_t val);
void YM2612_SetOptions(ym2612_ *YM2612, uint32_t Flags);

void YM2612_DacAndTimers_Update(ym2612_ *YM2612, DEV_SMPL **buffer, uint32_t length);
void YM2612_Special_Update(ym2612_ *YM2612);

#endif	// __YM2612_H__
