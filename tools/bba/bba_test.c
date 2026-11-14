/* The broadband adapter (core/hw/bba: the GAPS PCI bridge and its RTL8139C)
 * by itself, as the SH4 sees it through bba_ReadMem() and bba_WriteMem(),
 * with the network and the rest of the machine stubbed:
 *
 * - the bridge's name, and the MAC address the chip has after a reset;
 * - a frame the game puts in the adapter's memory and hands to a transmit
 *   descriptor reaches the network as it was, and the descriptor and the
 *   interrupt status say it was sent;
 * - a frame from the network is written to the receive ring with its
 *   header, the interrupt status says so, and the SH4's interrupt is
 *   raised when the bridge lets it through;
 * - frames the receive filter does not accept are not written;
 * - the state written (since version 24) and read back gives the same
 *   state, written again byte for byte.
 *
 * Built as C, or as C++ against a tree whose adapter is C++. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/bba/bba.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include "network/picoppp.h"
#include <features/features_cpu.h>

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

/* The machine around the adapter */
sched_list sch_list[SH4_SCHED_SLOTS];
static int sched_ids;
static u64 now64 = 1000;
static int irq_raised;

int sh4_sched_register(int tag, sh4_sched_callback *ssc)
{
	sch_list[sched_ids].cb = ssc;
	sch_list[sched_ids].tag = tag;
	sch_list[sched_ids].start = -1;
	sch_list[sched_ids].end = -1;
	return sched_ids++;
}
void sh4_sched_request(int id, int cycles)
{
	sch_list[id].start = cycles < 0 ? -1 : 0;
	sch_list[id].end = cycles < 0 ? -1 : cycles;
}
bool sh4_sched_is_scheduled(int id) { return sch_list[id].end != -1; }
u64 sh4_sched_now64(void) { return now64; }
void asic_RaiseInterrupt(HollyInterruptID inter) { (void)inter; irq_raised = 1; }
void asic_CancelInterrupt(HollyInterruptID inter) { (void)inter; irq_raised = 0; }
retro_time_t cpu_features_get_time_usec(void) { return 12345; }
uint64_t cpu_features_get(void) { return 0; }
/* (no log) */
retro_log_printf_t log_cb;

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(*dest, src, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}
bool ra_unserialize(void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(src, *dest, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}

/* The network */
static int pico_started, pico_stopped;
static u8 sent[2048];
static u32 sent_size;
static int sent_count;
static u8 incoming[NET_ETH_FRAME_MAX];
static u32 incoming_size;

bool start_pico(void) { pico_started++; return true; }
void stop_pico(void) { pico_stopped++; }
void pico_receive_eth_frame(const u8 *frame, u32 size)
{
	if (size <= sizeof(sent))
		memcpy(sent, frame, size);
	sent_size = size;
	sent_count++;
}
u32 net_eth_take(u8 *frame)
{
	u32 size = incoming_size;
	if (size != 0)
		memcpy(frame, incoming, size);
	incoming_size = 0;
	return size;
}

/* The adapter as the SH4 sees it */
#define RTL(r) (0x001700 + (r))
#define RAM(a) (0x840000 + (a))
#define GAPS_INT_ENABLE 0x001414
#define GAPS_RESET 0x001418

static void ram_write(u32 at, const u8 *src, u32 size)
{
	u32 i;
	for (i = 0; i < size; i++)
		bba_WriteMem(RAM(at + i), src[i], 1);
}
static u32 ram_read(u32 at, u32 sz) { return bba_ReadMem(RAM(at), sz); }

/* Runs the adapter's event that gives it what came from the network */
static void deliver(void)
{
	int i;
	for (i = 0; i < sched_ids; i++)
		if (sch_list[i].cb != NULL && sch_list[i].end != -1)
		{
			now64 += 200000;
			sch_list[i].cb(sch_list[i].tag, 0, 0);
		}
}

static u8 state1[0x20000], state2[0x20000];

int main(void)
{
	u8 frame[60];
	u32 i, header, size1, size2;
	void *p;
	unsigned int total;

	bba_Init();

	/* The bridge, and the chip's address after the bridge resets it */
	CHECK(bba_ReadMem(0x001400, 4) == 0x53504147);	/* "GAPS" */
	CHECK(bba_ReadMem(0x00141c, 4) == 0x41474553);	/* "SEGA" */
	bba_WriteMem(GAPS_RESET, 1, 1);
	CHECK(bba_ReadMem(RTL(0), 1) == 0x0c);
	CHECK(bba_ReadMem(RTL(1), 1) == 0x0a);
	CHECK(bba_ReadMem(RTL(2), 1) == 0x0f);
	CHECK(bba_ReadMem(RTL(3), 1) == 0x0e);
	CHECK(bba_ReadMem(RTL(4), 1) == 0x00);
	CHECK(bba_ReadMem(RTL(5), 1) == 0x00);

	/* Reset, the receive ring at 0x2000 (8 KiB), interrupts for both ways */
	bba_WriteMem(RTL(0x37), 0x10, 1);
	CHECK((bba_ReadMem(RTL(0x37), 1) & 0x10) == 0);
	bba_WriteMem(RTL(0x37), 0x0c, 1);
	bba_WriteMem(RTL(0x30), RAM(0x2000), 4);
	bba_WriteMem(RTL(0x44), 0x0a, 4);	/* broadcast, our own address */
	bba_WriteMem(RTL(0x3c), 0x05, 2);
	bba_WriteMem(GAPS_INT_ENABLE, 1, 1);

	/* A frame sent */
	for (i = 0; i < sizeof(frame); i++)
		frame[i] = (u8)(i * 7 + 1);
	ram_write(0x100, frame, sizeof(frame));
	bba_WriteMem(RTL(0x20), RAM(0x100), 4);
	bba_WriteMem(RTL(0x10), sizeof(frame), 4);
	CHECK(pico_started == 1);
	CHECK(sent_count == 1);
	CHECK(sent_size == sizeof(frame));
	CHECK(memcmp(sent, frame, sizeof(frame)) == 0);
	CHECK((bba_ReadMem(RTL(0x10), 4) & 0xa000) == 0xa000);	/* OK, the host's */
	CHECK((bba_ReadMem(RTL(0x3e), 2) & 0x04) != 0);
	CHECK(irq_raised);
	bba_WriteMem(RTL(0x3e), 0x04, 2);
	CHECK((bba_ReadMem(RTL(0x3e), 2) & 0x04) == 0);
	CHECK(!irq_raised);

	/* A broadcast frame received */
	memset(incoming, 0xff, 6);
	for (i = 6; i < 64; i++)
		incoming[i] = (u8)(i ^ 0x5a);
	incoming_size = 64;
	deliver();
	header = ram_read(0x2000, 4);
	CHECK((header & 0x0001) != 0);			/* received OK */
	CHECK((header & 0x2000) != 0);			/* broadcast */
	CHECK((header >> 16) == 64 + 4);		/* with the CRC */
	for (i = 0; i < 64; i++)
		CHECK(ram_read(0x2004 + i, 1) == incoming[i]);
	CHECK(bba_ReadMem(RTL(0x3a), 2) == ((64 + 4 + 4 + 3) & ~3u));
	CHECK((bba_ReadMem(RTL(0x3e), 2) & 0x01) != 0);
	CHECK(irq_raised);
	bba_WriteMem(RTL(0x3e), 0x01, 2);

	/* A frame for another address is not */
	incoming[0] = 0x02;
	incoming_size = 64;
	deliver();
	CHECK(bba_ReadMem(RTL(0x3a), 2) == ((64 + 4 + 4 + 3) & ~3u));
	CHECK((bba_ReadMem(RTL(0x3e), 2) & 0x01) == 0);

	/* The state, written, read back and written again */
	p = state1; total = 0;
	bba_serialize_v24(&p, &total);
	size1 = total;
	bba_WriteMem(RTL(0x37), 0x10, 1);
	bba_WriteMem(RAM(0x2000), 0, 4);
	p = state1; total = 0;
	CHECK(bba_unserialize_v24(&p, &total));
	CHECK(total == size1);
	p = state2; total = 0;
	bba_serialize_v24(&p, &total);
	size2 = total;
	CHECK(size1 == size2);
	CHECK(memcmp(state1, state2, size1) == 0);
	CHECK(ram_read(0x2000, 4) == header);

	bba_Term();
	CHECK(pico_stopped >= 1);

	if (failures)
	{
		fprintf(stderr, "bba_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("bba_test: OK (state %u bytes)\n", size1);
	return 0;
}
