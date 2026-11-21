/*
	Copyright 2021 flyinghead

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
/* The per-triangle renderer's shaders: the Vulkan renderer's GLSL
 * (vulkan/shaders.cpp) in HLSL, in the shape upstream's Direct3D 11
 * shaders have. What a pixel comes out as is what the Vulkan and OpenGL
 * renderers make of it, bit for bit where the two languages allow. */
#include "dx11_shaders.h"
#include <stdlib.h>
#include <string.h>
#include <streams/file_stream.h>
#include "dx11_shaders_text.h"
#include "paths.h"

/* The shaders the renderers' start makes, as bytecode (dx11_dxbc_index[]
 * sorted by hash, the bytes in dx11_dxbc_bytes[]) */
#include "dx11_dxbc.inc"

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

/* ---- Compiling ---- */

ID3DBlob *dx11_compiler_compile(dx11_shader_compiler *c, const char *source, const char *entry, const char *profile,
		const D3D_SHADER_MACRO *macros, ID3DInclude *include)
{
	ID3DBlob *blob = NULL;
	ID3DBlob *errors = NULL;
	HRESULT hr;

	if (c->compile == NULL)
		return NULL;
	hr = c->compile(source, strlen(source), NULL, macros, include, entry, profile, 0, 0, &blob, &errors);
	if (FAILED(hr))
	{
		ERROR_LOG(RENDERER, "Shader compilation failed (%x): %s\n", (unsigned)hr,
				errors != NULL ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "");
		if (blob != NULL)
			ID3D10Blob_Release(blob);
		blob = NULL;
	}
	if (errors != NULL)
		ID3D10Blob_Release(errors);
	return blob;
}

ID3D11VertexShader *dx11_compiler_compile_vs(dx11_shader_compiler *c, const char *source, const char *entry,
		const char *profile, const D3D_SHADER_MACRO *macros, ID3DBlob **blob_out, ID3DInclude *include)
{
	ID3D11VertexShader *shader = NULL;
	ID3DBlob *blob = dx11_compiler_compile(c, source, entry, profile, macros, include);
	if (blob != NULL && FAILED(ID3D11Device_CreateVertexShader(c->device, ID3D10Blob_GetBufferPointer(blob),
				ID3D10Blob_GetBufferSize(blob), NULL, &shader)))
	{
		shader = NULL;
		ERROR_LOG(RENDERER, "Vertex shader creation failed\n");
	}
	if (blob_out != NULL)
		*blob_out = blob;
	else if (blob != NULL)
		ID3D10Blob_Release(blob);
	return shader;
}

ID3D11PixelShader *dx11_compiler_compile_ps(dx11_shader_compiler *c, const char *source, const char *entry,
		const char *profile, const D3D_SHADER_MACRO *macros, ID3DInclude *include)
{
	ID3D11PixelShader *shader = NULL;
	ID3DBlob *blob = dx11_compiler_compile(c, source, entry, profile, macros, include);
	if (blob != NULL)
	{
		if (FAILED(ID3D11Device_CreatePixelShader(c->device, ID3D10Blob_GetBufferPointer(blob),
					ID3D10Blob_GetBufferSize(blob), NULL, &shader)))
		{
			shader = NULL;
			ERROR_LOG(RENDERER, "Pixel shader creation failed\n");
		}
		ID3D10Blob_Release(blob);
	}
	return shader;
}

/* ---- The file, the thread ---- */

/* Bytecode from the file or the core, for an input layout: a blob of its
 * own, the bytes after it */
typedef struct file_blob
{
	ID3D10Blob iface;
	LONG refs;
	size_t size;
} file_blob;

