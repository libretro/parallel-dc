/* A disc and its tracks, and a track read straight from a file: see
 * common.h */
#include <stdlib.h>
#include <string.h>

#include "common.h"

u8 q_subchannel[96];

bool ConvertSector(const u8* in_buff , u8* out_buff , int from , int to,int sector)
{
   /* get subchannel data, if any */
   if (from==2448)
   {
      memcpy(q_subchannel,in_buff+2352,96);
      from-=96;
   }
   /* if no conversion */
   if (to==from)
   {
      memcpy(out_buff,in_buff,to);
      return true;
   }
   switch (to)
   {
      case 2340:
         verify((from==2352) || (from==2336));
         if (from == 2336)
         {
            /* A sector kept without its sync and header (a .cue's
             * MODE2/2336) and asked for with the header: that is made
             * from where the sector is - minutes, seconds and frames in
             * BCD, mode 2. A PlayStation disc is read this way.
             * (flyinghead/flycast#2443, Bruceleeto) */
            out_buff[0] = (u8)(((sector / 75 / 60) % 10) | (((sector / 75 / 60) / 10) << 4));
            out_buff[1] = (u8)((((sector / 75) % 60) % 10) | ((((sector / 75) % 60) / 10) << 4));
            out_buff[2] = (u8)(((sector % 75) % 10) | (((sector % 75) / 10) << 4));
            out_buff[3] = 2;
            memcpy(out_buff + 4, in_buff, 2336);
         }
         else
            memcpy(out_buff,&in_buff[12],2340);
         break;
      case 2328:
         verify((from==2352));
         memcpy(out_buff,&in_buff[24],2328);
         break;
      case 2336:
         verify(from>=2336);
         verify((from==2352));
         memcpy(out_buff,&in_buff[0x10],2336);
         break;
      case 2048:
         verify(from>=2048);
         verify((from==2448) || (from==2352) || (from==2336));
         if ((from == 2352) || (from == 2448))
         {
            if (in_buff[15]==1)
               memcpy(out_buff,&in_buff[0x10],2048); /* 0x10 -> mode1 */
            else
               memcpy(out_buff,&in_buff[0x18],2048); /* 0x18 -> mode2 (all forms ?) */
         }
         else
            memcpy(out_buff,&in_buff[0x8],2048);	/* hmm only possible on mode2.Skip the mode2 header */
         break;
      case 2352:
         /* if (from >= 2352) */
         memcpy(out_buff,&in_buff[0],2352);
         break;
      default :
		INFO_LOG(GDROM, "Sector conversion from %d to %d not supported \n", from , to);
         /* (a blank sector, not what the buffer held before) */
         if (to > 0)
            memset(out_buff, 0, to);
         break;
   }

   return true;
}

/* ---- the disc ---------------------------------------------------------- */

Disc *disc_new(void)
{
	return (Disc *)calloc(1, sizeof(Disc));
}

static void track_destroy(Track *t)
{
	if (t->file)
		t->file->ops->destroy(t->file);
	t->file = NULL;
}

void disc_free(Disc *d)
{
	u32 i;

	if (!d)
		return;
	/* (what the reader kept first, as the disc it had made did) */
	if (d->owner_free)
		d->owner_free(d->owner);
	for (i = 0; i < d->track_count; i++)
		track_destroy(&d->tracks[i]);
	free(d);
}

bool disc_add_track(Disc *d, const Track *t)
{
	if (d->track_count >= DISC_TRACKS)
	{
		Track gone = *t;
		track_destroy(&gone);
		return false;
	}
	d->tracks[d->track_count++] = *t;
	return true;
}

bool disc_add_session(Disc *d, u32 first_track, u32 start_fad)
{
	Session *s;

	if (d->session_count >= DISC_SESSIONS)
		return false;
	s = &d->sessions[d->session_count++];
	s->FirstTrack = (u8)first_track;
	s->StartFAD = start_fad;
	return true;
}

static bool track_holds(const Track *t, u32 FAD)
{
	return FAD>=t->StartFAD && (FAD<=t->EndFAD || t->EndFAD==0) && t->file;
}

bool disc_read_sector(Disc *d, u32 FAD, u8 *dst, SectorFormat *sector_type,
		u8 *subcode, SubcodeFormat *subcode_type)
{
	u32 i;

	for (i = d->track_count; i-- > 0;)
	{
		*subcode_type=SUBFMT_NONE;
		if (track_holds(&d->tracks[i], FAD))
		{
			d->tracks[i].file->ops->read(d->tracks[i].file, FAD, dst, sector_type, subcode, subcode_type);
			return true;
		}
	}
	return false;
}

