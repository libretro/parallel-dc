/* Textures made on the GPU from their bytes in video memory: see
 * dx9_texdecoder.h */
#include "dx9_texdecoder.h"
#include "dx9context_lr.h"
#define TEXD_SM3_KERNEL 0
#include "rend/texdecode_sm3.h"

/* The shaders, by TEXD_SM3_KERNEL, as bytecode made from rend/texdecode_sm3.h
 * when the core was built: tools/rend/kernels_dxbc.sh, which checks that
 * they still are its */
#include "rend/texdecode_dx9.inc"

#define MAX_JOBS 4096

typedef struct dx9_texdec_job
{
	IDirect3DSurface9 *surface;
	float k[16];		/* the shader's four constants */
	u32 width, height;
} dx9_texdec_job;

static struct
{
	bool ready;
	bool takes[5];
	IDirect3DTexture9 *ring;
	u32 ring_rows;
	u8 *mapped;
	u32 head;			/* where the next bytes go */
	IDirect3DVertexShader9 *vertex_shader;
	IDirect3DPixelShader9 *pixel_shader;
	IDirect3DVertexDeclaration9 *decl;

	dx9_texdec_job jobs[MAX_JOBS];
	u32 job_count;
	u32 serial;
	/* what has been done so far, for measuring */
	u64 stat_bytes;
	u64 stat_levels;
	u64 stat_full;
} dec = { false };

void dx9_texdec_init(void)
{
	IDirect3DDevice9 *device = dx9c.device;
	IDirect3D9 *d3d = NULL;
	D3DDEVICE_CREATION_PARAMETERS params;
	D3DDISPLAYMODE mode;
	D3DCAPS9 caps;
	bool ok;
	u64 bytes;
	int t;
	static const D3DVERTEXELEMENT9 elements[] = {
		{ 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
		D3DDECL_END()
	};

	dx9_texdec_term();
	if (!settings.rend.GpuTextureDecodeD3D9 || device == NULL)
		return;

	/* what can be drawn into */
	ok = SUCCEEDED(IDirect3DDevice9_GetDirect3D(device, &d3d)) && d3d != NULL
			&& SUCCEEDED(IDirect3DDevice9_GetCreationParameters(device, &params))
			&& SUCCEEDED(IDirect3D9_GetAdapterDisplayMode(d3d, params.AdapterOrdinal, &mode))
			&& SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(device, &caps));
	for (t = 0; ok && t < 5; t++)
		dec.takes[t] = dx9c_format_supported(t)
				&& IDirect3D9_CheckDeviceFormat(d3d, params.AdapterOrdinal, params.DeviceType, mode.Format,
						D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, dx9c_texture_format(t)) == D3D_OK;
	if (d3d != NULL)
		IDirect3D9_Release(d3d);

	/* The ring: what textures take as they are in video memory - a few
	 * hundred KB in most frames, megabytes when a scene is loaded - a share
	 * of the video memory there is, between 4 and 16 MB, rows of 4 KB */
	bytes = (u64)vram_budget_available_kib(&dx9c.vram_budget) * 1024 / 256;
	if (bytes < (4 << 20))
		bytes = 4 << 20;
	if (bytes > (16 << 20))
		bytes = 16 << 20;
	dec.ring_rows = (u32)(bytes / (TEXD_SM3_RING_WIDTH * 4));
	if (ok && dec.ring_rows > caps.MaxTextureHeight)
		dec.ring_rows = caps.MaxTextureHeight;
	/* (a power of two: the shader's row is a multiplication by its inverse) */
	while (dec.ring_rows & (dec.ring_rows - 1))
		dec.ring_rows &= dec.ring_rows - 1;
	ok = ok && dx9c.dynamic_textures && dec.ring_rows >= 256 && caps.MaxTextureWidth >= TEXD_SM3_RING_WIDTH
			&& SUCCEEDED(IDirect3DDevice9_CreateTexture(device, TEXD_SM3_RING_WIDTH, dec.ring_rows, 1, D3DUSAGE_DYNAMIC,
					D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dec.ring, NULL))
			&& SUCCEEDED(IDirect3DDevice9_CreateVertexShader(device, (const DWORD *)texdecode_dx9[1], &dec.vertex_shader))
			&& SUCCEEDED(IDirect3DDevice9_CreatePixelShader(device, (const DWORD *)texdecode_dx9[2], &dec.pixel_shader))
			&& SUCCEEDED(IDirect3DDevice9_CreateVertexDeclaration(device, elements, &dec.decl));
	if (ok)
	{
		/* (the rows are where the shader reads them only if they are 4 KB apart) */
		D3DLOCKED_RECT rect;
		ok = SUCCEEDED(IDirect3DTexture9_LockRect(dec.ring, 0, &rect, NULL, D3DLOCK_DISCARD));
		if (ok)
		{
			ok = rect.Pitch == TEXD_SM3_RING_WIDTH * 4;
			IDirect3DTexture9_UnlockRect(dec.ring, 0);
		}
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX9: the texture shaders could not be set up: textures are converted on the CPU\n");
		dx9_texdec_term();
		return;
	}
	dec.head = 0;
	dec.job_count = 0;
	if (dec.serial == 0)
		dec.serial = 1;
	dec.ready = true;
	NOTICE_LOG(RENDERER, "DX9: textures are drawn on the GPU (a ring of %u KB; drawn into 565 %d, 5551 %d, 4444 %d, 8888 %d, 8-bit %d)\n",
			dec.ring_rows * TEXD_SM3_RING_WIDTH * 4 / 1024, dec.takes[0], dec.takes[1], dec.takes[2], dec.takes[3], dec.takes[4]);
}

