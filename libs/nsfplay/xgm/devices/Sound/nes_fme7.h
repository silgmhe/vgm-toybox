#ifndef _NES_FME7_H_
#define _NES_FME7_H_
#include "../device.h"
#include "legacy/emu2149.h"

namespace xgm
{

  class NES_FME7:public ISoundChip
  {
  public:
    enum
    {
      OPT_END
    };
  protected:
    //int option[OPT_END];
    int32_t sm[2][3]; // stereo mix
    int16_t buf[2];
    int mask; // kept here as well: RenderChannels reads the channels directly
    PSG *psg;
    int divider; // clock divider
    double clock, rate;
    TrackInfoBasic trkinfo[5];
  public:
      NES_FME7 ();
     ~NES_FME7 ();
    virtual void Reset ();
    virtual void Tick (uint32_t clocks);
    virtual uint32_t Render (int32_t b[2]);
    uint32_t RenderChannels (int32_t* dest, int stride, int maxChannels);
    virtual bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    virtual bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    virtual void SetClock (double);
    virtual void SetRate (double);
    virtual void SetOption (int, int);
    virtual void SetMask (int m){ mask = m; if(psg) PSG_setMask(psg,m); }
    virtual void SetStereoMix (int trk, int16_t mixl, int16_t mixr);
    virtual ITrackInfo *GetTrackInfo(int trk);
  };

}                               // namespace

#endif
