/* A CHD (MAME's compressed hunks of data) of a CD or a GD-ROM: its hunks
 * decoded by chd_image.c, a read-ahead thread a stream decoding the next
 * ones before they are read */
#include <stdlib.h>
#include <string.h>

#include <string/stdstring.h>
#include <vfs/vfs_prefetch.h>

#include "common.h"
#include "chd_image.h"

/* tracks are padded to a multiple of this many frames */
#define CD_TRACK_PADDING 4

/* Which read-ahead a track reads through. CD audio and data are read
 * from different places on the disc at once (music playing while a
 * game streams), so each has its own, and neither throws out the
 * hunks the other is about to use. */
enum { CHD_STREAM_DATA, CHD_STREAM_AUDIO, CHD_STREAMS };

/* Hunks each stream's thread decodes ahead of its reader. */
static const unsigned chd_stream_slots[CHD_STREAMS] = { 8, 4 };

#define NO_HUNK 0xFFFFFFFFu

typedef struct chd_disc chd_disc_t;

/* A stream's read-ahead: libretro-common's prefetch thread, which decodes
 * the hunks ahead of the reader on an image handle of its own. One window
 * is one hunk. */
typedef struct chd_stream
{
	chd_disc_t*          disc;
	chd_image_t*         chd;
	struct vfs_prefetch* p;
} chd_stream_t;

/* What the disc keeps of the image (the Disc's owner) */
struct chd_disc
{
	chd_image_t* chd;
	u8* hunk_mem;
	u32 old_hunk;

	u32 hunkbytes;
	u32 sph;
	u32 total_hunks;

	chd_stream_t streams[CHD_STREAMS];
};

typedef struct chd_track
{
	track_file_t base;
	chd_disc_t* disc;
	u32 StartFAD;
	u32 Offset;
	u32 fmt;
	unsigned stream;
	bool swap_bytes;
} chd_track_t;

static int64_t chd_stream_fetch(void *user, uint64_t off, uint8_t *buf, size_t len)
{
	chd_stream_t *st = (chd_stream_t *)user;
	u32 hunk = (u32)(off / st->disc->hunkbytes);

	if (hunk >= st->disc->total_hunks)
		return 0;
	if (len < st->disc->hunkbytes || !chd_image_read_hunk(st->chd, hunk, buf))
		return -1;
	return (int64_t)st->disc->hunkbytes;
}

/* Decompress @hunk into hunk_mem on this thread. A hunk that fails to
 * decode reads as zeros and is not kept, so the next read retries it. */
static void chd_hunk_read_sync(chd_disc_t *d, u32 hunk)
{
	if (chd_image_read_hunk(d->chd, hunk, d->hunk_mem))
		d->old_hunk = hunk;
	else
	{
		WARN_LOG(GDROM, "chd: hunk %u failed to decode", hunk);
		memset(d->hunk_mem, 0, d->hunkbytes);
		d->old_hunk = NO_HUNK;
	}
}

/* Where the sector at @fad_offs is decoded: in the stream's window when
 * its thread got there first, else decoded here. Valid until the next
 * read on this disc. */
static const u8* chd_sector_get(chd_disc_t *d, unsigned stream, u32 fad_offs)
{
	const u32 hunk = fad_offs / d->sph;
	const u32 in   = (fad_offs % d->sph) * (2352 + 96);
	const u8* ptr;

	if (d->streams[stream].p
			&& vfs_prefetch_view(d->streams[stream].p, (uint64_t)hunk * d->hunkbytes + in, &ptr) > 0)
		return ptr;
	if (hunk != d->old_hunk)
		chd_hunk_read_sync(d, hunk);
	return d->hunk_mem + in;
}

/* The stream's thread starts on @hunk now, ahead of the reads. */
static void chd_stream_aim(chd_disc_t *d, unsigned stream, u32 hunk)
{
	if (d->streams[stream].p)
		vfs_prefetch_seek(d->streams[stream].p, (uint64_t)hunk * d->hunkbytes);
}

