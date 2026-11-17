/* The NAOMI's I/O board bridge: see jvs_bridge.h */
#include <stdio.h>
#include <string.h>
#include <streams/file_stream.h>
#include <encodings/xxh32.h>
#include "jvs_bridge.h"
#include "maple_devs.h"
#include "maple_state.h"
#include "paths.h"
#include "hw/serial_port.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/naomi/naomi_periph.h"
#include "log/Log.h"

#define LOGJVS(...) DEBUG_LOG(JVS, __VA_ARGS__)

/* ---- The EEPROM ---- */

char EEPROM[0x100];
bool EEPROM_loaded = false;

/* The EEPROM's first record, the BIOS's settings: bytes 2 to 17, their
 * CRC in bytes 0 and 1, and all of it again in bytes 18 to 35. Byte 8 is
 * the cabinet: bit 0 the coin chutes (individual), bits 4-5 the players
 * less one. */
static u16 naomi_eeprom_crc(const u8 *buf, int size)
{
	u32 n = 0xdebdeb00;
	int i, c;
	for (i = 0; i <= size; i++)
	{
		/* (and a zero byte after the last) */
		n = (n & 0xffffff00) + (i < size ? buf[i] : 0);
		for (c = 0; c < 8; c++)
			n = (n & 0x80000000) ? (n << 1) + 0x10210000 : n << 1;
	}
	return (u16)(n >> 16);
}

static void naomi_eeprom_bios_set(u32 offset, u8 value)
{
	u8 *eeprom = (u8 *)EEPROM;
	u16 crc;
	eeprom[offset] = value;
	eeprom[offset + 18] = value;
	crc = naomi_eeprom_crc(eeprom + 2, 16);
	eeprom[0] = eeprom[18] = (u8)crc;
	eeprom[1] = eeprom[19] = (u8)(crc >> 8);
}

static u8 at_least_1(u8 v)
{
	return v > 1 ? v : 1;
}

/* A cabinet of these games is set for four players when the EEPROM has
 * not been set up for the game (its file is not there, or is another
 * game's): the BIOS's settings are then what the BIOS would make from the
 * game's header, with the four-player cabinet. One that has been is left
 * as it is, unless its number of players is one the header says the game
 * cannot have. */
static void naomi_eeprom_dual_cabinet(void)
{
	u8 header[0x500];
	const u8 *eeprom = (const u8 *)EEPROM;
	const u8 *game_id, *coin;
	u8 cabinet, vertical;
	int i;

	if (!jvs_dual_reversed_game() || !naomi_cart_GetBootHeader(header, sizeof(header)))
		return;
	game_id = &header[0x134];
	coin = &header[0x1e0];		/* the first region's coin settings */
	cabinet = header[0x429];	/* the numbers of players the game can have, a bit each */
	vertical = header[0x42b];

	if (memcmp(game_id, eeprom + 3, 4))
	{
		for (i = 0; i < 4; i++)
			naomi_eeprom_bios_set(3 + i, game_id[i]);
		naomi_eeprom_bios_set(7, 9);
		naomi_eeprom_bios_set(8, cabinet == 0 || (cabinet & 8) ? 0x30
				: (cabinet & 4) ? 0x20 : (cabinet & 2) ? 0x10 : 0);
		if (coin[0] == 1)
		{
			/* the game's own */
			naomi_eeprom_bios_set(2, (u8)((coin[1] & 1) | 0x10));
			if (coin[2] == 1)
				naomi_eeprom_bios_set(8, (u8)(eeprom[8] | 1));
			naomi_eeprom_bios_set(9, (u8)(coin[3] - 1));
			naomi_eeprom_bios_set(10, at_least_1(coin[6]));
			naomi_eeprom_bios_set(11, at_least_1(coin[4]));
			naomi_eeprom_bios_set(12, at_least_1(coin[5]));
			naomi_eeprom_bios_set(13, coin[7]);
			for (i = 0; i < 4; i++)
				naomi_eeprom_bios_set(14 + i, (u8)(coin[8 + i * 2] | (coin[9 + i * 2] << 4)));
		}
		else
		{
			/* the BIOS's */
			naomi_eeprom_bios_set(2, (vertical & 2) ? 0x11 : 0x10);
			naomi_eeprom_bios_set(9, 0);
			naomi_eeprom_bios_set(10, 1);
			naomi_eeprom_bios_set(11, 1);
			naomi_eeprom_bios_set(12, 1);
			naomi_eeprom_bios_set(13, 0);
			for (i = 0; i < 4; i++)
				naomi_eeprom_bios_set(14 + i, 0x11);
		}
		NOTICE_LOG(MAPLE, "EEPROM: set up for game %s, %d-player cabinet\n", naomi_game_id, (eeprom[8] >> 4) + 1);
	}
	if (cabinet != 0 && cabinet < 0x10 && !(cabinet & (1 << (eeprom[8] >> 4))))
	{
		const u8 players = (cabinet & 8) ? 0x30 : (cabinet & 4) ? 0x20 : (cabinet & 2) ? 0x10 : 0;
		NOTICE_LOG(MAPLE, "EEPROM: %d-player cabinet\n", (players >> 4) + 1);
		naomi_eeprom_bios_set(8, (u8)(players | (eeprom[8] & 1)));
	}
}

