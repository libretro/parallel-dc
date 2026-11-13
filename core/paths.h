#pragma once
/* Where the core keeps what it writes: its directory in the frontend's
 * system directory, and the memory cards' (libretro.cpp sets them up). */
#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* With no slash at the end */
extern char game_dir_no_slash[1024];
extern char vmu_dir_no_slash[];
/* The content's name, for memory cards of its own */
extern char content_name[];
/* 1: port A1's memory card is the content's own; 2: all of them are */
extern unsigned per_content_vmus;

/* The path of @filename (as it is appended: with its own slashes, if it has
 * any) in the core's directory, into @out of @size bytes, cut short there
 * (@out may be NULL with @size 0). Its whole length, as snprintf() has it. */
size_t writable_data_path(char *out, size_t size, const char *filename);
/* The file of the memory card in @logical_port ("A1"...) */
size_t writable_vmu_path(char *out, size_t size, const char *logical_port);

#ifdef __cplusplus
}
#endif
