/*
 * x76f100.h
 *
 * From mame x76f100.h
 * // license:BSD-3-Clause
 * // copyright-holders:smf
 *
 * The X76F100 secure serial flash: 112 bytes behind a read and a write
 * password. A NAOMI has one on its main board and one on its ROM board,
 * which hold the boards' serial numbers.
 */
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

enum
{
	X76F100_STOP, X76F100_RESPONSE_TO_RESET, X76F100_LOAD_COMMAND,
	X76F100_LOAD_PASSWORD, X76F100_VERIFY_PASSWORD, X76F100_READ_DATA,
	X76F100_WRITE_DATA
};

/* All zeroes is a chip with nothing in it, stopped */
typedef struct X76F100
{
	u8 data[112];
	u8 readPassword[8];
	u8 writePassword[8];
	u8 responseToReset[4];
	u8 writeBuffer[8];
	bool lastSCL;
	bool lastSDA;
	bool sda;
	bool lastRST;
	bool lastCS;
	bool SCLChanged;
	u8 state;
	u8 command;
	u8 byteOffset;
	u8 bit;
	u8 shift;
} X76F100;

void x76f100_reset(X76F100 *f);
/* The whole chip, 0x84 bytes: the response to reset (4), the write
 * password (8), the read password (8) and the data (112) */
void x76f100_setData(X76F100 *f, const u8 *data);
static TYPES_INLINE const u8 *x76f100_getProtectedData(const X76F100 *f) { return f->data; }
void x76f100_writeCS(X76F100 *f, bool v);
void x76f100_writeRST(X76F100 *f, bool v);
void x76f100_writeSCL(X76F100 *f, bool v);
void x76f100_writeSDA(X76F100 *f, bool v);
static TYPES_INLINE bool x76f100_readSDA(const X76F100 *f) { return f->lastCS ? true : f->sda; }
/* 142 bytes: the lines and the command under way, the buffer, the data
 * and the passwords */
void x76f100_serialize(X76F100 *f, void **data, unsigned int *total_size);
void x76f100_unserialize(X76F100 *f, void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif
