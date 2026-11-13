/* The SH4 opcode table (core/hw/sh4/sh4_opcode_list.c): what each of the
 * 65536 opcodes is said to be - its entry, mask, type, cycles, unit,
 * exception group, recompiler decoding, name and handlers - hashed, against
 * what the table has always said (sh tools/sh4/opcode_table_run.sh). */
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "hw/sh4/sh4_opcode_list.h"

static unsigned h = 2166136261u;

/* FNV-1a */
static void mix(const void *p, size_t n)
{
	const unsigned char *b = (const unsigned char *)p;
	while (n--)
	{
		h ^= *b++;
		h *= 16777619u;
	}
}

int main(void)
{
	unsigned i;
	int n;

	BuildOpcodeTables();
	/* (a second call leaves it as it is) */
	BuildOpcodeTables();
	for (n = 0; opcodes[n].oph; n++)
		;
	for (i = 0; i < 0x10000; i++)
	{
		const sh4_opcodelistentry *e = OpDesc[i];
		u32 v[8];
		v[0] = (e >= opcodes && e < opcodes + n) ? (u32)(e - opcodes) : 0xFFFFFFFFu;
		v[1] = e->mask;
		v[2] = e->rez;
		v[3] = e->type;
		v[4] = e->IssueCycles;
		v[5] = e->LatencyCycles;
		v[6] = e->unit;
		v[7] = e->ex_type;
		mix(v, sizeof(v));
		mix(&e->decode, 8);
		mix(&e->fallbacks, 8);
		mix(e->diss, strlen(e->diss));
		v[0] = OpPtr[i] == e->oph;
		v[1] = e->rec_oph != 0;
		mix(v, 8);
	}
	printf("%d entries, %08x\n", n, h);
	return 0;
}
