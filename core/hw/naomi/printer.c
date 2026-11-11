/*
	The thermal printer of Touch de Uno! and Touch de Uno! 2, on their I/O
	board's serial port.

	The printer takes what the game sends it and reports that all is well:
	it is always ready, never out of paper. What is printed is not kept -
	making a picture of the ticket needs the printer's fonts, which a core
	has not got - so nothing of it is in a save state either.
*/
#include "printer.h"

static bool printerPresent;

void printer_init(void)
{
	printerPresent = true;
}

void printer_term(void)
{
	printerPresent = false;
}

void printer_print(char c)
{
	(void)c;
}

bool printer_present(void)
{
	return printerPresent;
}
