#pragma once
#include "types.h"

#ifdef _MSC_VER
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

double os_GetSeconds(void);

void os_DoEvents(void);

static TYPES_INLINE u32 bitscanrev(u32 v)
{
#ifdef __GNUC__
	return 31-__builtin_clz(v);
#else
	unsigned long rv;
	_BitScanReverse(&rv,v);
	return rv;
#endif
}

void os_DebugBreak(void);

#ifdef __cplusplus
}
#endif
