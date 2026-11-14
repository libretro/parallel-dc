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
#include <string.h>
#include "v42bis.h"

/* A node: none; a parent: the node is free */
#define NODE_NONE 0xffff
/* A parent: the node is a single character */
#define NODE_ROOT 0xfffe

enum
{
	/* Control code words (compressed mode) */
	ETM = 0,         /* Enter transparent mode */
	FLUSH = 1,       /* Flush data */
	STEPUP = 2,      /* Step up codeword size */

	/* Command codes (transparent mode) */
	ECM = 0,         /* Enter compression mode */
	EID = 1,         /* Escape character in data */
	RESET = 2        /* Force reinitialization */
};

/* The dictionary */

static void dict_reset(v42b_dict *d)
{
	u32 i;
	for (i = 0; i < V42B_MAX_TOTAL_CODEWORDS; i++)
	{
		if (i >= V42B_CW_RESERVED && i < V42B_CW_FIRST)
		{
			d->parent[i] = NODE_ROOT;
			d->character[i] = (u8)(i - V42B_CW_RESERVED);
		}
		else
		{
			d->parent[i] = NODE_NONE;
			d->character[i] = 0;
		}
		d->first_child[i] = NODE_NONE;
		d->next_sibling[i] = NODE_NONE;
	}
}

static bool dict_is_free(const v42b_dict *d, u32 code)
{
	return d->parent[code] == NODE_NONE;
}

/* The child of @code for @c, NODE_NONE if it has none */
static u16 dict_get_child(const v42b_dict *d, u16 code, u8 c)
{
	/* A node has a child for each character at most */
	int left = V42B_ALPHABET_SIZE;
	u16 n;
	for (n = d->first_child[code]; n != NODE_NONE && left > 0; n = d->next_sibling[n], left--)
		if (d->character[n] == c)
			return n;
	return NODE_NONE;
}

static void dict_add_child(v42b_dict *d, u16 child, u16 parent, u8 c)
{
	d->parent[child] = parent;
	d->character[child] = c;
	d->first_child[child] = NODE_NONE;
	d->next_sibling[child] = d->first_child[parent];
	d->first_child[parent] = child;
}

/* Takes a leaf off its parent: the node is free again */
static void dict_detach(v42b_dict *d, u16 code)
{
	const u16 p = d->parent[code];
	if (p != NODE_NONE && p != NODE_ROOT)
	{
		u16 *link = &d->first_child[p];
		int left = V42B_ALPHABET_SIZE;
		while (*link != NODE_NONE && *link != code && left-- > 0)
			link = &d->next_sibling[*link];
		if (*link == code)
			*link = d->next_sibling[code];
	}
	d->parent[code] = NODE_NONE;
	d->next_sibling[code] = NODE_NONE;
}

/* The strings, by their parent and last character. Which children a node
 * has follows from them, and is worked out again on a load. */
static void dict_serialize(const v42b_dict *d, void **data, unsigned int *total_size)
{
	/* (what a free node had before is not kept) */
	u8 chars[V42B_MAX_TOTAL_CODEWORDS - V42B_CW_FIRST];
	u32 i;
	for (i = V42B_CW_FIRST; i < V42B_MAX_TOTAL_CODEWORDS; i++)
		chars[i - V42B_CW_FIRST] = d->parent[i] == NODE_NONE ? 0 : d->character[i];
	LIBRETRO_SA(&d->parent[V42B_CW_FIRST], V42B_MAX_TOTAL_CODEWORDS - V42B_CW_FIRST);
	LIBRETRO_SA(chars, V42B_MAX_TOTAL_CODEWORDS - V42B_CW_FIRST);
}

