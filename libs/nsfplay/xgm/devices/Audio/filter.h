#ifndef _FILTER_H_
#define _FILTER_H_
#include <math.h>
#include "../device.h"


namespace xgm
{
  class Compressor : public IRenderable
  {
  private:
    int32_t strength;
    int32_t limit;
    int32_t threshold;

  public:
    Compressor()
    {
      SetParam(32767,1.0,1.0);
    }
    ~Compressor()
    {
    }

    void Reset()
    {
    }

    void SetRate(double rate)
    {
    }

    void SetParam(double lim, double thr, double str)
    {
      if(lim<0.0) lim=0.0;
      limit = (lim<1.0)?((int32_t)(lim*32767)):0;
      threshold = (int32_t)(thr * limit);
      strength =  (int32_t)(str * (1<<12));
    }

    inline int32_t Limiter(int32_t in)
    {
      if( threshold < in)
        in = threshold + (((in - threshold) * strength) >> 12);
      else if( in < -threshold )
        in = -threshold + (((in + threshold) * strength) >> 12);

      if(limit)
      {
        if(limit<in)
          in = limit;
        else if(in<-limit)
          in = -limit;
      }

      return in;
    }

    void Tick(uint32_t clocks)
    {
    }

    uint32_t Render(int32_t b[2])
    {
      return FastRender(b);
    }

    inline uint32_t FastRender(int32_t b[2])
    {
      b[0] = Limiter(b[0]);
      b[1] = Limiter(b[1]);
      return 2;
    }
  };

  class DCFilter : public IRenderable
  {
  private:
    double R, C;
    double a;
    double in[2], out[2];
    double rate;

  public:
    DCFilter()
    {
      R = C = 0.0;
      Reset();
    }

    ~DCFilter()
    {
    }

    void UpdateFactor()
    {
      if(C==0.0||R==0.0)
      {
        a = 2.0; // disable
      }
      else
      {
        a = (R*C) / ((R*C) + (1.0/rate));
      }
    }

    double GetFactor() { return a; }

    void SetRate(double r)
    {
      // カットオフ周波数 : 2pi*R*C
      rate = r;
      UpdateFactor();
    }

    void SetParam(double r, int c) // c = 0-256, 256 = off, 0 = max
    {
      R = r;
      //C = c;

      if (c > 255)
        C = 0.0; // disable
      else
        C = 2.0e-4*(1.0-::pow(1.0-(double(c+1)/256.0),0.05));

      // the goal of this curve is to have a wide range of practical use,
      // though it may look a little complicated. points of interest:
      //   HPF = 163 ~ my NES
      //   HPF = 228 ~ my Famicom
      //   low values vary widely and have an audible effect
      //   high values vary slowly and have a visible effect on DC offset

      UpdateFactor();
    }

    // 非virtualなRender
    inline uint32_t FastRender(int32_t b[2])
    {
      if(a<1.0)
      {
        out[0] = a * ( out[0] + b[0] - in [0] );
        in[0] = b[0];
        b[0] = (int32_t)out[0];

        out[1] = a * ( out[1] + b[1] - in [1] );
        in[1] = b[1];
        b[1] = (int32_t)out[1];
      }
      return 2;
    }

    uint32_t Render (int32_t b[2])
    {
      return FastRender(b);
    }

    void Tick(uint32_t clocks)
    {
    }

    void Reset()
    {
      in[0] = in[1] = 0;
      out[0] = out[1] = 0;
    }

    void SetLevel(int32_t b[2])
    {
      in[0] = b[0];
      in[1] = b[1];
    }
  };

  class SimpleFIR
  {
  protected:
	  int32_t *tap;
	  double *h;
	  int N,M;
	  double rate;
	  double cutoff;
  public:
	  SimpleFIR(int tap_num);
	  virtual ~SimpleFIR();
	  void SetRate(double r);
	  void SetCutoff(double c);
	  void Reset();
	  void Put(int32_t wav);
	  int32_t Get();
  };
 
  class Filter : public IRenderable
  {
  protected:
    IRenderable * target;

    int type;
    int32_t out[2];
    double a;
    double rate, R, C;
    bool disable;
    const int GETA_BITS;
  public:
    Filter () : GETA_BITS(20)
    {
      target = NULL;
      rate = DEFAULT_RATE;
      R = 4700;
      C = 10.0E-9;
      disable = false;
      out[0]=out[1]=0;
    }

    virtual ~Filter ()
    {
    }

    void Attach (IRenderable * t)
    {
      target = t;
    }

    inline uint32_t FastRender (int32_t b[2])
    {
      if(target) target->Render (b);
      if(a<1.0)
      {
        out[0]+=(int32_t)(a*(b[0]-out[0]));
        out[1]+=(int32_t)(a*(b[1]-out[1]));
        b[0]=out[0];
        b[1]=out[1];
      }
      return 2;
    }

    virtual void Tick(uint32_t clocks)
    {
      if (target) target->Tick(clocks);
    }

    virtual uint32_t Render(int32_t b[2])
    {
      return FastRender(b);
    }

    void SetParam (double r, int c) // c = 0-400, 0=off, 400=max
    {
      //C = 1.0E-10 * c;
      R = r;

      C = ::pow(double(c) / 400.0, 2.0) * 1.0E-10 * 400.0;
      // curved to try to provide useful range of settings
      //   LPF = 112 ~ my NES

      UpdateFactor();
    }

    void SetClock (double clock)
    {
      Reset ();
    }

    void SetRate (double r)
    {
      rate = r;
      UpdateFactor();
    }

    void UpdateFactor()
    {
      if (R!=0.0&&C!=0.0&&rate!=0.0)
        a = (1.0/rate) / ((R*C) + (1.0/rate));
      else
        a = 2.0; // disabled
    }

    double GetFactor() { return a; }

    void Reset ()
    {
      UpdateFactor();
      out[0] = out[1] = 0;
    }

  };

}  // namespace

#endif
