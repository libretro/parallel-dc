/* The NAOMI's card readers (core/hw/naomi/card_reader.c), by themselves:
 * Initial D's and Derby Owners Club's two Sanwa readers sent every command
 * they know, each track read and written, with the card in, out and the
 * door open, and packets that are not right; Club Kart's sent each of its
 * own, a read waiting for a card; the barcode readers' card read, from a
 * file and with none; the cards' files; and their part of a save state,
 * written, read back, for another reader and from before. What the
 * readers answer and keep is printed, for card_reader_expected.txt
 * (sh tools/naomi/card_reader_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "hw/serial_port.h"
#include "hw/naomi/card_reader.h"
#include "hw/naomi/naomi_periph.h"
#include "hw/naomi/naomi_cart.h"
#include "hw/sh4/modules/modules.h"

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

settings_t settings;
char naomi_game_id[128];

static u32 hash(const u8 *p, size_t n)
{
	u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* The files: the card's and the barcode's */
static u8 card_file[256];
static size_t card_file_size;
static const char *barcode_file;
size_t periph_load_file(const char *suffix, void *buf, size_t size)
{
	size_t n = 0;
	memset(buf, 0, size);
	if (!strcmp(suffix, ".card"))
	{
		n = card_file_size < size ? card_file_size : size;
		memcpy(buf, card_file, n);
	}
	else if (!strcmp(suffix, ".barcode") && barcode_file != NULL)
	{
		n = strlen(barcode_file) < size ? strlen(barcode_file) : size;
		memcpy(buf, barcode_file, n);
	}
	printf("load %s %u of %u\n", suffix, (unsigned)n, (unsigned)size);
	return n;
}

void periph_save_file(const char *suffix, const void *buf, size_t size, retro_atomic_int_t *failed)
{
	(void)failed;
	printf("save %s: %u bytes %08x\n", suffix, (unsigned)size, hash((const u8 *)buf, size));
	memcpy(card_file, buf, size < sizeof(card_file) ? size : sizeof(card_file));
	card_file_size = size;
}

/* The ports */
static serial_pipe *scif_pipe, *mie_pipe, *pipe_in;
static int updates;
void scif_set_pipe(serial_pipe *pipe) { scif_pipe = pipe; printf("scif %s\n", pipe ? "set" : "unset"); }
void naomi_mie_set_pipe(serial_pipe *pipe) { mie_pipe = pipe; printf("mie %s\n", pipe ? "set" : "unset"); }
void scif_update_status(void) { updates++; }

/* The IC card readers: player 1's card, there or not */
static int rfid;
static u8 rfid_card[128];
void insertRfidCard(int playerNum) { printf("insertRfidCard %d\n", playerNum); }
const u8 *getRfidCardData(int playerNum) { (void)playerNum; return rfid ? rfid_card : NULL; }
void rfid_files_serialize_v30(void **data, unsigned int *total_size)
{
	LIBRETRO_SA(rfid_card, sizeof(rfid_card));
	LIBRETRO_SA(rfid_card, sizeof(rfid_card));
}
bool rfid_files_unserialize_v30(void **data, unsigned int *total_size)
{
	u8 b[256];
	return LIBRETRO_USA(b, sizeof(b));
}

static unsigned int state_limit = 0xffffffff;
bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest) { memcpy(*dest, src, size); *(u8 **)dest += size; }
	*total += size;
	return true;
}
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	if (*total + size > state_limit)
		return false;
	memcpy(dst, *src, size); *(u8 **)src += size; *total += size;
	return true;
}

/* What the reader has for the game */
static void drain(const char *what)
{
	static u8 out[4096];
	int n = 0, i;
	while (pipe_in != NULL && pipe_in->ops->available(pipe_in) > 0 && n < (int)sizeof(out))
		out[n++] = pipe_in->ops->read(pipe_in);
	printf("%s: %d bytes %08x (%d updates):", what, n, hash(out, n), updates);
	for (i = 0; i < n && i < 24; i++)
		printf(" %02x", out[i]);
	printf("\n");
	updates = 0;
}

