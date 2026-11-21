/* core/rend/dx9/dx9_dxbc.inc: the Direct3D 9 renderer's shaders
 * (dx9_shaders.c's ShaderDescs), compiled with the d3dcompiler_47.dll
 * given, by the hash of what each is compiled from. See kernels_dx9.sh.
 * Built with mingw-w64 and run under wine. C89.
 *
 *   dx9_dxbc_make.exe d3dcompiler_47.dll OUT */
#define DX9_DXBC_MAKE
#include "../../core/rend/dx9/dx9_shaders.c"
#include <stdarg.h>
#include <stdio.h>

static void RETRO_CALLCONV test_log(enum retro_log_level level, const char *fmt, ...)
{
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}
retro_log_printf_t log_cb = test_log;

int main(int argc, char **argv)
{
	static dx9_code code[DX9_SHADERS];
	HMODULE m;
	pD3DCompile compile;
	FILE *out;
	size_t total = 0, at = 0, k;
	int i;
	if (argc != 3)
		return 2;
	m = LoadLibraryA(argv[1]);
	compile = m ? (pD3DCompile)GetProcAddress(m, "D3DCompile") : NULL;
	if (compile == NULL)
	{
		fprintf(stderr, "dx9_dxbc_make: no D3DCompile in %s\n", argv[1]);
		return 1;
	}
	for (i = 0; i < DX9_SHADERS; i++)
		if (!compileOne(compile, &ShaderDescs[i], &code[i]))
			return 1;
	out = fopen(argv[2], "wb");
	if (out == NULL)
		return 1;
	fprintf(out, "/* The bytecode of the Direct3D 9 renderer's shaders (dx9_shaders.c's\n"
			" * ShaderDescs), by the hash of what each is compiled from: made by\n"
			" * tools/rend/kernels_dx9.sh. Not to be\n"
			" * edited. */\n");
	/* (in whole words, as the device takes them) */
	for (i = 0; i < DX9_SHADERS; i++)
		total += (code[i].size + 3) / 4 * 4;
	fprintf(out, "static const u8 dx9_dxbc_bytes[%u] = {\n", (unsigned)total);
	for (i = 0; i < DX9_SHADERS; i++)
	{
		const u8 *b = (const u8 *)code[i].words;
		const size_t len = (code[i].size + 3) / 4 * 4;
		memset((u8 *)code[i].words + code[i].size, 0, len - code[i].size);
		for (k = 0; k < len; k++)
			fprintf(out, "0x%02x,%s", b[k], (k % 16 == 15 || k + 1 == len) ? "\n" : "");
	}
	fprintf(out, "};\nstatic const struct { u64 hash; u32 offset; u32 size; } dx9_dxbc_index[%d] = {\n", DX9_SHADERS);
	for (i = 0; i < DX9_SHADERS; i++)
	{
		const size_t len = (code[i].size + 3) / 4 * 4;
		fprintf(out, "{ 0x%016llxull, %u, %u },\n", (unsigned long long)descHash(&ShaderDescs[i]), (unsigned)at,
				(unsigned)len);
		at += len;
	}
	fprintf(out, "};\nstatic const size_t dx9_dxbc_count = %d;\n", DX9_SHADERS);
	fclose(out);
	printf("dx9_dxbc_make: %d shaders\n", DX9_SHADERS);
	return 0;
}