static void chd_start_streams(chd_disc_t *d, const char* file)
{
	unsigned i;

	for (i = 0; i < CHD_STREAMS; i++)
	{
		chd_stream_t *st = &d->streams[i];
		st->disc = d;
		if (!(st->chd = chd_image_open(file, NULL, 0)))
			continue;
		if (!(st->p = vfs_prefetch_new(chd_stream_fetch, st, d->hunkbytes,
				chd_stream_slots[i], 0)))
		{
			chd_image_close(st->chd);
			st->chd = NULL;
		}
	}
}

static void chd_stop_streams(chd_disc_t *d)
{
	unsigned i;

	for (i = 0; i < CHD_STREAMS; i++)
	{
		vfs_prefetch_free(d->streams[i].p);
		d->streams[i].p = NULL;
		chd_image_close(d->streams[i].chd);
		d->streams[i].chd = NULL;
	}
}

static void chd_disc_free(void *owner)
{
	chd_disc_t *d = (chd_disc_t *)owner;

	if (!d)
		return;
	chd_stop_streams(d);
	free(d->hunk_mem);
	chd_image_close(d->chd);
	free(d);
}

/* (a CD's mode 2 track can be kept without the sync and header: 2336 bytes) */
static SectorFormat chd_track_format(const chd_track_t *t)
{
	return t->fmt == 2352 ? SECFMT_2352 : t->fmt == 2336 ? SECFMT_2336_MODE2 : SECFMT_2048_MODE1;
}

static void chd_track_read(track_file_t *f, u32 FAD, u8* dst, SectorFormat* sector_type,
		u8* subcode, SubcodeFormat* subcode_type)
{
	chd_track_t *t = (chd_track_t *)f;
	const u8* from = chd_sector_get(t->disc, t->stream, FAD + t->Offset);

	if (t->swap_bytes)
	{
		/* The other byte order, made on the way across: 16 bits at a
		 * time, which the compiler does sixteen bytes at a time. (It
		 * was copied, and then swapped where it lay a byte at a time.) */
		u32 i;
		for (i = 0; i + 1 < t->fmt; i += 2)
		{
			u16 w;
			memcpy(&w, from + i, 2);
			w = (u16)((w << 8) | (w >> 8));
			memcpy(dst + i, &w, 2);
		}
	}
	else
		memcpy(dst,from,t->fmt);

	*sector_type=chd_track_format(t);

	/* While space is reserved for it, the images contain no actual subcodes */
	*subcode_type=SUBFMT_NONE;
}

/* The compressed bytes of every hunk the sectors span, so neither this
 * thread nor the read-ahead stalls on a cold page (all handles are on the
 * same file, so one hint serves them all), and the stream's thread set
 * decoding from the first one. */
static void chd_track_prefetch(track_file_t *f, u32 FAD, u32 count)
{
	chd_track_t *t = (chd_track_t *)f;
	u32 first, last;

	if (!count)
		return;
	first = (FAD + t->Offset) / t->disc->sph;
	last  = (FAD + t->Offset + count - 1) / t->disc->sph;
	chd_image_prefetch(t->disc->chd, first, last - first + 1);
	chd_stream_aim(t->disc, t->stream, first);
}

/* The sector inside the decoded hunk; a track whose bytes have to be
 * swapped is read through a buffer. */
static const u8* chd_track_view(track_file_t *f, u32 FAD, SectorFormat* sector_type)
{
	chd_track_t *t = (chd_track_t *)f;

	if (t->swap_bytes)
		return NULL;
	*sector_type = chd_track_format(t);
	return chd_sector_get(t->disc, t->stream, FAD + t->Offset);
}

static void chd_track_destroy(track_file_t *f)
{
	free(f);
}

