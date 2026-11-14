#include "types.h"
#include <string.h>

#include "maple_if.h"

#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/maple/maple_helper.h"
#include "hw/pvr/spg.h"
#include "hw/sh4/sh4_core.h"

enum MaplePattern
{
	MP_Start,
	MP_SDCKBOccupy = 2,
	MP_Reset,
	MP_SDCKBOccupyCancel,
	MP_NOP = 7
};

maple_device* MapleDevices[4][6];

int maple_sched;

/*
	Maple host controller
	Direct processing, async interrupt handling
	Device code is on maple_devs.cpp/h, config&management is on maple_cfg.cpp/h

	This code is missing many of the hardware details, like proper trigger handling,
	DMA continuation on suspect, etc ...
*/

static void maple_DoDma(void);
static void maple_handle_reconnect(void);
static bool IsOnSh4Ram(u32 addr);

/* really hackish
 * misses delay , and stop/start implementation
 * ddt/etc are just hacked for wince to work
 * now with proper maple delayed DMA maybe its time to look into it ? */
bool maple_ddt_pending_reset;

/* What the devices have answered in the transfer under way.
 *
 * An answer takes time to come in over the bus, and the game is told with an
 * interrupt when the whole transfer is over. Until then its memory has to be
 * as it was: a game that looks before the interrupt must not find the answer
 * there already. So the answers wait here, each as the address it goes to,
 * its length in bytes and its words, and are put in memory by
 * maple_dma_done(). An address of 0 is an answer with nowhere to go: the
 * overrun interrupt, raised then too.
 *
 * In the save state, from V18. */
u32 maple_out[MAPLE_OUT_WORDS];
u32 maple_out_used;

static void maple_out_write(u32 dest, const u32 *data, u32 bytes)
{
	u32 *p;

	if (dest == 0)
	{
		asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
		return;
	}
	/* An answer goes to RAM, and all of it there. The address and the length
	 * can be a save state's, and the game can ask for an answer at the very
	 * end of RAM: what would be written outside it is left out. */
	if (!IsOnSh4Ram(dest) || bytes > RAM_SIZE - (dest & RAM_MASK))
		return;
	p = (u32 *)GetMemPtr(dest, bytes);
	if (p != NULL)
		memcpy(p, data, bytes);
}

static void maple_out_put(u32 dest, const u32 *data, u32 bytes)
{
	const u32 words = (bytes + 3) / 4;

	if (maple_out_used + 2 + words > MAPLE_OUT_WORDS)
	{
		/* more answers than there is room to keep: this one arrives at once */
		maple_out_write(dest, data, bytes);
		return;
	}
	maple_out[maple_out_used++] = dest;
	maple_out[maple_out_used++] = bytes;
	memcpy(&maple_out[maple_out_used], data, bytes);
	maple_out_used += words;
}

/* Room in maple_out for the longest answer, lent to the device to write
 * there: NULL if there is not that much left. maple_out_commit() keeps
 * what it wrote. */
static u32 *maple_out_lend(void)
{
	if (maple_out_used + 2 + MAPLE_FRAME_MAX_WORDS > MAPLE_OUT_WORDS)
		return NULL;
	return &maple_out[maple_out_used + 2];
}

static void maple_out_commit(u32 dest, u32 bytes)
{
	maple_out[maple_out_used++] = dest;
	maple_out[maple_out_used++] = bytes;
	maple_out_used += (bytes + 3) / 4;
}

/* The transfer is over: the answers go where the game asked, and it is told */
void maple_dma_done(void)
{
	u32 at = 0;

	while (at + 2 <= maple_out_used)
	{
		const u32 dest = maple_out[at], bytes = maple_out[at + 1];
		/* (a length from a save state can be anything: no more than is kept here) */
		if (bytes > (maple_out_used - at - 2) * 4)
			break;
		maple_out_write(dest, &maple_out[at + 2], bytes);
		at += 2 + (bytes + 3) / 4;
	}
	maple_out_used = 0;
	SB_MDST = 0;
	asic_RaiseInterrupt(holly_MAPLE_DMA);
}

