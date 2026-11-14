/* V.42 bis compression (core/hw/modem/v42bis.c), by itself. Text, runs,
 * random bytes and the escape character, compressed with each of a few
 * pairs of parameters, flushed and switched between the two modes at random,
 * and the compressor and the decompressor saved and loaded on the way: what
 * the decompressor gives back must be what went in. What the compressor
 * sends, the states and what the decompressor makes of a damaged stream
 * are hashed, and must be what they always were (v42bis_expected.txt). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/modem/v42bis.h"

/* (no log: what is refused is counted) */
retro_log_printf_t log_cb;

static v42b_compressor comp;
static v42b_decompressor decomp;

static u32 seed;

static u32 rnd(void)
{
   seed = seed * 1103515245u + 12345u;
   return (seed >> 8) & 0xffffff;
}

static u32 fnv(u32 h, u8 v)
{
   return (h ^ v) * 16777619u;
}

/* What the decompressor gives back */
static u8 got[1 << 20];
static unsigned got_len;

static void take(void *ctx, u8 v)
{
   u32 *h = (u32 *)ctx;
   if (got_len < sizeof(got))
      got[got_len] = v;
   got_len++;
   *h = fnv(*h, v);
}

/* A save state's */
static u8 state[64 * 1024];

bool ra_serialize(const void *src, unsigned int src_size, void **dest, unsigned int *total_size)
{
   memcpy(*dest, src, src_size);
   *dest = (u8 *)*dest + src_size;
   *total_size += src_size;
   return true;
}

bool ra_unserialize(void *dest, unsigned int size, void **src, unsigned int *total_size)
{
   memcpy(dest, *src, size);
   *src = (u8 *)*src + size;
   *total_size += size;
   return true;
}

/* Both saved, made again and loaded; the state's hash */
static u32 save_load(void)
{
   void *p = state;
   unsigned int size = 0, size2 = 0;
   u32 h = 2166136261u;
   unsigned i;

   v42b_compressor_serialize(&comp, &p, &size);
   v42b_decompressor_serialize(&decomp, &p, &size);
   for (i = 0; i < size; i++)
      h = fnv(h, state[i]);
   v42b_compressor_init(&comp);
   v42b_decompressor_init(&decomp);
   p = state;
   v42b_compressor_unserialize(&comp, &p, &size2);
   v42b_decompressor_unserialize(&decomp, &p, &size2);
   if (size2 != size)
   {
      printf("FAIL the state read is %u bytes, written %u\n", size2, size);
      exit(1);
   }
   return h ^ size;
}

/* The input */
static u8 in[1 << 20];

static unsigned make_input(void)
{
   static const char *words[] = {
      "the ", "dreamcast ", "modem ", "connects ", "at ", "33600 ", "bps ",
      "GET /index.html HTTP/1.0\r\n", "\r\n", "PlanetWeb ", "<html>", "</td>"
   };
   unsigned n = 0;
   while (n < 300000)
   {
      u32 kind = rnd() % 5;
      u32 len = 1 + rnd() % 400;
      u32 i;
      for (i = 0; i < len && n < sizeof(in); i++)
      {
         switch (kind)
         {
         case 0: /* text */
         {
            const char *w = words[rnd() % (sizeof(words) / sizeof(words[0]))];
            while (*w && n < sizeof(in))
               in[n++] = (u8)*w++;
            break;
         }
         case 1: /* random */
            in[n++] = (u8)rnd();
            break;
         case 2: /* a run */
            in[n++] = (u8)(len & 0xff);
            break;
         case 3: /* the escape characters, which move along by 51 */
            in[n++] = (u8)(51 * (rnd() % 6));
            break;
         default: /* a few characters */
            in[n++] = (u8)("abc"[rnd() % 3]);
            break;
         }
      }
   }
   return n;
}

static void run(u32 p1, int p2, u32 s)
{
   unsigned n, i, sent = 0, saves = 0;
   u32 sent_hash = 2166136261u, got_hash = 2166136261u, state_hash = 0;

   seed = s;
   n = make_input();
   v42b_compressor_init(&comp);
   v42b_decompressor_init(&decomp);
   v42b_compressor_set_params(&comp, p1, p2);
   v42b_compressor_reset(&comp);
   v42b_decompressor_set_params(&decomp, p1, p2);
   v42b_decompressor_reset(&decomp);
   got_len = 0;

   for (i = 0; i < n; )
   {
      /* a frame's worth, as the protocol takes them */
      unsigned frame = 1 + rnd() % 256;
      int c;
      while (v42b_compressor_available(&comp) < (int)frame && i < n)
      {
         v42b_compressor_write(&comp, in[i++]);
         if ((rnd() % 3000) == 0)
            v42b_compressor_change_mode(&comp);
      }
      v42b_compressor_flush(&comp);
      while ((c = v42b_compressor_read(&comp)) != -1)
      {
         sent_hash = fnv(sent_hash, (u8)c);
         sent++;
         if (!v42b_decompressor_write(&decomp, (u8)c, take, &got_hash))
         {
            printf("FAIL p1 %u p2 %d: the decompressor refused byte %u\n", (unsigned)p1, p2, sent);
            exit(1);
         }
      }
      if ((rnd() % 40) == 0)
      {
         state_hash = state_hash * 31 + save_load();
         saves++;
      }
   }
   if (got_len != n || memcmp(got, in, n))
   {
      printf("FAIL p1 %u p2 %d: %u bytes back of %u, not the same\n", (unsigned)p1, p2, got_len, n);
      exit(1);
   }
   printf("p1 %4u p2 %3d: %u bytes, %u sent %08x, %u saves %08x\n", (unsigned)p1, p2, n, sent,
         (unsigned)sent_hash, saves, (unsigned)state_hash);
}

