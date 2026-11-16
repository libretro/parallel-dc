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
#ifndef MODEM_V42_H
#define MODEM_V42_H
#include "types.h"
#include "internal.h"
#include "v42bis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The far end of a synchronous modem connection: V.14 (characters framed
 * by start and stop bits over the synchronous line), V.42 error correction
 * (LAPM: HDLC frames) and V.42 bis compression, and the V.8 bis exchange
 * some games start a call with. All of it has a fixed size, so that it can
 * be in a save state, and all that the other end sends is checked against
 * those sizes. Each part is set up once (its _init()) before it is used. */

/* Characters, to bits framed by start and stop bits */
typedef struct v14_encoder
{
	byte_fifo buffer;
	u8 buf[512];
	u8 txCurByte;
	u32 txBitCount;
	int txStopBits;
} v14_encoder;

void v14_encoder_init(v14_encoder *e);
void v14_encoder_write(v14_encoder *e, u8 v);
void v14_encoder_flush(v14_encoder *e);
void v14_encoder_reset(v14_encoder *e);
int v14_encoder_read(v14_encoder *e);
int v14_encoder_available(const v14_encoder *e);
void v14_encoder_set_stop_bits(v14_encoder *e, int stopBits);
void v14_encoder_serialize(const v14_encoder *e, void **data, unsigned int *total_size);
void v14_encoder_unserialize(v14_encoder *e, void **data, unsigned int *total_size);

/* ...and back */
typedef struct v14_decoder
{
	byte_fifo buffer;
	u8 buf[16];
	u8 rxCurByte;
	int rxState;
	u32 rxBitCount;
	int charsSinceMissingStop;
	int rxStopBits;
	int maxStopBits;
} v14_decoder;

void v14_decoder_init(v14_decoder *d);
void v14_decoder_write(v14_decoder *d, u8 v);
int v14_decoder_read(v14_decoder *d);
/* The most stop bits seen since this was last asked */
int v14_decoder_received_stop_bits(v14_decoder *d);
void v14_decoder_reset(v14_decoder *d);
void v14_decoder_serialize(const v14_decoder *d, void **data, unsigned int *total_size);
void v14_decoder_unserialize(v14_decoder *d, void **data, unsigned int *total_size);

/* The bytes of HDLC frames, as they go on the line: flags, bit stuffing,
 * CRC; in a queue of @size bytes at @buf */
typedef struct hdlc_encoder
{
	byte_fifo buffer;
	u32 txBitBuffer;
	int txBitCount;
} hdlc_encoder;

void hdlc_encoder_init(hdlc_encoder *e, u8 *buf, unsigned size);
/* False, and nothing sent, if there is no room for it */
bool hdlc_encoder_send_frame(hdlc_encoder *e, const u8 *data, unsigned len);
void hdlc_encoder_send_flag(hdlc_encoder *e);
void hdlc_encoder_reset(hdlc_encoder *e);
int hdlc_encoder_read(hdlc_encoder *e);
int hdlc_encoder_available(const hdlc_encoder *e);
void hdlc_encoder_serialize(const hdlc_encoder *e, void **data, unsigned int *total_size);
void hdlc_encoder_unserialize(hdlc_encoder *e, void **data, unsigned int *total_size);

/* The frames in the bits received, into the @size bytes at @frame. A frame
 * longer than that (its CRC included) is dropped. */
typedef struct hdlc_decoder
{
	u8 *rxFrame;
	unsigned size;
	unsigned rxSize;
	bool overflow;
	bool ready;
	unsigned readySize;
	int rxOnes;
	int rxPosition;
	u8 rxCurByte;
} hdlc_decoder;

void hdlc_decoder_init(hdlc_decoder *d, u8 *frame, unsigned size);
void hdlc_decoder_write(hdlc_decoder *d, u8 v);
/* A whole frame has been received: its bytes, without the CRC, until it
 * is taken */
bool hdlc_decoder_frame_available(const hdlc_decoder *d);
const u8 *hdlc_decoder_frame(const hdlc_decoder *d);
unsigned hdlc_decoder_frame_size(const hdlc_decoder *d);
void hdlc_decoder_frame_taken(hdlc_decoder *d);
void hdlc_decoder_reset(hdlc_decoder *d);
/* (a frame that has been received is always taken before the state is saved) */
void hdlc_decoder_serialize(const hdlc_decoder *d, void **data, unsigned int *total_size);
void hdlc_decoder_unserialize(hdlc_decoder *d, void **data, unsigned int *total_size);

/* The longest information field of an I frame either way (N401) */
#define V42_MAX_INFO 256

/* V.42 between the game's side of the line (read from and written to as a
 * stream) and @in, @out: what is sent and what is received */
typedef struct v42_protocol
{
	const modem_stream *in;
	const modem_stream *out;
	int phase;
	u8 lastRx;
	int odpCount;
	v14_encoder v14Encoder;
	v14_decoder v14Decoder;
	/* for V42 mode */
	hdlc_encoder hdlcEncoder;
	u8 hdlcEncoderBuf[2048];
	hdlc_decoder hdlcDecoder;
	u8 hdlcDecoderFrame[V42_MAX_INFO + 5 + 64];
	u8 txSeqNum;
	u8 rxSeqNum;
	u8 txSeqAck;
	bool receiverReady;
	unsigned txMaxSize;
	int txWindow;
	/* The I frames sent and not acknowledged yet, by their number (the
	 * window is 15 at most) */
	u8 sentFrames[16][3 + V42_MAX_INFO];
	u16 sentSize[16];
	/* The next frame to send again after a REJ, until txSeqNum */
	u8 resendSeq;
	bool resending;
	/* V42 bis */
	v42b_compressor compressor;
	v42b_decompressor decompressor;
	bool compressionEnabled;
	u64 detectionStart;		/* Detection phase timer (T400) */
	u64 inactivityStart;	/* Inactivity timer (T403) */
} v42_protocol;

/* Set up, and reset at time 0 */
void v42_init(v42_protocol *p, const modem_stream *in, const modem_stream *out);
int v42_read(v42_protocol *p);
void v42_write(v42_protocol *p, u8 v);
void v42_reset(v42_protocol *p);
void v42_serialize(const v42_protocol *p, void **data, unsigned int *total_size);
void v42_unserialize(v42_protocol *p, void **data, unsigned int *total_size);

/* V.8 bis is used by older browsers (Planet Web 1.125, DreamKey 1.0/1.5 and
 * the many games that include them) and Windows CE games. To enable 56k, a
 * non-standard info field must be returned indicating K56flex/V90 support. */
typedef struct v8bis_protocol
{
	hdlc_encoder encoder;
	u8 encoderBuf[256];
	hdlc_decoder decoder;
	u8 decoderFrame[128];
	int toneState;
	u64 toneTime;
	bool dataMode;
	bool done;
} v8bis_protocol;

/* Set up, and reset */
void v8bis_init(v8bis_protocol *p);
void v8bis_emit_tone(v8bis_protocol *p, u8 tone);
u8 v8bis_detect_tone(v8bis_protocol *p);
int v8bis_read(v8bis_protocol *p);
int v8bis_available(v8bis_protocol *p);
void v8bis_write(v8bis_protocol *p, u8 v);
void v8bis_reset(v8bis_protocol *p);
bool v8bis_completed(v8bis_protocol *p);
void v8bis_serialize(const v8bis_protocol *p, void **data, unsigned int *total_size);
void v8bis_unserialize(v8bis_protocol *p, void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif

#endif
