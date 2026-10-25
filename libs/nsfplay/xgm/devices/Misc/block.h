#ifndef _BLOCK_H_
#define _BLOCK_H_
#include "../device.h"

namespace xgm
{
  // Layerに挟んで，このデバイス以下のデバイスアクセスを完全にブロックするデバイス
  class BlockingDevice : public IDevice
  {
  public:
    void Reset(){}
    bool Write(uint32_t adr, uint32_t val, uint32_t id = 0)
    {
      return blocking;
    }
    bool Read(uint32_t adr, uint32_t &val, uint32_t id = 0)
    {
      return false;
    }
  };
}// namespace

#endif _BLOCK_H_