void disc_prefetch(Disc *d, u32 FAD, u32 count)
{
	while (count)
	{
		u32 i;
		u32 n = count;
		const Track *t = NULL;

		for (i = d->track_count; i-- > 0;)
			if (track_holds(&d->tracks[i], FAD))
			{
				t = &d->tracks[i];
				break;
			}
		if (!t)
			return;
		if (t->EndFAD && t->EndFAD - FAD + 1 < n)
			n = t->EndFAD - FAD + 1;
		/* (a track an image gives no length ends one sector before it starts, and has none to give: this went round for ever) */
		if (n == 0)
			return;
		if (t->file->ops->prefetch)
			t->file->ops->prefetch(t->file, FAD, n);
		FAD += n;
		count -= n;
	}
}

const u8 *disc_sector_view(Disc *d, u32 FAD, SectorFormat *sector_type)
{
	u32 i;

	for (i = d->track_count; i-- > 0;)
		if (track_holds(&d->tracks[i], FAD))
			return d->tracks[i].file->ops->view
				? d->tracks[i].file->ops->view(d->tracks[i].file, FAD, sector_type) : NULL;
	return NULL;
}

const u8 *disc_sector_lend(Disc *d, u32 FAD, SectorFormat *sector_type)
{
	u32 i;

	for (i = d->track_count; i-- > 0;)
		if (track_holds(&d->tracks[i], FAD))
			return d->tracks[i].file->ops->lend
				? d->tracks[i].file->ops->lend(d->tracks[i].file, FAD, sector_type) : NULL;
	return NULL;
}

void disc_read_sectors(Disc *d, u32 FAD, u32 count, u8 *dst, u32 fmt)
{
	u8 temp[2448];
	SectorFormat secfmt;
	SubcodeFormat subfmt;

	while(count)
	{
		const u8* src = disc_sector_view(d, FAD, &secfmt);
		if (!src && disc_read_sector(d, FAD, temp, &secfmt, q_subchannel, &subfmt))
			src = temp;
		if (src)
		{
			/* TODO: Proper sector conversions */
			if (secfmt==SECFMT_2352)
			{
				ConvertSector(src,dst,2352,fmt,FAD);
			}
			else if (fmt == 2048 && secfmt==SECFMT_2336_MODE2)
				memcpy(dst,src+8,2048);
			else if (fmt == 2340 && secfmt==SECFMT_2336_MODE2)
				ConvertSector(src, dst, 2336, fmt, FAD);
			else if (fmt == 2352 && secfmt==SECFMT_2336_MODE2)
			{
				/* The whole sector from one kept without its first
				 * sixteen bytes: those are the sync pattern, which is
				 * the same in every sector, and the header, which is
				 * made as it is for 2340. (Nothing was written.) */
				static const u8 sync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
				memcpy(dst, sync, 12);
				ConvertSector(src, dst + 12, 2336, 2340, FAD);
			}
			else if (fmt==2048 && (secfmt==SECFMT_2048_MODE1 || secfmt==SECFMT_2048_MODE2_FORM1 ))
			{
				memcpy(dst,src,2048);
			}
			else if (fmt==2352 && (secfmt==SECFMT_2048_MODE1 || secfmt==SECFMT_2048_MODE2_FORM1 ))
			{
				INFO_LOG(GDROM, "GDR:fmt=2352;secfmt=2048");
				memcpy(dst,src,2048);
				/* (and the rest of what was asked for is blank, not what the buffer held) */
				memset(dst+2048,0,2352-2048);
			}
			else if (secfmt==SECFMT_2448_MODE2)
			{
				/* A whole sector with its 96 bytes of subcode after
				 * it: without those it is a whole sector, and is
				 * converted as one. (Only to 2048 was: Pier Solar and
				 * the Great Architects. Asked for whole, as music is,
				 * nothing was written.) */
				ConvertSector(src, dst, 2448, fmt, FAD);
			}
			else
			{
				/* No way from one to the other - the header and the
				 * error correction of a sector kept as 2048 bytes are
				 * not there to give: a blank sector, not what the
				 * buffer held before. */
				WARN_LOG(GDROM, "Unable to convert sector. Format: %d Sector format: %d", fmt, secfmt);
				memset(dst, 0, fmt);
			}
		}
		else
		{
			/* nothing there: a blank sector, not whatever was in the buffer */
			INFO_LOG(GDROM, "Sector Read miss FAD: %d", FAD);
			memset(dst, 0, fmt);
		}
		dst+=fmt;
		FAD++;
		count--;
	}
}

