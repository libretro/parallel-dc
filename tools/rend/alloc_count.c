/* What the core allocates while a test host runs it: see alloc_count.h. */
#define _GNU_SOURCE 1 /* dladdr, dl_iterate_phdr */
#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "alloc_count.h"

static volatile int vkh_alloc_on;
static uintptr_t vkh_core_lo, vkh_core_hi, vkh_core_base;
#define VKH_RUNTIMES 8
static uintptr_t vkh_rt_lo[VKH_RUNTIMES], vkh_rt_hi[VKH_RUNTIMES];
static int vkh_nrt;
#define VKH_A_KINDS 16
static int vkh_kinds = ALLOC_HOST_KINDS;
static const char *const vkh_heap_names[ALLOC_HOST_KINDS] = { "heap allocations", "heap frees" };
static const char *const vkh_heap_site[ALLOC_HOST_KINDS] = { "heap", NULL };
static const char *const *vkh_a_names = vkh_heap_names;
static const char *const *vkh_a_site = vkh_heap_site;
static long vkh_a_count[VKH_A_KINDS];
static long vkh_heap_bytes;
/* a site: up to VKH_CHAIN of the core's frames, innermost first (by their
 * offsets in it), for its name (the first that is not a library's) */
#define VKH_SITES 1024
#define VKH_CHAIN 12
static uintptr_t vkh_site_key[VKH_SITES];
static uintptr_t vkh_site_chain[VKH_SITES][VKH_CHAIN];
static int vkh_site_kind[VKH_SITES];
static long vkh_site_n[VKH_SITES];

static int vkh_in(uintptr_t a, uintptr_t lo, uintptr_t hi) { return a >= lo && a < hi; }

static int vkh_runtime(uintptr_t a)
{
   int i;
   for (i = 0; i < vkh_nrt; i++)
      if (vkh_in(a, vkh_rt_lo[i], vkh_rt_hi[i]))
         return 1;
   return 0;
}

/* the core's executable segments, and the C and C++ runtimes' */
static int vkh_find_ranges(struct dl_phdr_info *info, size_t size, void *core_base)
{
   int i;
   const char *n = info->dlpi_name ? info->dlpi_name : "";
   const int core = info->dlpi_addr == (uintptr_t)core_base;
   const int rt = strstr(n, "libc.so") || strstr(n, "libstdc++") || strstr(n, "libgcc_s") || strstr(n, "libm.so");
   (void)size;
   if (!core && !(rt && vkh_nrt < VKH_RUNTIMES))
      return 0;
   for (i = 0; i < info->dlpi_phnum; i++)
   {
      const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
      uintptr_t lo, hi;
      if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
         continue;
      lo = info->dlpi_addr + ph->p_vaddr;
      hi = lo + ph->p_memsz;
      if (core)
      {
         vkh_core_lo = lo;
         vkh_core_hi = hi;
         vkh_core_base = info->dlpi_addr;
      }
      else
      {
         vkh_rt_lo[vkh_nrt] = lo;
         vkh_rt_hi[vkh_nrt] = hi;
         vkh_nrt++;
      }
   }
   return 0;
}

static __thread int vkh_in_hook;

/* the site of @n core frames @chain (absolute addresses) */
static void vkh_site(const uintptr_t *chain, int n, int kind)
{
   uintptr_t c[VKH_CHAIN] = { 0 }, key = (uintptr_t)kind * 0x9e3779b9u;
   unsigned h, i;
   int j;
   for (j = 0; j < n && j < VKH_CHAIN; j++)
   {
      c[j] = chain[j] - vkh_core_base;
      key = key * 1000003u ^ c[j];
   }
   key |= 1;
   h = (unsigned)((key * 2654435761u) % VKH_SITES);
   for (i = 0; i < VKH_SITES; i++, h = (h + 1) % VKH_SITES)
   {
      if (vkh_site_key[h] == 0 && __sync_bool_compare_and_swap(&vkh_site_key[h], 0, (uintptr_t)-1))
      {
         memcpy(vkh_site_chain[h], c, sizeof(c));
         vkh_site_kind[h] = kind;
         __sync_synchronize();
         vkh_site_key[h] = key;
      }
      /* (a slot being filled in is passed by: a site may then have two,
       * which vk_allocs.sh adds up by its name) */
      if (vkh_site_key[h] == key)
      {
         __sync_fetch_and_add(&vkh_site_n[h], 1);
         return;
      }
   }
}

/* The core's frames from @ret outward (@ret itself the first, when it is
 * the core's; else the first of the core's past the C and C++ runtimes),
 * into @chain: how many, 0 if the call is not the core's */
static int vkh_core_chain(void *ret, uintptr_t *chain)
{
   void *bt[32];
   int depth, i, from = -1, n = 0;
   if (vkh_in_hook)
      return 0;
   vkh_in_hook = 1;
   depth = backtrace(bt, 32);
   vkh_in_hook = 0;
   for (i = 0; i < depth; i++)
      if (bt[i] == ret)
      {
         from = i;
         break;
      }
   if (from < 0)
      return 0;
   for (i = from; i < depth; i++)
   {
      const uintptr_t a = (uintptr_t)bt[i];
      if (n == 0 && vkh_runtime(a))
         continue;
      if (!vkh_in(a, vkh_core_lo, vkh_core_hi))
         break;
      /* (a return address: the call is the byte before it) */
      chain[n++] = a - 1;
      if (n == VKH_CHAIN)
         break;
   }
   return n;
}

void alloc_count_kinds(int kinds, const char *const *names, const char *const *site)
{
   vkh_kinds = kinds < VKH_A_KINDS ? kinds : VKH_A_KINDS;
   vkh_a_names = names;
   vkh_a_site = site;
}

