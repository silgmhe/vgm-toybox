#ifndef __NP_NES_DMC_H__
#define __NP_NES_DMC_H__

void* NES_DMC_np_Create(uint32_t clock, uint32_t rate);
void NES_DMC_np_Destroy(void *chip);
void NES_DMC_np_Reset(void *chip);
void NES_DMC_np_SetRate(void* chip, uint32_t rate);
void NES_DMC_np_SetPal(void* chip, bool is_pal);
void NES_DMC_np_SetAPU(void* chip, void* apu_);
uint32_t NES_DMC_np_Render(void* chip, int32_t b[2]);
void NES_DMC_np_SetMemory(void* chip, const uint8_t* r);
bool NES_DMC_np_Write(void* chip, uint16_t adr, uint8_t val);
bool NES_DMC_np_Read(void* chip, uint16_t adr, uint8_t* val);
void NES_DMC_np_SetClock(void* chip, uint32_t rate);
void NES_DMC_np_SetOption(void* chip, int id, int val);
int NES_DMC_np_GetDamp(void* chip);
void NES_DMC_np_SetMask(void* chip, int m);
void NES_DMC_np_SetStereoMix(void* chip, int trk, int16_t mixl, int16_t mixr);

#endif	// __NP_NES_DMC_H__
