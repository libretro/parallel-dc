#pragma once
#include "ImgReader.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void DriveRead(u8 * buff,u32 StartSector,u32 SectorCount,u32 secsz);
typedef void DriveGetToc(u32* toc,DiskArea area);

typedef DiscType DriveGetType(void);
typedef void DriveInit(void);
typedef void DriveTerm(void);

#ifdef __cplusplus
}
#endif
