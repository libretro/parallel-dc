/* The G2 bus's DMA channels to the expansion devices (aica_if.c): a start
 * written to each channel moves the channel's own transfer, in the
 * direction its DIR says, leaves its registers where a finished transfer
 * leaves them, and raises its own interrupt; a channel's address registers
 * keep the bits they have. Nothing of another channel is touched.
 *
 *    g2dma_test_run.sh builds it with aica_if.c and runs it
 *
 * The rest of the machine is stubbed: the bus's register table, the
 * interrupts, the scheduler and the transfer itself, which only records
 * what it was asked to do.
 */
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/aica_mem.h"

RegisterStruct sb_regs[SB_REGS];
u32 SB_ISTNRM;
bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{ (void)src; (void)src_size; (void)dest; (void)total_size; return true; }
bool ra_unserialize(void *dest, unsigned int size, void **src, unsigned int *total_size)
{ (void)dest; (void)size; (void)src; (void)total_size; return true; }
static RegWriteAddrFP *writer[SB_REGS];

void sb_rio_register(u32 reg_addr, RegIO flags, RegReadAddrFP* rf, RegWriteAddrFP* wf)
{
	(void)flags;
	(void)rf;
	writer[(reg_addr - SB_BASE) >> 2] = wf;
}

static int raised[64], raised_count;
void asic_RaiseInterrupt(HollyInterruptID inter)
{
	raised[raised_count++ & 63] = (int)inter;
}

static u32 dma_dst, dma_src, dma_len, dma_count;
void WriteMemBlock_nommu_dma(u32 dst, u32 src, u32 size)
{
	dma_dst = dst;
	dma_src = src;
	dma_len = size;
	dma_count++;
}

int sh4_sched_register(int tag, sh4_sched_callback* ssc) { (void)tag; (void)ssc; return 0; }
void sh4_sched_request(int id, int cycles) { (void)id; (void)cycles; }
bool sh4_sched_is_scheduled(int id) { (void)id; return false; }
u32 libAICA_ReadReg(u32 addr, u32 size) { (void)addr; (void)size; return 0; }
void libAICA_WriteReg(u32 addr, u32 data, u32 size) { (void)addr; (void)data; (void)size; }
void aicaarm_enable(bool enabled) { (void)enabled; }
#if FEAT_SHREC != DYNAREC_NONE
void bm_Periodical_1s(void) { }
#endif
/* (no log) */
retro_log_printf_t log_cb;

#define REG(a) sb_regs[((a) - SB_BASE) >> 2].data32

typedef struct channel
{
	u32 en, st, star, stag, len, dir;
	int done;
	const char *name;
} channel;

static const channel channels[] = {
	{ SB_E1EN_addr, SB_E1ST_addr, SB_E1STAR_addr, SB_E1STAG_addr, SB_E1LEN_addr, SB_E1DIR_addr, holly_EXT_DMA1, "EXT1" },
	{ SB_E2EN_addr, SB_E2ST_addr, SB_E2STAR_addr, SB_E2STAG_addr, SB_E2LEN_addr, SB_E2DIR_addr, holly_EXT_DMA2, "EXT2" },
	{ SB_DDEN_addr, SB_DDST_addr, SB_DDSTAR_addr, SB_DDSTAG_addr, SB_DDLEN_addr, SB_DDDIR_addr, holly_DEV_DMA, "DDEV" },
};
#define CHANNELS (sizeof(channels) / sizeof(channels[0]))

static int failures;
static void expect(int ok, const char *name, const char *what)
{
	if (!ok)
	{
		printf("FAIL %s: %s\n", name, what);
		failures++;
	}
}

static void write_reg(u32 addr, u32 data)
{
	RegWriteAddrFP *w = writer[(addr - SB_BASE) >> 2];
	if (w)
		w(addr, data);
	else
		REG(addr) = data;
}

