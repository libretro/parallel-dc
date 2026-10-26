#include "common.h"

#include "chd_image.h"

#include <vfs/vfs_prefetch.h>

/* tracks are padded to a multiple of this many frames */
const uint32_t CD_TRACK_PADDING = 4;

/* Which read-ahead a track reads through. CD audio and data are read
 * from different places on the disc at once (music playing while a
 * game streams), so each has its own, and neither throws out the
 * hunks the other is about to use. */
enum { CHD_STREAM_DATA, CHD_STREAM_AUDIO, CHD_STREAMS };

/* Hunks each stream's thread decodes ahead of its reader. */
static const unsigned chd_stream_slots[CHD_STREAMS] = { 8, 4 };

struct CHDDisc : Disc
{
	chd_image_t* chd;
	u8* hunk_mem;
	u32 old_hunk;

	u32 hunkbytes;
	u32 sph;
	u32 total_hunks = 0;

	/* A stream's read-ahead: libretro-common's prefetch thread, which
	 * decodes the hunks ahead of the reader on an image handle of its
	 * own. One window is one hunk. */
	struct Stream
	{
		CHDDisc*             disc;
		chd_image_t*         chd;
		struct vfs_prefetch* p;
	} streams[CHD_STREAMS];

	static const u32 NO_HUNK = 0xFFFFFFFFu;

	static int64_t stream_fetch(void *user, uint64_t off, uint8_t *buf, size_t len)
	{
		Stream *st = (Stream *)user;
		u32 hunk = (u32)(off / st->disc->hunkbytes);

		if (hunk >= st->disc->total_hunks)
			return 0;
		if (len < st->disc->hunkbytes || !chd_image_read_hunk(st->chd, hunk, buf))
			return -1;
		return (int64_t)st->disc->hunkbytes;
	}

	/* Decompress @hunk into hunk_mem on this thread. A hunk that fails to
	 * decode reads as zeros and is not kept, so the next read retries it. */
	void hunk_read_sync(u32 hunk)
	{
		if (chd_image_read_hunk(chd, hunk, hunk_mem))
			old_hunk = hunk;
		else
		{
			WARN_LOG(GDROM, "chd: hunk %u failed to decode", hunk);
			memset(hunk_mem, 0, hunkbytes);
			old_hunk = NO_HUNK;
		}
	}

	/* Where the sector at @fad_offs is decoded: in the stream's window
	 * when its thread got there first, else decoded here. Valid until
	 * the next read on this disc. */
	const u8* sector_get(unsigned stream, u32 fad_offs)
	{
		const u32 hunk = fad_offs / sph;
		const u32 in   = (fad_offs % sph) * (2352 + 96);
		const u8* ptr;

		if (streams[stream].p
				&& vfs_prefetch_view(streams[stream].p, (uint64_t)hunk * hunkbytes + in, &ptr) > 0)
			return ptr;
		if (hunk != old_hunk)
			hunk_read_sync(hunk);
		return hunk_mem + in;
	}

	/* The stream's thread starts on @hunk now, ahead of the reads. */
	void stream_aim(unsigned stream, u32 hunk)
	{
		if (streams[stream].p)
			vfs_prefetch_seek(streams[stream].p, (uint64_t)hunk * hunkbytes);
	}

	void start_streams(const char* file)
	{
		for (unsigned i = 0; i < CHD_STREAMS; i++)
		{
			Stream& st = streams[i];
			st.disc = this;
			if (!(st.chd = chd_image_open(file, NULL, 0)))
				continue;
			if (!(st.p = vfs_prefetch_new(stream_fetch, &st, hunkbytes,
					chd_stream_slots[i], 0)))
			{
				chd_image_close(st.chd);
				st.chd = nullptr;
			}
		}
	}

	void stop_streams()
	{
		for (unsigned i = 0; i < CHD_STREAMS; i++)
		{
			vfs_prefetch_free(streams[i].p);
			streams[i].p = nullptr;
			chd_image_close(streams[i].chd);
			streams[i].chd = nullptr;
		}
	}

	CHDDisc()
	{
		chd=0;
		hunk_mem=0;
		for (unsigned i = 0; i < CHD_STREAMS; i++)
		{
			streams[i].disc = this;
			streams[i].chd  = nullptr;
			streams[i].p    = nullptr;
		}
	}

	bool TryOpen(const char* file);

