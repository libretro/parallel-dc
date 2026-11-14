/*
    Created on: Apr 20, 2020

    Copyright 2020 flyinghead

    This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"
#include <retro_atomic.h>

#ifdef __cplusplus
extern "C" {
#endif

int dc_init(void);
void dc_reset(bool hard);
void dc_run(void);
void dc_term(void);
void dc_stop(void);
void dc_request_reset(void);
bool dc_is_running(void);
/* The memory sizes of the platform in settings.System */
void dc_prepare_system(void);
/* The game's own settings put back over the options' (after these are read) */
void dc_apply_game_settings(void);

/* The frontend's side (libretro.cpp): the BIOS is started with no content */
extern bool boot_to_bios;
/* A reset asked for, done on the emulator's thread */
extern retro_atomic_int_t reset_requested;
/* The exception handlers, and the code cache's unwind table (common.cpp) */
void common_libretro_setup(void);
void common_libretro_cleanup(void);
#ifdef _WIN64
void setup_seh(void);
#endif

#ifdef __cplusplus
}
#endif
