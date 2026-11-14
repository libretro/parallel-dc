/*
	bios & nvmem related code: the BIOS ROM, the SRAM and the flash memory
	(flashrom.c)
*/

#pragma once
#include <retro_miscellaneous.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	MEMCHIP_ROM,
	MEMCHIP_SRAM,
	/* Macronix 29LV160TMC; AtomisWave uses a custom 29L001mc model */
	MEMCHIP_FLASH
};

/* The flash's command state (MemChip.state) */
enum
{
	FS_Normal,
	FS_ReadAMDID1,
	FS_ReadAMDID2,
	FS_ByteProgram,
	FS_EraseAMD1,
	FS_EraseAMD2,
	FS_EraseAMD3,
	FS_SelectMode
};

typedef struct MemChip
{
	u8 *data;
	u32 size;
	u32 mask;
	/* the bytes at the start that are neither written nor loaded */
	u32 write_protect_size;
	u32 kind;
	/* the flash's command state, in the save state as it is (4 bytes) */
	u32 state;
	/* the file loaded last, for memchip_reload() */
	char load_filename[PATH_MAX_LENGTH];
} MemChip;

/* The BIOS, a NAOMI's SRAM and the Dreamcast's flash (sb_mem.cpp) */
extern MemChip sys_rom;
extern MemChip sys_nvmem_sram;
extern MemChip sys_nvmem_flash;

/* @size bytes (a power of 2), zeroed, in place of what the chip had */
void memchip_allocate(MemChip *chip, u32 size);
void memchip_free(MemChip *chip);
u8 memchip_read8(MemChip *chip, u32 addr);
/* @sz bytes (1, 2 or 4), little-endian */
u32 memchip_read(MemChip *chip, u32 addr, u32 sz);
void memchip_write(MemChip *chip, u32 addr, u32 val, u32 sz);
void memchip_reset(MemChip *chip);
/* The chip's bytes past write_protect_size from @file, all of them or false */
bool memchip_load(MemChip *chip, const char *file);
bool memchip_reload(MemChip *chip);
void memchip_save(MemChip *chip, const char *file);
/* From the first of @names (';' between them, a '%' first for @prefix
 * before it) found in @root */
bool memchip_load_named(MemChip *chip, const char *root, const char *prefix,
		const char *names, const char *title);
void memchip_save_named(MemChip *chip, const char *root, const char *prefix,
		const char *name, const char *title);


/* Flash block handling code borrowed from redream (https://github.com/inolen/redream) */


/* magic cookie every block-allocated partition begins with */
#define FLASH_MAGIC_COOKIE "KATANA_FLASH____"

#define FLASH_BLOCK_SIZE 0x40
/* each bitmap is 64 bytes in length, and each byte can record the state of 8 */
/* physical blocks (one per bit), therefore, each bitmap can represent up to */
/* 512 physical blocks. these 512 blocks are each 64-bytes in length, meaning */
/* each partition would need partition_size / 32768 bitmap blocks to represent */
/* all of its physical blocks */
#define FLASH_BITMAP_BLOCKS (FLASH_BLOCK_SIZE * 8)
#define FLASH_BITMAP_BYTES (FLASH_BITMAP_BLOCKS * 64)

/* flash partitions */
#define FLASH_PT_FACTORY 0
#define FLASH_PT_RESERVED 1
#define FLASH_PT_USER 2
#define FLASH_PT_GAME 3
#define FLASH_PT_UNKNOWN 4
#define FLASH_PT_NUM 5

/* flash logical blocks */
#define FLASH_USER_SYSCFG 0x05
#define FLASH_USER_INET 0x80
#define FLASH_USER_ISP1 0xC0
#define FLASH_USER_ISP2 0xC6

typedef struct flash_syscfg_block {
  u16 block_id;
  /* last set time (seconds since 1/1/1950 00:00) */
  u16 time_lo;
  u16 time_hi;
  u8 unknown1;
  u8 lang;
  u8 mono;
  u8 autostart;
  u8 unknown2[4];
  u8 reserved[50];
} flash_syscfg_block;

typedef struct flash_isp1_block {
	u16 block_id;
	u8 _unknown[4];
	char sega[4];
	char username[28];
	char password[16];
	char phone[8];
	u16 crc;
} flash_isp1_block;

typedef struct flash_isp2_block {
	u16 block_id;
	char sega[4];
	char username[28];
	char password[16];
	char phone[12];
	u16 crc;
} flash_isp2_block;

/* header block in block-allocated partition */
typedef struct flash_header_block {
  char magic[16];
  u8 part_id;
  u8 version;
  u8 reserved[46];
} flash_header_block;

/* user block in block-allocated partition */
typedef struct flash_user_block {
  u16 block_id;
  u8 data[60];
  u16 crc;
} flash_user_block;


/* The flash's partitions and the blocks in them */
void flash_partition_info(int part_id, int *offset, int *size);
int flash_write_block(MemChip *flash, u32 part_id, u32 block_id, const void *data);
int flash_read_block(MemChip *flash, u32 part_id, u32 block_id, void *data);
/* The partitions made what the system expects where they are not */
void flash_validate(MemChip *flash);

#ifdef __cplusplus
}
#endif
