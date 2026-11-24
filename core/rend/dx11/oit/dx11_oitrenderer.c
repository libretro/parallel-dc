#define PVR_REGS_FOR_RENDERER	/* see hw/pvr/pvr_regs.h */
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
/* The per-pixel renderer: the translucent polygons' pixels are put in
 * lists, sorted by depth and blended at the end, as the Vulkan per-pixel
 * renderer does it (vulkan/oit/oit_drawer.cpp), step for step:
 *
 *  - the opaque and punch-through polygons' depth, and the modifier
 *    volumes into the stencil;
 *  - their colour, where their depth is the one left, darkened where the
 *    stencil says they are in a volume;
 *  - the translucent polygons' pixels into the lists (or, in a list that
 *    is not to be sorted, drawn as they come);
 *  - the translucent modifier volumes, marking the pixels in the lists;
 *  - the lists put together over the colour, into the picture - or, with
 *    a render pass after this one, into a picture the next pass draws on.
 *
 * The rest is the per-triangle renderer's: the textures, renders to
 * textures, the screen's pictures, the last picture. */
#include "../dx11_renderer.h"
#include <stdlib.h>
#include <string.h>
#include <retro_miscellaneous.h>
#include "dx11_oitshaders.h"
#include "../dx11_texture.h"
#include "../dx11_ta_decoder.h"
#include "hw/pvr/ta.h"
#include "rend/oit_size.h"
#include "rend/tileclip.h"
#include "rend/modvol_clip.h"
#include "rend/texconv.h"

#define PPW(w, f) TAW_GET(w, f)
#define MIN_U(a, b) ((a) < (b) ? (a) : (b))
#define MAX_U(a, b) ((a) > (b) ? (a) : (b))

static const D3D11_INPUT_ELEMENT_DESC OITLayout[] =
{
	{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, (UINT)offsetof(Vertex, x), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    1, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, vtx_spc), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    2, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, col1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "COLOR",    3, DXGI_FORMAT_R8G8B8A8_UNORM,  0, (UINT)offsetof(Vertex, spc1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	{ "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,    0, (UINT)offsetof(Vertex, u1), D3D11_INPUT_PER_VERTEX_DATA, 0 },
	/* the polygon's number, from the numbers at vertex slot 2: the draw's first instance's */
	{ "POLYNUMBER", 0, DXGI_FORMAT_R32_UINT,      2, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
};

/* ---- The per-pixel lists ----
 *
 * Every translucent pixel drawn, in one large buffer (planned_size(), or as
 * large as the card will make it), and for each screen pixel the first of
 * its list.
 *
 * How many a frame asked for is counted by the shaders (the counter goes
 * up for a pixel that finds no room too) and read back without waiting:
 * after each band's render pass the counter is copied, on the GPU, into a
 * word of the frame's slot of a small ring of staging buffers, and the
 * oldest slot is mapped without waiting (D3D11_MAP_FLAG_DO_NOT_WAIT)
 * before a frame is drawn: a slot the GPU is still copying into is left
 * for later. Three frames back is the earliest a count is seen. */
#define COUNT_SLOTS 4
#define COUNT_WORDS 256

typedef struct oit_buffers
{
	u64 size;
	u32 room;
	ID3D11Buffer *count_staging[COUNT_SLOTS];
	u32 count_words[COUNT_SLOTS];	/* how many words of the slot were copied into */
	ID3D11Buffer *pixels;
	ID3D11UnorderedAccessView *pixels_view;
	ID3D11Buffer *counter;
	ID3D11UnorderedAccessView *counter_view;
	ID3D11Buffer *constants;
	ID3D11Texture2D *pointers;
	ID3D11UnorderedAccessView *pointers_view;
	u32 width;
	u32 height;
} oit_buffers;

/* For a screen of @width x @height: rend/oit_size.h, and no more than
 * a resource can be (2 GB) */
static u64 planned_size(int width, int height)
{
	return oit_buffer_bytes((u32)(width > 0 ? width : 0), (u32)(height > 0 ? height : 0),
			vram_budget_available_kib(&dx11c.vram_budget),
			(u64)D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u);
}

static void buffers_term(oit_buffers *b)
{
	u32 i;
	DX11_RELEASE(b->pointers_view);
	DX11_RELEASE(b->pointers);
	DX11_RELEASE(b->pixels_view);
	DX11_RELEASE(b->pixels);
	DX11_RELEASE(b->counter_view);
	DX11_RELEASE(b->counter);
	DX11_RELEASE(b->constants);
	for (i = 0; i < COUNT_SLOTS; i++)
	{
		DX11_RELEASE(b->count_staging[i]);
		b->count_words[i] = 0;
	}
	b->room = 0;
	b->width = b->height = 0;
}

static bool buffers_init(oit_buffers *b, ID3D11Device *device, u64 wanted)
{
	/* (the most a resource can be is a quarter of the card's memory, but
	 * at least 128 MB and at most 2 GB: it is asked for no larger than
	 * that, and halved until the card makes it, a whole number of
	 * pixels each time) */
	const UINT largest = D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_C_TERM * 1024u * 1024u;
	const UINT smallest = D3D11_REQ_RESOURCE_SIZE_IN_MEGABYTES_EXPRESSION_A_TERM * 1024u * 1024u;
	D3D11_BUFFER_DESC desc, counterDesc, constDesc, stagingDesc;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uav, counterUav;
	D3D11_SUBRESOURCE_DATA initial;
	u32 count[4];
	HRESULT hr;
	u32 i;

	DX11_RELEASE(b->pixels_view);
	DX11_RELEASE(b->pixels);
	DX11_RELEASE(b->counter_view);
	DX11_RELEASE(b->counter);
	DX11_RELEASE(b->constants);
	for (i = 0; i < COUNT_SLOTS; i++)
	{
		DX11_RELEASE(b->count_staging[i]);
		b->count_words[i] = 0;
	}
	b->room = 0;
	b->size = wanted;

	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = (UINT)(wanted < largest ? wanted : largest) & ~15u;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	/* sizeof(struct Pixel) of the shaders: an RGBA8 colour, the depth,
	 * the polygon's number and the next pixel, a uint each (OIT_FRAGMENT_BYTES) */
	desc.StructureByteStride = OIT_FRAGMENT_BYTES;
	hr = ID3D11Device_CreateBuffer(device, &desc, NULL, &b->pixels);
	while (FAILED(hr) && desc.ByteWidth > smallest)
	{
		desc.ByteWidth = MAX_U((desc.ByteWidth / 2) & ~15u, smallest);
		hr = ID3D11Device_CreateBuffer(device, &desc, NULL, &b->pixels);
	}
	if (FAILED(hr))
	{
		WARN_LOG(RENDERER, "DX11: pixel buffer creation failed (%x)\n", (unsigned)hr);
		b->pixels = NULL;
		return false;
	}
	if (desc.ByteWidth < b->size)
		NOTICE_LOG(RENDERER, "DX11: pixel buffer of %u MB\n", desc.ByteWidth / 1024 / 1024);

	memset(&uav, 0, sizeof(uav));
	uav.Format = DXGI_FORMAT_UNKNOWN;
	uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	uav.Buffer.FirstElement = 0;
	uav.Buffer.NumElements = desc.ByteWidth / desc.StructureByteStride;
	hr = ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)b->pixels, &uav, &b->pixels_view);
	if (FAILED(hr))
	{
		WARN_LOG(RENDERER, "DX11: pixel buffer view creation failed (%x)\n", (unsigned)hr);
		b->pixels_view = NULL;
		DX11_RELEASE(b->pixels);
		return false;
	}

	/* How many of them are taken, counted up by the shaders as the
	 * Vulkan renderer's are; and how many there are room for */
	memset(&counterDesc, 0, sizeof(counterDesc));
	counterDesc.ByteWidth = 16;
	counterDesc.Usage = D3D11_USAGE_DEFAULT;
	counterDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	counterDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	memset(&counterUav, 0, sizeof(counterUav));
	counterUav.Format = DXGI_FORMAT_R32_TYPELESS;
	counterUav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	counterUav.Buffer.NumElements = 4;
	counterUav.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	count[0] = uav.Buffer.NumElements;
	count[1] = count[2] = count[3] = 0;
	memset(&initial, 0, sizeof(initial));
	initial.pSysMem = count;
	memset(&constDesc, 0, sizeof(constDesc));
	constDesc.ByteWidth = 16;
	constDesc.Usage = D3D11_USAGE_IMMUTABLE;
	constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	if (FAILED(ID3D11Device_CreateBuffer(device, &counterDesc, NULL, &b->counter))
			|| FAILED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)b->counter, &counterUav, &b->counter_view))
			|| FAILED(ID3D11Device_CreateBuffer(device, &constDesc, &initial, &b->constants)))
	{
		WARN_LOG(RENDERER, "DX11: pixel counter creation failed\n");
		buffers_term(b);
		return false;
	}
	b->room = uav.Buffer.NumElements;

	/* the ring the counts are read back through */
	memset(&stagingDesc, 0, sizeof(stagingDesc));
	stagingDesc.ByteWidth = COUNT_WORDS * sizeof(u32);
	stagingDesc.Usage = D3D11_USAGE_STAGING;
	stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (i = 0; i < COUNT_SLOTS; i++)
		if (FAILED(ID3D11Device_CreateBuffer(device, &stagingDesc, NULL, &b->count_staging[i])))
		{
			WARN_LOG(RENDERER, "DX11: pixel count staging buffer creation failed\n");
			buffers_term(b);
			return false;
		}
	return true;
}

