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
#pragma once
/* (Direct3D's headers before the emulator's: hw/pvr/pvr_regs.h has macros
 * with the names of some of their parameters) */
#include "rend/d3d_system.h"
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "types.h"
#include "rend/d3d_warm.h"
#include "rend/d3d_shader_cache.h"
#include "rend/tex_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a shader is compiled from: its source, profile and macros (a list
 * ending with a null one). One function says it for each shader, for the
 * thread that renders and the one that makes them ahead alike. */
typedef struct dx11_shader_desc
{
	const char *source;		/* the family's, a string literal */
	size_t length;			/* (without its 0) */
	const char *profile;
	D3D_SHADER_MACRO macros[16];
} dx11_shader_desc;

typedef struct dx11_shader_compiler dx11_shader_compiler;

/* A renderer's families of shaders */
typedef struct dx11_shader_families
{
	const char *file_name;
	/* What @family's @variant is compiled from (both threads) */
	bool (*describe)(u32 family, u32 variant, dx11_shader_desc *d);
	/* A pixel shader the thread made, kept if its place is empty, else
	 * released */
	void (*adopt)(u32 family, u32 variant, ID3D11PixelShader *shader);
	/* The families of pixel shaders, made ahead */
	bool (*pixel_family)(u32 family);
	/* The pixel shaders that are one of a kind, made ahead whether or not
	 * the file has them: the family << 16 | the variant */
	const u32 *fixed;
	u32 fixed_count;
} dx11_shader_families;

/* The shaders, in HLSL, compiled with the compiler the frontend hands over
 * (or d3dcompiler_47.dll's), and kept.
 *
 * Nothing is compiled or made on the draw path (AGENTS.md). Every vertex
 * shader, every pixel shader of a kind of its own and a general pixel
 * shader - one that reads a polygon's key from its constants and draws
 * what the polygon's own would - are made as the renderer starts. A
 * polygon's own pixel shader is made on a thread of its own
 * (rend/d3d_warm.h), the ones the file had ahead, the others as a frame
 * first wants them; until it is there the polygon is drawn with the general
 * one. Without the thread (a device made for one thread, no threads) the
 * general one draws what the file did not have, for the run.
 *
 * The shaders the start makes are in the core as bytecode (dx11_dxbc.inc,
 * tools/rend/kernels_dx11.sh), found by the hash of what they are compiled
 * from: the renderers start with no compiler, and draw every polygon with
 * the general shaders without one. A polygon's own is compiled when there
 * is a compiler, and kept on disk (rend/d3d_shader_cache.h), whatever the
 * driver: a shader compiled once is not compiled again. */
struct dx11_shader_compiler
{
	const dx11_shader_families *families;
	ID3D11Device *device;
	pD3DCompile compile;
	/* What has been done so far, for measuring */
	u32 stat_draw_path;		/* shaders made by the thread that renders once it started: always 0 */
	u32 stat_general;		/* draws with a general pixel shader, the polygon's own not there yet */
	u32 stat_compiled;		/* compiled by the thread that renders */
	u32 stat_from_file;		/* ...or found in the file */
	u32 stat_built_in;		/* ...or in the core */
	u32 stat_ahead;			/* made by the thread, and taken */
	u32 stat_compiled_ahead;	/* of them, compiled (not in the file) */
	/* What is made from now on is made on the draw path */
	bool started;
	d3d_warm_t *warm;
	/* The file read, its index sorted by hash, its blobs from file_blobs */
	u8 *file;
	d3d_shcache_entry_t *file_index;
	size_t file_count;
	size_t file_blobs;
	/* compiled this run, by hash: room made in dx11_compiler_init() */
	tex_map_t kept;
	bool file_changed;
	/* The pixel shaders last looked up, by their key: a polygon's is
	 * looked up for every polygon, and is most often one of a few */
	struct { u32 key; ID3D11PixelShader *shader; } memo[64];
	/* A bit a polygon key, asked of the thread; and the polygons' own,
	 * by key: made in dx11_compiler_start(), nothing allocated in play */
	u32 *requested;
	ID3D11PixelShader **pixel;
	u32 pixel_keys;
};

void dx11_compiler_init(dx11_shader_compiler *c, const dx11_shader_families *families,
		ID3D11Device *device, pD3DCompile compile);
/* The polygons' own pixel shaders' places: @keys of them */
bool dx11_compiler_start(dx11_shader_compiler *c, u32 keys);
/* The thread stopped, what it made taken, the file saved; the polygons'
 * own released */
