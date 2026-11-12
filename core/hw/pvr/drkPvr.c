/* The PowerVR's entry points: its registers, the sync generator, the tile
 * accelerator and the renderer, together */

#include "elan_host.h"
#include "spg.h"
#include "pvr_regs.h"
#include "pvr_mem.h"
#include "ta.h"
#include "Renderer_if.h"

void libPvr_Reset(bool hard)
{
   KillTex = true;
   Regs_Reset(hard);
   spg_Reset(hard);
   /* what the last run left half sent to the tile accelerator is not this run's */
   tactx_Reset();
   ta_vtx_Reset();
   elan_host_reset(hard);
   if (hard)
      YUV_reset();
   else
   {
      /* The registers keep what the game set the YUV converter up with,
       * and so does the converter; a macroblock the last run left half
       * sent is not the start of this run's first. */
      YUV_index = 0;
   }
}


s32 libPvr_Init(void)
{
   if (!spg_Init())
   {
      /* failed */
      return -1;
   }
   elan_host_init();

	return 0;
}

/* called when exiting from sh4 thread, from the new thread context (for any thread specific de init) */
void libPvr_Term(void)
{
   custom_texture_terminate();	/* Avoid deadlock on exit (win32) */
   rend_term();
   spg_Term();
   elan_host_term();
}
