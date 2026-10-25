#ifndef _ECHO_H_
#define _ECHO_H_
#include "filter.h"

namespace xgm {

  class EchoUnit : public IRenderable {
  protected:
    int rate;
    int32_t *echo_buf;
    int h[32];
    int eidx, edelay;
    Filter lpf;
    DCFilter hpf;

  public:
    EchoUnit(){ echo_buf = new int32_t[1<<17]; }
    ~EchoUnit(){ delete [] echo_buf; }
    void Reset();
    void SetRate(double r);
    virtual void Tick(uint32_t clocks);
    virtual uint32_t Render(int32_t b[2]);
    inline uint32_t FastRender(int32_t b[2]);
  };

} // xgm

#endif