/* The counter, as the draws before left it, copied on the GPU into
 * @word of @slot: nothing waits for it */
static void buffers_copy_count(oit_buffers *b, ID3D11DeviceContext *ctx, u32 slot, u32 word)
{
	D3D11_BOX box;
	if (slot >= COUNT_SLOTS || word >= COUNT_WORDS)
		return;
	box.left = 0;
	box.top = 0;
	box.front = 0;
	box.right = sizeof(u32);
	box.bottom = 1;
	box.back = 1;
	ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)b->count_staging[slot], 0, word * sizeof(u32), 0, 0,
			(ID3D11Resource *)b->counter, 0, &box);
	if (word >= b->count_words[slot])
		b->count_words[slot] = word + 1;
}

/* The counts @slot holds, read if the GPU is done copying them, and
 * the largest of them in @largest: false when it is not done, or when
 * nothing was copied there. Never waits. */
static bool buffers_read_count(oit_buffers *b, ID3D11DeviceContext *ctx, u32 slot, u32 *largest)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	const u32 *words;
	HRESULT hr;
	u32 i;
	if (slot >= COUNT_SLOTS || b->count_words[slot] == 0 || b->count_staging[slot] == NULL)
		return false;
	hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b->count_staging[slot], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
	if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
		return false;
	if (FAILED(hr))
	{
		/* (a slot that cannot be read is given up on, not waited for) */
		b->count_words[slot] = 0;
		return false;
	}
	words = (const u32 *)mapped.pData;
	*largest = 0;
	for (i = 0; i < b->count_words[slot]; i++)
		*largest = MAX_U(*largest, words[i]);
	ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b->count_staging[slot], 0);
	b->count_words[slot] = 0;
	return true;
}

static bool buffers_has_count(const oit_buffers *b, u32 slot)
{
	return slot < COUNT_SLOTS && b->count_words[slot] != 0;
}

/* The first pixels of the lists, for a picture of at least this size.
 * They start as no list at all, and the pass that puts the lists
 * together leaves them that way. */
static bool buffers_resize(oit_buffers *b, ID3D11Device *device, u32 width, u32 height)
{
	D3D11_TEXTURE2D_DESC desc;
	D3D11_SUBRESOURCE_DATA initial;
	u32 *none;
	size_t i, n;
	bool ok;
	if (b->pointers != NULL && width <= b->width && height <= b->height)
		return true;
	width = MAX_U(width, b->width);
	height = MAX_U(height, b->height);
	DX11_RELEASE(b->pointers_view);
	DX11_RELEASE(b->pointers);
	b->width = b->height = 0;

	n = (size_t)width * height;
	none = (u32 *)malloc(n * sizeof(u32));
	if (none == NULL)
		return false;
	for (i = 0; i < n; i++)
		none[i] = 0xffffffffu;
	memset(&desc, 0, sizeof(desc));
	desc.Width = width;
	desc.Height = height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R32_UINT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	memset(&initial, 0, sizeof(initial));
	initial.pSysMem = none;
	initial.SysMemPitch = width * sizeof(u32);
	ok = SUCCEEDED(ID3D11Device_CreateTexture2D(device, &desc, &initial, &b->pointers));
	if (!ok)
		b->pointers = NULL;
	ok = ok && SUCCEEDED(ID3D11Device_CreateUnorderedAccessView(device, (ID3D11Resource *)b->pointers, NULL, &b->pointers_view));
	free(none);
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX11: per-pixel list heads creation failed (%d x %d)\n", width, height);
		b->pointers_view = NULL;
		DX11_RELEASE(b->pointers);
		return false;
	}
	b->width = width;
	b->height = height;
	return true;
}

/* With one render target, the lists at u1, u2 and u3, and how large
 * they can be at b2 */