static const IID iid_unknown = { 0x00000000, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
static const IID iid_blob = { 0x8ba5fb08, 0x5195, 0x40e2, { 0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02 } };

static ULONG STDMETHODCALLTYPE file_blob_add_ref(ID3D10Blob *self)
{
	return (ULONG)InterlockedIncrement(&((file_blob *)self)->refs);
}

static ULONG STDMETHODCALLTYPE file_blob_release(ID3D10Blob *self)
{
	const ULONG n = (ULONG)InterlockedDecrement(&((file_blob *)self)->refs);
	if (n == 0)
		free(self);
	return n;
}

static HRESULT STDMETHODCALLTYPE file_blob_query(ID3D10Blob *self, REFIID riid, void **out)
{
	if (memcmp(riid, &iid_unknown, sizeof(IID)) == 0 || memcmp(riid, &iid_blob, sizeof(IID)) == 0)
	{
		file_blob_add_ref(self);
		*out = self;
		return S_OK;
	}
	*out = NULL;
	return E_NOINTERFACE;
}

static LPVOID STDMETHODCALLTYPE file_blob_pointer(ID3D10Blob *self)
{
	return (u8 *)self + sizeof(file_blob);
}

static SIZE_T STDMETHODCALLTYPE file_blob_size(ID3D10Blob *self)
{
	return ((file_blob *)self)->size;
}

static ID3D10BlobVtbl file_blob_vtbl = {
	file_blob_query, file_blob_add_ref, file_blob_release, file_blob_pointer, file_blob_size
};

static ID3DBlob *file_blob_new(const void *code, size_t size)
{
	file_blob *b = (file_blob *)malloc(sizeof(file_blob) + size);
	if (b == NULL)
		return NULL;
	b->iface.lpVtbl = &file_blob_vtbl;
	b->refs = 1;
	b->size = size;
	memcpy((u8 *)b + sizeof(file_blob), code, size);
	return &b->iface;
}

typedef struct kept_blob
{
	u32 family;
	u32 variant;
	ID3DBlob *blob;
} kept_blob;

static void kept_unmake(void *e)
{
	kept_blob *k = (kept_blob *)e;
	if (k->blob != NULL)
		ID3D10Blob_Release(k->blob);
	k->blob = NULL;
}

u64 dx11_shader_hash(const dx11_shader_desc *d)
{
	const D3D_SHADER_MACRO *m;
	u64 h = d3d_shcache_hash(D3D_SHCACHE_HASH_INIT, d->source, d->length + 1);
	h = d3d_shcache_hash(h, d->profile, strlen(d->profile) + 1);
	for (m = d->macros; m->Name != NULL; m++)
	{
		h = d3d_shcache_hash(h, m->Name, strlen(m->Name) + 1);
		h = d3d_shcache_hash(h, m->Definition, strlen(m->Definition) + 1);
	}
	return h;
}

/* The core's own bytecode of @hash (dx11_dxbc.inc) */
static const u8 *built_in(u64 hash, u32 *size)
{
	size_t lo = 0, hi = dx11_dxbc_count;
	while (lo < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (dx11_dxbc_index[mid].hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == dx11_dxbc_count || dx11_dxbc_index[lo].hash != hash)
		return NULL;
	*size = dx11_dxbc_index[lo].size;
	return &dx11_dxbc_bytes[dx11_dxbc_index[lo].offset];
}

/* The file's bytecode of @hash (both threads: the file is not changed
 * while the thread runs) */
static const u8 *from_file(const dx11_shader_compiler *c, u64 hash, u32 *size)
{
	size_t lo = 0, hi = c->file_count;
	while (lo < hi)
	{
		const size_t mid = (lo + hi) / 2;
		if (c->file_index[mid].hash < hash)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == c->file_count || c->file_index[lo].hash != hash)
		return NULL;
	*size = c->file_index[lo].size;
	return &c->file[c->file_blobs + c->file_index[lo].offset];
}

/* @blob's bytecode kept, the blob held (nothing copied) */
static void keep(dx11_shader_compiler *c, u64 hash, u32 family, u32 variant, ID3DBlob *blob)
{
	kept_blob *k;
	if (blob == NULL || ID3D10Blob_GetBufferSize(blob) == 0 || c->kept.elem == 0 || tex_map_find(&c->kept, hash) != NULL)
		return;
	k = (kept_blob *)tex_map_add(&c->kept, hash);
	if (k == NULL)
		return;
	k->family = family;
	k->variant = variant;
	ID3D10Blob_AddRef(blob);
	k->blob = blob;
	c->file_changed = true;
}

/* The bytecode of @d: the core's, the file's, this run's, or compiled now
 * (and kept, *hold holding it) */
static bool bytecode(dx11_shader_compiler *c, u32 family, u32 variant, const dx11_shader_desc *d, ID3DBlob **hold,
		const void **code, size_t *size, bool *in_file)
{
	const u64 hash = dx11_shader_hash(d);
	u32 bytes = 0;
	const u8 *p = built_in(hash, &bytes);
	const kept_blob *k;
	*hold = NULL;
	*in_file = false;
	if (p != NULL)
	{
		c->stat_built_in++;
		*code = p;
		*size = bytes;
		return true;
	}
	p = from_file(c, hash, &bytes);
	*in_file = p != NULL;
	if (p != NULL)
	{
		*code = p;
		*size = bytes;
		return true;
	}
	k = c->kept.elem != 0 ? (const kept_blob *)tex_map_find(&c->kept, hash) : NULL;
	if (k != NULL)
	{
		*code = ID3D10Blob_GetBufferPointer(k->blob);
		*size = ID3D10Blob_GetBufferSize(k->blob);
		return true;
	}
	*hold = dx11_compiler_compile(c, d->source, "main", d->profile, d->macros, NULL);
	if (*hold == NULL)
		return false;
	c->stat_compiled++;
	*code = ID3D10Blob_GetBufferPointer(*hold);
	*size = ID3D10Blob_GetBufferSize(*hold);
	keep(c, hash, family, variant, *hold);
	return true;
}

/* The thread's: the pixel shader of @job (kind the family, aux the
 * variant), from the file or compiled; key handed back the hash, extra
 * the bytecode compiled */
static bool dx11_warm_build(void *ctx, const d3d_warm_job_t *job, d3d_warm_result_t *res)
{
	const dx11_shader_compiler *c = (const dx11_shader_compiler *)ctx;
	dx11_shader_desc d;
	u64 hash;
	u32 size = 0;
	const void *code;
	ID3DBlob *blob = NULL;
	ID3D11PixelShader *shader = NULL;
	if (!c->families->describe(job->kind, job->aux, &d))
		return false;
	hash = dx11_shader_hash(&d);
	code = built_in(hash, &size);
	if (code == NULL)
		code = from_file(c, hash, &size);
	if (code == NULL)
	{
		ID3DBlob *errors = NULL;
		if (c->compile == NULL
				|| FAILED(c->compile(d.source, d.length, NULL, d.macros, NULL, "main", d.profile, 0, 0, &blob, &errors)))
		{
			if (errors != NULL)
				ID3D10Blob_Release(errors);
			if (blob != NULL)
				ID3D10Blob_Release(blob);
			return false;
		}
		if (errors != NULL)
			ID3D10Blob_Release(errors);
		code = ID3D10Blob_GetBufferPointer(blob);
		size = (u32)ID3D10Blob_GetBufferSize(blob);
	}
	if (FAILED(ID3D11Device_CreatePixelShader(c->device, code, size, NULL, &shader)))
	{
		if (blob != NULL)
			ID3D10Blob_Release(blob);
		return false;
	}
	res->key = hash;
	res->obj = shader;
	res->extra = blob;
	return true;
}

static void dx11_warm_release(void *ctx, d3d_warm_result_t *res)
{
	(void)ctx;
	if (res->obj != NULL)
		ID3D11PixelShader_Release((ID3D11PixelShader *)res->obj);
	if (res->extra != NULL)
		ID3D10Blob_Release((ID3DBlob *)res->extra);
	res->obj = res->extra = NULL;
}

void dx11_compiler_take_built(dx11_shader_compiler *c)
{
	d3d_warm_result_t r;
	if (c->warm == NULL)
		return;
	while (d3d_warm_take(c->warm, &r))
	{
		if (r.extra != NULL)
		{
			ID3DBlob *blob = (ID3DBlob *)r.extra;
			keep(c, r.key, r.kind, r.aux, blob);
			ID3D10Blob_Release(blob);
			c->stat_compiled_ahead++;
		}
		c->stat_ahead++;
		c->families->adopt(r.kind, r.aux, (ID3D11PixelShader *)r.obj);
	}
}

/* @family's @variant asked of the thread: false only when it has no room
 * (to be asked again); with no thread, true, the general shader drawing it
 * for the run */
static bool request(dx11_shader_compiler *c, u32 family, u32 variant)
{
	d3d_warm_job_t j;
	if (c->warm == NULL)
		return true;
	j.key = 0;
	j.kind = family;
	j.aux = variant;
	return d3d_warm_submit(c->warm, &j, 1) == 1;
}

ID3D11PixelShader *dx11_compiler_missed_ps(dx11_shader_compiler *c, u32 family, u32 variant, ID3D11PixelShader **slot)
{
	dx11_shader_desc d;
	ID3DBlob *hold;
	const void *code;
	size_t size;
	bool in_file;
	dx11_compiler_take_built(c);
	if (*slot != NULL)
		return *slot;
	if (c->started)
		c->stat_draw_path++;
	if (!c->families->describe(family, variant, &d) || !bytecode(c, family, variant, &d, &hold, &code, &size, &in_file))
		return NULL;
	if (in_file)
		c->stat_from_file++;
	if (FAILED(ID3D11Device_CreatePixelShader(c->device, code, size, NULL, slot)))
	{
		*slot = NULL;
		ERROR_LOG(RENDERER, "Pixel shader creation failed\n");
	}
	if (hold != NULL)
		ID3D10Blob_Release(hold);
	return *slot;
}

ID3D11VertexShader *dx11_compiler_make_vs(dx11_shader_compiler *c, u32 family, u32 variant,
		ID3D11VertexShader **slot, ID3DBlob **blob)
{
	dx11_shader_desc d;
	ID3DBlob *hold;
	const void *code;
	size_t size;
	bool in_file;
	if (c->started)
		c->stat_draw_path++;
	if (!c->families->describe(family, variant, &d) || !bytecode(c, family, variant, &d, &hold, &code, &size, &in_file))
		return NULL;
	if (in_file)
		c->stat_from_file++;
	if (FAILED(ID3D11Device_CreateVertexShader(c->device, code, size, NULL, slot)))
	{
		*slot = NULL;
		ERROR_LOG(RENDERER, "Vertex shader creation failed\n");
	}
	if (blob != NULL)
	{
		if (hold == NULL)
			hold = file_blob_new(code, size);
		*blob = hold;
	}
	else if (hold != NULL)
		ID3D10Blob_Release(hold);
	return *slot;
}

ID3D11ComputeShader *dx11_compiler_make_cs(dx11_shader_compiler *c, u32 family, u32 variant, ID3D11ComputeShader **slot)
{
	dx11_shader_desc d;
	ID3DBlob *hold;
	const void *code;
	size_t size;
	bool in_file;
	if (!c->families->describe(family, variant, &d) || !bytecode(c, family, variant, &d, &hold, &code, &size, &in_file))
		return NULL;
	if (in_file)
		c->stat_from_file++;
	if (FAILED(ID3D11Device_CreateComputeShader(c->device, code, size, NULL, slot)))
	{
		*slot = NULL;
		ERROR_LOG(RENDERER, "Compute shader creation failed\n");
	}
	if (hold != NULL)
		ID3D10Blob_Release(hold);
	return *slot;
}

/* The path of @name in the writable data directory: made at the start and
 * the end, never in play */
static char *data_path(const char *name, const char *suffix)
{
	const size_t n = writable_data_path(NULL, 0, name), s = strlen(suffix);
	char *path = (char *)malloc(n + s + 1);
	if (path == NULL)
		return NULL;
	writable_data_path(path, n + 1, name);
	memcpy(path + n, suffix, s + 1);
	return path;
}

static int entry_by_hash(const void *a, const void *b)
{
	const u64 x = ((const d3d_shcache_entry_t *)a)->hash, y = ((const d3d_shcache_entry_t *)b)->hash;
	return x < y ? -1 : x > y ? 1 : 0;
}

static void free_file(dx11_shader_compiler *c)
{
	free(c->file);
	free(c->file_index);
	c->file = NULL;
	c->file_index = NULL;
	c->file_count = 0;
	c->file_blobs = 0;
}

static void load_file(dx11_shader_compiler *c)
{
	char *path = data_path(c->families->file_name, "");
	void *data = NULL;
	int64_t size = 0;
	free_file(c);
	if (path == NULL)
		return;
	if (filestream_read_file(path, &data, &size) && data != NULL && size > 0)
	{
		d3d_shcache_entry_t *entries = NULL;
		size_t count = 0;
		if (d3d_shcache_parse((const u8 *)data, (size_t)size, &entries, &count, &c->file_blobs))
		{
			c->file = (u8 *)data;
			data = NULL;
			c->file_index = entries;
			entries = NULL;
			c->file_count = count;
			qsort(c->file_index, count, sizeof(*c->file_index), entry_by_hash);
		}
		else
		{
			c->file_blobs = 0;
			WARN_LOG(RENDERER, "DX11: shader file %s is not one: ignored\n", path);
		}
		free(entries);
	}
	free(data);
	free(path);
}

/* An entry to save, its name (family << 32 | variant), its place in order */
typedef struct save_entry
{
	u64 name;
	size_t order;
	d3d_shcache_entry_t entry;
	const void *blob;
} save_entry;

static int save_by_name(const void *a, const void *b)
{
	const save_entry *x = (const save_entry *)a, *y = (const save_entry *)b;
	if (x->name != y->name)
		return x->name < y->name ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

static int save_by_order(const void *a, const void *b)
{
	const save_entry *x = (const save_entry *)a, *y = (const save_entry *)b;
	return x->order < y->order ? -1 : x->order > y->order ? 1 : 0;
}

/* What the file had and what this run compiled, by family and variant:
 * this run's where both have one */
static void save_file(dx11_shader_compiler *c)
{
	save_entry *all;
	d3d_shcache_entry_t *entries = NULL;
	const void **blobs = NULL;
	u8 *out = NULL;
	size_t n = 0, kept = 0, i, out_size;
	u32 e;
	char *path, *tmp_path;
	bool written;
	RFILE *f;

	if (!c->file_changed || c->kept.elem == 0)
		return;
	all = (save_entry *)calloc(c->kept.count + c->file_count + 1, sizeof(*all));
	if (all == NULL)
		return;
	for (e = 0; e < tex_map_places(&c->kept); e++)
	{
		u64 hash;
		const kept_blob *k = (const kept_blob *)tex_map_at(&c->kept, e, &hash);
		if (k == NULL)
			continue;
		all[n].name = ((u64)k->family << 32) | k->variant;
		all[n].order = n;
		all[n].entry.hash = hash;
		all[n].entry.family = k->family;
		all[n].entry.variant = k->variant;
		all[n].entry.size = (u32)ID3D10Blob_GetBufferSize(k->blob);
		all[n].blob = ID3D10Blob_GetBufferPointer(k->blob);
		n++;
	}
	for (i = 0; i < c->file_count; i++)
	{
		const d3d_shcache_entry_t *fe = &c->file_index[i];
		all[n].name = ((u64)fe->family << 32) | fe->variant;
		all[n].order = n;
		all[n].entry = *fe;
		all[n].blob = &c->file[c->file_blobs + fe->offset];
		n++;
	}
	/* (the first of each name, in order, and no more than the file holds) */
	qsort(all, n, sizeof(*all), save_by_name);
	for (i = 0; i < n; i++)
		if (i == 0 || all[i].name != all[i - 1].name)
			all[kept++] = all[i];
	qsort(all, kept, sizeof(*all), save_by_order);
	if (kept > D3D_SHCACHE_MAX_ENTRIES)
		kept = D3D_SHCACHE_MAX_ENTRIES;
	if (kept == 0)
	{
		free(all);
		return;
	}
	entries = (d3d_shcache_entry_t *)malloc(kept * sizeof(*entries));
	blobs = (const void **)malloc(kept * sizeof(*blobs));
	if (entries != NULL && blobs != NULL)
	{
		for (i = 0; i < kept; i++)
		{
			entries[i] = all[i].entry;
			blobs[i] = all[i].blob;
		}
		out_size = d3d_shcache_file_size(entries, kept);
		out = (u8 *)malloc(out_size);
	}
	free(all);
	if (out == NULL || !d3d_shcache_write(out, out_size, entries, kept, blobs))
	{
		free(out);
		free(entries);
		free((void *)blobs);
		return;
	}
	free(entries);
	free((void *)blobs);
	/* (written beside it, then put in its place: a write cut short leaves
	 * the one there was) */
	path = data_path(c->families->file_name, "");
	tmp_path = data_path(c->families->file_name, ".tmp");
	f = path != NULL && tmp_path != NULL
			? filestream_open(tmp_path, RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE) : NULL;
	if (f != NULL)
	{
		written = filestream_write(f, out, (int64_t)out_size) == (int64_t)out_size;
		filestream_close(f);
		if (written)
		{
			filestream_delete(path);
			written = filestream_rename(tmp_path, path) == 0;
		}
		if (!written)
			filestream_delete(tmp_path);
		else
			NOTICE_LOG(RENDERER, "DX11: shader file saved: %u shaders\n", (u32)kept);
	}
	free(out);
	free(path);
	free(tmp_path);
}

void dx11_compiler_init(dx11_shader_compiler *c, const dx11_shader_families *families,
		ID3D11Device *device, pD3DCompile compile)
{
	d3d_warm_job_t *jobs;
	size_t count = 0, i, taken;
	/* (made again without being ended: the last thread's work is not kept) */
	if (c->warm != NULL)
	{
		d3d_warm_free(c->warm);
		c->warm = NULL;
	}
	c->families = families;
	c->device = device;
	c->compile = compile;
	c->stat_compiled = c->stat_from_file = c->stat_built_in = c->stat_ahead = c->stat_compiled_ahead = 0;
	/* (room for what a run compiles; more is made, and said) */
	if (c->kept.elem == 0)
	{
		tex_map_init(&c->kept, sizeof(kept_blob), NULL, kept_unmake);
		c->kept.name = "DX11 shaders compiled";
	}
	tex_map_clear(&c->kept);
	tex_map_reserve(&c->kept, 1024);
	c->stat_draw_path = c->stat_general = 0;
	c->started = false;
	c->file_changed = false;
	load_file(c);
	/* (a device made for one thread only is used from the one that renders
	 * only: the file still spares it compiling) */
	if (device == NULL || (ID3D11Device_GetCreationFlags(device) & D3D11_CREATE_DEVICE_SINGLETHREADED) != 0)
		return;
	jobs = (d3d_warm_job_t *)malloc((families->fixed_count + c->file_count + 1) * sizeof(*jobs));
	if (jobs == NULL)
		return;
	for (i = 0; i < families->fixed_count; i++)
	{
		jobs[count].key = 0;
		jobs[count].kind = families->fixed[i] >> 16;
		jobs[count].aux = families->fixed[i] & 0xFFFF;
		count++;
	}
	for (i = 0; i < c->file_count; i++)
	{
		const d3d_shcache_entry_t *e = &c->file_index[i];
		size_t j = 0;
		if (!families->pixel_family(e->family))
			continue;
		/* (one of a kind, queued already; the file has each once) */
		while (j < families->fixed_count && (jobs[j].kind != e->family || jobs[j].aux != e->variant))
			j++;
		if (j < families->fixed_count)
			continue;
		jobs[count].key = 0;
		jobs[count].kind = e->family;
		jobs[count].aux = e->variant;
		count++;
	}
	/* (room for the keys a game first wants while it runs, too) */
	c->warm = d3d_warm_new(dx11_warm_build, dx11_warm_release, c, MAX(count, 1024));
	if (c->warm != NULL && count != 0)
	{
		taken = d3d_warm_submit(c->warm, jobs, count);
		NOTICE_LOG(RENDERER, "DX11: %u shaders in %s, %u pixel shaders to be made ahead\n",
				(u32)c->file_count, families->file_name, (u32)taken);
	}
	free(jobs);
}

bool dx11_compiler_start(dx11_shader_compiler *c, u32 keys)
{
	u32 i;
	for (i = 0; i < c->pixel_keys; i++)
		if (c->pixel[i] != NULL)
			ID3D11PixelShader_Release(c->pixel[i]);
	free(c->pixel);
	free(c->requested);
	c->pixel = (ID3D11PixelShader **)calloc(keys, sizeof(*c->pixel));
	c->requested = (u32 *)calloc(keys / 32 + 1, sizeof(u32));
	c->pixel_keys = c->pixel != NULL && c->requested != NULL ? keys : 0;
	return c->pixel_keys != 0;
}

ID3D11PixelShader *dx11_compiler_polygon_ps(dx11_shader_compiler *c, u32 family, u32 key)
{
	const u32 m = (key ^ (key >> 6) ^ (key >> 12)) & 63;
	if (c->memo[m].shader != NULL && c->memo[m].key == key)
		return c->memo[m].shader;
	if (key >= c->pixel_keys)
		return NULL;
#ifndef DX11_SHADER_TEST_GENERAL
	if (c->pixel[key] == NULL)
		dx11_compiler_take_built(c);
	if (c->pixel[key] != NULL)
	{
		c->memo[m].key = key;
		c->memo[m].shader = c->pixel[key];
		return c->pixel[key];
	}
	/* (its own asked for once; drawn with the general one till it is there) */
	if ((c->requested[key >> 5] & (1u << (key & 31))) == 0)
	{
		c->requested[key >> 5] |= 1u << (key & 31);
		/* (the queue full: asked again at its next draw) */
		if (!request(c, family, key))
			c->requested[key >> 5] &= ~(1u << (key & 31));
	}
#else
	(void)family;
#endif
	c->stat_general++;
	return NULL;
}

void dx11_compiler_term(dx11_shader_compiler *c)
{
	u32 i;
	if (c->warm != NULL)
	{
		d3d_warm_stop(c->warm);
		dx11_compiler_take_built(c);
		d3d_warm_free(c->warm);
		c->warm = NULL;
	}
	if (c->device != NULL && c->families != NULL)
		NOTICE_LOG(RENDERER, "DX11: %s: %u compiled, %u the core's, %u from the file, %u made ahead (%u of them compiled), %u made on the draw path, %u draws with a general shader\n",
				c->families->file_name, c->stat_compiled, c->stat_built_in, c->stat_from_file, c->stat_ahead,
				c->stat_compiled_ahead, c->stat_draw_path, c->stat_general);
	if (c->families != NULL)
		save_file(c);
	free_file(c);
	if (c->kept.elem != 0)
		tex_map_free(&c->kept);
	memset(&c->kept, 0, sizeof(c->kept));
	c->file_changed = false;
	for (i = 0; i < c->pixel_keys; i++)
		if (c->pixel[i] != NULL)
			ID3D11PixelShader_Release(c->pixel[i]);
	free(c->pixel);
	free(c->requested);
	c->pixel = NULL;
	c->requested = NULL;
	c->pixel_keys = 0;
	memset(c->memo, 0, sizeof(c->memo));
	c->device = NULL;
	c->compile = NULL;
}

/* ---- The per-triangle renderer's ---- */

#define TEXT_VS \
"\n" \
"#if pp_Gouraud == 1\n" \
"#define INTERPOLATION\n" \
"#else\n" \
"#define INTERPOLATION nointerpolation\n" \
"#endif\n" \
"\n" \
"struct VertexIn\n" \
"{\n" \
"\tfloat3 pos : POSITION;\n" \
"\tfloat4 col : COLOR0;\n" \
"\tfloat4 spec : COLOR1;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"};\n" \
"\n" \
"struct VertexOut\n" \
"{\n" \
"\tprecise float4 pos : SV_POSITION;\n" \
"\tINTERPOLATION float4 col : COLOR0;\n" \
"\tINTERPOLATION float4 spec : COLOR1;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"};\n" \
"\n" \
"VertexOut main(in VertexIn vin)\n" \
"{\n" \
"\tVertexOut vo;\n" \
"\tvo.pos = transformVertex(vin.pos, vo.clipW);\n" \
"\tvo.col = vin.col;\n" \
"\tvo.spec = vin.spec;\n" \
"\tvo.uv = vin.uv;\n" \
"\treturn vo;\n" \
"}\n"

#define TEXT_PS_HEADER \
"\n" \
"cbuffer FragmentConstants : register(b0)\n" \
"{\n" \
"\tfloat4 colorClampMin;\n" \
"\tfloat4 colorClampMax;\n" \
"\tfloat4 sp_FOG_COL_RAM;\n" \
"\tfloat4 sp_FOG_COL_VERT;\n" \
"\tfloat cp_AlphaTestValue;\n" \
"\tfloat sp_FOG_DENSITY;\n" \
"\tfloat shade_scale_factor;\n" \
"};\n" \
"\n" \
"Texture2D fog_table : register(t2);\n" \
"// nearest, clamped: the fog table's and the palette's\n" \
"SamplerState pointSampler : register(s1);\n"

#define TEXT_PS \
"\n" \
"#if pp_Gouraud == 1\n" \
"#define INTERPOLATION\n" \
"#else\n" \
"#define INTERPOLATION nointerpolation\n" \
"#endif\n" \
"#define PI 3.1415926\n" \
"\n" \
"cbuffer PolyConstants : register(b1)\n" \
"{\n" \
"\tfloat4 clipTest;\n" \
"\tfloat trilinearAlpha;\n" \
"\tfloat palette_index;\n" \
"\tfloat prepassAlpha;\n" \
"\tuint polyKey;\t// the general shader's: DX11FragmentShaderParams::hash()\n" \
"};\n" \
"\n" \
"// A polygon's own shader has its key's bits as numbers, and the branches\n" \
"// they decide are compiled away; the general shader (GENERAL) reads them\n" \
"// from the polygon's constants, and draws what its own shader would while\n" \
"// that is made: the same code, the same pixels\n" \
"#ifdef GENERAL\n" \
"#define cp_AlphaTest (polyKey & 1u)\n" \
"#define pp_ClipInside ((polyKey >> 1) & 1u)\n" \
"#define pp_UseAlpha ((polyKey >> 2) & 1u)\n" \
"#define pp_Texture ((polyKey >> 3) & 1u)\n" \
"#define pp_IgnoreTexA ((polyKey >> 4) & 1u)\n" \
"#define pp_ShadInstr ((polyKey >> 5) & 3u)\n" \
"#define pp_Offset ((polyKey >> 7) & 1u)\n" \
"#define pp_FogCtrl ((polyKey >> 8) & 3u)\n" \
"#define pp_BumpMap ((polyKey >> 11) & 1u)\n" \
"#define ColorClamping ((polyKey >> 12) & 1u)\n" \
"#define pp_TriLinear ((polyKey >> 13) & 1u)\n" \
"#define pp_Palette ((polyKey >> 14) & 1u)\n" \
"#define pp_Shadowed ((polyKey >> 15) & 1u)\n" \
"#endif\n" \
"\n" \
"Texture2D tex : register(t0);\n" \
"SamplerState texSampler : register(s0);\n" \
"Texture2D palette : register(t1);\n" \
"\n" \
"float4 colorClamp(float4 col)\n" \
"{\n" \
"\tif (ColorClamping == 1)\n" \
"\t\treturn clamp(col, colorClampMin, colorClampMax);\n" \
"\treturn col;\n" \
"}\n" \
"\n" \
"float4 palettePixel(float2 coords)\n" \
"{\n" \
"\t// palette_index is where the middle of the palette's first colour is in the row of 1024\n" \
"\tfloat2 c = float2(tex.Sample(texSampler, coords).r * (255.0 / 1024.0) + palette_index, 0.5);\n" \
"\treturn palette.SampleLevel(pointSampler, c, 0);\n" \
"}\n" \
"\n" \
"struct Pixel\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tINTERPOLATION float4 col : COLOR0;\n" \
"\tINTERPOLATION float4 spec : COLOR1;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"};\n" \
"\n" \
"// (the depth is the position's, which the depth test took: no SV_Depth)\n" \
"struct PSO\n" \
"{\n" \
"\tfloat4 col : SV_Target;\n" \
"};\n" \
"\n" \
"PSO main(in Pixel inpix)\n" \
"{\n" \
"\tconst float w = 1.0 / inpix.clipW;\n" \
"\t// Clip inside the box\n" \
"\tif (pp_ClipInside == 1)\n" \
"\t{\n" \
"\t\tif (inpix.pos.x >= clipTest.x && inpix.pos.x <= clipTest.z\n" \
"\t\t\t\t&& inpix.pos.y >= clipTest.y && inpix.pos.y <= clipTest.w)\n" \
"\t\t\tdiscard;\n" \
"\t}\n" \
"\n" \
"\tfloat4 color = inpix.col;\n" \
"\tif (pp_UseAlpha == 0)\n" \
"\t\tcolor.a = 1.0;\n" \
"\tfloat4 offset = inpix.spec;\n" \
"\tif (pp_Shadowed == 1)\n" \
"\t{\n" \
"\t\t// A pixel inside a modifier volume: what the polygon is shaded with,\n" \
"\t\t// its base and offset colours, is scaled before the texture is\n" \
"\t\t// combined with them. The texture itself is not.\n" \
"\t\tcolor.rgb *= shade_scale_factor;\n" \
"\t\toffset.rgb *= shade_scale_factor;\n" \
"\t}\n" \
"\tif (pp_Texture == 1)\n" \
"\t{\n" \
"\t\tfloat4 texcol;\n" \
"\t\tif (pp_Palette == 0)\n" \
"\t\t\ttexcol = tex.Sample(texSampler, inpix.uv);\n" \
"\t\telse\n" \
"\t\t\ttexcol = palettePixel(inpix.uv);\n" \
"\t\tif (pp_BumpMap == 1)\n" \
"\t\t{\n" \
"\t\t\tfloat s = PI / 2.0 * (texcol.a * 15.0 * 16.0 + texcol.r * 15.0) / 255.0;\n" \
"\t\t\tfloat r = 2.0 * PI * (texcol.g * 15.0 * 16.0 + texcol.b * 15.0) / 255.0;\n" \
"\t\t\ttexcol.a = clamp(offset.a + offset.r * sin(s) + offset.g * cos(s) * cos(r - 2.0 * PI * offset.b), 0.0, 1.0);\n" \
"\t\t\ttexcol.rgb = float3(1.0, 1.0, 1.0);\n" \
"\t\t}\n" \
"\t\telse if (pp_IgnoreTexA == 1)\n" \
"\t\t\ttexcol.a = 1.0;\n" \
"\t\tif (pp_ShadInstr == 0)\n" \
"\t\t\tcolor = texcol;\n" \
"\t\tif (pp_ShadInstr == 1)\n" \
"\t\t{\n" \
"\t\t\tcolor.rgb *= texcol.rgb;\n" \
"\t\t\tcolor.a = texcol.a;\n" \
"\t\t}\n" \
"\t\tif (pp_ShadInstr == 2)\n" \
"\t\t\tcolor.rgb = lerp(color.rgb, texcol.rgb, texcol.a);\n" \
"\t\tif (pp_ShadInstr == 3)\n" \
"\t\t\tcolor *= texcol;\n" \
"\t\tif (pp_Offset == 1 && pp_BumpMap == 0)\n" \
"\t\t\tcolor.rgb += offset.rgb;\n" \
"\t}\n" \
"\n" \
"\tcolor = colorClamp(color);\n" \
"\n" \
"\t// Look-up table mode 2: the fog colour takes the place of the pixel's\n" \
"\t// colour and the fog coefficient the place of its alpha, after the\n" \
"\t// texture has been combined in.\n" \
"\tif (pp_FogCtrl == 3)\n" \
"\t\tcolor = float4(sp_FOG_COL_RAM.rgb, fog_mode2(w) * (256.0 / 255.0));\n" \
"\tif (pp_FogCtrl == 0)\n" \
"\t{\n" \
"\t\tfloat fog = fog_mode2(w);\n" \
"\t\tcolor.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_RAM.rgb * fog;\n" \
"\t}\n" \
"\tif (pp_FogCtrl == 1 && pp_Offset == 1 && pp_BumpMap == 0)\n" \
"\t{\n" \
"\t\tfloat fog = offset.a * (255.0 / 256.0);\n" \
"\t\tcolor.rgb = color.rgb * (255.0 / 256.0 - fog) + sp_FOG_COL_VERT.rgb * fog;\n" \
"\t}\n" \
"\n" \
"\tif (pp_TriLinear == 1)\n" \
"\t\tcolor *= trilinearAlpha;\n" \
"\n" \
"\t// Punch-through polygons are tested on the alpha of the finished colour,\n" \
"\t// after the texture and the vertex colours are combined and clamped.\n" \
"\tif (cp_AlphaTest == 1)\n" \
"\t{\n" \
"\t\tif (cp_AlphaTestValue > color.a)\n" \
"\t\t\tdiscard;\n" \
"\t\tcolor.a = 1.0;\n" \
"\t}\n" \
"\t// The depth prepass of sorted translucent polygons (rend/sorter.h): the\n" \
"\t// pixels that hide what is behind them. (0 for the colours.)\n" \
"\tif (color.a < prepassAlpha)\n" \
"\t\tdiscard;\n" \
"\n" \
"\tPSO pso;\n" \
"\tpso.col = color;\n" \
"\treturn pso;\n" \
"}\n"

#define TEXT_MODVOL_PS \
"\n" \
"struct Pixel\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tfloat clipW : TEXCOORD1;\n" \
"};\n" \
"\n" \
"// The colour is only ever written by the pass that darkens what is left\n" \
"// in the volumes: by what the shadows take away (the depth is the position's)\n" \
"float4 main(in Pixel inpix) : SV_Target\n" \
"{\n" \
"\treturn float4(0.0, 0.0, 0.0, 1.0 - shade_scale_factor);\n" \
"}\n"

#define TEXT_QUAD_VS \
"\n" \
"struct VertexIn\n" \
"{\n" \
"\tfloat2 pos : POSITION;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"};\n" \
"\n" \
"struct VertexOut\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"};\n" \
"\n" \
"VertexOut main(in VertexIn vin)\n" \
"{\n" \
"\tVertexOut vo;\n" \
"\tvo.pos = float4(vin.pos, 0.0, 1.0);\n" \
"\tvo.uv = vin.uv;\n" \
"\treturn vo;\n" \
"}\n"

#define TEXT_QUAD_PS \
"\n" \
"struct VertexOut\n" \
"{\n" \
"\tfloat4 pos : SV_POSITION;\n" \
"\tfloat2 uv : TEXCOORD0;\n" \
"};\n" \
"\n" \
"Texture2D tex : register(t0);\n" \
"SamplerState texSampler : register(s0);\n" \
"\n" \
"float4 main(in VertexOut vin) : SV_Target\n" \
"{\n" \
"\treturn tex.Sample(texSampler, vin.uv);\n" \
"}\n"

static const char src_vs[] = DX11_TEXT_TRANSFORM TEXT_VS;
static const char src_ps[] = TEXT_PS_HEADER DX11_TEXT_FOG TEXT_PS;
static const char src_modvol_vs[] = DX11_TEXT_TRANSFORM DX11_TEXT_MODVOL_VS;
static const char src_modvol_ps[] = TEXT_PS_HEADER DX11_TEXT_FOG TEXT_MODVOL_PS;
static const char src_quad_vs[] = TEXT_QUAD_VS;
static const char src_quad_ps[] = TEXT_QUAD_PS;
static const char src_writeout[] =
#include "dx11_writeout_text.inc"
;

static const char * const MacroValues[] = { "0", "1", "2", "3" };

#define SOURCE(d, s) ((d)->source = (s), (d)->length = sizeof(s) - 1)
#define MACRO(m, n, v) ((m)->Name = (n), (m)->Definition = (v), (m)++)

static bool tri_describe(u32 family, u32 variant, dx11_shader_desc *d)
{
	D3D_SHADER_MACRO *m = d->macros;
	switch (family)
	{
	case DX11F_VS:
		SOURCE(d, src_vs);
		d->profile = "vs_4_0";
		MACRO(m, "pp_Gouraud", MacroValues[variant & 1]);
		break;
	case DX11F_PS:
		/* (the variant is dx11_shaders_key()) */
		SOURCE(d, src_ps);
		d->profile = "ps_4_0";
		MACRO(m, "cp_AlphaTest", MacroValues[variant & 1]);
		MACRO(m, "pp_ClipInside", MacroValues[(variant >> 1) & 1]);
		MACRO(m, "pp_UseAlpha", MacroValues[(variant >> 2) & 1]);
		MACRO(m, "pp_Texture", MacroValues[(variant >> 3) & 1]);
		MACRO(m, "pp_IgnoreTexA", MacroValues[(variant >> 4) & 1]);
		MACRO(m, "pp_ShadInstr", MacroValues[(variant >> 5) & 3]);
		MACRO(m, "pp_Offset", MacroValues[(variant >> 7) & 1]);
		MACRO(m, "pp_FogCtrl", MacroValues[(variant >> 8) & 3]);
		MACRO(m, "pp_Gouraud", MacroValues[(variant >> 10) & 1]);
		MACRO(m, "pp_BumpMap", MacroValues[(variant >> 11) & 1]);
		MACRO(m, "ColorClamping", MacroValues[(variant >> 12) & 1]);
		MACRO(m, "pp_TriLinear", MacroValues[(variant >> 13) & 1]);
		MACRO(m, "pp_Palette", MacroValues[(variant >> 14) & 1]);
		MACRO(m, "pp_Shadowed", MacroValues[(variant >> 15) & 1]);
		break;
	case DX11F_GENERAL_PS:
		SOURCE(d, src_ps);
		d->profile = "ps_4_0";
		MACRO(m, "GENERAL", "1");
		MACRO(m, "pp_Gouraud", MacroValues[variant & 1]);
		break;
	case DX11F_MODVOL_VS:
		SOURCE(d, src_modvol_vs);
		d->profile = "vs_4_0";
		break;
	case DX11F_MODVOL_PS:
		SOURCE(d, src_modvol_ps);
		d->profile = "ps_4_0";
		break;
	case DX11F_QUAD_VS:
		SOURCE(d, src_quad_vs);
		d->profile = "vs_4_0";
		break;
	case DX11F_QUAD_PS:
		SOURCE(d, src_quad_ps);
		d->profile = "ps_4_0";
		break;
	case DX11F_WRITEOUT_CS:
		SOURCE(d, src_writeout);
		d->profile = "cs_5_0";
		break;
	default:
		return false;
	}
	m->Name = NULL;
	m->Definition = NULL;
	return true;
}

/* by dx11_shaders_key(), all of them: nothing allocated in play */
#define TRI_PIXEL_KEYS (1u << 16)

static struct
{
	dx11_shader_compiler c;
	ID3D11VertexShader *vs[2];
	ID3DBlob *vs_blob;
	ID3D11PixelShader *general[2];	/* by pp_Gouraud */
	ID3D11VertexShader *modvol_vs;
	ID3DBlob *modvol_vs_blob;
	ID3D11PixelShader *modvol_ps;
	ID3D11VertexShader *quad_vs;
	ID3DBlob *quad_vs_blob;
	ID3D11PixelShader *quad_ps;
} tri;

static void tri_adopt(u32 family, u32 variant, ID3D11PixelShader *shader)
{
	ID3D11PixelShader **slot = NULL;
	switch (family)
	{
	case DX11F_PS:
		if (variant < tri.c.pixel_keys)
			slot = &tri.c.pixel[variant];
		break;
	case DX11F_MODVOL_PS:
		slot = &tri.modvol_ps;
		break;
	case DX11F_QUAD_PS:
		slot = &tri.quad_ps;
		break;
	}
	if (slot != NULL && *slot == NULL)
		*slot = shader;
	else
		ID3D11PixelShader_Release(shader);
}

static bool tri_pixel_family(u32 family)
{
	return family == DX11F_PS || family == DX11F_MODVOL_PS || family == DX11F_QUAD_PS;
}

static const u32 tri_fixed[] = { DX11F_MODVOL_PS << 16, DX11F_QUAD_PS << 16 };

static const dx11_shader_families tri_families = {
	"d3d11_shaders.bin", tri_describe, tri_adopt, tri_pixel_family, tri_fixed, 2
};

u32 dx11_shaders_key(const DX11FragmentShaderParams *p)
{
	return (u32)p->alphaTest | ((u32)p->insideClipTest << 1) | ((u32)p->useAlpha << 2)
		| ((u32)p->texture << 3) | ((u32)p->ignoreTexAlpha << 4) | ((u32)p->shaderInstr << 5)
		| ((u32)p->offset << 7) | ((u32)p->fog << 8) | ((u32)p->gouraud << 10)
		| ((u32)p->bumpmap << 11) | ((u32)p->clamping << 12) | ((u32)p->trilinear << 13)
		| ((u32)p->palette << 14) | ((u32)p->shadowed << 15);
}

ID3D11VertexShader *dx11_shaders_vs(bool gouraud)
{
	ID3D11VertexShader **shader = &tri.vs[gouraud ? 1 : 0];
	if (*shader == NULL)
	{
		ID3DBlob *blob = NULL;
		dx11_compiler_make_vs(&tri.c, DX11F_VS, gouraud ? 1 : 0, shader, &blob);
		if (tri.vs_blob == NULL)
			tri.vs_blob = blob;
		else if (blob != NULL)
			ID3D10Blob_Release(blob);
	}
	return *shader;
}

bool dx11_shaders_start(void)
{
	u32 g;
	bool ok = dx11_compiler_start(&tri.c, TRI_PIXEL_KEYS);
	for (g = 0; g < 2; g++)
	{
		dx11_shaders_vs(g != 0);
		dx11_compiler_missed_ps(&tri.c, DX11F_GENERAL_PS, g, &tri.general[g]);
	}
	dx11_shaders_modvol_vs();
	dx11_shaders_modvol_ps();
	dx11_shaders_quad_vs();
	dx11_shaders_quad_ps();
	tri.c.started = true;
	return ok && tri.vs[0] != NULL && tri.vs[1] != NULL && tri.general[0] != NULL && tri.general[1] != NULL
			&& tri.modvol_vs != NULL && tri.modvol_ps != NULL && tri.quad_vs != NULL && tri.quad_ps != NULL;
}

ID3D11PixelShader *dx11_shaders_ps(const DX11FragmentShaderParams *params, bool *general)
{
	ID3D11PixelShader *own = dx11_compiler_polygon_ps(&tri.c, DX11F_PS, dx11_shaders_key(params));
	*general = own == NULL;
	return own != NULL ? own : tri.general[params->gouraud ? 1 : 0];
}

ID3D11VertexShader *dx11_shaders_modvol_vs(void)
{
	if (tri.modvol_vs == NULL)
		dx11_compiler_make_vs(&tri.c, DX11F_MODVOL_VS, 0, &tri.modvol_vs, &tri.modvol_vs_blob);
	return tri.modvol_vs;
}

ID3D11PixelShader *dx11_shaders_modvol_ps(void)
{
	if (tri.modvol_ps == NULL)
		dx11_compiler_missed_ps(&tri.c, DX11F_MODVOL_PS, 0, &tri.modvol_ps);
	return tri.modvol_ps;
}

ID3D11VertexShader *dx11_shaders_quad_vs(void)
{
	if (tri.quad_vs == NULL)
		dx11_compiler_make_vs(&tri.c, DX11F_QUAD_VS, 0, &tri.quad_vs, &tri.quad_vs_blob);
	return tri.quad_vs;
}

ID3D11PixelShader *dx11_shaders_quad_ps(void)
{
	if (tri.quad_ps == NULL)
		dx11_compiler_missed_ps(&tri.c, DX11F_QUAD_PS, 0, &tri.quad_ps);
	return tri.quad_ps;
}

ID3D11ComputeShader *dx11_shaders_writeout_cs(ID3D11ComputeShader **slot)
{
	return dx11_compiler_make_cs(&tri.c, DX11F_WRITEOUT_CS, 0, slot);
}

ID3DBlob *dx11_shaders_vs_blob(void)
{
	dx11_shaders_vs(true);
	return tri.vs_blob;
}

ID3DBlob *dx11_shaders_modvol_vs_blob(void)
{
	dx11_shaders_modvol_vs();
	return tri.modvol_vs_blob;
}

ID3DBlob *dx11_shaders_quad_vs_blob(void)
{
	dx11_shaders_quad_vs();
	return tri.quad_vs_blob;
}

dx11_shader_compiler *dx11_shaders_compiler(void)
{
	return &tri.c;
}

const dx11_shader_families *dx11_shaders_families(void)
{
	return &tri_families;
}

void dx11_shaders_init(ID3D11Device *device, pD3DCompile compile)
{
	dx11_compiler_init(&tri.c, &tri_families, device, compile);
}

#define RELEASE_TO_NULL(p, how) do { if ((p) != NULL) how(p); (p) = NULL; } while (0)

void dx11_shaders_term(void)
{
	u32 i;
	dx11_compiler_term(&tri.c);
	for (i = 0; i < 2; i++)
	{
		RELEASE_TO_NULL(tri.vs[i], ID3D11VertexShader_Release);
		RELEASE_TO_NULL(tri.general[i], ID3D11PixelShader_Release);
	}
	RELEASE_TO_NULL(tri.vs_blob, ID3D10Blob_Release);
	RELEASE_TO_NULL(tri.modvol_vs, ID3D11VertexShader_Release);
	RELEASE_TO_NULL(tri.modvol_vs_blob, ID3D10Blob_Release);
	RELEASE_TO_NULL(tri.modvol_ps, ID3D11PixelShader_Release);
	RELEASE_TO_NULL(tri.quad_vs, ID3D11VertexShader_Release);
	RELEASE_TO_NULL(tri.quad_vs_blob, ID3D10Blob_Release);
	RELEASE_TO_NULL(tri.quad_ps, ID3D11PixelShader_Release);
}
