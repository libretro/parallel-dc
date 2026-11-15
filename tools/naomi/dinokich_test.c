/* The IC card reader of dinokich and loveber3cn (core/hw/naomi/dinokich.c),
 * by itself: its two cards from their files, and made new where there are
 * none; every request it knows, to either card and to neither, read and
 * written where there is room and where there is not; frames that are not
 * right; an answer nobody reads; and its part of a save state, read back,
 * not right and from before V25. What it answers and keeps is printed, for
 * dinokich_expected.txt (sh tools/naomi/dinokich_run.sh). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include <streams/file_stream.h>
#include "hw/naomi/dinokich.h"
#include "savewriter.h"

/* (no log) */
retro_log_printf_t log_cb;
void os_DebugBreak(void) { printf("FAIL: os_DebugBreak\n"); exit(1); }

static u32 hash(const u8 *p, size_t n)
{
	u32 h = 2166136261u;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

/* The cards' files: "card1" and "card2", there or not */
static u8 files[2][1024];
static int have_file[2];
static int open_files;
RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{
	int i = !strcmp(path, "card1") ? 0 : !strcmp(path, "card2") ? 1 : -1;
	(void)mode;
	(void)hints;
	if (i < 0 || !have_file[i])
		return NULL;
	open_files++;
	return (RFILE *)files[i];
}
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{
	memcpy(data, stream, (size_t)len);
	return len;
}
int filestream_close(RFILE *stream)
{
	(void)stream;
	open_files--;
	return 0;
}

/* What is saved: with a card made new, random but for what it must have */
static int made;
int save_writer_put_file(const char *path, const void *data, size_t len, retro_atomic_int_t *failed)
{
	const u8 *p = (const u8 *)data;
	u32 sum = 0, i;
	(void)failed;
	for (i = 0; i < 64; i++)
		sum += p[0x40 + i];
	if (made)
		printf("save %s: %u bytes, id %.6s, check %02x, sum %s\n", path, (unsigned)len,
				(const char *)p + 0x20, p[0x30], p[0x80] == (u8)sum ? "right" : "WRONG");
	else
		printf("save %s: %u bytes %08x\n", path, (unsigned)len, hash(p, len));
	return 1;
}
void save_writer_drain(void) { printf("drain\n"); }

static int updates;
static void update_status(void *port) { (void)port; updates++; }

bool ra_serialize(const void *src, unsigned int size, void **dest, unsigned int *total)
{
	if (*dest) { memcpy(*dest, src, size); *(u8 **)dest += size; }
	*total += size;
	return true;
}
bool ra_unserialize(void *dst, unsigned int size, void **src, unsigned int *total)
{
	memcpy(dst, *src, size); *(u8 **)src += size; *total += size;
	return true;
}

static dinokich_reader *r;

static void drain(const char *what)
{
	static u8 out[1024];
	int n = 0, i;
	while (dinokich_reader_available(r) > 0 && n < (int)sizeof(out))
		out[n++] = dinokich_reader_read(r);
	printf("%s: %d bytes %08x (%d updates):", what, n, hash(out, n), updates);
	for (i = 0; i < n && i < 28; i++)
		printf(" %02x", out[i]);
	printf("\n");
	updates = 0;
}

/* AA cmd 00 N, the N bytes, their xor */
static void frame(u8 cmd, const u8 *p, u32 n, int bad)
{
	u8 x = 0xaa ^ cmd ^ (u8)n;
	u32 i;
	dinokich_reader_write(r, 0xaa);
	dinokich_reader_write(r, cmd);
	dinokich_reader_write(r, 0);
	dinokich_reader_write(r, (u8)n);
	for (i = 0; i < n; i++)
	{
		dinokich_reader_write(r, p[i]);
		x ^= p[i];
	}
	dinokich_reader_write(r, (u8)(x ^ (bad ? 1 : 0)));
}

/* a request whose bytes from f[4] are: slot, two zeroes, address (2),
 * salt (2), then @rest */
static void request(u8 cmd, int slot, u32 addr, const u8 *rest, u32 nrest, const char *what)
{
	u8 p[300];
	memset(p, 0, sizeof(p));
	p[0] = (u8)slot;
	p[3] = (u8)(addr >> 8);
	p[4] = (u8)addr;
	p[5] = 0x5a;
	p[6] = 0xc3;
	if (nrest != 0)
		memcpy(p + 7, rest, nrest);
	frame(cmd, p, 7 + nrest, 0);
	drain(what);
}

static void state(const char *what, int spoil)
{
	static u8 buf[DINOKICH_STATE_SIZE + 16];
	void *p = buf;
	unsigned int total = 0;
	dinokich_reader_serialize_v25(r, &p, &total);
	printf("%s: state %u bytes %08x\n", what, total, hash(buf, total));
	if (spoil == 1)
	{
		/* a request so far longer than any, and what is to be sent past the end */
		const u16 bad = 1000;
		buf[0] = 0xff;	/* and the first card's flags all set */
		memcpy(buf + 2 * (1 + 1024), &bad, 2);
		memcpy(buf + 2 * (1 + 1024) + 2 + 260, &bad, 2);
	}
	else if (spoil == 2)
	{
		/* a request so far that does not start as one */
		buf[2 * (1 + 1024) + 2] = 0x12;
	}
	p = buf;
	total = 0;
	dinokich_reader_unserialize_v25(r, &p, &total);
	/* as it is now */
	p = buf;
	total = 0;
	dinokich_reader_serialize_v25(r, &p, &total);
	printf("%s: read %08x\n", what, hash(buf, total));
	drain(what);
}

int main(void)
{
	static const u8 version[7] = { 1, 2, 3, 4, 5, 6, 8 };
	static const u8 bad_version[7] = { 1, 2, 3, 4, 5, 6, 9 };
	u8 data[200];
	u32 i;
	int slot;

	for (i = 0; i < 1024; i++)
	{
		files[0][i] = (u8)(i * 7 + 1);
		files[1][i] = (u8)(i * 11 + 5);
	}

	/* the cards made new: what they must have, the rest random */
	made = 1;
	r = dinokich_reader_open("loveber3cn", "card1", "card2", update_status, NULL);
	dinokich_reader_close(r);
	r = dinokich_reader_open("dinokich", "card1", "card2", update_status, NULL);
	dinokich_reader_close(r);
	made = 0;

	/* from their files */
	have_file[0] = have_file[1] = 1;
	r = dinokich_reader_open("dinokich", "card1", "card2", update_status, NULL);
	printf("files open: %d\n", open_files);

	frame(0xb0, version, 7, 0);
	drain("version");
	frame(0xb0, bad_version, 7, 0);
	drain("version, not right");
	frame(0xb0, version, 6, 0);
	drain("version, short");
	frame(0xc0, NULL, 0, 0);
	drain("lamp");
	frame(0xb9, NULL, 0, 0);
	drain("which cards");
	for (slot = 0; slot <= 3; slot++)
	{
		char what[64];
		sprintf(what, "slot %d: salt", slot);
		request(0x67, slot, 0, NULL, 0, what);
		sprintf(what, "slot %d: power on", slot);
		request(0xb2, slot, 0, NULL, 0, what);
		data[0] = 0;
		data[1] = 16;
		sprintf(what, "slot %d: read", slot);
		request(0x61, slot, 0x20, data, 2, what);
		sprintf(what, "slot %d: read at the end", slot);
		data[1] = 8;
		request(0x61, slot, 1020, data, 2, what);
		data[1] = 0;
		sprintf(what, "slot %d: read nothing", slot);
		request(0x61, slot, 0, data, 2, what);
		for (i = 0; i < sizeof(data); i++)
			data[i] = (u8)(i * 3 + slot);
		sprintf(what, "slot %d: write", slot);
		request(0x60, slot, 0x100, data, 40, what);
		sprintf(what, "slot %d: write past the end", slot);
		request(0x60, slot, 1000, data, 40, what);
		sprintf(what, "slot %d: write 0x6d", slot);
		request(0x6d, slot, 0x20, data, 16, what);
		sprintf(what, "slot %d: read what was written", slot);
		data[0] = 0;
		data[1] = 40;
		request(0x61, slot, 0x100, data, 2, what);
		sprintf(what, "slot %d: check value shown", slot);
		request(0x64, slot, 0, data, 2, what);
		sprintf(what, "slot %d: check value written", slot);
		request(0x63, slot, 0, data, 2, what);
		sprintf(what, "slot %d: power off", slot);
		request(0xb3, slot, 0, NULL, 0, what);
	}
	/* a write with no data, short requests */
	{
		u8 p[1] = { 1 };
		frame(0x60, p, 1, 0);
		drain("write, nothing");
		frame(0x61, p, 1, 0);
		drain("read, short");
		frame(0x67, p, 1, 0);
		drain("salt, short");
	}
	request(0x99, 1, 0, NULL, 0, "unknown");
	frame(0xc0, NULL, 0, 1);
	drain("bad checksum");
	dinokich_reader_write(r, 0x12);
	dinokich_reader_write(r, 0x34);
	frame(0xb9, NULL, 0, 0);
	drain("bytes before a frame");
	/* answers nobody reads, past the room there is */
	data[0] = 0;
	data[1] = 255;
	for (i = 0; i < 4; i++)
		request(0x61, 1, 0, data, 2, i == 3 ? "read, nobody reading" : "read");
	for (i = 0; i < 4; i++)
	{
		u8 p[9];
		memset(p, 0, sizeof(p));
		p[0] = 1;
		p[8] = 200;
		frame(0x61, p, 9, 0);
	}
	drain("four answers, three sent");
	/* two of 252: the second has 255 bytes of room, and needs 257 */
	for (i = 0; i < 2; i++)
	{
		u8 p[9];
		memset(p, 0, sizeof(p));
		p[0] = 1;
		p[8] = 252;
		frame(0x61, p, 9, 0);
	}
	drain("two answers, one sent");

	/* states */
	dinokich_reader_write(r, 0xaa);
	dinokich_reader_write(r, 0xb9);
	frame(0xc0, NULL, 0, 0);
	state("state, a request on its way", 0);
	dinokich_reader_write(r, 0);
	dinokich_reader_write(r, 0);
	dinokich_reader_write(r, 0xaa ^ 0xb9);
	drain("the request finished");
	frame(0xc0, NULL, 0, 0);
	state("state, not right", 1);
	frame(0xb9, NULL, 0, 0);
	drain("after it");
	dinokich_reader_write(r, 0xaa);
	state("state, not a request", 2);
	frame(0xb9, NULL, 0, 0);
	drain("after it");
	request(0xb2, 2, 0, NULL, 0, "power on");
	frame(0xc0, NULL, 0, 0);
	dinokich_reader_state_before_v25(r);
	drain("state before V25");
	frame(0xc0, NULL, 0, 0);
	dinokich_reader_reset(r);
	drain("reset");
	dinokich_reader_close(r);
	printf("files open: %d\n", open_files);
	return 0;
}
