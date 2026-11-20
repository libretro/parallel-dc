#ifndef VRAM_LOCK_H
#define VRAM_LOCK_H
/* The locks on video memory (vram_lock.c): what a texture was made from,
 * write-protected while it is, so that a write to it - the game's, a DMA's,
 * a state's - has the texture made again. C89, for C and C++ alike. */
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Room for the locks of @textures textures, made now, so that locking one
 * takes none: more is made if it runs out, and that is said (render
 * thread) */
void vramlock_reserve(u32 textures);
/* What [@start, @end] of video memory is made into, @owner, locked: the
 * writes that came before taken in first (dropping what they hit), then a
 * lock made for @owner into *@slot, if it has none (render thread) */
void vramlock_lock(u32 start, u32 end, void *owner, vram_block **slot);
/* A lock dropped and its room given back (render thread) */
void vramlock_unlock(vram_block *block);
/* The writes the emulation thread has marked taken in: every lock on a page
 * written dropped, its owner told (render thread) */
void vramlock_collect(void);
/* Whether there are any to take in (render thread) */
bool vramlock_pending(void);
/* Moved for every written page taken in (render thread) */
extern u32 vram_write_epoch;

/* The owner of a lock dropped by a write (or a render over it) is told,
 * and the lock is given back after: the texture cache's (TexCache.c) */
void vramlock_dropped(void *owner);

/* A write to video memory hit a protected page at @offset: it is opened
 * again and marked written. From the fault handler: system calls and
 * atomics only. False if it is not video memory. */
bool VramLockedWriteOffset(size_t offset);
/* The same at the host's @address (common.c's fault handler) */
bool VramLockedWrite(u8 *address);
/* The same for a run of pages, with one change of protection for all of
 * them. Any thread, the machine standing still. */
void VramLockedWriteRange(u32 offset, u32 size);
/* A render to a texture covers @size bytes from @offset: every texture
 * made from them is made again, but @target, the one rendered to (render
 * thread, the machine standing still) */
void VramRendered(u32 offset, u32 size, const void *target);
/* A block copy (a DMA) is about to write all of @size from @offset */
void VramBlockWrite(u32 offset, u32 size);
/* Video memory from a save state, all of it at @from */
void vram_load(const u8 *from);

#ifdef __cplusplus
}
#endif

#endif
