#ifndef __CST_STDTYPE_H__
#define __CST_STDTYPE_H__

// This header used to define UINT8/UINT16/UINT32/UINT64 and their signed counterparts - from
// <stdint.h> when the build had one, from MAME's types when it did not. Every use of those names
// is uint32_t and friends now, so the definitions are gone and only the include is left:
// a hundred and thirty-odd files pull <cstdint> in through here, and that is the one thing this
// file still does.
#include <stdint.h>

#endif	// __CST_STDTYPE_H__
