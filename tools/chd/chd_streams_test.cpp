/* CD audio and data read from a CHD at once, as a game plays music while
 * it streams from the disc.
 *
 *    chd_streams_test <image.chd> <image.bin>
 *
 * The image is the two-track CD tools/chd/make_fixture.py writes (data,
 * then audio). The audio is read a sector at a time, steadily, and the
 * data in bursts announced with Prefetch() as the GD-ROM's read command
 * does, between pauses that stand in for emulation. The pauses are
 * scaled to how fast this build decodes, so the reads are paced like the
 * drive's, well below the decoder, under a sanitizer as without. Every
 * sector must match the BIN it came from, and the hunks must be decoded
 * by the read-ahead threads: the reading thread may decode only where a
 * stream starts. chd_image_read_hunk is wrapped (run.sh links with
 * --wrap) to count the decodes done on the reading thread.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <rthreads/rthreads.h>
#include <retro_timers.h>
#include <features/features_cpu.h>

#include "types.h"
#include "imgread/common.h"
#include "imgread/chd_image.h"

Disc* chd_parse(const char* file);

u8 q_subchannel[96];

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char*, int,
		const char* fmt, ...)
{
	(void)fmt;
}

static uintptr_t reader_thread;
static long      reader_decodes;
static long      decodes;

extern "C" bool __real_chd_image_read_hunk(chd_image_t *img, uint32_t hunk, uint8_t *dst);

extern "C" bool __wrap_chd_image_read_hunk(chd_image_t *img, uint32_t hunk, uint8_t *dst)
{
	if (sthread_get_current_thread_id() == reader_thread)
		reader_decodes++;
	__sync_fetch_and_add(&decodes, 1);
	return __real_chd_image_read_hunk(img, hunk, dst);
}

#define BURSTS        12
#define BURST_SECTORS 96
#define AUDIO_STEPS   40	/* audio sectors between two bursts */

static retro_time_t pace_us;	/* emulation between two sector reads */
static retro_time_t owed_us;

static void pace(retro_time_t us)
{
	owed_us += us;
	if (owed_us >= 1000)
	{
		retro_sleep((unsigned)(owed_us / 1000));
		owed_us %= 1000;
	}
}

/* Microseconds this build takes to decode a hunk, best of a few. */
static retro_time_t decode_us(const char *path)
{
	chd_image_t *img = chd_image_open(path, NULL, 0);
	retro_time_t best = 0;
	u8 *buf;
	u32 h;

	if (!img)
		return 1000;
	buf = (u8 *)malloc(chd_image_hunk_bytes(img));
	for (h = 0; h < 16 && h < chd_image_hunk_count(img); h++)
	{
		retro_time_t t0 = cpu_features_get_time_usec(), t;
		__real_chd_image_read_hunk(img, h, buf);
		t = cpu_features_get_time_usec() - t0;
		if (!best || t < best)
			best = t;
	}
	free(buf);
	chd_image_close(img);
	return best ? best : 1;
}

int main(int argc, char **argv)
{
	FILE  *f;
	u8    *bin;
	long   bin_len;
	Disc  *disc;
	u8     sector[2352];
	SectorFormat  fmt;
	SubcodeFormat sub;
	u32    data_fad, audio_fad, data_lba, audio_lba, data_end, audio_end;
	int    bad = 0, b, i;
	long   expected;

	if (argc < 3)
	{
		fprintf(stderr, "usage: %s image.chd image.bin\n", argv[0]);
		return 2;
	}
	if (!(f = fopen(argv[2], "rb")))
		return 2;
	fseek(f, 0, SEEK_END);
	bin_len = ftell(f);
	fseek(f, 0, SEEK_SET);
	bin = (u8 *)malloc(bin_len);
	if (fread(bin, 1, bin_len, f) != (size_t)bin_len)
		return 2;
	fclose(f);

	/* a hunk holds 8 sectors: read one every half hunk's decode time,
	 * at least 50 us, a quarter of the speed the decoder keeps up with */
	pace_us = decode_us(argv[1]) / 2;
	if (pace_us < 50)
		pace_us = 50;
	reader_thread = sthread_get_current_thread_id();
	if (!(disc = chd_parse(argv[1])) || disc->tracks.size() != 2
			|| disc->tracks[1].CTRL != 0)
	{
		printf("FAIL: open as a data track and an audio track\n");
		return 1;
	}
	/* LBA in the BIN = FAD - 150 */
	data_fad  = disc->tracks[0].StartFAD;
	data_end  = disc->tracks[0].EndFAD;
	audio_fad = disc->tracks[1].StartFAD;
	audio_end = disc->tracks[1].EndFAD;

	/* let the threads settle after the open */
	retro_sleep(50);
	reader_decodes = 0;

	for (b = 0; b < BURSTS; b++)
	{
		for (i = 0; i < AUDIO_STEPS && audio_fad <= audio_end; i++, audio_fad++)
		{
			audio_lba = audio_fad - 150;
			if (!disc->ReadSector(audio_fad, sector, &fmt, NULL, &sub)
					|| memcmp(sector, bin + (size_t)audio_lba * 2352, 2352))
				bad++;
			pace(pace_us);
		}
		/* the drive seeks before the first sector comes */
		disc->Prefetch(data_fad, BURST_SECTORS);
		pace(pace_us * 16);
		for (i = 0; i < BURST_SECTORS && data_fad <= data_end; i++, data_fad++)
		{
			data_lba = data_fad - 150;
			if (!disc->ReadSector(data_fad, sector, &fmt, NULL, &sub)
					|| memcmp(sector, bin + (size_t)data_lba * 2352, 2352))
				bad++;
			pace(pace_us);
		}
	}

	/* the threads stop with the disc, and their count with them */
	delete disc;

	/* A decode here as each stream starts, and room for a thread now and
	 * then late; reads that throw out each other's hunks decode here at
	 * least twice a burst. */
	expected = 2 + BURSTS / 2;
	printf("  %ld hunks decoded, %ld of them on the reading thread "
			"(at most %ld wanted)\n", decodes, reader_decodes, expected);
	if (bad)
		printf("FAIL: %d sectors do not match the BIN\n", bad);
	if (reader_decodes > expected)
		printf("FAIL: audio and data reads decode on the reading thread\n");

	free(bin);
	if (bad || reader_decodes > expected)
		return 1;
	printf("chd_streams_test: PASS\n");
	return 0;
}
