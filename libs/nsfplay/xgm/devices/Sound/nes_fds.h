#ifndef _NES_FDS_H_
#define _NES_FDS_H_
#include "../device.h"

namespace xgm {

class TrackInfoFDS : public TrackInfoBasic
{
public:
    int16_t wave[64];
    virtual IDeviceInfo *Clone(){ return new TrackInfoFDS(*this); }
};

class NES_FDS : public ISoundChip
{
public:
    enum
    {
        OPT_CUTOFF=0,
        OPT_4085_RESET,
        OPT_WRITE_PROTECT,
        OPT_END
    };

protected:
    double rate, clock;
    int mask;
    int32_t sm[2]; // stereo mix
    int32_t fout; // current output
    TrackInfoFDS trkinfo;
    int option[OPT_END];

    bool master_io;
    uint32_t master_vol;
    uint32_t last_freq; // for trackinfo
    uint32_t last_vol;  // for trackinfo

    // two wavetables
    enum { TMOD=0, TWAV=1 };
    int32_t wave[2][64];
    uint32_t freq[2];
    uint32_t phase[2];
    bool wav_write;
    bool wav_halt;
    bool env_halt;
    bool mod_halt;
    uint32_t mod_pos;
    uint32_t mod_write_pos;

    // two ramp envelopes
    enum { EMOD=0, EVOL=1 };
    bool env_mode[2];
    bool env_disable[2];
    uint32_t env_timer[2];
    uint32_t env_speed[2];
    uint32_t env_out[2];
    uint32_t master_env_speed;

    // 1-pole RC lowpass filter
    int32_t rc_accum;
    int32_t rc_k;
    int32_t rc_l;

public:
    NES_FDS ();
    virtual ~ NES_FDS ();

    virtual void Reset ();
    virtual void Tick (uint32_t clocks);
    virtual uint32_t Render (int32_t b[2]);
    virtual bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    virtual bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    virtual void SetRate (double);
    virtual void SetClock (double);
    virtual void SetOption (int, int);
    virtual void SetMask(int m){ mask = m&1; }
    virtual void SetStereoMix (int trk, int16_t mixl, int16_t mixr);
    virtual ITrackInfo *GetTrackInfo(int trk);
};

} // namespace xgm

#endif