void maple_vblank(void)
{
	if (SB_MDEN & 1)
	{
		if (SB_MDTSEL & 1)
		{
			if (maple_ddt_pending_reset)
			{
				DEBUG_LOG(MAPLE, "DDT vblank ; reset pending\n");
			}
			else
			{
				DEBUG_LOG(MAPLE, "DDT vblank\n");
				SB_MDST = 1;
				maple_DoDma();
				/* if trigger reset is manual, mark it as pending */
				if ((SB_MSYS >> 12) & 1)
					maple_ddt_pending_reset = true;
			}
		}
		else
		{
			maple_ddt_pending_reset = false;
		}
	}
	maple_handle_reconnect();
}

static void maple_SB_MSHTCL_Write(u32 addr, u32 data)
{
	if (data&1)
		maple_ddt_pending_reset=false;
}

static void maple_SB_MDST_Write(u32 addr, u32 data)
{
	if (data & 0x1)
	{
		if (SB_MDEN &1)
		{
			SB_MDST=1;
			/* A transfer the game starts where a run ends is made at the
			 * start of the next run, with its pad (hw/pvr/spg.cpp). Only
			 * one with no other under way: a transfer started with nothing
			 * scheduled to end it is how a state says one is waiting. */
			if (!sh4_sched_is_scheduled(maple_sched) && !spg_gun_pending() && spg_maple_defer())
				return;
			maple_DoDma();
		}
	}
}

/* dc_run(): a run starts. A transfer the last one left for it is made now,
 * where the machine stands, before it runs on. */
void maple_start_deferred(void)
{
	if ((SB_MDEN & 1) && (SB_MDST & 1) && !sh4_sched_is_scheduled(maple_sched) && !spg_gun_pending())
		maple_DoDma();
}

static void maple_SB_MDEN_Write(u32 addr, u32 data)
{
	SB_MDEN=data&1;
}

#ifdef STRICT_MODE
static bool check_mdapro(u32 addr)
{
	u32 area = (addr >> 26) & 7;
	u32 bottom = ((((SB_MDAPRO >> 8) & 0x7f) << 20) | 0x08000000);
	u32 top = (((SB_MDAPRO & 0x7f) << 20) | 0x080fffe0);

	if (area != 3 || addr < bottom || addr > top)
	{
		INFO_LOG(MAPLE, "MAPLE ERROR : Invalid address: %08x. SB_MDAPRO: %x %x\n", addr, (SB_MDAPRO >> 8) & 0x7f, SB_MDAPRO & 0x7f);
		return false;
	}
	return true;
}

static void maple_SB_MDSTAR_Write(u32 addr, u32 data)
{
	SB_MDSTAR = data & 0x1fffffe0;
	if (!check_mdapro(SB_MDSTAR))
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
}
#endif

static bool IsOnSh4Ram(u32 addr)
{
	if (((addr>>26)&0x7)==3)
	{
		if ((((addr>>29) &0x7)!=7))
			return true;
	}

	return false;
}