static void send(const u8 *p, u32 n)
{
	u32 i;
	for (i = 0; i < n; i++)
		pipe_in->ops->write(pipe_in, p[i]);
}

static u8 xor_of(const u8 *p, u32 n)
{
	u8 c = 0;
	u32 i;
	for (i = 0; i < n; i++)
		c ^= p[i];
	return c;
}

/* A Sanwa command: STX, its length, the command, three zeroes, its three
 * parameters, @len bytes of data, ETX, the crc; then ENQ for the answer */
static void sanwa(u8 cmd, u8 p4, u8 p5, u8 p6, const u8 *data, u32 len, const char *what)
{
	static u8 pkt[300];
	u32 n = 0;
	pkt[n++] = 2;
	pkt[n++] = (u8)(len + 9);
	pkt[n++] = cmd;
	pkt[n++] = 0;
	pkt[n++] = 0;
	pkt[n++] = 0;
	pkt[n++] = p4;
	pkt[n++] = p5;
	pkt[n++] = p6;
	if (len != 0)
		memcpy(pkt + n, data, len);
	n += len;
	pkt[n++] = 3;
	pkt[n] = xor_of(pkt + 1, n - 1);
	n++;
	send(pkt, n);
	drain(what);
	pkt[0] = 5;
	send(pkt, 1);
	drain(what);
}

static void insert(const char *what)
{
	card_reader_insert_card(0);
	drain(what);
}

static void state(const char *what)
{
	static u8 buf[8192];
	void *p = buf;
	unsigned int total = 0;
	card_reader_serialize_v24(&p, &total);
	naomi_cards_serialize_v30(&p, &total);
	printf("%s: state %u bytes %08x\n", what, total, hash(buf, total));
	p = buf;
	total = 0;
	printf("%s: read back %d", what, (int)card_reader_unserialize_v24(&p, &total));
	printf(" %d\n", (int)naomi_cards_unserialize_v30(&p, &total));
	/* and a state for another reader, empty, read past: what was read
	 * back is the reader's own, not where that one was read into */
	memset(buf, 0, sizeof(buf));
	buf[0] = buf[2] == 1 ? 2 : 1;
	p = buf;
	total = 0;
	printf("%s: another's %d\n", what, (int)card_reader_unserialize_v24(&p, &total));
	drain(what);
}

/* A state made with @make, read by the reader there is now */
static void state_across(void (*make)(void), void (*now)(void), const char *what)
{
	static u8 buf[8192];
	void *p = buf;
	unsigned int total = 0;
	make();
	insert("the other's card");
	card_reader_serialize_v24(&p, &total);
	naomi_cards_serialize_v30(&p, &total);
	now();
	p = buf;
	total = 0;
	printf("%s: read %d", what, (int)card_reader_unserialize_v24(&p, &total));
	printf(" %d\n", (int)naomi_cards_unserialize_v30(&p, &total));
	drain(what);
	insert("its card");
}

