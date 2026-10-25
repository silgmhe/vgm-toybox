#ifndef __NP_NES_FDS_H__
#define __NP_NES_FDS_H__

void* NES_FDS_Create(uint32_t clock, uint32_t rate);
void NES_FDS_Destroy(void* chip);
void NES_FDS_Reset(void* chip);
uint32_t NES_FDS_Render(void* chip, int32_t b[2]);
bool NES_FDS_Write(void* chip, uint16_t adr, uint8_t val);
bool NES_FDS_Read(void* chip, uint16_t adr, uint8_t* val);
void NES_FDS_SetRate(void* chip, uint32_t r);
void NES_FDS_SetClock(void* chip, uint32_t c);
void NES_FDS_SetOption(void* chip, int id, int val);
int NES_FDS_GetOption(void* chip, int id);
void NES_FDS_SetMask(void* chip, int m);
void NES_FDS_SetStereoMix(void* chip, int trk, int16_t mixl, int16_t mixr);

#endif	// __NP_NES_FDS_H__
