/* The SH4's address space (core/hw/mem/_vmem.c), by itself: the table of
 * what each 16 MB of it is - a handler's functions or a block of memory,
 * mirrored or not - and the reads and writes of each size that go through
 * it; where the recompilers find a constant address's memory or handler;
 * the layouts of the mapped address space the memories are put in; and the
 * protection of video memory's pages. What the reads give, what reaches
 * the memories and the handlers, the layouts and the protection calls are
 * hashed, and must be what they always were (vmem_expected.txt). */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "types.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/aica/aica_if.h"
#include "hw/sh4/dyna/blockmanager.h"

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

/* What the rest of the core has */

retro_log_printf_t log_cb;
settings_t settings;
unsigned RAM_SIZE = 16 * 1024 * 1024, VRAM_SIZE = 8 * 1024 * 1024, ARAM_SIZE = 2 * 1024 * 1024;
unsigned VRAM_MASK = 8 * 1024 * 1024 - 1;
VArray2 mem_b, vram, aica_ram;
Sh4RCB *p_sh4rcb;
bool mmu_strict;
RegisterStruct CCN[SH4_CCN_REGS];

void os_DebugBreak(void) { mix(0xdeadu); }

void varray2_zero(VArray2 *a)
{
   memset(a->data, 0, a->size);
}

void bm_vmem_pagefill(void **ptr, u32 size_bytes) { mix(0xb0000000u | size_bytes); }

/* (the protection of the host's pages: by whom, of what) */
static u8 *base_seen;

static void region(u32 kind, void *start, size_t len)
{
   mix(kind);
   mix(base_seen ? (u32)((u8 *)start - base_seen) : (u32)((u8 *)start - vram.data));
   mix((u32)len);
}

bool mem_region_lock(void *start, size_t len) { region(0xa1, start, len); return true; }
bool mem_region_unlock(void *start, size_t len) { region(0xa2, start, len); return true; }
bool mem_region_noaccess(void *start, size_t len) { region(0xa3, start, len); return true; }

/* The mapped address space: what the core asks for */
static VMemType layout = MemTypeError;
static u8 *space;
static size_t space_size;

VMemType vmem_platform_init(void **vmem_base_addr, void **sh4rcb_addr)
{
   if (layout == MemTypeError)
      return MemTypeError;
   *vmem_base_addr = space;
   *sh4rcb_addr = calloc(1, sizeof(Sh4RCB));
   return layout;
}

void vmem_platform_create_mappings(const vmem_mapping *maps, unsigned n)
{
   unsigned i;
   mix(n);
   for (i = 0; i < n; i++)
   {
      mix((u32)maps[i].start_address);
      mix((u32)(maps[i].start_address >> 32));
      mix((u32)maps[i].end_address);
      mix((u32)(maps[i].end_address >> 32));
      mix((u32)maps[i].memoffset);
      mix((u32)maps[i].memsize);
      mix(maps[i].allow_writes);
   }
}

void vmem_platform_reset_mem(void *ptr, unsigned size_bytes) { mix(0xc1u); mix(size_bytes); }
void vmem_platform_ondemand_page(void *address, unsigned size_bytes) { mix(0xc2u); mix(size_bytes); }
void vmem_platform_destroy(void)
{
   mix(0xc3u);
   free(p_sh4rcb);
   p_sh4rcb = NULL;
}

/* The run */

static u32 seed = 777;

static u32 rnd(void)
{
   seed = seed * 1103515245u + 12345u;
   return (seed >> 8) & 0xffffff;
}

/* Two blocks of memory, and the handlers' log */
static u8 *block_a, *block_b;
#define BLOCK_A (2 * 1024 * 1024)
#define BLOCK_B (64 * 1024)

static u8 DYNACALL h_r8(u32 a) { mix(0x08000000u); mix(a); return (u8)(a * 3); }
static u16 DYNACALL h_r16(u32 a) { mix(0x16000000u); mix(a); return (u16)(a * 5 + 0x8000); }
static u32 DYNACALL h_r32(u32 a) { mix(0x32000000u); mix(a); return a * 7 + 0x80000000u; }
static void DYNACALL h_w8(u32 a, u8 d) { mix(0x08100000u); mix(a); mix(d); }
static void DYNACALL h_w16(u32 a, u16 d) { mix(0x16100000u); mix(a); mix(d); }
static void DYNACALL h_w32(u32 a, u32 d) { mix(0x32100000u); mix(a); mix(d); }

