/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
/* The Direct3D 11 renderers' shaders made ahead and kept on disk
 * (rend/dx11/dx11_shaders.c, rend/dx11/oit/dx11_oitshaders.c), on a
 * device, with d3dcompiler_47.dll: see dx11_shaders_ahead_test.sh.
 *
 * As the renderer has them: init, then start - everything but the
 * polygons' own pixel shaders - and then the shaders a game wants, a
 * polygon's own pixel shader drawn with the general one until the thread
 * has made it. A first run, with no file: the polygons' own made by the
 * thread, nothing on the draw path, and a file written. A second: nothing
 * compiled - what the start makes from the file, the pixel shaders the
 * file has all made ahead - and each one's bytecode what compiling it
 * makes. A file that is not one (a byte changed) is not used, and is
 * written again right; a file whose shaders were compiled from another
 * source has them compiled again. A device for one thread only gets no
 * thread, and still the file. Each for both renderers. */
/* (COBJMACROS before Direct3D's headers) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rend/dx11/dx11_shaders.h"
#include "rend/dx11/oit/dx11_oitshaders.h"

static int failures, skipped;
#define CHECK(c) do { if (!(c)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } } while (0)

#include "d3d_core_stubs.h"

/* ---- the test ---- */

static pD3DCompile compileFn;

typedef struct bytes
{
	u8 *data;
	size_t size;
} bytes;

static bytes readFile(const char *path)
{
	bytes v;
	void *data = NULL;
	int64_t size = 0;
	v.data = NULL;
	v.size = 0;
	if (filestream_read_file(path, &data, &size))
	{
		v.data = (u8 *)data;
		v.size = (size_t)size;
	}
	return v;
}

static void writeFile(const char *path, const bytes *v)
{
	FILE *f = fopen(path, "wb");
	if (f != NULL)
	{
		fwrite(v->data, 1, v->size, f);
		fclose(f);
	}
}

static bool sameBytes(const bytes *a, const bytes *b)
{
	return a->size == b->size && (a->size == 0 || memcmp(a->data, b->data, a->size) == 0);
}

/* Every entry's bytecode is what compiling what it says makes */
static void checkFile(dx11_shader_compiler *probe, const char *path, size_t expect)
{
	bytes f = readFile(path);
	d3d_shcache_entry_t *e = NULL;
	size_t count = 0, at = 0, i;
	CHECK(d3d_shcache_parse(f.data, f.size, &e, &count, &at));
	CHECK(count >= expect);
	for (i = 0; i < count; i++)
	{
		dx11_shader_desc d;
		ID3DBlob *b;
		CHECK(probe->families->describe(e[i].family, e[i].variant, &d));
		CHECK(dx11_shader_hash(&d) == e[i].hash);
		b = dx11_compiler_compile(probe, d.source, "main", d.profile, d.macros, NULL);
		CHECK(b != NULL && ID3D10Blob_GetBufferSize(b) == e[i].size
				&& memcmp(ID3D10Blob_GetBufferPointer(b), &f.data[at + e[i].offset], e[i].size) == 0);
		if (b != NULL)
			ID3D10Blob_Release(b);
	}
	free(e);
	free(f.data);
}

static ID3D11Device *newDevice(UINT flags)
{
	ID3D11Device *device = NULL;
	D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
	if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, &level, 1,
			D3D11_SDK_VERSION, &device, NULL, NULL)))
		return NULL;
	return device;
}

/* What a game would want of the per-triangle renderer's */
static size_t useTriangle(size_t *own)
{
	size_t made = 0;
	u32 v;
	made += dx11_shaders_vs(false) != NULL;
	made += dx11_shaders_vs(true) != NULL;
	made += dx11_shaders_modvol_vs() != NULL;
	made += dx11_shaders_quad_vs() != NULL;
	made += dx11_shaders_modvol_ps() != NULL;
	made += dx11_shaders_quad_ps() != NULL;
	for (v = 0; v < 6; v++)
	{
		DX11FragmentShaderParams p;
		bool general;
		memset(&p, 0, sizeof(p));
		p.texture = (v & 1) != 0;
		p.useAlpha = (v & 2) != 0;
		p.shaderInstr = (int)(v % 4);
		p.fog = (int)(v % 3);
		p.gouraud = v >= 3;
		p.shadowed = v == 5;
		made += dx11_shaders_ps(&p, &general) != NULL;
		if (own != NULL && !general)
			(*own)++;
	}
	CHECK(dx11_shaders_vs_blob() != NULL && dx11_shaders_modvol_vs_blob() != NULL && dx11_shaders_quad_vs_blob() != NULL);
	return made;
}