/* The registers of every channel but @skip, as they are */
static u32 others[CHANNELS][6];
static void keep_others(unsigned skip)
{
	unsigned i;
	for (i = 0; i < CHANNELS; i++)
	{
		if (i == skip)
			continue;
		others[i][0] = REG(channels[i].en);
		others[i][1] = REG(channels[i].st);
		others[i][2] = REG(channels[i].star);
		others[i][3] = REG(channels[i].stag);
		others[i][4] = REG(channels[i].len);
		others[i][5] = REG(channels[i].dir);
	}
}
static int others_kept(unsigned skip)
{
	unsigned i;
	for (i = 0; i < CHANNELS; i++)
		if (i != skip && (others[i][0] != REG(channels[i].en) || others[i][1] != REG(channels[i].st)
				|| others[i][2] != REG(channels[i].star) || others[i][3] != REG(channels[i].stag)
				|| others[i][4] != REG(channels[i].len) || others[i][5] != REG(channels[i].dir)))
			return 0;
	return 1;
}

int main(void)
{
	unsigned i, k;

	aica_sb_Init();
	for (i = 0; i < CHANNELS; i++)
	{
		const channel *c = &channels[i];
		expect(writer[(c->st - SB_BASE) >> 2] != NULL, c->name, "its start is registered");
		expect(writer[(c->star - SB_BASE) >> 2] != NULL, c->name, "its STAR is registered");
		expect(writer[(c->stag - SB_BASE) >> 2] != NULL, c->name, "its STAG is registered");
	}
	/* every channel set up, so that one moved alone shows */
	for (i = 0; i < CHANNELS; i++)
	{
		REG(channels[i].en) = 1;
		REG(channels[i].star) = 0x0C000000 + i * 0x100000;
		REG(channels[i].stag) = 0x01000000 + i * 0x10000;
		REG(channels[i].len) = 0x40 * (i + 1);
		REG(channels[i].dir) = 0;
	}

	for (k = 0; k < 4; k++)
		for (i = 0; i < CHANNELS; i++)
		{
			const channel *c = &channels[i];
			const u32 dir = k & 1, last = (k & 2) ? 0x80000000u : 0;
			const u32 star = 0x0C000000 + i * 0x100000 + k * 0x1000;
			const u32 stag = 0x01000000 + i * 0x10000 + k * 0x100;
			const u32 len = 0x40 * (i + 1) + k * 0x20;

			REG(c->en) = 1;
			write_reg(c->star, star | 0x1F);	/* (the low bits are not kept) */
			write_reg(c->stag, stag | 0x1F);
			expect(REG(c->star) == star && REG(c->stag) == stag, c->name, "STAR and STAG keep their bits");
			REG(c->len) = len | last;
			REG(c->dir) = dir;
			keep_others(i);
			raised_count = 0;
			dma_count = 0;

			/* a start with bit 0 clear is nothing */
			write_reg(c->st, 0);
			expect(dma_count == 0 && raised_count == 0, c->name, "a start of 0 starts nothing");

			write_reg(c->st, 1);
			expect(dma_count == 1, c->name, "the start moves one transfer");
			expect(dma_len == len, c->name, "of its own length");
			expect(dir ? dma_dst == star && dma_src == stag : dma_dst == stag && dma_src == star,
					c->name, "from and to its own addresses, the way DIR says");
			expect(REG(c->star) == star + len && REG(c->stag) == stag + len, c->name, "its addresses move on by the length");
			expect(REG(c->len) == 0 && REG(c->st) == 0, c->name, "its length and start are cleared");
			expect(REG(c->en) == (last ? 0u : 1u), c->name, "it stays enabled unless the length said last");
			expect(raised_count == 1 && raised[0] == c->done, c->name, "its own interrupt");
			expect(others_kept(i), c->name, "nothing of the other channels changes");

			/* with the channel not enabled, a start is nothing */
			REG(c->en) = 0;
			dma_count = 0;
			raised_count = 0;
			write_reg(c->st, 1);
			expect(dma_count == 0 && raised_count == 0, c->name, "a disabled channel does not start");
		}

	if (failures)
	{
		printf("g2dma_test: %d failures\n", failures);
		return 1;
	}
	printf("g2dma_test: the expansion DMA channels PASS\n");
	return 0;
}
