/*
	Copyright 2025 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include <string.h>
#include "v42.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_if.h"

/* V.42 timers */
#define T400_CYCLES ((u64)SH4_MAIN_CLOCK / 1000 * 750)	/* detection phase */
#define T403_CYCLES ((u64)SH4_MAIN_CLOCK / 1000 * 1000)	/* inactivity */

/*
 * The queues
 */
void byte_fifo_init(byte_fifo *f, u8 *buf, unsigned size)
{
	f->buf = buf;
	f->size = size;
	f->head = 0;
	f->count = 0;
	memset(buf, 0, size);
}

void byte_fifo_clear(byte_fifo *f)
{
	f->head = 0;
	f->count = 0;
}

int byte_fifo_size(const byte_fifo *f)
{
	return (int)f->count;
}

int byte_fifo_room(const byte_fifo *f)
{
	return (int)(f->size - f->count);
}

bool byte_fifo_push(byte_fifo *f, u8 v)
{
	if (f->count == f->size)
		return false;
	f->buf[(f->head + f->count) & (f->size - 1)] = v;
	f->count++;
	return true;
}

int byte_fifo_pop(byte_fifo *f)
{
	u8 v;
	if (f->count == 0)
		return -1;
	v = f->buf[f->head];
	f->head = (f->head + 1) & (f->size - 1);
	f->count--;
	return v;
}

int byte_fifo_front(const byte_fifo *f)
{
	return f->count == 0 ? -1 : f->buf[f->head];
}

void byte_fifo_serialize(const byte_fifo *f, void **data, unsigned int *total_size)
{
	static const u8 zeroes[2048] = { 0 };
	u16 n = (u16)f->count;
	/* the bytes in order: up to the end of the buffer, then from its start */
	const unsigned first = f->size - f->head < f->count ? f->size - f->head : f->count;
	unsigned left = f->size - f->count;
	LIBRETRO_S(n);
	LIBRETRO_SA(&f->buf[f->head], first);
	LIBRETRO_SA(f->buf, f->count - first);
	while (left > 0)
	{
		const unsigned k = left < sizeof(zeroes) ? left : (unsigned)sizeof(zeroes);
		LIBRETRO_SA(zeroes, k);
		left -= k;
	}
}

void byte_fifo_unserialize(byte_fifo *f, void **data, unsigned int *total_size)
{
	u16 n = 0;
	LIBRETRO_US(n);
	LIBRETRO_USA(f->buf, f->size);
	f->head = 0;
	f->count = n <= f->size ? n : f->size;
}

/*
 * V.14
 */
static void v14_transmit_bit(v14_encoder *e, u8 v)
{
	e->txCurByte = (u8)((e->txCurByte >> 1) | (v << 7));
	e->txBitCount++;
	if (e->txBitCount == 8)
	{
		byte_fifo_push(&e->buffer, e->txCurByte);
		e->txCurByte = 0;
		e->txBitCount = 0;
	}
}

void v14_encoder_init(v14_encoder *e)
{
	byte_fifo_init(&e->buffer, e->buf, sizeof(e->buf));
	e->txCurByte = 0;
	e->txBitCount = 0;
	e->txStopBits = 1;
}

void v14_encoder_write(v14_encoder *e, u8 v)
{
	int i;
	v14_transmit_bit(e, 0); /* start bit */
	for (i = 0; i < 8; i++)
	{
		v14_transmit_bit(e, v & 1);
		v >>= 1;
	}
	/* stop bit(s) */
	for (i = 0; i < e->txStopBits; i++)
		v14_transmit_bit(e, 1);
}

void v14_encoder_flush(v14_encoder *e)
{
	int i;
	if (e->txBitCount != 0)
	{
		while (e->txBitCount != 0)
			v14_transmit_bit(e, 1);
	}
	else
	{
		for (i = 0; i < 8; i++)
			v14_transmit_bit(e, 1);
	}
}

void v14_encoder_reset(v14_encoder *e)
{
	byte_fifo_clear(&e->buffer);
	e->txCurByte = 0;
	e->txBitCount = 0;
	e->txStopBits = 1;
}

int v14_encoder_read(v14_encoder *e)
{
	return byte_fifo_pop(&e->buffer);
}

int v14_encoder_available(const v14_encoder *e)
{
	return byte_fifo_size(&e->buffer);
}

void v14_encoder_set_stop_bits(v14_encoder *e, int stopBits)
{
	e->txStopBits = stopBits;
}

void v14_encoder_serialize(const v14_encoder *e, void **data, unsigned int *total_size)
{
	u8 bitCount = (u8)e->txBitCount;
	u8 stopBits = (u8)e->txStopBits;

	byte_fifo_serialize(&e->buffer, data, total_size);
	LIBRETRO_S(e->txCurByte);
	LIBRETRO_S(bitCount);
	LIBRETRO_S(stopBits);
}

void v14_encoder_unserialize(v14_encoder *e, void **data, unsigned int *total_size)
{
	u8 bitCount = 0;
	u8 stopBits = 1;

	byte_fifo_unserialize(&e->buffer, data, total_size);
	LIBRETRO_US(e->txCurByte);
	LIBRETRO_US(bitCount);
	LIBRETRO_US(stopBits);
	e->txBitCount = bitCount & 7;
	e->txStopBits = stopBits >= 1 && stopBits <= 16 ? stopBits : 1;
}

enum
{
	RX_IDLE,
	RX_RECEIVING,
	RX_STOP_BIT
};

