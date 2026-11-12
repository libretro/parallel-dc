/* nullAICA.cpp : Defines the entry point for the DLL application. */
/* */

#include "arm7.h"

/* called when plugin is used by emu (you should do first time init here) */
s32 libARM_Init(void)
{
	aicaarm_init();

	return 0;
}

/* It's supposed to reset anything */
void libARM_Reset(bool hard)
{
	aicaarm_reset();
	aicaarm_enable(false);
}
