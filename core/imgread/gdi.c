/* A GD-ROM's .gdi sheet: a line with the count of tracks, then a line a
 * track - its number, where it starts, its control bits, its sector size,
 * its file (in quotes when the name has spaces) and where in the file it
 * starts. Read as the C++ stream it was read with read it (sheet.h). */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#include "common.h"
#include "sheet.h"

/* (longer than any path there is room for) */
#define GDI_NAME 600
/* (the sheet, read into memory made at the load: not on the stack) */
#define GDI_MAX 8193

static Disc* gdi_read(sheet_t *sheet, const char *file);

static Disc* load_gdi(const char* file)
{
	core_file* t=core_fopen(file);
	size_t gdi_len;
	char *gdi_data;
	sheet_t gdi;
	Disc* disc;

	if (!t)
		return NULL;

	gdi_len = core_fsize(t);

	if (gdi_len >= GDI_MAX)
	{
		WARN_LOG(GDROM, "GDI: file too big\n");
		core_fclose(t);
		return NULL;
	}
	if (!(gdi_data = (char *)calloc(1, gdi_len + 1)))
	{
		core_fclose(t);
		return NULL;
	}

	core_fread(t, gdi_data, gdi_len);
	core_fclose(t);

	/* (to the first 0 in it, as the stream took it) */
	sheet_init(&gdi, gdi_data, strlen(gdi_data));
	disc = gdi_read(&gdi, file);
	free(gdi_data);
	return disc;
}

