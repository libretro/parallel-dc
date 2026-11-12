/* The text of a .gdi or .cue sheet, read word by word and number by number
 * as the C++ streams the sheets were read with read them, so that a sheet
 * reads now as it read then, a broken one too:
 *
 * - words and numbers come after whitespace, which is skipped;
 * - a number is the digits there are, with a sign, and stops at the first
 *   character that is not one: "01AUDIO" is 1, and then the word AUDIO;
 * - a read that gets nothing - at the end of the text, or no digits where
 *   a number was wanted - leaves what it was to fill as it was (a number
 *   with no digits is 0), and every read after it gets nothing too until
 *   sheet_clear(): sheet.fail;
 * - a read that comes to the end of the text sets sheet.eof.
 *
 * A word is cut short to fit; sheet.cut says that the last one was. C89. */
#ifndef IMGREAD_SHEET_H
#define IMGREAD_SHEET_H

#include <stddef.h>
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sheet
{
   const char *p;
   const char *end;
   int fail;
   int eof;
   int cut;
} sheet_t;

void sheet_init(sheet_t *s, const char *text, size_t len);
/* (both flags off: what clear() did) */
void sheet_clear(sheet_t *s);
/* The next word into @out, @size bytes with its 0; 0 for none, and @out
 * left as it was */
int sheet_word(sheet_t *s, char *out, size_t size);
/* The next character that is not whitespace, or with @raw the next one
 * whatever it is; 0 for none, and @c left as it was */
int sheet_char(sheet_t *s, char *c, int raw);
/* A number, unsigned or signed: one past what it takes is its largest,
 * and fails */
int sheet_u32(sheet_t *s, u32 *v);
int sheet_s32(sheet_t *s, s32 *v);

#ifdef __cplusplus
}
#endif

#endif
