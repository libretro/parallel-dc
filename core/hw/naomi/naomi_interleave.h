/* Two ROMs that share a cartridge address range, a 16-bit word each in
 * turn: one of them spread over every other word. C89. */
#ifndef NAOMI_INTERLEAVE_H
#define NAOMI_INTERLEAVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The 16-bit words of @blob (@len bytes of them) to every other word from
 * @to on. The words in between are the other ROM's and are left as they
 * are. */
void naomi_copy_interleaved(uint16_t *to, const uint8_t *blob, uint32_t len);

/* archive_entry_stream()'s sink for such a ROM, @ud where it starts: a
 * piece at @offset of the ROM goes @offset words on from there (the
 * pieces are an even number of bytes apart). */
int naomi_interleave_piece(void *ud, uint64_t offset, const uint8_t *p, size_t len);

#ifdef __cplusplus
}
#endif

#endif
