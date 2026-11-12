/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */

/* A .cue sheet, read as the C++ stream it was read with read it (sheet.h) */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#include "types.h"
#include "common.h"
#include "sheet.h"

/* (longer than a word of the sheet's or a path there is room for) */
#define CUE_WORD 1024

static u32 getSectorSize(const char *type) {
		if (!strcmp(type, "AUDIO"))
			return 2352;	/* PCM Audio */
		else if (!strcmp(type, "CDG"))
			return 2352;	/* Karaoke cd+g */
		else if (!strcmp(type, "MODE1/2048"))
			return 2048;	/* CDROM Mode1 Data (cooked) */
		else if (!strcmp(type, "MODE1/2352"))
			return 2352;	/* CDROM Mode1 Data (raw) */
		else if (!strcmp(type, "MODE2/2336"))
			return 2336;	/* CDROM XA Mode2 Data */
		else if (!strcmp(type, "MODE2/2352"))
			return 2352;	/* CDROM XA Mode2 Data */
		else if (!strcmp(type, "CDI/2336"))
			return 2336;	/* CDI Mode2 Data */
		else if (!strcmp(type, "CDI/2352"))
			return 2352;	/* CDI Mode2 Data */
		else
			return 0;
}

/* What a sheet is read into, and the words it is read in: made at the
 * load, not on the stack */
#define CUE_MAX (64 * 1024)
typedef struct cue_read
{
	char data[CUE_MAX];
	char token[CUE_WORD];
	char track_type[CUE_WORD];
	char track_filename[CUE_WORD];
	char word[CUE_WORD];
} cue_read_t;

/* The disc not made: the file the sheet had open closed, and @disc */
static Disc* cue_fail(cue_read_t *r, Disc *disc, core_file *file_handle)
{
	if (file_handle != NULL)
		core_fclose(file_handle);
	disc_free(disc);
	free(r);
	return NULL;
}

