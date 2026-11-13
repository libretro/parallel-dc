/*
    Created on: Sep 23, 2019

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
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Cheat
{
	const char *game_id;
	const char *area_or_version;
	u32 addresses[16];
	u32 values[16];
	/* Where not 0: the value is only written over this one. (The game puts
	 * the same word there for other uses, which must not be changed.) */
	u32 orig_values[16];
} Cheat;

/* A change a game needs to work, made whatever the options say: @value is
 * written at @address in main memory whenever something else is there -
 * or, with @only_if set, only when @if_value is there (code that is
 * patched once it is loaded, and left alone if it is not the code). */
typedef struct GameFix
{
	const char *game_id;
	u32 bits;		/* 8, 16 or 32 */
	u32 address;
	u32 value;
	bool only_if;
	u32 if_value;
} GameFix;

/* The game's fixes and widescreen cheat found (cheats.c): true if it has
 * one, the picture then 16:9 anamorphic */
bool cheats_reset(void);
/* What they write, written (each frame) */
void cheats_apply(void);
/* The option changed while the game runs (the machine standing still).
 * Returns what cheats_reset() does. */
bool cheats_change(void);

#ifdef __cplusplus
}
#endif
