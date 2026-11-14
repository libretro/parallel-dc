/* The disc layer (core/imgread) against what it read before: every image
 * make_images.py writes - .cue, .gdi, .cdi and .chd, well made and not -
 * opened as the drive opens one, and everything the rest of the core can
 * see of it printed: its tracks, sessions and lead-out, both areas' tables
 * of contents, every session's info, every track's place and kind, and a
 * running CRC of every sector read whole and in each size the drive asks
 * for. run.sh compares that with expected.txt, which the C++ disc layer
 * this replaced printed.
 *
 *    disc_test <image>...
 *
 * C89. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <encodings/crc32.h>

#include "types.h"
#include "imgread/common.h"

/* what the disc layer links against, stood in for */
settings_t settings;
signed int sns_asc, sns_ascq, sns_key;
void gd_swap_cancel(void) {}
bool gd_swap_pending(void) { return false; }
void gd_disc_inserted(void) {}
void gd_setdisc(void) {}
void libCore_gdrom_disc_change(void) {}
void libCore_CDDA_Detach(void) {}
void LoadSettings(void) {}
int msgboxf(const char *text, unsigned int type, ...) { (void)text; (void)type; return 0; }
int GetFile(char *szFileName, char *szParse, u32 flags) { (void)szFileName; (void)szParse; (void)flags; return 0; }
/* (no log) */
retro_log_printf_t log_cb;

static u8 buf[2448 * 4];
static u8 many[2352 * 40];

static void dump(const char *path)
{
	static const u32 fmts[] = { 2048, 2336, 2340, 2352 };
	u32 crc[4] = { 0, 0, 0, 0 };
	u32 look = 0;
	Disc *d = OpenDisc(path);
	const char *name = strchr(path, '/') ? strchr(path, '/') + 1 : path;
	u32 i, area, s, n, last, fad;

	printf("== %s\n", name);
	if (!d)
	{
		printf("not opened\n");
		return;
	}
	disc = d;
	printf("type %u end %u leadout %u/%u/%u tracks %u sessions %u\n", (unsigned)d->type, d->EndFAD,
			d->LeadOut.StartFAD, d->LeadOut.CTRL, d->LeadOut.ADDR, d->track_count, d->session_count);
	for (i = 0; i < d->track_count; i++)
		printf(" track %u: %u-%u ctrl %u addr %u file %d\n", i + 1, d->tracks[i].StartFAD,
				d->tracks[i].EndFAD, d->tracks[i].CTRL, d->tracks[i].ADDR, d->tracks[i].file != 0);
	for (i = 0; i < d->session_count; i++)
		printf(" session %u: track %u fad %u\n", i + 1, d->sessions[i].FirstTrack, d->sessions[i].StartFAD);
	for (area = 0; area < 2; area++)
	{
		u32 toc[102];
		GetDriveToc(toc, (DiskArea)area);
		printf(" toc %u:", area);
		for (i = 0; i < 102; i++)
			if (toc[i] != 0xFFFFFFFF)
				printf(" %u=%08x", i, toc[i]);
		printf("\n");
	}
	for (s = 0; s <= d->session_count + 1; s++)
	{
		u8 si[6];
		GetDriveSessionInfo(si, (u8)s);
		printf(" sessinfo %u: %02x %02x %02x %02x %02x %02x\n", s, si[0], si[1], si[2], si[3], si[4], si[5]);
	}
	for (n = 0; n <= d->track_count + 1; n++)
	{
		u32 a0 = 0, b0 = 0;
		u8 adr = 9, ctrl = 9;
		bool ok = libGDR_GetTrack(n, &a0, &b0);
		libGDR_GetTrackAdrAndControl(n, &adr, &ctrl);
		printf(" gettrack %u: %d %u %u adr %u ctrl %u\n", n, ok, a0, b0, adr, ctrl);
	}
	printf(" leadoutfad %u disctype %u present %d\n", libGDR_LeadOutFAD(), libGDR_GetDiscType(), libGDR_DiscPresent());
	last = d->EndFAD + 20;
	if (last < 1000)
		last = 1000;
	for (n = 0; n < d->track_count; n++)
		if (d->tracks[n].EndFAD + 20 > last)
			last = d->tracks[n].EndFAD + 20;
	disc_prefetch(d, 150, 50);
	for (fad = 0; fad < last; fad++)
	{
		u32 elapsed = 7, w[4];
		SectorFormat sf;
		int f;

		/* where it is, and whether it can be lent or seen in place */
		w[0] = libGDR_GetTrackNumber(fad, &elapsed);
		w[1] = elapsed;
		w[2] = disc_sector_lend(d, fad, &sf) != NULL;
		w[3] = disc_sector_view(d, fad, &sf) != NULL;
		look = encoding_crc32(look, (const u8 *)w, sizeof(w));
		for (f = 0; f < 4; f++)
		{
			memset(buf, 0xA5, sizeof(buf));
			disc_read_sectors(d, fad, 1, buf, fmts[f]);
			crc[f] = encoding_crc32(crc[f], buf, fmts[f] + 16);
		}
	}
	/* several at once */
	memset(many, 0, sizeof(many));
	disc_read_sectors(d, 140, 40, many, 2352);
	printf(" many %08x\n", encoding_crc32(0, many, sizeof(many)));
	printf(" crc %08x %08x %08x %08x lookup %08x\n", crc[0], crc[1], crc[2], crc[3], look);
	disc = NULL;
	disc_free(d);
}

int main(int argc, char **argv)
{
	int a;

	for (a = 1; a < argc; a++)
		dump(argv[a]);
	return 0;
}
