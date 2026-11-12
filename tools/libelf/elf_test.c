/* The ELF reader (core/deps/libelf), over a small SH-4 executable made
 * here, as the BIOS's loader reads a disc's or a homebrew's: the file is
 * one, its program headers and their segments are where they are, and
 * every way of reading a program header gives the same. The file is put
 * above 4 GiB where the system lets it, where an address cut to a 32-bit
 * long (64-bit Windows) is another. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "deps/libelf/elf.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

#define FILE_SIZE 0x1000

static unsigned char *place_high(void)
{
	static unsigned char low[FILE_SIZE + 16];
	uintptr_t at;
	void *p = NULL;
	if (sizeof(void *) > 4)
		for (at = (uintptr_t)1 << 32; at != 0 && at < ((uintptr_t)1 << 32) * 64 && p == NULL;
				at += (uintptr_t)1 << 32)
		{
#ifdef _WIN32
			p = VirtualAlloc((void *)at, FILE_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
			p = mmap((void *)at, FILE_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			if (p == MAP_FAILED)
				p = NULL;
			else if ((uintptr_t)p >> 32 == 0)
			{
				munmap(p, FILE_SIZE);
				p = NULL;
			}
#endif
		}
	if (p == NULL)
		return (unsigned char *)(((uintptr_t)low + 15) & ~(uintptr_t)15);
	return (unsigned char *)p;
}

static void put16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }

int main(void)
{
	unsigned char *f = place_high();
	struct Elf32_Header *h = (struct Elf32_Header *)f;
	struct Elf32_Phdr *ph;
	uint64_t vaddr, paddr, filesz, offset, memsz;
	uint16_t i;
	const uint32_t phoff = 0x40;
	static const uint32_t type[2] = { PT_LOAD, PT_NOTE };
	static const uint32_t off[2] = { 0x100, 0x800 };
	static const uint32_t va[2] = { 0x8c010000, 0x8c020000 };
	static const uint32_t pa[2] = { 0x0c010000, 0x0c020000 };
	static const uint32_t fs[2] = { 0x200, 0x10 };
	static const uint32_t ms[2] = { 0x300, 0x10 };

	memset(f, 0, FILE_SIZE);
	f[0] = 0x7f; f[1] = 'E'; f[2] = 'L'; f[3] = 'F';
	f[4] = ELFCLASS32;
	f[5] = ELFDATA2LSB;
	f[6] = 1;
	put16(f + 16, 2);		/* executable */
	put16(f + 18, 42);		/* SH */
	put32(f + 20, 1);
	put32(f + 24, 0x8c010000);	/* entry */
	put32(f + 28, phoff);
	put16(f + 40, sizeof(struct Elf32_Header));
	put16(f + 42, sizeof(struct Elf32_Phdr));
	put16(f + 44, 2);
	for (i = 0; i < 2; i++)
	{
		unsigned char *p = f + phoff + i * sizeof(struct Elf32_Phdr);
		put32(p + 0, type[i]);
		put32(p + 4, off[i]);
		put32(p + 8, va[i]);
		put32(p + 12, pa[i]);
		put32(p + 16, fs[i]);
		put32(p + 20, ms[i]);
		put32(p + 24, 7);
		put32(p + 28, 4);
	}

	CHECK(elf_checkFile(f) == 0);
	CHECK(elf_getEntryPoint(f) == 0x8c010000);
	CHECK(elf_getNumProgramHeaders(f) == 2);
	ph = elf32_getProgramSegmentTable(h);
	CHECK((unsigned char *)ph == f + phoff);
	for (i = 0; i < 2; i++)
	{
		CHECK(elf_getProgramHeaderType(f, i) == type[i]);
		CHECK(elf_getProgramHeaderOffset(f, i) == off[i]);
		CHECK(elf_getProgramHeaderVaddr(f, i) == va[i]);
		CHECK(elf_getProgramHeaderPaddr(f, i) == pa[i]);
		CHECK(elf_getProgramHeaderFileSize(f, i) == fs[i]);
		CHECK(elf_getProgramHeaderMemorySize(f, i) == ms[i]);
		CHECK(elf_getProgramHeaderFlags(f, i) == 7);
		CHECK(elf32_getSegmentType(h, i) == (int)type[i]);
		elf32_getSegmentInfo(h, i, &vaddr, &paddr, &filesz, &offset, &memsz);
		CHECK(vaddr == va[i] && paddr == pa[i] && filesz == fs[i]
				&& offset == off[i] && memsz == ms[i]);
	}
	/* Not an ELF file */
	f[1] = 'X';
	CHECK(elf_checkFile(f) != 0);

	if (failures)
	{
		fprintf(stderr, "elf_test: %d failure(s)\n", failures);
		return 1;
	}
	printf("elf_test: OK (the file at %p)\n", (void *)f);
	return 0;
}
