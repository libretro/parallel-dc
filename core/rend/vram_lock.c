/* The locks on video memory (vram_lock.h) */
#include <stdlib.h>
#include <string.h>
#include "vram_lock.h"
#include <retro_atomic.h>
#include "hw/mem/_vmem.h"
#include "hw/pvr/pvr_mem.h"
#include "rtt_watch.h"

/* The locks on each page of video memory: a list, the page being
 * protected while it has any. A lock is on each page of the texture's, as
 * a node of each page's list; the nodes and the locks are in arenas made
 * when the renderer starts, for the textures the cache has room for
 * (texcache_reserve()), the free ones linked. Taking a lock and dropping
 * one allocate nothing; an arena that runs out is made larger, and that is
 * said. Render thread only. */
typedef struct VramLockNode
{
	vram_block *lock;
	u32 next;		/* +1, 0: none */
} VramLockNode;
static u32 VramLocks[VRAM_SIZE_MAX / PAGE_SIZE];	/* each page's first node, +1; 0: none */
static VramLockNode *vramlock_nodes;
static u32 vramlock_node_count, vramlock_free_nodes;
VArray2 vram;  /* vram 32-64b */

/* Locks gone, kept for the next (linked by userdata): a texture rendered
 * to is locked again each frame, and takes one of these, not new memory */
static vram_block *vramlock_free_blocks;
static u32 vramlock_block_count;
static bool vramlock_said;

static void vramlock_ran_out(const char *what)
{
	if (vramlock_said)
		return;
	vramlock_said = true;
	WARN_LOG(RENDERER, "Video memory locks: no %s left, more made (the room for the texture cache is too small for this game)\n", what);
}

/* @count more locks, onto the free list */
static bool vramlock_blocks_more(u32 count)
{
	vram_block *blocks = (vram_block *)malloc((size_t)count * sizeof(vram_block));
	u32 i;
	if (blocks == NULL)
		return false;
	for (i = 0; i < count; i++)
	{
		blocks[i].userdata = vramlock_free_blocks;
		vramlock_free_blocks = &blocks[i];
	}
	vramlock_block_count += count;
	return true;
}

/* Room for @count nodes: more, onto the free list (the arena is moved;
 * the lists hold the nodes' places in it, not their addresses) */
static bool vramlock_nodes_more(u32 count)
{
	VramLockNode *nodes = (VramLockNode *)realloc(vramlock_nodes, ((size_t)vramlock_node_count + count) * sizeof(VramLockNode));
	u32 i;
	if (nodes == NULL)
		return false;
	vramlock_nodes = nodes;
	for (i = vramlock_node_count + count; i-- > vramlock_node_count; )
	{
		vramlock_nodes[i].next = vramlock_free_nodes;
		vramlock_free_nodes = i + 1;
	}
	vramlock_node_count += count;
	return true;
}

void vramlock_reserve(u32 textures)
{
	/* (a texture has one lock; on four pages, most of them less: a lock
	 * on more takes the room of smaller ones) */
	if (vramlock_block_count < textures)
		vramlock_blocks_more(textures - vramlock_block_count);
	if (vramlock_node_count < textures * 4)
		vramlock_nodes_more(textures * 4 - vramlock_node_count);
}

static vram_block *vramlock_block_new(void)
{
	vram_block *block;
	if (vramlock_free_blocks == NULL)
	{
		vramlock_ran_out("lock");
		if (!vramlock_blocks_more(vramlock_block_count > 64 ? vramlock_block_count / 2 : 64))
			return NULL;
	}
	block = vramlock_free_blocks;
	vramlock_free_blocks = (vram_block *)block->userdata;
	return block;
}

/* List functions */
static void vramlock_list_remove(vram_block* block)
{
	u32 base = block->start / PAGE_SIZE;
	u32 end = block->end / PAGE_SIZE;
	u32 i;

	for (i = base; i <= end; i++)
	{
		u32 *at = &VramLocks[i];
		while (*at != 0)
		{
			VramLockNode *node = &vramlock_nodes[*at - 1];
			if (node->lock == block)
			{
				const u32 n = *at;
				*at = node->next;
				node->next = vramlock_free_nodes;
				vramlock_free_nodes = n;
			}
			else
				at = &node->next;
		}
	}
}

static void vramlock_list_add(vram_block* block)
{
	u32 base = block->start / PAGE_SIZE;
	u32 end = block->end / PAGE_SIZE;
	u32 i;

	for (i = base; i <= end; i++)
	{
		u32 n;
		/* If the list has no lock then we need to protect vram, otherwise
		 * it's already been done - or a lock dropped since left it so
		 * (vramlock_drop()): no system call for that */
		if (VramLocks[i] == 0 && !_vmem_vram_page_protected(i))
			_vmem_protect_vram(i * PAGE_SIZE, PAGE_SIZE);
		if (vramlock_free_nodes == 0)
		{
			vramlock_ran_out("room");
			if (!vramlock_nodes_more(vramlock_node_count > 256 ? vramlock_node_count / 2 : 256))
				die("Out of memory");
		}
		n = vramlock_free_nodes;
		vramlock_free_nodes = vramlock_nodes[n - 1].next;
		vramlock_nodes[n - 1].lock = block;
		vramlock_nodes[n - 1].next = VramLocks[i];
		VramLocks[i] = n;
	}
}

