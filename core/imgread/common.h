/* The disc in the drive: the images' readers (gdi.c, cue.c, cdi.c, chd.c)
 * make a Disc (disc.c) of tracks, each read through a track_file_t of its
 * own kind, and the drive (common.c, ImgReader.c: the GD-ROM's and the HLE
 * BIOS's code) reads its sectors and table of contents. C89. */
#ifndef IMGREAD_COMMON_H
#define IMGREAD_COMMON_H

#include "gd_driver.h"
#include "deps/coreio/coreio.h"

#ifdef __cplusplus
extern "C" {
#endif

extern u32 NullDriveDiscType;

/*
Mode2 Subheader:

"1" file number for identification of nested files (0 = not interleaver.)

"2" channel number, the infantry of the various channels are selectable for playback

"3" SUBMODE byte:

7: last sector of file (EOF)
6: Real-time sector (f.Echtzeitwiedergabe without error correction)
5: Form 2 (bit = 1), form 1 (bit = 0)
4: Trigger on (depending on OS)
3: data sector (Submodebyte 3 or 2 or 1)
2: ADPCM audio sector "
1: Video-sector "
0: last sector of a record (EOR)
"4" Encoding type of audio (eg mono / stereo) and video data (in this byte data sectors is set to 0)

"5" to "8" is the repetition of "1" through "4"


RAW: 2352
MODE1:
SYNC (12) | HEAD (4) | data (2048) | edc (4) | space (8) | ecc (276)
MODE2:
SYNC (12) | HEAD (4) | sub-head (8) | sector_data (2328)
  -form1 sector_data:
   data (2048) | edc (4) | ecc (276)

  -form2 sector_data:
   data (2324) |edc(4)
*/

typedef enum SectorFormat
{
	SECFMT_2352,				/* full sector */
	SECFMT_2048_MODE1,			/* 2048 user byte, form1 sector */
	SECFMT_2048_MODE2_FORM1,	/* 2048 user bytes, form2m1 sector */
	SECFMT_2336_MODE2,			/* 2336 user bytes, */
	SECFMT_2448_MODE2			/* a whole sector and its 96 bytes of subcode */
} SectorFormat;

typedef enum SubcodeFormat
{
	SUBFMT_NONE,				/* No subcode info */
	SUBFMT_96					/* raw 96-byte subcode info */
} SubcodeFormat;

bool ConvertSector(const u8* in_buff , u8* out_buff , int from , int to,int sector);

/* @fileflags for GetFile() (0 for none) */
bool InitDrive(u32 fileflags);
void TermDrive(void);
bool DiscSwap(u32 fileflags);
void DiscOpenLid(void);
void DiscCloseEmpty(void);
bool DiscPutBack(const char* fn);
extern signed int sns_asc;
extern signed int sns_ascq;
extern signed int sns_key;

void GetDriveToc(u32* to,DiskArea area);
void GetDriveSector(u8 * buff,u32 StartSector,u32 SectorCount,u32 secsz);

void GetDriveSessionInfo(u8* to,u8 session);
/* @szParse and @flags 0 for none */
int GetFile(char *szFileName, char *szParse, u32 flags);
extern u8 q_subchannel[96];

typedef struct Session
{
	u32 StartFAD;			/* session's start fad */
	u8 FirstTrack;			/* session's first track */
} Session;

/* Where a track's sectors come from: an image's kind of track embeds one
 * first, and the disc reads through its ops */
typedef struct track_file track_file_t;
typedef struct track_file_ops
{
	void (*read)(track_file_t *f, u32 FAD, u8 *dst, SectorFormat *sector_type,
			u8 *subcode, SubcodeFormat *subcode_type);
	/* The sector's bytes where they already are - a mapped file, a
	 * decoded hunk - or NULL when they have to be read into a buffer.
	 * Valid until the next call on the same disc. (NULL: never) */
	const u8 *(*view)(track_file_t *f, u32 FAD, SectorFormat *sector_type);
	/* Read-ahead hint for the sectors a read is about to ask for (NULL:
	 * none) */
	void (*prefetch)(track_file_t *f, u32 FAD, u32 count);
	/* Like view, but the bytes stay where they are until the disc is
	 * closed: a mapped file can lend them, a decoded hunk cannot. (NULL:
	 * never) */
	const u8 *(*lend)(track_file_t *f, u32 FAD, SectorFormat *sector_type);
	void (*destroy)(track_file_t *f);
} track_file_ops_t;
struct track_file
{
	const track_file_ops_t *ops;
};

typedef struct Track
{
	track_file_t *file;	/* handler for actual IO, NULL for none */
	u32 StartFAD;		/* Start FAD */
	u32 EndFAD;			/* End FAD, 0 for up to the next track */
	u8 CTRL;
	u8 ADDR;
} Track;

/* A disc has 99 tracks at most, and so has its table of contents; and as
 * many sessions, a track each */
#define DISC_TRACKS   99
#define DISC_SESSIONS 99

typedef struct Disc
{
	Session sessions[DISC_SESSIONS];	/* info for sessions */
	u32 session_count;
	Track tracks[DISC_TRACKS];			/* info for tracks */
	u32 track_count;
	Track LeadOut;				/* info for lead out track (can't read from here) */
	u32 EndFAD;					/* Last valid disc sector */
	DiscType type;
	/* What an image's reader keeps for the disc's lifetime, let go of by
	 * owner_free() when the disc is (NULL for nothing) */
	void *owner;
	void (*owner_free)(void *owner);
} Disc;

/* All zero, no tracks: NULL for no memory */
Disc *disc_new(void);
/* What the disc's reader kept, its tracks' files, and it */
void disc_free(Disc *d);
/* @t, its file the disc's now: false, and its file closed, past
 * DISC_TRACKS */
bool disc_add_track(Disc *d, const Track *t);
/* false past DISC_SESSIONS */
bool disc_add_session(Disc *d, u32 first_track, u32 start_fad);
/* From the track that holds FAD, or false */
bool disc_read_sector(Disc *d, u32 FAD, u8 *dst, SectorFormat *sector_type,
		u8 *subcode, SubcodeFormat *subcode_type);
/* Hints the tracks holding [FAD, FAD + count) that it is about to be
 * read, before the reads themselves land. */
void disc_prefetch(Disc *d, u32 FAD, u32 count);
/* The sector in place from the track that holds it, or NULL. */
const u8 *disc_sector_view(Disc *d, u32 FAD, SectorFormat *sector_type);
/* The sector lent for the disc's lifetime, or NULL. */
const u8 *disc_sector_lend(Disc *d, u32 FAD, SectorFormat *sector_type);
/* @count sectors of @fmt bytes from FAD into @dst: each sector converted
 * straight out of the image when the track can show it in place; only a
 * track that cannot goes through a buffer. */
void disc_read_sectors(Disc *d, u32 FAD, u32 count, u8 *dst, u32 fmt);
/* The two sessions and the lead-out of a GD-ROM */
void disc_fill_gd_session(Disc *d);

extern Disc* disc;

Disc* OpenDisc(const char* fn);

/* A track read straight from a file, @secfmt bytes a sector, its sector
 * at @first_fad at @file_offs: NULL, and @file closed, for no memory */
track_file_t *raw_track_new(core_file *file, u32 file_offs, u32 first_fad, u32 secfmt);

DiscType GuessDiscType(bool m1, bool m2, bool da);

/* (each image kind's: NULL for not one, or not one that can be read) */
Disc* chd_parse(const char* file);
Disc* gdi_parse(const char* file);
Disc* cdi_parse(const char* file);
Disc* cue_parse(const char* file);

#ifdef __cplusplus
}
#endif

#endif