	~CHDDisc()
	{
		stop_streams();
		if (hunk_mem)
			delete [] hunk_mem;
		chd_image_close(chd);
	}
};

struct CHDTrack : TrackFile
{
	CHDDisc* disc;
	u32 StartFAD;
	u32 Offset;
	u32 fmt;
	unsigned stream;
	bool swap_bytes;

	CHDTrack(CHDDisc* disc, u32 StartFAD,u32 Offset, u32 fmt, unsigned stream, bool swap_bytes)
	{
		this->disc=disc;
		this->StartFAD=StartFAD;
		this->Offset=Offset;
		this->fmt=fmt;
		this->stream=stream;
		this->swap_bytes = swap_bytes;
	}

	virtual void Read(u32 FAD,u8* dst,SectorFormat* sector_type,u8* subcode,SubcodeFormat* subcode_type)
	{
		const u8* from = disc->sector_get(stream, FAD + Offset);

		if (swap_bytes)
		{
			/* The other byte order, made on the way across: 16 bits at a
			 * time, which the compiler does sixteen bytes at a time. (It
			 * was copied, and then swapped where it lay a byte at a time.) */
			for (u32 i = 0; i + 1 < fmt; i += 2)
			{
				u16 w;
				memcpy(&w, from + i, 2);
				w = (u16)((w << 8) | (w >> 8));
				memcpy(dst + i, &w, 2);
			}
		}
		else
			memcpy(dst,from,fmt);

		*sector_type=Format();

		//While space is reserved for it, the images contain no actual subcodes
		//memcpy(subcode,disc->hunk_mem+hunk_ofs*(2352+96)+2352,96);
		*subcode_type=SUBFMT_NONE;
	}

	// (a CD's mode 2 track can be kept without the sync and header: 2336 bytes)
	SectorFormat Format() const
	{
		return fmt == 2352 ? SECFMT_2352 : fmt == 2336 ? SECFMT_2336_MODE2 : SECFMT_2048_MODE1;
	}

	/* The compressed bytes of every hunk the sectors span, so neither
	 * this thread nor the read-ahead stalls on a cold page (all handles
	 * are on the same file, so one hint serves them all), and the
	 * stream's thread set decoding from the first one. */
	virtual void Prefetch(u32 FAD, u32 count)
	{
		u32 first = (FAD + Offset) / disc->sph;
		u32 last  = (FAD + Offset + count - 1) / disc->sph;

		if (!count)
			return;
		chd_image_prefetch(disc->chd, first, last - first + 1);
		disc->stream_aim(stream, first);
	}

	/* The sector inside the decoded hunk; a track whose bytes have to
	 * be swapped is read through a buffer. */
	virtual const u8* View(u32 FAD, SectorFormat* sector_type)
	{
		if (swap_bytes)
			return NULL;
		*sector_type = Format();
		return disc->sector_get(stream, FAD + Offset);
	}
};