static void buffers_bind(oit_buffers *b, ID3D11RenderTargetView *target, ID3D11DepthStencilView *depth)
{
	ID3D11UnorderedAccessView *views[3];
	views[0] = b->pixels_view;
	views[1] = b->pointers_view;
	views[2] = b->counter_view;
	dx11c_set_targets_and_uavs(target, depth, views);
	dx11c_set_ps_constants(2, b->constants);
}

/* The buffer starts filling from its beginning again */
static void buffers_reset_count(oit_buffers *b, ID3D11DeviceContext *ctx)
{
	static const UINT zero[4] = { 0, 0, 0, 0 };
	ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, b->counter_view, zero);
}

static bool buffers_valid(const oit_buffers *b)
{
	return b->pixels_view != NULL && b->pointers_view != NULL && b->counter_view != NULL;
}

/* ---- The renderer ---- */

typedef struct OITPolyConstants
{
	float clipTest[4];
	int blend_mode0[4];
	float trilinearAlpha;
	int pp_Number;
	float palette_index;
	u32 polyKey;	/* the general pixel shader's: the polygon's key (dx11_oitshaders.h) */

	/* two volume mode */
	int blend_mode1[4];
	int shading_instr0;
	int shading_instr1;
	int fog_control0;
	int fog_control1;
	int use_alpha0;
	int use_alpha1;
	int ignore_tex_alpha0;
	int ignore_tex_alpha1;
} OITPolyConstants;

typedef char dx11_oit_constants_check[sizeof(OITPolyConstants) % 16 == 0 ? 1 : -1];

typedef struct oit_picture
{
	ID3D11Texture2D *texture;
	ID3D11RenderTargetView *target;
	ID3D11ShaderResourceView *view;
} oit_picture;

#define MAX_BANDS 64

static struct
{
	oit_buffers buffers;
	ID3D11InputLayout *input_layout;
	ID3D11Buffer *constants;
	OITPolyConstants last_constants;
	bool last_constants_valid;
	ID3D11Buffer *tr_poly_params;
	ID3D11ShaderResourceView *tr_poly_params_view;
	u32 tr_poly_params_size;
	/* The numbers 0, 1, 2... as many as there are translucent polygons:
	 * a polygon's number is read from here as the instance it is drawn as,
	 * and its constants are then the same as the polygon's before more
	 * often than not */
	ID3D11Buffer *poly_numbers;
	u32 poly_numbers_count;
	/* What the opaque and punch-through polygons are drawn into, two of
	 * them: with several render passes, each draws on what the last one
	 * put together */
	oit_picture pictures[2];
	u32 pictures_width;
	u32 pictures_height;
	/* A frame drawn in horizontal bands (oit_draw_frame()): the band being
	 * drawn, in pixels of the target - from its top to its bottom, which is
	 * 0 when the frame is drawn whole */
	LONG band_top, band_bottom;
	/* The counts (oit_buffers) of the frames in flight: the slot the
	 * frame being drawn copies into, and the bands each slot's frame was
	 * drawn in and whether it was a render to a texture */
	u32 count_head;
	u32 count_bands[COUNT_SLOTS];
	bool count_rtt[COUNT_SLOTS];
	/* how many bands frames are drawn in: the screen's [0], renders to a texture [1] */
	u32 bands[2];
	u32 roomy_frames[2];
} oit;

static void release_picture(oit_picture *p)
{
	DX11_RELEASE(p->view);
	DX11_RELEASE(p->target);
	DX11_RELEASE(p->texture);
}

/* Every scissor rectangle the frame's draws set, cut to the band being drawn */
static void set_scissor(const D3D11_RECT *rect)
{
	if (oit.band_bottom != 0)
	{
		D3D11_RECT cut = *rect;
		cut.top = rect->top > oit.band_top ? rect->top : oit.band_top;
		cut.bottom = rect->bottom < oit.band_bottom ? rect->bottom : oit.band_bottom;
		if (cut.bottom < cut.top)
			cut.bottom = cut.top;
		dx11c_set_scissor(&cut);
	}
	else
		dx11c_set_scissor(rect);
}

static void set_scissor_whole(void)
{
	D3D11_RECT r;
	r.left = 0;
	r.top = 0;
	r.right = (LONG)dx11r.target_width;
	r.bottom = (LONG)dx11r.target_height;
	set_scissor(&r);
}

static ID3D11DepthStencilState *depth_state(bool depthTest, bool depthWrite, int depthFunc, DX11Stencil stencil)
{
	return dx11_depth_states_get(&dx11r.depth_states, depthTest, depthWrite, depthFunc, stencil);
}

/* the depth buffer's depth or stencil, for the shaders to read */
static void set_t4(ID3D11ShaderResourceView *view)
{
	dx11c_set_ps_resource(4, view);
}

/* The bands this frame is drawn in, from the counts of the oldest frame
 * whose copies the GPU is done with: never this frame's, which nothing
 * waits for. More at once when that one had pixels that found no room;
 * fewer when a second of frames in a row would have fitted in fewer. Each
 * band is taken to need what the fullest one did, with a quarter more
 * room. The screen's frames and renders to a texture are counted apart. */
static u32 choose_bands(bool rtt)
{
	const u32 oldest = (oit.count_head + 1) % COUNT_SLOTS;	/* when all are in flight */
	u32 k;
	for (k = 0; k < COUNT_SLOTS; k++)
	{
		const u32 slot = (oldest + k) % COUNT_SLOTS;
		u32 fragments, drawnIn, wanted, was;
		u64 room, frame, w;
		u32 *bands, *roomy;
		if (!buffers_has_count(&oit.buffers, slot))
			continue;
		if (!buffers_read_count(&oit.buffers, dx11r.ctx, slot, &fragments))
		{
			if (buffers_has_count(&oit.buffers, slot))
				/* (still being copied: the ones after it are younger) */
				break;
			continue;
		}
		drawnIn = MAX_U(MIN_U(oit.count_bands[slot], MAX_BANDS), 1u);
		room = MAX_U((u64)oit.buffers.room, 1u);
		frame = (u64)fragments * drawnIn;
		w = (frame + frame / 4 + room - 1) / room;
		wanted = (u32)MAX_U(MIN_U(w, (u64)MAX_BANDS), 1u);
		bands = &oit.bands[oit.count_rtt[slot] ? 1 : 0];
		roomy = &oit.roomy_frames[oit.count_rtt[slot] ? 1 : 0];
		was = *bands;
		if (fragments > room)
		{
			u32 most = MAX_U(*bands, wanted);
			most = MAX_U(most, drawnIn + 1);
			*bands = MIN_U(most, MAX_BANDS);
			*roomy = 0;
		}
		else if (wanted < *bands)
		{
			if (++*roomy >= 60)
			{
				*bands = wanted;
				*roomy = 0;
			}
		}
		else
			*roomy = 0;
		if (*bands != was)
			NOTICE_LOG(RENDERER, "DX11 per-pixel renderer: %u translucent pixels in a band of %u%s, room for %u: %u band%s\n",
					fragments, drawnIn, oit.count_rtt[slot] ? " (a render to a texture)" : "", (u32)room, *bands, *bands > 1 ? "s" : "");
		break;
	}
	return oit.bands[rtt ? 1 : 0];
}

