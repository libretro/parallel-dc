/* The devices on the maple bus: the controllers, memory cards, vibration
 * packs, keyboard, mouse, guns and microphone of a Dreamcast, an
 * Atomiswave's controllers and guns, the NAOMI's I/O board bridge
 * (jvs_bridge.c) and the card readers of its games. maple_if.h's
 * maple_dev_*() functions are all there is of them: each kind's own is in
 * memory of its own after the maple_device (maple_devs.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <streams/file_stream.h>
#include <encodings/deflate.h>
#include <retro_atomic.h>
#include <retro_miscellaneous.h>
#include "types.h"
#include "maple_if.h"
#include "maple_helper.h"
#include "maple_devs.h"
#include "maple_cfg.h"
#include "maple_state.h"
#include "jvs_bridge.h"
#include "vmu_beep.h"
#include "libretro/pad_state.h"
#include "hw/pvr/spg.h"
#include "hw/aica/aica_if.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "savewriter.h"
#include "paths.h"
#include "log/Log.h"

static const char *const maple_sega_controller_name = "Dreamcast Controller";
static const char *const maple_sega_twinstick_name = "Twin Stick";
static const char *const maple_sega_vmu_name = "Visual Memory";
static const char *const maple_ascii_stick_name = "ASCII STICK";
static const char *const maple_sega_kbd_name = "Emulated Dreamcast Keyboard";
static const char *const maple_sega_mouse_name = "Emulated Dreamcast Mouse";
static const char *const maple_sega_mic_name = "MicDevice for Dreameye";
static const char *const maple_sega_purupuru_name = "Puru Puru Pack";
static const char *const maple_sega_lightgun_name = "Dreamcast Gun";

static const char *const maple_sega_brand = "Produced By or Under License From SEGA ENTERPRISES,LTD.";

bool enable_naomi_15khz_dipswitch = false;
bool naomi_bios_multiregion = false;

/* The functions a device says it has (a word: unsigned) */
#define MFID_0_Input       0x01000000u	/* DC Controller, Lightgun buttons, arcade stick .. stuff like that */
#define MFID_1_Storage     0x02000000u	/* VMU , VMS */
#define MFID_2_LCD         0x04000000u	/* VMU */
#define MFID_3_Clock       0x08000000u	/* VMU */
#define MFID_4_Mic         0x10000000u	/* DC Mic (, dreameye too ?) */
#define MFID_5_ARGun       0x20000000u	/* Artificial Retina gun ? never developed */
#define MFID_6_Keyboard    0x40000000u	/* DC Keyboard */
#define MFID_7_LightGun    0x80000000u	/* DC Lightgun */
#define MFID_8_Vibration   0x00010000u	/* Puru Puru Puur~~~ */
#define MFID_9_Mouse       0x00020000u	/* DC Mouse */
#define MFID_10_StorageExt 0x00040000u	/* Storage ? probably never used */
#define MFID_11_Camera     0x00080000u	/* DreamEye */

/* ---- A frame: what is read from it, what is written to the answer ---- */

static void w8(maple_device *d, u8 data)
{
	*d->dma_buffer_out = data;
	d->dma_buffer_out += 1;
	d->dma_count_out[0] += 1;
}

static void w16(maple_device *d, u16 data)
{
	memcpy(d->dma_buffer_out, &data, 2);
	d->dma_buffer_out += 2;
	d->dma_count_out[0] += 2;
}

static void w32(maple_device *d, u32 data)
{
	memcpy(d->dma_buffer_out, &data, 4);
	d->dma_buffer_out += 4;
	d->dma_count_out[0] += 4;
}

static void wptr(maple_device *d, const void *src, u32 len)
{
	const u8 *src8 = (const u8 *)src;
	while (len--)
		w8(d, *src8++);
}

static void wstr(maple_device *d, const char *str, u32 len)
{
	size_t ln = strlen(str);
	verify(len >= ln);
	len -= (u32)ln;
	while (ln--)
		w8(d, (u8)*str++);
	while (len--)
		w8(d, 0x20);
}

static u8 r8(maple_device *d)
{
	const u8 rv = *d->dma_buffer_in;
	d->dma_buffer_in += 1;
	d->dma_count_in -= 1;
	return rv;
}

/* (a frame can be shorter than its command needs: the words it has not got
 * read as 0, and the count does not go below 0) */
static u32 r32(maple_device *d)
{
	u32 rv;
	if (d->dma_count_in < 4)
		return 0;
	memcpy(&rv, d->dma_buffer_in, 4);
	d->dma_buffer_in += 4;
	d->dma_count_in -= 4;
	return rv;
}

static void rptr(maple_device *d, void *dst, u32 len)
{
	u8 *dst8 = (u8 *)dst;
	while (len--)
		*dst8++ = r8(d);
}

static u32 r_count(const maple_device *d)
{
	return d->dma_count_in;
}

static u32 maple_dev_command(maple_device *d, u32 cmd);

/* A frame for the device: its command, the device's answer, the address
 * of each the other way round, and the length of the answer's data. A
 * device answering for a port says which others are plugged into it. */
static u32 maple_dev_frame(maple_device *d, const u32 *buffer_in, u32 buffer_in_len, u32 *buffer_out)
{
	const u32 command = buffer_in[0] & 0xFF;
	u32 reci = (buffer_in[0] >> 8) & 0xFF;	/* the recipient */
	const u32 send = (buffer_in[0] >> 16) & 0xFF;	/* the sender */
	u32 outlen = 0;
	u32 resp;

	d->dma_buffer_out = (u8 *)&buffer_out[1];
	d->dma_count_out = &outlen;
	d->dma_buffer_in = (const u8 *)&buffer_in[1];
	d->dma_count_in = buffer_in_len - 4;
	resp = maple_dev_command(d, command);

	if (reci & 0x20)
		reci |= maple_GetAttachedDevices(d->bus_id);

	verify((u32)(u8)(outlen / 4) * 4 == outlen);
	buffer_out[0] = (resp << 0) | (send << 8) | (reci << 16) | ((outlen / 4) << 24);
	return outlen + 4;
}

/* (how far a device's answers to "what are you" go: its maker's) */
static void w_status(maple_device *d, u32 func, u32 f0, u32 f1, u32 f2, u8 area,
		const char *name, u16 standby, u16 maximum)
{
	w32(d, func);
	w32(d, f0);
	w32(d, f1);
	w32(d, f2);
	w8(d, area);	/* area code: FF worldwide, 01 North America */
	w8(d, 0);	/* direction */
	wstr(d, name, 30);
	wstr(d, maple_sega_brand, 60);
	w16(d, standby);	/* the current it takes at rest, and at the most (0.1 mA) */
	w16(d, maximum);
}

/* ---- The save writer's progress, a device's ---- */

typedef struct maple_save_state
{
	retro_atomic_int_t failed;	/* set by the writer: a write did not go through */
	u32 wait;			/* frames until the next try, after one that failed */
	u32 again;			/* how long the last such wait was */
} maple_save_state;

/* Whether a write has failed since this was last asked: 0 if not, 1 the
 * first time, 2 every time after. There is a while before the next try -
 * a second, then twice as long each time, up to a minute: a file that
 * cannot be written at all (no such directory, a disk that is full) is not
 * hammered at, and whoever logs the failure does so the first time only. */
static int save_state_failed(maple_save_state *s)
{
	if (!retro_atomic_load_acquire_int(&s->failed) || !retro_atomic_exchange_int(&s->failed, 0))
		return 0;
	s->again = s->again == 0 ? 60 : s->again >= 1800 ? 3600 : s->again * 2;
	s->wait = s->again;
	return s->again == 60 ? 1 : 2;
}

/* Whether it is still too soon after a failure to try again */
static bool save_state_waiting(maple_save_state *s)
{
	if (s->wait == 0)
		return false;
	s->wait--;
	return true;
}

/* ---- The controllers: a Dreamcast's, an Atomiswave's, the twin stick and
 * the arcade stick ---- */

/* byte 0: 0  0  0  0  0  0  0  0
 * byte 1: 0  0  a5 a4 a3 a2 a1 a0
 * byte 2: R2 L2 D2 U2 D  X  Y  Z
 * byte 3: R  L  D  U  St A  B  C */
static u32 controller_capabilities(const maple_device *d)
{
	switch (d->type)
	{
	case MDT_TwinStick:
		return 0xfefe0000;	/* no analog axes, X Y A B D Start U/D/L/R U2/D2/L2/R2 */
	case MDT_AsciiStick:
		return 0xff070000;	/* no analog axes, X Y Z A B C Start U/D/L/R */
	default:
		/* 6 analog axes, X Y L2/D2(?) A B C Start U D L R; 4 analog axes (0-3) X Y A B Start U D L R */
		return d->atomiswave ? 0xff663f00 : 0xfe060f00;
	}
}

static u32 controller_kcode(const maple_device *d, u32 kcode)
{
	switch (d->type)
	{
	case MDT_TwinStick:
		return kcode | 0x0101;
	case MDT_AsciiStick:
		return kcode | 0xF800;
	default:
		/* (a Dreamcast's: DPad2, C, D and Z masked off) */
		return d->atomiswave ? kcode | AWAVE_TRIGGER_KEY : kcode | 0xF901;
	}
}