static int max_int(int a, int b) { return a > b ? a : b; }
static int clamp_int(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void v14_decoder_init(v14_decoder *d)
{
	byte_fifo_init(&d->buffer, d->buf, sizeof(d->buf));
	v14_decoder_reset(d);
}

void v14_decoder_write(v14_decoder *d, u8 v)
{
	int i;
	for (i = 0; i < 8; i++)
	{
		if (d->rxState == RX_STOP_BIT)
		{
			/* waiting for 1st stop bit */
			if ((v & 1) == 0)
			{
				if (d->charsSinceMissingStop < 4)
					INFO_LOG(MODEM, "V.14 stop bit missing after %d chars\n", d->charsSinceMissingStop);
				d->charsSinceMissingStop = 0;
				d->rxState = RX_RECEIVING;
				d->rxBitCount = 0;
				d->maxStopBits = max_int(d->maxStopBits, d->rxStopBits);
			}
			else
			{
				d->rxStopBits = 1;
				d->rxState = RX_IDLE;
			}
		}
		else if (d->rxState == RX_IDLE)
		{
			/* idle or more stop bits state */
			if ((v & 1) == 0)
			{
				/* start bit => now receiving */
				d->rxState = RX_RECEIVING;
				d->rxBitCount = 0;
				d->maxStopBits = max_int(d->maxStopBits, d->rxStopBits);
			}
			else if (d->rxStopBits < 0x10000)
				d->rxStopBits++;
		}
		else
		{
			d->rxCurByte = (u8)((d->rxCurByte >> 1) | ((v & 1) << 7));
			d->rxBitCount++;
			if (d->rxBitCount == 8)
			{
				d->rxState = RX_STOP_BIT;
				byte_fifo_push(&d->buffer, d->rxCurByte);
				if (d->charsSinceMissingStop < 0x10000)
					d->charsSinceMissingStop++;
				d->rxStopBits = 0;
			}
		}
		v >>= 1;
	}
}

int v14_decoder_read(v14_decoder *d)
{
	return byte_fifo_pop(&d->buffer);
}

int v14_decoder_received_stop_bits(v14_decoder *d)
{
	const int max = d->maxStopBits;
	d->maxStopBits = 0;
	return max;
}

void v14_decoder_reset(v14_decoder *d)
{
	byte_fifo_clear(&d->buffer);
	d->rxCurByte = 0;
	d->rxState = RX_IDLE;
	d->rxBitCount = 0;
	d->charsSinceMissingStop = 0;
	d->rxStopBits = 0;
	d->maxStopBits = 0;
}

void v14_decoder_serialize(const v14_decoder *d, void **data, unsigned int *total_size)
{
	u8 st = (u8)d->rxState;
	u8 bitCount = (u8)d->rxBitCount;
	s32 since = d->charsSinceMissingStop;
	s32 stopBits = d->rxStopBits;
	s32 maxStop = d->maxStopBits;

	byte_fifo_serialize(&d->buffer, data, total_size);
	LIBRETRO_S(d->rxCurByte);
	LIBRETRO_S(st);
	LIBRETRO_S(bitCount);
	LIBRETRO_S(since);
	LIBRETRO_S(stopBits);
	LIBRETRO_S(maxStop);
}

void v14_decoder_unserialize(v14_decoder *d, void **data, unsigned int *total_size)
{
	u8 st = 0;
	u8 bitCount = 0;
	s32 since = 0;
	s32 stopBits = 0;
	s32 maxStop = 0;

	byte_fifo_unserialize(&d->buffer, data, total_size);
	LIBRETRO_US(d->rxCurByte);
	LIBRETRO_US(st);
	LIBRETRO_US(bitCount);
	LIBRETRO_US(since);
	LIBRETRO_US(stopBits);
	LIBRETRO_US(maxStop);
	d->rxState = st <= RX_STOP_BIT ? st : RX_IDLE;
	d->rxBitCount = bitCount <= 8 ? bitCount : 0;
	d->charsSinceMissingStop = clamp_int(since, 0, 0x10000);
	d->rxStopBits = clamp_int(stopBits, 0, 0x10000);
	d->maxStopBits = clamp_int(maxStop, 0, 0x10000);
}

/*
 * HDLC
 */
static u16 calcCrc16(const u8 *data, unsigned len)
{
	u16 crc = 0xffff;
	unsigned i;
	for (i = 0; i < len; i++)
	{
		u16 word = (crc ^ data[i]) & 0xFFu;
		word = (u16)(word ^ (u16)((word << 4u) & 0xFFu));
		word = (u16)((word << 8u) ^ (word << 3u) ^ (word >> 4u));
		crc = (u16)((crc >> 8) ^ word);
	}
	return crc ^ 0xffff;
}

void hdlc_encoder_init(hdlc_encoder *e, u8 *buf, unsigned size)
{
	byte_fifo_init(&e->buffer, buf, size);
	e->txBitBuffer = 0;
	e->txBitCount = 0;
}

void hdlc_encoder_send_flag(hdlc_encoder *e)
{
	e->txBitBuffer |= 0x7e << (8 + e->txBitCount);
	e->txBitBuffer >>= 8;
	byte_fifo_push(&e->buffer, e->txBitBuffer & 0xff);
}

static void hdlc_encoder_send_byte(hdlc_encoder *e, u8 byte)
{
	u32 mask;
	int i;
	e->txBitBuffer |= (u32)byte << (8 + e->txBitCount);
	mask = 0x1f0 << e->txBitCount;
	for (i = 0; i < 8; i++)
	{
		if ((e->txBitBuffer & mask) == mask)
		{
			const u32 lShiftMask = 0xffffffff << (8 + e->txBitCount + i + 1);
			e->txBitBuffer = ((e->txBitBuffer & lShiftMask) << 1) | (e->txBitBuffer & ~lShiftMask);
			e->txBitCount++;
			mask <<= 1;
		}
		mask <<= 1;
	}
	e->txBitCount += 8;
	while (e->txBitCount >= 8)
	{
		e->txBitBuffer >>= 8;
		byte_fifo_push(&e->buffer, e->txBitBuffer & 0xff);
		e->txBitCount -= 8;
	}
}

bool hdlc_encoder_send_frame(hdlc_encoder *e, const u8 *data, unsigned len)
{
	unsigned i;
	u16 crc;
	/* Flags, the frame and its CRC with a stuffed bit after every five ones at most */
	if ((unsigned)byte_fifo_room(&e->buffer) < (len + 2) * 6 / 5 + 4)
		return false;
	/* opening flag */
	hdlc_encoder_send_flag(e);
	/* data */
	for (i = 0; i < len; i++)
		hdlc_encoder_send_byte(e, data[i]);
	/* crc */
	crc = calcCrc16(data, len);
	hdlc_encoder_send_byte(e, crc & 0xff);
	hdlc_encoder_send_byte(e, crc >> 8);
	/* closing flag */
	hdlc_encoder_send_flag(e);
	return true;
}

void hdlc_encoder_reset(hdlc_encoder *e)
{
	byte_fifo_clear(&e->buffer);
	e->txBitCount = 0;
	e->txBitBuffer = 0;
}

int hdlc_encoder_read(hdlc_encoder *e)
{
	return byte_fifo_pop(&e->buffer);
}

int hdlc_encoder_available(const hdlc_encoder *e)
{
	return byte_fifo_size(&e->buffer);
}

void hdlc_encoder_serialize(const hdlc_encoder *e, void **data, unsigned int *total_size)
{
	u8 bitCount = (u8)e->txBitCount;

	byte_fifo_serialize(&e->buffer, data, total_size);
	LIBRETRO_S(e->txBitBuffer);
	LIBRETRO_S(bitCount);
}

void hdlc_encoder_unserialize(hdlc_encoder *e, void **data, unsigned int *total_size)
{
	u8 bitCount = 0;

	byte_fifo_unserialize(&e->buffer, data, total_size);
	LIBRETRO_US(e->txBitBuffer);
	LIBRETRO_US(bitCount);
	/* Fewer bits than an octet wait between two calls */
	e->txBitCount = bitCount & 7;
}

void hdlc_decoder_init(hdlc_decoder *d, u8 *frame, unsigned size)
{
	d->rxFrame = frame;
	d->size = size;
	memset(frame, 0, size);
	hdlc_decoder_reset(d);
}

void hdlc_decoder_write(hdlc_decoder *d, u8 v)
{
	int bit;
	/* (a frame not taken by now is dropped) */
	d->ready = false;
	for (bit = 0; bit < 8; bit++)
	{
		if (d->rxOnes >= 5)
		{
			if ((v & 1) == 1)
			{
				/* we either have a flag or an abort */
				d->rxCurByte = (u8)((d->rxCurByte >> 1) | 0x80);
				d->rxPosition++;
			}
			else
			{
				if (d->rxOnes == 6)
				{
					/* we have a flag */
					if (d->rxSize != 0 || d->overflow)
					{
						if (d->overflow)
							INFO_LOG(MODEM, "HDLC frame too long\n");
						else if (d->rxSize < 3)
							INFO_LOG(MODEM, "HDLC frame too short: %d bytes\n", d->rxSize);
						else
						{
							const u16 crc = calcCrc16(d->rxFrame, d->rxSize - 2);
							if (crc != (d->rxFrame[d->rxSize - 2] | (d->rxFrame[d->rxSize - 1] << 8)))
								WARN_LOG(MODEM, "Invalid CRC in received frame\n");
							else
							{
								d->ready = true;
								d->readySize = d->rxSize - 2;
							}
						}
						d->rxSize = 0;
						d->overflow = false;
					}
					d->rxCurByte = 0;
					d->rxPosition = 0;
				}
				else if (d->rxOnes > 6)
				{
					/* abort */
					INFO_LOG(MODEM, "HDLC abort received\n");
					d->rxCurByte = 0;
					d->rxPosition = 1; /* keep the last 0 */
					d->rxSize = 0;
					d->overflow = false;
				}
				/* ignore stuffed 0 */
				d->rxOnes = 0;
			}
		}
		else
		{
			d->rxCurByte = (u8)((d->rxCurByte >> 1) | ((v & 1) << 7));
			d->rxPosition++;
		}
		if (d->rxPosition == 8)
		{
			if (d->rxSize < d->size)
				d->rxFrame[d->rxSize++] = d->rxCurByte;
			else
				d->overflow = true;
			d->rxCurByte = 0;
			d->rxPosition = 0;
		}
		if ((v & 1) == 1)
		{
			/* (a long run of ones is an idle line: only how many up to 7 matters) */
			if (d->rxOnes < 7)
				d->rxOnes++;
		}
		else
			d->rxOnes = 0;
		v >>= 1;
	}
}

bool hdlc_decoder_frame_available(const hdlc_decoder *d)
{
	return d->ready;
}

const u8 *hdlc_decoder_frame(const hdlc_decoder *d)
{
	return d->rxFrame;
}

unsigned hdlc_decoder_frame_size(const hdlc_decoder *d)
{
	return d->readySize;
}

void hdlc_decoder_frame_taken(hdlc_decoder *d)
{
	d->ready = false;
}

void hdlc_decoder_reset(hdlc_decoder *d)
{
	d->rxSize = 0;
	d->overflow = false;
	d->ready = false;
	d->readySize = 0;
	d->rxPosition = 0;
	d->rxCurByte = 0;
	d->rxOnes = 0;
}

void hdlc_decoder_serialize(const hdlc_decoder *d, void **data, unsigned int *total_size)
{
	static const u8 zeroes[512] = { 0 };
	u16 size = (u16)d->rxSize;
	u8 bits[4];
	unsigned left = d->size - d->rxSize;

	bits[0] = (u8)d->overflow;
	bits[1] = (u8)d->rxOnes;
	bits[2] = (u8)d->rxPosition;
	bits[3] = d->rxCurByte;
	LIBRETRO_S(size);
	/* the frame so far, and zeroes after it */
	LIBRETRO_SA(d->rxFrame, d->rxSize);
	while (left > 0)
	{
		const unsigned k = left < sizeof(zeroes) ? left : (unsigned)sizeof(zeroes);
		LIBRETRO_SA(zeroes, k);
		left -= k;
	}
	LIBRETRO_SA(bits, 4);
}

void hdlc_decoder_unserialize(hdlc_decoder *d, void **data, unsigned int *total_size)
{
	u16 size = 0;
	u8 bits[4];

	memset(bits, 0, sizeof(bits));
	LIBRETRO_US(size);
	LIBRETRO_USA(d->rxFrame, d->size);
	LIBRETRO_USA(bits, 4);
	d->rxSize = size <= d->size ? size : 0;
	d->overflow = bits[0] != 0;
	d->rxOnes = bits[1] < 7 ? bits[1] : 7;
	d->rxPosition = bits[2] & 7;
	d->rxCurByte = bits[3];
	d->ready = false;
	d->readySize = 0;
}

/*
 * V.42
 */
enum
{
	PHASE_NONE,
	PHASE_DETECTION,
	PHASE_ESTABLISH,
	PHASE_CONNECTED,
	PHASE_RELEASE,
	PHASE_V14			/* Bomberman Online, Power Smash, Pro Yakyu Team de asobou net! */
};

static unsigned seqDistance(unsigned from, unsigned to)
{
	return (to - from) & 127;
}

static u8 incMod128(unsigned v)
{
	return (u8)((v + 1) & 127);
}

/* What the decompressor decodes, to the stream it goes to */
static void write_out(void *ctx, u8 v)
{
	const modem_stream *s = (const modem_stream *)ctx;
	s->write(s->ctx, v);
}

/* The time in SH4 cycles. The modem's protocols are set up before the
 * machine is made, when there is no time yet. */
static u64 now(void)
{
	return p_sh4rcb != NULL ? sh4_sched_now64() : 0;
}

static void v42_send_flag(v42_protocol *p)
{
	hdlc_encoder_send_flag(&p->hdlcEncoder);
}

static void v42_send_frame(v42_protocol *p, const u8 *data, unsigned len)
{
	if (!hdlc_encoder_send_frame(&p->hdlcEncoder, data, len))
		INFO_LOG(MODEM, "V.42: no room for a frame of %d bytes\n", len);
}

static void v42_ack_iframe(v42_protocol *p, int seqNum)
{
	/* N(R) acknowledges the frames before it: no further than the last one sent */
	if (seqDistance(p->txSeqAck, seqNum) <= seqDistance(p->txSeqAck, p->txSeqNum))
	{
		unsigned seq;
		for (seq = p->txSeqAck; seq != (unsigned)seqNum; seq = incMod128(seq))
			p->sentSize[seq & 15] = 0;
		p->txSeqAck = (u8)seqNum;
	}
	else
		WARN_LOG(MODEM, "Ack seq# %d outside of the frames sent %d-%d\n", seqNum, p->txSeqAck, p->txSeqNum);
}

static void v42_handle_sabme(v42_protocol *p, u8 address, u8 control)
{
	u8 ua[2];
	if (p->phase == PHASE_CONNECTED)
		WARN_LOG(MODEM, "V.42: SABME received while already connected\n");
	else
		INFO_LOG(MODEM, "V.42: Received SABME\n");
	ua[0] = address;
	ua[1] = (u8)(0x63 | (control & 0x10));
	v42_send_frame(p, ua, sizeof(ua));

	p->phase = PHASE_CONNECTED;
	p->txSeqNum = 0;
	p->rxSeqNum = 0;
	p->txSeqAck = 0;
	p->receiverReady = true;
	memset(p->sentSize, 0, sizeof(p->sentSize));
	p->resending = false;
}

static void v42_handle_disc(v42_protocol *p, u8 address)
{
	u8 ua[2];
	INFO_LOG(MODEM, "V.42: Received DISC\n");
	p->phase = PHASE_RELEASE;
	ua[0] = address;
	ua[1] = 0x73;
	v42_send_frame(p, ua, sizeof(ua));
}

static void v42_handle_iframe(v42_protocol *p, const u8 *rxFrame, unsigned size)
{
	const int seqNum = rxFrame[1] >> 1;
	unsigned i;
	if (seqNum != p->rxSeqNum)
	{
		u8 reject[3];
		INFO_LOG(MODEM, "Received frame num out of sequence: expected %d but recv %d\n", p->rxSeqNum, seqNum);
		/* send REJECT: N(R) is in the upper 7 bits */
		reject[0] = rxFrame[0];
		reject[1] = 9;
		reject[2] = (u8)(p->rxSeqNum << 1);
		v42_send_frame(p, reject, sizeof(reject));
		return;
	}
	p->rxSeqNum = incMod128(p->rxSeqNum);
	if (p->compressionEnabled)
	{
		for (i = 3; i < size; i++)
		{
			if (!v42b_decompressor_write(&p->decompressor, rxFrame[i], write_out, (void *)p->out))
			{
				/* send DISC */
				static const u8 disc[2] = { 1, 0x43 };
				v42_send_frame(p, disc, sizeof(disc));
				p->phase = PHASE_RELEASE;
				memset(p->sentSize, 0, sizeof(p->sentSize));
				p->resending = false;
				return;
			}
		}
	}
	else
	{
		for (i = 3; i < size; i++)
			p->out->write(p->out->ctx, rxFrame[i]);
	}

	/* if P bit set, respond with RR
	 * if not set, respond with RR if no I-frame available to send */
	if ((rxFrame[2] & 1) || p->in->available(p->in->ctx) == 0)
	{
		u8 rr[3];
		rr[0] = rxFrame[0];
		rr[1] = 1;
		rr[2] = (u8)((p->rxSeqNum << 1) | (rxFrame[2] & 1));
		v42_send_frame(p, rr, sizeof(rr));
	}
}

/* The value of a parameter: big-endian, 4 bytes at most */
static u32 xidValue(const u8 *p, unsigned len)
{
	u32 v = 0;
	unsigned i;
	for (i = 0; i < len; i++)
		v = (v << 8) | p[i];
	return v;
}

static void xidSetValue(u8 *p, unsigned len, u32 v)
{
	unsigned i;
	for (i = len; i > 0; i--)
	{
		p[i - 1] = v & 0xff;
		v >>= 8;
	}
}

enum
{
	GID_PARAM = 0x80,
	GID_PRIVATE = 0xf0,
	GID_USER_DATA = 0xff
};

/* The other end proposes the parameters of the link and of compression:
 * the answer is the same frame, with the values this end cannot have
 * lowered to what it can, which the negotiation allows. */
static void v42_handle_xid(v42_protocol *p, const u8 *rxFrame, unsigned size)
{
	u8 echo[3 + V42_MAX_INFO + 64];
	u32 maxCodeWords = V42B_MIN_TOTAL_CODEWORDS;
	u32 maxStringLength = V42B_DEF_STRING_SIZE;
	unsigned echoSize;
	unsigned i;
	p->compressionEnabled = false;

	/* format identifier */
	if (size < 3 || rxFrame[2] != 0x82)
	{
		WARN_LOG(MODEM, "Unexpected XID format: %02x\n", size < 3 ? 0 : rxFrame[2]);
		return;
	}
	if (size > sizeof(echo))
		size = sizeof(echo);
	memcpy(echo, rxFrame, size);
	echoSize = size;

	/* Iterate over parameter groups */
	for (i = 3; i + 3 <= size; )
	{
		const u8 gid = echo[i];
		unsigned groupSize, end, j;
		if (gid == GID_USER_DATA)
		{
			/* not answered */
			echoSize = i;
			break;
		}
		groupSize = echo[i + 1] * 256 + echo[i + 2];
		end = i + 3 + groupSize < size ? i + 3 + groupSize : size;
		if (gid != GID_PARAM && gid != GID_PRIVATE)
		{
			INFO_LOG(MODEM, "Unexpected XID GI: %02x\n", gid);
			i = end;
			continue;
		}
		/* Parameters */
		for (j = i + 3; j + 2 <= end; )
		{
			const u8 paramId = echo[j];
			const u8 paramSize = echo[j + 1];
			u8 *pv = &echo[j + 2];
			bool small;
			u32 v;
			if (j + 2 + paramSize > end)
				break;
			small = paramSize <= 4;
			v = small ? xidValue(pv, paramSize) : 0;
			if (!small)
				WARN_LOG(MODEM, "Unexpected param length for PI %d: %x\n", paramId, paramSize);
			switch (paramId)
			{
			case 0:	/* (private) Parameter set identification */
				break;
			case 1:	/* (private) Data compression request */
				if (gid == GID_PRIVATE && paramSize >= 1)
				{
					DEBUG_LOG(MODEM, "xid: Data compression request %x\n", pv[0]);
					if (pv[0] == 3)
						p->compressionEnabled = true;
					else
						/* Disable all compression if only one direction is compressed (not used anyway) */
						pv[0] = 0;
				}
				break;
			case 2:	/* (private) Number of codewords */
				if (gid == GID_PRIVATE && small)
				{
					DEBUG_LOG(MODEM, "xid: Number of codewords %d\n", v);
					maxCodeWords = v;
					if (v > V42B_MAX_TOTAL_CODEWORDS)
					{
						maxCodeWords = V42B_MAX_TOTAL_CODEWORDS;
						xidSetValue(pv, paramSize, maxCodeWords);
					}
				}
				break;
			case 3:
				if (gid == GID_PARAM && small)
				{
					/* HDLC optional function (public) */
					DEBUG_LOG(MODEM, "xid: HDLC optional function %x\n", v);
				}
				else if (gid == GID_PRIVATE && small)
				{
					/* (private) Maximum string length */
					DEBUG_LOG(MODEM, "xid: Maximum string length %d\n", v);
					maxStringLength = v;
					if (v > (u32)V42B_MAX_STRING_SIZE)
					{
						maxStringLength = V42B_MAX_STRING_SIZE;
						xidSetValue(pv, paramSize, maxStringLength);
					}
				}
				break;
			case 5:	/* Maximum length of information field: tx */
			case 6:	/* Maximum length of information field: rx */
				if (gid == GID_PARAM && small)
				{
					u32 bits = v;
					DEBUG_LOG(MODEM, "xid: Max %s len %d bits\n", paramId == 5 ? "tx" : "rx", v);
					if (bits > V42_MAX_INFO * 8)
					{
						bits = V42_MAX_INFO * 8;
						xidSetValue(pv, paramSize, bits);
					}
					if (paramId == 5 && bits >= 8)
						p->txMaxSize = bits / 8;
				}
				break;
			case 7:	/* Window size: tx */
			case 8:	/* Window size: rx */
				if (gid == GID_PARAM && small)
				{
					u32 k = v;
					DEBUG_LOG(MODEM, "xid: %s window %d\n", paramId == 7 ? "Tx" : "Rx", v);
					if (k > 15)
					{
						k = 15;
						xidSetValue(pv, paramSize, k);
					}
					if (paramId == 7 && k >= 1)
						p->txWindow = (int)k;
				}
				break;
			default:
				WARN_LOG(MODEM, "Unexpected PI %d (size %x, group id %x)\n", paramId, paramSize, gid);
				break;
			}
			j += 2 + paramSize;
		}
		i = end;
	}
	v42_send_frame(p, echo, echoSize);

	if (p->compressionEnabled)
	{
		NOTICE_LOG(MODEM, "V.42bis compression enabled: max dictionary size %d\n", maxCodeWords);
		v42b_compressor_set_params(&p->compressor, maxCodeWords, maxStringLength);
		v42b_compressor_reset(&p->compressor);
		v42b_decompressor_set_params(&p->decompressor, maxCodeWords, maxStringLength);
		v42b_decompressor_reset(&p->decompressor);
	}
}

static void v42_handle_reject(v42_protocol *p, u8 control2)
{
	const u8 seq = control2 >> 1;
	INFO_LOG(MODEM, "Received REJECT %d\n", seq);
	/* The frames from N(R) on are sent again, one at a time as the line has room */
	if (seqDistance(p->txSeqAck, seq) <= seqDistance(p->txSeqAck, p->txSeqNum) && seq != p->txSeqNum)
	{
		p->resendSeq = seq;
		p->resending = true;
	}
}

static void v42_handle_frame(v42_protocol *p)
{
	const u8 *rxFrame;
	unsigned size;
	if (!hdlc_decoder_frame_available(&p->hdlcDecoder))
		return;
	hdlc_decoder_frame_taken(&p->hdlcDecoder);
	rxFrame = hdlc_decoder_frame(&p->hdlcDecoder);
	size = hdlc_decoder_frame_size(&p->hdlcDecoder);
	if (size < 2)
	{
		WARN_LOG(MODEM, "Invalid frame: %d bytes\n", size);
		return;
	}
	if ((rxFrame[0] & 1) == 0)
	{
		WARN_LOG(MODEM, "Invalid frame: Unexpected extended address\n");
		return;
	}
	if ((rxFrame[0] >> 2) != 0)
	{
		WARN_LOG(MODEM, "Invalid frame: Unknown address %02x\n", rxFrame[0] >> 2);
		return;
	}

	if ((rxFrame[1] & 3) != 3)
	{
		/* Information and Supervisory frames have a second control byte */
		if (size < 3)
		{
			WARN_LOG(MODEM, "Invalid frame: control %02x and %d bytes\n", rxFrame[1], size);
			return;
		}
		/* and they acknowledge sent frames (except SREJ) */
		if (rxFrame[1] != 0xd)
			v42_ack_iframe(p, rxFrame[2] >> 1);
	}
	p->inactivityStart = now();

	switch (rxFrame[1] & 0xef)
	{
	case 0x01:	/* RR */
		if (!p->receiverReady)
			NOTICE_LOG(MODEM, "Received RR\n");
		p->receiverReady = true;
		break;
	case 0x05:	/* RNR */
		if (p->receiverReady)
			WARN_LOG(MODEM, "Received RNR\n");
		p->receiverReady = false;
		break;
	case 0x09:	/* REJECT */
		p->receiverReady = true;
		v42_handle_reject(p, rxFrame[2]);
		break;
	case 0x0d:	/* SREJ */
		WARN_LOG(MODEM, "Received SREJ\n");
		break;
	case 0x6f:	/* SABME */
		v42_handle_sabme(p, rxFrame[0], rxFrame[1]);
		break;
	case 0x0f:	/* DM */
		break;
	case 0x03:	/* UI */
		WARN_LOG(MODEM, "Received UI\n");
		break;
	case 0x43:	/* DISC */
		v42_handle_disc(p, rxFrame[0]);
		break;
	case 0x63:	/* UA */
		p->receiverReady = true;
		break;
	case 0x87:	/* FRMR */
		WARN_LOG(MODEM, "Received FRMR\n");
		break;
	case 0xaf:	/* XID */
		v42_handle_xid(p, rxFrame, size);
		break;
	case 0xe3:	/* TEST */
		break;
	default:
		if (rxFrame[1] & 1)
		{
			WARN_LOG(MODEM, "Invalid HDLC command: %02x\n", rxFrame[1]);
			return;
		}
		/* I-frame */
		if (p->phase != PHASE_CONNECTED)
		{
			WARN_LOG(MODEM, "I-frame received but not connected\n");
			return;
		}
		v42_handle_iframe(p, rxFrame, size);
		break;
	}
}

static void v42_send_iframe(v42_protocol *p)
{
	u8 *frame;
	unsigned len = 3;
	if (!p->receiverReady || p->in->available(p->in->ctx) == 0)
		return;
	/* check that tx window isn't reached */
	if ((int)seqDistance(p->txSeqAck, p->txSeqNum) >= p->txWindow)
		return;

	frame = p->sentFrames[p->txSeqNum & 15];
	frame[0] = 1;
	frame[1] = (u8)(p->txSeqNum << 1);
	frame[2] = (u8)(p->rxSeqNum << 1);
	if (p->compressionEnabled)
	{
		while (v42b_compressor_available(&p->compressor) < (int)p->txMaxSize)
		{
			const int c = p->in->read(p->in->ctx);
			if (c == -1)
				break;
			v42b_compressor_write(&p->compressor, (u8)c);
		}
		v42b_compressor_flush(&p->compressor);
		while (len - 3 < p->txMaxSize)
		{
			const int cc = v42b_compressor_read(&p->compressor);
			if (cc == -1)
				break;
			frame[len++] = cc & 0xff;
		}
	}
	else
	{
		while (len - 3 < p->txMaxSize)
		{
			const int c = p->in->read(p->in->ctx);
			if (c == -1)
				break;
			frame[len++] = c & 0xff;
		}
	}
	if (len == 3)
		/* don't send an empty frame */
		return;
	p->sentSize[p->txSeqNum & 15] = (u16)len;
	p->txSeqNum = incMod128(p->txSeqNum);
	v42_send_frame(p, frame, len);
}

/* V14 mode: the characters from the network, as many as the encoder needs */
static void v42_feed_v14(v42_protocol *p)
{
	while (v14_encoder_available(&p->v14Encoder) < 64)
	{
		const int c = p->in->read(p->in->ctx);
		if (c == -1)
			break;
		v14_encoder_write(&p->v14Encoder, (u8)c);
	}
}

int v42_read(v42_protocol *p)
{
	if (p->phase == PHASE_NONE || p->phase == PHASE_DETECTION)
	{
		v14_encoder_flush(&p->v14Encoder);
		return v14_encoder_read(&p->v14Encoder);
	}
	else if (p->phase == PHASE_V14)
	{
		v42_feed_v14(p);
		if (v14_encoder_available(&p->v14Encoder) == 0)
			v14_encoder_flush(&p->v14Encoder);
		return v14_encoder_read(&p->v14Encoder);
	}
	if (hdlc_encoder_available(&p->hdlcEncoder) == 0)
	{
		if (p->phase == PHASE_CONNECTED)
		{
			/* Frames rejected by the other end first */
			if (p->resending && seqDistance(p->txSeqAck, p->resendSeq) > seqDistance(p->txSeqAck, p->txSeqNum))
				/* acknowledged in the meantime */
				p->resendSeq = p->txSeqAck;
			while (p->resending && hdlc_encoder_available(&p->hdlcEncoder) == 0)
			{
				unsigned slot;
				if (p->resendSeq == p->txSeqNum)
				{
					p->resending = false;
					break;
				}
				slot = p->resendSeq & 15;
				if (p->sentSize[slot] != 0 && (p->sentFrames[slot][1] >> 1) == p->resendSeq)
				{
					p->sentFrames[slot][2] = (u8)(p->rxSeqNum << 1);
					v42_send_frame(p, p->sentFrames[slot], p->sentSize[slot]);
				}
				else
					WARN_LOG(MODEM, "REJ frame %d not found\n", p->resendSeq);
				p->resendSeq = incMod128(p->resendSeq);
			}
			if (hdlc_encoder_available(&p->hdlcEncoder) == 0)
				v42_send_iframe(p);
			if (hdlc_encoder_available(&p->hdlcEncoder) == 0 && now() - p->inactivityStart >= T403_CYCLES)
			{
				/* send an RR frame with P bit set */
				u8 rr[3];
				rr[0] = 1;
				rr[1] = 1;
				rr[2] = (u8)((p->rxSeqNum << 1) | 1);
				v42_send_frame(p, rr, sizeof(rr));
			}
		}
		if (hdlc_encoder_available(&p->hdlcEncoder) == 0)
			v42_send_flag(p);
	}
	return hdlc_encoder_read(&p->hdlcEncoder);
}

void v42_write(v42_protocol *p, u8 v)
{
	if (p->phase == PHASE_NONE || p->phase == PHASE_DETECTION)
	{
		if (now() - p->detectionStart >= T400_CYCLES)
		{
			INFO_LOG(MODEM, "Switching to V.14 mode\n");
			p->phase = PHASE_V14;
		}
		else
		{
			int c;
			v14_decoder_write(&p->v14Decoder, v);
			c = v14_decoder_read(&p->v14Decoder);
			if (c == -1)
				return;
			v = (u8)c;
		}
	}
	if (p->phase == PHASE_NONE || p->phase == PHASE_DETECTION)
	{
		/* V.42 ODP: DC1 with alternating parity followed by 8+1 or 16+1 ones */
		bool dc1 = false;
		if (v == 0x11 || v == 0x91)
		{
			const int stopBits = v14_decoder_received_stop_bits(&p->v14Decoder);
			dc1 = stopBits == 9 || stopBits == 17;
		}
		if (p->phase == PHASE_NONE)
		{
			if (dc1)
			{
				p->phase = PHASE_DETECTION;
				p->lastRx = v;
				p->odpCount = 1;
			}
		}
		else
		{
			if (dc1 && v != p->lastRx)
			{
				p->odpCount++;
				if (p->odpCount == 4)
				{
					p->odpCount = 0;
					/* send ADP */
					v14_encoder_set_stop_bits(&p->v14Encoder, 9);
					v14_encoder_write(&p->v14Encoder, 'E');
					v14_encoder_write(&p->v14Encoder, 'C');
					/* (a 0 after them switches to V.14 only and starts the PPP negotiation) */
					v14_encoder_set_stop_bits(&p->v14Encoder, 1);
				}
			}
			else
			{
				p->odpCount = 0;
				if (v == 0x7e)
					p->phase = PHASE_ESTABLISH;
			}
			p->lastRx = v;
		}
	}
	else if (p->phase == PHASE_ESTABLISH || p->phase == PHASE_CONNECTED)
	{
		hdlc_decoder_write(&p->hdlcDecoder, v);
		v42_handle_frame(p);
	}
	else if (p->phase == PHASE_V14)
	{
		int c;
		v14_decoder_write(&p->v14Decoder, v);
		for (c = v14_decoder_read(&p->v14Decoder); c != -1; c = v14_decoder_read(&p->v14Decoder))
			p->out->write(p->out->ctx, (u8)c);
	}
}

static void v42_reset_at(v42_protocol *p, u64 time)
{
	p->phase = PHASE_NONE;
	p->lastRx = 0;
	p->odpCount = 0;
	v14_encoder_reset(&p->v14Encoder);
	v14_decoder_reset(&p->v14Decoder);
	hdlc_encoder_reset(&p->hdlcEncoder);
	hdlc_decoder_reset(&p->hdlcDecoder);
	p->txSeqNum = 0;
	p->rxSeqNum = 0;
	p->txSeqAck = 0;
	p->receiverReady = true;
	p->txMaxSize = 128;
	p->txWindow = 15;
	memset(p->sentFrames, 0, sizeof(p->sentFrames));
	memset(p->sentSize, 0, sizeof(p->sentSize));
	p->resendSeq = 0;
	p->resending = false;
	p->detectionStart = time;
	p->inactivityStart = p->detectionStart;
	p->compressionEnabled = false;
	v42b_compressor_set_params(&p->compressor, V42B_MIN_TOTAL_CODEWORDS, V42B_DEF_STRING_SIZE);
	v42b_compressor_reset(&p->compressor);
	v42b_decompressor_set_params(&p->decompressor, V42B_MIN_TOTAL_CODEWORDS, V42B_DEF_STRING_SIZE);
	v42b_decompressor_reset(&p->decompressor);
}

void v42_init(v42_protocol *p, const modem_stream *in, const modem_stream *out)
{
	p->in = in;
	p->out = out;
	v14_encoder_init(&p->v14Encoder);
	v14_decoder_init(&p->v14Decoder);
	hdlc_encoder_init(&p->hdlcEncoder, p->hdlcEncoderBuf, sizeof(p->hdlcEncoderBuf));
	hdlc_decoder_init(&p->hdlcDecoder, p->hdlcDecoderFrame, sizeof(p->hdlcDecoderFrame));
	v42b_compressor_init(&p->compressor);
	v42b_decompressor_init(&p->decompressor);
	v42_reset_at(p, 0);
}

void v42_reset(v42_protocol *p)
{
	v42_reset_at(p, now());
}

void v42_serialize(const v42_protocol *p, void **data, unsigned int *total_size)
{
	u8 b[11];
	u16 maxSize = (u16)p->txMaxSize;

	b[0] = (u8)p->phase;
	b[1] = p->lastRx;
	b[2] = (u8)p->odpCount;
	b[3] = p->txSeqNum;
	b[4] = p->rxSeqNum;
	b[5] = p->txSeqAck;
	b[6] = (u8)p->receiverReady;
	b[7] = (u8)p->txWindow;
	b[8] = p->resendSeq;
	b[9] = (u8)p->resending;
	b[10] = (u8)p->compressionEnabled;
	LIBRETRO_SA(b, 11);
	LIBRETRO_S(maxSize);
	LIBRETRO_S(p->detectionStart);
	LIBRETRO_S(p->inactivityStart);
	LIBRETRO_SA(&p->sentSize[0], 16);
	LIBRETRO_SA(&p->sentFrames[0][0], sizeof(p->sentFrames));
	v14_encoder_serialize(&p->v14Encoder, data, total_size);
	v14_decoder_serialize(&p->v14Decoder, data, total_size);
	hdlc_encoder_serialize(&p->hdlcEncoder, data, total_size);
	hdlc_decoder_serialize(&p->hdlcDecoder, data, total_size);
	v42b_compressor_serialize(&p->compressor, data, total_size);
	v42b_decompressor_serialize(&p->decompressor, data, total_size);
}

void v42_unserialize(v42_protocol *p, void **data, unsigned int *total_size)
{
	u8 b[11];
	u16 maxSize = 128;
	int i;

	memset(b, 0, sizeof(b));
	LIBRETRO_USA(b, 11);
	LIBRETRO_US(maxSize);
	LIBRETRO_US(p->detectionStart);
	LIBRETRO_US(p->inactivityStart);
	LIBRETRO_USA(&p->sentSize[0], 16);
	LIBRETRO_USA(&p->sentFrames[0][0], sizeof(p->sentFrames));
	v14_encoder_unserialize(&p->v14Encoder, data, total_size);
	v14_decoder_unserialize(&p->v14Decoder, data, total_size);
	hdlc_encoder_unserialize(&p->hdlcEncoder, data, total_size);
	hdlc_decoder_unserialize(&p->hdlcDecoder, data, total_size);
	v42b_compressor_unserialize(&p->compressor, data, total_size);
	v42b_decompressor_unserialize(&p->decompressor, data, total_size);

	p->phase = b[0] <= PHASE_V14 ? b[0] : PHASE_NONE;
	p->lastRx = b[1];
	p->odpCount = b[2] & 3;
	p->txSeqNum = b[3] & 127;
	p->rxSeqNum = b[4] & 127;
	p->txSeqAck = b[5] & 127;
	p->receiverReady = b[6] != 0;
	p->txWindow = b[7] >= 1 && b[7] <= 15 ? b[7] : 15;
	p->resendSeq = b[8] & 127;
	p->resending = b[9] != 0;
	p->compressionEnabled = b[10] != 0;
	p->txMaxSize = maxSize >= 1 && maxSize <= V42_MAX_INFO ? maxSize : 128;
	if ((int)seqDistance(p->txSeqAck, p->txSeqNum) > p->txWindow)
		p->txSeqAck = p->txSeqNum;
	for (i = 0; i < 16; i++)
		if (p->sentSize[i] > 3 + V42_MAX_INFO)
			p->sentSize[i] = 0;
}

/*
 * V.8 bis
 */
void v8bis_init(v8bis_protocol *p)
{
	hdlc_encoder_init(&p->encoder, p->encoderBuf, sizeof(p->encoderBuf));
	hdlc_decoder_init(&p->decoder, p->decoderFrame, sizeof(p->decoderFrame));
	v8bis_reset(p);
}

u8 v8bis_detect_tone(v8bis_protocol *p)
{
	switch (p->toneState)
	{
	case 0:
		if (p->toneTime == 0)
			p->toneTime = now();
		else if (now() - p->toneTime >= SH4_MAIN_CLOCK / 1000 * 400)
		{
			p->toneState++;
			p->toneTime = now();
		}
		break;

	case 1: /* Segment 1 dual tone received and single tone segment 2 is being detected */
		if (now() - p->toneTime >= SH4_MAIN_CLOCK / 1000 * 100)
		{
			p->toneState++;
			p->toneTime = now();
		}
		return 0xE0;

	case 2:
		return 0x20; /* Mode Request (MRe) received */
	}
	return 0;
}

static void v8bis_send_cl(v8bis_protocol *p)
{
	static const u8 cl[] = {
			0x12,	/* CL, rev.1 (0x13: CLR, rev.1) */
					/* identification field (I) */
			0xc3,	/* NPar1: V.8, short V.8, non-standard field */
			0x80,	/* SPar1: network type not specified */
					/* standard information field (S) */
			0x80,	/* NPar1 */
			0x01,	/* SPar2[1]: data */
			0x80,	/* SPar2[2] */
			  0x0F,	/* NPar2[1]: transparent data, V.42, V.42 bis, V.14 */
			  0x30,	/* NPar2[2]: V.34, V.32 bis */
			  0xFF,	/* NPar2[3]: V.32, V.22 bis, V.22, V.21, V.90 digital+ana
					 * (V.8 bis rev.2: NPar2[4] 0xC0) */
					/* non-standard information (NS) */
			0x09,	/* size */
			0xb5,	/* country code (USA) */
			0x02,	/* manuf. code len */
			0x00,	/* manufacturer code (K56flex) */
			0x94,
			0x81,	/* licensee code (Rockwell) */
			0x83,	/* capabilities (K56flex, V90?) */
			0x43,	/* K56flex version number */
			0x47,	/* Rockwell MDP version number */
			0xC4	/* u-law (none) and controller version number (4) */
	};
	p->dataMode = true;
	hdlc_encoder_send_flag(&p->encoder);
	hdlc_encoder_send_frame(&p->encoder, cl, sizeof(cl));
	hdlc_encoder_send_flag(&p->encoder);
	hdlc_encoder_send_flag(&p->encoder);
}

static void v8bis_send_one(v8bis_protocol *p, u8 b)
{
	hdlc_encoder_send_flag(&p->encoder);
	hdlc_encoder_send_frame(&p->encoder, &b, 1);
	hdlc_encoder_send_flag(&p->encoder);
	hdlc_encoder_send_flag(&p->encoder);
}

/* ACK(@n): 0x14 is ACK(1), 0x15 ACK(2) */
static void v8bis_send_ack(v8bis_protocol *p, int n)
{
	v8bis_send_one(p, (u8)(0x13 + n));
}

/* NAK(@n): 8 is NAK(1), 9 NAK(2)... */
static void v8bis_send_nak(v8bis_protocol *p, int n)
{
	v8bis_send_one(p, (u8)(7 + n));
}

void v8bis_emit_tone(v8bis_protocol *p, u8 tone)
{
	if (p->toneState == 2 && tone == 0x33) /* emit Capabilities Request (CRd) */
		/* should send CL or CLR */
		v8bis_send_cl(p);
}

int v8bis_read(v8bis_protocol *p)
{
	if (!p->dataMode)
		return -1;
	return hdlc_encoder_read(&p->encoder);
}

int v8bis_available(v8bis_protocol *p)
{
	if (!p->dataMode)
		return 0;
	return hdlc_encoder_available(&p->encoder);
}

bool v8bis_completed(v8bis_protocol *p)
{
	return v8bis_available(p) == 0 && p->done;
}

/* The fields of a V.8 bis message are runs of octets, the last of which
 * has its top bit set (the delimiter): the length of the one at @p,
 * delimiter included, as far as @end */
static unsigned paramSize(const u8 *p, const u8 *end)
{
	const u8 *begin = p;
	while (p < end && (*p & 0x80) == 0)
		p++;
	return (unsigned)(p - begin + 1);
}

static void v8bis_handle_ms(v8bis_protocol *p, const u8 *frame, unsigned size)
{
	const u8 *end = frame + size;
	const u8 *q;
	bool v8, shortV8, txAck, hasNSF, v42, v42bis, v14;
	unsigned sz;
	u8 spar1;
	int speed = 0;
	/* the octet at @q, 0 past the end of the message */
#define AT(q) ((q) < end ? *(q) : 0)
	/* identification field (I) */
	/* NPar1 */
	q = &frame[1];
	v8 = (AT(q) & 1) != 0;
	shortV8 = (AT(q) & 2) != 0;
	txAck = (AT(q) & 8) != 0;
	hasNSF = (AT(q) & 0x40) != 0;
	sz = paramSize(q, end);
	q += sz;
	sz = paramSize(q, end);
	if (AT(q) & 0x7f)
	{
		q += sz;
		q += paramSize(q, end); /* NPar2's */
	}
	else
		q += sz;
	/* standard info field (S) */
	q += paramSize(q, end); /* NPar1 */
	sz = paramSize(q, end);
	spar1 = AT(q);
	if ((spar1 & 1) == 0)
	{
		WARN_LOG(MODEM, "V.8 bis MS: Data bit not set in field S - SPar1\n");
		v8bis_send_nak(p, 1);
		return;
	}
	q += sz;
	v42 = (AT(q) & 2) != 0;
	v42bis = (AT(q) & 4) != 0;
	v14 = (AT(q) & 8) != 0;
	if ((AT(q) & 0xc0) == 0)
	{
		++q;
		if (AT(q) & 0x10)
			/* V34 */
			speed = 33600;
		else if (AT(q) & 0x20)
			/* V32 bis */
			speed = 14400;
		if (speed == 0 && (AT(q) & 0xc0) == 0)
		{
			++q;
			if (AT(q) & 1)
				/* V32 */
				speed = 9600;
			else if (AT(q) & 2)
				/* V22 bis */
				speed = 2400;
			else if (AT(q) & 4)
				/* V22 */
				speed = 1200;
			else if (AT(q) & 8)
				/* V21 */
				speed = 300;
		}
	}
	q += paramSize(q, end);
	if (hasNSF && AT(q) >= 5 && AT(q + 2) == 2)
	{
		if (AT(q + 3) == 0 && AT(q + 4) == 0x94)
			/* K56flex / V90 */
			speed = 56000;
	}
#undef AT
	if (txAck)
		v8bis_send_ack(p, 1);
	DEBUG_LOG(MODEM, "V8bis: received MS: V8 %d sV8 %d V14 %d V42 %d V42bis %d speed %d\n",
			v8, shortV8, v14, v42, v42bis, speed);
	(void)v8;
	(void)shortV8;
	(void)v14;
	(void)v42;
	(void)v42bis;
}

void v8bis_write(v8bis_protocol *p, u8 v)
{
	const u8 *frame;
	unsigned size;
	u8 frameType;
	if (!p->dataMode)
		return;

	hdlc_decoder_write(&p->decoder, v);
	if (!hdlc_decoder_frame_available(&p->decoder))
		return;
	hdlc_decoder_frame_taken(&p->decoder);

	frame = hdlc_decoder_frame(&p->decoder);
	size = hdlc_decoder_frame_size(&p->decoder);
	frameType = frame[0] & 0xf;
	switch (frameType)
	{
	case 1: /* MS */
		v8bis_handle_ms(p, frame, size);
		break;
	default:
		WARN_LOG(MODEM, "Unhandled V.8 bis frame type %d\n", frameType);
		break;
	}
	p->done = true;
}

void v8bis_reset(v8bis_protocol *p)
{
	hdlc_encoder_reset(&p->encoder);
	hdlc_decoder_reset(&p->decoder);
	p->dataMode = false;
	p->toneState = 0;
	p->toneTime = 0;
	p->done = false;
}

void v8bis_serialize(const v8bis_protocol *p, void **data, unsigned int *total_size)
{
	u8 b[3];

	b[0] = (u8)p->toneState;
	b[1] = (u8)p->dataMode;
	b[2] = (u8)p->done;
	hdlc_encoder_serialize(&p->encoder, data, total_size);
	hdlc_decoder_serialize(&p->decoder, data, total_size);
	LIBRETRO_SA(b, 3);
	LIBRETRO_S(p->toneTime);
}

void v8bis_unserialize(v8bis_protocol *p, void **data, unsigned int *total_size)
{
	u8 b[3];

	memset(b, 0, sizeof(b));
	hdlc_encoder_unserialize(&p->encoder, data, total_size);
	hdlc_decoder_unserialize(&p->decoder, data, total_size);
	LIBRETRO_USA(b, 3);
	LIBRETRO_US(p->toneTime);
	p->toneState = b[0] <= 2 ? b[0] : 0;
	p->dataMode = b[1] != 0;
	p->done = b[2] != 0;
}
