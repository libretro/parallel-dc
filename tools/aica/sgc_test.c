/* The sound chip's channels (core/hw/aica/sgc_if.c) against what they made
 * before: programs of random register writes - every sample format, loop,
 * envelope, LFO and filter setting, keyed on and off as they play - over
 * random sound memory, each run for its samples through the chip's own
 * code, its mixer and DSP output included, and every sample it sends out
 * taken into a CRC with what a channel's position and envelope read back
 * as. A third of them have the channels saved to a state half way through,
 * let go of and run on, and the state loaded back. sgc_run.sh compares the CRCs with sgc_expected.txt, which the C++
 * channels this replaced made.
 *
 *    sgc_test [programs]
 *
 * C89 (built as C++ against the C++ channels, for their expectations). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <encodings/crc32.h>
#include <features/features_cpu.h>

#include "types.h"
#include "hw/aica/aica.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/aica_mem.h"
#include "hw/aica/sgc_if.h"
#include "hw/aica/dsp.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/holly/holly_intc.h"
#include "hw/naomi/midiffb.h"

/* ---- what the channels link against, stood in for --------------------- */

unsigned ARAM_SIZE = 2 * 1024 * 1024;
unsigned ARAM_MASK = 2 * 1024 * 1024 - 1;
VArray2 aica_ram;
bool aica_interr;
RegisterStruct sb_regs[SB_REGS];
bool midiffb_enabled;

static u32 out_crc;
static u32 out_count;
static u32 out_loud;

void WriteSample(s16 r, s16 l)
{
	u8 b[4];
	b[0] = (u8)l; b[1] = (u8)((u16)l >> 8);
	b[2] = (u8)r; b[3] = (u8)((u16)r >> 8);
	out_crc = encoding_crc32(out_crc, b, 4);
	out_count++;
	out_loud += l != 0 || r != 0;
}

/* (a disc's music: a pattern, the same for every program) */
const s16 *libCore_CDDA_Sector(s16 *sector)
{
	int i;
	for (i = 0; i < 2352 / 2; i++)
		sector[i] = (s16)((i * 977) ^ 0x1234);
	return sector;
}

/* (the CRC with no SIMD found: the same CRC, and nothing of the CPU code) */
uint64_t cpu_features_get(void) { return 0; }
void varray2_zero(VArray2 *a) { memset(a->data, 0, a->size); }
void midiffb_reset(void) {}
int midiffb_in_byte(bool pop) { (void)pop; return -1; }
void midiffb_receive(u8 data) { (void)data; }
void aica_Init(void) {}
void aica_Reset(bool hard) { (void)hard; }
u32 ReadMem_aica_reg(u32 addr, u32 sz) { return libAICA_ReadReg(addr & 0x7FFF, sz); }
void WriteMem_aica_reg(u32 addr, u32 data, u32 sz) { libAICA_WriteReg(addr & 0x7FFF, data, sz); }
void aicaarm_run(u32 samples) { (void)samples; }
void asic_RaiseInterrupt(HollyInterruptID inter) { (void)inter; }
void asic_CancelInterrupt(HollyInterruptID inter) { (void)inter; }
void libARM_InterruptChange(u32 bits, u32 L) { (void)bits; (void)L; }
int sh4_sched_register(int tag, sh4_sched_callback *ssc) { (void)tag; (void)ssc; return 0; }
void sh4_sched_request(int id, int cycles) { (void)id; (void)cycles; }
bool sh4_sched_is_scheduled(int id) { (void)id; return true; }
u32 sh4_sched_remaining(int id) { (void)id; return 0; }
void os_DebugBreak(void) { abort(); }
bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
	memcpy(*dest, src, src_size);
	*dest = (u8 *)*dest + src_size;
	*total_size += src_size;
	return true;
}
bool ra_unserialize(void *dest, unsigned int size, void **src, unsigned int *total_size)
{
	memcpy(dest, *src, size);
	*src = (u8 *)*src + size;
	*total_size += size;
	return true;
}
/* (no log) */
retro_log_printf_t log_cb;

/* ---- the programs ------------------------------------------------------ */

static u32 seed;

static u32 rnd(void)
{
	seed = seed * 1103515245u + 12345u;
	return seed >> 8;
}

/* A channel's 18 registers (0x00 to 0x44, two bytes in four), random: the
 * address, the loop, the envelopes, the LFOs, the filter, the pitch and
 * the sends. The key-on bit is left off: the program keys the channels on
 * after. */
static void channel_random(u32 ch)
{
	u32 reg, lsa = 0;
	/* (half the time a short loop near the start, that the channel gets
	 * round many times) */
	const u32 short_loop = rnd() & 1;

	for (reg = 0; reg <= 0x44; reg += 4)
	{
		u32 v = rnd() & 0xFFFF;
		if (reg == 0)
			v &= ~0x8000u;
		if (short_loop && reg == 0x08)
			v = lsa = v & 63;
		if (short_loop && reg == 0x0C)
			v = lsa + (v & 63);
		/* (a quarter of the time a plain one: PCM, no LFO, open filter) */
		if (reg == 0x1C && (rnd() & 3) == 0)
			v = 0;
		libAICA_WriteReg(ch * 0x80 + reg, v, 2);
	}
}