/* @limit: P1, the number of codewords in use */
static void dict_unserialize(v42b_dict *d, void **data, unsigned int *total_size, u32 limit)
{
	static u8 keep[V42B_MAX_TOTAL_CODEWORDS];
	u32 i;

	dict_reset(d);
	LIBRETRO_USA(&d->parent[V42B_CW_FIRST], V42B_MAX_TOTAL_CODEWORDS - V42B_CW_FIRST);
	LIBRETRO_USA(&d->character[V42B_CW_FIRST], V42B_MAX_TOTAL_CODEWORDS - V42B_CW_FIRST);

	/* A string is kept if it leads to a single character in no more steps
	 * than the longest string has characters, through strings of the
	 * dictionary: then nothing that walks the tree goes outside it or round
	 * in circles. A string that does not is dropped, and every longer string
	 * made from it is dropped with it, since its walk goes the same way. */
	for (i = V42B_CW_FIRST; i < V42B_MAX_TOTAL_CODEWORDS; i++)
	{
		u32 n = i;
		int steps;
		keep[i] = 0;
		if (i >= limit || d->parent[i] == NODE_NONE)
			continue;
		for (steps = 0; steps <= V42B_MAX_STRING_SIZE; steps++)
		{
			const u16 p = d->parent[n];
			if (p < V42B_CW_RESERVED || (p >= limit && p != NODE_ROOT))
				break;
			if (p == NODE_ROOT)
			{
				keep[i] = n < V42B_CW_FIRST;
				break;
			}
			n = p;
		}
	}
	for (i = V42B_CW_FIRST; i < V42B_MAX_TOTAL_CODEWORDS; i++)
	{
		if (!keep[i])
		{
			d->parent[i] = NODE_NONE;
			d->character[i] = 0;
		}
	}
	for (i = V42B_CW_FIRST; i < V42B_MAX_TOTAL_CODEWORDS; i++)
		if (d->parent[i] != NODE_NONE)
		{
			d->next_sibling[i] = d->first_child[d->parent[i]];
			d->first_child[d->parent[i]] = (u16)i;
		}
}

/* What both directions have */

static void base_reset(v42b_base *b)
{
	dict_reset(&b->dict);
	b->next_code_word = V42B_CW_FIRST;
	b->code_word_size = V42B_CHAR_SIZE + 1;
	b->code_word_threshold = 2 * V42B_ALPHABET_SIZE;
	b->last_added_code = -1;
	b->match_len = 0;
	b->cur_code = -1;
	b->transparent = true;
	b->mode_transition = false;
	b->escape_code = 0;
}

static void base_set_params(v42b_base *b, u32 max_code_words, int max_string_length)
{
	if (max_code_words < V42B_MIN_TOTAL_CODEWORDS)
		max_code_words = V42B_MIN_TOTAL_CODEWORDS;
	else if (max_code_words > V42B_MAX_TOTAL_CODEWORDS)
		max_code_words = V42B_MAX_TOTAL_CODEWORDS;
	if (max_string_length < V42B_DEF_STRING_SIZE)
		max_string_length = V42B_DEF_STRING_SIZE;
	else if (max_string_length > V42B_MAX_STRING_SIZE)
		max_string_length = V42B_MAX_STRING_SIZE;
	b->max_code_words = max_code_words;
	b->max_string_length = max_string_length;
	base_reset(b);
}

static void base_recover_node(v42b_base *b)
{
	/* 7.7 Node recovery
	 * 6.5 Recovering a dictionary entry to use next
	 * (a tree has a leaf among the nodes that are not single characters: one
	 * round is enough) */
	u32 i;
	for (i = V42B_CW_FIRST; i <= b->max_code_words; i++)
	{
		/* 6.5(a) and (b) counter C1 shall be incremented; if the value of C1 exceeds N2-1 then C1 shall be set to N5 */
		if (++b->next_code_word >= b->max_code_words)
			b->next_code_word = V42B_CW_FIRST;
		/* 6.5(c) We need to reuse a leaf or find a free node */
		if (b->dict.first_child[b->next_code_word] != NODE_NONE)
			continue;
		/* 6.5(d) if the node is a leaf node, then it shall be detached from its parent */
		if (!dict_is_free(&b->dict, b->next_code_word))
			dict_detach(&b->dict, (u16)b->next_code_word);
		break;
	}
}

static void compressor_check_dict_size(v42b_compressor *c);

/* The string matching: the code to send, -1 if none; @c the compressor,
 * NULL for the decompressor */
static int base_accept_char(v42b_base *b, u8 octet, v42b_compressor *c)
{
	u16 node;
	int emit_code;

	if (b->cur_code == -1)
	{
		b->cur_code = octet + V42B_CW_RESERVED;
		b->match_len = 1;
		b->mode_transition = false;
		return -1;
	}
	b->match_len++;
	node = dict_get_child(&b->dict, (u16)b->cur_code, octet);
	if (node != NODE_NONE && !b->mode_transition)
	{
		/* 6.3(b) If the string matches a dictionary entry, and the entry is not that entry
		 * created by the last invocation of the string matching procedure, then the
		 * next character shall be read and appended to the string and this step
		 * repeated. */
		if (node != b->last_added_code)
		{
			b->cur_code = node;
			return -1;
		}
	}
	/* 6.4 Add the string to the dictionary */
	if (node == NODE_NONE && b->match_len <= b->max_string_length)
	{
		if (c)
			compressor_check_dict_size(c);
		dict_add_child(&b->dict, (u16)b->next_code_word, (u16)b->cur_code, octet);
		b->last_added_code = (int)b->next_code_word;
		base_recover_node(b);
	}
	else
		b->last_added_code = -1;
	if (b->mode_transition)
		/* this code's string has already been pushed in transparent mode */
		emit_code = -1;
	else
		emit_code = b->cur_code;
	b->mode_transition = false;
	b->cur_code = octet + V42B_CW_RESERVED;
	b->match_len = 1;
	return emit_code;
}

