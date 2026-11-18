/* The content's discs (lr_disk.c): the list a playlist (.m3u) or the
 * content itself gives, which of them is in the drive and whether its lid
 * is open, the frontend's disc control, and the disc's words in a state.
 * C89, for C and C++ alike. */
#ifndef LR_DISK_H
#define LR_DISK_H
#include <boolean.h>
#include <stddef.h>
#include <libretro.h>
#include <retro_common_api.h>
#include "types.h"

RETRO_BEGIN_DECLS

/* retro_load_game() went through, and retro_unload_game() has a machine to
 * stop (libretro.cpp) */
extern bool game_loaded;
/* The disc in the drive, an index in the list, and the drive's lid open */
extern unsigned disk_index;
extern bool disc_tray_open;

/* The list emptied, the first disc in the drive */
void lr_disk_clear(void);
/* How many discs there are */
unsigned lr_disk_count(void);
/* The frontend's disc control given the list (retro_set_environment()) */
void lr_disk_init_interface(retro_environment_t env);
/* The discs of a playlist read into the list, those it names relative to
 * itself in @dir; false for none */
bool lr_disk_read_m3u(const char *file, const char *dir);
/* The disc a game starts with: of the list read from a playlist, the first
 * or the one the frontend last had in (strdup()'d); otherwise @content, as
 * the list's only disc. NULL for none (no memory) */
char *lr_disk_start(const char *content);
/* The words of a state that say which disc was in and the lid open
 * (LR_DISK_TRAILER_SIZE bytes) */
#define LR_DISK_TRAILER_SIZE 16
void lr_disk_trailer_write(u8 *at);
void lr_disk_trailer_read(const u8 *at);
/* A file's name without its folder or extension (a disc's label) */
void short_pathname_representation(char *out, const char *path, size_t size);

RETRO_END_DECLS

#endif
