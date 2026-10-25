#ifndef _XTYPES_H_
#define _XTYPES_H_

#if defined(_MSC_VER)
#define INLINE __forceinline
#elif defined(__GNUC__)
#define INLINE __inline__
#elif defined(_MWERKS_)
#define INLINE inline
#else
#define INLINE
#endif

// The (U)INTnn aliases that used to live here are gone: the sources say uint32_t now, which
// is the same type on every compiler this project builds with, and says so without a namespace to
// qualify it. What is left is INLINE, which upstream's sources use, and the standard header the
// rest of them need.
#include <stdint.h>

#endif	// _XTYPES_H_
