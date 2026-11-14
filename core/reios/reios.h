#ifndef REIOS_H
#define REIOS_H

#include "types.h"
#include "hw/flashrom/flashrom.h"

#ifdef __cplusplus
extern "C" {
#endif
/* The HLE BIOS's opcode: what it does, at its address (sh4_opcode_list.c) */
void DYNACALL reios_trap(u32 op);
#define REIOS_OPCODE 0x085B
/* The HLE BIOS set up in @rom, with @flash the console's flash memory */
void reios_reset(u8* rom, MemChip *flash);
/* The disc's IP.BIN says it is a GD-ROM */
bool reios_disc_is_gdrom(void);

/* The disc's IP.BIN */
typedef struct ip_meta_t
{
	char hardware_id[16];
	char maker_id[16];
	char ks[5];
	char disk_type[6];
	char disk_num[5];
	char area_symbols[8];
	char ctrl[4];
	char dev;
	char vga;
	char wince;
	char _unk1;
	char product_number[10];
	char product_version[6];
	char release_date[8];
	char _unk2[8];
	char boot_filename[16];
	char software_company[16];
	char software_name[128];

#ifdef __cplusplus
	bool isWindowsCE() { return wince == '1' || memcmp("0WINCEOS.BIN", boot_filename, 12) == 0; }
	bool supportsVGA() { return vga == '1'; }
	bool isMILCD() const { return memcmp("MIL CD", disk_type, 6) == 0; }
#endif
} ip_meta_t;
extern ip_meta_t ip_meta;
#ifdef __cplusplus
}

bool reios_init();

void reios_disk_id();

#endif

#endif /* REIOS_H */
