#include <cstring>
#include "echo.h"

using namespace xgm;

void EchoUnit::Reset()
{
  int hdef[16] = { 
      0,  0,  0,  0,
     64, 32, 16,  8,
     32, 16,  8,  4,
     16,  8,  4,  2,
  };
  eidx = 0;
  memset(echo_buf,0,sizeof(int32_t)*(1<<17));
  for(int i=0; i<16; i++) h[i] = hdef[i];
  lpf.SetParam(4700,100);
  lpf.Reset();
  hpf.SetParam(270,100);
  hpf.Reset();
}

void EchoUnit::SetRate(double rate)
{
  edelay = ((int)rate)/16;
  lpf.SetRate(rate);
  hpf.SetRate(rate);
}

inline uint32_t EchoUnit::FastRender(int32_t b[2])
{
  int32_t buf[2];

  int tmp = eidx;
  for(int i=0; i<16; i++) 
  {
    echo_buf[tmp&((1<<17)-1)] += (b[0]*h[i])>>8;
    tmp += edelay;
  }
  buf[0] = buf[1] = echo_buf[eidx];
  lpf.FastRender(buf);
  hpf.FastRender(buf);
  echo_buf[eidx] = 0;
  eidx = (eidx + 1)&((1<<17)-1);

  b[0] += buf[0];
  b[1] += buf[1];

  return 2;
}

void xgm::EchoUnit::Tick(uint32_t clocks)
{
  lpf.Tick(clocks);
  //hpf.Tick(clocks); // hpf has no sub-renderable
}

uint32_t EchoUnit::Render(int32_t b[2])
{
  return FastRender(b);
}