/* A stream with bytes changed: where the decompressor stops, what it made */
static void damaged(u32 s)
{
   static u8 stream[1 << 20];
   unsigned n, i, len = 0, refused = 0, flips = 0;
   u32 got_hash = 2166136261u;
   int c;

   seed = s;
   n = make_input();
   v42b_compressor_init(&comp);
   v42b_decompressor_init(&decomp);
   for (i = 0; i < n && len < sizeof(stream) - 4096; i++)
   {
      v42b_compressor_write(&comp, in[i]);
      if ((i & 255) == 255)
         v42b_compressor_flush(&comp);
      while ((c = v42b_compressor_read(&comp)) != -1)
         stream[len++] = (u8)c;
   }
   got_len = 0;
   for (i = 0; i < len; i++)
   {
      u8 b = stream[i];
      if ((rnd() % 5000) == 0)
      {
         b ^= (u8)(1 + rnd() % 255);
         flips++;
      }
      if (!v42b_decompressor_write(&decomp, b, take, &got_hash))
      {
         /* (the connection is released: it starts again) */
         refused++;
         v42b_decompressor_reset(&decomp);
      }
   }
   printf("damaged %08x: %u bytes, %u changed, %u refused, %u made %08x\n", (unsigned)s, len, flips,
         refused, got_len, (unsigned)got_hash);
}

/* A state with bytes changed loaded: what it is made into, and what the
 * two make of the rest */
static void damaged_state(u32 s)
{
   unsigned n, i, size = 0, size2 = 0, refused = 0;
   u32 got_hash = 2166136261u, state_hash = 2166136261u;
   void *p;
   int c;

   seed = s;
   n = make_input();
   v42b_compressor_init(&comp);
   v42b_decompressor_init(&decomp);
   v42b_compressor_set_params(&comp, 2048, 250);
   v42b_compressor_reset(&comp);
   v42b_decompressor_set_params(&decomp, 2048, 250);
   v42b_decompressor_reset(&decomp);
   got_len = 0;
   for (i = 0; i < n; i++)
   {
      v42b_compressor_write(&comp, in[i]);
      if (i == n / 2)
      {
         unsigned k;
         v42b_compressor_flush(&comp);
         while ((c = v42b_compressor_read(&comp)) != -1)
            v42b_decompressor_write(&decomp, (u8)c, take, &got_hash);
         p = state;
         v42b_compressor_serialize(&comp, &p, &size);
         v42b_decompressor_serialize(&decomp, &p, &size);
         for (k = 0; k < 200; k++)
            state[rnd() % size] ^= (u8)(1 + rnd() % 255);
         /* (some parents, to strings that are not in the dictionary, or in a circle) */
         for (k = 0; k < 40; k++)
         {
            unsigned at = 32 + 2 * (rnd() % 1700);
            state[at] = (u8)rnd();
            state[at + 1] = (u8)(rnd() % 9);
         }
         p = state;
         v42b_compressor_unserialize(&comp, &p, &size2);
         v42b_decompressor_unserialize(&decomp, &p, &size2);
         p = state;
         size2 = 0;
         v42b_compressor_serialize(&comp, &p, &size2);
         v42b_decompressor_serialize(&decomp, &p, &size2);
         for (k = 0; k < size2; k++)
            state_hash = fnv(state_hash, state[k]);
      }
      while ((c = v42b_compressor_read(&comp)) != -1)
         if (!v42b_decompressor_write(&decomp, (u8)c, take, &got_hash))
         {
            refused++;
            v42b_decompressor_reset(&decomp);
         }
   }
   printf("damaged state %08x: state %08x, %u refused, %u made %08x\n", (unsigned)s,
         (unsigned)state_hash, refused, got_len, (unsigned)got_hash);
}

int main(void)
{
   run(512, 6, 1);
   run(512, 250, 2);
   run(1024, 32, 3);
   run(2048, 6, 4);
   run(2048, 250, 5);
   /* (out of range: made the nearest) */
   run(100, 1, 6);
   run(65535, 255, 7);
   damaged(8);
   damaged(9);
   damaged_state(10);
   damaged_state(11);
   damaged_state(12);
   return 0;
}
