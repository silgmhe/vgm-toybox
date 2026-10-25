#ifndef __EMUTYPES_H__
#define __EMUTYPES_H__

#if ! defined(_STDINT_H) && ! defined(__int8_t_defined)

#ifdef HAVE_STDINT_H
#include <stdint.h>
#else
#include "../../stdtype.h"

#define __int8_t_defined
typedef uint8_t uint8_t;
typedef int8_t int8_t;
typedef uint16_t uint16_t;
typedef int16_t int16_t;
typedef uint32_t uint32_t;
typedef int32_t int32_t;
typedef uint64_t uint64_t;
typedef int64_t int64_t;
#endif

#endif

#endif	// __EMUTYPES_H__