/* The disc @gdi says, its files beside @file */
static Disc* gdi_read(sheet_t *sheet, const char *file)
{
	sheet_t gdi = *sheet;
	u32 iso_tc = 0;
	char path[512];
	long len;
	const char* delim;
	char* pathptr;
	Disc* disc;
	u32 TRACK=0,FADS=0,CTRL=0,SSIZE=0;
	s32 OFFSET=0;
	u32 i;

	sheet_u32(&gdi, &iso_tc);
	if (iso_tc == 0)
	{
		WARN_LOG(GDROM, "GDI: empty or invalid GDI file\n");
		return NULL;
	}
	/* A disc has 99 tracks at most, and so has its table of contents. (A
	 * count above that was taken as it came: a track was made for every
	 * one of them, lines in the file or not.) */
	if (iso_tc > 99)
	{
		WARN_LOG(GDROM, "GDI: invalid track count: %u\n", iso_tc);
		return NULL;
	}
	INFO_LOG(GDROM, "GDI : %d tracks\n", iso_tc);

	if (strlen(file) >= sizeof(path))
	{
		WARN_LOG(GDROM, "GDI: path too long\n");
		return NULL;
	}
	strcpy(path,file);
	len=(long)strlen(file);
	delim = path_get_archive_delim(file);
	while (len>=0)
	{
		if (path[len]=='\\' || path[len]=='/' || (delim && &file[len] == delim))
			break;
		len--;
	}
	len++;
	pathptr=&path[len];

	if (!(disc = disc_new()))
		return NULL;
	for (i=0;i<iso_tc;i++)
	{
		char track_filename[GDI_NAME];
		size_t name_len = 0;
		bool name_cut = false;
		char last = 0;
		Track tr;

		track_filename[0] = '\0';

		/* TRACK FADS CTRL SSIZE file OFFSET */
		sheet_u32(&gdi, &TRACK);
		sheet_u32(&gdi, &FADS);
		sheet_u32(&gdi, &CTRL);
		sheet_u32(&gdi, &SSIZE);
		/* (Fewer lines than the first line says, or one that is not
		 * numbers: the track read before was made again, in the same
		 * place and with no name for its file, once for each that was
		 * missing.) */
		if (gdi.fail)
		{
			WARN_LOG(GDROM, "GDI: track %u of %u is missing or invalid\n", i + 1, iso_tc);
			disc_free(disc);
			return NULL;
		}

		/* (A file that ends here - in the middle of a line, or of a name
		 * in quotes that is never closed - ends the reading: these loops
		 * went on for ever on one, the second adding to the name until
		 * there was no memory left.) */
		do {
			sheet_char(&gdi, &last, 0);
		} while (!gdi.fail && isspace((unsigned char)last));

		if (last == '"')
		{
			for(;;) {
				if (!sheet_char(&gdi, &last, 1) || last == '"')
					break;
				if (name_len + 1 < sizeof(track_filename))
					track_filename[name_len++] = last;
				else
					name_cut = true;
			}
			track_filename[name_len] = '\0';
		}
		else
		{
			/* the character read, and the word after it (after any
			 * whitespace) */
			char word[GDI_NAME - 1];
			word[0] = '\0';
			if (sheet_word(&gdi, word, sizeof(word)))
				name_cut = gdi.cut != 0;
			track_filename[0] = last;
			strcpy(track_filename + 1, word);
			name_len = strlen(track_filename);
		}

		sheet_s32(&gdi, &OFFSET);

		DEBUG_LOG(GDROM, "file[%d] \"%s\": FAD:%d, CTRL:%d, SSIZE:%d, OFFSET:%d\n", TRACK, track_filename, FADS, CTRL, SSIZE, OFFSET);

		memset(&tr, 0, sizeof(tr));
		tr.ADDR=0;
		tr.StartFAD=FADS+150;
		tr.EndFAD=0;		/* fill it in */
		tr.file=0;
		tr.CTRL = (u8)CTRL;

		/* Only the sector sizes the reader has a layout for: any other
		 * was read, that many bytes of it, into one sector of room. */
		if (SSIZE!=0 && SSIZE!=2048 && SSIZE!=2336 && SSIZE!=2352 && SSIZE!=2448)
		{
			WARN_LOG(GDROM, "GDI: track %d: unsupported sector size %u\n", TRACK, SSIZE);
			disc_free(disc);
			return NULL;
		}
		if (SSIZE!=0)
		{
			core_file *track_file;
			s64 track_bytes;

			/* (a name that does not fit is not copied past the end of
			 * the path; and a track whose file is not there is the image
			 * not loading, as for a cue sheet - it used to be a track
			 * with no file behind it, found out at the first read) */
			if (name_cut || name_len >= sizeof(path) - (size_t)len)
			{
				WARN_LOG(GDROM, "GDI: track %d: file name too long\n", TRACK);
				disc_free(disc);
				return NULL;
			}
			strcpy(pathptr, track_filename);
			track_file = core_fopen(path);
			if (track_file == NULL)
			{
				WARN_LOG(GDROM, "GDI: cannot open track %d: %s\n", TRACK, path);
				disc_free(disc);
				return NULL;
			}
			/* The track ends where its file does: the sectors between it
			 * and the next track are not on the disc. (It was made to run
			 * up to the next track.) */
			track_bytes = (s64)core_fsize(track_file) - OFFSET;
			if (track_bytes % SSIZE != 0)
				WARN_LOG(GDROM, "GDI: size of track %d is not a multiple of its sector size %u\n", TRACK, SSIZE);
			if (track_bytes >= (s64)SSIZE)
				tr.EndFAD = tr.StartFAD + (u32)(track_bytes / SSIZE) - 1;
			if (!(tr.file = raw_track_new(track_file,(u32)OFFSET,tr.StartFAD,SSIZE)))
			{
				disc_free(disc);
				return NULL;
			}
		}
		/* (a track with no file, or an empty one, still ends where the next starts) */
		if (disc->track_count != 0 && disc->tracks[disc->track_count - 1].EndFAD == 0)
			disc->tracks[disc->track_count - 1].EndFAD = tr.StartFAD - 1;
		disc_add_track(disc, &tr);
	}

	disc_fill_gd_session(disc);

	return disc;
}


Disc* gdi_parse(const char* file)
{
	size_t len=strlen(file);
	if (len>4)
	{
		if (string_is_equal_noncase(&file[len-4],".gdi"))
		{
			return load_gdi(file);
		}
	}
	return 0;
}
