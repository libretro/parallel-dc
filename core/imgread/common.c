/* The disc in the drive: see common.h */
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "archive/archive.h"

u32 NullDriveDiscType;
Disc* disc;

static Disc*(* const drivers[])(const char* path)=
{
	chd_parse,
	gdi_parse,
	cdi_parse,
	cue_parse,
	0
};

Disc* OpenDisc(const char* fn)
{
	Disc* rv = NULL;
	char archived[1024];
	int i;

	if (archive_resolve_disc(fn, archived, sizeof(archived)))
		fn = archived;

	for (i=0;drivers[i] && !(rv=drivers[i](fn));i++) ;

	return rv;
}

/* @swap: a disc put in while the game runs. The drive is told with
 * gd_disc_inserted(), and is not ready at once. */
static bool InitDrive_(char* fn, bool swap)
{
	gd_swap_cancel();
	TermDrive();

	/* try all drivers */
	disc = OpenDisc(fn);

	if (disc != NULL)
	{
		INFO_LOG(GDROM, "gdrom: Opened image \"%s\"\n", fn);
		if (swap)
		{
			gd_disc_inserted();
			return true;
		}
		NullDriveDiscType = Busy;
	}
	else
	{
		INFO_LOG(GDROM, "gdrom: Failed to open image \"%s\"\n", fn);
		NullDriveDiscType = NoDisk; /* no disc :) */
	}
	libCore_gdrom_disc_change();

	return disc != NULL;
}

bool InitDrive(u32 fileflags)
{
	char fn[512];
	int gfrv = 0;

	if (settings.imgread.LoadDefaultImage)
	{
		INFO_LOG(GDROM, "Loading default image \"%s\"\n", settings.imgread.DefaultImage);
		if (!InitDrive_(settings.imgread.DefaultImage, false))
		{
			msgboxf("Default image \"%s\" failed to load", MBX_ICONERROR, settings.imgread.DefaultImage);
			return false;
		}
      return true;
	}

	fn[0] = '\0';
   if (settings.System == DC_PLATFORM_DREAMCAST)
      gfrv=GetFile(fn,0,fileflags);

   switch (gfrv)
   {
      case 0:
         NullDriveDiscType=NoDisk;
         gd_setdisc();
         sns_asc=0x29;
         sns_ascq=0x00;
         sns_key=0x6;
         return true;
      case -1:
         return false;
   }

	if (!InitDrive_(fn, false))
	{
		/* msgboxf("Selected image failed to load",MBX_ICONERROR); */
			NullDriveDiscType=NoDisk;
			gd_setdisc();
			sns_asc=0x29;
			sns_ascq=0x00;
			sns_key=0x6;
	}
   return true;
}

void DiscOpenLid(void)
{
	gd_swap_cancel();
	TermDrive();
	NullDriveDiscType = Open;
	gd_setdisc();
}

/* The lid shut on an empty drive: there is nothing to spin up */
void DiscCloseEmpty(void)
{
	gd_swap_cancel();
	TermDrive();
	NullDriveDiscType = NoDisk;
	gd_setdisc();
}

/* A save state made with this image in the drive: the image is opened and */
/* nothing else is done, since what the drive was doing comes with the state. */
bool DiscPutBack(const char* fn)
{
	TermDrive();
	disc = OpenDisc(fn);
	return disc != NULL;
}

bool DiscSwap(u32 fileflags)
{
	char fn[512];
	int gfrv = 0;

	/* What the game is told, and when: see gd_disc_inserted() */
	if (settings.imgread.LoadDefaultImage)
	{
		INFO_LOG(GDROM, "Loading default image \"%s\"\n", settings.imgread.DefaultImage);
		if (!InitDrive_(settings.imgread.DefaultImage, true))
		{
			msgboxf("Default image \"%s\" failed to load", MBX_ICONERROR, settings.imgread.DefaultImage);
			return false;
		}
      return true;
	}

	fn[0] = '\0';
   if (settings.System == DC_PLATFORM_DREAMCAST)
      gfrv=GetFile(fn,0,fileflags);
	if (gfrv == 0)
	{
		NullDriveDiscType=Open;
		gd_setdisc();
		return true;
	}
	else if (gfrv == -1)
	{
		return false;
	}

	if (!InitDrive_(fn, true))
	{
		/* msgboxf("Selected image failed to load",MBX_ICONERROR); */
		NullDriveDiscType=Open;
		gd_setdisc();
	}
	return true;
}

void TermDrive(void)
{
	libCore_CDDA_Detach();
	if (disc != NULL)
		disc_free(disc);

	disc = NULL;
	core_archive_release();
}


/* */
/* convert our nice toc struct to dc's native one :) */

static u32 CreateTrackInfo(u32 ctrl,u32 addr,u32 fad)
{
	u8 p[4];
#ifdef MSB_FIRST
	p[1]=(ctrl<<4)|(addr<<0);
	p[0]=fad>>16;
	p[3]=fad>>8;
	p[2]=fad>>0;
#else
	p[0]=(ctrl<<4)|(addr<<0);
	p[1]=fad>>16;
	p[2]=fad>>8;
	p[3]=fad>>0;
#endif

	return *(u32*)p;
}

