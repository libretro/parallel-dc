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

/* The same, for a device in C++: a Pipe is a serial_pipe whose operations
 * are its methods */
class SerialPort
{
public:
	class Pipe : public serial_pipe
	{
	public:
		Pipe() { ops = cxx_ops(); }
		Pipe(const Pipe&) : serial_pipe() { ops = cxx_ops(); }
		Pipe& operator=(const Pipe&) { return *this; }

		// a byte the game sends
		virtual void write(u8 data) { }
		virtual void sendBreak() { }
		// how many bytes the device has for the game
		virtual int available() { return 0; }
		// the next of them (only asked when there is one)
		virtual u8 read() { return 0; }

		virtual ~Pipe() = default;

	private:
		static void op_write(serial_pipe *p, u8 data) { static_cast<Pipe *>(p)->write(data); }
		static int op_available(serial_pipe *p) { return static_cast<Pipe *>(p)->available(); }
		static u8 op_read(serial_pipe *p) { return static_cast<Pipe *>(p)->read(); }
		static const serial_pipe_ops *cxx_ops()
		{
			static const serial_pipe_ops o = { &op_write, &op_available, &op_read };
			return &o;
		}
	};

	virtual void setPipe(Pipe *pipe) = 0;
	// The device has something new to send: the port is to look for it.
	virtual void updateStatus() = 0;
	virtual void receiveBreak() { }

	virtual ~SerialPort() = default;
};
#endif
