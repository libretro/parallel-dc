/* Test for the NAOMI M3 link board's sharing with its network thread. See
 * run.sh.
 *
 * Builds the real core/hw/naomi/naomi_m3comm.c against a stand-in for the
 * network: packets that are one value repeated, so a packet seen half
 * written shows. One thread plays the game, writing its slot and reading
 * what came in through the board's registers and DMA; the board's own
 * thread sends and receives. ThreadSanitizer reports any memory the two
 * touch together. */
#include <stdio.h>
#include <string.h>
#include <retro_timers.h>
#include <retro_miscellaneous.h>

#include "types.h"
#include "hw/naomi/naomi_m3comm.h"
#include "network/naomi_network.h"
#include "hw/naomi/naomi_regs.h"
#include "hw/holly/sb.h"
#include "hw/mem/_vmem.h"

static int failures;

#define CHECK(cond) do { \
	if (!(cond)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		failures++; \
	} } while (0)

#define SLOT_SIZE   0x180
#define SLOT_COUNT  3
#define PACKET_SIZE (SLOT_SIZE * SLOT_COUNT)
#define ROUNDS      3000

/* What the board needs from the rest of the core */

RegisterStruct sb_regs[SB_REGS];

static u8 guest_ram[0x10000];	/* the game's memory, for DMA */

u8 DYNACALL _vmem_ReadMem8(u32 address) { return guest_ram[address & 0xFFFF]; }
void DYNACALL _vmem_WriteMem8(u32 address, u8 data) { guest_ram[address & 0xFFFF] = data; }

/* (no log) */
retro_log_printf_t log_cb;

/* The board starts its network thread only for a link that has a server:
 * this one is its own. */
settings_t settings;

/* (the board's part of a save state is not what this tests) */
bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total) { return true; }
bool ra_unserialize(void *dest, unsigned int size, void **src, unsigned int *total) { return true; }

/* The network: a ring that hands back packets of one value */

static retro_atomic_int_t net_sent, net_received, net_torn;
static u8 net_last_rx[PACKET_SIZE];	/* network thread only */

void naomi_network_create(naomi_network *net)
{
	memset(net, 0, sizeof(*net));
}

bool naomi_network_init(naomi_network *net) { return true; }

bool naomi_network_start(naomi_network *net)
{
	net->slot_count = SLOT_COUNT;
	net->slot_id = 1;
	net->packet_number = 0;
	net->got_token = false;
	return true;
}

void naomi_network_pipe_slaves(naomi_network *net) {}

bool naomi_network_receive(naomi_network *net, u8 *data, u32 size)
{
	if (net->got_token || size != PACKET_SIZE)
		return false;
	net->packet_number++;
	memset(data, (u8)net->packet_number, size);
	memcpy(net_last_rx, data, size);
	net->got_token = true;
	retro_atomic_fetch_add_int(&net_received, 1);
	return true;
}

void naomi_network_send(naomi_network *net, const u8 *data, u32 size)
{
	u32 i;
	if (!net->got_token)
		return;
	/* The board's own slot, whole as the game wrote it... */
	for (i = 1; i < SLOT_SIZE; i++)
		if (data[i] != data[0])
		{
			retro_atomic_fetch_add_int(&net_torn, 1);
			break;
		}
	/* ...followed by what came in from the others. */
	if (size != PACKET_SIZE || memcmp(data + SLOT_SIZE, net_last_rx, PACKET_SIZE - SLOT_SIZE))
		retro_atomic_fetch_add_int(&net_torn, 1);
	net->packet_number++;
	net->got_token = false;
	retro_atomic_fetch_add_int(&net_sent, 1);
}

void naomi_network_shutdown(naomi_network *net) { retro_atomic_store_release_int(&net->network_stopping, 1); }
bool naomi_network_arm_wake(naomi_network *net) { retro_atomic_store_release_int(&net->network_stopping, 0); return true; }
void naomi_network_wait_for_data(naomi_network *net) {}
void naomi_network_wait_stop(naomi_network *net, int64_t usec) {}
void naomi_network_close_sockets(naomi_network *net) {}
void naomi_network_terminate(naomi_network *net) { naomi_network_shutdown(net); }

/* The game */

static void board_write(u32 addr, u32 data) { m3comm_write(addr, data, 2); }
static u32 board_read(u32 addr) { return m3comm_read(addr, 2); }

/* DMA @len bytes between guest memory and the board's RAM at @offset. */
static void board_dma(u32 offset, u32 len, bool to_board)
{
	board_write(NAOMI_COMM2_OFFSET_addr, offset);
	SB_GDSTAR = 0x1000;
	SB_GDLEN = len;
	SB_GDDIR = to_board ? 0 : 1;
	CHECK(m3comm_dma_start(0, 0));
}

int main(void)
{
	int waited = 0;
	long whole = 0;
	int round;

	settings.network.ActAsServer = true;

	/* The slot size goes into the M68K's RAM, then the M68K is reset,
	 * which starts the network. */
	board_write(NAOMI_COMM2_CTRL_addr, 1);
	board_write(NAOMI_COMM2_OFFSET_addr, 0x204);
	board_write(NAOMI_COMM2_DATA_addr, SLOT_SIZE);
	board_write(NAOMI_COMM2_CTRL_addr, 1 << 5);

	/* The board reports the link up once the network thread has it. */
	while (board_read(NAOMI_COMM2_STATUS0_addr) != 0xff01 && waited++ < 5000)
		retro_sleep(1);
	CHECK(board_read(NAOMI_COMM2_STATUS0_addr) == 0xff01);
	CHECK(board_read(NAOMI_COMM2_STATUS1_addr) == ((SLOT_COUNT << 8) | 1));

	for (round = 0; round < ROUNDS; round++)
	{
		/* Write this board's slot: one value all through. */
		memset(&guest_ram[0x1000], (u8)round, SLOT_SIZE);
		board_dma(0x100, SLOT_SIZE, true);

		/* Read what has come in. Whatever packet it is, it is a whole
		 * one. */
		board_dma(0x100 + SLOT_SIZE, PACKET_SIZE, false);
		{
			bool same = true;
			u32 i;
			for (i = 1; i < PACKET_SIZE; i++)
				if (guest_ram[0x1000 + i] != guest_ram[0x1000])
					same = false;
			CHECK(same);
			if (same && guest_ram[0x1000] != 0)
				whole++;
		}

		/* The packet number, through the data port. */
		board_write(NAOMI_COMM2_OFFSET_addr, 6);
		(void)board_read(NAOMI_COMM2_DATA_addr);

		if ((round & 63) == 0)
			retro_sleep(1);
	}

	m3comm_term();

	CHECK(retro_atomic_load_acquire_int(&net_torn) == 0);
	CHECK(retro_atomic_load_acquire_int(&net_received) > 0);
	CHECK(retro_atomic_load_acquire_int(&net_sent) > 0);
	CHECK(whole > 0);
	printf("%s m3comm (%d packets in, %d out)\n", failures ? "FAIL" : "ok  ",
			retro_atomic_load_acquire_int(&net_received), retro_atomic_load_acquire_int(&net_sent));
	return failures ? 1 : 0;
}
