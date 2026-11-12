#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Init/Term , global */
void pvr_sb_Init(void);
void pvr_sb_Term(void);
/* Reset -> Reset - Initialise */
void pvr_sb_Reset(bool hard);

#ifdef __cplusplus
}
#endif
