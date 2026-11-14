/*
	Copyright 2025 flyinghead

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

/* V.42 bis data compression. The dictionary is a tree kept in arrays,
 * so that it has a fixed size and can be in a save state. */

/* N3 Character size (bits) */
#define V42B_CHAR_SIZE 8
/* N4 Number of characters in the alphabet */
#define V42B_ALPHABET_SIZE (1 << V42B_CHAR_SIZE)
/* N6 Number of control codewords */
#define V42B_CW_RESERVED 3
/* N5 Index number of first dictionary entry used to store a string */
#define V42B_CW_FIRST (V42B_ALPHABET_SIZE + V42B_CW_RESERVED)
/* 5.1 Minimum and default value for P1 (Total number of codewords) */
#define V42B_MIN_TOTAL_CODEWORDS 512
/* The most this modem accepts for P1. The other end may ask for more (the
 * standard allows 65535); it is then answered with this, which the
 * negotiation lets it do. */
#define V42B_MAX_TOTAL_CODEWORDS 2048
/* 5.1 Valid range for P2 (Maximum string length) */
#define V42B_DEF_STRING_SIZE 6
#define V42B_MAX_STRING_SIZE 250

/* The strings: each a node, its parent the string one character shorter */
typedef struct v42b_dict
{
	u16 parent[V42B_MAX_TOTAL_CODEWORDS];
	u16 first_child[V42B_MAX_TOTAL_CODEWORDS];
	u16 next_sibling[V42B_MAX_TOTAL_CODEWORDS];
	u8 character[V42B_MAX_TOTAL_CODEWORDS];
} v42b_dict;

/* What both directions keep */
typedef struct v42b_base
{
	u32 max_code_words;			/* V.42 bis param N2 */
	int max_string_length;		/* V.42 bis param N7 */
	u32 next_code_word;			/* V.42 bis param C1 */
	u32 code_word_size;			/* V.42 bis param C2 */
	u32 code_word_threshold;	/* V.42 bis param C3 */
	v42b_dict dict;
	int last_added_code;
	int match_len;
	int cur_code;
	bool transparent;
	/* The current codeword has already been transferred and string matching
	 * should restart (but the dictionary is updated as normal): for mode
	 * transitions, and a flush in compressed mode */
	bool mode_transition;
	u8 escape_code;
} v42b_base;

#define V42B_OUT_SIZE 512

typedef struct v42b_compressor
{
	v42b_base base;
	int compress_test;
	int compress_test_count;
	u32 out_bit_buf;
	int out_bit_pos;
	/* what it made, to be read: a queue */
	u8 out[V42B_OUT_SIZE];
	unsigned out_head;
	unsigned out_count;
} v42b_compressor;

typedef struct v42b_decompressor
{
	v42b_base base;
	bool escaped;
	u32 input_bit_buffer;
	int input_bit_count;
} v42b_decompressor;

/* Where the decompressor puts what it decodes, as it comes */
typedef void (*v42b_out_fn)(void *ctx, u8 v);

/* As they are made: the default parameters, nothing in them */
void v42b_compressor_init(v42b_compressor *c);
/* @max_code_words and @max_string_length are the negotiated P1 and P2 */
void v42b_compressor_set_params(v42b_compressor *c, u32 max_code_words, int max_string_length);
void v42b_compressor_reset(v42b_compressor *c);
void v42b_compressor_write(v42b_compressor *c, u8 b);
void v42b_compressor_change_mode(v42b_compressor *c);
/* What is waiting sent out, to an octet */
void v42b_compressor_flush(v42b_compressor *c);
/* A byte it made, -1 if none is waiting */
int v42b_compressor_read(v42b_compressor *c);
int v42b_compressor_available(const v42b_compressor *c);
void v42b_compressor_serialize(const v42b_compressor *c, void **data, unsigned int *total_size);
void v42b_compressor_unserialize(v42b_compressor *c, void **data, unsigned int *total_size);

void v42b_decompressor_init(v42b_decompressor *d);
void v42b_decompressor_set_params(v42b_decompressor *d, u32 max_code_words, int max_string_length);
void v42b_decompressor_reset(v42b_decompressor *d);
/* What is decoded goes to @out as it comes. False if the data is in
 * error (V.42 bis 5.8): the connection is to be released. */
bool v42b_decompressor_write(v42b_decompressor *d, u8 b, v42b_out_fn out, void *ctx);
void v42b_decompressor_serialize(const v42b_decompressor *d, void **data, unsigned int *total_size);
void v42b_decompressor_unserialize(v42b_decompressor *d, void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif
