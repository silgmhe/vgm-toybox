#include <cstring>
#include "nes_n106.h"

namespace xgm {

NES_N106::NES_N106 ()
{
    option[OPT_SERIAL] = 0;
    option[OPT_PHASE_READ_ONLY] = 0;
    option[OPT_LIMIT_WAVELENGTH] = 0;
    SetClock (DEFAULT_CLOCK);
    SetRate (DEFAULT_RATE);
    for (int i=0; i < 8; ++i)
    {
        sm[0][i] = 128;
        sm[1][i] = 128;
    }
    Reset();
}

NES_N106::~NES_N106 ()
{
}

void NES_N106::SetStereoMix (int trk, int16_t mixl, int16_t mixr)
{
    if (trk < 0 || trk >= 8) return;
    trk = 7-trk; // displayed channels are inverted
    sm[0][trk] = mixl;
    sm[1][trk] = mixr;
}

ITrackInfo *NES_N106::GetTrackInfo (int trk)
{
    int channels = get_channels();
    int channel = 7-trk; // invert the track display

    TrackInfoN106* t = &trkinfo[channel];

    if (trk >= channels)
    {
        t->max_volume = 15;
        t->volume = 0;
        t->_freq = 0;
        t->wavelen = 0;
        t->tone = -1;
        t->output = 0;
        t->key = false;
        t->freq = 0;
    }
    else
    {
        t->max_volume = 15;
        t->volume = get_vol(channel);
        t->_freq = get_freq(channel);
        t->wavelen = get_len(channel);
        t->tone = get_off(channel);
        t->output = fout[channel];

        t->key = (t->volume > 0) && (t->_freq > 0);
        t->freq = (double(t->_freq) * clock) / double(15 * 65536 * channels * t->wavelen);

        for (int i=0; i < t->wavelen; ++i)
            t->wave[i] = get_sample((i+t->tone)&0xFF);
    }

    return t;
}

void NES_N106::SetClock (double c)
{
    clock = c;
}

void NES_N106::SetRate (double r)
{
    rate = r;
}

void NES_N106::SetMask (int m)
{
    // bit reverse the mask,
    // N163 waves are displayed in reverse order
    mask = 0
        | ((m & (1<<0)) ? (1<<7) : 0)
        | ((m & (1<<1)) ? (1<<6) : 0)
        | ((m & (1<<2)) ? (1<<5) : 0)
        | ((m & (1<<3)) ? (1<<4) : 0)
        | ((m & (1<<4)) ? (1<<3) : 0)
        | ((m & (1<<5)) ? (1<<2) : 0)
        | ((m & (1<<6)) ? (1<<1) : 0)
        | ((m & (1<<7)) ? (1<<0) : 0);
}

void NES_N106::SetOption (int id, int val)
{
    if (id<OPT_END) option[id] = val;
}

void NES_N106::Reset ()
{
    master_disable = false;
    ::memset(reg, 0, sizeof(reg));
    reg_select = 0;
    reg_advance = false;
    tick_channel = 0;
    tick_clock = 0;
    render_channel = 0;
    render_clock = 0;
    render_subclock = 0;

    for (int i=0; i<8; ++i) fout[i] = 0;

    Write(0xE000, 0x00); // master disable off
    Write(0xF800, 0x80); // select $00 with auto-increment
    for (unsigned int i=0; i<0x80; ++i) // set all regs to 0
    {
        Write(0x4800, 0x00);
    }
    Write(0xF800, 0x00); // select $00 without auto-increment
}

void NES_N106::Tick (uint32_t clocks)
{
    if (master_disable) return;

    int channels = get_channels();

    tick_clock += clocks;
    render_clock += clocks; // keep render in sync
    while (tick_clock > 0)
    {
        int channel = 7-tick_channel;

        uint32_t phase = get_phase(channel);
        uint32_t freq  = get_freq(channel);
        uint32_t len   = get_len(channel);
        uint32_t off   = get_off(channel);
        int32_t  vol   = get_vol(channel);

        // accumulate 24-bit phase
        phase = (phase + freq) & 0x00FFFFFF;

        // wrap phase if wavelength exceeded
        uint32_t hilen = len << 16;
        while (phase >= hilen) phase -= hilen;

        // write back phase
        set_phase(phase, channel);

        // fetch sample (note: N163 output is centred at 8, and inverted w.r.t 2A03)
        int32_t sample = 8 - get_sample(((phase >> 16) + off) & 0xFF);
        fout[channel] = sample * vol;

        // cycle to next channel every 15 clocks
        tick_clock -= 15;
        ++tick_channel;
        if (tick_channel >= channels)
            tick_channel = 0;
    }
}

uint32_t NES_N106::Render (int32_t b[2])
{
    b[0] = 0;
    b[1] = 0;
    if (master_disable) return 2;

    int channels = get_channels();

    if (option[OPT_SERIAL]) // hardware accurate serial multiplexing
    {
        // this could be made more efficient than going clock-by-clock
        // but this way is simpler
        int clocks = render_clock;
        while (clocks > 0)
        {
            int c = 7-render_channel;
            if (0 == ((mask >> c) & 1))
            {
                b[0] += fout[c] * sm[0][c];
                b[1] += fout[c] * sm[1][c];
            }
            
            ++render_subclock;
            if (render_subclock >= 15) // each channel gets a 15-cycle slice
            {
                render_subclock = 0;
                ++render_channel;
                if (render_channel >= channels)
                    render_channel = 0;
            }
            --clocks;
        }

        // increase output level by 1 bits (7 bits already added from sm)
        b[0] <<= 1;
        b[1] <<= 1;

        // average the output
        if (render_clock > 0)
        {
            b[0] /= render_clock;
            b[1] /= render_clock;
        }
        render_clock = 0;
    }
    else // just mix all channels
    {
        for (int i = (8-channels); i<8; ++i)
        {
            if (0 == ((mask >> i) & 1))
            {
                b[0] += fout[i] * sm[0][i];
                b[1] += fout[i] * sm[1][i];
            }
        }

        // mix together, increase output level by 8 bits, roll off 7 bits from sm
        int32_t MIX[9] = { 256/1, 256/1, 256/2, 256/3, 256/4, 256/5, 256/6, 256/6, 256/6 };
        b[0] = (b[0] * MIX[channels]) >> 7;
        b[1] = (b[1] * MIX[channels]) >> 7;
        // when approximating the serial multiplex as a straight mix, once the
        // multiplex frequency gets below the nyquist frequency an average mix
        // begins to sound too quiet. To approximate this effect, I don't attenuate
        // any further after 6 channels are active.
    }

    // 8 bit approximation of master volume
    // max N163 vol vs max APU square
    // unfortunately, games have been measured as low as 3.4x and as high as 8.5x
    // with higher volumes on Erika, King of Kings, and Rolling Thunder
    // and lower volumes on others. Using 6.0x as a rough "one size fits all".
    const double MASTER_VOL = 6.0 * 1223.0;
    const double MAX_OUT = 15.0 * 15.0 * 256.0; // max digital value
    const int32_t GAIN = int((MASTER_VOL / MAX_OUT) * 256.0f);
    b[0] = (b[0] * GAIN) >> 8;
    b[1] = (b[1] * GAIN) >> 8;

    return 2;
}

bool NES_N106::Write (uint32_t adr, uint32_t val, uint32_t id)
{
    if (adr == 0xE000) // master disable
    {
        master_disable = ((val & 0x40) != 0);
        return true;
    }
    else if (adr == 0xF800) // register select
    {
        reg_select = (val & 0x7F);
        reg_advance = (val & 0x80) != 0;
        return true;
    }
    else if (adr == 0x4800) // register write
    {
        if (option[OPT_PHASE_READ_ONLY]) // old emulators didn't know phase was stored here
        {
            int c = 15 - (reg_select/8);
            int r = reg_select & 7;
            if (c < get_channels() &&
                (r == 1 ||
                 r == 3 ||
                 r == 5))
            {
                if (reg_advance)
                    reg_select = (reg_select + 1) & 0x7F;
                return true;
            }
        }
        if (option[OPT_LIMIT_WAVELENGTH]) // old emulators ignored top 3 bits of length
        {
            int c = 15 - (reg_select/8);
            int r = reg_select & 7;
            if (c < get_channels() && r == 4)
            {
                val |= 0xE0;
            }
        }
        reg[reg_select] = val;
        if (reg_advance)
            reg_select = (reg_select + 1) & 0x7F;
        return true;
    }
    return false;
}

bool NES_N106::Read (uint32_t adr, uint32_t & val, uint32_t id)
{
    if (adr == 0x4800) // register read
    {
        val = reg[reg_select];
        if (reg_advance)
            reg_select = (reg_select + 1) & 0x7F;
        return true;
    }
    return false;
}

//
// register decoding/encoding functions
// 

inline uint32_t NES_N106::get_phase (int channel)
{
    // 24-bit phase stored in channel regs 1/3/5
    channel = channel << 3;
    return (reg[0x41 + channel]      )
        +  (reg[0x43 + channel] << 8 )
        +  (reg[0x45 + channel] << 16);
}

inline uint32_t NES_N106::get_freq (int channel)
{
    // 19-bit frequency stored in channel regs 0/2/4
    channel = channel << 3;
    return ( reg[0x40 + channel]              )
        +  ( reg[0x42 + channel]         << 8 )
        +  ((reg[0x44 + channel] & 0x03) << 16);
}

inline uint32_t NES_N106::get_off (int channel)
{
    // 8-bit offset stored in channel reg 6
    channel = channel << 3;
    return reg[0x46 + channel];
}

inline uint32_t NES_N106::get_len (int channel)
{
    // 6-bit<<3 length stored obscurely in channel reg 4
    channel = channel << 3;
    return 256 - (reg[0x44 + channel] & 0xFC);
}

inline int32_t NES_N106::get_vol (int channel)
{
    // 4-bit volume stored in channel reg 7
    channel = channel << 3;
    return reg[0x47 + channel] & 0x0F;
}

inline int32_t NES_N106::get_sample (uint32_t index)
{
    // every sample becomes 2 samples in regs
    return (index&1) ?
        ((reg[index>>1] >> 4) & 0x0F) :
        ( reg[index>>1]       & 0x0F) ;
}

inline int NES_N106::get_channels ()
{
    // 3-bit channel count stored in reg 0x7F
    return ((reg[0x7F] >> 4) & 0x07) + 1;
}

inline void NES_N106::set_phase (uint32_t phase, int channel)
{
    // 24-bit phase stored in channel regs 1/3/5
    channel = channel << 3;
    reg[0x41 + channel] =  phase        & 0xFF;
    reg[0x43 + channel] = (phase >> 8 ) & 0xFF;
    reg[0x45 + channel] = (phase >> 16) & 0xFF;
}

  uint32_t NES_N106::RenderChannels (int32_t* dest, int stride, int maxChannels)
  {
    // One Render() per channel with the others muted, then the mask put back. Correct, and it
    // keeps this file out of the chip's internal mixing - but it costs one render per channel, so
    // it is the shape to improve once the channels are exposed directly.
    const int saved = mask;

    for (int i = 0; i < 8; ++i)
    {
      mask = ~(1 << i);

      int32_t b[2] = { 0, 0 };
      Render (b);

      if (i < maxChannels)
      {
        dest[i * stride + 0] = b[0];
        dest[i * stride + 1] = b[1];
      }
    }

    mask = saved;
    return 8;
  }

} //namespace