/* The EEPROM is read from its file once, when the machine starts
 * (maple_naomi_eeprom_load()), and is in memory from then on. (It was read
 * again each time the game asked. What the game writes goes to the file
 * later, from the writer's thread: asked for before that, the file was
 * still the old one, and the old one was then what got written.) The game
 * asking for it reads it here only after a state saved before it had been
 * read. */
void jvs_eeprom_load(void)
{
	RFILE *f;
	if (EEPROM_loaded)
		return;
	EEPROM_loaded = true;
	memset(EEPROM, 0, sizeof(EEPROM));
	f = filestream_open(eeprom_file, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	if (f)
	{
		filestream_read(f, EEPROM, 0x80);
		filestream_close(f);
		DEBUG_LOG(MAPLE, "Loaded EEPROM from %s\n", eeprom_file);
	}
	else if (naomi_default_eeprom != NULL)
		memcpy(EEPROM, naomi_default_eeprom, 0x80);
	naomi_eeprom_dual_cabinet();
}

/* The EEPROM read from its file as the machine starts, before a state can
 * be saved: a state has it then, and not word that it is yet to be read.
 * Read only when the game first asked, a state saved before that was loaded
 * into a machine whose file the game had written to since - which run-ahead
 * does every frame - and the game found what it had not yet written. */
void maple_naomi_eeprom_load(void)
{
	if (maple_jvs_bridge() != NULL)
		jvs_eeprom_load();
}

jvs_boards *maple_jvs_boards(void)
{
	jvs_bridge *br = maple_jvs_bridge();
	return br != NULL ? &br->boards : NULL;
}

/* ---- The bridge's processor and its RS-422 port ---- */

/* The program a game sends to the I/O board's processor, a piece at a time
 * over many frames: which program it is decides how the board's requests
 * are read (crazy_mode). All zeroes when none is being sent. In a save
 * state from V23: a state loaded while one is on its way goes on with what
 * had been sent by then. */
static u8 jvs_firmware[0x10000];

/* What is on the I/O board bridge's RS-422 port: Derby Owners Club's card
 * reader, through a converter board. Not the bridge's own: the bridge is
 * made anew by a state load. */
static serial_pipe *mie_pipe;

void naomi_mie_set_pipe(serial_pipe *pipe)
{
	mie_pipe = pipe;
}

/* The I/O board's program on its way, for naomi_serialize_v23(). */
void naomi_jvs_serialize_v23(void **data, unsigned int *total_size)
{
	LIBRETRO_SA(jvs_firmware, sizeof(jvs_firmware));
}

bool naomi_jvs_unserialize_v23(void **data, unsigned int *total_size)
{
	return LIBRETRO_USA(jvs_firmware, sizeof(jvs_firmware));
}

/* A reset: a program that was on its way to the board is sent again from
 * its start, and what had come of it is not to be taken for part of it. */
void maple_jvs_firmware_reset(void)
{
	memset(jvs_firmware, 0, sizeof(jvs_firmware));
}

/* ---- A frame ---- */

#define ALL_NODES 0xff

/* What a frame has, and where its answer goes */
typedef struct jvs_dma
{
	jvs_bridge *br;
	const struct mcfg_map *config;
	const u8 *in;
	u32 count_in;
	u8 *out;
	u32 count_out;
} jvs_dma;

static void w8(jvs_dma *d, u32 v)
{
	*d->out++ = (u8)v;
	d->count_out++;
}

static void w32(jvs_dma *d, u32 v)
{
	memcpy(d->out, &v, 4);
	d->out += 4;
	d->count_out += 4;
}

static void wbytes(jvs_dma *d, const void *src, u32 len)
{
	memcpy(d->out, src, len);
	d->out += len;
	d->count_out += len;
}

/* the first bytes of an answer */
static void w_head(jvs_dma *d, u32 code, u32 b1, u32 b2, u32 words)
{
	w8(d, code);
	w8(d, b1);
	w8(d, b2);
	w8(d, words);
}

static u8 sense_line(const jvs_bridge *br, u32 node_id)
{
	/* (bit 0 the sense line's level: set during F1 <n>, more I/O boards need addressing) */
	return node_id == br->boards.count ? 0x8E : 0x8F;
}

static void send_jvs_message(jvs_dma *d, u32 node_id, u32 channel, u32 length, const u8 *data)
{
	jvs_bridge *br = d->br;
	u8 temp_buffer[256];
	u32 out_len;
	if (node_id - 1 >= br->boards.count)
		return;
	if (br->boards.cabinet_order)
		jvs_boards_order(&br->boards);
	out_len = jvs_board_message(&br->boards, node_id - 1, d->config, data, length, temp_buffer);
	if (out_len > 0)
	{
		u8 *pbuf = &br->receive_buffer[channel][br->receive_length[channel]];
		if (br->receive_length[channel] + out_len + 3 <= sizeof(br->receive_buffer[0]))
		{
			if (br->crazy_mode)
			{
				pbuf[0] = 0x00;		/* ? 0: ok, 2: timeout, 3: dest node !=0, 4: checksum failed */
				pbuf[1] = (u8)out_len;
				memcpy(&pbuf[2], temp_buffer, out_len);
				br->receive_length[channel] += out_len + 2;
			}
			else
			{
				pbuf[0] = (u8)node_id;
				pbuf[1] = 0x00;		/* 0: ok, 2: timeout, 3: dest node !=0, 4: checksum failed */
				pbuf[2] = (u8)out_len;
				memcpy(&pbuf[3], temp_buffer, out_len);
				br->receive_length[channel] += out_len + 3;
			}
		}
	}
}

static void send_jvs_messages(jvs_dma *d, u32 node_id, u32 channel, bool use_repeat, u32 length,
		const u8 *data, bool repeat_first)
{
	jvs_bridge *br = d->br;
	/* A request is 255 bytes at the most. The four more are zeroes put
	 * after it: a request that stops short of what its code needs is read
	 * by a board that far past its end, and no further. */
	u8 temp_buffer[256 + 4];
	u32 i;
	if (data)
		memcpy(temp_buffer, data, length);
	if (node_id == ALL_NODES)
	{
		memset(temp_buffer + length, 0, 4);
		for (i = 0; i < br->boards.count; i++)
			send_jvs_message(d, i + 1, channel, length, temp_buffer);
	}
	else if (node_id >= 1 && node_id <= 32)
	{
		const u32 repeat_len = br->repeat_request[node_id - 1][0];
		/* (a stored request is left out if it and this one together are longer than a request can be) */
		if (use_repeat && repeat_len > 0 && length + repeat_len <= 256)
		{
			if (repeat_first)
			{
				memmove(temp_buffer + repeat_len, temp_buffer, length);
				memcpy(temp_buffer, &br->repeat_request[node_id - 1][1], repeat_len);
			}
			else
				memcpy(temp_buffer + length, &br->repeat_request[node_id - 1][1], repeat_len);
			length += repeat_len;
		}
		memset(temp_buffer + length, 0, 4);
		send_jvs_message(d, node_id, channel, length, temp_buffer);
	}
}

static void receive_jvs_messages(jvs_dma *d, u32 channel)
{
	jvs_bridge *br = d->br;
	u32 dword_length, pad;
	if (d->count_in == 0)
	{
		w_head(d, MDRS_JVSReply, 0, 0x20, 0x00);
		return;
	}
	dword_length = (br->receive_length[channel] + 0x10 + 3 - 1) / 4 + 1;

	w8(d, MDRS_JVSReply);
	w8(d, 0x00);
	w8(d, 0x20);
	if (br->receive_length[channel] == 0)
	{
		w8(d, 0x05);
		w8(d, 0x32);
	}
	else
	{
		w8(d, dword_length);
		w8(d, 0x16);
	}
	w8(d, 0xff);
	w8(d, 0xff);
	w8(d, 0xff);
	w32(d, 0xffffff00);
	w32(d, 0);
	w32(d, 0);

	if (br->receive_length[channel] == 0)
	{
		w32(d, 0);
		return;
	}

	w8(d, 0);
	w8(d, channel);
	if (br->crazy_mode)
		w8(d, 0x8E);
	else
		w8(d, sense_line(br, br->receive_buffer[channel][0]));

	/* The reply is made up to whole words, and what is past the message is
	 * zeroes: it was whatever the buffer had there (upstream 58f05f74c, an
	 * SH4 crash while a NAOMI started). */
	pad = dword_length * 4 - 0x10 - 3;
	memcpy(d->out, br->receive_buffer[channel], br->receive_length[channel]);
	memset(d->out + br->receive_length[channel], 0, pad - br->receive_length[channel]);
	d->out += pad;
	d->count_out += pad;
	br->receive_length[channel] = 0;
}

/* (the usual answer to a request sent on: a word, 0x18 (or @code) first) */
static void w_sent(jvs_dma *d, u32 code, u32 channel, u32 sense)
{
	w_head(d, MDRS_JVSReply, 0, 0x20, 0x01);
	w8(d, code);
	w8(d, channel);
	w8(d, sense);
	w8(d, 0);
}

static void handle_86_subcommand(jvs_dma *d)
{
	jvs_bridge *br = d->br;
	const u8 *in = d->in;
	u32 subcode;
	u8 node_id = 0;
	const u8 *cmd = NULL;
	u32 len = 0;
	u8 channel = 0;

	if (d->count_in == 0)
	{
		w_head(d, MDRS_JVSReply, 0, 0x20, 0x00);
		return;
	}
	subcode = in[0];
	/* CT fw uses 13 as a 17, and 17 as 13 and also uses 19 */
	if (br->crazy_mode)
	{
		if (subcode == 0x13)
			subcode = 0x17;
		else if (subcode == 0x17)
			subcode = 0x13;
	}
	if (d->count_in >= 3)
	{
		/* every request but "store" has its channel, node and length further on, if it is long enough */
		if (subcode != 0x13 && d->count_in >= 8)
		{
			node_id = in[6];
			len = in[7];
			cmd = &in[8];
			channel = in[5] & 0x1f;
		}
		else
		{
			node_id = in[1];
			len = in[2];
			cmd = &in[3];
		}
		/* (the length is the game's: no more than the frame has after it) */
		if (len > (u32)(in + d->count_in - cmd))
			len = (u32)(in + d->count_in - cmd);
	}

	switch (subcode)
	{
	case 0x13:	/* store repeated request */
		if (len > 0 && node_id > 0 && node_id <= 0x1f)
		{
			INFO_LOG(MAPLE, "JVS node %d: Storing %d cmd bytes\n", node_id, len);
			br->repeat_request[node_id - 1][0] = (u8)len;
			memcpy(&br->repeat_request[node_id - 1][1], cmd, len);
		}
		w_head(d, MDRS_JVSReply, 0, 0x20, 0x01);
		w8(d, in[0] + 1);	/* subcommand + 1 */
		w8(d, 0);
		w8(d, len + 1);
		w8(d, 0);
		break;

	case 0x15:	/* receive JVS data (there are 32 channels) */
		receive_jvs_messages(d, in[1] & 0x1f);
		if (br->hotd2p)
		{
			/* this firmware sends its next request along with it */
			send_jvs_messages(d, node_id, channel, true, len, cmd, false);
			w_sent(d, 0x18, channel, sense_line(br, node_id));
		}
		break;

	case 0x17:	/* transmit without repeat */
		br->receive_length[channel] = 0;
		send_jvs_messages(d, node_id, channel, false, len, cmd, false);
		w_sent(d, 0x18, channel, 0x8E);
		break;

	case 0x19:	/* transmit with repeat */
		br->receive_length[channel] = 0;
		send_jvs_messages(d, node_id, channel, true, len, cmd, true);
		w_sent(d, 0x18, channel, sense_line(br, node_id));
		break;

	case 0x21:	/* transmit with repeat */
		br->receive_length[channel] = 0;
		send_jvs_messages(d, node_id, channel, true, len, cmd, false);
		w_sent(d, 0x18, channel, sense_line(br, node_id));
		break;

	case 0x35:	/* receive then transmit with repeat (15 then 27) */
		receive_jvs_messages(d, channel);
		/* fall through */
	case 0x27:	/* transmit with repeat */
		{
			/* How many requests, and how long each is, are the game's to
			 * say: no more than the frame has is read. */
			const u32 cmd_count = d->count_in >= 8 ? in[6] : 0;
			u32 idx = 7, i;
			br->receive_length[channel] = 0;
			for (i = 0; i < cmd_count && idx + 2 <= d->count_in; i++)
			{
				node_id = in[idx];
				len = in[idx + 1];
				cmd = &in[idx + 2];
				if (len > d->count_in - idx - 2)
					len = d->count_in - idx - 2;
				idx += len + 2;
				send_jvs_messages(d, node_id, channel, true, len, cmd, false);
			}
			w_sent(d, 0x26, channel, sense_line(br, node_id));
		}
		break;

	case 0x33:	/* receive then transmit with repeat (15 then 21) */
		receive_jvs_messages(d, channel);
		send_jvs_messages(d, node_id, channel, true, len, cmd, false);
		w_sent(d, 0x18, channel, sense_line(br, node_id));
		break;

	case 0x0B:	/* EEPROM write */
		{
			int address = in[1];
			int size = in[2];
			DEBUG_LOG(MAPLE, "EEprom write %08X %08X\n", address, size);
			jvs_eeprom_load();
			/* (the chip has 0x80 bytes; the game's address and size went unchecked) */
			address &= 0x7f;
			if (size > 0x80 - address)
				size = 0x80 - address;
			/* (nor more than the frame has) */
			if (size > (int)d->count_in - 4)
				size = (int)d->count_in - 4;
			if (size > 0)
				memcpy(EEPROM + address, in + 4, size);
			br->eeprom_dirty = true;
			w_head(d, MDRS_JVSReply, 0x00, 0x20, 0x01);
			wbytes(d, EEPROM, 4);
		}
		break;

	case 0x3:	/* EEPROM read */
		jvs_eeprom_load();
		/* (0x80 bytes from an address below 0x80 are inside the array, which has 0x100) */
		w_head(d, MDRS_JVSReply, 0x00, 0x20, 0x20);
		wbytes(d, EEPROM + (in[1] & 0x7f), 0x80);
		break;

	case 0x31:	/* DIP switches */
		{
			/* in(5) bit 0: 1 VGA, 0 NTSCi. Switches 2 to 4 say the region
			 * to the multi-region BIOS: Korea is 2 and 3 on, 4 off (other
			 * BIOSes have their own). */
			u8 dip = enable_naomi_15khz_dipswitch ? 0x00 : 0xff;
			if (naomi_bios_multiregion)
				dip = (u8)((dip & ~0x0e) | 0x08);
			w_head(d, MDRS_JVSReply, 0x00, 0x20, 0x05);
			w8(d, 0x32);
			w8(d, 0xff);	/* in(0) */
			w8(d, 0xff);	/* in(1) */
			w8(d, 0xff);	/* in(2) */
			w8(d, 0x00);
			w8(d, 0xff);	/* in(4) */
			w8(d, dip);	/* in(5) */
			w8(d, 0xff);	/* in(6) */
			w32(d, 0x00);
			w32(d, 0x00);
			w32(d, 0x00);
		}
		break;

	case 0x1:
		w_head(d, MDRS_JVSReply, 0x00, 0x20, 0x01);
		w8(d, 0x2);
		w8(d, 0x0);
		w8(d, 0x0);
		w8(d, 0x0);
		break;

	/* The RS-422 port, with a device on it (it is not answered at all
	 * then). Without one the bridge says it knows none of these, as it
	 * always has here. */
	case 0x41:	/* reset? */
		if (mie_pipe == NULL)
			goto unknown;
		DEBUG_LOG(MAPLE, "JVS: RS422 reset\n");
		while (mie_pipe->ops->available(mie_pipe) > 0)
			mie_pipe->ops->read(mie_pipe);
		break;

	case 0x47:	/* send data */
		if (mie_pipe == NULL)
			goto unknown;
		DEBUG_LOG(MAPLE, "JVS: RS422 send %02x\n", in[4]);
		if (d->count_in >= 5)
			mie_pipe->ops->write(mie_pipe, in[4]);
		break;

	case 0x4d:	/* receive data */
		{
			int avail, i;
			if (mie_pipe == NULL)
				goto unknown;
			avail = mie_pipe->ops->available(mie_pipe);
			if (avail > 0xfe)
				avail = 0xfe;
			DEBUG_LOG(MAPLE, "JVS: RS422 receive %d bytes\n", avail);
			w_head(d, MDRS_JVSReply, 0x00, 0x20, 1 + (avail + 3) / 4);
			w8(d, 0);
			w8(d, 0);
			w8(d, 0);
			w8(d, avail == 0 ? 0xff : avail);	/* 0xff: no data, else the byte count */
			for (i = 0; i < ((avail + 3) / 4) * 4; i++)
				w8(d, i >= avail ? 0 : mie_pipe->ops->read(mie_pipe));
		}
		break;

	case 0x49:	/* I? */
	case 0x4b:	/* K? */
	case 0x4f:	/* O? */
		if (mie_pipe == NULL)
			goto unknown;
		break;

	default:
	unknown:
		INFO_LOG(MAPLE, "JVS: Unknown 0x86 sub-command %x\n", subcode);
		w_head(d, MDRE_UnknownCmd, 0x00, 0x20, 0x00);
		break;
	}
}

/* A piece of the program for the bridge's processor, or its end */
static void upload_firmware(jvs_dma *d)
{
	jvs_bridge *br = d->br;
	const u8 *in = d->in;
	u8 *ram = jvs_firmware;
	int xfer_bytes;
	u16 addr;
	u8 sum = 0;
	u32 i;

	if (d->count_in < 4)
	{
		/* (not long enough to say where its bytes go) */
		w_head(d, MDRE_UnknownCmd, 0x00, 0x00, 0x00);
		return;
	}
	if (in[1] == 0xff)
	{
		const u32 hash = xxh32(ram, 0x10000, 0);
		LOGJVS("JVS Firmware hash %08x\n", hash);
		br->hotd2p = hash == 0xa6784e26;
		br->crazy_mode = hash == 0xa7c50459	/* CT */
				|| hash == 0xae841e36		/* HOTD2 */
				|| br->hotd2p;
#ifdef DUMP_JVS_FW
		{
			RFILE *fw_dump;
			char filename[128];
			int n;
			for (n = 0; ; n++)
			{
				sprintf(filename, "z80_fw_%d.bin", n);
				fw_dump = filestream_open(filename, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
				if (fw_dump == NULL)
				{
					fw_dump = filestream_open(filename, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE);
					INFO_LOG(MAPLE, "Saving JVS firmware to %s\n", filename);
					break;
				}
				filestream_close(fw_dump);
			}
			if (fw_dump)
			{
				filestream_write(fw_dump, ram, 0x10000);
				filestream_close(fw_dump);
			}
		}
#endif
		/* (empty for the next one) */
		memset(ram, 0, 0x10000);
		for (i = 0; i < 32; i++)
			br->repeat_request[i][0] = 0;
		/* (the reply was never written: its code was returned as the length of one) */
		w_head(d, MDRS_DeviceReply, 0x00, 0x20, 0x00);
		return;
	}
	xfer_bytes = in[0] == 0xff ? 0x1C : 0x18;
	addr = (u16)((in[2] << 8) + in[3]);
	/* (where the bytes go is the game's to say: what is past the end of the 64 KB is left out) */
	if (xfer_bytes > 0x10000 - addr)
		xfer_bytes = 0x10000 - addr;
	/* (nor what is past the end of the frame, which may be the end of RAM) */
	if (xfer_bytes > (int)d->count_in - 4)
		xfer_bytes = (int)d->count_in - 4;
	memcpy(ram + addr, &in[4], xfer_bytes);
	for (i = 0; i < 0x1C && i < d->count_in; i++)
		sum = (u8)(sum + in[i]);

	w_head(d, 0x80, 0, 0x20, 0x01);	/* (or 0x81 on the boot ROM?) */
	w8(d, sum);
	w8(d, 0);
	w8(d, 0);
	w8(d, 0);
	w_head(d, MDRS_DeviceReply, 0x00, 0x20, 0x00);
}

/* (a string, made up to @len with spaces) */
static void wstr(jvs_dma *d, const char *str, u32 len)
{
	const u32 ln = (u32)strlen(str);
	wbytes(d, str, ln);
	for (len -= ln; len > 0; len--)
		w8(d, 0x20);
}

void jvs_bridge_init(jvs_bridge *br)
{
	memset(br, 0, sizeof(*br));
	jvs_boards_create(&br->boards);
}

u32 jvs_bridge_dma(jvs_bridge *br, const struct mcfg_map *config,
		const u32 *buffer_in, u32 buffer_in_len, u32 *buffer_out)
{
	jvs_dma d;
	u32 cmd;
#ifdef DUMP_JVS
	{
		u32 i;
		printf("JVS IN: ");
		for (i = 0; i < buffer_in_len; i++)
			printf("%02x ", ((const u8 *)buffer_in)[i]);
		printf("\n");
	}
#endif
	jvs_boards_create(&br->boards);
	/* the card reader and hopper buttons, read as the board's inputs are */
	naomi_periph_poll();

	d.br = br;
	d.config = config;
	d.in = (const u8 *)buffer_in + 4;
	d.count_in = buffer_in_len - 4;
	d.out = (u8 *)buffer_out;
	d.count_out = 0;

	cmd = *(const u8 *)buffer_in;
	switch (cmd)
	{
	case MDC_JVSSelfTest:
		w_head(&d, MDRS_JVSSelfTestReply, 0x00, 0x20, 0x01);
		w8(&d, 0x00);
		break;

	case MDC_JVSCommand:
		handle_86_subcommand(&d);
		break;

	case MDC_JVSUploadFirmware:
		upload_firmware(&d);
		break;

	case MDC_JVSGetId:
		w_head(&d, 0x83, 0x00, 0x20, 0x07);
		wstr(&d, "315-6149    COPYRIGHT SEGA E", 28);
		w_head(&d, 0x83, 0x00, 0x20, 0x05);
		wstr(&d, "NTERPRISES CO,LTD.  1998    ", 28);
		break;

	case MDC_DeviceRequest:
		w_head(&d, MDRS_DeviceStatus, 0x00, 0x20, 0x00);
		break;

	case MDC_AllStatusReq:
		/* (it went on into the next answer, and both were sent) */
		w_head(&d, MDRS_DeviceStatusAll, 0x00, 0x20, 0x00);
		break;

	case MDC_DeviceReset:
	case MDC_DeviceKill:
		w_head(&d, MDRS_DeviceReply, 0x00, 0x20, 0x00);
		break;

	case MDCF_GetCondition:
		w_head(&d, MDRE_UnknownCmd, 0x00, 0x00, 0x00);
		break;

	default:
		INFO_LOG(MAPLE, "Unknown Maple command %x\n", cmd);
		w_head(&d, MDRE_UnknownCmd, 0x00, 0x00, 0x00);
		break;
	}
#ifdef DUMP_JVS
	{
		u32 i;
		printf("JVS OUT: ");
		for (i = 0; i < d.count_out; i++)
			printf("%02x ", ((const u8 *)buffer_out)[i]);
		printf("\n");
	}
#endif
	return d.count_out;
}

/* ---- Its part of a state ---- */

bool jvs_bridge_serialize(const jvs_bridge *br, void **data, unsigned int *total_size)
{
	/* (how many boards there are as the build's own size_t: see below) */
	size_t board_count = br->boards.count;
	u32 i;
	LIBRETRO_S(br->crazy_mode);
	LIBRETRO_S(br->repeat_request);
	LIBRETRO_S(br->receive_length);
	LIBRETRO_S(br->receive_buffer);
	LIBRETRO_S(board_count);
	for (i = 0; i < br->boards.count; i++)
		jvs_board_serialize(&br->boards.board[i], data, total_size);
	return true;
}

bool jvs_bridge_unserialize(jvs_bridge *br, void **data, unsigned int *total_size)
{
	u8 crazy_mode = 0;
	u32 board_count = 0;
	u32 count_high = 0;
	u32 i;
	/* (a byte of the state's, which is true unless 0) */
	LIBRETRO_USA(&crazy_mode, 1);
	br->crazy_mode = crazy_mode != 0;
	LIBRETRO_US(br->repeat_request);
	LIBRETRO_US(br->receive_length);
	LIBRETRO_US(br->receive_buffer);
	/* (a state cannot have more waiting on a channel than a channel holds) */
	for (i = 0; i < 32; i++)
		if (br->receive_length[i] > sizeof(br->receive_buffer[0]))
			br->receive_length[i] = 0;
	/* (the prototype's own firmware is one of those that set crazy_mode; the BIOS's, before it, is not) */
	br->hotd2p = br->crazy_mode && !strcmp(naomi_game_id, "hotd2p");
	jvs_boards_create(&br->boards);
	/* How many boards there are is written as the build's own size_t:
	 * eight bytes by a 64-bit build, four by a 32-bit one. The four more
	 * are zeroes. Where a 32-bit build's state goes straight on, the next
	 * byte is the first board's address on the bus or, with no board, the
	 * device on the next port: never 0. So a state of either build is read
	 * by both. (Each took the other's for its own: the boards and every
	 * device after them were read four bytes out.) */
	LIBRETRO_US(board_count);
	if (LIBRETRO_US(count_high) && count_high != 0)
	{
		*data = (u8 *)*data - sizeof(count_high);
		*total_size -= sizeof(count_high);
	}
	/* The boards are this cabinet's, how many the state has is the state's
	 * to say: one it has and the cabinet has not is read past (what a board
	 * keeps: jvs_board_serialize()), and there are 31 addresses on the bus. */
	for (i = 0; i < board_count && i < 31; i++)
	{
		if (i < br->boards.count)
			jvs_board_unserialize(&br->boards.board[i], data, total_size);
		else
		{
			u8 skipped[2];
			LIBRETRO_US(skipped);
		}
	}
	return true;
}