static void base_serialize(const v42b_base *b, void **data, unsigned int *total_size)
{
	u16 n2 = (u16)b->max_code_words;
	u8 n7 = (u8)b->max_string_length;
	u16 c1 = (u16)b->next_code_word;
	u8 c2 = (u8)b->code_word_size;
	s32 last_added = b->last_added_code;
	s32 len = b->match_len;
	s32 cur = b->cur_code;
	u8 flags = (b->transparent ? 1 : 0) | (b->mode_transition ? 2 : 0);
	u8 escape = b->escape_code;

	LIBRETRO_S(n2);
	LIBRETRO_S(n7);
	LIBRETRO_S(c1);
	LIBRETRO_S(c2);
	LIBRETRO_S(last_added);
	LIBRETRO_S(len);
	LIBRETRO_S(cur);
	LIBRETRO_S(flags);
	LIBRETRO_S(escape);
	dict_serialize(&b->dict, data, total_size);
}

static void base_unserialize(v42b_base *b, void **data, unsigned int *total_size)
{
	u16 n2 = 0;
	u8 n7 = 0;
	u16 c1 = 0;
	u8 c2 = 0;
	s32 last_added = -1;
	s32 len = 0;
	s32 cur = -1;
	u8 flags = 1;
	u8 escape = 0;

	LIBRETRO_US(n2);
	LIBRETRO_US(n7);
	LIBRETRO_US(c1);
	LIBRETRO_US(c2);
	LIBRETRO_US(last_added);
	LIBRETRO_US(len);
	LIBRETRO_US(cur);
	LIBRETRO_US(flags);
	LIBRETRO_US(escape);
	base_set_params(b, n2, n7);
	dict_unserialize(&b->dict, data, total_size, b->max_code_words);

	b->next_code_word = c1 >= V42B_CW_FIRST && c1 < b->max_code_words ? c1 : V42B_CW_FIRST;
	/* C2 goes from 9 bits to as many as the largest codeword has */
	b->code_word_size = V42B_CHAR_SIZE + 1;
	while (b->code_word_size < c2 && (1u << b->code_word_size) < b->max_code_words)
		b->code_word_size++;
	b->code_word_threshold = 1u << b->code_word_size;
	b->last_added_code = last_added >= (s32)V42B_CW_FIRST && last_added < (s32)b->max_code_words ? last_added : -1;
	b->match_len = len >= 0 && len <= V42B_MAX_STRING_SIZE ? len : 0;
	b->cur_code = cur >= (s32)V42B_CW_RESERVED && cur < (s32)b->max_code_words
			&& !dict_is_free(&b->dict, (u32)cur) ? cur : -1;
	b->transparent = (flags & 1) != 0;
	b->mode_transition = (flags & 2) != 0;
	b->escape_code = escape;
}

/* The compressor */

/* (a byte that does not fit is dropped: the queue is sized so that it does
 * not happen) */
static void compressor_push(v42b_compressor *c, u8 b)
{
	if (c->out_count == V42B_OUT_SIZE)
		return;
	c->out[(c->out_head + c->out_count) & (V42B_OUT_SIZE - 1)] = b;
	c->out_count++;
}

static void compressor_push_code(v42b_compressor *c, u16 code)
{
	c->out_bit_buf |= (u32)code << c->out_bit_pos;
	c->out_bit_pos += c->base.code_word_size;
	while (c->out_bit_pos >= 8)
	{
		compressor_push(c, (u8)(c->out_bit_buf & 0xff));
		c->out_bit_buf >>= 8;
		c->out_bit_pos -= 8;
	}
}

static void compressor_check_dict_size(v42b_compressor *c)
{
	v42b_base *b = &c->base;
	if (b->transparent)
		return;
	while (b->next_code_word >= b->code_word_threshold && b->code_word_threshold <= b->max_code_words)
	{
		DEBUG_LOG(MODEM, "Compressor: STEPUP\n");
		compressor_push_code(c, STEPUP);
		b->code_word_size++;
		b->code_word_threshold *= 2;
	}
}

