#ifndef NAOMI_CART_H
#define NAOMI_CART_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The romset is a Sega System SP's (from naomi_cart_GetSystemType()) */
extern bool naomi_cart_systemsp;

/* The game's buttons and axes, as the core's descriptors and the boards read them */
typedef struct ButtonDescriptor ButtonDescriptor;
struct ButtonDescriptor
{
   u32 mask;
   const char *name;
   u32 p2_mask;     /* read as these of the second player's switches */
   /* Read as this other switch of the player's own; or, 0xffffffff, a
    * switch of the cabinet's that no button is for and that is always on
    * (the player's own, or the second player's p2_mask if there is one). */
   u32 target;
};

enum AxisType {
   Full,
   Half
};
typedef enum AxisType AxisType;

typedef struct AxisDescriptor AxisDescriptor;
struct AxisDescriptor
{
   const char *name;
   AxisType type;
   u32 axis;
   bool inverted;
};

typedef struct InputDescriptors InputDescriptors;
struct InputDescriptors
{
   ButtonDescriptor buttons[18];
   AxisDescriptor axes[8];
};

extern InputDescriptors *naomi_game_inputs;
/* The games' own, that the table of special settings gives (naomi_roms_input.h) */
extern InputDescriptors gunsur2_inputs;
extern InputDescriptors ftspeed_inputs;
extern InputDescriptors maxspeed_inputs;
extern InputDescriptors monkeyba_inputs;
extern InputDescriptors cvs2_inputs;
extern InputDescriptors deathcox_inputs;
/* The game's name, as its ROM header has it */
extern char naomi_game_id[];
/* A BIOS file was loaded (a NAOMI runs no game without one) */
extern bool bios_loaded;
/* The System SP's ROM board's part of the G2 area: @sz bytes (systemsp.c) */
u32 systemsp_read_area0(u32 addr, u32 sz);
void systemsp_write_area0(u32 addr, u32 data, u32 sz);
/* The romset of the content, loaded (false: none that runs) */
bool naomi_cart_SelectFile(void);
void naomi_cart_Close(void);
/* Reads the cartridge's ROM board (false: no cartridge, or the read failed) */
bool naomi_cart_loaded(void);
bool naomi_cart_read(u32 offset, u32 size, void *dst);

/* A cartridge: a ROM board and its ROMs, as the NAOMI (or the AtomisWave,
 * or the System SP) sees them. The board's kind is in its ops; what every
 * kind has is here. */
typedef struct cartridge cartridge;
typedef struct cartridge_ops
{
	/* Once its ROMs are in place: false if the board cannot run */
	bool (*init)(cartridge *c);
	/* The machine is reset as when it is switched on: the board's
	 * registers are as they are then. (Its ROMs stay as they are.) */
	void (*reset)(cartridge *c);
	/* The board's registers, at 5f7000-5f70ff */
	u32 (*read_mem)(cartridge *c, u32 address, u32 size);
	void (*write_mem)(cartridge *c, u32 address, u32 data, u32 size);
	/* What the board gives at @offset, or takes there */
	bool (*read)(cartridge *c, u32 offset, u32 size, void *dst);
	bool (*write)(cartridge *c, u32 offset, u32 size, u32 data);
	/* The ROMs at @offset, @*size bytes of them (NULL and 0 past them) */
	void *(*get_ptr)(cartridge *c, u32 offset, u32 *size);
	/* What a transfer reads next, @*size bytes of it at most; and that
	 * @size bytes of it were read */
	void *(*get_dma_ptr)(cartridge *c, u32 *size);
	void (*advance_ptr)(cartridge *c, u32 size);
	/* The game's name, without the spaces after it: CART_GAME_ID_SIZE
	 * bytes at most, with its end */
	void (*game_id)(cartridge *c, char *id);
	void (*serialize)(cartridge *c, void **data, unsigned int *total_size);
	void (*unserialize)(cartridge *c, void **data, unsigned int *total_size);
	/* What a cartridge keeps that the save states before V21 did not hold:
	 * written and read after everything else, by naomi_serialize_v21() and
	 * naomi_unserialize_v21(). */
	void (*serialize_v21)(cartridge *c, void **data, unsigned int *total_size);
	void (*unserialize_v21)(cartridge *c, void **data, unsigned int *total_size);
	/* The set's key, and its key file (malloc()ed: the board's from here) */
	void (*set_key)(cartridge *c, u32 key);
	void (*set_key_data)(cartridge *c, u8 *key_data);
	/* A NAOMI board's transfer, or its PIO, starts at another offset */
	void (*dma_offset_changed)(cartridge *c, u32 dma_offset);
	void (*pio_offset_changed)(cartridge *c, u32 pio_offset);
	/* Done with: what it has is released */
	void (*close)(cartridge *c);
} cartridge_ops;

