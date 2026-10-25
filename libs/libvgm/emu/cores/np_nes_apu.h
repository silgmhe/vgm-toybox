#ifndef __NP_NES_APU_H__
#define __NP_NES_APU_H__

void NES_APU_np_FrameSequence(void* chip, int s);
void* NES_APU_np_Create(uint32_t clock, uint32_t rate);
void NES_APU_np_Destroy(void* chip);
void NES_APU_np_Reset(void* chip);
bool NES_APU_np_Read(void* chip, uint16_t adr, uint8_t* val);
bool NES_APU_np_Write(void* chip, uint16_t adr, uint8_t val);
uint32_t NES_APU_np_Render(void* chip, int32_t b[2]);
void NES_APU_np_SetRate(void* chip, uint32_t rate);
void NES_APU_np_SetClock(void* chip, uint32_t clock);
void NES_APU_np_SetOption(void* chip, int id, int b);
void NES_APU_np_SetMask(void* chip, int m);
void NES_APU_np_SetStereoMix(void* chip, int trk, int16_t mixl, int16_t mixr);

#endif	// __NP_NES_APU_H__