static void compressor_flush(v42b_compressor *c, bool enter_transparent)
{
	v42b_base *b = &c->base;
	if (b->transparent)
		return;
	/* Output the last state of the string */
	if (b->cur_code != -1 && !b->mode_transition)
	{
		/* 7.8.2(a) ensure that the codeword representing any partially encoded data has been transferred */
		compressor_push_code(c, (u16)b->cur_code);
		b->mode_transition = true;
	}
	if (enter_transparent)
		/* 7.8.2(c) indicate to the peer data compression function by transferring the ETM control codeword a
		 * transition to transparent mode */
		compressor_push_code(c, ETM);
	/* 7.9(c) if the previous step leaves extra bits pending for transmission (octet alignment not yet achieved), then
	 * transfer the FLUSH codeword */
	else if ((c->out_bit_pos % 8) != 0)
		compressor_push_code(c, FLUSH);
	/* 7.8.2(d), 7.9(c) transmit sufficient 0 bits to recover octet alignment */
	while (c->out_bit_pos > 0)
	{
		compressor_push(c, (u8)(c->out_bit_buf & 0xff));
		c->out_bit_buf >>= 8;
		c->out_bit_pos -= 8;
	}
	c->out_bit_pos = 0;
	c->out_bit_buf = 0;
}

void v42b_compressor_init(v42b_compressor *c)
{
	memset(c, 0, sizeof(*c));
	v42b_compressor_set_params(c, V42B_MIN_TOTAL_CODEWORDS, V42B_DEF_STRING_SIZE);
	v42b_compressor_reset(c);
}

void v42b_compressor_set_params(v42b_compressor *c, u32 max_code_words, int max_string_length)
{
	base_set_params(&c->base, max_code_words, max_string_length);
}

void v42b_compressor_reset(v42b_compressor *c)
{
	base_reset(&c->base);
	c->out_bit_buf = 0;
	c->out_bit_pos = 0;
	c->compress_test = 0;
	c->compress_test_count = 64;
	c->out_head = 0;
	c->out_count = 0;
}

void v42b_compressor_change_mode(v42b_compressor *c)
{
	v42b_base *b = &c->base;
	if (b->transparent)
	{
		DEBUG_LOG(MODEM, "Compressor: switching to compressed mode\n");
		/* 7.8.1 Transition to compressed mode */
		compressor_push(c, b->escape_code);
		compressor_push(c, ECM);
		b->transparent = false;
		c->compress_test_count = 256;
	}
	else
	{
		DEBUG_LOG(MODEM, "Compressor: switching to transparent mode\n");
		/* 7.8.2 Transition to transparent mode */
		compressor_flush(c, true);
		b->transparent = true;
		c->compress_test_count = 64;
	}
	b->mode_transition = true;
	c->compress_test = 0;
}

void v42b_compressor_write(v42b_compressor *c, u8 octet)
{
	v42b_base *b = &c->base;
	int matched_len, code;

	if (b->transparent)
		compressor_push(c, octet);
	if (octet == b->escape_code)
	{
		if (b->transparent)
			compressor_push(c, EID);
		b->escape_code += 51;
	}
	matched_len = b->match_len;
	code = base_accept_char(b, octet, c);
	if (code != -1)
	{
		if (!b->transparent)
			/* 7.5 Transfer - output the last state of the string */
			compressor_push_code(c, (u16)code);

		/* 7.8 Data compressibility test
		 * Same algorithm as the dreamcast modem driver, but we could use something different
		 * Accumulate the difference in bits between compressed and uncompressed size. Positive numbers
		 * indicate compressible data. */
		c->compress_test += 8 * matched_len - (int)b->code_word_size;
		c->compress_test_count -= matched_len;
		if (c->compress_test_count <= 0)
		{
			if (b->transparent && c->compress_test > 16)
				/* Schedule a switch to compressed mode */
				v42b_compressor_change_mode(c);
			else if (!b->transparent && c->compress_test < -16)
				/* Schedule a switch to transparent mode */
				v42b_compressor_change_mode(c);
		}
	}
}

void v42b_compressor_flush(v42b_compressor *c)
{
	compressor_flush(c, false);
}

int v42b_compressor_read(v42b_compressor *c)
{
	u8 v;
	if (c->out_count == 0)
		return -1;
	v = c->out[c->out_head];
	c->out_head = (c->out_head + 1) & (V42B_OUT_SIZE - 1);
	c->out_count--;
	return v;
}

