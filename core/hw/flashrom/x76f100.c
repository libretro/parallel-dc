/*
 * x76f100.c
 *
 * From mame x76f100.cpp
 * // license:BSD-3-Clause
 * // copyright-holders:smf
 */
#include "x76f100.h"
#include <string.h>

/* Each write to the port sets the chip's four lines at once, chip select
 * and reset first, then the clock, then the data: a data line that
 * changes in the same write as the clock is not a start or a stop. The
 * password a game sends is not checked. */

void x76f100_setData(X76F100 *f, const u8 *src)
{
	memcpy(f->responseToReset, src, sizeof(f->responseToReset));
	src += sizeof(f->responseToReset);
	memcpy(f->writePassword, src, sizeof(f->writePassword));
	src += sizeof(f->writePassword);
	memcpy(f->readPassword, src, sizeof(f->readPassword));
	src += sizeof(f->readPassword);
	memcpy(f->data, src, sizeof(f->data));
}

void x76f100_reset(X76F100 *f)
{
	f->lastSCL = false;
	f->SCLChanged = false;
	f->lastSDA = false;
	f->sda = false;
	f->lastRST = false;
	f->lastCS = false;
	f->state = X76F100_STOP;
	f->bit = 0;
	f->byteOffset = 0;
	f->command = 0;
	f->shift = 0;
	memset(f->writeBuffer, 0, sizeof(f->writeBuffer));
}

static int x76f100_dataOffset(const X76F100 *f)
{
	/* (blocks 14 and 15 are past the 112 bytes) */
	const int offset = ((f->command >> 1) & 0x0f) * 8 + f->byteOffset;
	return offset < (int)sizeof(f->data) ? offset : -1;
}

void x76f100_writeCS(X76F100 *f, bool v)
{
	v = v != 0;
	if (f->lastCS != v)
		f->state = X76F100_STOP;
	if (!f->lastCS && v)
		f->sda = false;
	f->lastCS = v;
}

void x76f100_writeRST(X76F100 *f, bool v)
{
	v = v != 0;
	if (v && !f->lastRST && !f->lastCS)
	{
		f->state = X76F100_RESPONSE_TO_RESET;
		f->bit = 0;
		f->byteOffset = 0;
	}
	f->lastRST = v;
}

void x76f100_writeSCL(X76F100 *f, bool v)
{
	v = v != 0;
	if (!f->lastCS)
	{
		switch (f->state)
		{
		case X76F100_RESPONSE_TO_RESET:
			if (f->lastSCL && !v)
			{
				if (f->bit == 0)
					f->shift = f->responseToReset[f->byteOffset];
				f->sda = f->shift & 1;
				f->shift >>= 1;
				if (++f->bit == 8)
				{
					f->bit = 0;
					if (++f->byteOffset == sizeof(f->responseToReset))
						f->byteOffset = 0;
				}
			}
			break;

		case X76F100_LOAD_COMMAND:
		case X76F100_LOAD_PASSWORD:
		case X76F100_VERIFY_PASSWORD:
		case X76F100_WRITE_DATA:
			if (!f->lastSCL && v)
			{
				if (f->bit < 8)
				{
					f->shift = (f->shift << 1) | (f->lastSDA ? 1 : 0);
					f->bit++;
					break;
				}
				/* the ninth clock: the byte is in */
				f->sda = false;
				switch (f->state)
				{
				case X76F100_LOAD_COMMAND:
					f->command = f->shift;
					f->state = X76F100_LOAD_PASSWORD;
					break;

				case X76F100_LOAD_PASSWORD:
					f->writeBuffer[f->byteOffset++] = f->shift;
					if (f->byteOffset == sizeof(f->writeBuffer))
						f->state = X76F100_VERIFY_PASSWORD;
					break;

				case X76F100_VERIFY_PASSWORD:
					/* the password acknowledge poll: any password is taken */
					if (f->shift == 0x55)
					{
						if ((f->command & 0x81) == 0x81)
							f->state = X76F100_READ_DATA;
						else if ((f->command & 0x81) == 0x80)
						{
							f->state = X76F100_WRITE_DATA;
							/* (the eight bytes of the password are not data) */
							f->byteOffset = 0;
						}
					}
					break;

				case X76F100_WRITE_DATA:
					f->writeBuffer[f->byteOffset++] = f->shift;
					if (f->byteOffset == sizeof(f->writeBuffer))
					{
						if (f->command == 0xfc)
							memcpy(f->writePassword, f->writeBuffer, sizeof(f->writePassword));
						else if (f->command == 0xfe)
							memcpy(f->readPassword, f->writeBuffer, sizeof(f->readPassword));
						else
						{
							for (f->byteOffset = 0; f->byteOffset < sizeof(f->writeBuffer); f->byteOffset++)
							{
								const int offset = x76f100_dataOffset(f);
								if (offset == -1)
									break;
								f->data[offset] = f->writeBuffer[f->byteOffset];
							}
						}
						f->byteOffset = 0;
					}
					break;
				}
				f->bit = 0;
				f->shift = 0;
			}
			break;

		case X76F100_READ_DATA:
			if (!f->lastSCL && v)
			{
				if (f->bit < 8)
				{
					if (f->bit == 0)
					{
						const int offset = x76f100_dataOffset(f);
						f->shift = offset != -1 ? f->data[offset] : 0;
					}
					f->sda = (f->shift >> 7) & 1;
					f->shift <<= 1;
					f->bit++;
				}
				else
				{
					/* the ninth clock: the next byte if the NAOMI acknowledges */
					f->bit = 0;
					f->sda = false;
					if (!f->lastSDA)
						f->byteOffset++;
				}
			}
			break;

		default:
			break;
		}
	}
	f->SCLChanged = f->lastSCL != v;
	f->lastSCL = v;
}