static bool make_pictures(u32 width, u32 height)
{
	/* (made again for another size of screen; a render to a texture is not
	 * one, and a frame that wants more is drawn in bands) */
	const u64 wanted = planned_size(screen_width, screen_height);
	u32 i;
	if (wanted != oit.buffers.size && !buffers_init(&oit.buffers, dx11r.device, wanted))
		return false;
	dx11r_common_render(&width, &height);
	if (!buffers_resize(&oit.buffers, dx11r.device, width, height))
		return false;
	if (oit.pictures[0].texture != NULL && width <= oit.pictures_width && height <= oit.pictures_height)
		return true;
	width = MAX_U(width, oit.pictures_width);
	height = MAX_U(height, oit.pictures_height);
	oit.pictures_width = oit.pictures_height = 0;
	for (i = 0; i < 2; i++)
	{
		oit_picture *picture = &oit.pictures[i];
		D3D11_TEXTURE2D_DESC desc;
		release_picture(picture);
		memset(&desc, 0, sizeof(desc));
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
		if (FAILED(ID3D11Device_CreateTexture2D(dx11r.device, &desc, NULL, &picture->texture)))
			picture->texture = NULL;
		else if (FAILED(ID3D11Device_CreateRenderTargetView(dx11r.device, (ID3D11Resource *)picture->texture, NULL, &picture->target)))
			picture->target = NULL;
		else if (FAILED(ID3D11Device_CreateShaderResourceView(dx11r.device, (ID3D11Resource *)picture->texture, NULL, &picture->view)))
			picture->view = NULL;
		if (picture->view == NULL)
		{
			WARN_LOG(RENDERER, "DX11: per-pixel renderer picture creation failed (%d x %d)\n", width, height);
			release_picture(picture);
			return false;
		}
	}
	oit.pictures_width = width;
	oit.pictures_height = height;
	return dx11r_ensure_depth(width, height);
}

/* What the shaders that put the lists together need to know of each
 * translucent polygon: its TSP, depth function and mask, whether it takes
 * shadows, and its second volume's TSP */