static void maple_DoDma(void)
{
	u32 addr = SB_MDSTAR;
	u32 ram_end;
	const bool swap_msb = (SB_MMSEL == 0);
	u32 xfer_in = 0, xfer_out = 0;		/* bytes from the console, and from the devices */
	bool last   = false;
	bool occupy = false;

	verify(SB_MDEN &1)
	verify(SB_MDST &1)

	DEBUG_LOG(MAPLE, "Maple: DoMapleDma SB_MDSTAR=%x\n", SB_MDSTAR);
#ifdef STRICT_MODE
	if (!check_mdapro(addr))
	{
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
		SB_MDST = 0;
		return;
	}
#endif
	/* The table of frames is in RAM, and ends where RAM does at the latest.
	 * (It was read from wherever the game said, and one with no entry marked
	 * as its last went on through all of memory.) */
	if (!IsOnSh4Ram(addr))
	{
		INFO_LOG(MAPLE, "MAPLE ERROR : INVALID SB_MDSTAR value 0x%X\n", addr);
		SB_MDST = 0;
		return;
	}
	ram_end = (addr | RAM_MASK) + 1;
	while (last != true)
	{
		u32 header_1, header_2, plen, maple_op, bus;

		if (addr + 4 > ram_end)
		{
			INFO_LOG(MAPLE, "MAPLE ERROR : the table at 0x%X has no end\n", SB_MDSTAR);
			SB_MDST = 0;
			maple_out_used = 0;
			return;
		}
		header_1 = ReadMem32_nommu(addr);
		header_2 = ReadMem32_nommu(addr + 4) &0x1FFFFFE0;

		last = (header_1 >> 31) == 1;	/* is last transfer ? */
		plen = (header_1 & 0xFF )+1;	/* transfer length (32-bit unit) */
		/* Pattern selection: 0 - START, 2 - SDCKB occupy permission,
		 * 3 - RESET, 4 - SDCKB occupy cancel, 7 - NOP */
		maple_op=(header_1>>8)&7;
		/* The port the frame goes out on is the transfer's to say, not the frame's */
		bus = (header_1 >> 16) & 3;
		/* this is kinda wrong .. but meh
		 * really need to properly process the commands at some point */
		switch (maple_op)
		{
		case MP_Start:
		{
			u32 *p_data;
			u32 frame_header, command, reci, inlen, port;
#ifdef STRICT_MODE
			if (!check_mdapro(header_2) || !check_mdapro(addr + 8 + plen * sizeof(u32) - 1))
			{
				asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
				SB_MDST = 0;
				return;
			}
#else
			if (!IsOnSh4Ram(header_2))
			{
				/* nowhere for the answer to go: the overrun interrupt, when the transfer is over */
				INFO_LOG(MAPLE, "MAPLE ERROR : DESTINATION NOT ON SH4 RAM 0x%X\n", header_2);
				header_2 = 0;
			}
#endif

			/* (a frame that starts in RAM and ends past it is not one) */
			p_data = addr + 8 + plen * sizeof(u32) > ram_end ? NULL
					: (u32*) GetMemPtr(addr + 8,(plen)*sizeof(u32));
			if (p_data == NULL)
			{
				INFO_LOG(MAPLE, "MAPLE ERROR : INVALID SB_MDSTAR value 0x%X\n", addr);
				SB_MDST=0;
				maple_out_used = 0;
				return;
			}

			frame_header = swap_msb ? SWAP32(p_data[0]) : p_data[0];

			command = frame_header & 0xFF;			/* Command code */
			reci = (frame_header >> 8) & 0xFF;		/* Recipient address, 0-5 */
			/* (the sender's address, (p_data[0] >> 16) & 0xFF, is not used) */
			inlen = (frame_header >> 24) & 0xFF;	/* Number of additional words in frame */
			/* (no more than the table says the frame has: the device reads no further) */
			if (inlen >= plen)
				inlen = plen - 1;

			port=maple_GetPort(reci);
			/* A device with no slots for others hears every frame sent down
			 * its bus, whatever the frame is addressed to. (Asked only when
			 * nothing is in the slot addressed, which a device with slots
			 * may have empty too.) */
			if (port != 5 && MapleDevices[bus][port] == NULL && MapleDevices[bus][5] != NULL
					&& !maple_dev_has_slots(MapleDevices[bus][5]))
				port = 5;

			if (MapleDevices[bus][5] && MapleDevices[bus][port])
			{
				static u32 maple_out_buf[MAPLE_FRAME_MAX_WORDS];
				/* The answer is written where it is kept until the transfer
				 * is over, and copied only when there is no room left there. */
				u32 *out = maple_out_lend();
				u32 outlen, i;

				if (out == NULL)
					out = maple_out_buf;
				if (swap_msb)
				{
					/* (frames in RAM are read as they are, and copied only to be swapped) */
					static u32 maple_in_buf[MAPLE_FRAME_MAX_WORDS];
					maple_in_buf[0] = frame_header;
					/* every word of the frame: plen of them, the header included */
					for (i = 1; i < plen; i++)
						maple_in_buf[i] = SWAP32(p_data[i]);
					p_data = maple_in_buf;
				}
				outlen = maple_dev_dma(MapleDevices[bus][port], &p_data[0], inlen * 4 + 4, out);
				/* each frame also has its start, parity and stop on the wire */
				xfer_in += plen * 4 + 3;
				xfer_out += outlen + 3;
#ifdef STRICT_MODE
				if (!check_mdapro(header_2 + outlen - 1))
				{
					/* TODO: This isn't correct (with SB_MMSEL=1) since the interrupt
					 * should be raised before the memory is written to */
					asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
					SB_MDST = 0;
					maple_out_used = 0;
					return;
				}
#endif
				if (swap_msb)
					for (i = 0; i < outlen / 4; i++)
						out[i] = SWAP32(out[i]);
				if (out == maple_out_buf)
					maple_out_put(header_2, out, outlen);
				else
					maple_out_commit(header_2, outlen);
			}
			else
			{
				static const u32 nobody = 0xFFFFFFFF;
				if (port != 5 && command != 1)
					INFO_LOG(MAPLE, "MAPLE: Unknown device bus %d port %d cmd %d\n", bus, port, command);
				maple_out_put(header_2, &nobody, 4);
			}

			/* goto next command */
			addr += 2 * 4 + plen * 4;
		}
		break;

		case MP_SDCKBOccupy:
		{
			if (MapleDevices[bus][5])
			{
				/* (a later request, for a bus with no gun, does not undo one that has) */
				if (maple_dev_lightgun_pos(MapleDevices[bus][5]))
					occupy = true;
				xfer_in++;
			}

			addr += 1 * 4;
		}
		break;

		case MP_SDCKBOccupyCancel:
			/* (the transfer ends in its own time, and not at vblank out) */
			occupy = false;
			spg_gun_occupy_cancel();
			addr += 1 * 4;
			break;

		case MP_Reset:
			addr += 1 * 4;
			xfer_in++;
			break;

		case MP_NOP:
			addr += 1 * 4;
			break;

		default:
			INFO_LOG(MAPLE, "MAPLE: Unknown maple_op == %d length %d\n", maple_op, plen * 4);
			addr += 1 * 4;
		}
	}

	/* How long the transfer takes. The console sends at 2 Mbit/s; devices
	 * answer more slowly, 724 to 738 kbit/s as measured on the wire with a
	 * protocol analyser (the captures are in OrangeFox86's
	 * DreamcastControllerUsbPico, under measurements). These are upstream's
	 * figures, which it came to through Silent Scope, which reads its
	 * memory card at boot and fails if the answer comes back too soon. */
	if (!occupy)
	{
		u64 cycles = (u64)SH4_MAIN_CLOCK * xfer_in / (2000000 / 8)
			+ (u64)SH4_MAIN_CLOCK * xfer_out / (740000 / 8);
		if (cycles > (u64)SH4_MAIN_CLOCK)
			cycles = SH4_MAIN_CLOCK;
		sh4_sched_request(maple_sched, (int)cycles);
	}
}