#define CART_GAME_ID_SIZE 33

struct cartridge
{
	const cartridge_ops *ops;
	u8 *rom_ptr;
	u32 rom_size;
	/* a NAOMI ROM board's registers */
	u32 rom_pio_offset;
	bool rom_pio_autoinc;
	u32 dma_offset;
	u32 dma_count;
	u32 key;
};

extern cartridge *CurrentCartridge;

/* @size bytes of ROM, all ones (none, and a size of 0, when there is no
 * memory for them); and released (cartridge.c) */
void cart_construct(cartridge *c, const cartridge_ops *ops, u32 size);
void cart_destruct(cartridge *c);
/* What every kind of board does, for those that do nothing else */
bool cart_init(cartridge *c);
void cart_nothing(cartridge *c);
bool cart_read(cartridge *c, u32 offset, u32 size, void *dst);
bool cart_write(cartridge *c, u32 offset, u32 size, u32 data);
void *cart_get_ptr(cartridge *c, u32 offset, u32 *size);
void cart_game_id(cartridge *c, char *id);
void cart_no_state(cartridge *c, void **data, unsigned int *total_size);
void cart_set_key(cartridge *c, u32 key);
void cart_set_key_data(cartridge *c, u8 *key_data);
void cart_offset_changed(cartridge *c, u32 offset);
/* And a NAOMI ROM board's */
void naomi_board_reset(cartridge *c);
u32 naomi_board_read_mem(cartridge *c, u32 address, u32 size);
void naomi_board_write_mem(cartridge *c, u32 address, u32 data, u32 size);
void *naomi_board_get_dma_ptr(cartridge *c, u32 *size);
void naomi_board_advance_ptr(cartridge *c, u32 size);
void naomi_board_serialize(cartridge *c, void **data, unsigned int *total_size);
void naomi_board_unserialize(cartridge *c, void **data, unsigned int *total_size);
void naomi_board_set_key(cartridge *c, u32 key);
/* @len bytes at @s, with the spaces after them taken off, into @id: how
 * many are left */
u32 cart_trim_id(char *id, const char *s, u32 len);

struct archive;
/* The boards, one of each at a time (NULL: no memory for its ROMs) */
/* (an M2 board: its ROMs, and a decryption chip) */
cartridge *m2_cart_open(u32 size);
/* (a flat image, mapped by the loader, which is the cartridge) */
cartridge *decrypted_cart_open(u8 *rom_ptr, u32 size);
/* (a flat image that was a member of @archive, and is the archive's
 * memory: the cartridge closes the archive) */
cartridge *archived_cart_open(const u8 *rom_ptr, u32 size, struct archive *archive);

/* The System SP's ROM board (systemsp.c): one for the set @game_name,
 * whose game is on the CompactFlash card @media_name (NULL: in ROM), or
 * its parent's @parent_name; the region the game is started in (known
 * once it is initialized), and its EEPROM, for that region */
cartridge *systemsp_cart_open(u32 size, const char *game_name, const char *media_name, const char *parent_name);
int systemsp_cart_region(void);
void systemsp_cart_load_eeprom(void);
/* Its state, from V24 (nothing when the machine is not one); and the
 * dinokich and loveber3cn IC card reader's, from V25 */
void systemsp_serialize_v24(void **data, unsigned int *total_size);
bool systemsp_unserialize_v24(void **data, unsigned int *total_size);
void systemsp_state_before_v24(void);
void systemsp_serialize_v25(void **data, unsigned int *total_size);
bool systemsp_unserialize_v25(void **data, unsigned int *total_size);
void systemsp_state_before_v25(void);
/* The game's header, @size bytes of it (naomi_cart.c): false if what is
 * there is not this game's */
bool naomi_cart_GetBootHeader(u8 *header, u32 size);

/* The platform of the romset at @file, or -1 for an archive that is not
 * a known Naomi or AtomisWave romset. */
int naomi_cart_GetSystemType(const char *file);
/* A merged romset's games, for the core option that chooses among them,
 * and the choice: naomi_cart.cpp has the particulars. */
typedef struct NaomiSet NaomiSet;
struct NaomiSet
{
   const char *name;          /* the set's, which is the option's value */
   const char *description;   /* the game's, which is what the option shows */
};
int naomi_cart_MergedSets(const char *path, NaomiSet *sets, int max);
void naomi_cart_WantSet(const char *name);
int naomi_cart_GetRotation(void);

extern u8 *naomi_default_eeprom;
/* The content's file name, the folder it is in, and the set's parent's
 * name: where a set's other files are looked for (libretro.cpp,
 * naomi_cart.cpp) */
extern char g_base_name[128];
extern char g_roms_dir[];
extern char g_parent_name[128];

#ifdef __cplusplus
}
#endif

#endif /* NAOMI_CART_H */
