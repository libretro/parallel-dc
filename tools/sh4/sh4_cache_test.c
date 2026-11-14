/* The SH4's instruction and operand caches (core/hw/sh4/sh4_cache.c), by
 * themselves: memory, the TLB and the registers they read are stand-ins
 * here. A long run of random accesses through them - the cached memory
 * functions, OCBI/OCBP/OCBWB, PREF, the address and data arrays - with the
 * cache's settings, the MMU's and the TLB's changed now and then; what they
 * give back, the exceptions they raise, what reaches memory and what a save
 * state holds of them is hashed, and must be what it always was
 * (sh4_cache_expected.txt). Built with STRICT_MODE, which is where the
 * cached memory functions are. */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "types.h"
#include "hw/sh4/sh4_cache.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/mem/_vmem.h"

/* The stand-ins */

RegisterStruct CCN[SH4_CCN_REGS];
Sh4RCB *p_sh4rcb;

ReadMem8Func ReadMem8;
ReadMem16Func ReadMem16;
ReadMem16Func IReadMem16;
ReadMem32Func ReadMem32;
ReadMem64Func ReadMem64;
WriteMem8Func WriteMem8;
WriteMem16Func WriteMem16;
WriteMem32Func WriteMem32;
WriteMem64Func WriteMem64;

#define RAM_BASE 0x0C000000u
#define WINDOW   0x10000u

/* RAM, which GetMemPtr() gives; and memory it does not, read and written
 * through _vmem_*() */
static u8 ram[WINDOW];
static u8 io[WINDOW];

static u32 hash = 2166136261u;

static void mix(u32 v)
{
   int i;
   for (i = 0; i < 4; i++)
   {
      hash ^= (v >> (i * 8)) & 0xff;
      hash *= 16777619u;
   }
}

static u32 hash_bytes(const u8 *p, size_t n)
{
   u32 h = 2166136261u;
   size_t i;
   for (i = 0; i < n; i++)
   {
      h ^= p[i];
      h *= 16777619u;
   }
   return h;
}

u8 *GetMemPtr(u32 addr, u32 size)
{
   if (((addr & 0x1FFFFFFF) & ~(WINDOW - 1)) == RAM_BASE)
      return &ram[addr & (WINDOW - 1) & ~(size - 1)];
   return NULL;
}

static u64 io_read(u32 addr, int size)
{
   u64 v = 0;
   int i;
   mix(0x10000000u | (u32)size);
   mix(addr);
   for (i = size - 1; i >= 0; i--)
      v = (v << 8) | io[(addr + i) & (WINDOW - 1)];
   return v;
}

static void io_write(u32 addr, u64 v, int size)
{
   int i;
   mix(0x20000000u | (u32)size);
   mix(addr);
   mix((u32)v);
   for (i = 0; i < size; i++, v >>= 8)
      io[(addr + i) & (WINDOW - 1)] = (u8)v;
}

/* (memory read past the cache: RAM is read in place) */
static u64 mem_read(u32 addr, int size)
{
   u8 *p = GetMemPtr(addr, 1);
   if (p)
   {
      u64 v = 0;
      int i;
      for (i = size - 1; i >= 0; i--)
         v = (v << 8) | ram[(addr + i) & (WINDOW - 1)];
      return v;
   }
   return io_read(addr, size);
}

static void mem_write(u32 addr, u64 v, int size)
{
   u8 *p = GetMemPtr(addr, 1);
   if (p)
   {
      int i;
      for (i = 0; i < size; i++, v >>= 8)
         ram[(addr + i) & (WINDOW - 1)] = (u8)v;
      return;
   }
   io_write(addr, v, size);
}