/* Writes to protected video memory, by page.
 *
 * The lock lists above belong to the render thread; nothing else reads or
 * changes them. The emulation thread finds out that a protected page was
 * written when the write faults, possibly inside a signal handler, and all
 * it does there is open the page up again and mark it here. The render
 * thread collects the marks before it looks at any texture and drops every
 * lock on those pages, which is what marks their textures as changed.
 *
 * The emulation thread unprotects first and marks second. A page it has
 * opened up is therefore always marked afterwards, so the render thread
 * can go on treating "a page with locks on it is protected" as true: when
 * it is not, a mark is on its way that takes those locks off again. */
#define VRAM_PAGES (VRAM_SIZE_MAX / PAGE_SIZE)
static retro_atomic_int_t vram_writes[(VRAM_PAGES + 31) / 32];
static retro_atomic_int_t vram_writes_pending;

/* Emulation thread (or any): two atomic operations, nothing else */
static void vram_written(unsigned page)
{
	retro_atomic_fetch_or_int(&vram_writes[page >> 5], (int)(1u << (page & 31)));
	retro_atomic_store_release_int(&vram_writes_pending, 1);
}

/* Render thread: moved for every written page it collects. In a render
 * nothing else changes video memory under a texture already hashed in it:
 * a picture rendered to a texture is packed in when a read first touches
 * it, and the hash is such a read. */
u32 vram_write_epoch;

/* A lock dropped: its owner told, and the lock given back */
static void vramlock_drop(vram_block *block)
{
	vramlock_dropped(block->userdata);
	vramlock_unlock(block);
}

bool vramlock_pending(void)
{
	return retro_atomic_load_relaxed_int(&vram_writes_pending) != 0;
}

/* Render thread: apply the writes the emulation thread has marked. */
void vramlock_collect(void)
{
	int w;
	/* Clear the flag before looking: a mark that lands behind the scan
	 * sets it again. */
	if (!retro_atomic_exchange_int(&vram_writes_pending, 0))
		return;
	for (w = 0; w < (VRAM_PAGES + 31) / 32; w++)
	{
		unsigned word, b;

		if (!retro_atomic_load_relaxed_int(&vram_writes[w]))
			continue;
		word = (unsigned)retro_atomic_exchange_int(&vram_writes[w], 0);
		for (b = 0; word != 0; b++, word >>= 1)
		{
			const unsigned page = (unsigned)w * 32 + b;
			if (!(word & 1))
				continue;
			vram_write_epoch++;

			/* (dropping a lock takes it off every page it is on, this one's
			 * first node with it) */
			while (VramLocks[page] != 0)
			{
				const u32 first = VramLocks[page];
				vramlock_drop(vramlock_nodes[first - 1].lock);

				if (VramLocks[page] == first)
				{
					ERROR_LOG(PVR, "Error : pvr is supposed to remove lock\n");
					die("Invalid state");
				}
			}
		}
	}
}

void vramlock_lock(u32 start_offset64, u32 end_offset64, void *owner, vram_block **slot)
{
	if (end_offset64>(VRAM_SIZE-1))
	{
		WARN_LOG(PVR, "vramlock_Lock_64: end_offset64>(VRAM_SIZE-1) \n Tried to lock area out of vram , possibly bug on the pvr plugin\n");
		end_offset64=(VRAM_SIZE-1);
	}

	if (start_offset64>end_offset64)
	{
		WARN_LOG(PVR, "vramlock_Lock_64: start_offset64>end_offset64 \n Tried to lock negative block , possibly bug on the pvr plugin\n");
		start_offset64=0;
	}

	/* Writes that came before this lock are not its business. */
	vramlock_collect();

	if (*slot == NULL)
	{
		vram_block* block = vramlock_block_new();
		if (block == NULL)
			return;

		block->end=end_offset64;
		block->start=start_offset64;
		block->len=end_offset64-start_offset64+1;
		block->userdata = owner;
		block->type=64;

		/* This also protects vram if needed */
		vramlock_list_add(block);
		*slot = block;
	}
}

/* A write to video memory hit a protected page. Called on the emulation
 * thread from the fault handler, so it stays within what a signal handler
 * may do: system calls and atomics, no locks and no allocation. The render
 * thread calls it too, before it writes to video memory itself. */
bool VramLockedWriteOffset(size_t offset)
{
	if (offset >= VRAM_SIZE)
		return false;

	_vmem_unprotect_vram((u32)(offset & ~PAGE_MASK), PAGE_SIZE);
	vram_written((unsigned)(offset / PAGE_SIZE));

	return true;
}

