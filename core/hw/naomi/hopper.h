/*
	Copyright 2023 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The hopper board on the SCIF, for the game that is loaded (a medal or
 * prize game), and gone */
void hopper_init(void);
void hopper_term(void);
bool hopper_active(void);
/* The coin switch, as the I/O board bridge reads it (naomi_periph_poll()) */
void hopper_poll(void);

/* What the hopper board has in a save state since V24 (hopper.c) */
void hopper_serialize_v24(void **data, unsigned int *total_size);
bool hopper_unserialize_v24(void **data, unsigned int *total_size);
void hopper_state_before_v24(void);

#ifdef __cplusplus
}
#endif