static bool upload_tr_poly_params(void)
{
	const int count = TA_LIST_USED(pvrrc.global_param_tr, PolyParam);
	const u32 size = (u32)((count > 1 ? count : 1) * 2 * sizeof(u32));
	D3D11_MAPPED_SUBRESOURCE mapped;
	const PolyParam *pp;
	u32 *words;
	int i;
	/* (a frame decoded on the GPU: its words are the decode's, and its
	 * strips' numbers the decoder's) */
	if (pvrrc.gpu)
	{
		const dx11_ta_draw *gpu = dx11_ta_get_draw();
		dx11c_set_ps_resource(5, gpu->trParams);
		dx11c_set_vertex_buffer(gpu->instances, sizeof(u32), 2);
		return true;
	}
	if (oit.tr_poly_params == NULL || oit.tr_poly_params_size < size)
	{
		u32 newSize = 4096;
		D3D11_BUFFER_DESC desc;
		D3D11_SHADER_RESOURCE_VIEW_DESC view;
		DX11_RELEASE(oit.tr_poly_params_view);
		DX11_RELEASE(oit.tr_poly_params);
		oit.tr_poly_params_size = 0;
		while (newSize < size)
			newSize *= 2;
		memset(&desc, 0, sizeof(desc));
		desc.ByteWidth = newSize;
		desc.Usage = D3D11_USAGE_DYNAMIC;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		/* (a pair of words a polygon, as the shaders read them: TrPolyParamWords) */
		memset(&view, 0, sizeof(view));
		view.Format = DXGI_FORMAT_R32G32_UINT;
		view.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		view.Buffer.NumElements = newSize / (sizeof(u32) * 2);
		if (FAILED(ID3D11Device_CreateBuffer(dx11r.device, &desc, NULL, &oit.tr_poly_params)))
			oit.tr_poly_params = NULL;
		else if (FAILED(ID3D11Device_CreateShaderResourceView(dx11r.device, (ID3D11Resource *)oit.tr_poly_params, &view,
					&oit.tr_poly_params_view)))
			oit.tr_poly_params_view = NULL;
		if (oit.tr_poly_params_view == NULL)
		{
			WARN_LOG(RENDERER, "DX11: translucent polygon parameters buffer creation failed\n");
			DX11_RELEASE(oit.tr_poly_params);
			return false;
		}
		oit.tr_poly_params_size = newSize;
	}
	if (FAILED(ID3D11DeviceContext_Map(dx11r.ctx, (ID3D11Resource *)oit.tr_poly_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	/* (written where it is mapped, in order: a pair of words a polygon, or
	 * one of zeros for none) */
	words = (u32 *)mapped.pData;
	if (count == 0)
		words[0] = words[1] = 0;
	pp = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	for (i = 0; i < count; i++, pp++, words += 2)
	{
		words[0] = (pp->tsp & 0xffff00c0) | ((pp->isp >> 16) & 0xe400) | ((pp->pcw >> 7) & 1);
		words[1] = pp->tsp1;
	}
	ID3D11DeviceContext_Unmap(dx11r.ctx, (ID3D11Resource *)oit.tr_poly_params, 0);
	dx11c_set_ps_resource(5, oit.tr_poly_params_view);

	if (oit.poly_numbers == NULL || oit.poly_numbers_count < (u32)count)
	{
		u32 newCount = 4096, n;
		u32 *numbers;
		D3D11_BUFFER_DESC desc;
		D3D11_SUBRESOURCE_DATA initial;
		DX11_RELEASE(oit.poly_numbers);
		oit.poly_numbers_count = 0;
		while (newCount < (u32)count)
			newCount *= 2;
		numbers = (u32 *)malloc(newCount * sizeof(u32));
		if (numbers == NULL)
			return false;
		for (n = 0; n < newCount; n++)
			numbers[n] = n;
		memset(&desc, 0, sizeof(desc));
		desc.ByteWidth = newCount * sizeof(u32);
		desc.Usage = D3D11_USAGE_IMMUTABLE;
		desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		memset(&initial, 0, sizeof(initial));
		initial.pSysMem = numbers;
		if (FAILED(ID3D11Device_CreateBuffer(dx11r.device, &desc, &initial, &oit.poly_numbers)))
		{
			WARN_LOG(RENDERER, "DX11: polygon number buffer creation failed\n");
			oit.poly_numbers = NULL;
			free(numbers);
			return false;
		}
		free(numbers);
		oit.poly_numbers_count = newCount;
	}
	dx11c_set_vertex_buffer(oit.poly_numbers, sizeof(u32), 2);
	return true;
}

static void draw_poly(u32 listType, bool autosort, dx11_oit_pass pass, const PolyParam *poly, u32 first, u32 count)
{
	D3D11_RECT scissorRect;
	const int tileClip = dx11r_set_tile_clip(poly->tileclip, &scissorRect);
	/* The depth pass of a polygon whose pixels are kept whatever their
	 * colour - any but a punch-through one, which is tested on its alpha -
	 * writes the depth and the stencil only: what only the colour is made
	 * from is not needed for it, the texture and the constants but for a
	 * clip test inside the box. (The shader is the one the polygon has,
	 * all the same: the colour pass draws it where its depth is the one
	 * this pass wrote, to the last bit, and another shader is another
	 * compiler's work, which need not come out the same to the last bit.) */
	const bool depthOnly = pass == DX11OIT_DEPTH && listType != ListType_Punch_Through;
	const bool insideClipTest = (poly->tileclip >> 28) == 3;
	const bool twoVolumes = poly->tsp1 != (u32)-1 || poly->tcw1 != (u32)-1;
	const bool keepAll = twoVolumes || PPW(poly->tsp, TSP_SrcSelect);
	const bool palette = tex_gpu_paletted(poly->tsp, poly->tcw, 0);
	const bool colourPassWrites = pass == DX11OIT_COLOR && !autosort && listType == ListType_Translucent;
	float trilinearAlpha = 1.f, palette_index = 0.f;
	OITPolyConstants constants;
	dx11_oit_ps_params params;
	bool general, depthWrite;
	int depthFunc;

	if (tileClip == TILECLIP_OUTSIDE)
		set_scissor(&scissorRect);
	else
		set_scissor(&dx11r.base_scissor);

	if (PPW(poly->tsp, TSP_FilterMode) > 1 && PPW(poly->pcw, PCW_Texture) && listType != ListType_Punch_Through
			&& PPW(poly->tcw, TCW_MipMapped) == 1)
	{
		trilinearAlpha = 0.25f * (PPW(poly->tsp, TSP_MipMapD) & 0x3);
		if (PPW(poly->tsp, TSP_FilterMode) == 2)
			/* Trilinear pass A */
			trilinearAlpha = 1.f - trilinearAlpha;
	}
	if (palette)
	{
		if (PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_4BPP)
			palette_index = ((float)(PPW(poly->tcw, TCW_PalSelect) << 4) + 0.5f) / 1024.f;
		else
			palette_index = ((float)((PPW(poly->tcw, TCW_PalSelect) >> 4) << 8) + 0.5f) / 1024.f;
	}

	memset(&constants, 0, sizeof(constants));
	constants.clipTest[0] = (float)scissorRect.left;
	constants.clipTest[1] = (float)scissorRect.top;
	constants.clipTest[2] = (float)scissorRect.right;
	constants.clipTest[3] = (float)scissorRect.bottom;
	/* The per-pixel pass drops a fragment whose blend would leave the
	 * pixel as it is. It cannot tell that when the source is the second
	 * accumulation buffer, or when the polygon has a second volume and
	 * which blend applies is known only when the lists are put
	 * together: those get a blend (8) that none of its cases take. */
	constants.blend_mode0[0] = keepAll ? 8 : (int)PPW(poly->tsp, TSP_SrcInstr);
	constants.blend_mode0[1] = keepAll ? 8 : (int)PPW(poly->tsp, TSP_DstInstr);
	constants.trilinearAlpha = trilinearAlpha;
	/* (pp_Number is not used: the instance's number is the polygon's) */
	constants.palette_index = palette_index;
	if (twoVolumes)
	{
		constants.blend_mode1[0] = (int)PPW(poly->tsp1, TSP_SrcInstr);
		constants.blend_mode1[1] = (int)PPW(poly->tsp1, TSP_DstInstr);
		constants.shading_instr0 = (int)PPW(poly->tsp, TSP_ShadInstr);
		constants.shading_instr1 = (int)PPW(poly->tsp1, TSP_ShadInstr);
		constants.fog_control0 = (int)PPW(poly->tsp, TSP_FogCtrl);
		constants.fog_control1 = (int)PPW(poly->tsp1, TSP_FogCtrl);
		constants.use_alpha0 = (int)PPW(poly->tsp, TSP_UseAlpha);
		constants.use_alpha1 = (int)PPW(poly->tsp1, TSP_UseAlpha);
		constants.ignore_tex_alpha0 = (int)PPW(poly->tsp, TSP_IgnoreTexA);
		constants.ignore_tex_alpha1 = (int)PPW(poly->tsp1, TSP_IgnoreTexA);
	}

	if (PPW(poly->pcw, PCW_Texture) && !depthOnly)
	{
		if (poly->texid != 0)
			dx11r_set_texture(0, ((const dx11_texture *)(uintptr_t)poly->texid)->texture_view, dx11_sampler_get(poly->tsp));
		if (twoVolumes && poly->texid1 != 0)
			dx11r_set_texture(3, ((const dx11_texture *)(uintptr_t)poly->texid1)->texture_view, dx11_sampler_get(poly->tsp1));
	}

	memset(&params, 0, sizeof(params));
	params.alphaTest = listType == ListType_Punch_Through;
	params.bumpmap = PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_BUMPMAP;
	params.clamping = PPW(poly->tsp, TSP_ColorClamp) != 0;
	params.insideClipTest = insideClipTest;
	params.fog = (int)PPW(poly->tsp, TSP_FogCtrl);
	params.gouraud = PPW(poly->pcw, PCW_Gouraud) != 0;
	params.ignoreTexAlpha = PPW(poly->tsp, TSP_IgnoreTexA) || PPW(poly->tcw, TCW_PixelFmt) == TA_PIXEL_565;
	params.offset = PPW(poly->pcw, PCW_Offset) != 0;
	params.shaderInstr = (int)PPW(poly->tsp, TSP_ShadInstr);
	params.texture = PPW(poly->pcw, PCW_Texture) != 0;
	params.useAlpha = PPW(poly->tsp, TSP_UseAlpha) != 0;
	params.pass = pass;
	params.twoVolume = twoVolumes;
	params.palette = palette;
	dx11c_set_vs(dx11_oit_shaders_vs(params.gouraud));
	dx11c_set_ps(dx11_oit_shaders_ps(&params, &general));
	/* (the general shader's key is the polygon's: in its constants) */
	constants.polyKey = general ? dx11_oit_shaders_key(&params) : 0u;
	if ((!depthOnly || insideClipTest || general)
			&& (!oit.last_constants_valid || memcmp(&constants, &oit.last_constants, sizeof(constants))))
	{
		dx11r_set_constants(oit.constants, &constants);
		oit.last_constants = constants;
		oit.last_constants_valid = true;
	}

	/* Depth and stencil */
	if (pass == DX11OIT_COLOR && !PPW(poly->isp, ISP_TSP_ZWriteDis) && listType != ListType_Translucent)
		depthFunc = 2;	/* equal */
	else if (listType == ListType_Punch_Through || autosort)
		depthFunc = 6;	/* greater or equal */
	else
		depthFunc = (int)PPW(poly->isp, ISP_TSP_DepthMode);
	/* Depth is written in the depth pass. In the colour pass opaque and
	 * punch-through polygons are drawn where their depth is the one the
	 * depth pass left, and translucent ones that are sorted only into the
	 * lists; the translucent polygons that are not sorted are drawn there,
	 * in order, each tested against the depth the ones before it wrote, so
	 * they still write. */
	if (pass != DX11OIT_DEPTH && !colourPassWrites)
		depthWrite = false;
	/* Z Write Disable seems to be ignored for punch-through.
	 * Fixes Worms World Party, Bust-a-Move 4 and Re-Volt */
	else if (listType == ListType_Punch_Through)
		depthWrite = true;
	else
		depthWrite = !PPW(poly->isp, ISP_TSP_ZWriteDis);

	if (pass == DX11OIT_DEPTH && (listType == ListType_Opaque || listType == ListType_Punch_Through))
		dx11c_set_depth_stencil(depth_state(true, depthWrite, depthFunc, DX11Stencil_Shadow),
				PPW(poly->pcw, PCW_Shadow) != 0 ? 0x80 : 0);
	else
		dx11c_set_depth_stencil(depth_state(true, depthWrite, depthFunc, DX11Stencil_None), 0);

	/* Apparently punch-through polys support blending, or at least some combinations */
	if (listType == ListType_Punch_Through || pass == DX11OIT_COLOR)
		dx11c_set_blend(dx11_blend_get(true, PPW(poly->tsp, TSP_SrcInstr), PPW(poly->tsp, TSP_DstInstr), true), NULL);
	else
		dx11c_set_blend(dx11_blend_get(false, 1, 0, false), NULL);

	dx11r_set_cull_mode((int)PPW(poly->isp, ISP_TSP_CullMode));
	dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

	/* (a frame decoded on the GPU: a draw group by its arguments there, a translucent strip's number its record's instance) */
	if (pvrrc.gpu)
	{
		dx11r_draw_gpu(listType, false, first, count);
		return;
	}
	ID3D11DeviceContext_DrawIndexedInstanced(dx11r.ctx, count, 1, dx11r.index_base + first, dx11r.vertex_base,
			listType == ListType_Translucent ? (UINT)(poly - TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam)) : 0);
}

static void draw_list(u32 listType, bool autosort, dx11_oit_pass pass, const PolyParam *polys, u32 first, u32 last)
{
	const PolyParam *pp_end = polys + last;
	const PolyParam *pp;
	for (pp = polys + first; pp != pp_end; pp++)
		if (pp->count > 2)
			draw_poly(listType, autosort, pass, pp, pp->first, pp->count);
}

static void draw_modifier_volumes(bool translucent, int first, int count, const ModifierVolumeParam *modVolParams)
{
	const ModifierVolumeParam *params;
	int mod_base = -1, cmv;
	if (count == 0 || !dx11r.have_modvols)
		return;

	/* (the modifier volumes' triangles are at vertex slot 1) */
	dx11c_set_input_layout(dx11r.modvol_layout);
	dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	dx11c_set_vs(dx11_oit_shaders_modvol_vs());
	dx11c_set_blend(dx11_blend_get(false, 1, 0, false), NULL);
	if (translucent)
		/* (the lists are marked by the shaders, and nothing else is tested or written) */
		dx11c_set_depth_stencil(depth_state(false, false, 7, DX11Stencil_None), 0);
	else
		dx11c_set_ps(dx11_oit_shaders_modvol_ps());
	set_scissor(&dx11r.base_scissor);

	params = &modVolParams[first];

	for (cmv = 0; cmv < count; cmv++)
	{
		const ModifierVolumeParam *param = &params[cmv];
		u32 mvFirst, mvCount, mv_mode;
		D3D11_RECT clipRect;
		mv_clip_range_get(&pvrrc, modVolParams, param, &mvFirst, &mvCount);

		if (mvCount == 0)
			continue;

		mv_mode = PPW(param->isp, ISP_Modvol_DepthMode);

		/* clipped like a polygon, to the rectangle it was sent under if it asked to be */
		if (dx11r_set_tile_clip(param->tileclip, &clipRect) == TILECLIP_OUTSIDE)
			set_scissor(&clipRect);
		else
			set_scissor(&dx11r.base_scissor);

		if (mod_base == -1)
			mod_base = (int)mvFirst;

		dx11r_set_cull_mode((int)PPW(param->isp, ISP_Modvol_CullMode));
		if (!PPW(param->isp, ISP_Modvol_VolumeLast) && mv_mode > 0)
		{
			/* OR'ing (open volume or quad) */
			if (translucent)
				dx11c_set_ps(dx11_oit_shaders_tr_modvol_ps(DX11OIT_OR));
			else
				dx11c_set_depth_stencil(depth_state(true, false, 4, DX11Stencil_Or), 2);
		}
		else
		{
			/* XOR'ing (closed volume) */
			if (translucent)
				dx11c_set_ps(dx11_oit_shaders_tr_modvol_ps(DX11OIT_XOR));
			else
				dx11c_set_depth_stencil(depth_state(true, false, 4, DX11Stencil_Xor), 2);
		}
		ID3D11DeviceContext_Draw(dx11r.ctx, mvCount * 3, dx11r.modvol_base + mvFirst * 3);

		if (mv_mode == 1 || mv_mode == 2)
		{
			/* Sum the area */
			if (translucent)
				dx11c_set_ps(dx11_oit_shaders_tr_modvol_ps(mv_mode == 1 ? DX11OIT_INCLUSION : DX11OIT_EXCLUSION));
			else
				dx11c_set_depth_stencil(depth_state(false, false, 7, mv_mode == 1 ? DX11Stencil_Inclusion : DX11Stencil_Exclusion), 1);
			ID3D11DeviceContext_Draw(dx11r.ctx, (mvFirst + mvCount - mod_base) * 3, dx11r.modvol_base + mod_base * 3);
			mod_base = -1;
		}
	}
	set_scissor(&dx11r.base_scissor);
	dx11c_set_input_layout(oit.input_layout);
}

/* The lists put together over the opaque polygons' picture, into @target:
 * the whole picture, not the part the game clips its drawing to - what is
 * outside that has to be written too */
static void resolve(ID3D11RenderTargetView *target, ID3D11ShaderResourceView *opaque)
{
	buffers_bind(&oit.buffers, target, NULL);
	dx11r_set_texture(0, opaque, dx11_sampler_point());
	dx11c_set_vs(dx11_oit_shaders_final_vs());
	dx11c_set_ps(dx11_oit_shaders_final_ps());
	dx11c_set_blend(dx11_blend_get(false, 1, 0, true), NULL);
	dx11c_set_depth_stencil(depth_state(false, false, 7, DX11Stencil_None), 0);
	dx11r_set_cull_mode(0);
	set_scissor_whole();
	dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	dx11c_set_input_layout(NULL);
	ID3D11DeviceContext_Draw(dx11r.ctx, 4, 0);
	/* (the picture read from is drawn into next) */
	dx11c_set_ps_resource(0, NULL);
	dx11c_set_input_layout(oit.input_layout);
}

static void oit_draw_frame(void)
{
	ID3D11DeviceContext *ctx = dx11r.ctx;
	const RenderPass *passes = TA_LIST_HEAD(pvrrc.render_passes, RenderPass);
	const int render_pass_count = TA_LIST_USED(pvrrc.render_passes, RenderPass);
	const PolyParam *op = TA_LIST_HEAD(pvrrc.global_param_op, PolyParam);
	const PolyParam *pt = TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam);
	const PolyParam *tr = TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam);
	const ModifierVolumeParam *mvo = TA_LIST_HEAD(pvrrc.global_param_mvo, ModifierVolumeParam);
	const ModifierVolumeParam *mvo_tr = TA_LIST_HEAD(pvrrc.global_param_mvo_tr, ModifierVolumeParam);
	float clear[4] = { 0.f, 0.f, 0.f, 1.f };
	u32 bands, bandHeight, bandCount, bandIndex, countWord = 0, height1;

	if (!make_pictures(dx11r.target_width, dx11r.target_height) || !upload_tr_poly_params() || !buffers_valid(&oit.buffers))
		return;
	dx11c_set_viewport((float)dx11r.target_width, (float)dx11r.target_height);

	/* A framebuffer the game has not drawn to lately starts as the border
	 * colour, which is what the OpenGL renderers clear it to (never a
	 * render to a texture: that is not such a framebuffer); any other
	 * starts black. */
	if (pvrrc.clearFramebuffer)
	{
		clear[0] = VO_BORDER_COL.Red / 255.f;
		clear[1] = VO_BORDER_COL.Green / 255.f;
		clear[2] = VO_BORDER_COL.Blue / 255.f;
	}
	ID3D11DeviceContext_ClearRenderTargetView(ctx, oit.pictures[0].target, clear);
	if (pvrrc.isRTT)
		/* (what is outside the pixel clip, as the per-triangle renderer leaves it) */
		ID3D11DeviceContext_ClearRenderTargetView(ctx, dx11r.target_view, clear);
	ID3D11DeviceContext_ClearDepthStencilView(ctx, dx11r.depth_view, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.f, 0);
	set_t4(NULL);

	/* In bands, when the frames before had more translucent pixels than the
	 * lists have room for (choose_bands()): all of the frame's render passes
	 * once a band, every draw cut to the band by the scissor (set_scissor()),
	 * each band's lists starting empty and put on the picture before the
	 * next band's. Nothing is dropped and nothing is allocated; the opaque
	 * polygons are drawn once a band. (The clears above are of the whole
	 * target: a band's draws touch its own pixels alone, so the others'
	 * depth is still clear when their band comes. Between two draws,
	 * Direct3D 11 orders what they do to the lists: a band's polygons do
	 * not write the entries the band before is still reading, and its
	 * lists are not linked from before the pass before set the heads back
	 * to nothing - no barrier is needed, as it is in Vulkan and OpenGL.) */
	height1 = MAX_U(dx11r.target_height, 1u);
	bands = MAX_U(MIN_U(choose_bands(pvrrc.isRTT), height1), 1u);
	bandHeight = (dx11r.target_height + bands - 1) / bands;
	bandCount = MAX_U((dx11r.target_height + bandHeight - 1) / bandHeight, 1u);
	if (oit.count_head < COUNT_SLOTS)
		oit.buffers.count_words[oit.count_head] = 0;

	for (bandIndex = 0; bandIndex < bandCount; bandIndex++)
	{
		int cur = 0, render_pass;
		RenderPass previous_pass;
		oit.band_top = (LONG)(bandIndex * bandHeight);
		oit.band_bottom = bandCount > 1 ? (LONG)MIN_U((bandIndex + 1) * bandHeight, dx11r.target_height) : 0;
		memset(&previous_pass, 0, sizeof(previous_pass));
		for (render_pass = 0; render_pass < render_pass_count; render_pass++)
		{
			const RenderPass *current_pass = &passes[render_pass];
			const bool finalPass = render_pass == render_pass_count - 1;
			ID3D11RenderTargetView *target;

			DEBUG_LOG(RENDERER, "Render pass %d OP %d PT %d TR %d MV %d TrMV %d autosort %d\n", render_pass + 1,
					current_pass->op_count - previous_pass.op_count,
					current_pass->pt_count - previous_pass.pt_count,
					current_pass->tr_count - previous_pass.tr_count,
					current_pass->mvo_count - previous_pass.mvo_count,
					current_pass->mvo_tr_count - previous_pass.mvo_tr_count, current_pass->autosort);

			if (render_pass != 0)
				/* (each pass has its own volumes) */
				ID3D11DeviceContext_ClearDepthStencilView(ctx, dx11r.depth_view, D3D11_CLEAR_STENCIL, 0.f, 0);

			/* Depth and stencil. The pixel counter is reset: the lists are
			 * empty, and the buffer is filled from its start. */
			set_t4(NULL);
			buffers_reset_count(&oit.buffers, ctx);
			buffers_bind(&oit.buffers, oit.pictures[cur].target, dx11r.depth_view);
			draw_list(ListType_Opaque, false, DX11OIT_DEPTH, op, previous_pass.op_count, current_pass->op_count);
			draw_list(ListType_Punch_Through, false, DX11OIT_DEPTH, pt, previous_pass.pt_count, current_pass->pt_count);
			draw_modifier_volumes(false, previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count, mvo);

			/* Colour, the stencil read by the shaders */
			buffers_bind(&oit.buffers, oit.pictures[cur].target, dx11r.depth_view_read_stencil);
			set_t4(dx11r.stencil_srv);

			/* A render that covers only part of the screen leaves the rest as
			 * it was: the last picture goes in first, under everything */
			if (render_pass == 0 && !pvrrc.isRTT && dx11r_keeps_last_picture())
			{
				/* (the quad is drawn with the scissor that is set: the band's) */
				set_scissor_whole();
				dx11r_draw_last_picture(dx11r.target_width, dx11r.target_height);
				/* (the quad was drawn with its own) */
				dx11c_set_viewport((float)dx11r.target_width, (float)dx11r.target_height);
				dx11c_set_input_layout(oit.input_layout);
				dx11r_bind_geometry();
			}

			/* OP + PT */
			draw_list(ListType_Opaque, false, DX11OIT_COLOR, op, previous_pass.op_count, current_pass->op_count);
			draw_list(ListType_Punch_Through, false, DX11OIT_COLOR, pt, previous_pass.pt_count, current_pass->pt_count);

			/* TR. (In the first frame after the per-pixel lists are made, too:
			 * they start empty, and that frame's translucent polygons are
			 * drawn like any other's.) */
			if (current_pass->autosort)
			{
				/* into the lists, tested against the depth by the shaders as well */
				set_t4(NULL);
				buffers_bind(&oit.buffers, oit.pictures[cur].target, dx11r.depth_view_read_only);
				set_t4(dx11r.depth_srv);
				draw_list(ListType_Translucent, true, DX11OIT_OIT, tr, previous_pass.tr_count, current_pass->tr_count);
				/* how many pixels this band's pass asked for, copied out */
				buffers_copy_count(&oit.buffers, ctx, oit.count_head, countWord++);
			}
			else
				draw_list(ListType_Translucent, false, DX11OIT_COLOR, tr, previous_pass.tr_count, current_pass->tr_count);
			set_t4(NULL);

			/* The translucent modifier volumes, and the lists put together */
			target = finalPass ? dx11r.target_view : oit.pictures[cur ^ 1].target;
			buffers_bind(&oit.buffers, target, NULL);
			if (current_pass->mv_op_tr_shared)
				draw_modifier_volumes(true, previous_pass.mvo_count, current_pass->mvo_count - previous_pass.mvo_count, mvo);
			else
				draw_modifier_volumes(true, previous_pass.mvo_tr_count, current_pass->mvo_tr_count - previous_pass.mvo_tr_count, mvo_tr);
			resolve(target, oit.pictures[cur].view);

			if (!finalPass)
			{
				/* The translucent polygons' depth, for the passes after this one */
				buffers_bind(&oit.buffers, oit.pictures[cur].target, dx11r.depth_view);
				draw_list(ListType_Translucent, current_pass->autosort, DX11OIT_DEPTH, tr, previous_pass.tr_count, current_pass->tr_count);
				/* the next pass draws on what this one put together */
				cur ^= 1;
			}

			previous_pass = *current_pass;
		}
	}
	oit.band_top = oit.band_bottom = 0;
	/* (what is drawn on the picture after this is not cut to the last band) */
	set_scissor_whole();
	/* the frame's counts are in its slot: the next frame's go in the next */
	oit.count_bands[oit.count_head] = bandCount;
	oit.count_rtt[oit.count_head] = pvrrc.isRTT;
	oit.count_head = (oit.count_head + 1) % COUNT_SLOTS;
	/* (what is bound stays bound: the picture drawn into is unbound when
	 * it is handed over, and nothing that reads one binds it before
	 * binding another target) */
}