static u8 DYNACALL g_r8(u32 a) { mix(0x08200000u); mix(a); return (u8)~a; }
static u32 DYNACALL g_r32(u32 a) { mix(0x32200000u); mix(a); return ~a; }
static void DYNACALL g_w32(u32 a, u32 d) { mix(0x32300000u); mix(a); mix(d); }

/* (where a pointer goes: one of the blocks, a handler's function, or else) */
static u32 where(void *p, bool ismem)
{
   if (!ismem)
   {
      union { void *p; _vmem_ReadMem8FP *r8; _vmem_ReadMem16FP *r16; _vmem_ReadMem32FP *r32;
         _vmem_WriteMem8FP *w8; _vmem_WriteMem16FP *w16; _vmem_WriteMem32FP *w32; } u;
      u.p = p;
      if (u.r8 == h_r8) return 1;
      if (u.r16 == h_r16) return 2;
      if (u.r32 == h_r32) return 3;
      if (u.w8 == h_w8) return 4;
      if (u.w16 == h_w16) return 5;
      if (u.w32 == h_w32) return 6;
      if (u.r8 == g_r8) return 7;
      if (u.r32 == g_r32) return 8;
      if (u.w32 == g_w32) return 9;
      return p == NULL ? 10 : 11;
   }
   if ((u8 *)p >= block_a && (u8 *)p < block_a + BLOCK_A)
      return 0x100000u | (u32)((u8 *)p - block_a);
   if ((u8 *)p >= block_b && (u8 *)p < block_b + BLOCK_B + 0x1000000)
      return 0x200000u | (u32)((u8 *)p - block_b);
   return 0xffffffffu;
}

static u32 any_address(void)
{
   /* (the tops that are mapped, and some that are not) */
   static const u8 tops[] = { 0x00, 0x01, 0x02, 0x03, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
      0x20, 0x8c, 0xac, 0xff, 0x40, 0x41, 0x42 };
   u32 a = (u32)tops[rnd() % sizeof(tops)] << 24;
   a |= (rnd() & 1) ? (rnd() & 0xffff) : ((rnd() << 8) & 0xffffff);
   return a;
}

static void table(void)
{
   _vmem_handler h1, h2;
   int i;

   _vmem_init();
   h1 = _vmem_register_handler(h_r8, h_r16, h_r32, h_w8, h_w16, h_w32);
   /* (some functions not given: the default's) */
   h2 = _vmem_register_handler(g_r8, 0, g_r32, 0, 0, g_w32);
   mix(h1);
   mix(h2);
   _vmem_map_handler(h1, 0x00, 0x03);
   _vmem_map_handler(h2, 0x10, 0x1f);
   _vmem_map_block(block_a, 0x0c, 0x0f, BLOCK_A - 1);
   _vmem_map_block(block_b, 0x40, 0x43, BLOCK_B - 1);
   _vmem_mirror_mapping(0x8c, 0x0c, 4);
   _vmem_mirror_mapping(0xac, 0x00, 4);

   for (i = 0; i < 200000; i++)
   {
      const u32 a = any_address();
      const u32 op = rnd() % 14;
      const u32 v = rnd() | (rnd() << 24);
      mix(op);
      switch (op)
      {
      case 0: mix(_vmem_ReadMem8(a)); break;
      case 1: mix(_vmem_ReadMem16(a & ~1u)); break;
      case 2: mix(_vmem_ReadMem32(a & ~3u)); break;
      case 3: { u64 r = _vmem_ReadMem64(a & ~7u); mix((u32)r); mix((u32)(r >> 32)); break; }
      case 4: mix(_vmem_ReadMem8SX32(a)); break;
      case 5: mix(_vmem_ReadMem16SX32(a & ~1u)); break;
      case 6: _vmem_WriteMem8(a, (u8)v); break;
      case 7: _vmem_WriteMem16(a & ~1u, (u16)v); break;
      case 8: _vmem_WriteMem32(a & ~3u, v); break;
      case 9: _vmem_WriteMem64(a & ~7u, ((u64)v << 32) | rnd()); break;
      case 10: case 11:
      {
         bool ismem = false;
         const u32 sz = 1u << (rnd() % 3);
         void *p = op == 10 ? _vmem_read_ptr(a, &ismem, sz) : _vmem_write_ptr(a, &ismem, sz);
         mix(ismem);
         mix(where(p, ismem));
         break;
      }
      default:
         break;
      }
   }
   printf("table: %08x a %08x b %08x\n", (unsigned)hash, (unsigned)hash_bytes(block_a, BLOCK_A),
         (unsigned)hash_bytes(block_b, BLOCK_B));
}

