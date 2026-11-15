/*
	A serial port and what is plugged into it.

	A device on a serial line (a card reader, a touch panel, a hopper) is a
	pipe: the port hands it each byte the game sends and asks it for the
	bytes it has to send back. The ports are the SH4's SCIF
	(hw/sh4/modules/serial.c) and the RS-422 line of a NAOMI's I/O board
	bridge (hw/maple/maple_devs.cpp).

	Everything here runs on the thread that runs the machine.
*/
#pragma once
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct serial_pipe serial_pipe;

typedef struct serial_pipe_ops
{
	/* a byte the game sends */
	void (*write)(serial_pipe *pipe, u8 data);
	/* how many bytes the device has for the game */
	int (*available)(serial_pipe *pipe);
	/* the next of them (only asked when there is one) */
	u8 (*read)(serial_pipe *pipe);
} serial_pipe_ops;

/* A device: the first member of its own struct */
struct serial_pipe
{
	const serial_pipe_ops *ops;
};

#ifdef __cplusplus
}
#endif