static void sanwa_reader(void (*init)(void), int mie, const char *id)
{
	static u8 track[0x45 * 3];
	static const u8 tracks[] = "0123456x";
	char what[64];
	u32 i;

	strcpy(naomi_game_id, id);
	init();
	pipe_in = mie ? mie_pipe : scif_pipe;
	printf("== %s, %s\n", id, mie ? "mie" : "scif");
	insert("insert, no file");
	sanwa(0x10, 0, 0, 0, NULL, 0, "init");
	sanwa(0x20, 0, 0, 0, NULL, 0, "state");
	sanwa(0x33, '0', '0', '0', NULL, 0, "read, no card");
	sanwa(0xD0, '1', 0, 0, NULL, 0, "door open");
	sanwa(0xB0, 0, 0, 0, NULL, 0, "new card");
	for (i = 0; i < sizeof(track); i++)
		track[i] = (u8)(i * 5 + 3);
	for (i = 0; i < 8; i++)
	{
		track[0] = (u8)i;
		sanwa(0x53, '0', '1', tracks[i], track, i == 6 ? 0x45 * 3 : i >= 3 ? 0x45 * 2 : 0x45, "write");
	}
	for (i = 0; i < 8; i++)
	{
		sprintf(what, "read track %c", tracks[i]);
		sanwa(0x33, '0', '1', tracks[i], NULL, 0, what);
	}
	sanwa(0x33, '2', '1', '0', NULL, 0, "card capture");
	sanwa(0xD0, '1', 0, 0, NULL, 0, "door open");
	sanwa(0x33, '0', '1', '0', NULL, 0, "read, door open");
	sanwa(0xD0, '0', 0, 0, NULL, 0, "door closed");
	sanwa(0x80, 0, 0, 0, NULL, 0, "eject");
	insert("insert from its file");
	sanwa(0x33, '0', '1', '6', NULL, 0, "read");
	sanwa(0x7C, 0, 0, 0, NULL, 0, "print");
	sanwa(0x99, 0, 0, 0, NULL, 0, "unknown");
	/* not right: a bad crc, a bad start, too long */
	{
		static const u8 bad_crc[] = { 2, 9, 0x20, 0, 0, 0, 0, 0, 0, 3, 0x55, 5 };
		static const u8 bad_start[] = { 7, 6, 0x20, 5 };
		static u8 longer[260];
		send(bad_crc, sizeof(bad_crc));
		drain("bad crc");
		send(bad_start, sizeof(bad_start));
		drain("bad start");
		longer[0] = 2;
		longer[1] = 0xff;
		for (i = 2; i < sizeof(longer); i++)
			longer[i] = (u8)i;
		send(longer, sizeof(longer));
		drain("too long");
	}
	/* a command on its way, an answer not read, in a state */
	{
		static const u8 part[] = { 2, 9, 0x20, 0 };
		static const u8 rest[] = { 0, 0, 0, 0, 0, 3, 0x20 ^ 9 ^ 3 };
		static const u8 enq[] = { 5 };
		sanwa(0x20, 0, 0, 0, NULL, 0, "state");
		send(enq, 1);
		send(part, sizeof(part));
		state("state");
		send(rest, sizeof(rest));
		drain("the command finished");
		send(enq, 1);
		drain("and answered");
	}
	card_reader_state_before_v24();
	drain("state before V24");
	insert("insert after it");
	card_reader_term();
}

static void clubk(const char *cmd, const u8 *data, u32 len, const char *what)
{
	static u8 pkt[100];
	u32 n = 0;
	pkt[n++] = 2;
	pkt[n++] = (u8)cmd[0];
	pkt[n++] = (u8)cmd[1];
	if (cmd[2] != 0)
		pkt[n++] = (u8)cmd[2];
	if (len != 0)
		memcpy(pkt + n, data, len);
	n += len;
	pkt[n++] = 3;
	pkt[n] = xor_of(pkt + 1, n - 1);
	n++;
	send(pkt, n);
	drain(what);
}

static void clubkart(void)
{
	static u8 card[0x45];
	u32 i;

	strcpy(naomi_game_id, "CLUB KART IN JAPAN");
	card_reader_clubk_init();
	pipe_in = scif_pipe;
	printf("== Club Kart\n");
	clubk("IN", NULL, 0, "init");
	clubk("SS", NULL, 0, "status");
	clubk("RL", NULL, 0, "read, no card");
	insert("insert, waited for");
	clubk("HI", NULL, 0, "new card");
	clubk("SS", NULL, 0, "status");
	for (i = 0; i < sizeof(card); i++)
		card[i] = (u8)(i * 3 + 1);
	clubk("WL", card, sizeof(card), "write");
	clubk("RL", NULL, 0, "read");
	clubk("OT0", NULL, 0, "eject");
	clubk("RT5", NULL, 0, "RT5");
	clubk("CA", NULL, 0, "cancel");
	clubk("CL", NULL, 0, "clean");
	clubk("XY", NULL, 0, "unknown");
	{
		static const u8 bad[] = { 2, 'S', 'S', 3, 0 };
		send(bad, sizeof(bad));
		drain("bad crc");
	}
	clubk("RL", NULL, 0, "read, no card");
	state("state, a read waiting");
	insert("insert after the state");
	card_reader_state_before_v24();
	card_reader_term();
}