int v42b_compressor_available(const v42b_compressor *c)
{
	return (int)c->out_count;
}

void v42b_compressor_serialize(const v42b_compressor *c, void **data, unsigned int *total_size)
{
	s32 test = c->compress_test;
	s32 test_count = c->compress_test_count;
	u32 bit_buf = c->out_bit_buf;
	u8 bit_pos = (u8)c->out_bit_pos;
	/* The queue: always its size and 2 bytes, the count, then the bytes in
	 * order and zeroes after them, so that two queues holding the same
	 * bytes are saved the same */
	u8 lin[V42B_OUT_SIZE];
	u16 n = (u16)c->out_count;
	unsigned i;

	base_serialize(&c->base, data, total_size);
	LIBRETRO_S(test);
	LIBRETRO_S(test_count);
	LIBRETRO_S(bit_buf);
	LIBRETRO_S(bit_pos);
	for (i = 0; i < V42B_OUT_SIZE; i++)
		lin[i] = i < c->out_count ? c->out[(c->out_head + i) & (V42B_OUT_SIZE - 1)] : 0;
	LIBRETRO_S(n);
	LIBRETRO_SA(lin, V42B_OUT_SIZE);
}

void v42b_compressor_unserialize(v42b_compressor *c, void **data, unsigned int *total_size)
{
	s32 test = 0;
	s32 test_count = 64;
	u8 bit_pos = 0;
	u16 n = 0;

	base_unserialize(&c->base, data, total_size);
	LIBRETRO_US(test);
	LIBRETRO_US(test_count);
	LIBRETRO_US(c->out_bit_buf);
	LIBRETRO_US(bit_pos);
	LIBRETRO_US(n);
	LIBRETRO_USA(c->out, V42B_OUT_SIZE);
	c->out_head = 0;
	c->out_count = n <= V42B_OUT_SIZE ? n : V42B_OUT_SIZE;
	c->compress_test = test;
	c->compress_test_count = test_count;
	/* Fewer bits than an octet are waiting between two codewords */
	c->out_bit_pos = bit_pos < 8 ? bit_pos : 0;
	c->out_bit_buf &= (1u << c->out_bit_pos) - 1;
}

/* The decompressor */

void v42b_decompressor_init(v42b_decompressor *d)
{
	memset(d, 0, sizeof(*d));
	v42b_decompressor_set_params(d, V42B_MIN_TOTAL_CODEWORDS, V42B_DEF_STRING_SIZE);
	v42b_decompressor_reset(d);
}

void v42b_decompressor_set_params(v42b_decompressor *d, u32 max_code_words, int max_string_length)
{
	base_set_params(&d->base, max_code_words, max_string_length);
}

void v42b_decompressor_reset(v42b_decompressor *d)
{
	base_reset(&d->base);
	d->escaped = false;
	d->input_bit_buffer = 0;
	d->input_bit_count = 0;
}

