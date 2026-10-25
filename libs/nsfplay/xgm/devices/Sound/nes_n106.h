#ifndef _NES_N106_H_
#define _NES_N106_H_
#include "../device.h"

namespace xgm {

class TrackInfoN106 : public TrackInfoBasic
{
public:
    int wavelen;
    int16_t wave[256];
    virtual IDeviceInfo *Clone(){ return new TrackInfoN106(*this); }
};

class NES_N106:public ISoundChip
{
public:
    enum
    {
        OPT_SERIAL = 0,
        OPT_PHASE_READ_ONLY = 1,
        OPT_LIMIT_WAVELENGTH = 2,
        OPT_END
    };

protected:
    double rate, clock;
    int mask;
    int32_t sm[2][8]; // stereo mix
    int32_t fout[8]; // current output
    TrackInfoN106 trkinfo[8];
    int option[OPT_END];

    bool master_disable;
    uint32_t reg[0x80]; // all state is contained here
    unsigned int reg_select;
    bool reg_advance;
    int tick_channel;
    int tick_clock;
    int render_channel;
    int render_clock;
    int render_subclock;

    // convenience functions to interact with regs
    inline uint32_t get_phase (int channel);
    inline uint32_t get_freq (int channel);
    inline uint32_t get_off (int channel);
    inline uint32_t get_len (int channel);
    inline int32_t  get_vol (int channel);
    inline int32_t  get_sample (uint32_t index);
    inline int    get_channels ();
    // for storing back the phase after modifying
    inline void   set_phase (uint32_t phase, int channel);

public:
    NES_N106 ();
    ~NES_N106 ();

    virtual void Reset ();
    virtual void Tick (uint32_t clocks);
    virtual uint32_t Render (int32_t b[2]);

  // Added for VGMToybox: the 8 channels Render() sums, kept apart.
  uint32_t RenderChannels (int32_t* dest, int stride, int maxChannels);
    virtual bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    virtual bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    virtual void SetRate (double);
    virtual void SetClock (double);
    virtual void SetOption (int, int);
    virtual void SetMask (int m);
    virtual void SetStereoMix (int trk, int16_t mixl, int16_t mixr);
    virtual ITrackInfo *GetTrackInfo(int trk);
};

} // namespace xgm

#endif