static void barcode(const char *id, const char *file)
{
	int i;
	strcpy(naomi_game_id, id);
	barcode_file = file;
	card_reader_barcode_init();
	pipe_in = scif_pipe;
	printf("== barcode %s, a file of %d, available %d %d\n", id, file ? (int)strlen(file) : -1,
			(int)card_reader_barcode_available(), (int)card_reader_available());
	insert("read");
	for (i = 0; i < 5; i++)
		card_reader_insert_card(0);
	state("state, five read");
	card_reader_state_before_v24();
	drain("state before V24");
	card_reader_barcode_term();
	printf("available %d %d\n", (int)card_reader_barcode_available(), (int)card_reader_available());
}

static void initd(void) { card_reader_initd_init(); pipe_in = scif_pipe; }
static void derby(void) { card_reader_derby_init(); pipe_in = mie_pipe; }
static void club(void) { card_reader_clubk_init(); pipe_in = scif_pipe; }

int main(void)
{
	settings.System = DC_PLATFORM_NAOMI;
	printf("available %d\n", (int)card_reader_available());
	insert("insert, no reader");
	sanwa_reader(card_reader_initd_init, 0, "INITIAL D");
	card_file_size = 0;
	sanwa_reader(card_reader_derby_init, 1, "DERBY OWNERS CLUB WE");
	card_file_size = 0;
	sanwa_reader(card_reader_derby_init, 1, " DERBY OWNERS CLUB ------------");
	card_file_size = 0;
	clubkart();
	barcode("BTR 2K9 VER 1.004", "*0123456789*\r\nnext line");
	barcode("SOMETHING ELSE", "a barcode longer than thirty-two characters, to the end of it");
	barcode("SOMETHING ELSE", NULL);

	/* states across readers, and with IC card readers */
	strcpy(naomi_game_id, "DERBY OWNERS CLUB WE");
	state_across(initd, derby, "Initial D's state, Derby's reader");
	state_across(derby, derby, "Derby's state, Derby's reader");
	state_across(club, initd, "Club Kart's state, Initial D's reader");
	rfid = 1;
	state_across(initd, initd, "with IC card readers");
	card_reader_term();
	printf("available %d\n", (int)card_reader_available());
	rfid = 0;
	/* states that are not right */
	{
		static u8 buf[2048];
		void *p = buf;
		unsigned int total = 0;
		buf[0] = 9;
		printf("bad kind: %d", (int)card_reader_unserialize_v24(&p, &total));
		p = buf;
		total = 0;
		printf(" %d", (int)naomi_cards_unserialize_v30(&p, &total));
		buf[0] = 0x31;
		p = buf;
		total = 0;
		printf(" %d\n", (int)naomi_cards_unserialize_v30(&p, &total));
		card_reader_initd_init();
		buf[0] = 1;	/* Initial D's */
		p = buf;
		total = 0;
		state_limit = 100;
		printf("cut short: %d", (int)card_reader_unserialize_v24(&p, &total));
		p = buf;
		total = 0;
		printf(" %d\n", (int)naomi_cards_unserialize_v30(&p, &total));
		state_limit = 0xffffffff;
		card_reader_term();
	}
	settings.System = DC_PLATFORM_DREAMCAST;
	{
		static u8 buf[64];
		void *p = buf;
		unsigned int total = 0;
		naomi_cards_serialize_v30(&p, &total);
		printf("Dreamcast: %u bytes %d\n", total, (int)naomi_cards_unserialize_v30(&p, &total));
	}
	return 0;
}