static const track_file_ops_t chd_track_ops =
{
	chd_track_read, chd_track_view, chd_track_prefetch, NULL, chd_track_destroy
};

/* The disc's tracks and sessions, from the image's: false for an image
 * that is not one of a disc that can be read */
static bool chd_try_open(Disc *disc, chd_disc_t *d, const char* file)
{
	char err[512];
	u32 total_frames = 150;
	u32 Offset = 0;
	bool gdrom;
	u32 last_pregap = 0;

	err[0] = '\0';
	d->chd = chd_image_open(file, err, sizeof(err));
	if (d->chd == NULL)
	{
		INFO_LOG(GDROM, "chd: %s", err[0] ? err : "out of memory");
		return false;
	}

	INFO_LOG(GDROM, "chd: parsing file %s", file);

	d->hunkbytes = chd_image_hunk_bytes(d->chd);
	d->total_hunks = chd_image_hunk_count(d->chd);
	if (!(d->hunk_mem = (u8 *)malloc(d->hunkbytes ? d->hunkbytes : 1)))
		return false;
	d->old_hunk = NO_HUNK;

	d->sph = d->hunkbytes/(2352+96);

	if (d->hunkbytes%(2352+96)!=0)
	{
		INFO_LOG(GDROM, "chd: hunkbytes is invalid, %d\n",d->hunkbytes);
		return false;
	}

	gdrom = chd_image_version(d->chd) < 5;	/* a CD's image is only read from version 5 on */

	for(;;)
	{
		chd_image_track_t meta;
		int tkid, frames, pregap, postgap;
		const char *type, *subtype;
		u32 sector_size = 0;
		u32 pregap_kept;
		Track t;
		chd_track_t *ct;
		int padded;

		if (!chd_image_track(d->chd, (uint32_t)disc->track_count, &meta))
			break;

		tkid = meta.track; frames = meta.frames; pregap = meta.pregap; postgap = meta.postgap;
		type = meta.type; subtype = meta.subtype;
		if (meta.gdrom)
			gdrom = true;

		/* The sector sizes of the CD and GD-ROM track types. The subcode
		 * is kept in its 96 bytes after each sector, whatever the subtype,
		 * and the sectors are where they are with it or without. */
		if (!strcmp(type, "AUDIO") || !strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE2_RAW"))
			sector_size = 2352;
		else if (!strcmp(type, "MODE1"))
			sector_size = 2048;
		else if (!strcmp(type, "MODE2"))
			sector_size = 2336;
		/* A pregap of type "V..." is kept in the image, at the start of
		 * the track's frames; any other is on the disc and not in the
		 * image. A postgap is never in the image. */
		pregap_kept = pregap > 0 && meta.pgtype[0] == 'V' ? (u32)pregap : 0;
		if ((u32)tkid!=(disc->track_count+1) || sector_size == 0 || frames < 0 || pregap < 0 || postgap < 0 || (int)pregap_kept > frames)
		{
			INFO_LOG(GDROM, "chd: track %d of type %s, %d frames, pregap %d is not supported", tkid, type, frames, pregap);
			return false;
		}
		if (strcmp(subtype, "NONE") != 0)
			WARN_LOG(GDROM, "chd: track %d has subcode (%s): not used", tkid, subtype);
		/* (a disc has 99 tracks at most, and so has its table of contents) */
		if (disc->track_count >= 99)
		{
			INFO_LOG(GDROM, "chd: more than 99 tracks");
			return false;
		}
		DEBUG_LOG(GDROM, "chd: track %d %s %d frames", tkid, type, frames);
		memset(&t, 0, sizeof(t));
		/* (where INDEX 01 is: after the pregap) */
		t.StartFAD = total_frames + pregap;
		total_frames = t.StartFAD + frames - pregap_kept;
		t.EndFAD = total_frames - 1;
		/* The pad frames of a GD-ROM track fill it up to a whole number
		 * of frames in the image: they are not on the disc. */
		if (meta.pad > 0 && meta.pad < frames - (int)pregap_kept)
			t.EndFAD -= (u32)meta.pad;
		t.ADDR = 0;
		t.CTRL = strcmp(type,"AUDIO") == 0 ? 0 : 4;
		last_pregap = pregap;
		if (!(ct = (chd_track_t *)calloc(1, sizeof(*ct))))
			return false;
		ct->base.ops = &chd_track_ops;
		ct->disc = d;
		ct->StartFAD = t.StartFAD;
		ct->Offset = Offset + pregap_kept - t.StartFAD;
		ct->fmt = sector_size;
		ct->stream = t.CTRL == 0 ? CHD_STREAM_AUDIO : CHD_STREAM_DATA;
		/* Music is kept in the other byte order in a version 5 image (and
		 * in any of a CD, which is read from version 5 only). */
		ct->swap_bytes = t.CTRL == 0 && chd_image_version(d->chd) >= 5;
		t.file = &ct->base;

		padded = (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING;
		Offset += padded * CD_TRACK_PADDING;

		disc_add_track(disc, &t);
	}

	/* (a hard disk's image, or anything else with no track in it, is no disc) */
	if (disc->track_count == 0)
	{
		INFO_LOG(GDROM, "chd: no track found");
		return false;
	}

	if (gdrom)
	{
		if (total_frames!=549300 || disc->track_count<3)
			WARN_LOG(GDROM, "WARNING: chd: Total frames is wrong: %u frames in %u tracks", total_frames, disc->track_count);

		disc_fill_gd_session(disc);
	}
	else
	{
		/* A CD. The image does not say where its sessions are: one that
		 * ends in a data track after others is taken for a MIL-CD, its
		 * last track alone in a second session; any other has one. */
		Track *last = &disc->tracks[disc->track_count - 1];

		disc_add_session(disc, 1, disc->tracks[0].StartFAD);
		disc->LeadOut.ADDR = 1;	/* subcode-q channel */
		disc->LeadOut.CTRL = 4;	/* data */
		if (disc->track_count > 1 && last->CTRL == 4)
		{
			/* Between the sessions: the first one's lead-out (01:30:00),
			 * the second's lead-in (01:00:00) and the pregap of its
			 * track (00:02:00), which is counted already when the image
			 * has it. */
			const u32 gap = 6750 + 4500 + 150 - (last_pregap < 150 ? last_pregap : 150);
			disc->type = CdRom_XA;
			last->StartFAD += gap;
			last->EndFAD += gap;
			((chd_track_t *)last->file)->Offset -= gap;
			disc_add_session(disc, disc->track_count, last->StartFAD);
			disc->EndFAD = disc->LeadOut.StartFAD = total_frames + gap;
		}
		else
		{
			u32 i;
			disc->type = CdRom;
			for (i = 0; disc->track_count > 1 && i < disc->track_count; i++)
				if (disc->tracks[i].CTRL == 4)
					disc->type = CdRom_XA;
			disc->EndFAD = disc->LeadOut.StartFAD = total_frames;
		}
	}

	chd_start_streams(d, file);

	return true;
}


Disc* chd_parse(const char* file)
{
	size_t len = strlen(file);
	Disc *disc;
	chd_disc_t *d;
	unsigned i;

	/* Only try to open .chd files */
	if (len > 3 && !string_is_equal_noncase(&file[len - 3], "chd"))
		return NULL;

	if (!(disc = disc_new()))
		return NULL;
	if (!(d = (chd_disc_t *)calloc(1, sizeof(*d))))
	{
		disc_free(disc);
		return NULL;
	}
	for (i = 0; i < CHD_STREAMS; i++)
		d->streams[i].disc = d;
	disc->owner = d;
	disc->owner_free = chd_disc_free;

	if (chd_try_open(disc, d, file))
		return disc;
	disc_free(disc);
	return NULL;
}
