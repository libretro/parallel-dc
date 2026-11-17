/* What of the recompiler a state's loading needs, for C and C++ alike. */
#ifndef DYNA_STATE_H
#define DYNA_STATE_H
#include "types.h"
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Every block thrown away */
void bm_Reset(void);
/* Main memory as a state has it at @from: the blocks made from pages that
 * changed thrown away, the rest kept */
void bm_LoadRam(const u8 *from);
/* The size of the decoder's state_t, which states before V9 hold */
extern const unsigned dec_state_size;

RETRO_END_DECLS

#endif