void alloc_count_start(void *core_symbol)
{
   Dl_info info;
   if (dladdr(core_symbol, &info))
      dl_iterate_phdr(vkh_find_ranges, info.dli_fbase);
   {
      /* (its first call loads the unwinder, which allocates) */
      void *bt[2];
      backtrace(bt, 2);
   }
   vkh_alloc_on = 1;
}

void alloc_count_stop(void)
{
   vkh_alloc_on = 0;
}

int alloc_count_on(void)
{
   return vkh_alloc_on;
}

void alloc_count_seen(int kind, void *ret)
{
   uintptr_t chain[VKH_CHAIN];
   int n;
   if (!vkh_alloc_on || kind < 0 || kind >= vkh_kinds || !vkh_in((uintptr_t)ret, vkh_core_lo, vkh_core_hi))
      return;
   __sync_fetch_and_add(&vkh_a_count[kind], 1);
   if (vkh_a_site[kind] == NULL)
      return;
   n = vkh_core_chain(ret, chain);
   if (n)
      vkh_site(chain, n, kind);
}

void alloc_count_print(const char *prefix, const char *what, int sites)
{
   int k, i;
   printf("%s: during play (%s):", prefix, what);
   for (k = 0; k < vkh_kinds; k++)
      printf("%s %ld %s", k ? "," : "", __sync_fetch_and_add(&vkh_a_count[k], 0), vkh_a_names[k]);
   printf("; %ld bytes%s\n", __sync_fetch_and_add(&vkh_heap_bytes, 0),
#ifdef ALLOC_COUNT
         ""
#else
         " (the heap not counted: not built with -DALLOC_COUNT)"
#endif
         );
   if (sites)
      for (i = 0; i < VKH_SITES; i++)
         if (vkh_site_key[i])
         {
            int j;
            printf("%s: site %s %ld", prefix, vkh_a_site[vkh_site_kind[i]], vkh_site_n[i]);
            for (j = 0; j < VKH_CHAIN && vkh_site_chain[i][j]; j++)
               printf(" 0x%lx", (unsigned long)vkh_site_chain[i][j]);
            printf("\n");
         }
}

#ifdef ALLOC_COUNT
/* The heap's, in place of the C library's (this program exports them:
 * -rdynamic), counted where the core asked: from its own code, or through
 * the C and C++ runtimes for it - the first frame outside them is the
 * core's. A driver's own allocations, and this program's, are not. */
extern void *__libc_malloc(size_t n);
extern void *__libc_calloc(size_t n, size_t s);
extern void *__libc_realloc(void *p, size_t n);
extern void __libc_free(void *p);
extern void *__libc_memalign(size_t align, size_t n);
static void vkh_heap_seen(int kind, void *ret, size_t size)
{
   uintptr_t chain[VKH_CHAIN];
   const uintptr_t a = (uintptr_t)ret;
   int n;
   if (!vkh_alloc_on || vkh_in_hook)
      return;
   /* (the cheap test first: neither the core's nor through the runtimes) */
   if (!vkh_in(a, vkh_core_lo, vkh_core_hi) && !vkh_runtime(a))
      return;
   if (!(n = vkh_core_chain(ret, chain)))
      return;
   __sync_fetch_and_add(&vkh_a_count[kind], 1);
   if (kind == ALLOC_HEAP)
   {
      __sync_fetch_and_add(&vkh_heap_bytes, (long)size);
      vkh_site(chain, n, ALLOC_HEAP);
   }
}

void *malloc(size_t n)
{
   void *p = __libc_malloc(n);
   vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), n);
   return p;
}
void *calloc(size_t n, size_t s)
{
   void *p = __libc_calloc(n, s);
   vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), n * s);
   return p;
}
void *realloc(void *old, size_t n)
{
   void *p = __libc_realloc(old, n);
   if (n != 0)
      vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), n);
   return p;
}
void free(void *p)
{
   if (p)
      vkh_heap_seen(ALLOC_FREE, __builtin_return_address(0), 0);
   __libc_free(p);
}
void *memalign(size_t align, size_t n)
{
   void *p = __libc_memalign(align, n);
   vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), n);
   return p;
}
void *aligned_alloc(size_t align, size_t n)
{
   void *p = __libc_memalign(align, n);
   vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), n);
   return p;
}
int posix_memalign(void **out, size_t align, size_t n)
{
   void *p = __libc_memalign(align, n);
   vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), n);
   if (!p)
      return 12; /* ENOMEM */
   *out = p;
   return 0;
}
/* operator new, plain and aligned, throwing and not (the C++ runtime's
 * delete frees with free()) */
#define VKH_NEW(name, args, size, call) \
   void *name args { void *p = call; vkh_heap_seen(ALLOC_HEAP, __builtin_return_address(0), size); return p; }
VKH_NEW(_Znwm, (size_t n), n, __libc_malloc(n))
VKH_NEW(_Znam, (size_t n), n, __libc_malloc(n))
VKH_NEW(_ZnwmRKSt9nothrow_t, (size_t n, const void *t), n, ((void)t, __libc_malloc(n)))
VKH_NEW(_ZnamRKSt9nothrow_t, (size_t n, const void *t), n, ((void)t, __libc_malloc(n)))
VKH_NEW(_ZnwmSt11align_val_t, (size_t n, size_t al), n, __libc_memalign(al, n))
VKH_NEW(_ZnamSt11align_val_t, (size_t n, size_t al), n, __libc_memalign(al, n))
VKH_NEW(_ZnwmSt11align_val_tRKSt9nothrow_t, (size_t n, size_t al, const void *t), n, ((void)t, __libc_memalign(al, n)))
VKH_NEW(_ZnamSt11align_val_tRKSt9nothrow_t, (size_t n, size_t al, const void *t), n, ((void)t, __libc_memalign(al, n)))
#endif
