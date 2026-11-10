/* What the core allocates while a test host runs it, and where: the heap's
 * (this file, built with -DALLOC_COUNT -rdynamic, has malloc, free and the
 * operator new variants in place of the C library's) and the host's own
 * kinds - a graphics API's objects - that it reports itself
 * (alloc_count_seen()). A site is a chain of the core's frames, innermost
 * first, by their offsets in it; tools/vulkan/vk_alloc_sites.py names and
 * judges them. Used by tools/vulkan/vk_hdr.c and tools/gl/gl_frames.c. */
#ifndef TOOLS_ALLOC_COUNT_H
#define TOOLS_ALLOC_COUNT_H

/* the kinds every host has */
enum { ALLOC_HEAP, ALLOC_FREE, ALLOC_HOST_KINDS };

/* @names: what each kind is, for the line printed, from ALLOC_HEAP on
 * (the two first are the heap's); @site: for each, the word its sites are
 * printed with, NULL for a kind that has none (what is given back) */
void alloc_count_kinds(int kinds, const char *const *names, const char *const *site);
/* counting on, from now: @core_symbol is one of the core's */
void alloc_count_start(void *core_symbol);
void alloc_count_stop(void);
int alloc_count_on(void);
/* a thing of @kind the core made, asked for from @ret (its return
 * address: the core's own code, or not counted) */
void alloc_count_seen(int kind, void *ret);
/* "<prefix>: during play (<what>): N heap allocations, ...; B bytes", and
 * with @sites every site: "<prefix>: site <word> N 0x... 0x..." */
void alloc_count_print(const char *prefix, const char *what, int sites);

#endif