static u32 controller_axis(const maple_device *d, int index, const PlainJoystickState *pjs)
{
	u8 val = 0;
	if (d->type == MDT_TwinStick || d->type == MDT_AsciiStick)
		return 0x80;
	if (!d->atomiswave)
	{
		switch (index)
		{
		case 2:
			return pjs->joy[PJAI_X1];
		case 3:
			return pjs->joy[PJAI_Y1];
		case 0:
			return pjs->trigger[PJTI_R];	/* right trigger */
		case 1:
			return pjs->trigger[PJTI_L];	/* left trigger */
		default:
			return 0x80;			/* unused */
		}
	}
	if (index < 2 || index > 5)
		return 0x80;
	index -= 2;
	if (naomi_game_inputs != NULL && naomi_game_inputs->axes[index].name != NULL)
	{
		switch (naomi_game_inputs->axes[index].axis)
		{
		case 0:
			val = (u8)(pjs->joy[0] - 0x80);
			break;
		case 1:
			val = (u8)(pjs->joy[1] - 0x80);
			break;
		case 2:
			val = (u8)(pjs->joy[2] - 0x80);
			break;
		case 3:
			val = (u8)(pjs->joy[3] - 0x80);
			break;
		case 4:
			val = pjs->trigger[1];
			break;
		case 5:
			val = pjs->trigger[0];
			break;
		}
		if (naomi_game_inputs->axes[index].inverted)
			val = (u8)(0xff - val);
	}
	else
		val = (u8)(pjs->joy[index] - 0x80);
	return val;
}

static const char *controller_name(const maple_device *d)
{
	switch (d->type)
	{
	case MDT_TwinStick:
		return maple_sega_twinstick_name;
	case MDT_AsciiStick:
		return maple_ascii_stick_name;
	default:
		return maple_sega_controller_name;
	}
}

