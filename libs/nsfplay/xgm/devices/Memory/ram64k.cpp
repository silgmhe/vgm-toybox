#include <assert.h>
#include <cstring>
#include "ram64k.h"

namespace xgm
{
/////////////////////////////////////////////////////////////////////////////

RAM64K::RAM64K()
{
}

RAM64K::~RAM64K()
{
}

void
RAM64K::Reset ()
{
  memset (image, 0, 0x10000);
}

bool
RAM64K::SetImage (uint8_t * data, uint32_t offset, uint32_t size)
{
  if( offset + size < 0x10000 )
    memcpy (image + offset, data, size );
  else 
    memcpy (image + offset, data, 0x10000 - offset);
  return true;
}

bool
RAM64K::Write (uint32_t adr, uint32_t val, uint32_t id)
{
  image[adr & 0xFFFF] = val&0xFF;
  return true;
}

bool
RAM64K::Read (uint32_t adr, uint32_t & val, uint32_t id)
{
  val = image[adr & 0xFFFF];
  return true;
}

/////////////////////////////////////////////////////////////////////////////
}// namespace