/* The same for a run of pages, with one change of protection for all of
 * them. Any thread, the machine standing still. */
void VramLockedWriteRange(u32 offset, u32 size)
{
	u32 first, last, page;
	if (size == 0 || offset >= VRAM_SIZE)
		return;
	if (size > VRAM_SIZE - offset)
		size = VRAM_SIZE - offset;

	first = offset / PAGE_SIZE;
	last = (offset + size - 1) / PAGE_SIZE;

	_vmem_unprotect_vram(first * PAGE_SIZE, (last - first + 1) * PAGE_SIZE);
	for (page = first; page <= last; page++)
		vram_written(page);
}

/* Render thread, the machine standing still: a render to a texture covers
 * @size bytes from @offset. On the console it writes them, and a texture
 * made from them - another size, another format, a part of it - shows what
 * was there before; here the picture is on the graphics card, and video
 * memory gets it later (rend/rtt_watch.h) without a write that faults. So
 * every texture made from them is made again now, but @target, the one
 * rendered to. No page changes protection: a lock dropped leaves its page
 * as it is. */
void VramRendered(u32 offset, u32 size, const void *target)
{
	u32 end, page;
	if (size == 0 || offset >= VRAM_SIZE)
		return;
	if (size > VRAM_SIZE - offset)
		size = VRAM_SIZE - offset;
	vramlock_collect();

	end = offset + size - 1;
	for (page = offset / PAGE_SIZE; page <= end / PAGE_SIZE; page++)
	{
		u32 n = VramLocks[page];
		while (n != 0)
		{
			vram_block *lock = vramlock_nodes[n - 1].lock;
			/* (dropping one takes it off every page it is on: this node
			 * goes, the next stays) */
			n = vramlock_nodes[n - 1].next;
			if (lock->userdata != target && lock->start <= end && lock->end >= offset)
				vramlock_drop(lock);
		}
	}
}

/* A block copy (a DMA) is about to write every byte of @size from @offset
 * of video memory, on the thread that runs the machine. What its writes
 * would fault on a page at a time is done once for all of it: a picture
 * rendered there that it replaces whole is not fetched off the card first
 * (rtt_watch_overwrite()), and the protected pages are opened and marked as
 * written, each run of them with one change of protection - in the order a
 * fault has it, opened first and marked second. */
void VramBlockWrite(u32 offset, u32 size)
{
	u32 last, page;
	if (size == 0 || offset >= VRAM_SIZE)
		return;
	if (size > VRAM_SIZE - offset)
		size = VRAM_SIZE - offset;
	rtt_watch_overwrite(offset, size);

	last = (offset + size - 1) / PAGE_SIZE;
	for (page = offset / PAGE_SIZE; page <= last; )
	{
		u32 end;
		if (!_vmem_vram_page_protected(page))
		{
			page++;
			continue;
		}
		end = page + 1;
		while (end <= last && _vmem_vram_page_protected(end))
			end++;
		_vmem_unprotect_vram(page * PAGE_SIZE, (end - page) * PAGE_SIZE);
		for (; page < end; page++)
			vram_written(page);
	}
}

/* Video memory from a save state: @from is all of it. Any thread, the
 * machine standing still.
 *
 * It used to be copied over whole and every texture thrown away, to be made
 * again from the same bytes as often as not: with run-ahead a state is
 * loaded every frame, and it is the state of one frame ago. Here a page
 * that textures were made from - a protected one - is compared first. The
 * same in the state, it is left alone and so are its textures; another, it
 * is written as the game would have written it, which is what has them made
 * again. The pages nothing was made from are copied as before, a run of
 * them at a time. */
void vram_load(const u8 *from)
{
	u8 * const to = vram.data;
	u32 run = 0;		/* where the pages to be copied in one go start */
	u32 addr;

	for (addr = 0; addr < VRAM_SIZE; addr += PAGE_SIZE)
	{
		if (!_vmem_vram_page_protected(addr / PAGE_SIZE))
			continue;
		if (addr > run)
			memcpy(to + run, from + run, addr - run);
		run = addr + PAGE_SIZE;
		if (memcmp(to + addr, from + addr, PAGE_SIZE) == 0)
			continue;
		VramLockedWriteOffset(addr);
		memcpy(to + addr, from + addr, PAGE_SIZE);
	}
	if (VRAM_SIZE > run)
		memcpy(to + run, from + run, VRAM_SIZE - run);
}

bool VramLockedWrite(u8* address)
{
	const u32 offset = _vmem_get_vram_offset(address);
	if (offset == (u32)-1)
		return false;
	return VramLockedWriteOffset(offset);
}

/* unlocks mem, also frees the handle */
void vramlock_unlock(vram_block* block)
{
	vramlock_list_remove(block);
	block->userdata = vramlock_free_blocks;
	vramlock_free_blocks = block;
}
