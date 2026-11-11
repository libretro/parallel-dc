/* What rend/rtt_watch.cpp asks of the renderer that has a picture rendered
 * to a texture, with the renderer and the memory map stood in for:
 *
 *  - video memory written whole by a DMA (rtt_watch_overwrite()) over a
 *    picture: never fetched off the card, its pages no longer watched;
 *  - written in part: fetched, once, and what the DMA does not write is
 *    the picture's;
 *  - a picture that settles: its copy begun (prefetch) the frame before it
 *    is read, and read once;
 *  - several fetched at once (rtt_watch_flush()): their copies begun in
 *    one batch, before the first read;
 *  - more pictures than slots: the one that has waited longest goes to
 *    video memory, not the one in the first slot.
 *
 * To run: tools/rend/rtt_watch_test.sh */
#include "rend/rtt_watch.h"
#include "hw/mem/_vmem.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/modules/mmu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* what rtt_watch.cpp links against, stood in for */
unsigned VRAM_SIZE = 8 * 1024 * 1024;
VArray2 vram;
settings_t settings;
bool mmu_strict;
RegisterStruct CCN[SH4_CCN_REGS];

static std::vector<bool> watched;

void _vmem_watch_vram(u32 addr, u32 size)
{
	for (u32 p = addr / PAGE_SIZE; p <= (addr + size - 1) / PAGE_SIZE; p++)
		watched[p] = true;
}
void _vmem_unwatch_vram(u32 addr, u32 size)
{
	for (u32 p = addr / PAGE_SIZE; p <= (addr + size - 1) / PAGE_SIZE; p++)
		watched[p] = false;
}
void _vmem_open_watched_vram(u32, u32) {}
bool _vmem_vram_page_watched(u32 page) { return watched[page]; }
u32 _vmem_get_vram_offset(void *) { return (u32)-1; }
void VramLockedWriteRange(u32, u32) {}
void VramRendered(u32, u32, const BaseTextureCacheData *) {}

/* the renderer: every read and every batch of copies begun, in order */
struct Event { char what; u32 serial; unsigned count; };
static std::vector<Event> events;
static u8 rgba[256 * 256 * 4];

static void test_read(const RttWatch *watch, u8 *, RttWatchTake take)
{
	events.push_back({ 'r', watch->serial, 1 });
	take(watch, rgba, (size_t)watch->w * 4);
}
static void test_release(uintptr_t) {}
static void test_prefetch(const RttWatch *const *watches, unsigned count)
{
	for (unsigned i = 0; i < count; i++)
		events.push_back({ 'p', watches[i]->serial, count });
}
static const RttWatchBackend backend = { test_read, test_release, test_prefetch };

static int failures;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (line %d)\n", what, __LINE__); failures++; } } while (0)

/* a 64 by 64 RGB565 picture at @addr, lines 128 bytes apart; its serial,
 * which counts the pictures added (rtt_watch.h) */
static u32 added;

static u32 add(u32 addr)
{
	RttWatch w = {};
	w.addr = addr;
	w.w = 64;
	w.h = 64;
	w.stride = 128;
	w.bytes = 128 * 64;
	w.packmode = 1;
	w.tex = addr;
	w.scale = 1;
	rtt_watch_add(&w, &backend);
	return ++added;
}

static unsigned count(char what, u32 serial = 0)
{
	unsigned n = 0;
	for (const Event& e : events)
		if (e.what == what && (serial == 0 || e.serial == serial))
			n++;
	return n;
}

int main()
{
	vram.data = (u8 *)calloc(1, VRAM_SIZE);
	vram.size = VRAM_SIZE;
	watched.assign(VRAM_SIZE / PAGE_SIZE, false);
	for (size_t i = 0; i < sizeof(rgba); i += 4)
	{
		rgba[i] = 0xFF;      /* yellow: 0xFFE0 in RGB565 */
		rgba[i + 1] = 0xFF;
		rgba[i + 2] = 0;
		rgba[i + 3] = 0xFF;
	}

	/* written whole: not fetched */
	add(0x100000);
	events.clear();
	rtt_watch_overwrite(0x100000, 128 * 64);
	CHECK(count('r') == 0, "a picture a DMA writes whole is not fetched");
	CHECK(!watched[0x100000 / PAGE_SIZE], "its pages are not watched any more");
	rtt_watch_flush();
	CHECK(count('r') == 0, "nor later: it is gone");

	/* written in part: fetched once, the rest of it the picture's */
	add(0x200000);
	events.clear();
	rtt_watch_overwrite(0x200000, 128 * 32);
	CHECK(count('r') == 1, "a picture a DMA writes in part is fetched, once");
	CHECK(*(u16 *)&vram[0x200000 + 128 * 40] == 0xFFE0, "the lines the DMA does not write are the picture's");
	CHECK(!watched[0x200000 / PAGE_SIZE], "its pages are not watched any more");
	rtt_watch_flush();
	CHECK(count('r') == 1, "nor fetched again");

	/* a picture that settles: begun the frame before, read once */
	const u32 settles = add(0x300000);
	events.clear();
	rtt_watch_frame();
	CHECK(events.empty(), "nothing is done for a picture a frame old");
	rtt_watch_frame();
	CHECK(count('p', settles) == 1 && count('r') == 0, "two frames old: its copy is begun, not read");
	rtt_watch_frame();
	CHECK(count('r', settles) == 1, "three frames old: it is read");
	CHECK(events.size() >= 2 && events[0].what == 'p', "after its copy was begun");

	/* several at once: one batch begun before the first read */
	const u32 a = add(0x400000), b = add(0x404000), c = add(0x408000);
	events.clear();
	rtt_watch_flush();
	CHECK(count('r') == 3, "a flush reads all three");
	CHECK(!events.empty() && events[0].what == 'p' && events[0].count == 3,
			"their copies begun together, before the first read");
	CHECK(count('p', a) == 1 && count('p', b) == 1 && count('p', c) == 1, "each of the three begun");

	/* more than there are slots: the oldest goes */
	for (int i = 0; i < 16; i++)
		add(0x500000 + i * 0x4000);
	rtt_watch_overwrite(0x500000, 128 * 64);    /* the first slot's: free again */
	rtt_watch_frame();                          /* the other fifteen a frame old */
	const u32 young = add(0x700000);            /* into the first slot */
	events.clear();
	add(0x740000);                              /* one too many */
	CHECK(count('r', young) == 0, "the one in the first slot, young, stays watched");
	CHECK(count('r') >= 1, "one that waited longer goes to video memory");

	rtt_watch_term();
	free(vram.data);
	if (failures != 0)
	{
		printf("rtt_watch_test: %d failed\n", failures);
		return 1;
	}
	printf("rtt_watch_test: passed\n");
	return 0;
}
