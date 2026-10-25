#ifndef __RATIOCNTR_H__
#define __RATIOCNTR_H__

#include "../common_def.h"

#if ! LOW_PRECISION_RATIOCNTR
// by default we use a high-precision 32.32 fixed point counter
#define RC_SHIFT	32
typedef uint64_t		RC_TYPE;
typedef int64_t		RC_STYPE;
#else
// alternatively we can use lower-precision 12.20 fixed point
#define RC_SHIFT	20
typedef uint32_t		RC_TYPE;
typedef int32_t		RC_STYPE;
#endif

typedef struct
{
	RC_TYPE inc;	// counter increment
	RC_TYPE val;	// current value
} RATIO_CNTR;

INLINE void RC_SET_RATIO(RATIO_CNTR* rc, uint32_t mul, uint32_t div)
{
	rc->inc = (RC_TYPE)((((uint64_t)mul << RC_SHIFT) + div / 2) / div);
}

INLINE void RC_SET_INC(RATIO_CNTR* rc, double val)
{
	rc->inc = (RC_TYPE)(((RC_TYPE)1 << RC_SHIFT) * val + 0.5);
}

INLINE void RC_STEP(RATIO_CNTR* rc)
{
	rc->val += rc->inc;
}

INLINE void RC_STEPS(RATIO_CNTR* rc, uint32_t step)
{
	rc->val += rc->inc * step;
}

INLINE uint32_t RC_GET_VAL(const RATIO_CNTR* rc)
{
	return (uint32_t)(rc->val >> RC_SHIFT);
}

INLINE void RC_SET_VAL(RATIO_CNTR* rc, uint32_t val)
{
	rc->val = (RC_TYPE)val << RC_SHIFT;
}

INLINE void RC_RESET(RATIO_CNTR* rc)
{
	rc->val = 0;
}

INLINE void RC_RESET_PRESTEP(RATIO_CNTR* rc)
{
	rc->val = ((RC_TYPE)1 << RC_SHIFT) - rc->inc;
}

INLINE void RC_MASK(RATIO_CNTR* rc)
{
	rc->val &= (((RC_TYPE)1 << RC_SHIFT) - 1);
}

INLINE void RC_VAL_INC(RATIO_CNTR* rc)
{
	rc->val += (RC_TYPE)1 << RC_SHIFT;
}

INLINE void RC_VAL_DEC(RATIO_CNTR* rc)
{
	rc->val -= (RC_TYPE)1 << RC_SHIFT;
}

INLINE void RC_VAL_ADD(RATIO_CNTR* rc, int32_t val)
{
	rc->val += (RC_STYPE)val << RC_SHIFT;
}

INLINE void RC_VAL_SUB(RATIO_CNTR* rc, int32_t val)
{
	rc->val -= (RC_STYPE)val << RC_SHIFT;
}

#endif	// __RATIOCNTR_H__
