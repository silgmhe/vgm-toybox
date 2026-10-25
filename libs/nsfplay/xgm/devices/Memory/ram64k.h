#ifndef _RAM64K_H_
#define _RAM64K_H_
#include "../device.h"

namespace xgm
{
  class RAM64K : public IDevice
  {

  public:
    uint8_t image[0x10000];
      RAM64K ();
     ~RAM64K ();
    void Reset ();
    bool Read (uint32_t adr, uint32_t & val, uint32_t id=0);
    bool Write (uint32_t adr, uint32_t val, uint32_t id=0);
    bool SetImage (uint8_t * data, uint32_t offset, uint32_t size);
  };

} // namespace

#endif
