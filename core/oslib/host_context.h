#pragma once
#include "types.h"

struct host_context_t {
	size_t pc;		/* 0 where the host's is not had (CPU_GENERIC) */

#if HOST_CPU == CPU_X86
	u32 eax;
	u32 ecx;
	u32 esp;
#elif HOST_CPU == CPU_ARM
	u32 r[15];
#elif HOST_CPU == CPU_ARM64
	u64 x2;
#endif
};