void dx9_texdec_term(void)
{
	int t;
	if (dec.ready)
		NOTICE_LOG(RENDERER, "DX9: textures drawn on the GPU: %u levels, %u MB of bytes; %u not, for want of room\n",
				(u32)dec.stat_levels, (u32)(dec.stat_bytes >> 20), (u32)dec.stat_full);
	if (dec.mapped != NULL && dec.ring != NULL)
		IDirect3DTexture9_UnlockRect(dec.ring, 0);
	dec.mapped = NULL;
	dec.ready = false;
	dec.job_count = 0;
	DX9_RELEASE(dec.decl);
	DX9_RELEASE(dec.pixel_shader);
	DX9_RELEASE(dec.vertex_shader);
	DX9_RELEASE(dec.ring);
	for (t = 0; t < 5; t++)
		dec.takes[t] = false;
}

bool dx9_texdec_ready(void)
{
	return dec.ready;
}

bool dx9_texdec_takes(int type)
{
	return dec.ready && (unsigned)type < 5 && dec.takes[type];
}

u8 *dx9_texdec_begin(u32 bytes, u32 levels, u32 *base)
{
	const u32 size = dec.ring_rows * TEXD_SM3_RING_WIDTH * 4;
	u8 *to;
	bytes = (bytes + 15) & ~15u;
	if (!dec.ready || dec.head + bytes > size || dec.job_count + levels > MAX_JOBS)
	{
		/* (the ring is the frame's: what does not fit is converted on the
		 * CPU, as before) */
		dec.stat_full++;
		return NULL;
	}
	if (dec.mapped == NULL)
	{
		D3DLOCKED_RECT rect;
		if (FAILED(IDirect3DTexture9_LockRect(dec.ring, 0, &rect, NULL, D3DLOCK_DISCARD)))
			return NULL;
		dec.mapped = (u8 *)rect.pBits;
		dec.head = 0;
	}
	to = dec.mapped + dec.head;
	*base = dec.head / 4;
	dec.head += bytes;
	dec.stat_bytes += bytes;
	return to;
}

void dx9_texdec_add(IDirect3DSurface9 *surface, int type, const u32 pc[6])
{
	dx9_texdec_job *job = &dec.jobs[dec.job_count++];
	const u32 m = pc[5];
	job->surface = surface;
	job->width = pc[3] & 0xFFFF;
	job->height = pc[3] >> 16;
	job->k[0] = (float)pc[0];
	job->k[1] = (float)pc[1];
	job->k[2] = (float)pc[2];
	job->k[3] = 1.f / dec.ring_rows;
	job->k[4] = (float)job->width;
	job->k[5] = (float)job->height;
	job->k[6] = (float)(pc[4] & 0xFFFF);
	job->k[7] = (float)(pc[4] >> 16);
	job->k[8] = (float)(m & 15);
	job->k[9] = (float)((m >> 4) & 3);
	job->k[10] = (float)((m >> 6) & 3);
	job->k[11] = (float)((m >> 8) & 1);
	job->k[12] = (float)((m >> 12) & 15);
	job->k[13] = (float)((m >> 16) & 15);
	job->k[14] = (float)type;
	job->k[15] = 0.f;
	dec.stat_levels++;
}

u32 dx9_texdec_next(void)
{
	return dec.job_count;
}

u32 dx9_texdec_serial(void)
{
	return dec.serial;
}

void dx9_texdec_cancel(u32 job, u32 count, u32 serial)
{
	u32 i;
	if (serial != dec.serial)
		return;
	for (i = job; i < job + count && i < dec.job_count; i++)
		dec.jobs[i].surface = NULL;
}

void dx9_texdec_flush(void)
{
	static const float triangle[3][4] = { { -1.f, -1.f, 0.f, 1.f }, { -1.f, 3.f, 0.f, 1.f }, { 3.f, -1.f, 0.f, 1.f } };
	u32 live = 0, i;
	if (dec.mapped != NULL)
	{
		IDirect3DTexture9_UnlockRect(dec.ring, 0);
		dec.mapped = NULL;
	}
	dec.head = 0;
	if (dec.job_count == 0)
		return;
	dec.serial++;
	for (i = 0; i < dec.job_count; i++)
		live += dec.jobs[i].surface != NULL;
	if (live == 0 || !dx9c_begin_scene())
	{
		dec.job_count = 0;
		return;
	}
	/* Every state through the context, which knows what the device has:
	 * nothing blended, tested or culled, the ring read texel for texel */
	dx9c_set_no_blend();
	dx9c_set_depth_stencil(false, false, 7, DX9Stencil_None, 0);
	dx9c_set_render_state(D3DRS_ALPHATESTENABLE, FALSE);
	dx9c_set_render_state(D3DRS_CULLMODE, D3DCULL_NONE);
	dx9c_set_decl(dec.decl);
	dx9c_set_vs(dec.vertex_shader);
	dx9c_set_ps(dec.pixel_shader);
	dx9c_set_texture(0, DX9_BASE(dec.ring));
	dx9c_set_sampler(0, DX9C_POINT_SAMPLER);
	for (i = 0; i < dec.job_count; i++)
	{
		const dx9_texdec_job *job = &dec.jobs[i];
		if (job->surface == NULL)
			continue;
		dx9c_set_target(job->surface, NULL, job->width, job->height);
		IDirect3DDevice9_SetPixelShaderConstantF(dx9c.device, TEXD_SM3_CONSTANT, job->k, 4);
		IDirect3DDevice9_DrawPrimitiveUP(dx9c.device, D3DPT_TRIANGLELIST, 1, triangle, sizeof(triangle[0]));
	}
	/* (the texture slot is the polygons': the ring is not left there. The
	 * last level drawn into stays the target until the frame's own is set,
	 * which is before anything is drawn with it) */
	dx9c_set_texture(0, NULL);
	dec.job_count = 0;
}