static int maple_schd(int tag, int c, int j)
{
	if (SB_MDEN&1)
		maple_dma_done();
	else
	{
		INFO_LOG(MAPLE, "WARNING: MAPLE DMA ABORT\n");
		SB_MDST=0; /* I really wonder what this means, can the DMA be continued ? */
		maple_out_used = 0;
	}

	return 0;
}

void maple_SB_MDAPRO_Write(u32 addr, u32 data)
{
	if ((data >> 16) == 0x6155)
		SB_MDAPRO = data & 0x00007f7f;
}

/* Init registers :) */
void maple_Init(void)
{
	sb_rio_register(SB_MDST_addr,RIO_WF,0,&maple_SB_MDST_Write);
	sb_rio_register(SB_MDEN_addr,RIO_WF,0,&maple_SB_MDEN_Write);
	sb_rio_register(SB_MSHTCL_addr,RIO_WF,0,&maple_SB_MSHTCL_Write);
	sb_rio_register(SB_MDAPRO_addr, RIO_WO_FUNC, NULL, &maple_SB_MDAPRO_Write);
#ifdef STRICT_MODE
	sb_rio_register(SB_MDSTAR_addr, RIO_WF, 0, &maple_SB_MDSTAR_Write);
	sb_rio_register(SB_MDSTAR_addr, RIO_WF, NULL, &maple_SB_MDSTAR_Write);
#endif

	maple_sched=sh4_sched_register(0,&maple_schd);
}

void maple_Reset(bool hard)
{
	maple_ddt_pending_reset=false;
	maple_out_used = 0;
	/* (and the end of the transfer those answers were for is not to come) */
	if (sh4_sched_is_scheduled(maple_sched))
		sh4_sched_request(maple_sched, -1);
	SB_MDTSEL = 0x00000000;
	SB_MDEN   = 0x00000000;
	SB_MDST   = 0x00000000;
	SB_MSYS   = 0x3A980000;
	SB_MSHTCL = 0x00000000;
	SB_MDAPRO = 0x00007F00;
	SB_MMSEL  = 0x00000001;
	maple_jvs_firmware_reset();
}

void maple_Term(void)
{
	
}

void maple_FlushSaves()
{
	int bus, port;
	for (bus = 0; bus < 4; bus++)
		for (port = 0; port < 6; port++)
			if (MapleDevices[bus][port])
				maple_dev_flush_save(MapleDevices[bus][port]);
}

static u64 reconnect_time;

/* The settings have changed what is plugged in. What they no longer have is
 * unplugged, and a tenth of a second later what they have is plugged in, so
 * that the game sees the port empty in between. On a Dreamcast that is only
 * the devices that change: a game saving to a card on another port went on
 * writing to nothing for that tenth of a second, and a save cut off there
 * left the card's file system half written. */
void maple_ReconnectDevices(void)
{
	if (settings.System == DC_PLATFORM_DREAMCAST)
		mcfg_ReconcileDevices(false);
	else
		mcfg_DestroyDevices();
	reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

static void maple_handle_reconnect(void)
{
	if (reconnect_time != 0 && reconnect_time <= sh4_sched_now64())
	{
		reconnect_time = 0;
		/* (a state loaded in the meantime has put its own devices there:
		 * what is not as the settings say goes first) */
		if (settings.System == DC_PLATFORM_DREAMCAST)
			mcfg_ReconcileDevices(true);
		else
		{
			mcfg_DestroyDevices();
			mcfg_CreateDevices();
		}
	}
}

/* After a load. A state saved before the settings changed a Dreamcast's
 * devices and loaded after - run-ahead does just that, every frame - puts
 * back the devices of before, with nothing waiting to bring the new ones:
 * the change was lost. What is not as the settings say is unplugged then,
 * and what they say is plugged in a tenth of a second later, as when they
 * changed. It is the same for the same state and settings, every time. */
static void maple_devices_after_load(void)
{
	if (settings.System != DC_PLATFORM_DREAMCAST || reconnect_time != 0
			|| mcfg_DevicesAsSettingsSay())
		return;
	mcfg_ReconcileDevices(false);
	reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

/* What Maple has had in a save state since V21, after everything older:
 * the I/O boards' part (maple_devs.cpp), and when the devices that have
 * been unplugged are to be plugged in again, 0 if they are not waiting to
 * be. The size is always the same.
 *
 * The time was not in a state. With run-ahead, which loads a state every
 * frame, the devices came back in a frame that was run ahead and went
 * again with the load that followed, and by then nothing was waiting to
 * bring them back: a port's device changed, and every port was empty from
 * then on. */
void maple_serialize_v21(void **data, unsigned int *total_size)
{
	maple_jvs_serialize_v21(data, total_size);
	LIBRETRO_S(reconnect_time);
}

bool maple_unserialize_v21(void **data, unsigned int *total_size)
{
	u64 latest;

	maple_jvs_unserialize_v21(data, total_size);
	reconnect_time = 0;
	LIBRETRO_US(reconnect_time);
	/* (it is never more than a tenth of a second away) */
	latest = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
	if (reconnect_time > latest)
		reconnect_time = latest;
	maple_devices_after_load();
	return true;
}

/* A state from before V21: the boards as maple_devs.cpp says, and devices
 * waiting to be plugged in again go on waiting, as they did. */
void maple_state_before_v21(void)
{
	maple_jvs_state_before_v21();
	maple_devices_after_load();
}
