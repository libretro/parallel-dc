/* What arm7_test.c needs besides itself on 32-bit ARM, where the ARM7's
 * main loop is in core/rec-ARM/ngen_arm.S with the SH4's (tools/arm7/
 * run.sh): the SH4's parts that file goes to, never gone to here. */
#include <stdlib.h>
#include <sys/mman.h>

#include "types.h"

bool mem_region_set_exec(void *start, size_t len)
{
	const uintptr_t from = (uintptr_t)start & ~(uintptr_t)4095;
	return mprotect((void *)from, ((uintptr_t)start + len - from + 4095) & ~(uintptr_t)4095,
			PROT_READ | PROT_WRITE | PROT_EXEC) == 0;
}

void ta_vtx_data32(void) { abort(); }
void YUV_data(void) { abort(); }
void TAWriteSQ_to_vram(void) { abort(); }
void rdv_LinkBlock(void) { abort(); }
void rdv_FailedToFindBlock(void) { abort(); }
void rdv_BlockCheckFail(void) { abort(); }
void rdv_DoInterrupts(void) { abort(); }
void rec_arm_jmp_env(void) { abort(); }
void rec_arm_mmu_on(void) { abort(); }
void rec_arm_code_mmu(void) { abort(); }
void UpdateSystem(void) { abort(); }
