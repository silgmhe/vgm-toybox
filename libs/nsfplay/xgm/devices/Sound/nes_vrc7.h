#ifndef _NES_VRC7_H_
#define _NES_VRC7_H_
#include "../device.h"
#include "legacy/emu2413.h"

namespace xgm
{

  class NES_VRC7 : public ISoundChip
  {
  public:
    enum
    {
      OPT_OPLL=0,
      OPT_END
    };
  protected:
    int option[OPT_END];
    int mask;
    int patch_set;
    const uint8_t* patch_custom;
    //int32_t sm[2][6]; // stereo mix
    int32_t sm[2][9]; // stereo mix temporary HACK to support YM2413
    int16_t buf[2];
    OPLL *opll;
    uint32_t divider; // clock divider
    double clock, rate;
    //TrackInfoBasic trkinfo[6];
    TrackInfoBasic trkinfo[9]; // HACK to support YM2413
    bool use_all_channels;
  public:
      NES_VRC7 ();
     ~NES_VRC7 ();

    virtual void Reset ();
    virtual void Tick (uint32_t clocks);
    virtual uint32_t Render (int32_t b[2]);

  // Added for VGMToybox: the 6 channels Render() sums, kept apart.
  uint32_t RenderChannels (int32_t* dest, int stride, int maxChannels);
    virtual bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    virtual bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    virtual void UseAllChannels (bool b);
    virtual void SetPatchSet (int p);
    virtual void SetPatchSetCustom (const uint8_t* pset);
    virtual void SetClock (double);
    virtual void SetRate (double);
    virtual void SetOption (int, int);
    virtual void SetMask (int m){ mask = m; if(opll) OPLL_setMask(opll, m); }
    virtual void SetStereoMix (int trk, int16_t mixl, int16_t mixr);
    virtual ITrackInfo *GetTrackInfo(int trk);
  };

}                               // namespace

#endif
