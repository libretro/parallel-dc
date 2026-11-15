/*
	Copyright 2022 flyinghead

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

/* The card reader of the game that is loaded, made on a hard reset:
 * Initial D's, Derby Owners Club's, Club Kart's; and gone */
void card_reader_initd_init(void);
void card_reader_derby_init(void);
void card_reader_clubk_init(void);
void card_reader_term(void);

/* A barcode reader on the SCIF instead */
void card_reader_barcode_init(void);
void card_reader_barcode_term(void);
bool card_reader_barcode_available(void);

/* The "Insert Card" button: the reader's card, the barcode, or player
 * @playerNum's IC card */
void card_reader_insert_card(int playerNum);
/* the game has something to put a card in */
bool card_reader_available(void);

/* What the readers have in a save state since V24 (card_reader.c) */
void card_reader_serialize_v24(void **data, unsigned int *total_size);
bool card_reader_unserialize_v24(void **data, unsigned int *total_size);
void card_reader_state_before_v24(void);

/* The cards out of the readers in a save state from V30 on (card_reader.c) */
void naomi_cards_serialize_v30(void **data, unsigned int *total_size);
bool naomi_cards_unserialize_v30(void **data, unsigned int *total_size);
void naomi_cards_state_before_v30(void);

#ifdef __cplusplus
}
#endif