static size_t useOIT(size_t *own)
{
	size_t made = 0;
	u32 v;
	int m;
	made += dx11_oit_shaders_vs(false) != NULL;
	made += dx11_oit_shaders_vs(true) != NULL;
	made += dx11_oit_shaders_modvol_vs() != NULL;
	made += dx11_oit_shaders_final_vs() != NULL;
	made += dx11_oit_shaders_modvol_ps() != NULL;
	made += dx11_oit_shaders_final_ps() != NULL;
	for (m = 0; m < 4; m++)
		made += dx11_oit_shaders_tr_modvol_ps((dx11_oit_modvol_mode)m) != NULL;
	for (v = 0; v < 6; v++)
	{
		dx11_oit_ps_params p;
		bool general;
		memset(&p, 0, sizeof(p));
		p.texture = (v & 1) != 0;
		p.twoVolume = (v & 2) != 0;
		p.fog = (int)(v % 3);
		p.pass = (dx11_oit_pass)(v % 3);
		made += dx11_oit_shaders_ps(&p, &general) != NULL;
		if (own != NULL && !general)
			(*own)++;
	}
	return made;
}

/* A renderer's shaders: @total used, @fixed pixel ones of a kind, @pixel of them all */
typedef struct renderer_shaders
{
	const char *name;
	void (*init)(ID3D11Device *device, pD3DCompile compile);
	bool (*start)(void);
	void (*term)(void);
	size_t (*use)(size_t *own);
	dx11_shader_compiler *(*compiler)(void);
	const dx11_shader_families *(*families)(void);
	size_t total, fixed, pixel;
} renderer_shaders;

static const renderer_shaders triangle = {
	"per triangle", dx11_shaders_init, dx11_shaders_start, dx11_shaders_term, useTriangle,
	dx11_shaders_compiler, dx11_shaders_families, 12, 2, 8	/* (VS 2, mod. vol. 2, quad 2, PS 6) */
};
static const renderer_shaders perPixel = {
	"per pixel", dx11_oit_shaders_init, dx11_oit_shaders_start, dx11_oit_shaders_term, useOIT,
	dx11_oit_shaders_compiler, dx11_oit_shaders_families, 16, 6, 12	/* (VS 2, mod. vol. 2, final 2, tr. mod. vol. 4, PS 6) */
};

/* The checksum made right again for what was changed */
static void resign(bytes *f)
{
	const u64 h = d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, f->data + 32, f->size - 32);
	int i;
	for (i = 0; i < 8; i++)
		f->data[24 + i] = (u8)(h >> (8 * i));
}

/* (the thread drained and what it made taken, as a frame would) */
static void drained(dx11_shader_compiler *c)
{
	d3d_warm_drain_wait(c->warm);
	dx11_compiler_take_built(c);
}