bool v42b_decompressor_write(v42b_decompressor *d, u8 octet, v42b_out_fn out, void *ctx)
{
	v42b_base *b = &d->base;
	u32 new_code;
	/* Walk up the tree one character at a time */
	u8 decode_buf[V42B_MAX_STRING_SIZE + 1];
	int pos = sizeof(decode_buf);
	int len, i;
	u16 node;
	u8 first_char;

	if (b->transparent)
	{
		if (d->escaped)
		{
			d->escaped = false;
			switch (octet)
			{
			case ECM:
				DEBUG_LOG(MODEM, "Decompressor: ECM\n");
				b->transparent = false;
				b->mode_transition = true;
				return true;
			case EID:
				DEBUG_LOG(MODEM, "Decompressor: EID\n");
				/* the escape code needs to be processed normally */
				octet = b->escape_code;
				b->escape_code += 51;
				break;
			case RESET:
				DEBUG_LOG(MODEM, "Decompressor: RESET\n");
				v42b_decompressor_reset(d);
				return true;
			default:
				/* 5.8(a) receipt of a reserved command code is an error condition */
				WARN_LOG(MODEM, "V.42bis: invalid command code %02x\n", octet);
				return false;
			}
		}
		else if (octet == b->escape_code)
		{
			d->escaped = true;
			return true;
		}
		out(ctx, octet);
		base_accept_char(b, octet, NULL);
		return true;
	}

	d->input_bit_buffer |= (u32)octet << d->input_bit_count;
	d->input_bit_count += 8;
	if (d->input_bit_count < (int)b->code_word_size)
		return true;
	new_code = d->input_bit_buffer & ((1u << b->code_word_size) - 1);
	d->input_bit_count -= b->code_word_size;
	d->input_bit_buffer >>= b->code_word_size;

	if (new_code < V42B_CW_RESERVED)
	{
		/* Control codes */
		switch (new_code)
		{
		case ETM:
			DEBUG_LOG(MODEM, "Decompressor: ETM\n");
			b->transparent = true;
			b->mode_transition = true;
			d->input_bit_count = 0;
			d->input_bit_buffer = 0;
			break;
		case FLUSH:
			DEBUG_LOG(MODEM, "Decompressor: FLUSH\n");
			/* 7.9(c) if step a) leaves extra bits pending for transmission (octet alignment not yet achieved),
			 * then transfer sufficient 0 bits to recover octet alignment */
			d->input_bit_buffer >>= d->input_bit_count & 7;
			d->input_bit_count &= ~7;
			break;
		case STEPUP:
			DEBUG_LOG(MODEM, "Decompressor: STEPUP\n");
			/* 5.8(a) receipt of a STEPUP codeword when it would cause the value of C2 to exceed N1
			 * (the size of the largest codeword) is an error condition */
			if ((1u << b->code_word_size) >= b->max_code_words)
			{
				WARN_LOG(MODEM, "V.42bis: codeword size exceeds the limit\n");
				return false;
			}
			++b->code_word_size;
			b->code_word_threshold *= 2;
			break;
		}
		return true;
	}
	b->mode_transition = false;
	/* 5.8(c) receipt of a codeword representing an empty dictionary entry is an error condition */
	if (new_code >= b->max_code_words || dict_is_free(&b->dict, new_code)
			/* and so is a string to extend that is gone */
			|| (b->cur_code != -1 && dict_is_free(&b->dict, (u32)b->cur_code)))
	{
		WARN_LOG(MODEM, "V.42bis: unknown code %x\n", new_code);
		return false;
	}
	node = (u16)new_code;
	for (;;)
	{
		decode_buf[--pos] = b->dict.character[node];
		if (b->dict.parent[node] == NODE_ROOT)
			break;
		node = b->dict.parent[node];
		if (pos == 0 || node >= b->max_code_words || node < V42B_CW_RESERVED)
		{
			WARN_LOG(MODEM, "V.42bis: string too long\n");
			return false;
		}
	}
	first_char = decode_buf[pos];
	len = (int)sizeof(decode_buf) - pos;
	/* Output the decoded string */
	for (i = pos; i < (int)sizeof(decode_buf); i++)
	{
		out(ctx, decode_buf[i]);
		if (decode_buf[i] == b->escape_code)
			b->escape_code += 51;
	}
	/* 6.4 Add the string to the dictionary */
	if (b->match_len < b->max_string_length
		/* 6.4(a) The string does not exceed N7 in length */
		&& b->last_added_code != b->cur_code
		&& b->cur_code != -1
		/* 6.4(b) The string is not already in the dictionary */
		&& dict_get_child(&b->dict, (u16)b->cur_code, first_char) == NODE_NONE)
	{
		dict_add_child(&b->dict, (u16)b->next_code_word, (u16)b->cur_code, first_char);
		base_recover_node(b);
		b->last_added_code = (int)b->next_code_word;
	}
	else
		b->last_added_code = -1;
	b->cur_code = (int)new_code;
	b->match_len = len;
	return true;
}

void v42b_decompressor_serialize(const v42b_decompressor *d, void **data, unsigned int *total_size)
{
	u8 esc = d->escaped;
	u32 bit_buffer = d->input_bit_buffer;
	u8 bit_count = (u8)d->input_bit_count;

	base_serialize(&d->base, data, total_size);
	LIBRETRO_S(esc);
	LIBRETRO_S(bit_buffer);
	LIBRETRO_S(bit_count);
}

void v42b_decompressor_unserialize(v42b_decompressor *d, void **data, unsigned int *total_size)
{
	u8 esc = 0;
	u8 bit_count = 0;

	base_unserialize(&d->base, data, total_size);
	LIBRETRO_US(esc);
	LIBRETRO_US(d->input_bit_buffer);
	LIBRETRO_US(bit_count);
	d->escaped = esc != 0;
	/* Fewer bits than a codeword are waiting */
	d->input_bit_count = bit_count < d->base.code_word_size ? bit_count : 0;
	d->input_bit_buffer &= (1u << d->input_bit_count) - 1;
}