/* Keyed on (or off, @on 0) with the key-on of all channels */
static void channel_key(u32 ch, u32 on)
{
	u32 r0 = libAICA_ReadReg(ch * 0x80, 2);
	r0 = (r0 & ~0x4000u) | (on ? 0x4000u : 0) | 0x8000u;
	libAICA_WriteReg(ch * 0x80, r0, 2);
}

/* The channels saved, as a state has them, and loaded straight back */
static void state_round_trip(void)
{
	static u8 buf[64 * 256];
	void *p = buf;
	unsigned int size = 0;

	u32 i;

	channel_serialize(&p, &size);
	channel_serialize_v21(&p, &size);
	/* (and between the two, every channel let go and played on: what is
	 * loaded is not what is there) */
	for (i = 0; i < 64; i++)
		channel_key(i, 0);
	for (i = 0; i < 300; i++)
		AICA_Sample();
	p = buf;
	size = 0;
	channel_unserialize(&p, &size, VCUR_LIBRETRO);
	channel_unserialize_v21(&p, &size);
}

static void program(u32 n, u32 samples)
{
	u32 i, s, monitor;

	seed = 0x9E3779B9u * (n + 1);
	monitor = rnd() & 63;
	libAICA_Reset(true);

	/* sound memory: noise, slow waves and silence, a block at a time */
	for (i = 0; i < ARAM_SIZE; i += 4096)
	{
		u32 kind = rnd() & 3, j;
		for (j = 0; j < 4096; j++)
			aica_ram.data[i + j] = kind == 0 ? 0 : kind == 1 ? (u8)rnd()
				: kind == 2 ? (u8)(j * 3) : (u8)((j >> 4) ^ (j << 3));
	}

	/* the common registers: volume, 18-bit output, mono; the sends of the
	 * DSP's outputs and the disc's music */
	libAICA_WriteReg(0x2800, (rnd() & 0xF) | (rnd() & 0x8000) | (rnd() & 0x100), 2);
	for (i = 0; i < 18; i++)
		libAICA_WriteReg(0x2000 + i * 4, rnd() & 0x0FFF, 2);

	for (i = 0; i < 16; i++)
		channel_random(rnd() & 63);
	for (i = 0; i < 64; i++)
		if (rnd() & 1)
			channel_key(i, 1);

	out_crc = 0;
	out_count = 0;
	out_loud = 0;
	for (s = 0; s < samples; s++)
	{
		/* in a third of the programs, a state saved and loaded half way */
		if (n % 3 == 0 && s == samples / 2)
			state_round_trip();
		AICA_Sample();
		/* now and then: a channel changed, keyed on or off; which channel
		 * is watched, and where it is and its envelope read back */
		/* in half the programs, where the watched channel is after every
		 * sample: around the loop end in particular */
		if (n & 1)
		{
			u8 b[2];
			u32 v;

			libAICA_WriteReg(0x280C, monitor << 8, 2);
			v = libAICA_ReadReg(0x2814, 2);
			b[0] = (u8)v; b[1] = (u8)(v >> 8);
			out_crc = encoding_crc32(out_crc, b, 2);
		}
		if ((s & 63) == 0)
		{
			u32 r = rnd();
			u32 ch = r & 63;
			u8 b[4];
			u32 v;

			if ((r >> 6 & 7) == 0)
				channel_random(ch);
			else if ((r >> 6 & 7) == 1)
				channel_key(ch, (r >> 9) & 1);
			else if ((r >> 6 & 7) == 2)
				libAICA_WriteReg(ch * 0x80 + 4 * ((r >> 10) % 18), (r >> 12) & 0xFFFF, 2);
			libAICA_WriteReg(0x280C, (monitor << 8) | ((r >> 20) & 0x80), 2);
			v = libAICA_ReadReg(0x2810, 2) | (libAICA_ReadReg(0x2814, 2) << 16);
			b[0] = (u8)v; b[1] = (u8)(v >> 8); b[2] = (u8)(v >> 16); b[3] = (u8)(v >> 24);
			out_crc = encoding_crc32(out_crc, b, 4);
		}
	}
	printf("%u %08x %u %u\n", n, out_crc, out_count, out_loud);
}

int main(int argc, char **argv)
{
	u32 n, programs = argc > 1 ? (u32)atoi(argv[1]) : 200;

	aica_ram.data = (u8 *)calloc(1, ARAM_SIZE);
	aica_ram.size = ARAM_SIZE;
	if (!aica_ram.data)
		return 2;
	libAICA_Init();
	for (n = 0; n < programs; n++)
		program(n, 6000);
	free(aica_ram.data);
	return 0;
}
