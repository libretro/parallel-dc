#ifndef REIOS_H
#define REIOS_H

#include <string.h>
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
} ip_meta_t;
extern ip_meta_t ip_meta;

/* The disc runs Windows CE */
static TYPES_INLINE bool ip_meta_is_wince(const ip_meta_t *m)
{
	return m->wince == '1' || memcmp("0WINCEOS.BIN", m->boot_filename, 12) == 0;
}
/* The disc says it can be shown through the VGA box */
static TYPES_INLINE bool ip_meta_supports_vga(const ip_meta_t *m)
{
	return m->vga == '1';
}
/* The disc's product number: in @trimmed without the spaces it is padded
 * with, in @raw (when not NULL) as it is; both of
 * sizeof(m->product_number) + 1 bytes, ended with a NUL */
static TYPES_INLINE void ip_meta_product_id(const ip_meta_t *m,
		char *trimmed, char *raw)
{
	size_t len = sizeof(m->product_number);
	memcpy(trimmed, m->product_number, len);
	trimmed[len] = '\0';
	len = strlen(trimmed);
	if (raw)
		memcpy(raw, trimmed, len + 1);
	while (len > 0 && trimmed[len - 1] == ' ')
		trimmed[--len] = '\0';
}
/* The disc is for the area @area ('J', 'U', 'E') */
static TYPES_INLINE bool ip_meta_has_area(const ip_meta_t *m, char area)
{
	return memchr(m->area_symbols, area, sizeof(m->area_symbols)) != NULL;
}
/* The disc is a MIL-CD */
static TYPES_INLINE bool ip_meta_is_milcd(const ip_meta_t *m)
{
	return memcmp("MIL CD", m->disk_type, 6) == 0;
}
/* The HLE BIOS's tables, made once */
bool reios_init(void);
/* The disc's IP.BIN read into ip_meta */
void reios_disk_id(void);

#ifdef __cplusplus
}
#endif

#endif /* REIOS_H */