static u32 CreateTrackInfo_se(u32 ctrl,u32 addr,u32 tracknum)
{
	u8 p[4];
#ifdef MSB_FIRST
	p[1]=(ctrl<<4)|(addr<<0);
	p[0]=tracknum;
	p[3]=0;
	p[2]=0;
#else
	p[0]=(ctrl<<4)|(addr<<0);
	p[1]=tracknum;
	p[2]=0;
	p[3]=0;
#endif
	return *(u32*)p;
}


void GetDriveSector(u8 * buff,u32 StartSector,u32 SectorCount,u32 secsz)
{
   /* printf("GD: read %08X, %d\n",StartSector,SectorCount); */
   if (!disc)
   {
      /* no disc: blank sectors, not what the buffer held before */
      memset(buff, 0, (size_t)SectorCount * secsz);
      return;
   }

   disc_read_sectors(disc,StartSector,SectorCount,buff,secsz);
}
void GetDriveToc(u32* to,DiskArea area)
{
	u32 first_track, last_track, adr, i;

	/* (filled first: with no disc in the drive the table was left as the
	 * caller had it, which is whatever was on its stack, and sent) */
	memset(to, 0xFF, 102 * 4);
	if (!disc)
		return;

	/* can't get toc on the second area on discs that don't have it: there
	 * is none to give. (This was a verify(), which checks nothing in a
	 * build that is not for debugging, and the third track of a disc with
	 * one or two was read all the same.) */
	if (area == DoubleDensity && disc->type != GdRom)
		return;

	/* normal CDs: 1 .. tc */
	/* GDROM: area0 is 1 .. 2, area1 is 3 ... tc */

	first_track=1;
	last_track=disc->track_count;
	/* the table has room for 99 tracks, and a disc has no more */
	if (last_track > 99)
		last_track = 99;
	if (area==DoubleDensity)
		first_track=3;
	else if (disc->type==GdRom)
		last_track=2;

	if (first_track - 1 >= disc->track_count || last_track - 1 >= disc->track_count)
		return;

	/* Generate the TOC info. ADR is 1 in every entry - "what follows is a
	 * position" - whatever the image said or did not say of it: it was 0
	 * for gdi, cue and chd images, and upstream has Eldorado Gate 4 to 7
	 * and V-Rally 2's music down to that. */
	adr = 1;

	/* -1 for 1..99 0 ..98 */
	to[99]  = CreateTrackInfo_se(disc->tracks[first_track-1].CTRL, adr, first_track);
	to[100] = CreateTrackInfo_se(disc->tracks[last_track-1].CTRL, adr, last_track);
	
	if (disc->type==GdRom && area==SingleDensity)
		/* The single-density area's lead-out starts right after its
		 * second track; 13085 when the image does not say where that
		 * track ends. */
		to[101] = CreateTrackInfo(disc->LeadOut.CTRL, adr,
				disc->tracks[1].EndFAD != 0 ? disc->tracks[1].EndFAD + 1 : 13085);
	else
		/* (a GD-ROM's second area had none at all: the entry was left as
		 * the 0xFFFFFFFF the table is filled with) */
		to[101] = CreateTrackInfo(disc->LeadOut.CTRL, adr, disc->LeadOut.StartFAD);

	for (i=first_track-1;i<last_track;i++)
		to[i] = CreateTrackInfo(disc->tracks[i].CTRL, adr, disc->tracks[i].StartFAD);
}

void GetDriveSessionInfo(u8* to,u8 session)
{
	if (!disc)
	{
		/* no disc, no sessions: said, and not left as the caller's buffer was */
		memset(to, 0, 6);
		return;
	}
	to[0]=2;/* status, will get overwritten anyway */
	to[1]=0;/* 0's */
	
	if (session==0)
	{
		to[2]=(u8)disc->session_count;/* count of sessions */
		to[3]=disc->EndFAD>>16;/* fad is sessions end */
		to[4]=disc->EndFAD>>8;
		to[5]=disc->EndFAD>>0;
	}
	else if (session > disc->session_count)
	{
		/* no such session (upstream) */
		to[2] = to[3] = to[4] = to[5] = 0;
	}
	else
	{
		to[2]=disc->sessions[session-1].FirstTrack;/* start track of this session */
		to[3]=disc->sessions[session-1].StartFAD>>16;/* fad is session start */
		to[4]=disc->sessions[session-1].StartFAD>>8;
		to[5]=disc->sessions[session-1].StartFAD>>0;
	}
}

DiscType GuessDiscType(bool m1, bool m2, bool da)
{
	if ((m1==true) && (da==false) && (m2==false))
		return  CdRom;
	else if (m2)
		return  CdRom_XA;
	else if (da && m1) 
		return CdRom_Extra;
   return CdRom;
}
