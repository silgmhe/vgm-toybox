#ifndef _NES_MMC5_H_
#define _NES_MMC5_H_
#include "../device.h"
#include "../CPU/nes_cpu.h"

namespace xgm
{
  class NES_MMC5:public ISoundChip
  {
  public:
    enum
    { OPT_NONLINEAR_MIXER=0, OPT_PHASE_REFRESH, OPT_END };

  protected:
    int option[OPT_END];
    int mask;
    int32_t sm[2][3]; // stereo panning
    uint8_t ram[0x6000 - 0x5c00];
    uint8_t reg[8];
    uint8_t mreg[2];
    uint8_t pcm; // PCM channel
    bool pcm_mode; // PCM channel
    NES_CPU* cpu; // PCM channel reads need CPU access

    uint32_t scounter[2];            // frequency divider
    uint32_t sphase[2];              // phase counter

    uint32_t duty[2];
    uint32_t volume[2];
    uint32_t freq[2];
    int32_t out[3];
    bool enable[2];

    bool envelope_disable[2];   // エンベロープ有効フラグ
    bool envelope_loop[2];      // エンベロープループ
    bool envelope_write[2];
    int envelope_div_period[2];
    int envelope_div[2];
    int envelope_counter[2];

    int length_counter[2];

    int frame_sequence_count;

    double clock, rate;
    int32_t calc_sqr (int i, uint32_t clocks);
    int32_t square_table[32];
    int32_t pcm_table[256];
    TrackInfoBasic trkinfo[3];
  public:
      NES_MMC5 ();
     ~NES_MMC5 ();

    void FrameSequence ();
    void TickFrameSequence (uint32_t clocks);

    virtual void Reset ();
    virtual void Tick (uint32_t clocks);
    virtual uint32_t Render (int32_t b[2]);
    uint32_t RenderChannels (int32_t* dest, int stride, int maxChannels);
    virtual bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    virtual bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    virtual void SetOption (int id, int b);
    virtual void SetClock (double);
    virtual void SetRate (double);
    virtual void SetMask (int m){ mask = m; }
    virtual void SetStereoMix (int trk, int16_t mixl, int16_t mixr);
    virtual ITrackInfo *GetTrackInfo(int trk);

    void SetCPU(NES_CPU* cpu_);
  };

}

#endif
