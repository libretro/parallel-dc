/* The host's side of the SH4's address space (core/libretro/vmem_utils.c),
 * by itself: the address space reserved, the memory file mapped into it
 * with its mirrors, what is written through one mirror read through the
 * others and through the next layout's mapping of it, a page locked and
 * unlocked, and the recompilers' code memory, both the kind that is
 * written where it runs and the kind written through a second mapping of
 * it. Each check is a line, and every line is "ok". Built for Windows with
 * mingw-w64 it runs under Wine as well. */
#include <stdio.h>
#include <string.h>

#include "types.h"
#include "hw/mem/_vmem.h"
#include "hw/sh4/sh4_if.h"

retro_log_printf_t log_cb;
settings_t settings;
u8 *virt_ram_base;
Sh4RCB *p_sh4rcb;
unsigned RAM_SIZE = 16 * 1024 * 1024, VRAM_SIZE = 8 * 1024 * 1024, ARAM_SIZE = 2 * 1024 * 1024;
void os_DebugBreak(void) { }

/* (where the memory file goes when there is no shared memory) */
size_t writable_data_path(char *out, size_t size, const char *filename)
{
   size_t n = (size_t)snprintf(out, size, "/tmp%s", filename);
   return n;
}

static int failures;

static void check(int ok, const char *what)
{
   printf("%s %s\n", ok ? "ok  " : "FAIL", what);
   if (!ok)
      failures++;
}

/* A code buffer, pages of it, in the program's own data as the
 * recompilers' are */
static u8 code[1 << 20];

int main(void)
{
   void *base = NULL, *rcb = NULL;
   VMemType type = vmem_platform_init(&base, &rcb);
   vmem_mapping maps[3];
   u8 *ram;
   void *rwx = NULL, *rw = NULL;
   uintptr_t rx_offset = 0;
   u8 *page;

   check(type != MemTypeError, "the address space reserved");
   if (type == MemTypeError)
      return 1;
   virt_ram_base = (u8 *)base;
   p_sh4rcb = (Sh4RCB *)rcb;
   check(((uintptr_t)rcb & 0xffff) == 0 && (u8 *)base == (u8 *)rcb + sizeof(Sh4RCB),
         "the context before the address space, at 64 KB");

   /* main RAM and its three mirrors, video memory, and nothing between */
   maps[0].start_address = 0x0C000000; maps[0].end_address = 0x10000000;
   maps[0].memoffset = 0; maps[0].memsize = RAM_SIZE; maps[0].allow_writes = true;
   maps[1].start_address = 0x04000000; maps[1].end_address = 0x05000000;
   maps[1].memoffset = RAM_SIZE; maps[1].memsize = VRAM_SIZE; maps[1].allow_writes = true;
   maps[2].start_address = 0x05000000; maps[2].end_address = 0x06000000;
   maps[2].memoffset = 0; maps[2].memsize = 0; maps[2].allow_writes = false;
   vmem_platform_create_mappings(maps, 3);

   ram = virt_ram_base + 0x0C000000;
   memset(ram, 0x11, 4096);
   ram[RAM_SIZE - 1] = 0x22;
   check(ram[RAM_SIZE] == 0x11 && ram[3 * RAM_SIZE + 4095] == 0x11
         && ram[2 * RAM_SIZE - 1] == 0x22 && ram[4 * RAM_SIZE - 1] == 0x22,
         "main RAM the same through its mirrors");
   ram[3 * RAM_SIZE + 8] = 0x33;
   check(ram[8] == 0x33, "a mirror written, main RAM changed");
   virt_ram_base[0x04000000 + VRAM_SIZE + 5] = 0x44;
   check(virt_ram_base[0x04000000 + 5] == 0x44, "video memory wrapped at its size");

   /* a page locked and unlocked again (a write between would fault) */
   page = virt_ram_base + 0x04000000 + 3 * PAGE_SIZE;
   check(mem_region_lock(page, PAGE_SIZE) && page[0] == 0, "a page of video memory locked, readable");
   check(mem_region_unlock(page, PAGE_SIZE), "and unlocked");
   page[0] = 0x55;
   check(virt_ram_base[0x04000000 + VRAM_SIZE + 3 * PAGE_SIZE] == 0x55, "and writable");
   check(mem_region_noaccess(page, PAGE_SIZE) && mem_region_unlock(page, PAGE_SIZE), "no access, and open again");

   /* the context's page that is not the table of blocks */
   p_sh4rcb->cntx.pc = 0x8c010000;
   check(p_sh4rcb->cntx.pc == 0x8c010000, "the context writable");

   /* the second layout's mappings in place of the first */
   maps[0].start_address = 0x0C000000; maps[0].end_address = 0x0E000000;
   vmem_platform_create_mappings(maps, 1);
   check(ram[8] == 0x33 && ram[RAM_SIZE + 8] == 0x33, "mapped again, the memory kept");

   /* code memory where it is */
   {
      u8 *area = (u8 *)(((uintptr_t)code + PAGE_SIZE - 1) & ~(uintptr_t)PAGE_MASK);
      const unsigned size = (unsigned)(sizeof(code) / 2) & ~(unsigned)PAGE_MASK;
      check(vmem_platform_prepare_jit_block(area, size, &rwx) && rwx != NULL, "code memory, writable and executable");
      ((u8 *)rwx)[0] = 0xc3;
      check(((u8 *)rwx)[0] == 0xc3, "code memory written");
   }
   /* and written through a second mapping */
   {
      u8 *area = (u8 *)(((uintptr_t)code + sizeof(code) / 2 + PAGE_SIZE - 1) & ~(uintptr_t)PAGE_MASK);
      const unsigned size = 4 * PAGE_SIZE;
      if (vmem_platform_prepare_jit_block_rw(area, size, &rw, &rx_offset))
      {
         ((u8 *)rw)[10] = 0x90;
         /* (where it runs is near @area: at it on POSIX, wherever there is room on Windows) */
         check(((u8 *)rw + rx_offset)[10] == 0x90,
               "code memory written through its second mapping, read where it runs");
      }
      else
         check(0, "code memory with a second mapping");
   }

   vmem_platform_destroy();
   check(1, "given back");
   if (failures)
   {
      printf("vmem_platform: %d failed\n", failures);
      return 1;
   }
   printf("vmem_platform: all passed\n");
   return 0;
}