void dx11_compiler_term(dx11_shader_compiler *c);
u64 dx11_shader_hash(const dx11_shader_desc *d);
/* (an array of macros ending with a null one, or null) */
ID3DBlob *dx11_compiler_compile(dx11_shader_compiler *c, const char *source, const char *entry, const char *profile,
		const D3D_SHADER_MACRO *macros, ID3DInclude *include);
ID3D11VertexShader *dx11_compiler_compile_vs(dx11_shader_compiler *c, const char *source, const char *entry,
		const char *profile, const D3D_SHADER_MACRO *macros, ID3DBlob **blob, ID3DInclude *include);
ID3D11PixelShader *dx11_compiler_compile_ps(dx11_shader_compiler *c, const char *source, const char *entry,
		const char *profile, const D3D_SHADER_MACRO *macros, ID3DInclude *include);
/* Whether a shader the thread made, or one asked of it, is taken: the
 * renderer's thread, with nothing waited for */
void dx11_compiler_take_built(dx11_shader_compiler *c);
/* A pixel shader not there: the thread's, else made here (the start's) */
ID3D11PixelShader *dx11_compiler_missed_ps(dx11_shader_compiler *c, u32 family, u32 variant, ID3D11PixelShader **slot);
/* A vertex shader, made here, and its bytecode for an input layout */
ID3D11VertexShader *dx11_compiler_make_vs(dx11_shader_compiler *c, u32 family, u32 variant,
		ID3D11VertexShader **slot, ID3DBlob **blob);
/* A compute shader, made here (the renderer's start) */
ID3D11ComputeShader *dx11_compiler_make_cs(dx11_shader_compiler *c, u32 family, u32 variant, ID3D11ComputeShader **slot);
/* A polygon's own pixel shader of @family, its @key; null while the thread
 * makes it, and then the general one draws it */
ID3D11PixelShader *dx11_compiler_polygon_ps(dx11_shader_compiler *c, u32 family, u32 key);

/* ---- The per-triangle renderer's ---- */

/* Their families (the file's numbers, kept) */
enum { DX11F_VS, DX11F_PS, DX11F_MODVOL_VS, DX11F_MODVOL_PS, DX11F_QUAD_VS, DX11F_QUAD_PS, DX11F_GENERAL_PS, DX11F_WRITEOUT_CS };

/* What a polygon's pixel shader is made for: as the Vulkan renderer's */
typedef struct DX11FragmentShaderParams
{
	bool alphaTest;
	bool insideClipTest;
	bool useAlpha;
	bool texture;
	bool ignoreTexAlpha;
	int shaderInstr;
	bool offset;
	int fog;
	bool gouraud;
	bool bumpmap;
	bool clamping;
	bool trilinear;
	bool palette;
	bool shadowed;	/* drawn a second time where it is in a modifier volume: rend/shadows.h */
} DX11FragmentShaderParams;

u32 dx11_shaders_key(const DX11FragmentShaderParams *p);

/* Made with the context (dx11context_lr.c) */
void dx11_shaders_init(ID3D11Device *device, pD3DCompile compile);
void dx11_shaders_term(void);
/* Everything a frame can want, made before the first: the vertex shaders,
 * the ones of a kind, the general pixel shaders */
bool dx11_shaders_start(void);
ID3D11VertexShader *dx11_shaders_vs(bool gouraud);
/* A polygon's own, or the general one while its own is made (*general):
 * the polygon's key then in its constants */
ID3D11PixelShader *dx11_shaders_ps(const DX11FragmentShaderParams *params, bool *general);
ID3D11VertexShader *dx11_shaders_modvol_vs(void);
ID3D11PixelShader *dx11_shaders_modvol_ps(void);
ID3D11VertexShader *dx11_shaders_quad_vs(void);
ID3D11PixelShader *dx11_shaders_quad_ps(void);
/* The chip's write-out pass (dx11_writeout.h), made into @slot */
ID3D11ComputeShader *dx11_shaders_writeout_cs(ID3D11ComputeShader **slot);
/* For the input layouts: what the vertex shaders take */
ID3DBlob *dx11_shaders_vs_blob(void);
ID3DBlob *dx11_shaders_modvol_vs_blob(void);
ID3DBlob *dx11_shaders_quad_vs_blob(void);
/* (for the tests) */
dx11_shader_compiler *dx11_shaders_compiler(void);
const dx11_shader_families *dx11_shaders_families(void);

#ifdef __cplusplus
}
#endif