static void run(const renderer_shaders *r)
{
	dx11_shader_compiler probe;
	const dx11_shader_families *fam = r->families();
	dx11_shader_compiler *c = r->compiler();
	char file[1200];
	size_t kept = 0, own, i;
	bytes good;
	ID3D11Device *device;
	bool all = true;

	printf("%s\n", r->name);
	snprintf(file, sizeof(file), "%s%s", dataDir, fam->file_name);
	memset(&probe, 0, sizeof(probe));
	probe.families = fam;
	probe.compile = compileFn;
	/* (wine's compiler does not compile the per-pixel renderer's) */
	for (i = 0; i < fam->fixed_count && all; i++)
	{
		dx11_shader_desc d;
		ID3DBlob *b = NULL;
		all = fam->describe(fam->fixed[i] >> 16, fam->fixed[i] & 0xFFFF, &d)
				&& (b = dx11_compiler_compile(&probe, d.source, "main", d.profile, d.macros, NULL)) != NULL;
		if (b != NULL)
			ID3D10Blob_Release(b);
	}
	if (!all)
	{
		printf("  (this compiler does not compile its shaders: not tested)\n");
		skipped++;
		return;
	}
	remove(file);
	device = newDevice(0);
	CHECK(device != NULL);
	if (device == NULL)
		return;
	probe.device = device;

	/* no file: all the core does not have built in compiled - the
	 * polygons' own on the thread - none on the draw path; the file keeps
	 * those it compiled */
	r->init(device, compileFn);
	CHECK(c->warm != NULL);
	d3d_warm_drain_wait(c->warm);
	CHECK(r->start());
	CHECK(r->use(NULL) == r->total);
	drained(c);
	own = 0;
	CHECK(r->use(&own) == r->total && own == r->pixel - r->fixed);
	CHECK(c->stat_ahead >= r->fixed + own && c->stat_compiled_ahead > 0 && c->stat_compiled_ahead <= c->stat_ahead);
	CHECK(c->stat_from_file == 0 && c->stat_draw_path == 0);
	kept = c->stat_compiled + c->stat_compiled_ahead;
	r->term();
	checkFile(&probe, file, kept);
	good = readFile(file);

	/* the file: nothing compiled, every pixel shader ahead, the file as it was */
	{
		bytes now;
		r->init(device, compileFn);
		d3d_warm_drain_wait(c->warm);
		CHECK(r->start());
		own = 0;
		CHECK(r->use(&own) == r->total && own == r->pixel - r->fixed);
		CHECK(c->stat_compiled == 0 && c->stat_compiled_ahead == 0 && c->stat_draw_path == 0);
		CHECK(c->stat_ahead >= r->pixel);	/* (those compiled before, from the file) */
		r->term();
		now = readFile(file);
		CHECK(sameBytes(&now, &good));
		free(now.data);
	}

	/* wanted before the thread is done: still nothing compiled */
	{
		bytes now;
		r->init(device, compileFn);
		CHECK(r->start());
		CHECK(r->use(NULL) == r->total);
		CHECK(c->stat_compiled == 0 && c->stat_draw_path == 0);
		r->term();
		now = readFile(file);
		CHECK(sameBytes(&now, &good));
		free(now.data);
	}

	/* not a file (a byte changed): not used, and written again */
	if (good.size > 7)
	{
		bytes bad;
		bad.size = good.size;
		bad.data = (u8 *)malloc(good.size);
		memcpy(bad.data, good.data, good.size);
		bad.data[bad.size - 7] ^= 0x40;
		writeFile(file, &bad);
		free(bad.data);
		r->init(device, compileFn);
		d3d_warm_drain_wait(c->warm);
		CHECK(r->start());
		CHECK(r->use(NULL) == r->total);
		drained(c);
		CHECK(c->stat_from_file == 0 && c->stat_draw_path == 0);
		r->term();
	}
	checkFile(&probe, file, kept);
	free(good.data);

	/* one compiled from another source: compiled again (ahead), and replaced */
	{
		bytes stale = readFile(file);
		d3d_shcache_entry_t *e = NULL;
		size_t count = 0, at = 0, which = 0;
		const bool parsed = d3d_shcache_parse(stale.data, stale.size, &e, &count, &at);
		CHECK(parsed);
		while (which < count && !fam->pixel_family(e[which].family))
			which++;
		CHECK(which < count);
		free(e);
		if (!parsed || which == count)
		{
			free(stale.data);
			probe.device = NULL;
			ID3D11Device_Release(device);
			return;
		}
		stale.data[32 + which * 24] ^= 1;		/* (its hash) */
		resign(&stale);
		writeFile(file, &stale);
		free(stale.data);
		r->init(device, compileFn);
		d3d_warm_drain_wait(c->warm);
		CHECK(r->start());
		CHECK(r->use(NULL) == r->total);
		drained(c);
		CHECK(c->stat_compiled == 0 && c->stat_compiled_ahead == 1 && c->stat_draw_path == 0);
		r->term();
	}
	checkFile(&probe, file, kept);

	/* a device for one thread only: no thread, the file still (a device
	 * that does not say it is one - wine's - is not known to be) */
	{
		ID3D11Device *one = newDevice(D3D11_CREATE_DEVICE_SINGLETHREADED);
		CHECK(one != NULL);
		if (one != NULL && !(ID3D11Device_GetCreationFlags(one) & D3D11_CREATE_DEVICE_SINGLETHREADED))
		{
			printf("  (the device does not say it is for one thread: not tested)\n");
			ID3D11Device_Release(one);
			one = NULL;
		}
		if (one != NULL)
		{
			r->init(one, compileFn);
			CHECK(c->warm == NULL);
			CHECK(r->start());
			CHECK(r->use(NULL) == r->total);
			CHECK(c->stat_compiled == 0 && c->stat_ahead == 0 && c->stat_from_file > 0);
			r->term();
			ID3D11Device_Release(one);
		}
	}
	probe.device = NULL;
	ID3D11Device_Release(device);
}

int main(int argc, char **argv)
{
	HMODULE dll;
	ID3D11Device *device;
	if (argc < 2)
	{
		printf("usage: dx11_shaders_ahead_test DIR\\ [COMPILER.DLL]\n");
		return 2;
	}
	snprintf(dataDir, sizeof(dataDir), "%s", argv[1]);
	/* (another compiler than d3dcompiler_47.dll: one with D3DCompile) */
	dll = LoadLibraryA(argc > 2 ? argv[2] : "d3dcompiler_47.dll");
	compileFn = dll != NULL ? (pD3DCompile)GetProcAddress(dll, "D3DCompile") : NULL;
	if (compileFn == NULL)
	{
		printf("dx11_shaders_ahead_test: no d3dcompiler_47.dll\n");
		return 2;
	}
	device = newDevice(0);
	if (device == NULL)
	{
		printf("dx11_shaders_ahead_test: no Direct3D 11 device of feature level 11.0\n");
		return 2;
	}
	ID3D11Device_Release(device);
	run(&triangle);
	run(&perPixel);
	if (failures)
	{
		printf("dx11_shaders_ahead_test: %d failed\n", failures);
		return 1;
	}
	printf("dx11_shaders_ahead_test: PASS%s\n", skipped ? " (not all tested)" : "");
	return 0;
}