u8 DYNACALL _vmem_ReadMem8(u32 a) { return (u8)mem_read(a, 1); }
u16 DYNACALL _vmem_ReadMem16(u32 a) { return (u16)mem_read(a, 2); }
u32 DYNACALL _vmem_ReadMem32(u32 a) { return (u32)mem_read(a, 4); }
u64 DYNACALL _vmem_ReadMem64(u32 a) { return mem_read(a, 8); }
void DYNACALL _vmem_WriteMem8(u32 a, u8 d) { mem_write(a, d, 1); }
void DYNACALL _vmem_WriteMem16(u32 a, u16 d) { mem_write(a, d, 2); }
void DYNACALL _vmem_WriteMem32(u32 a, u32 d) { mem_write(a, d, 4); }
void DYNACALL _vmem_WriteMem64(u32 a, u64 d) { mem_write(a, d, 8); }

/* The TLB: one entry, for every address it is asked about */
static TLB_Entry tlb;
static bool tlb_miss;
static u32 tlb_or;

static u32 lookup(u32 va, const TLB_Entry **entry, u32 *rv)
{
   mix(0x30000000u);
   mix(va);
   if (tlb_miss)
      return MMU_ERROR_TLB_MISS;
   *entry = &tlb;
   *rv = (va & 0x1FFFFFFF) | tlb_or;
   return MMU_ERROR_NONE;
}

u32 mmu_full_lookup_c(u32 va, const TLB_Entry **entry, u32 *rv)
{
   return lookup(va, entry, rv);
}

u32 mmu_instruction_lookup_c(u32 va, const TLB_Entry **entry, u32 *rv)
{
   mix(0x31000000u);
   return lookup(va, entry, rv);
}

u32 mmu_full_SQ_c(u32 va, u32 *rv, u32 translation_type)
{
   mix(0x32000000u | translation_type);
   mix(va);
   *rv = va;
   return (va & 0x100) ? MMU_ERROR_NONE : MMU_ERROR_PROTECTED;
}

/* (the exception is taken: the access goes no further) */
static jmp_buf raised;