/* GPU-side TA processing: the translucent draw records number each strip
 * (TA_GPU_INSTANCE), which picks its number from the decoder's numbers as
 * a polygon's own does from poly_numbers; no sorting here */
static u32 oit_ta_flags(void)
{
	return TA_GPU_DRAW | TA_GPU_PROVOKE | TA_GPU_INSTANCE;
}

static ID3D11InputLayout *oit_poly_input_layout(void)
{
	return oit.input_layout;
}

static ID3D11Buffer *oit_poly_constants(void)
{
	return oit.constants;
}

static const dx11_renderer_kind oit_kind = {
	oit_draw_frame, false, false, oit_ta_flags, false, oit_poly_input_layout, oit_poly_constants
};

static void oit_term(void)
{
	u32 i;
	dx11c_lock();
	DX11_RELEASE(oit.tr_poly_params_view);
	DX11_RELEASE(oit.tr_poly_params);
	oit.tr_poly_params_size = 0;
	DX11_RELEASE(oit.poly_numbers);
	oit.poly_numbers_count = 0;
	for (i = 0; i < 2; i++)
		release_picture(&oit.pictures[i]);
	oit.pictures_width = oit.pictures_height = 0;
	oit.band_top = oit.band_bottom = 0;
	oit.count_head = 0;
	oit.bands[0] = oit.bands[1] = 1;
	oit.roomy_frames[0] = oit.roomy_frames[1] = 0;
	buffers_term(&oit.buffers);
	oit.buffers.size = 0;
	DX11_RELEASE(oit.constants);
	DX11_RELEASE(oit.input_layout);
	dx11_oit_shaders_term();
	dx11c_unlock();
	dx11r_term();
}

