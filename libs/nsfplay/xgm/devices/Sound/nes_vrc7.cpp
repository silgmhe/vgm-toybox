#include <cstring>
#include "nes_vrc7.h"

namespace xgm
{
  NES_VRC7::NES_VRC7 ()
  {
    use_all_channels = false;
    patch_set = OPLL_VRC7_TONE;
    patch_custom = NULL;
    divider = 0;

    opll = OPLL_new ( 3579545, DEFAULT_RATE);
    OPLL_reset_patch (opll, patch_set);
    SetClock(DEFAULT_CLOCK);

    for(int c=0;c<2;++c)
        //for(int t=0;t<6;++t)
        for(int t=0;t<9;++t) // HACK for YM2413 support
            sm[c][t] = 128;
  }

  NES_VRC7::~NES_VRC7 ()
  {
    OPLL_delete (opll);
  }

  void NES_VRC7::UseAllChannels(bool b)
  {
    use_all_channels = b;
  }

  void NES_VRC7::SetPatchSet(int p)
  {
    patch_set = p;
  }

  void NES_VRC7::SetPatchSetCustom (const uint8_t* pset)
  {
    patch_custom = pset;
  }

  void NES_VRC7::SetClock (double c)
  {
    clock = c / 36;
  }

  void NES_VRC7::SetRate (double r)
  {
    //rate = r ? r : DEFAULT_RATE;
    (void)r; // rate is ignored
    rate = 49716;
    OPLL_set_quality(opll, 1); // quality always on (not really a CPU hog)
    OPLL_set_rate(opll,(uint32_t)rate);
  }

  void NES_VRC7::SetOption (int id, int val)
  {
    if(id<OPT_END)
    {
      option[id] = val;
    }
  }

  void NES_VRC7::Reset ()
  {
    for (int i=0; i < 0x40; ++i)
    {
        Write(0x9010,i);
        Write(0x9030,0);
    }

    divider = 0;
    OPLL_reset_patch (opll, patch_set);
    if (patch_custom) 
    {
      uint8_t dump[19 * 8];
      memcpy(dump, patch_custom, 16 * 8);
      memset(dump + 16 * 8, 0, 3 * 8);
      OPLL_setPatch(opll, dump);
    }
    OPLL_reset (opll);
  }

  void NES_VRC7::SetStereoMix(int trk, int16_t mixl, int16_t mixr)
  {
      if (trk < 0) return;
      //if (trk > 5) return;
      if (trk > 8) return; // HACK YM2413
      sm[0][trk] = mixl;
      sm[1][trk] = mixr;
  }

  ITrackInfo *NES_VRC7::GetTrackInfo(int trk)
  {
    //if(opll&&trk<6)
    if(opll&&trk<9) // HACK YM2413 (percussion mode isn't very diagnostic this way though)
    {
      trkinfo[trk].max_volume = 15;
      trkinfo[trk].volume = 15 - ((opll->reg[0x30+trk])&15);
      trkinfo[trk]._freq = opll->reg[0x10+trk]+((opll->reg[0x20+trk]&1)<<8);
      int blk = (opll->reg[0x20+trk]>>1)&7;
      trkinfo[trk].freq = clock*trkinfo[trk]._freq/(double)(0x80000>>blk);
      trkinfo[trk].tone = (opll->reg[0x30+trk]>>4)&15;
      //trkinfo[trk].key = (opll->reg[0x20+trk]&0x10)?true:false;

      // Two bits per channel, so channel 1 starts at bit 2. Shifting by one put every channel's
      // window on top of its neighbour's: keying channel 0 lit channels 1 and 2 as well, and the
      // keyboard showed voices nobody had started.
      trkinfo[trk].key = (opll->slot_key_status & (3 << (trk * 2))) != 0;
      return &trkinfo[trk];
    }
    else
      return NULL;
  }

  bool NES_VRC7::GetFmParameters (int trk, int& multiple, int& level, int& waveform, int& feedback)
  {
    if (opll == nullptr || trk < 0 || trk >= 6)
    {
      return false;
    }

    // Slot 2n is the modulator and 2n+1 the carrier, which is how OPLL_update lays them out.
    const OPLL_PATCH* modulator = opll->slot[trk * 2].patch;
    const OPLL_PATCH* carrier = opll->slot[trk * 2 + 1].patch;

    if (modulator == nullptr || carrier == nullptr)
    {
      return false;
    }

    multiple = (int) modulator->ML;
    level = (int) modulator->TL;
    waveform = (int) carrier->WS;
    feedback = (int) carrier->FB;

    return true;
  }

  bool NES_VRC7::Write (uint32_t adr, uint32_t val, uint32_t id)
  {
    if (adr == 0x9010)
    {
      OPLL_writeIO (opll, 0, val);
      return true;
    }
    if (adr == 0x9030)
    {
      OPLL_writeIO (opll, 1, val);
      return true;
    }
    else
      return false;
  }

  bool NES_VRC7::Read (uint32_t adr, uint32_t & val, uint32_t id)
  {
    return false;
  }

  void NES_VRC7::Tick (uint32_t clocks)
  {
    divider += clocks;
    while (divider >= 36)
    {
        divider -= 36;
        OPLL_calc(opll);
    }
  }

  uint32_t NES_VRC7::Render (int32_t b[2])
  {
    b[0] = b[1] = 0;
    for (int i=0; i < 6; ++i)
    {
        int32_t val = (mask & (1<<i)) ? 0 : opll->ch_out[i] >> 4;
        b[0] += val * sm[0][i];
        b[1] += val * sm[1][i];
    }

    // HACK for YM2413 support
    if (use_all_channels)
    {
        for (int i=6; i < 9; ++i)
        {
            if (mask & (1<<i)) continue;
            
            int32_t val;
            if (opll->patch_number[i] > 15) // rhytm mode
            {
              if      (i == 6) val = opll->ch_out[9]; // BD
              else if (i == 7) val = opll->ch_out[10] + opll->ch_out[11]; // HH&SD
              else             val = opll->ch_out[12] + opll->ch_out[13]; // TOM&CYM
                  /*  (i == 8) is implied */
            }
            else
            {
              val = opll->ch_out[i];
            }
            val >>= 4;
            b[0] += val * sm[0][i];
            b[1] += val * sm[1][i];
        }
    }

    b[0] >>= (7 - 4);
    b[1] >>= (7 - 4);

    // master volume adjustment
    const int32_t MASTER = int32_t(1.15 * 256.0);
    b[0] = (b[0] * MASTER) >> 8;
    b[1] = (b[1] * MASTER) >> 8;

    return 2;
  }

  uint32_t NES_VRC7::RenderChannels (int32_t* dest, int stride, int maxChannels)
  {
    // One Render() per channel with the others muted, then the mask put back. Correct, and it
    // keeps this file out of the chip's internal mixing - but it costs one render per channel, so
    // it is the shape to improve once the channels are exposed directly.
    //
    // `saved` is ORed in rather than discarded: it is what the mixer set for solo, track mute and
    // the chip's own checkbox. Overwriting it meant those controls did nothing in this path -
    // the mix was right, because the mix does not come through here, but every track played.
    const int saved = mask;

    for (int i = 0; i < 6; ++i)
    {
      mask = ~(1 << i) | saved;

      int32_t b[2] = { 0, 0 };
      Render (b);

      if (i < maxChannels)
      {
        dest[i * stride + 0] = b[0];
        dest[i * stride + 1] = b[1];
      }
    }

    mask = saved;
    return 6;
  }

}