void mmu_raise_exception(u32 mmu_error, u32 address, u32 am)
{
   mix(0x40000000u | (mmu_error << 8) | am);
   mix(address);
   longjmp(raised, 1);
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

/* The run */

static u32 seed = 12345;

static u32 rnd(void)
{
   seed = seed * 1103515245u + 12345u;
   return (seed >> 8) & 0xffffff;
}

static u32 any_address(void)
{
   static const u32 bases[] = {
      0x0C000000, 0x2C000000, 0x4C000000, 0x6C000000, /* P0/U0 */
      0x0E000000, 0x8E000000, /* address bit 25: the caches' IIX and OIX */
      0x8C000000, /* P1 */
      0xAC000000, /* P2 */
      0xCC000000, /* P3 */
      0xE0000000, /* P4: the store queues */
      0xF4000000, /* P4: the caches' arrays */
      0x10000000, /* not RAM */
      0x7C000000  /* not translated */
   };
   u32 a = bases[rnd() % (sizeof(bases) / sizeof(bases[0]))];
   /* (a few lines, often the same ones: hits, misses and write-backs) */
   u32 off = (rnd() & 0x3) ? (rnd() & 0xfff) : (rnd() & (WINDOW - 1));
   return a | off;
}

static u32 aligned(u32 a, u32 size)
{
   /* (now and then not, for the alignment error) */
   if ((rnd() & 63) == 0)
      return a | 1;
   return a & ~(size - 1);
}

static void configure(void)
{
   CCN_CCR.reg_data = rnd() & 0x81a7; /* OCE WT CB ORA OIX ICE IIX */
   CCN_MMUCR.AT = (rnd() & 1);
   p_sh4rcb->cntx.sr.MD = (rnd() & 3) != 0;
   tlb.Data.reg_data = rnd();
   tlb_miss = (rnd() & 7) == 0;
   tlb_or = (rnd() & 7) == 0 ? 0x1C000000 : 0;
}

static void step(void)
{
   u32 op = rnd() % 20;
   u32 a = any_address();
   u32 v = rnd() | (rnd() << 24);

   mix(op);
   if (setjmp(raised))
      return;
   switch (op)
   {
   case 0: mix(IReadMem16(aligned(a, 2))); break;
   case 1: mix(ReadMem8(a)); break;
   case 2: mix(ReadMem16(aligned(a, 2))); break;
   case 3: mix(ReadMem32(aligned(a, 4))); break;
   case 4:
   {
      u64 r = ReadMem64(aligned(a, 8));
      mix((u32)r);
      mix((u32)(r >> 32));
      break;
   }
   case 5: WriteMem8(a, (u8)v); break;
   case 6: WriteMem16(aligned(a, 2), (u16)v); break;
   case 7: WriteMem32(aligned(a, 4), v); break;
   case 8: WriteMem64(aligned(a, 8), ((u64)v << 32) | rnd()); break;
   case 9: sh4_ocache_write_back(a, rnd() & 1, rnd() & 1); break;
   case 10: sh4_ocache_prefetch(a); break;
   case 11:
      if ((rnd() & 3) == 0)
         /* (associative, for an address the cache may have) */
         sh4_icache_write_address_array(0xF0000008 | (a & 0x1FE0), (a & ~0x3ff) | (v & 1));
      else if (rnd() & 1)
         sh4_icache_write_address_array(a, v);
      else
         mix(sh4_icache_read_address_array(a));
      break;
   case 12:
      if (rnd() & 1)
         sh4_icache_write_data_array(a, v);
      else
         mix(sh4_icache_read_data_array(a));
      break;
   case 13:
      if ((rnd() & 3) == 0)
         sh4_ocache_write_address_array(0xF4000008 | (a & 0x3FE0), (a & ~0x3ff) | (v & 3));
      else if (rnd() & 1)
         sh4_ocache_write_address_array(a, v);
      else
         mix(sh4_ocache_read_address_array(a));
      break;
   case 14:
      if (rnd() & 1)
         sh4_ocache_write_data_array(a, v);
      else
         mix(sh4_ocache_read_data_array(a));
      break;
   case 15:
      if ((rnd() & 15) == 0)
         sh4_icache_invalidate();
      if ((rnd() & 15) == 0)
         sh4_ocache_invalidate();
      break;
   case 16:
   case 17:
      configure();
      break;
   case 18:
      if ((rnd() & 31) == 0)
         sh4_caches_write_back();
      break;
   default:
      /* RAM written past the cache */
      mem_write(RAM_BASE | (a & (WINDOW - 4)), v, 4);
      break;
   }
}

/* What a save state has of them; and it read back gives the same */
static u32 state_hash(void)
{
   void *p = state;
   unsigned int size = 0, size2 = 0;
   u32 h;
   sh4_icache_serialize(&p, &size);
   sh4_ocache_serialize(&p, &size);
   h = hash_bytes(state, size) ^ size;
   sh4_caches_reset(true);
   p = state;
   sh4_icache_unserialize(&p, &size2);
   sh4_ocache_unserialize(&p, &size2);
   if (size2 != size)
   {
      printf("FAIL the state read back is %u bytes, written %u\n", size2, size);
      exit(1);
   }
   p = state + size;
   size2 = 0;
   sh4_icache_serialize(&p, &size2);
   sh4_ocache_serialize(&p, &size2);
   if (memcmp(state, state + size, size))
   {
      printf("FAIL the state read back is not the one written\n");
      exit(1);
   }
   return h;
}

int main(void)
{
   int i;

   p_sh4rcb = (Sh4RCB *)calloc(1, sizeof(*p_sh4rcb));
   if (!p_sh4rcb)
      return 1;
   for (i = 0; i < (int)WINDOW; i++)
   {
      ram[i] = (u8)(i * 7);
      io[i] = (u8)(i * 13 + 1);
   }
   sh4_caches_reset(true);
   sh4_caches_set_handlers();
   configure();

   for (i = 1; i <= 200000; i++)
   {
      step();
      if (i % 20000 == 0)
         printf("%6d: %08x ram %08x io %08x state %08x\n", i, (unsigned)hash,
               (unsigned)hash_bytes(ram, WINDOW), (unsigned)hash_bytes(io, WINDOW),
               (unsigned)state_hash());
   }
   free(p_sh4rcb);
   return 0;
}
