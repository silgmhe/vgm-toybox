#ifndef _FADER_H_
#define _FADER_H_
#include "../device.h"
#include <vector>

namespace xgm
{
  class Fader : virtual public IRenderable
  {
  protected:
    uint32_t fade_pos, fade_end;
    IRenderable* d;

  public:
    Fader ()
    {
      d = NULL;
      Reset();
    }

     ~Fader ()
    {
    }

    void Attach (IRenderable * dev)
    {
      d = dev;
    }

    void Reset ()
    {
      fade_pos = 0;
      fade_end = 1;
    }

    bool IsFadeEnd ()
    {
      return (fade_pos >= fade_end);
    }

    bool IsFading ()
    {
      return (fade_pos > 0);
    }

    void FadeStart (double rate, int fade_in_ms)
    {
      if (fade_in_ms)
      {
        const uint32_t MAX_SAMPLES = (uint32_t)~0UL;
        double samples = (double)fade_in_ms * rate / 1000.0;
        if (samples < (double)MAX_SAMPLES)
        {
          fade_end = uint32_t(samples);
        }
        else
        {
          fade_end = MAX_SAMPLES;
        }
      }
      else
      {
        fade_end = 1;
      }
      fade_pos = 1; // begin fade
    }

    virtual void Skip ()
    {
      if (fade_pos > 0)
      {
        if (fade_pos < fade_end) ++fade_pos;
        else fade_pos = fade_end;
      }
      d->Skip();
    }

    virtual void Tick (uint32_t clocks)
    {
      d->Tick(clocks);
    }

    virtual uint32_t Render (int32_t b[2])
    {
      d->Render (b);
      if (fade_pos > 0)
      {
        double fade_amount = double(fade_end - fade_pos) / double(fade_end);
        b[0] = int32_t(fade_amount * b[0]);
        b[1] = int32_t(fade_amount * b[1]);

        if (fade_pos < fade_end) ++fade_pos;
        else fade_pos = fade_end;
      }
      return 2;
    }
  };

}                               // namespace

#endif