static bool oit_init(void)
{
	ID3DBlob *blob;
	D3D11_BUFFER_DESC desc;
	bool success;
	if (!dx11c_has_per_pixel())
	{
		/* (the per-triangle renderer is set up instead) */
		WARN_LOG(RENDERER, "DX11: the per-pixel renderer needs feature level 11.0 and a context that can be read back from\n");
		return false;
	}
	oit.bands[0] = oit.bands[1] = 1;
	if (!dx11r_init(&oit_kind))
		return false;

	dx11c_lock();
	dx11_oit_shaders_init(dx11r.device, dx11c.compile);
	/* (everything a frame can want, made now: AGENTS.md) */
	success = dx11_oit_shaders_start();
	blob = dx11_oit_shaders_vs_blob();
	success = success && blob != NULL
			&& SUCCEEDED(ID3D11Device_CreateInputLayout(dx11r.device, OITLayout, ARRAY_SIZE(OITLayout),
					ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &oit.input_layout));

	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = (sizeof(OITPolyConstants) + 15) & ~15u;
	/* (written whole with UpdateSubresource(), see dx11r_set_constants()) */
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	success = success && SUCCEEDED(ID3D11Device_CreateBuffer(dx11r.device, &desc, NULL, &oit.constants));

	success = success && buffers_init(&oit.buffers, dx11r.device, planned_size(screen_width, screen_height));
	oit.last_constants_valid = false;
	dx11c_unlock();
	if (!success)
	{
		WARN_LOG(RENDERER, "DX11 per-pixel renderer initialization failed\n");
		oit_term();
		return false;
	}
	return true;
}

static const renderer_ops dx11_oit_renderer_ops =
{
	oit_init,
	dx11r_resize,
	oit_term,
	dx11r_process,
	dx11r_render,
	dx11r_keep_picture,
	dx11r_get_texture,
	dx11r_options_changed,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

const renderer_ops *rend_OITD3D11(void)
{
	return &dx11_oit_renderer_ops;
}
