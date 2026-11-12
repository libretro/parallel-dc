/* The text of a .gdi or .cue sheet: see sheet.h */
#include <ctype.h>
#include <string.h>

#include "sheet.h"

void sheet_init(sheet_t *s, const char *text, size_t len)
{
   s->p    = text;
   s->end  = text + len;
   s->fail = 0;
   s->eof  = 0;
   s->cut  = 0;
}

void sheet_clear(sheet_t *s)
{
   s->fail = 0;
   s->eof  = 0;
}

/* Past whitespace: 0, failed, at the end */
static int sheet_skip(sheet_t *s)
{
   if (s->fail || s->eof)
   {
      s->fail = 1;
      return 0;
   }
   while (s->p < s->end && isspace((unsigned char)*s->p))
      s->p++;
   if (s->p >= s->end)
   {
      s->eof  = 1;
      s->fail = 1;
      return 0;
   }
   return 1;
}

int sheet_word(sheet_t *s, char *out, size_t size)
{
   size_t n = 0;

   if (!sheet_skip(s))
      return 0;
   s->cut = 0;
   while (s->p < s->end && !isspace((unsigned char)*s->p))
   {
      if (n + 1 < size)
         out[n++] = *s->p;
      else
         s->cut = 1;
      s->p++;
   }
   out[n] = '\0';
   if (s->p >= s->end)
      s->eof = 1;
   return 1;
}

int sheet_char(sheet_t *s, char *c, int raw)
{
   if (raw)
   {
      if (s->fail || s->eof || s->p >= s->end)
      {
         s->eof  = s->eof || s->p >= s->end;
         s->fail = 1;
         return 0;
      }
   }
   else if (!sheet_skip(s))
      return 0;
   *c = *s->p++;
   return 1;
}

/* The digits after a sign: 0 with none (and @neg, @mag what they were) */
static int sheet_digits(sheet_t *s, int *neg, u32 *mag, int *over)
{
   int digits = 0;

   *neg  = 0;
   *mag  = 0;
   *over = 0;
   if (s->p < s->end && (*s->p == '+' || *s->p == '-'))
      *neg = *s->p++ == '-';
   while (s->p < s->end && *s->p >= '0' && *s->p <= '9')
   {
      const u32 d = (u32)(*s->p++ - '0');
      if (*mag > (0xFFFFFFFFu - d) / 10)
         *over = 1;
      else
         *mag = *mag * 10 + d;
      digits = 1;
   }
   if (s->p >= s->end)
      s->eof = 1;
   if (!digits)
      s->fail = 1;
   return digits;
}

int sheet_u32(sheet_t *s, u32 *v)
{
   int neg, over;
   u32 mag;

   if (!sheet_skip(s))
      return 0;
   if (!sheet_digits(s, &neg, &mag, &over))
   {
      *v = 0;
      return 0;
   }
   if (over)
   {
      *v      = 0xFFFFFFFFu;
      s->fail = 1;
      return 0;
   }
   *v = neg ? 0u - mag : mag;
   return 1;
}

int sheet_s32(sheet_t *s, s32 *v)
{
   int neg, over;
   u32 mag;

   if (!sheet_skip(s))
      return 0;
   if (!sheet_digits(s, &neg, &mag, &over))
   {
      *v = 0;
      return 0;
   }
   if (over || mag > (neg ? 0x80000000u : 0x7FFFFFFFu))
   {
      *v      = neg ? (s32)(-0x7FFFFFFF - 1) : (s32)0x7FFFFFFF;
      s->fail = 1;
      return 0;
   }
   *v = neg ? (s32)(0u - mag) : (s32)mag;
   return 1;
}
