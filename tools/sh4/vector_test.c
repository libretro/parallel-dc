/* Runs the x86-64 recompiler's code for the SH4's two vector instructions,
 * FIPR and FTRV (core/rec-x64/x64_vector.h), against the reference
 * implementations the recompiler would otherwise call and the interpreter
 * agrees with, on random operands: every result has to be the same bits.
 * tools/sh4/run.sh builds and runs this. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "rec-x64/x64_vector.h"

typedef float (*fipr_fn)(const float *, const float *);
typedef void (*ftrv_fn)(float *, const float *, const float *);

/* as shil_canonical.h has them */
static float fipr_reference(const float *fn, const float *fm)
{
   double idp = (double)fn[0] * fm[0];
   idp += (double)fn[1] * fm[1];
   idp += (double)fn[2] * fm[2];
   idp += (double)fn[3] * fm[3];
   return (float)idp;
}

static void ftrv_reference(float *fd, const float *fn, const float *fm)
{
   int row;
   for (row = 0; row < 4; row++)
      fd[row] = (float)((double)fm[row] * fn[0] + (double)fm[row + 4] * fn[1]
            + (double)fm[row + 8] * fn[2] + (double)fm[row + 12] * fn[3]);
}

static unsigned long long seed = 0x9E3779B97F4A7C15ull;
static unsigned rnd(void)
{
   seed = seed * 6364136223846793005ull + 1442695040888963407ull;
   return (unsigned)(seed >> 32);
}

/* A float of any size, or one of a few million in size, as coordinates
 * are: the second kind is where sums lose their low bits. */
static unsigned bits_of(float f)
{
   unsigned u;
   memcpy(&u, &f, 4);
   return u;
}

static float value(void)
{
   unsigned bits = rnd();
   float f;
   if (rnd() & 1)
   {
      unsigned exp = 100 + rnd() % 60;
      bits = (bits & 0x807fffffu) | (exp << 23);
   }
   else if (((bits >> 23) & 0xff) == 0xff)
      bits &= ~0x00800000u;   /* not an infinity or a NaN: their bits are not the sum's to settle */
   memcpy(&f, &bits, 4);
   return f;
}

int main(int argc, char **argv)
{
   const long count = argc > 1 ? atol(argv[1]) : 5000000;
   long bad = 0, i;
   x86_emit e;
   u8 *code;
   fipr_fn fipr;
   ftrv_fn ftrv;

#ifdef _WIN32
   code = (u8 *)VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
   code = (u8 *)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
   if (code == (u8 *)MAP_FAILED)
      code = NULL;
#endif
   if (code == NULL)
   {
      printf("FAIL: no executable memory\n");
      return 1;
   }
   x86_init(&e, code, 4096);

   /* the host's own calling convention in, the recompiler's registers out */
   fipr = (fipr_fn)(uintptr_t)e.p;
#ifdef _WIN32
   x64_mov_rr64(&e, X86_EAX, X86_ECX); x64_mov_rr64(&e, X86_ECX, X86_EDX);
#else
   x64_mov_rr64(&e, X86_EAX, X86_EDI); x64_mov_rr64(&e, X86_ECX, X86_ESI);
#endif
   x64_emit_fipr(&e, 0);
   x86_ret(&e);

   ftrv = (ftrv_fn)(uintptr_t)e.p;
#ifdef _WIN32
   x64_mov_rr64(&e, X86_R9, X86_ECX); x64_mov_rr64(&e, X86_EAX, X86_EDX); x64_mov_rr64(&e, X86_ECX, X86_R8);
#else
   x64_mov_rr64(&e, X86_R9, X86_EDI); x64_mov_rr64(&e, X86_EAX, X86_ESI); x64_mov_rr64(&e, X86_ECX, X86_EDX);
#endif
   x64_emit_ftrv(&e);
   x64_movups_store(&e, x86_m(X86_R9, 0), 0);
   x86_ret(&e);
   if (e.full)
   {
      printf("FAIL: code did not fit\n");
      return 1;
   }

   for (i = 0; i < count; i++)
   {
      float v[4], w[4], m[16], want[4], got[4], a, b;
      int k;
      for (k = 0; k < 4; k++) v[k] = value();
      for (k = 0; k < 4; k++) w[k] = value();
      for (k = 0; k < 16; k++) m[k] = value();

      a = fipr(v, w);
      b = fipr_reference(v, w);
      if (memcmp(&a, &b, 4) && bad++ < 5)
         printf("FIPR: recompiled %08x, reference %08x\n", bits_of(a), bits_of(b));
      ftrv(got, v, m);
      ftrv_reference(want, v, m);
      if (memcmp(got, want, sizeof(got)) && bad++ < 5)
         printf("FTRV: recompiled %08x %08x %08x %08x, reference %08x %08x %08x %08x\n", bits_of(got[0]), bits_of(got[1]),
               bits_of(got[2]), bits_of(got[3]), bits_of(want[0]), bits_of(want[1]), bits_of(want[2]), bits_of(want[3]));
   }
   if (bad)
   {
      printf("FAIL: %ld of %ld differ\n", bad, 2 * count);
      return 1;
   }
   printf("sh4 vectors: ok (FIPR and FTRV as recompiled are the reference's, on %ld random operands each)\n", count);
   return 0;
}