/* The memories put in the address space, each layout */
static void reserve(VMemType l, bool naomi2)
{
   size_t i;
   layout = l;
   settings.System = naomi2 ? DC_PLATFORM_NAOMI2 : DC_PLATFORM_DREAMCAST;
   settings.dynarec.disable_nvmem = 0;
   mix(_vmem_reserve());
   mix(_nvmem_enabled());
   mix(_nvmem_4gb_space());
   if (_nvmem_enabled())
   {
      mix((u32)(mem_b.data - virt_ram_base));
      mix((u32)(vram.data - virt_ram_base));
      mix((u32)(aica_ram.data - virt_ram_base));
      mix(_vmem_elan_ram ? (u32)(_vmem_elan_ram - virt_ram_base) : 0);
   }
   else
   {
      /* (each with a page more, zeroed) */
      for (i = 0; i < PAGE_SIZE; i++)
         mix(mem_b.data[RAM_SIZE + i] | vram.data[VRAM_SIZE + i] | aica_ram.data[ARAM_SIZE + i]);
   }
   mix(mem_b.size);
   mix(vram.size);
   mix(aica_ram.size);
   printf("reserve %d%s: %08x\n", (int)l, naomi2 ? " naomi2" : "", (unsigned)hash);
}

/* Video memory's pages protected, watched and opened, and where an address
 * of it is */
static void vram_pages(void)
{
   int i;
   base_seen = _nvmem_enabled() ? virt_ram_base : NULL;
   for (i = 0; i < 4000; i++)
   {
      const u32 addr = (rnd() % (VRAM_SIZE / PAGE_SIZE)) * PAGE_SIZE + (rnd() & 0xff);
      const u32 size = 1 + rnd() % (PAGE_SIZE * 6);
      const u32 op = rnd() % 7;
      mix(op);
      switch (op)
      {
      case 0: _vmem_protect_vram(addr, size); break;
      case 1: _vmem_unprotect_vram(addr, size); break;
      case 2: _vmem_watch_vram(addr, size); break;
      case 3: _vmem_unwatch_vram(addr, size); break;
      case 4: _vmem_open_watched_vram(addr, size); break;
      case 5:
         mix(_vmem_vram_page_protected(addr / PAGE_SIZE));
         mix(_vmem_vram_page_watched(addr / PAGE_SIZE));
         break;
      default:
      {
         u8 *p = _nvmem_enabled() ? virt_ram_base + (rnd() & 0xffffffffu) : vram.data + (rnd() % (VRAM_SIZE * 2));
         mix(_vmem_get_vram_offset(p));
         break;
      }
      }
   }
   /* (all of it open again, as a load leaves it) */
   _vmem_unwatch_vram(0, VRAM_SIZE);
   _vmem_unprotect_vram(0, VRAM_SIZE);
   printf("vram %s: %08x\n", _nvmem_enabled() ? (_nvmem_4gb_space() ? "4gb" : "512mb") : "heap", (unsigned)hash);
}

int main(void)
{
   block_a = (u8 *)calloc(1, BLOCK_A + 256);
   block_b = (u8 *)calloc(1, BLOCK_B + 256);
   /* (aligned to 256, as a block must be) */
   block_a = (u8 *)(((uintptr_t)block_a + 255) & ~(uintptr_t)255);
   block_b = (u8 *)(((uintptr_t)block_b + 255) & ~(uintptr_t)255);
   memset(block_a, 0x5a, BLOCK_A);
   memset(block_b, 0xa5, BLOCK_B);
   table();

   /* The address space: reserved, never touched but where the memories are */
   space_size = (size_t)1 << 32;
   space = (u8 *)mmap(NULL, space_size + (64u << 20), PROT_READ | PROT_WRITE,
         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
   if (space == MAP_FAILED)
      return 1;

   reserve(MemTypeError, false);
   vram_pages();
   _vmem_release();
   reserve(MemType512MB, false);
   vram_pages();
   _vmem_release();
   reserve(MemType512MB, true);
   vram_pages();
   _vmem_release();
   reserve(MemType4GB, false);
   vram_pages();
   _vmem_release();
   reserve(MemType4GB, true);
   vram_pages();
   _vmem_release();
   munmap(space, space_size + (64u << 20));
   return 0;
}
