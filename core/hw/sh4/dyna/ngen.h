/*
	Header file for native generator interface
	Needs some cleanup


	SH4 -> Code gen -> Ram

	Ram -> link/relocate -> Staging buffer
	Ram -> link/relocate -> Steady-state buffer

	Staging      : scratch, relatively small, circular code buffer
	Steady state : 'final' code buffer. When blocks reach a steady-state, they get copied here

	When the Staging buffer is full, a reset is done on the dynarec.
	If the stating buffer is full, but re-locating everything will free enough space, it will be relocated (GC'd)

	If the stating buffer is full, then blocks in it are put into "hibernation"

	Block can be
	in Ram, only ('hibernated')
	in Ram + steady state buffer
	in Ram + staging buffer

	Changes required on the ngen/dynarecs for this to work

	- Support relocation
	- Support re-linking
	- Support hibernated blocks, or block removal

	Changes on BM

	- Block graph
	- Block removal
	- Relocation driving logic


	This will enable

	- Extensive block specialisation (Further mem opts, other things that might gain)
	- Possibility of superblock chains
*/

#pragma once
#include "rec_config.h"
#include "decoder.h"
#include "blockmanager.h"

/* (CC_RW2RX() and CC_RX2RW() are in blockmanager.h) */

#ifdef __cplusplus
extern "C" {
#endif

/* alternative emit ptr, set to 0 to use the main buffer */
extern u32* emit_ptr;

void emit_Write32(u32 data);
void emit_Skip(u32 sz);
u32 emit_FreeSpace(void);
void* emit_GetCCPtr(void);
void emit_SetBaseAddr(void);

/* Called from ngen_FailedToFindBlock */
DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock(u32 pc);
DynarecCodeEntryPtr DYNACALL rdv_FailedToFindBlock_pc(void);
/* Called when a block check failed, and the block needs to be invalidated */
DynarecCodeEntryPtr DYNACALL rdv_BlockCheckFail(u32 addr);
/* Called to compile code @pc */
DynarecCodeEntryPtr rdv_CompilePC(u32 blockcheck_failures);
/* Finds or compiles code @pc */
DynarecCodeEntryPtr rdv_FindOrCompile(void);

/* code -> pointer to code of block, dpc -> if dynamic block, pc. if cond, 0 for next, 1 for branch */
void* DYNACALL rdv_LinkBlock(u8* code,u32 dpc);

/* With the MMU on a block does not, as a rule, go straight on into the
 * next: where the next one's address is depends on what the TLB says by
 * then. Two kinds of address do not: one in P1 or P2, which the MMU does
 * not translate; and one in the same page as the block that is leaving
 * for it, which is in the same page of memory as that block whatever page
 * that is - and a block is only ever entered by an address that has just
 * been translated to where it is, or from a block of its page. Most of a
 * Windows CE game's blocks end that way.
 *
 * rdv_MmuMayGoOn(): whether @block may go on at all - the MMU is on, and
 * not the strict way (where nothing is to be left to chance), and the page
 * the block is in is not one of 1K, of which four different ones make up
 * what is called the same page here.
 * rdv_MmuSamePlace(): whether @target is such an address for @block, and
 * if it is, where it is: @addr, which is what the block manager goes by. */
bool rdv_MmuMayGoOn(const RuntimeBlockInfo *block);
bool rdv_MmuSamePlace(const RuntimeBlockInfo *block, u32 target, u32 *addr);

u32 DYNACALL rdv_DoInterrupts(void* block_cpde);
u32 DYNACALL rdv_DoInterrupts_pc(u32 pc);

/* Stuff to be implemented per dynarec core */

void ngen_init(void);

/* Called to compile a block */
void ngen_Compile(RuntimeBlockInfo* block,bool force_checks, bool reset, bool staging,bool optimise);

/* Called when blocks are reseted */
void ngen_ResetBlocks(void);
/* Value to be returned when the block manager failed to find a block, */
/* should call rdv_FailedToFindBlock and then jump to the return value */
extern void (*ngen_FailedToFindBlock)(void);
/* the dynarec mainloop */
void ngen_mainloop(void* cntx);
/* ngen features */

void ngen_GetFeatures(ngen_features* dst);
void ngen_HandleException(void);

/* Canonical callback interface */
/* (ngen_CC_*(): shil.h) */

#ifdef __cplusplus
}
#endif