bool CHDDisc::TryOpen(const char* file)
{
	char err[512];

	err[0] = '\0';
	chd = chd_image_open(file, err, sizeof(err));
	if (chd == nullptr)
	{
		INFO_LOG(GDROM, "chd: %s", err[0] ? err : "out of memory");
		return false;
	}

	INFO_LOG(GDROM, "chd: parsing file %s", file);

	hunkbytes = chd_image_hunk_bytes(chd);
	total_hunks = chd_image_hunk_count(chd);
	hunk_mem = new u8[hunkbytes];
	old_hunk = NO_HUNK;

	sph = hunkbytes/(2352+96);

	if (hunkbytes%(2352+96)!=0)
	{
		INFO_LOG(GDROM, "chd: hunkbytes is invalid, %d\n",hunkbytes);
		return false;
	}

	u32 total_frames = 150;

	u32 Offset = 0;
	bool gdrom = chd_image_version(chd) < 5;	// a CD's image is only read from version 5 on
	u32 last_pregap = 0;

	for(;;)
	{
		chd_image_track_t meta;

		if (!chd_image_track(chd, (uint32_t)tracks.size(), &meta))
			break;

		int tkid = meta.track, frames = meta.frames, pregap = meta.pregap, postgap = meta.postgap;
		const char *type = meta.type, *subtype = meta.subtype;
		if (meta.gdrom)
			gdrom = true;

		/* The sector sizes of the CD and GD-ROM track types. The subcode
		 * is kept in its 96 bytes after each sector, whatever the subtype,
		 * and the sectors are where they are with it or without. */
		u32 sector_size = 0;
		if (!strcmp(type, "AUDIO") || !strcmp(type, "MODE1_RAW") || !strcmp(type, "MODE2_RAW"))
			sector_size = 2352;
		else if (!strcmp(type, "MODE1"))
			sector_size = 2048;
		else if (!strcmp(type, "MODE2"))
			sector_size = 2336;
		/* A pregap of type "V..." is kept in the image, at the start of
		 * the track's frames; any other is on the disc and not in the
		 * image. A postgap is never in the image. */
		const u32 pregap_kept = pregap > 0 && meta.pgtype[0] == 'V' ? (u32)pregap : 0;
		if (tkid!=(tracks.size()+1) || sector_size == 0 || frames < 0 || pregap < 0 || postgap < 0 || (int)pregap_kept > frames)
		{
			INFO_LOG(GDROM, "chd: track %d of type %s, %d frames, pregap %d is not supported", tkid, type, frames, pregap);
			return false;
		}
		if (strcmp(subtype, "NONE") != 0)
			WARN_LOG(GDROM, "chd: track %d has subcode (%s): not used", tkid, subtype);
		// (a disc has 99 tracks at most, and so has its table of contents)
		if (tracks.size() >= 99)
		{
			INFO_LOG(GDROM, "chd: more than 99 tracks");
			return false;
		}
		DEBUG_LOG(GDROM, "chd: track %d %s %d frames", tkid, type, frames);
		Track t;
		// (where INDEX 01 is: after the pregap)
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
		/* Music is kept in the other byte order in a version 5 image (and
		 * in any of a CD, which is read from version 5 only). */
		t.file = new CHDTrack(this, t.StartFAD, Offset + pregap_kept - t.StartFAD, sector_size,
				t.CTRL == 0 ? CHD_STREAM_AUDIO : CHD_STREAM_DATA,
				t.CTRL == 0 && chd_image_version(chd) >= 5);

		int padded = (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING;
		Offset += padded * CD_TRACK_PADDING;

		tracks.push_back(t);
	}

	// (a hard disk's image, or anything else with no track in it, is no disc)
	if (tracks.empty())
	{
		INFO_LOG(GDROM, "chd: no track found");
		return false;
	}

	if (gdrom)
	{
		if (total_frames!=549300 || tracks.size()<3)
			WARN_LOG(GDROM, "WARNING: chd: Total frames is wrong: %u frames in %zu tracks", total_frames, tracks.size());

		FillGDSession();
	}
	else
	{
		/* A CD. The image does not say where its sessions are: one that
		 * ends in a data track after others is taken for a MIL-CD, its
		 * last track alone in a second session; any other has one. */
		Session ses;
		ses.FirstTrack = 1;
		ses.StartFAD = tracks[0].StartFAD;
		sessions.push_back(ses);
		LeadOut.ADDR = 1;	// subcode-q channel
		LeadOut.CTRL = 4;	// data
		if (tracks.size() > 1 && tracks.back().CTRL == 4)
		{
			/* Between the sessions: the first one's lead-out (01:30:00),
			 * the second's lead-in (01:00:00) and the pregap of its
			 * track (00:02:00), which is counted already when the image
			 * has it. */
			const u32 gap = 6750 + 4500 + 150 - std::min(last_pregap, (u32)150);
			type = CdRom_XA;
			tracks.back().StartFAD += gap;
			tracks.back().EndFAD += gap;
			((CHDTrack *)tracks.back().file)->Offset -= gap;
			ses.FirstTrack = tracks.size();
			ses.StartFAD = tracks.back().StartFAD;
			sessions.push_back(ses);
			EndFAD = LeadOut.StartFAD = total_frames + gap;
		}
		else
		{
			type = CdRom;
			for (size_t i = 0; tracks.size() > 1 && i < tracks.size(); i++)
				if (tracks[i].CTRL == 4)
					type = CdRom_XA;
			EndFAD = LeadOut.StartFAD = total_frames;
		}
	}

	start_streams(file);

	return true;
}


Disc* chd_parse(const char* file)
{
	// Only try to open .chd files
	size_t len = strlen(file);
	if (len > 4 && stricmp( &file[len - 4], ".chd"))
		return nullptr;

	CHDDisc* rv = new CHDDisc();

	if (rv->TryOpen(file))
		return rv;
	else
	{
		delete rv;
		return 0;
	}
}