void x76f100_writeSDA(X76F100 *f, bool v)
{
	v = v != 0;
	if (f->lastSCL && !f->SCLChanged && !f->lastCS)
	{
		if (!f->lastSDA && v)
		{
			/* stop */
			f->state = X76F100_STOP;
			f->sda = false;
		}
		if (f->lastSDA && !v)
		{
			/* start */
			if (f->state == X76F100_STOP)
				f->state = X76F100_LOAD_COMMAND;
			f->bit = 0;
			f->byteOffset = 0;
			f->shift = 0;
			f->sda = false;
		}
	}
	f->lastSDA = v;
}

void x76f100_serialize(X76F100 *f, void **data, unsigned int *total_size)
{
	u8 regs[6];

	regs[0] = f->lastSCL | (f->SCLChanged << 1) | (f->lastSDA << 2) | (f->sda << 3) | (f->lastRST << 4) | (f->lastCS << 5);
	regs[1] = f->state;
	regs[2] = f->command;
	regs[3] = f->byteOffset;
	regs[4] = f->bit;
	regs[5] = f->shift;
	LIBRETRO_SA(regs, 6);
	LIBRETRO_SA(f->writeBuffer, 8);
	LIBRETRO_SA(f->data, 112);
	LIBRETRO_SA(f->readPassword, 8);
	LIBRETRO_SA(f->writePassword, 8);
}

void x76f100_unserialize(X76F100 *f, void **data, unsigned int *total_size)
{
	u8 regs[6] = { 0 };

	LIBRETRO_USA(regs, 6);
	LIBRETRO_USA(f->writeBuffer, 8);
	LIBRETRO_USA(f->data, 112);
	LIBRETRO_USA(f->readPassword, 8);
	LIBRETRO_USA(f->writePassword, 8);
	f->lastSCL = regs[0] & 1;
	f->SCLChanged = (regs[0] >> 1) & 1;
	f->lastSDA = (regs[0] >> 2) & 1;
	f->sda = (regs[0] >> 3) & 1;
	f->lastRST = (regs[0] >> 4) & 1;
	f->lastCS = (regs[0] >> 5) & 1;
	f->state = regs[1] <= X76F100_WRITE_DATA ? regs[1] : (u8)X76F100_STOP;
	f->command = regs[2];
	f->byteOffset = regs[3];
	f->bit = regs[4] < 8 ? regs[4] : 8;
	f->shift = regs[5];
	/* (where a byte goes into a buffer, its place is inside it) */
	if ((f->state == X76F100_RESPONSE_TO_RESET && f->byteOffset >= sizeof(f->responseToReset))
			|| ((f->state == X76F100_LOAD_COMMAND || f->state == X76F100_LOAD_PASSWORD || f->state == X76F100_WRITE_DATA)
				&& f->byteOffset >= sizeof(f->writeBuffer)))
		f->byteOffset = 0;
	if (f->state == X76F100_RESPONSE_TO_RESET && f->bit >= 8)
		f->bit = 0;
}