Disc* cue_parse(const char* file)
{
	size_t len = strlen(file);
	core_file* fsource;
	size_t cue_len;
	cue_read_t *r;
	sheet_t cuesheet;
	char path[512];
	const char* delim;
	long dir_end;
	char* pathptr;
	Disc* disc;
	u32 current_fad = 150;
	u32 track_number = (u32)-1;
	u32 session_number = 0;
	bool high_density = false;
	/* The FILE being read: where on the disc it starts, the file itself
	 * until its first track takes it, and the sector size of its tracks
	 * (0 until its first TRACK). A file can hold several tracks. */
	bool have_file = false;
	core_file* file_handle = NULL;
	size_t file_size = 0;
	u32 file_start_fad = 0;
	u32 sector_size = 0;
	/* The first track of the second session: the gap that session starts
	 * after takes in that track's pregap already. */
	bool first_of_session = false;

	/* Only try to open .cue files */
	if (len > 4 && !string_is_equal_noncase(&file[len - 4], ".cue"))
		return NULL;

	fsource = core_fopen(file);

	if (fsource == NULL)
		return NULL;

	cue_len = core_fsize(fsource);

	if (cue_len >= CUE_MAX)
	{
		WARN_LOG(GDROM, "CUE parse error: CUE file too big");
		core_fclose(fsource);
		return NULL;
	}
	if (!(r = (cue_read_t *)calloc(1, sizeof(*r))))
	{
		core_fclose(fsource);
		return NULL;
	}

	core_fread(fsource, r->data, cue_len);
	core_fclose(fsource);

	/* (to the first 0 in it, as the stream took it) */
	sheet_init(&cuesheet, r->data, strlen(r->data));

	if (len >= sizeof(path))
	{
		WARN_LOG(GDROM, "CUE: path too long");
		free(r);
		return NULL;
	}
	strcpy(path, file);
	delim = path_get_archive_delim(file);
	dir_end = (long)len;
	while (dir_end >= 0)
	{
		if (path[dir_end]=='\\' || path[dir_end]=='/' || (delim && &file[dir_end] == delim))
			break;
		dir_end--;
	}
	dir_end++;
	pathptr = &path[dir_end];

	if (!(disc = disc_new()))
	{
		free(r);
		return NULL;
	}

	while (!cuesheet.eof)
	{
		char *token = r->token;

		/* A number that was not one - a session, track or index given in
		 * letters, or too large - stops the stream from reading at all,
		 * short of the end of the sheet, and this loop then went round for
		 * ever. The sheet is read on from where that was. */
		if (cuesheet.fail)
			sheet_clear(&cuesheet);
		token[0] = '\0';
		sheet_word(&cuesheet, token, CUE_WORD);

		if (!strcmp(token, "REM"))
		{
			sheet_word(&cuesheet, token, CUE_WORD);
			if (!strcmp(token, "SESSION"))
			{
				/* Multi-session */
				u32 cur_session = 0;
				sheet_u32(&cuesheet, &cur_session);
				if (cur_session == 0)
					WARN_LOG(GDROM, "CUE parse error: invalid session number %s", token);
				else if (cur_session != session_number)
				{
					session_number = cur_session;
					if (session_number == 2)
					{
						/* session 1 lead-out: 01:30:00, session 2 lead-in: 01:00:00, pregap: 00:02:00 */
						current_fad += 11400;
						first_of_session = true;
					}

					if (!disc_add_session(disc, disc->track_count + 1, current_fad))
					{
						WARN_LOG(GDROM, "CUE file: too many sessions");
						return cue_fail(r, disc, file_handle);
					}
					DEBUG_LOG(GDROM, "session[%u]: 1st track: %d FAD:%d", disc->session_count,
							disc->sessions[disc->session_count - 1].FirstTrack, current_fad);
				}
			}
			else
			{
				/* GD-Rom */
				if (!strcmp(token, "HIGH-DENSITY"))
				{
					current_fad = 45000 + 150;
					high_density = true;
				}
				else if (strcmp(token, "SINGLE-DENSITY"))
					WARN_LOG(GDROM, "CUE parse error: unrecognized REM token %s. Expected SINGLE-DENSITY, HIGH-DENSITY or SESSION", token);
				sheet_word(&cuesheet, token, CUE_WORD);
				if (strcmp(token, "AREA"))
					WARN_LOG(GDROM, "CUE parse error: unrecognized REM token %s. Expected AREA", token);
			}
			/* (a new area or session starts somewhere else on the disc: its tracks are in a FILE of their own) */
			if (file_handle != NULL)
				core_fclose(file_handle);
			file_handle = NULL;
			have_file = false;
		}
		else if (!strcmp(token, "FILE"))
		{
			char *track_filename = r->track_filename;
			size_t name_len = 0;
			bool name_cut = false;
			char last = 0;

			track_filename[0] = '\0';
			do {
				sheet_char(&cuesheet, &last, 0);
			} while (!cuesheet.fail && isspace((unsigned char)last));

			if (last == '"')
			{
				for (;;) {
					/* (to the closing quote, or the end of a file that has none) */
					if (!sheet_char(&cuesheet, &last, 1) || last == '"')
						break;
					if (name_len + 1 < CUE_WORD)
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
				r->word[0] = '\0';
				if (sheet_word(&cuesheet, r->word, CUE_WORD - 1))
					name_cut = cuesheet.cut != 0;
				track_filename[0] = last;
				strcpy(track_filename + 1, r->word);
				name_len = strlen(track_filename);
			}
			sheet_word(&cuesheet, token, CUE_WORD);	/* BINARY */
			if (strcmp(token, "BINARY"))
				WARN_LOG(GDROM, "CUE parse error: unrecognized FILE token %s. Expected BINARY", token);

			if (file_handle != NULL)
				core_fclose(file_handle);
			file_handle = NULL;
			have_file = false;
			if (name_cut || name_len >= sizeof(path) - (size_t)dir_end)
			{
				WARN_LOG(GDROM, "CUE file: file name too long: %s", track_filename);
				return cue_fail(r, disc, NULL);
			}
			strcpy(pathptr, track_filename);
			file_handle = core_fopen(path);
			if (file_handle == NULL)
			{
				WARN_LOG(GDROM, "CUE file: cannot open %s", path);
				return cue_fail(r, disc, NULL);
			}
			have_file = true;
			file_size = core_fsize(file_handle);
			file_start_fad = current_fad;
			sector_size = 0;
			r->track_type[0] = '\0';
		}
		else if (!strcmp(token, "TRACK"))
		{
			bool first_of_file;

			sheet_u32(&cuesheet, &track_number);
			sheet_word(&cuesheet, r->track_type, CUE_WORD);
			if (!have_file)
			{
				WARN_LOG(GDROM, "CUE file: track %d is not in a FILE", track_number);
				return cue_fail(r, disc, file_handle);
			}
			first_of_file = sector_size == 0;
			sector_size = getSectorSize(r->track_type);
			if (sector_size == 0)
			{
				WARN_LOG(GDROM, "CUE file: track %d has unknown sector type: %s", track_number, r->track_type);
				return cue_fail(r, disc, file_handle);
			}
			if (file_size % sector_size != 0)
				WARN_LOG(GDROM, "Warning: Size of track %s is not multiple of sector size %d", pathptr, sector_size);
			/* The file's sectors are on the disc one after the other from
			 * where it starts. (Tracks of different sizes in one file are
			 * not laid out right: the first one's size is taken for all.) */
			if (first_of_file)
				current_fad = file_start_fad + (u32)(file_size / sector_size);
		}
		else if (!strcmp(token, "INDEX"))
		{
			u32 index_num = 0;
			sheet_u32(&cuesheet, &index_num);
			if (index_num == 1 && sector_size != 0)
			{
				/* The track starts at INDEX 01, which is minutes, seconds
				 * and frames into its FILE: what comes before it in the
				 * file is the track's pregap (or the end of the track
				 * before, in the same file). (It was taken as the start
				 * of the file, and two seconds were added to the start of
				 * a track of music whatever the sheet said: a track with
				 * no pregap lost its first two seconds - the Sonic Team
				 * jingle of ChuChu Rocket - and a data track with one
				 * started early.) */
				u32 min = 0, sec = 0, frame = 0;
				u32 index_fad = 0;
				Track t;
				core_file* track_file;

				sheet_word(&cuesheet, token, CUE_WORD);
				if (strlen(token) == 8 && sscanf(token, "%2u:%2u:%2u", &min, &sec, &frame) == 3
						&& sec < 60 && frame < 75)
					index_fad = frame + 75 * (sec + 60 * min);
				else
					WARN_LOG(GDROM, "CUE file: track %d: INDEX 01 %s is not a time", track_number, token);
				/* (no further into the file than it goes) */
				if (index_fad > current_fad - file_start_fad)
				{
					WARN_LOG(GDROM, "CUE file: track %d: INDEX 01 %s is past the end of its file", track_number, token);
					index_fad = current_fad - file_start_fad;
				}
				if (first_of_session && index_fad != 0)
				{
					/* The pregap of the first track of the second session
					 * is in the gap counted before it: what of it is in the
					 * file is not on the disc a second time. */
					const u32 pregap = index_fad < 150 ? index_fad : 150;
					file_start_fad -= pregap;
					current_fad -= pregap;
				}
				first_of_session = false;

				memset(&t, 0, sizeof(t));
				t.ADDR = 0;
				t.StartFAD = file_start_fad + index_fad;
				t.EndFAD = current_fad - 1;
				t.CTRL = (!strcmp(r->track_type, "AUDIO") || !strcmp(r->track_type, "CDG")) ? 0 : 4;

				/* (a disc has 99 tracks at most, and so has its table of contents) */
				if (disc->track_count >= 99)
				{
					WARN_LOG(GDROM, "CUE file: more than 99 tracks");
					return cue_fail(r, disc, file_handle);
				}
				/* The file's first track takes it as it was opened; another track in it opens it again. */
				track_file = file_handle;
				file_handle = NULL;
				if (track_file == NULL)
					track_file = core_fopen(path);
				if (track_file == NULL)
				{
					WARN_LOG(GDROM, "CUE file: cannot open track %d: %s", track_number, path);
					return cue_fail(r, disc, NULL);
				}
				DEBUG_LOG(GDROM, "file[%u] \"%s\": session %d type %s FAD:%d -> %d", disc->track_count + 1, pathptr, session_number, r->track_type, t.StartFAD, t.EndFAD);

				if (!(t.file = raw_track_new(track_file, index_fad * sector_size, t.StartFAD, sector_size)))
					return cue_fail(r, disc, NULL);
				/* (the track before ends where this one starts, when they share a file) */
				if (disc->track_count != 0 && disc->tracks[disc->track_count - 1].EndFAD >= t.StartFAD)
					disc->tracks[disc->track_count - 1].EndFAD = t.StartFAD - 1;
				disc_add_track(disc, &t);

				track_number = (u32)-1;
			}
		}
	}
	if (file_handle != NULL)
		core_fclose(file_handle);
	free(r);
	if (disc->track_count == 0)
	{
		WARN_LOG(GDROM, "CUE parse error: failed to parse or invalid file with 0 tracks");
		disc_free(disc);
		return NULL;
	}

	if (session_number == 0 && high_density)
		disc_fill_gd_session(disc);
	else
	{
		if (session_number == 0)
		{
			/* No GD-ROM area and no sessions: a CD of one session, an
			 * audio CD or one with data. (It was made a GD-ROM, of which
			 * it had nothing where a GD-ROM has it.) */
			disc_add_session(disc, 1, disc->tracks[0].StartFAD);
			disc->type = CdRom;
		}
		else
			disc->type = CdRom_XA;
		disc->LeadOut.ADDR = 1;	/* subcode-q channel */
		disc->LeadOut.CTRL = 4;	/* data */
		disc->EndFAD = disc->LeadOut.StartFAD = current_fad;
	}

	return disc;
}
