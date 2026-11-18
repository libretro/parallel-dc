/*
	The devices an arcade game has besides its I/O board: card readers,
	touch panel, hopper (naomi.c, card_reader.c, touchscreen.c,
	hopper.c, and the IC card reader in maple_devs.c).
*/
#pragma once
#include "types.h"
#include "hw/serial_port.h"
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The bytes a device has for the game, oldest first, in @buf (@size
 * bytes, the device's own); more than that, the rest dropped */
typedef struct periph_queue
{
	u8 *buf;
	u32 size;
	u32 head;
	u32 count;
} periph_queue;

static TYPES_INLINE void periph_queue_init(periph_queue *q, u8 *buf, u32 size)
{
	q->buf = buf;
	q->size = size;
	q->head = q->count = 0;
}
static TYPES_INLINE void periph_queue_clear(periph_queue *q)
{
	q->head = q->count = 0;
}
static TYPES_INLINE void periph_queue_push(periph_queue *q, u8 b)
{
	if (q->count == q->size)
		return;
	q->buf[(q->head + q->count) % q->size] = b;
	q->count++;
}
static TYPES_INLINE u8 periph_queue_pop(periph_queue *q)
{
	u8 b;
	if (q->count == 0)
		return 0;
	b = q->buf[q->head];
	q->head = (q->head + 1) % q->size;
	q->count--;
	return b;
}
/* the count (16 bits), then the bytes oldest first and zeroes after: the
 * same however far round the ring they are */
static TYPES_INLINE void periph_queue_serialize(const periph_queue *q, void **data, unsigned int *total_size)
{
	static const u8 zeroes[64] = { 0 };
	u16 c = (u16)q->count;
	u32 first = q->size - q->head < q->count ? q->size - q->head : q->count;
	u32 left = q->size - q->count;
	LIBRETRO_S(c);
	ra_serialize(q->buf + q->head, first, data, total_size);
	ra_serialize(q->buf, q->count - first, data, total_size);
	while (left > 0)
	{
		u32 n = left < sizeof(zeroes) ? left : (u32)sizeof(zeroes);
		ra_serialize(zeroes, n, data, total_size);
		left -= n;
	}
}
static TYPES_INLINE void periph_queue_unserialize(periph_queue *q, void **data, unsigned int *total_size)
{
	u16 c = 0;
	LIBRETRO_US(c);
	LIBRETRO_USA(q->buf, q->size);
	q->head = 0;
	q->count = c < q->size ? c : q->size;
}

/* The ROM set's name ("clubkrt"), or "" for a game that is not a set */
extern char naomi_set_name[];
/* Core option: the card readers and the barcode reader are plugged in */
extern bool naomi_card_readers_enabled;
/* Whether L is the "Insert Card" button (naomi.c) */
extern bool naomi_card_button;

/* The game's devices made, for the game that is loaded: on a hard reset,
 * before the maple devices are (naomi.c). */
void naomi_periph_init(void);
void naomi_periph_term(void);
/* What a card reader game's I/O board bridge does once each time it is asked */
void naomi_periph_poll(void);
/* Whether a game has a card to put in (the "Insert Card" button) */
bool naomi_periph_has_card_reader(void);
/* Whether the game's maple bus has the IC card readers */
bool naomi_periph_rfid_game(void);

/* A file next to the game's EEPROM in the save directory, named by the
 * game and @suffix. Reading returns how many bytes there were (the rest
 * of @buf is zeroes), 0 when there is no such file. */
size_t periph_load_file(const char *suffix, void *buf, size_t size);
/* the same, made anew from @buf; the writing is done by the save writer */
void periph_save_file(const char *suffix, const void *buf, size_t size, retro_atomic_int_t *failed);
/* Where what the peripherals make up starts from (a new card's number):
 * the time */
u32 periph_seed(void);

/* An event of the scheduler as a save state has it. One that would come
 * later than any device asks for, or whose time has gone by, is due now. */
void periph_sched_restore(int id, s32 start, s32 end);

/* The buttons a player holds (active low) and where they point (maple_cfg.c) */
u32 mcfg_ArcadeKcode(int port);
void mcfg_ArcadeAbsPos(int port, f32 *x, f32 *y);

/* The IC card readers (maple_devs.c) */
void rfid_init(bool game, bool insert);
void insertRfidCard(int playerNum);
const u8 *getRfidCardData(int playerNum);
void rfid_serialize_v24(void **data, unsigned int *total_size);
bool rfid_unserialize_v24(void **data, unsigned int *total_size);
void rfid_state_before_v24(void);
/* The two IC cards as their files have them, 256 bytes (V30, naomi_cards_serialize_v30()) */
void rfid_files_serialize_v30(void **data, unsigned int *total_size);
bool rfid_files_unserialize_v30(void **data, unsigned int *total_size);

/* What the devices have in a save state since V24 (naomi.c) */
void naomi_periph_serialize_v24(void **data, unsigned int *total_size);
bool naomi_periph_unserialize_v24(void **data, unsigned int *total_size);
void naomi_periph_state_before_v24(void);

/* The RS-422 port of the I/O board bridge, which a card reader can be on (jvs_bridge.c) */
void naomi_mie_set_pipe(serial_pipe *pipe);

#ifdef __cplusplus
}
#endif
