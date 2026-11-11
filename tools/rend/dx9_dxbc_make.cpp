/* core/rend/dx9/dx9_dxbc.inc: the Direct3D 9 renderer's shaders
 * (dx9_shaders.cpp's ShaderDescs), compiled with the d3dcompiler_47.dll
 * given, by the hash of what each is compiled from. See kernels_dx9.sh.
 * Built with mingw-w64 and run under wine.
 *
 *   dx9_dxbc_make.exe d3dcompiler_47.dll OUT */
#define DX9_DXBC_MAKE
#include "../../core/rend/dx9/dx9_shaders.cpp"
#include <cstdarg>

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

bool DX9Shaders::Compile(pD3DCompile) { return false; }

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	HMODULE m = LoadLibraryA(argv[1]);
	pD3DCompile compile = m ? (pD3DCompile)(void *)GetProcAddress(m, "D3DCompile") : nullptr;
	if (compile == nullptr)
	{
		fprintf(stderr, "dx9_dxbc_make: no D3DCompile in %s\n", argv[1]);
		return 1;
	}
	const int n = DX9VS_Count + DX9PS_Count;
	std::vector<DWORD> code[DX9VS_Count + DX9PS_Count];
	for (int i = 0; i < n; i++)
		if (!compileOne(compile, ShaderDescs[i], code[i]))
			return 1;
	FILE *out = fopen(argv[2], "wb");
	if (out == nullptr)
		return 1;
	fprintf(out, "/* The bytecode of the Direct3D 9 renderer's shaders (dx9_shaders.cpp's\n"
			" * ShaderDescs), by the hash of what each is compiled from: made by\n"
			" * tools/rend/kernels_dx9.sh. Not to be\n"
			" * edited. */\n");
	size_t total = 0;
	for (int i = 0; i < n; i++)
		total += code[i].size() * 4;
	fprintf(out, "static const u8 dx9_dxbc_bytes[%u] = {\n", (unsigned)total);
	for (int i = 0; i < n; i++)
	{
		const u8 *b = (const u8 *)code[i].data();
		const size_t len = code[i].size() * 4;
		for (size_t k = 0; k < len; k++)
			fprintf(out, "0x%02x,%s", b[k], (k % 16 == 15 || k + 1 == len) ? "\n" : "");
	}
	fprintf(out, "};\nstatic const struct { u64 hash; u32 offset; u32 size; } dx9_dxbc_index[%d] = {\n", n);
	size_t at = 0;
	for (int i = 0; i < n; i++)
	{
		fprintf(out, "{ 0x%016llxull, %u, %u },\n", (unsigned long long)descHash(ShaderDescs[i]), (unsigned)at,
				(unsigned)(code[i].size() * 4));
		at += code[i].size() * 4;
	}
	fprintf(out, "};\nstatic const size_t dx9_dxbc_count = %d;\n", n);
	fclose(out);
	printf("dx9_dxbc_make: %d shaders\n", n);
	return 0;
}