static u32 controller_command(maple_device *d, u32 cmd)
{
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		w_status(d, MFID_0_Input, controller_capabilities(d), 0, 0, 0xFF, controller_name(d), 0x01AE, 0x01F4);	/* 43 mA, 50 mA */
		if (cmd == MDC_AllStatusReq)
		{
			/* what a controller adds when asked for all of its status: its maker's notes, 80 bytes */
			const char *extra = "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  +DF";
			wptr(d, extra, (u32)strlen(extra));
			return MDRS_DeviceStatusAll;
		}
		return MDRS_DeviceStatus;

	case MDCF_GetCondition:
		{
			PlainJoystickState pjs;
			pjs_init(&pjs);
			if (!SYSTEM_IS_NAOMI())
				mcfg_get_input(d->config, &pjs);
			w32(d, MFID_0_Input);
			/* the keys, the triggers R and L, the stick, and two more axes no Dreamcast has */
			w16(d, (u16)controller_kcode(d, pjs.kcode));
			w8(d, (u8)controller_axis(d, 0, &pjs));
			w8(d, (u8)controller_axis(d, 1, &pjs));
			w8(d, (u8)controller_axis(d, 2, &pjs));
			w8(d, (u8)controller_axis(d, 3, &pjs));
			w8(d, (u8)controller_axis(d, 4, &pjs));
			w8(d, (u8)controller_axis(d, 5, &pjs));
		}
		return MDRS_DataTransfer;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	default:
		INFO_LOG(MAPLE, "maple_sega_controller: Unknown maple command %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* ---- The Visual Memory Unit ---- */

#define VMU_FLASH_SIZE (128 * 1024)

typedef struct maple_vmu
{
	maple_device dev;
	RFILE *file;
	/* Bytes of flash_data changed since the last flush: [dirty_lo, dirty_hi) */
	u32 dirty_lo;
	u32 dirty_hi;
	/* Bytes where the card is not what its file has because a state was
	 * loaded: [loaded_lo, loaded_hi). See vmu_unserialize(). */
	u32 loaded_lo;
	u32 loaded_hi;
	maple_save_state save_state;	/* of the writes handed over: see vmu_flush_save() */
	u8 flash_data[VMU_FLASH_SIZE];
	u8 lcd_data[192];
	u8 lcd_data_decoded[VMU_SCREEN_WIDTH * VMU_SCREEN_HEIGHT];
	char path[PATH_MAX_LENGTH];	/* the card's file */
} maple_vmu;

/* The card a new one is: a zlib stream (vmu_inflate_default()) */
static const u8 vmu_default[] = {
		  0x78, 0x9c, 0xed, 0xdd, 0x5b, 0x88, 0x1c, 0x55, 0x1e, 0x07, 0xe0, 0xd3,
		  0x31, 0x6a, 0xf0, 0x3a, 0x0d, 0x0a, 0x5e, 0x18, 0x6a, 0x16, 0x02, 0x2e,
		  0xfb, 0x20, 0x68, 0x22, 0x2c, 0x28, 0x24, 0x21, 0x3d, 0xc2, 0x2e, 0xc6,
		  0x40, 0x26, 0xc3, 0x40, 0x2e, 0x90, 0x96, 0xec, 0xc8, 0x9a, 0xc4, 0x81,
		  0x6c, 0xa2, 0x20, 0x82, 0x11, 0xc1, 0x37, 0x41, 0x9f, 0x14, 0xf1, 0x61,
		  0x37, 0x64, 0xbc, 0x34, 0xb3, 0x17, 0xf0, 0xc1, 0x87, 0x0e, 0x9b, 0xb7,
		  0x55, 0x02, 0x3a, 0x19, 0x30, 0x8d, 0x0c, 0x1c, 0x82, 0xac, 0x17, 0x76,
		  0x59, 0x30, 0x77, 0xb5, 0x92, 0xce, 0xb1, 0xaa, 0x27, 0xc6, 0x98, 0x98,
		  0x68, 0xb2, 0x31, 0x15, 0x93, 0xef, 0x2b, 0x8a, 0x9a, 0xff, 0xef, 0xd4,
		  0xa9, 0x73, 0xe8, 0x3a, 0xf5, 0xd4, 0xc5, 0x74, 0x08, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd5, 0xfa, 0xf3, 0xdc, 0x2f, 0x47,
		  0x52, 0xfa, 0xae, 0xee, 0x7b, 0xfe, 0xf6, 0xaf, 0xe6, 0x0e, 0x84, 0xd0,
		  0x78, 0x20, 0x84, 0x4d, 0xa3, 0x95, 0x4d, 0x0b, 0x00, 0x7e, 0x31, 0xfe,
		  0xf8, 0xfb, 0x87, 0x6f, 0xa8, 0x7a, 0x0e, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xc0, 0x95, 0xe1, 0x77, 0x9f, 0xfc, 0x69, 0xb2, 0x3c, 0xde, 0x7a, 0xed,
		  0x9c, 0xf0, 0xef, 0xa1, 0xe9, 0x63, 0x55, 0xcf, 0x07, 0x00, 0x00, 0x00,
		  0x00, 0x2e, 0x07, 0x0b, 0xcf, 0x92, 0x5d, 0x5f, 0x7b, 0x69, 0xf0, 0xc7,
		  0xce, 0xfd, 0x29, 0xd7, 0x2b, 0xc5, 0x3b, 0x47, 0x36, 0xbd, 0x58, 0xfb,
		  0xe7, 0x55, 0xc7, 0x8a, 0x6d, 0x7b, 0x2d, 0x84, 0x05, 0x0b, 0x6e, 0xfe,
		  0xed, 0x6d, 0x3f, 0x75, 0x92, 0x5c, 0x14, 0x0b, 0x65, 0x57, 0x5c, 0x56,
		  0xe5, 0xd8, 0xb2, 0x6a, 0xb3, 0x4b, 0x6d, 0x3e, 0xb2, 0x6a, 0xb2, 0x2a,
		  0xc7, 0x96, 0xb9, 0xef, 0xb2, 0xea, 0xb3, 0x2a, 0xc7, 0x96, 0xb9, 0xef,
		  0xb2, 0x8b, 0x9f, 0x5d, 0x6a, 0xf3, 0x91, 0x55, 0x93, 0x55, 0x39, 0xb6,
		  0xcc, 0x7d, 0x97, 0x55, 0x9f, 0x55, 0x39, 0xb6, 0xac, 0xda, 0xfb, 0xfe,
		  0x7f, 0x6a, 0x8c, 0x8d, 0x6d, 0x78, 0x64, 0x7d, 0x73, 0xed, 0x1f, 0x4e,
		  0x04, 0x1b, 0x9b, 0x8f, 0xac, 0xdf, 0xbc, 0x69, 0x6c, 0xe3, 0xb7, 0xc1,
		  0xd0, 0xba, 0xb1, 0x8d, 0x63, 0x8f, 0x37, 0x37, 0x9d, 0xe8, 0xb2, 0xbc,
		  0xa8, 0xd7, 0x7d, 0xef, 0x22, 0xbf, 0x2e, 0xf6, 0xeb, 0x8a, 0xbd, 0x16,
		  0x66, 0xfe, 0xd1, 0xd7, 0x96, 0x0d, 0xff, 0x69, 0xbc, 0xfc, 0xfc, 0xdb,
		  0xc5, 0x94, 0x9f, 0x5a, 0xfc, 0xdf, 0xa2, 0xe3, 0xb3, 0xb3, 0x26, 0xff,
		  0x75, 0xf2, 0xf9, 0x0b, 0x7f, 0x60, 0x22, 0xb2, 0xca, 0xd7, 0x02, 0xf0,
		  0x0b, 0xf3, 0x41, 0xd5, 0x13, 0xa0, 0x22, 0x7d, 0x67, 0x6a, 0x98, 0xf3,
		  0xa3, 0x5d, 0x6b, 0x17, 0x74, 0x22, 0xe7, 0xed, 0xb3, 0xbb, 0xa7, 0xef,
		  0x5f, 0x74, 0x7a, 0x9c, 0xae, 0x3f, 0x2d, 0xfa, 0xcd, 0x45, 0x98, 0x0d,
		  0x17, 0xdb, 0x19, 0x57, 0xf0, 0xec, 0x62, 0x9f, 0x75, 0xa2, 0x38, 0x96,
		  0xce, 0x55, 0xaf, 0xdf, 0xb9, 0x77, 0x3b, 0xa3, 0x2c, 0xec, 0x3d, 0xb7,
		  0x0e, 0x0b, 0xce, 0x7f, 0xf0, 0x67, 0x8a, 0xa9, 0x6f, 0x39, 0xfb, 0xe7,
		  0x76, 0xd3, 0x85, 0xf8, 0xf0, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xb8, 0x24, 0xa4, 0x6d, 0x31, 0xc6, 0x3d, 0xdd, 0x5d, 0x77, 0x64, 0xbb,
		  0xfa, 0xa7, 0xba, 0xf9, 0x93, 0x69, 0xe7, 0xea, 0x18, 0xf3, 0x38, 0x55,
		  0xef, 0xee, 0xea, 0xcf, 0x52, 0xeb, 0xe9, 0x94, 0x2f, 0x89, 0x71, 0xd5,
		  0x5f, 0x9b, 0x59, 0x77, 0xaa, 0x9e, 0x26, 0x27, 0x26, 0x77, 0x1c, 0xdd,
		  0xb6, 0x3a, 0x4e, 0x77, 0x9b, 0x59, 0x7c, 0x21, 0x4b, 0xf9, 0xe4, 0x91,
		  0x1d, 0x69, 0xe7, 0xb6, 0x46, 0xa3, 0xf1, 0xab, 0xb4, 0x7c, 0x38, 0x5b,
		  0x31, 0x91, 0x8e, 0xb4, 0x53, 0xda, 0xd9, 0x6a, 0xb5, 0x26, 0xf3, 0xd6,
		  0x44, 0x9a, 0x48, 0xa9, 0xdd, 0xde, 0x71, 0xf2, 0x97, 0xd2, 0x47, 0xda,
		  0xed, 0xf6, 0xf7, 0xcb, 0xd3, 0xdf, 0xdb, 0x98, 0xd3, 0x7b, 0x0f, 0xb4,
		  0x7c, 0xb5, 0x24, 0xbf, 0x2d, 0x84, 0xc7, 0x76, 0x7f, 0xfa, 0x79, 0x79,
		  0xac, 0xcd, 0x0a, 0x61, 0xd9, 0xa2, 0xc7, 0x9f, 0x68, 0x6e, 0x3c, 0xf9,
		  0xdc, 0x5b, 0x06, 0xeb, 0xb3, 0x9f, 0xdb, 0xfa, 0xca, 0xab, 0xff, 0xb8,
		  0x7a, 0xd1, 0xa3, 0xd7, 0x5c, 0x88, 0x8f, 0x63, 0xf6, 0x59, 0xda, 0x2e,
		  0xcb, 0xdf, 0xcc, 0x7b, 0x68, 0x69, 0x63, 0xe9, 0xd0, 0xd0, 0xc0, 0xc8,
		  0xa2, 0x65, 0x03, 0x3d, 0xcb, 0x06, 0x17, 0x2f, 0x5d, 0xd6, 0x18, 0x58,
		  0xfa, 0xe0, 0xc0, 0xa9, 0x2d, 0x33, 0x4e, 0xed, 0x3f, 0xf3, 0x0a, 0xd0,
		  0x6b, 0xb5, 0xb5, 0x4f, 0xfc, 0xf0, 0xf5, 0xbf, 0x4e, 0x07, 0xd3, 0x9b,
		  0xf9, 0xe8, 0xe1, 0xb5, 0x87, 0x87, 0x0e, 0xdd, 0x7b, 0xf0, 0xde, 0x03,
		  0xf7, 0x1c, 0x2c, 0xb6, 0x03, 0xd9, 0xde, 0x81, 0xfd, 0x7d, 0xfb, 0xc2,
		  0xbe, 0xb0, 0xb7, 0xdd, 0x7e, 0x67, 0xb8, 0xb0, 0xbc, 0xb1, 0x78, 0xde,
		  0xbc, 0xf9, 0x2b, 0xdb, 0xc7, 0xcb, 0xe1, 0xe1, 0xc1, 0x32, 0x29, 0x83,
		  0xcd, 0x2b, 0x8a, 0xe5, 0x55, 0x2e, 0xa9, 0x56, 0x6b, 0x7c, 0xde, 0x60,
		  0x7b, 0xfb, 0xf0, 0xbb, 0x1f, 0xd7, 0xeb, 0x33, 0xcb, 0x27, 0x1f, 0x9f,
		  0xbf, 0x72, 0xfb, 0xaa, 0xb4, 0xa1, 0x96, 0xba, 0x31, 0xee, 0x2e, 0x96,
		  0x71, 0x3e, 0x74, 0xff, 0xd1, 0x9d, 0xdd, 0x58, 0x2b, 0xca, 0x38, 0x51,
		  0xae, 0xeb, 0xf1, 0xfb, 0x52, 0x2b, 0x76, 0x7a, 0xf5, 0x92, 0x5e, 0x3d,
		  0x2f, 0xfd, 0x3d, 0x76, 0xea, 0x65, 0x3d, 0x5d, 0xec, 0x31, 0x9f, 0x9f,
		  0x26, 0xe2, 0x68, 0x56, 0xfe, 0xb9, 0xa7, 0x57, 0x37, 0xd2, 0x1b, 0x71,
		  0x4d, 0x51, 0x77, 0x46, 0xdf, 0x5f, 0x31, 0x53, 0x8f, 0xc7, 0x62, 0xad,
		  0x97, 0xe7, 0x7c, 0x78, 0xbc, 0xee, 0x34, 0xbb, 0xb1, 0xd3, 0x4c, 0x31,
		  0xae, 0xd8, 0x13, 0xf3, 0xc1, 0x34, 0x5e, 0x34, 0x75, 0x46, 0x6b, 0x33,
		  0x33, 0x28, 0xea, 0xad, 0xcd, 0x7a, 0xbd, 0x2f, 0xf4, 0x86, 0x98, 0x58,
		  0x9d, 0x8f, 0xa4, 0xad, 0x59, 0xfa, 0xa2, 0x5e, 0xcf, 0xca, 0xba, 0x68,
		  0x1f, 0x49, 0xaf, 0x17, 0x3d, 0x53, 0x3c, 0xae, 0xec, 0x5f, 0x1e, 0xa7,
		  0x5b, 0x6f, 0x95, 0x87, 0xd1, 0xff, 0x0d, 0x17, 0xf5, 0x9e, 0x38, 0x5a,
		  0x5c, 0xa3, 0x3b, 0x5a, 0x8c, 0x92, 0x95, 0xf5, 0xee, 0xce, 0x9a, 0x6c,
		  0x5f, 0x16, 0x9b, 0xb1, 0xd3, 0x97, 0xf5, 0xce, 0x7f, 0xbd, 0xd6, 0x9d,
		  0x2a, 0xea, 0x2f, 0x3a, 0xbb, 0x7a, 0xed, 0xf1, 0x70, 0xad, 0x78, 0x58,
		  0x3f, 0xaa, 0x67, 0x71, 0x2a, 0xf5, 0xea, 0xbf, 0xd5, 0xe3, 0x96, 0xfe,
		  0x4e, 0xbd, 0xbf, 0xd3, 0xcc, 0x7b, 0xf5, 0x87, 0xf5, 0xb8, 0xa6, 0xff,
		  0x2f, 0x7b, 0xfb, 0x3b, 0x59, 0x51, 0x17, 0xcf, 0x7b, 0x27, 0xef, 0x2f,
		  0x1e, 0xee, 0xe2, 0x79, 0x4f, 0xf9, 0xc8, 0xcf, 0xb9, 0xb6, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		  0xe0, 0x72, 0x73, 0x34, 0xd9, 0x2e, 0xef, 0x2d, 0x4f, 0xfb, 0xc2, 0xfe,
		  0x70, 0x20, 0x1c, 0x0c, 0x87, 0xc2, 0xe1, 0xf0, 0x65, 0xf8, 0x2a, 0x7c,
		  0x1d, 0xf2, 0x70, 0x24, 0x1c, 0x0d, 0x79, 0xd1, 0x36, 0x7c, 0x8a, 0x5a,
		  0xf9, 0xc3, 0x7a, 0x67, 0x59, 0x2f, 0xb7, 0xbf, 0x52, 0xbf, 0x2b, 0x84,
		  0x81, 0x13, 0xbf, 0x93, 0x98, 0x7a, 0xfb, 0xb1, 0x50, 0x0b, 0xdd, 0x70,
		  0x63, 0xd8, 0x1c, 0xde, 0x0b, 0x59, 0x91, 0x6c, 0xf9, 0xb9, 0x97, 0x2d,
		  0x17, 0xc8, 0x37, 0x7a, 0x6b, 0xe6, 0x6e
};

/* The default VMU image, inflated into @dst, which must hold exactly the
 * uncompressed size */
static bool vmu_inflate_default(u8 *dst, size_t dst_len)
{
	void *inf;
	size_t wr = 0;
	int rv;

	if (!(inf = rinflate_new(15)))
		return false;
	rinflate_set_in(inf, vmu_default, sizeof(vmu_default));
	rinflate_set_out(inf, dst, dst_len);
	rv = rinflate_process(inf, NULL, &wr);
	rinflate_free(inf);
	return rv == RDEFLATE_PROCESS_END && wr == dst_len;
}

/* A date as a memory card gives it, from the console's clock (seconds since
 * 1950-01-01, local time): year (two bytes, low first), month, day, hour,
 * minute, second, and a byte that is 0. */
static void vmu_clock_date(u32 rtc, u8 *buf)
{
	const u32 secs = rtc % 86400;
	/* days since 0000-03-01 of the Gregorian calendar, in 400-year cycles */
	const u32 days = rtc / 86400 + 712163;
	const u32 era = days / 146097;
	const u32 doe = days - era * 146097;
	const u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const u32 mp = (5 * doy + 2) / 153;
	const u32 day = doy - (153 * mp + 2) / 5 + 1;
	const u32 month = mp < 10 ? mp + 3 : mp - 9;
	const u32 year = yoe + era * 400 + (month <= 2);

	buf[0] = (u8)(year % 256);
	buf[1] = (u8)(year / 256);
	buf[2] = (u8)month;
	buf[3] = (u8)day;
	buf[4] = (u8)(secs / 3600);
	buf[5] = (u8)(secs / 60 % 60);
	buf[6] = (u8)(secs % 60);
	buf[7] = 0;
}

static bool vmu_serialize(maple_vmu *v, void **data, unsigned int *total_size)
{
	LIBRETRO_SA(v->flash_data, VMU_FLASH_SIZE);
	LIBRETRO_SA(v->lcd_data, 192);
	LIBRETRO_SA(v->lcd_data_decoded, 48 * 32);
	return true;
}

/* What the game wrote to the card since the last time goes to the card's
 * file: handed to the writer's thread (savewriter.h), which does the
 * waiting for the disk. The file is the writer's from the first hand-over
 * on.
 *
 * A write that fails is found out later: the whole card is then written
 * again, since which part of the file is not what it should be is no
 * longer known, a second or so later and for as long as it goes on
 * failing. It used to be forgotten - what was to be written counted as
 * written whatever the write said. */
static void vmu_flush_save(maple_vmu *v)
{
	const int failed = save_state_failed(&v->save_state);
	if (failed)
	{
		if (failed == 1)
			WARN_LOG(MAPLE, "VMU %s: a write to its file failed. The card will be written again\n", v->dev.logical_port);
		v->dirty_lo = 0;
		v->dirty_hi = VMU_FLASH_SIZE;
	}
	if (v->dirty_lo >= v->dirty_hi || save_state_waiting(&v->save_state))
		return;
	if (v->file)
	{
		/* (not taken: too much is waiting to be written. Next frame.) */
		if (!save_writer_put(v->file, v->dirty_lo, &v->flash_data[v->dirty_lo], v->dirty_hi - v->dirty_lo, &v->save_state.failed))
			return;
	}
	else
		INFO_LOG(MAPLE, "Failed to save VMU %s data\n", v->dev.logical_port);
	v->dirty_lo = 0;
	v->dirty_hi = 0;
}

static bool vmu_unserialize(maple_vmu *v, void **data, unsigned int *total_size)
{
	/* What is on the card is about to be what the state says, and the file
	 * is still what it was. Loading a state does not write to the file:
	 * where the two now differ is noted, and goes to the file with the next
	 * thing the game writes to the card - or the file would be the card in
	 * part and what it was before in part, a broken file system. Whatever
	 * was waiting to be written belongs to the card as it was: out it goes
	 * first.
	 *
	 * (The whole card used to be written then, and every load counted as
	 * one that changed the card. A frontend's run-ahead loads a state every
	 * frame, and it is the card as it is but for what the game wrote in the
	 * last frame: usually nothing, and then there is nothing to do; while a
	 * game saves, a block or two, where it was 128K a frame.) */
	u32 lo = 0, hi = VMU_FLASH_SIZE, i;
	bool drawn;

	if (ra_unserialize_has(*data, VMU_FLASH_SIZE))
	{
		const u8 *const from = (const u8 *)*data;

		/* (by the block: that is what a game writes) */
		while (lo < hi && memcmp(&v->flash_data[lo], &from[lo], 512) == 0)
			lo += 512;
		while (hi > lo && memcmp(&v->flash_data[hi - 512], &from[hi - 512], 512) == 0)
			hi -= 512;
	}
	if (lo < hi)
	{
		v->save_state.wait = 0;
		vmu_flush_save(v);
		if (v->dirty_lo < v->dirty_hi)
		{
			/* (not taken: it waits with the rest) */
			lo = lo < v->dirty_lo ? lo : v->dirty_lo;
			hi = hi > v->dirty_hi ? hi : v->dirty_hi;
			v->dirty_lo = 0;
			v->dirty_hi = 0;
		}
		if (v->loaded_lo < v->loaded_hi)
		{
			lo = lo < v->loaded_lo ? lo : v->loaded_lo;
			hi = hi > v->loaded_hi ? hi : v->loaded_hi;
		}
		v->loaded_lo = lo;
		v->loaded_hi = hi;
	}
	LIBRETRO_USA(v->flash_data, VMU_FLASH_SIZE);
	LIBRETRO_USA(v->lcd_data, 192);
	LIBRETRO_USA(v->lcd_data_decoded, 48 * 32);
	/* The two cards of a controller show on one screen, which has the
	 * picture the game last drew on either. The second card's, after the
	 * first's, only if it has ever been drawn on: or a card the game does
	 * not draw on puts its blank screen in place of the other's picture at
	 * every load. */
	drawn = v->dev.bus_port == 0;
	for (i = 0; !drawn && i < sizeof(v->lcd_data); i++)
		drawn = v->lcd_data[i] != 0;
	if (drawn)
		vmu_lcd_publish(v->dev.bus_id, v->lcd_data_decoded);
	return true;
}

static void vmu_setup(maple_vmu *v)
{
	memset(v->flash_data, 0, sizeof(v->flash_data));
	memset(v->lcd_data, 0, sizeof(v->lcd_data));
	v->dirty_lo = 0;
	v->dirty_hi = 0;
	writable_vmu_path(v->path, sizeof(v->path), v->dev.logical_port);

	vmu_screen_params[v->dev.bus_id].vmu_lcd_screen = v->lcd_data_decoded;
	/* What the renderer shows until the game draws: the screen as it is now */
	vmu_lcd_publish(v->dev.bus_id, v->lcd_data_decoded);

	INFO_LOG(MAPLE, "Initializing VMU data...\n");
	verify(vmu_inflate_default(v->flash_data, sizeof(v->flash_data)));

	v->file = filestream_open(v->path, RETRO_VFS_FILE_ACCESS_READ_WRITE | RETRO_VFS_FILE_ACCESS_UPDATE_EXISTING, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (!v->file)
	{
		INFO_LOG(MAPLE, "Unable to open VMU save file \"%s\", creating new file\n", v->path);
		v->file = filestream_open(v->path, RETRO_VFS_FILE_ACCESS_READ_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
		if (v->file)
		{
			filestream_write(v->file, v->flash_data, sizeof(v->flash_data));
			filestream_seek(v->file, 0, RETRO_VFS_SEEK_POSITION_START);
		}
		else
			INFO_LOG(MAPLE, "Unable to create VMU!\n");
	}
	if (!v->file)
		WARN_LOG(MAPLE, "Failed to create VMU save file \"%s\"\n", v->path);
	else
	{
		filestream_read(v->file, v->flash_data, sizeof(v->flash_data));
		NOTICE_LOG(MAPLE, "Loaded VMU from file \"%s\"\n", v->path);
	}
}

static void vmu_destruct(maple_vmu *v)
{
	v->save_state.wait = 0;
	vmu_flush_save(v);
	if (v->save_state.wait != 0)
	{
		/* (that found a write had failed, and put the next one off: now, all the same) */
		v->save_state.wait = 0;
		vmu_flush_save(v);
	}
	/* the writer has the file, and the flag that is this card's */
	save_writer_drain();
	if (v->file && save_state_failed(&v->save_state))
		WARN_LOG(MAPLE, "VMU %s: the last write to its file failed\n", v->dev.logical_port);
	if (v->file)
		filestream_close(v->file);
}

static u32 vmu_command(maple_vmu *v, u32 cmd)
{
	maple_device *d = &v->dev;
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		/* its functions: the clock's, the LCD's, the storage's */
		w_status(d, MFID_1_Storage | MFID_2_LCD | MFID_3_Clock, 0x403f7e7e, 0x00100500, 0x00410f00,
				0xFF, maple_sega_vmu_name, 0x007c, 0x0082);	/* 12.4 mA, 13 mA */
		if (cmd == MDC_AllStatusReq)
		{
			/* and a memory card's */
			const char *extra = "Version 1.005,1999/04/15,315-6208-03,SEGA Visual Memory System BIOS Produced by ";
			wptr(d, extra, (u32)strlen(extra));
			return MDRS_DeviceStatusAll;
		}
		return MDRS_DeviceStatus;

	/* in[0] is the function used, and out[0] */
	case MDCF_GetMediaInfo:
		{
			const u32 function = r32(d);
			switch (function)
			{
			case MFID_1_Storage:
				{
					u16 system;
					DEBUG_LOG(MAPLE, "VMU %s GetMediaInfo storage\n", d->logical_port);
					w32(d, MFID_1_Storage);
					memcpy(&system, &v->flash_data[0xFF * 512 + 0x40], 2);
					if (system != 0xFF)
					{
						/* Unformatted: the media information it would have.
						 * total size, partition, system area block, FAT
						 * area block, FAT blocks, file information block,
						 * information blocks, volume icon, reserved, save
						 * area block, save blocks, reserved (for
						 * executable files?) */
						w16(d, 0xff);
						w16(d, 0);
						w16(d, 0xFF);
						w16(d, 0xfe);
						w16(d, 1);
						w16(d, 0xfd);
						w16(d, 0xd);
						w8(d, 0);
						w8(d, 0);
						w16(d, 0xc8);
						w16(d, 0x1f);
						w32(d, 0);
					}
					else
						/* from the card's system area (block 0xFF) */
						wptr(d, v->flash_data + 0xFF * 512 + 0x40, 24);
				}
				return MDRS_DataTransfer;

			case MFID_2_LCD:
				{
					const u32 pt = r32(d);
					if (pt != 0)
					{
						INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> bad input |%08X|, returning MDRE_UnknownCmd\n", pt);
						return MDRE_UnknownCmd;
					}
					w32(d, MFID_2_LCD);
					w8(d, 47);			/* X dots -1 */
					w8(d, 31);			/* Y dots -1 */
					w8(d, ((1) << 4) | (0));	/* 1 color, 0 contrast levels */
					w8(d, 2);			/* padding */
					return MDRS_DataTransfer;
				}

			default:
				INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> Bad function used |%08X|, returning -2\n", function);
				return MDRE_UnknownFunction;
			}
		}

	case MDCF_BlockRead:
		{
			const u32 function = r32(d);
			switch (function)
			{
			case MFID_1_Storage:
				{
					u32 xo, block;
					w32(d, MFID_1_Storage);
					xo = r32(d);
					block = (SWAP32(xo)) & 0xffff;
					w32(d, xo);
					if (block > 255)
					{
						DEBUG_LOG(MAPLE, "Block read : %d\n", block);
						DEBUG_LOG(MAPLE, "BLOCK READ ERROR\n");
						block &= 255;
					}
					else
						DEBUG_LOG(MAPLE, "VMU %s block read: Block %d addr %x len %d\n", d->logical_port, block, block * 512, 512);
					wptr(d, v->flash_data + block * 512, 512);
				}
				return MDRS_DataTransfer;

			case MFID_2_LCD:
				DEBUG_LOG(MAPLE, "VMU %s read LCD\n", d->logical_port);
				w32(d, MFID_2_LCD);
				w32(d, r32(d));
				/* (the screen's own bytes: it was the first 192 of the card's memory) */
				wptr(d, v->lcd_data, 192);
				return MDRS_DataTransfer;

			case MFID_3_Clock:
				if (r32(d) != 0)
				{
					INFO_LOG(MAPLE, "VMU: Block read: MFID_3_Clock : invalid params\n");
					return MDRE_TransmitAgain;
				}
				else
				{
					/* The date and time the console's clock says. (It was
					 * the host's: a game reading it got another answer when
					 * a frame was run again from a save state, which
					 * run-ahead and netplay do.) */
					u8 date[8];
					w32(d, MFID_3_Clock);
					vmu_clock_date(RealTimeClock, date);
					wptr(d, date, sizeof(date));
					DEBUG_LOG(MAPLE, "VMU: CLOCK Read-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d!\n",
							date[0] + date[1] * 256, date[2], date[3], date[4], date[5], date[6]);
					return MDRS_DataTransfer;
				}

			default:
				INFO_LOG(MAPLE, "VMU: cmd MDCF_BlockRead -> Bad function |%08X| used, returning -2\n", function);
				return MDRE_UnknownFunction;
			}
		}

	case MDCF_BlockWrite:
		switch (r32(d))
		{
		case MFID_1_Storage:
			{
				const u32 bph = r32(d);
				const u32 block = (SWAP32(bph)) & 0xffff;
				const u32 phase = ((SWAP32(bph)) >> 16) & 0xff;
				const u32 write_adr = block * 512 + phase * (512 / 4);
				const u32 write_len = r_count(d);
				DEBUG_LOG(MAPLE, "VMU %s block write: Block %d Phase %d addr %x len %d\n", d->logical_port, block, phase, write_adr, write_len);
				if (write_adr + write_len > sizeof(v->flash_data))
				{
					INFO_LOG(MAPLE, "Failed to write VMU %s: overflow\n", d->logical_port);
					return MDRE_TransmitAgain;
				}
				rptr(d, &v->flash_data[write_adr], write_len);

				/* written out by vmu_flush_save() between frames */
				if (v->dirty_lo >= v->dirty_hi)
				{
					v->dirty_lo = write_adr;
					v->dirty_hi = write_adr + write_len;
				}
				else
				{
					if (write_adr < v->dirty_lo)
						v->dirty_lo = write_adr;
					if (write_adr + write_len > v->dirty_hi)
						v->dirty_hi = write_adr + write_len;
				}
				if (v->loaded_lo < v->loaded_hi)
				{
					/* the first write after a state was loaded that changed the card: see vmu_unserialize() */
					v->dirty_lo = v->dirty_lo < v->loaded_lo ? v->dirty_lo : v->loaded_lo;
					v->dirty_hi = v->dirty_hi > v->loaded_hi ? v->dirty_hi : v->loaded_hi;
					v->loaded_lo = 0;
					v->loaded_hi = 0;
				}
				return MDRS_DeviceReply;
			}

		case MFID_2_LCD:
			{
				int x, y, l;
				DEBUG_LOG(MAPLE, "VMU %s LCD write\n", d->logical_port);
				r32(d);
				if (r_count(d) < 192)
				{
					INFO_LOG(MAPLE, "VMU %s LCD write invalid params: rcount %d\n", d->logical_port, r_count(d));
					return MDRE_TransmitAgain;
				}
				rptr(d, v->lcd_data, 192);
				for (y = 0; y < 32; ++y)
				{
					u8 *dst = v->lcd_data_decoded + y * 48;
					const u8 *src = v->lcd_data + 6 * y + 5;
					for (x = 0; x < 6; ++x)
					{
						u8 col = *src--;
						for (l = 0; l < 8; l++)
						{
							*dst++ = (col & 1) ? 0x00 : 0xff;
							col >>= 1;
						}
					}
				}
				mcfg_set_image(d->config, v->lcd_data_decoded);
				return MDRS_DeviceReply;
			}

		case MFID_3_Clock:
			if (r32(d) != 0 || r_count(d) != 8)
			{
				INFO_LOG(MAPLE, "VMU %s clock write invalid params: rcount %d\n", d->logical_port, r_count(d));
				return MDRE_TransmitAgain;
			}
			else
			{
				u8 timebuf[8];
				rptr(d, timebuf, 8);
				DEBUG_LOG(MAPLE, "VMU: CLOCK Write-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d! Nothing set tho ...\n",
						timebuf[0] + timebuf[1] * 256, timebuf[2], timebuf[3], timebuf[4], timebuf[5], timebuf[6]);
				return MDRS_DeviceReply;
			}

		default:
			INFO_LOG(MAPLE, "VMU: command MDCF_BlockWrite -> Bad function used, returning MDRE_UnknownFunction\n");
			return MDRE_UnknownFunction;
		}

	case MDCF_GetLastError:
		return MDRS_DeviceReply;

	case MDCF_SetCondition:
		switch (r32(d))
		{
		case MFID_3_Clock:
			{
				/* The alarm: its period, then its on time, in the first two
				 * bytes (the other two are a second alarm, not emulated). */
				const u32 bp = r32(d);
				if (bp)
					INFO_LOG(MAPLE, "BEEP : %08X\n", bp);
				vmu_beep_set(bp & 0xFF, (bp >> 8) & 0xFF);
				return MDRS_DeviceReply;
			}
		default:
			INFO_LOG(MAPLE, "VMU: command MDCF_SetCondition -> Bad function used, returning MDRE_UnknownFunction\n");
			return MDRE_UnknownFunction;
		}

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		vmu_beep_set(0, 0);
		return MDRS_DeviceReply;

	default:
		DEBUG_LOG(MAPLE, "Unknown MAPLE COMMAND %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* Whether a memory card would now be read from another file than the one it
 * was read from: the settings say which, and they may have changed. */
bool maple_vmu_file_changed(maple_device *dev)
{
	const maple_vmu *v = (const maple_vmu *)dev;
	char now[PATH_MAX_LENGTH];
	if (dev->type != MDT_SegaVMU)
		return false;
	writable_vmu_path(now, sizeof(now), dev->logical_port);
	return strcmp(v->path, now) != 0;
}

/* ---- The microphone ---- */

typedef struct maple_microphone
{
	maple_device dev;
	u8 micdata[SIZE_OF_MIC_DATA];
} maple_microphone;

static u32 microphone_command(maple_microphone *m, u32 cmd)
{
	maple_device *d = &m->dev;
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceRequest\n");
		/* (a controller's, with its function and name) */
		w_status(d, MFID_4_Mic, 0xfe060f00, 0, 0, 0xFF, maple_sega_mic_name, 0x01AE, 0x01F4);
		return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	/* (A microphone has no condition to ask for: the request is one it
	 * does not know. The answer made here was the controller's with all
	 * but two bytes taken out, six bytes where an answer is whole words.) */

	case MDC_DeviceReset:
		DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceReset\n");
		return MDRS_DeviceReply;

	case MDCF_MICControl:
		{
			const u32 function = r32(d);
			switch (function)
			{
			case MFID_4_Mic:
				{
					/* http://dcemulation.org/phpBB/viewtopic.php?f=34&t=69600
					 * the second word:
					 *   0x0000??03  the amplifier's gain, ?? from 00 to 1F (0x0f the default)
					 *   0x00008002  recording on
					 *   0x00000001  the samples while recording, else the microphone's status
					 *   0x00000002  recording off */
					const u32 secondword = r32(d);
					const u32 subcommand = secondword & 0xFF;	/* (its parameters not yet) */
					switch (subcommand)
					{
					case 0x01:
						w32(d, MFID_4_Mic);
						/* a word: the status (recording), the gain (the
						 * default), exp?, and the samples there are: none */
						w8(d, 0x04);
						w8(d, 0x0f);
						w8(d, 0);
						w8(d, 0);
						return MDRS_DataTransfer;
					case 0x02:
						DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl toggle recording %#010x\n", secondword);
						return MDRS_DeviceReply;
					case 0x03:
						DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl set gain %#010x\n", secondword);
						return MDRS_DeviceReply;
					case MDRE_TransmitAgain:
						WARN_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl MDRE_TransmitAgain\n");
						return MDRS_DeviceReply;
					default:
						INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED secondword %#010x\n", secondword);
						return MDRE_UnknownFunction;
					}
				}
			default:
				INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED function %#010x\n", function);
				return MDRE_UnknownFunction;
			}
		}

	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	default:
		INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED MAPLE COMMAND %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* ---- The vibration pack ---- */

typedef struct maple_purupuru
{
	maple_device dev;
	u16 AST;	/* the auto-stop time, and in ms */
	u16 AST_ms;
	u32 VIBSET;	/* the last vibration set */
} maple_purupuru;

static u32 purupuru_command(maple_purupuru *p, u32 cmd)
{
	maple_device *d = &p->dev;
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		w_status(d, MFID_8_Vibration, 0x00000101, 0, 0, 0xFF, maple_sega_purupuru_name, 0x00C8, 0x0640);
		return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	case MDCF_GetCondition:	/* the last vibration */
		w32(d, MFID_8_Vibration);
		w32(d, p->VIBSET);
		return MDRS_DataTransfer;

	case MDCF_GetMediaInfo:
		w32(d, MFID_8_Vibration);
		w32(d, 0x3B07E010);	/* the pack's vibration */
		return MDRS_DataTransfer;

	case MDCF_BlockRead:
		w32(d, MFID_8_Vibration);
		w32(d, 0);
		w16(d, 2);
		w16(d, p->AST);
		return MDRS_DataTransfer;

	case MDCF_BlockWrite:
		/* the auto-stop time (if the frame is long enough to have one) */
		if (d->dma_count_in >= 12)
		{
			p->AST = d->dma_buffer_in[10];
			p->AST_ms = (u16)(p->AST * 250 + 250);
		}
		return MDRS_DeviceReply;

	case MDCF_SetCondition:
		/* (a frame too short to say how is not acted on) */
		if (d->dma_count_in >= 8)
		{
			memcpy(&p->VIBSET, &d->dma_buffer_in[4], 4);
			mcfg_set_vibration(d->config, p->VIBSET, p->AST_ms);
		}
		return MDRS_DeviceReply;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	default:
		INFO_LOG(MAPLE, "UNKOWN MAPLE COMMAND %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* ---- The keyboard ---- */

u8 kb_shift;		/* shift keys pressed (bitmask) */
u8 kb_led;		/* leds currently lit */
u8 kb_key[6] = { 0 };	/* normal keys pressed */

static u32 keyboard_command(maple_device *d, u32 cmd)
{
	int i;
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		w_status(d, MFID_6_Keyboard, 0x80000502, 0, 0, 0xFF, maple_sega_kbd_name, 0x01AE, 0x01F5);	/* US, 104 keys */
		return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	case MDCF_GetCondition:
		w32(d, MFID_6_Keyboard);
		/* the shift keys, the leds lit, the other keys pressed (6) */
		w8(d, kb_shift);
		w8(d, kb_led);
		for (i = 0; i < 6; i++)
			w8(d, kb_key[i]);
		return MDRS_DataTransfer;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	default:
		INFO_LOG(MAPLE, "Keyboard: unknown MAPLE COMMAND %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* ---- The mouse ---- */

/* The buttons: bit 0 C, bit 1 the right (B), bit 2 the left (A), bit 3 the
 * wheel's */
u32 mo_buttons[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
/* Relative mouse coordinates [-512:511] */
f32 mo_x_delta[4];
f32 mo_y_delta[4];
f32 mo_wheel_delta[4];
/* Absolute mouse coordinates, [0:639] [0:479], or outside that when the
 * pointer is off the screen or outside the 4:3 window */
f32 mo_x_abs[4];
f32 mo_y_abs[4];

static u16 mo_cvt(f32 delta)
{
	delta += 0x200;
	if (delta <= 0)
		delta = 0;
	else if (delta > 0x3FF)
		delta = 0x3FF;
	return (u16)maple_round_away(delta);
}

static u32 mouse_command(maple_device *d, u32 cmd)
{
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		w_status(d, MFID_9_Mouse, 0x00070E00, 0, 0, 0xFF, maple_sega_mouse_name, 0x0069, 0x0120);	/* 3 buttons, 3 axes */
		return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	case MDCF_GetCondition:
		{
			u32 buttons;
			f32 delta_x, delta_y, delta_wheel;
			mcfg_get_mouse(d->config, &buttons, &delta_x, &delta_y, &delta_wheel);

			w32(d, MFID_9_Mouse);
			/* The buttons, pressed is 0: bit 2 left, bit 1 right, bit 3 the
			 * middle one (the wheel's). The three bytes after it are
			 * options, the axes that overflowed and one kept for later, and
			 * are 0. (All four were sent as one word of buttons with every
			 * bit above the three set: every axis always overflowing.
			 * Upstream 9d4711a95: Silent Scope, Dreamkey.) */
			w8(d, (u8)buttons);
			w8(d, 0);
			w8(d, 0);
			w8(d, 0);
			/* the axes (0-$3FF, low first): across, down, the wheel, and five more */
			w16(d, mo_cvt(delta_x));
			w16(d, mo_cvt(delta_y));
			w16(d, mo_cvt(delta_wheel));
			w16(d, mo_cvt(0));
			w16(d, mo_cvt(0));
			w16(d, mo_cvt(0));
			w16(d, mo_cvt(0));
			w16(d, mo_cvt(0));
		}
		return MDRS_DataTransfer;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	default:
		INFO_LOG(MAPLE, "Mouse: unknown MAPLE COMMAND %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* ---- The light gun: a Dreamcast's, an Atomiswave's ---- */

static u32 lightgun_kcode(const maple_device *d, u32 kcode)
{
	if (d->atomiswave)
		return (kcode & AWAVE_TRIGGER_KEY) == 0 ? ~(u32)AWAVE_BTN0_KEY : ~0u;
	return kcode | 0xFF01;
}

static u32 lightgun_command(maple_device *d, u32 cmd)
{
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		/* a light gun, a controller */
		w_status(d, MFID_7_LightGun | MFID_0_Input, 0, 0xFE000000, 0, 0x01, maple_sega_lightgun_name, 0x0069, 0x0120);	/* 10.5 mA, 28.8 mA */
		return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	case MDCF_GetCondition:
		{
			PlainJoystickState pjs;
			pjs_init(&pjs);
			mcfg_get_input(d->config, &pjs);
			w32(d, MFID_0_Input);
			w16(d, (u16)lightgun_kcode(d, pjs.kcode));
			/* the two trigger axes, which a gun has not: at rest. (They
			 * read as fully pulled, and Death Crimson 2 took that for "skip
			 * the story".) */
			w16(d, 0);
			w32(d, 0x80808080);	/* not used */
		}
		return MDRS_DataTransfer;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	default:
		INFO_LOG(MAPLE, "Light gun: unknown MAPLE COMMAND %d\n", cmd);
		return MDRE_UnknownCmd;
	}
}

/* ---- The NAOMI's I/O board bridge and its JVS I/O boards: jvs_bridge.c
 * and jvs_board.c ---- */

typedef struct maple_jamma
{
	maple_device dev;
	jvs_bridge bridge;
	maple_save_state save_state;
} maple_jamma;

/* The EEPROM, when the game has written to it, to its file: made anew each
 * time, by the writer's thread (savewriter.h). Tried again later if that
 * fails, as a memory card's is. */
static void jamma_flush_save(maple_jamma *j)
{
	const int failed = save_state_failed(&j->save_state);
	if (failed)
	{
		if (failed == 1)
			WARN_LOG(MAPLE, "Cannot save EEPROM to file %s. It will be tried again\n", eeprom_file);
		j->bridge.eeprom_dirty = true;
	}
	if (!j->bridge.eeprom_dirty || save_state_waiting(&j->save_state))
		return;
	if (save_writer_put_file(eeprom_file, EEPROM, 0x80, &j->save_state.failed))
		j->bridge.eeprom_dirty = false;
}

static void jamma_destruct(maple_jamma *j)
{
	j->save_state.wait = 0;
	jamma_flush_save(j);
	if (j->save_state.wait != 0)
	{
		j->save_state.wait = 0;
		jamma_flush_save(j);
	}
	/* (the flag is this device's: the writer is done with it first) */
	save_writer_drain();
	if (save_state_failed(&j->save_state))
		WARN_LOG(MAPLE, "Cannot save EEPROM to file %s\n", eeprom_file);
	EEPROM_loaded = false;
}

/* The NAOMI's I/O board bridge, if the machine has one: it is the first
 * port's, and it is the one in the state */
jvs_bridge *maple_jvs_bridge(void)
{
	maple_device *dev = MapleDevices[0][5];
	if (dev != NULL && dev->type == MDT_NaomiJamma)
		return &((maple_jamma *)dev)->bridge;
	return NULL;
}

/* ---- The IC card reader/writers of Virtua Fighter 4 and Mushiking ----
 *
 * A 838-14245-92 maple to RS232 converter wired to a 838-14243 RFID
 * reader/writer (apparently a Saxa HW210), one on the second and one on
 * the third bus, for the first and second player.
 *
 * The cards are the players': what is on them, whether one is in and
 * whether the game has locked it in are kept here and not in the devices,
 * which a state load makes anew. Each player's card is in a file in the
 * save directory, read when the game is started. */

typedef struct RfidCard
{
	u8 data[128];
	u8 d4Seen;
	u8 inserted;
	u8 locked;
} RfidCard;

static RfidCard rfid_card[2];
static u8 rfid_file[2][128];
static bool rfid_game;
static retro_atomic_int_t rfid_save_failed;

static const char *rfid_file_suffix(int player)
{
	return player == 0 ? "-p1.card" : "-p2.card";
}

static u8 rfid_random(u32 *seed)
{
	*seed = *seed * 1103515245u + 12345u;
	return (u8)(*seed >> 16);
}

/* A player's card from its file, or a new one: a Mushiking card with its
 * serial number and the key made from it, or a blank Virtua Fighter 4 card
 * with the random bytes the game makes its card number of, from @seed (not
 * rand(), which anything else in the process can move on). */
static void rfid_load_card(int player, u32 *seed)
{
	u8 *card = rfid_file[player];
	int i;
	if (periph_load_file(rfid_file_suffix(player), card, sizeof(rfid_file[0])) != 0)
		return;
	if (!strncmp(naomi_game_id, "MKG TKOB", 8))
	{
		static const u8 MUSHIKING_CHIP_DATA[128] = {
			0x12, 0x34, 0x56, 0x78,	/* Serial No.0 */
			0x31, 0x00, 0x86, 0x07,	/* Serial No.1 */
			0x00, 0x00, 0x00, 0x00,	/* Key */
			0x04, 0xf6, 0x00, 0xAA,	/* Extend  Extend  Access  Mode */
			0x23, 0xFF, 0xFF, 0xFF,	/* Counter4  Counter3  Counter2  Counter1 */
			0x00, 0x00, 0x00, 0x00,	/* User Data (first set date: day bits 0-4, month bits 5-8, year bits 9-... + 2000) */
			0x00, 0x00, 0x00, 0x00,	/* User Data */
			0x00, 0x00, 0x00, 0x00,	/* User Data */
			0x00, 0x00, 0x00, 0x00,	/* User Data */
			0x00, 0x00, 0x00, 0x00,	/* User Data */
			0x00, 0x00, 0x00, 0x00,	/* User Data */
			0x23, 0xFF, 0xFF, 0xFF	/* User Data (max counters) */
		};
		u32 mask = 0, serial1, key;
		memcpy(card, MUSHIKING_CHIP_DATA, sizeof(MUSHIKING_CHIP_DATA));
		for (i = 0; i < 8; i++)
			card[i] = rfid_random(seed);
		if (!strcmp(naomi_game_id, "MKG TKOB 2 JPN VER2.001-")		/* mushik2e */
				|| !strcmp(naomi_game_id, "MKG TKOB 4 JPN VER2.000-"))	/* mushik4e */
			mask = 0x40;
		card[4] &= ~0xc0;
		card[4] |= mask;

		serial1 = ((u32)card[4] << 24) | (card[5] << 16) | (card[6] << 8) | card[7];
		key = ~serial1;
		key = ((key >> 4) & 0x0f0f0f0f) | ((key << 4) & 0xf0f0f0f0);
		card[8] = (u8)(key >> 24);
		card[9] = (u8)(key >> 16);
		card[10] = (u8)(key >> 8);
		card[11] = (u8)key;
	}
	else
	{
		memset(card, 0, sizeof(rfid_file[0]));
		card[0] = 0x10;
		card[12] = 4;
		card[13] = 0x6c;
		card[127] = 0xff;
		card[2] = rfid_random(seed);
		card[4] = rfid_random(seed);
		card[5] = rfid_random(seed);
		card[6] = rfid_random(seed);
		card[7] = rfid_random(seed);
	}
	INFO_LOG(NAOMI, "Card P%d initialized\n", player + 1);
}

static void rfid_save_card(int player)
{
	memcpy(rfid_file[player], rfid_card[player].data, sizeof(rfid_file[0]));
	periph_save_file(rfid_file_suffix(player), rfid_file[player], sizeof(rfid_file[0]), &rfid_save_failed);
}

/* The game is one with the readers (or is not, with @game false): the
 * cards read from their files, and put in at once with @insert. */
void rfid_init(bool game, bool insert)
{
	u32 seed;
	int i;
	rfid_game = game;
	memset(rfid_card, 0, sizeof(rfid_card));
	if (!game)
		return;
	seed = periph_seed() * 2654435761u;
	for (i = 0; i < 2; i++)
	{
		rfid_load_card(i, &seed);
		if (insert)
			insertRfidCard(i);
	}
}

/* The card is put in, or taken out if it is in and not locked in */
void insertRfidCard(int playerNum)
{
	RfidCard *card;
	if (!rfid_game || playerNum < 0 || playerNum > 1)
		return;
	card = &rfid_card[playerNum];
	if (!card->inserted)
	{
		card->inserted = 1;
		memcpy(card->data, rfid_file[playerNum], sizeof(card->data));
	}
	else if (!card->locked)
	{
		card->inserted = 0;
		memset(card->data, 0, sizeof(card->data));
	}
}

const u8 *getRfidCardData(int playerNum)
{
	if (!rfid_game || playerNum < 0 || playerNum > 1)
		return NULL;
	return rfid_card[playerNum].data;
}

/* The cards in a save state (V24): what is on each, whether it is in and
 * locked, and whether the game asked for its data. 262 bytes. */
void rfid_serialize_v24(void **data, unsigned int *total_size)
{
	int i;
	if (!rfid_game)
		return;
	for (i = 0; i < 2; i++)
	{
		u8 flags[3];
		flags[0] = rfid_card[i].d4Seen;
		flags[1] = rfid_card[i].inserted;
		flags[2] = rfid_card[i].locked;
		LIBRETRO_SA(rfid_card[i].data, sizeof(rfid_card[i].data));
		LIBRETRO_SA(flags, 3);
	}
}

bool rfid_unserialize_v24(void **data, unsigned int *total_size)
{
	int i;
	if (!rfid_game)
		return true;
	for (i = 0; i < 2; i++)
	{
		u8 flags[3] = { 0, 0, 0 };
		LIBRETRO_USA(rfid_card[i].data, sizeof(rfid_card[i].data));
		if (!LIBRETRO_USA(flags, 3))
			return false;
		rfid_card[i].d4Seen = flags[0] != 0;
		rfid_card[i].inserted = flags[1] != 0;
		rfid_card[i].locked = flags[2] != 0;
	}
	return true;
}

/* An older state: no card was in, and no reader was there for one */
void rfid_state_before_v24(void)
{
	memset(rfid_card, 0, sizeof(rfid_card));
}

/* What each card out of its reader is - its file, or what the game last
 * wrote on it - in a state from V30 on (naomi_cards_serialize_v30()), 256
 * bytes: a card put in after a state load is the one the state was saved
 * with. Read past when the game has no readers. */
void rfid_files_serialize_v30(void **data, unsigned int *total_size)
{
	LIBRETRO_SA(&rfid_file[0][0], sizeof(rfid_file));
}

bool rfid_files_unserialize_v30(void **data, unsigned int *total_size)
{
	static u8 files[2][128];
	if (!LIBRETRO_USA(&files[0][0], sizeof(files)))
		return false;
	if (rfid_game)
		memcpy(rfid_file, files, sizeof(rfid_file));
	return true;
}

static RfidCard *rfid_reader_card(const maple_device *d)
{
	return &rfid_card[d->bus_id == 2 ? 1 : 0];
}

/* b0: !card switch, b1 to b5: state 4 (errors?) to 8, b6: the card locked
 * (0x40 while trying to read the card) */
static u32 rfid_status(const maple_device *d)
{
	const RfidCard *c = rfid_reader_card(d);
	u32 status = 1;
	if (c->inserted)
		status &= ~1u;
	if (c->locked)
		status |= 0x40;
	return status;
}

static u32 rfid_command(maple_device *d, u32 cmd)
{
	RfidCard *c = rfid_reader_card(d);
	switch (cmd)
	{
	case MDC_DeviceRequest:
	case MDC_AllStatusReq:
		/* a custom function, its flags; worldwide. The product name made up. */
		w_status(d, 0x00100000, 0, 0, 0, 0xff, "MAPLE/232C CONVERT BD", 0x0069, 0x0120);	/* 10.5 mA, 28.8 mA */
		return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

	case MDCF_GetCondition:
		w32(d, 0x00100000);	/* custom function */
		return MDRS_DataTransfer;

	case MDC_JVSSelfTest:
		w32(d, 0);
		return MDRS_JVSSelfTestReply;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		return MDRS_DeviceReply;

	/* 90 get status
	 * d0, 91, a0, 91, a1 (read md5 in data, or the data itself after d4), d4, 91: read test
	 * d9 lock
	 * da unlock
	 * d0, 91, then b1 (offset, length in words, two bytes, data) and 91 for each part, c1, 91: write test */
	case 0xD0:
		c->d4Seen = 0;
		/* fall through */
	case 0x90:
	case 0x91:
	case 0xA0:
	case 0xD4:
	case 0xC1:
		w32(d, rfid_status(d));
		if (cmd == 0xd4)
			c->d4Seen = 1;
		return 0xfe;

	case 0xA1:	/* read card data */
		DEBUG_LOG(MAPLE, "RFID card read (data? %d)\n", c->d4Seen);
		w32(d, rfid_status(d));
		/* serial0 and serial1 only, unless after D4 */
		wptr(d, c->data, c->d4Seen ? sizeof(c->data) : 8);
		return 0xfe;

	case 0xD9:	/* lock card */
		c->locked = 1;
		w32(d, rfid_status(d));
		INFO_LOG(MAPLE, "RFID card %d locked\n", d->bus_id);
		return 0xfe;

	case 0xDA:	/* unlock card */
		c->locked = 0;
		c->inserted = 0;
		w32(d, rfid_status(d));
		NOTICE_LOG(MAPLE, "RFID card %d unlocked\n", d->bus_id);
		return 0xfe;

	case 0xB1:	/* write to card */
		w32(d, rfid_status(d));
		if (r_count(d) >= 4)
		{
			const u32 offset = r8(d) * 4u;
			u32 size = r8(d) * 4u;
			r8(d);
			r8(d);
			/* (where and how much are the game's: no more than the card has, nor the frame) */
			if (size > r_count(d))
				size = r_count(d);
			if (offset < sizeof(c->data))
			{
				if (size > sizeof(c->data) - offset)
					size = (u32)sizeof(c->data) - offset;
				DEBUG_LOG(MAPLE, "RFID card write: offset 0x%x len %d\n", offset, size);
				rptr(d, c->data + offset, size);
				rfid_save_card(d->bus_id == 2 ? 1 : 0);
			}
		}
		return 0xfe;

	case 0xD1:	/* decrement counter */
		{
			int counter = r_count(d) >= 1 ? r8(d) : 0x03;
			switch (counter)
			{
			case 0x03:
				counter = 0;
				break;
			case 0x0c:
				counter = 1;
				break;
			case 0x30:
				counter = 2;
				break;
			case 0xc0:
				counter = 3;
				break;
			default:
				WARN_LOG(MAPLE, "Unknown counter selector %x\n", counter);
				counter = 0;
				break;
			}
			DEBUG_LOG(MAPLE, "RFID decrement %d\n", counter);
			c->data[19 - counter]--;
			rfid_save_card(d->bus_id == 2 ? 1 : 0);
			w32(d, rfid_status(d));
			return 0xfe;
		}

	default:
		INFO_LOG(MAPLE, "RFID: Unknown Maple command %x\n", cmd);
		return MDRE_UnknownCmd;
	}
}

static u32 rfid_frame(maple_device *d, const u32 *buffer_in, u32 buffer_in_len, u32 *buffer_out)
{
	const u32 cmd = buffer_in[0] & 0xff;
	u32 len;
	if (cmd == MDC_JVSGetId || cmd == MDC_JVSUploadFirmware || cmd == MDC_JVSCommand)
	{
		/* what the bridge answers with frames of its own */
		u32 out_len = 0;
		d->dma_buffer_out = (u8 *)buffer_out;
		d->dma_count_out = &out_len;
		d->dma_buffer_in = (const u8 *)buffer_in + 4;
		d->dma_count_in = buffer_in_len - 4;
		if (cmd == MDC_JVSGetId)
		{
			w8(d, 0x83);
			w8(d, 0x00);
			w8(d, 0x20);
			w8(d, 0x07);
			wstr(d, "315-6149    COPYRIGHT SEGA E", 28);
			w8(d, 0x83);
			w8(d, 0x00);
			w8(d, 0x20);
			w8(d, 0x05);
			wstr(d, "NTERPRISES CO,LTD.  1998    ", 28);
		}
		else if (cmd == MDC_JVSUploadFirmware && d->dma_count_in >= 4 && d->dma_buffer_in[1] != 0xff)
		{
			/* (the program is for the I/O board's processor, which this has not got) */
			u8 sum = 0;
			u32 i;
			for (i = 0; i < 0x1C && i < d->dma_count_in; i++)
				sum = (u8)(sum + d->dma_buffer_in[i]);
			w8(d, 0x80);
			w8(d, 0);
			w8(d, 0x20);
			w8(d, 0x01);
			w8(d, sum);
			w8(d, 0);
			w8(d, 0);
			w8(d, 0);
			w8(d, MDRS_DeviceReply);
			w8(d, 0x00);
			w8(d, 0x20);
			w8(d, 0x00);
		}
		else if (cmd == MDC_JVSUploadFirmware)
		{
			w8(d, MDRS_DeviceReply);
			w8(d, 0x00);
			w8(d, 0x20);
			w8(d, 0x00);
		}
		else
		{
			w8(d, MDRE_UnknownCmd);
			w8(d, 0x00);
			w8(d, 0x20);
			w8(d, 0x00);
		}
		return out_len;
	}
	len = maple_dev_frame(d, buffer_in, buffer_in_len, buffer_out);
	/* The recipient and the sender are not swapped in these replies (VF4 Final Tuned and Mushiking look) */
	if ((buffer_out[0] & 0xff) == 0xfe)
	{
		u8 *p = (u8 *)buffer_out;
		const u8 t = p[1];
		p[1] = p[2];
		p[2] = t;
	}
	return len;
}

/* ---- Every kind ---- */

static u32 maple_dev_command(maple_device *d, u32 cmd)
{
	switch (d->type)
	{
	case MDT_SegaVMU:
		return vmu_command((maple_vmu *)d, cmd);
	case MDT_Microphone:
		return microphone_command((maple_microphone *)d, cmd);
	case MDT_PurupuruPack:
		return purupuru_command((maple_purupuru *)d, cmd);
	case MDT_Keyboard:
		return keyboard_command(d, cmd);
	case MDT_Mouse:
		return mouse_command(d, cmd);
	case MDT_LightGun:
		return lightgun_command(d, cmd);
	case MDT_RFIDReaderWriter:
		return rfid_command(d, cmd);
	default:
		return controller_command(d, cmd);
	}
}

/* How much memory a device of @type has: what of it there is to keep */
static size_t maple_dev_size(MapleDeviceType type)
{
	switch (type)
	{
	case MDT_SegaVMU:
		return sizeof(maple_vmu);
	case MDT_Microphone:
		return sizeof(maple_microphone);
	case MDT_PurupuruPack:
		return sizeof(maple_purupuru);
	case MDT_NaomiJamma:
		return sizeof(maple_jamma);
	default:
		return sizeof(maple_device);
	}
}

/* The device of @type made at @at - the memory of one of the same type,
 * gone, so that a state loaded allocates nothing - or, with none, new */
maple_device *maple_dev_create_at(MapleDeviceType type, void *at)
{
	maple_device *d;
	switch (type)
	{
	case MDT_SegaController:
	case MDT_TwinStick:
	case MDT_AsciiStick:
	case MDT_Microphone:
	case MDT_SegaVMU:
	case MDT_PurupuruPack:
	case MDT_Keyboard:
	case MDT_Mouse:
	case MDT_LightGun:
	case MDT_NaomiJamma:
	case MDT_RFIDReaderWriter:
		break;
	default:
		ERROR_LOG(MAPLE, "Invalid device type %d\n", type);
		die("Invalid maple device type");
		return NULL;
	}
	if (at == NULL)
		at = malloc(maple_dev_size(type));
	if (at == NULL)
		return NULL;
	memset(at, 0, maple_dev_size(type));
	d = (maple_device *)at;
	d->type = (u8)type;
	d->atomiswave = (type == MDT_SegaController || type == MDT_LightGun)
			&& settings.System == DC_PLATFORM_ATOMISWAVE;
	if (type == MDT_PurupuruPack)
	{
		((maple_purupuru *)d)->AST = 19;
		((maple_purupuru *)d)->AST_ms = 5000;
	}
	else if (type == MDT_NaomiJamma)
		jvs_bridge_init(&((maple_jamma *)d)->bridge);
	return d;
}

maple_device *maple_dev_create(MapleDeviceType type)
{
	return maple_dev_create_at(type, NULL);
}

void *maple_dev_destruct(maple_device *dev)
{
	if (dev->type == MDT_SegaVMU)
		vmu_destruct((maple_vmu *)dev);
	else if (dev->type == MDT_NaomiJamma)
		jamma_destruct((maple_jamma *)dev);
	return dev;
}

void maple_dev_delete(maple_device *dev)
{
	if (dev == NULL)
		return;
	free(maple_dev_destruct(dev));
}

void maple_dev_setup(maple_device *dev, u32 addr, struct mcfg_map *config)
{
	dev->maple_port = (u8)addr;
	dev->bus_port = (u8)maple_GetPort(addr);
	dev->bus_id = (u8)maple_GetBusId(addr);
	dev->logical_port[0] = (char)('A' + dev->bus_id);
	dev->logical_port[1] = dev->bus_port == 5 ? 'x' : (char)('1' + dev->bus_port);
	dev->logical_port[2] = 0;
	dev->config = config;
	if (dev->type == MDT_SegaVMU)
		vmu_setup((maple_vmu *)dev);
	else if (dev->type == MDT_Microphone)
		memset(((maple_microphone *)dev)->micdata, 0, SIZE_OF_MIC_DATA);
}

u32 maple_dev_dma(maple_device *dev, u32 *in, u32 in_len, u32 *out)
{
	switch (dev->type)
	{
	case MDT_NaomiJamma:
		return jvs_bridge_dma(&((maple_jamma *)dev)->bridge, dev->config, in, in_len, out);
	case MDT_RFIDReaderWriter:
		return rfid_frame(dev, in, in_len, out);
	default:
		return maple_dev_frame(dev, in, in_len, out);
	}
}

bool maple_dev_has_slots(maple_device *dev)
{
	return maple_HasSlots((MapleDeviceType)dev->type);
}

/* A gun: the bus occupied for its position */
bool maple_dev_lightgun_pos(maple_device *dev)
{
	f32 x, y;
	if (dev->type != MDT_LightGun)
		return false;
	mcfg_get_absolute_position(dev->config, &x, &y);
	read_lightgun_position((int)(x + 0.5f), (int)(y + 0.5f));
	return true;
}

/* Write out what the game changed, off the emulation path */
void maple_dev_flush_save(maple_device *dev)
{
	if (dev->type == MDT_SegaVMU)
		vmu_flush_save((maple_vmu *)dev);
	else if (dev->type == MDT_NaomiJamma)
		jamma_flush_save((maple_jamma *)dev);
}

MapleDeviceType maple_dev_type(maple_device *dev)
{
	return (MapleDeviceType)dev->type;
}

u32 maple_dev_bus_id(const maple_device *dev)
{
	return dev->bus_id;
}

bool maple_dev_serialize(maple_device *dev, void **data, unsigned int *total_size)
{
	switch (dev->type)
	{
	case MDT_SegaVMU:
		return vmu_serialize((maple_vmu *)dev, data, total_size);
	case MDT_Microphone:
		LIBRETRO_SA(((maple_microphone *)dev)->micdata, SIZE_OF_MIC_DATA);
		return true;
	case MDT_PurupuruPack:
		{
			maple_purupuru *p = (maple_purupuru *)dev;
			LIBRETRO_S(p->AST);
			LIBRETRO_S(p->AST_ms);
			LIBRETRO_S(p->VIBSET);
		}
		return true;
	case MDT_NaomiJamma:
		return jvs_bridge_serialize(&((maple_jamma *)dev)->bridge, data, total_size);
	default:
		return true;
	}
}

bool maple_dev_unserialize(maple_device *dev, void **data, unsigned int *total_size)
{
	switch (dev->type)
	{
	case MDT_SegaVMU:
		return vmu_unserialize((maple_vmu *)dev, data, total_size);
	case MDT_Microphone:
		LIBRETRO_USA(((maple_microphone *)dev)->micdata, SIZE_OF_MIC_DATA);
		return true;
	case MDT_PurupuruPack:
		{
			maple_purupuru *p = (maple_purupuru *)dev;
			LIBRETRO_US(p->AST);
			LIBRETRO_US(p->AST_ms);
			LIBRETRO_US(p->VIBSET);
		}
		return true;
	case MDT_NaomiJamma:
		return jvs_bridge_unserialize(&((maple_jamma *)dev)->bridge, data, total_size);
	default:
		return true;
	}
}