void disc_fill_gd_session(Disc *d)
{
	u32 first;

	if (d->track_count == 0)
		return;

	/* session 1 : start @ track 1, and its fad */
	disc_add_session(d, 1, d->tracks[0].StartFAD);

	/* session 2 : start @ track 3, and its fad */
	/* (An image with fewer tracks is no GD-ROM, and gets here all the
	 * same - a cue sheet with one file, a CHD of a CD: its last track
	 * then, where the third of two was read. Such an image is not
	 * made a CD of yet.) */
	first = d->track_count < 3 ? d->track_count : 3;
	disc_add_session(d, first, d->tracks[first - 1].StartFAD);

	/* this isn't always true for gdroms, depends on area look @ the get-toc code */
	d->type=GdRom;
	d->LeadOut.ADDR=1;	/* subcode-q channel */
	d->LeadOut.CTRL=4;	/* data */
	d->LeadOut.StartFAD=549300;

	d->EndFAD=549300;
}

/* ---- a track read straight from a file ---------------------------------- */

typedef struct raw_track
{
	track_file_t base;
	core_file* file;
	s32 offset;
	u32 fmt;
} raw_track_t;

static bool raw_track_format(const raw_track_t *r, SectorFormat* sector_type)
{
	switch (r->fmt)
	{
		case 2352: *sector_type=SECFMT_2352; return true;
		case 2048: *sector_type=SECFMT_2048_MODE2_FORM1; return true;
		case 2336: *sector_type=SECFMT_2336_MODE2; return true;
		case 2448: *sector_type=SECFMT_2448_MODE2; return true;
		default: return false;
	}
}

static void raw_track_read(track_file_t *f, u32 FAD, u8* dst, SectorFormat* sector_type,
		u8* subcode, SubcodeFormat* subcode_type)
{
	raw_track_t *r = (raw_track_t *)f;
	size_t got;

	/* A size there is no sector of: nothing is read. (That many bytes
	 * were read into the caller's one sector of room.) */
	if (!raw_track_format(r, sector_type))
	{
		*sector_type=SECFMT_2352;
		memset(dst, 0, 2352);
		return;
	}

	/* A file shorter than its track says gives part of a sector, or
	 * none of it: the rest is blank, not what the buffer held before. */
	got = core_fread_at(r->file, (u32)r->offset + FAD * r->fmt, dst, r->fmt);
	if (got < r->fmt)
		memset(dst + got, 0, r->fmt - got);
}

static void raw_track_prefetch(track_file_t *f, u32 FAD, u32 count)
{
	raw_track_t *r = (raw_track_t *)f;
	core_fprefetch(r->file, (u32)r->offset + FAD * r->fmt, (size_t)count * r->fmt);
}

/* Lent until the disc is closed: only from a file that is all there
 * (mapped, or in memory). */
static const u8* raw_track_lend(track_file_t *f, u32 FAD, SectorFormat* sector_type)
{
	raw_track_t *r = (raw_track_t *)f;
	size_t map_len;
	const u8* map = core_fmap(r->file, &map_len);
	u32 at = (u32)r->offset + FAD * r->fmt;

	if (!map || at > map_len || r->fmt > map_len - at || !raw_track_format(r, sector_type))
		return NULL;
	return map + at;
}

/* Until the next call: from such a file, and also from the piece of a
 * large deflated archive member that is decoded around the sector. */
static const u8* raw_track_view(track_file_t *f, u32 FAD, SectorFormat* sector_type)
{
	raw_track_t *r = (raw_track_t *)f;

	if (!raw_track_format(r, sector_type))
		return NULL;
	return core_fview(r->file, (u32)r->offset + FAD * r->fmt, r->fmt);
}

static void raw_track_destroy(track_file_t *f)
{
	raw_track_t *r = (raw_track_t *)f;

	if (r->file)
		core_fclose(r->file);
	free(r);
}

static const track_file_ops_t raw_track_ops =
{
	raw_track_read, raw_track_view, raw_track_prefetch, raw_track_lend, raw_track_destroy
};

track_file_t *raw_track_new(core_file *file, u32 file_offs, u32 first_fad, u32 secfmt)
{
	raw_track_t *r;

	verify(file!=0);
	if (!(r = (raw_track_t *)calloc(1, sizeof(*r))))
	{
		core_fclose(file);
		return NULL;
	}
	r->base.ops = &raw_track_ops;
	r->file = file;
	r->offset = (s32)(file_offs - first_fad * secfmt);
	r->fmt = secfmt;
	return &r->base;
}
