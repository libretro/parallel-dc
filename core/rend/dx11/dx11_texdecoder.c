/* Textures made on the GPU from their bytes in video memory: see
 * dx11_texdecoder.h */
#include "dx11_texdecoder.h"
#include "dx11_nvapi.h"
#include "dx11context_lr.h"

/* The kernels, by TEXD_KERNEL_D3D, as DXBC made from rend/texdecode.h when
 * the core was built: tools/rend/kernels_dxbc.sh, which checks that they
 * still are its. Nothing is compiled when the core runs. */
#include "rend/texdecode_dxbc.inc"

#define MAX_JOBS 2048

typedef struct dx11_texdec_job
{
	ID3D11UnorderedAccessView *uav;
	ID3D11RenderTargetView *target;
	u32 pc[8];		/* (a whole constant buffer) */
} dx11_texdec_job;

static struct
{
	bool ready;
	bool takes[5];
	bool no_overwrite;	/* the device maps a buffer it reads from as a view without discarding it */
	bool overlap;		/* NVAPI's UAV overlap: a flush's kernels run without waits between them */
	ID3D11DeviceContext *device_context;

	ID3D11Buffer *ring;
	ID3D11ShaderResourceView *ring_view;
	u8 *mapped;
	u32 ring_size;
	u32 head;			/* where the next bytes go */
	ID3D11Buffer *constants;
	ID3D11ComputeShader *kernel32, *kernel8;
	ID3D11VertexShader *vertex_shader;
	ID3D11PixelShader *pixel_shader;
	ID3D11RasterizerState *rasterizer;
	ID3D11DepthStencilState *depth_stencil;

	dx11_texdec_job jobs[MAX_JOBS];
	u32 job_count;
	u32 serial;
	/* what has been done so far, for measuring */
	u64 stat_bytes;
	u64 stat_levels;
} dec;

static bool formatHas(ID3D11Device *device, DXGI_FORMAT format, UINT what)
{
	UINT support = 0;
	return SUCCEEDED(ID3D11Device_CheckFormatSupport(device, format, &support)) && (support & what) == what;
}

void dx11_texdec_init(void)
{
	const UINT uav = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
	const UINT drawn = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET;
	ID3D11Device *device;
	D3D11_FEATURE_DATA_D3D11_OPTIONS options;
	D3D11_BUFFER_DESC desc, cb;
	D3D11_RASTERIZER_DESC rast;
	D3D11_DEPTH_STENCIL_DESC ds;
	u64 bytes;
	bool ok;

	dx11_texdec_term();
	/* Compute shaders and a context that is the device's own: a deferred
	 * one maps a buffer only to discard it, every time */
	if (!dx11c_has_per_pixel())
	{
		INFO_LOG(RENDERER, "DX11: textures are converted on the CPU\n");
		return;
	}
	device = dx11c.device;
	dec.device_context = dx11c.device_context;

	dec.takes[TEX_TYPE_8888] = formatHas(device, DXGI_FORMAT_R8G8B8A8_UINT, uav);
	dec.takes[TEX_TYPE_8] = formatHas(device, DXGI_FORMAT_R8_UINT, uav);
	dec.takes[TEX_TYPE_565] = dx11c_format_supported(TEX_TYPE_565) && formatHas(device, DXGI_FORMAT_B5G6R5_UNORM, drawn);
	dec.takes[TEX_TYPE_5551] = dx11c_format_supported(TEX_TYPE_5551) && formatHas(device, DXGI_FORMAT_B5G5R5A1_UNORM, drawn);
	dec.takes[TEX_TYPE_4444] = dx11c_format_supported(TEX_TYPE_4444) && formatHas(device, DXGI_FORMAT_B4G4R4A4_UNORM, drawn);

	memset(&options, 0, sizeof(options));
	dec.no_overwrite = SUCCEEDED(ID3D11Device_CheckFeatureSupport(device, D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
			&& options.MapNoOverwriteOnDynamicBufferSRV;

	/* The ring: what textures take as they are in video memory - a few
	 * hundred KB in most frames, megabytes when a scene is loaded - a
	 * share of the video memory there is, between 4 and 16 MB. (The
	 * largest texture, 1024x1024 with its levels, is under 3 MB.) */
	bytes = (u64)vram_budget_available_kib(&dx11c.vram_budget) * 1024 / 256;
	if (bytes < 4 * 1024 * 1024)
		bytes = 4 * 1024 * 1024;
	if (bytes > 16 * 1024 * 1024)
		bytes = 16 * 1024 * 1024;
	dec.ring_size = (u32)bytes & ~15u;

	memset(&desc, 0, sizeof(desc));
	desc.ByteWidth = dec.ring_size;
	desc.Usage = D3D11_USAGE_DYNAMIC;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	ok = SUCCEEDED(ID3D11Device_CreateBuffer(device, &desc, NULL, &dec.ring));
	if (!ok)
		dec.ring = NULL;
	if (ok)
	{
		D3D11_SHADER_RESOURCE_VIEW_DESC view;
		memset(&view, 0, sizeof(view));
		view.Format = DXGI_FORMAT_R32_TYPELESS;
		view.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
		view.BufferEx.FirstElement = 0;
		view.BufferEx.NumElements = dec.ring_size / 4;
		view.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
		ok = SUCCEEDED(ID3D11Device_CreateShaderResourceView(device, (ID3D11Resource *)dec.ring, &view, &dec.ring_view));
		if (!ok)
			dec.ring_view = NULL;
	}
	memset(&cb, 0, sizeof(cb));
	cb.ByteWidth = sizeof(dec.jobs[0].pc);
	cb.Usage = D3D11_USAGE_DEFAULT;
	cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	/* every kernel there is, now: none is made while a game runs */
	ok = ok && SUCCEEDED(ID3D11Device_CreateBuffer(device, &cb, NULL, &dec.constants));
	ok = ok && SUCCEEDED(ID3D11Device_CreateComputeShader(device, texdecode_dxbc[1], texdecode_dxbc_size[1], NULL, &dec.kernel32));
	ok = ok && SUCCEEDED(ID3D11Device_CreateComputeShader(device, texdecode_dxbc[2], texdecode_dxbc_size[2], NULL, &dec.kernel8));
	ok = ok && SUCCEEDED(ID3D11Device_CreateVertexShader(device, texdecode_dxbc[3], texdecode_dxbc_size[3], NULL, &dec.vertex_shader));
	ok = ok && SUCCEEDED(ID3D11Device_CreatePixelShader(device, texdecode_dxbc[4], texdecode_dxbc_size[4], NULL, &dec.pixel_shader));

	memset(&rast, 0, sizeof(rast));
	rast.FillMode = D3D11_FILL_SOLID;
	rast.CullMode = D3D11_CULL_NONE;
	rast.DepthClipEnable = TRUE;
	ok = ok && SUCCEEDED(ID3D11Device_CreateRasterizerState(device, &rast, &dec.rasterizer));
	memset(&ds, 0, sizeof(ds));
	ds.DepthEnable = FALSE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
	ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp = ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	ds.BackFace = ds.FrontFace;
	ok = ok && SUCCEEDED(ID3D11Device_CreateDepthStencilState(device, &ds, &dec.depth_stencil));
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX11: the texture kernels could not be made: textures are converted on the CPU\n");
		dx11_texdec_term();
		return;
	}
	dec.head = 0;
	dec.job_count = 0;
	dec.ready = true;
	/* An NVIDIA device whose driver has NVAPI's UAV overlap: a frame's
	 * levels are made without the driver waiting for each kernel to be done
	 * before the next starts (dx11_texdec_flush()) */
	dec.overlap = dx11c.vendor == 0x10de && dx11_nvapi_uav_overlap_available();
	INFO_LOG(RENDERER, "DX11: textures are made on the GPU (a ring of %u KB%s; 32-bit %d, 8-bit %d, 16-bit drawn %d%d%d; %s)\n",
			dec.ring_size / 1024, dec.no_overwrite ? "" : ", discarded every frame",
			dec.takes[TEX_TYPE_8888], dec.takes[TEX_TYPE_8],
			dec.takes[TEX_TYPE_565], dec.takes[TEX_TYPE_5551], dec.takes[TEX_TYPE_4444],
			dec.overlap ? "the kernels overlapped through NVAPI" : "each kernel after the one before");
}

void dx11_texdec_term(void)
{
	int t;
	if (dec.mapped != NULL && dec.device_context != NULL)
		ID3D11DeviceContext_Unmap(dec.device_context, (ID3D11Resource *)dec.ring, 0);
	dec.mapped = NULL;
	dec.ready = false;
	dec.overlap = false;
	dec.job_count = 0;
	DX11_RELEASE(dec.depth_stencil);
	DX11_RELEASE(dec.rasterizer);
	DX11_RELEASE(dec.pixel_shader);
	DX11_RELEASE(dec.vertex_shader);
	DX11_RELEASE(dec.kernel8);
	DX11_RELEASE(dec.kernel32);
	DX11_RELEASE(dec.constants);
	DX11_RELEASE(dec.ring_view);
	DX11_RELEASE(dec.ring);
	for (t = 0; t < 5; t++)
		dec.takes[t] = false;
	dec.device_context = NULL;
}

bool dx11_texdec_ready(void)
{
	return dec.ready;
}

bool dx11_texdec_takes(int type)
{
	return dec.ready && (unsigned)type < 5 && dec.takes[type];
}

u8 *dx11_texdec_begin(u32 bytes, u32 levels, u32 *base)
{
	bool wraps;
	u8 *to;
	bytes = (bytes + 15) & ~15u;
	if (!dec.ready || bytes > dec.ring_size || levels > MAX_JOBS)
		return NULL;
	wraps = dec.head + bytes > dec.ring_size;
	if (dec.job_count + levels > MAX_JOBS || (wraps && dec.mapped != NULL))
		/* What the ring has now goes to the GPU first: a mapping that
		 * discards would take it away from the kernels yet to read it */
		dx11_texdec_flush();
	if (dec.mapped == NULL)
	{
		/* After what the GPU may still be reading, if the device lets a
		 * buffer it reads through a view be mapped so; from the start of
		 * new memory otherwise, and when the ring is full */
		const D3D11_MAP how = dec.no_overwrite && !wraps && dec.head != 0 ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
		D3D11_MAPPED_SUBRESOURCE map;
		if (FAILED(ID3D11DeviceContext_Map(dec.device_context, (ID3D11Resource *)dec.ring, 0, how, 0, &map)))
			return NULL;
		dec.mapped = (u8 *)map.pData;
		if (how == D3D11_MAP_WRITE_DISCARD)
			dec.head = 0;
	}
	to = dec.mapped + dec.head;
	*base = dec.head / 4;
	dec.head += bytes;
	dec.stat_bytes += bytes;
	return to;
}

void dx11_texdec_add(ID3D11UnorderedAccessView *uav, ID3D11RenderTargetView *target, const u32 pc[6])
{
	dx11_texdec_job *job = &dec.jobs[dec.job_count++];
	job->uav = uav;
	job->target = target;
	memcpy(job->pc, pc, sizeof(u32) * 6);
	job->pc[6] = job->pc[7] = 0;
	dec.stat_levels++;
}

u32 dx11_texdec_next(void)
{
	return dec.job_count;
}

u32 dx11_texdec_serial(void)
{
	return dec.serial;
}

void dx11_texdec_cancel(u32 job, u32 count, u32 serial)
{
	u32 i;
	if (serial != dec.serial)
		/* recorded already, and then written over by what came after */
		return;
	for (i = job; i < job + count && i < dec.job_count; i++)
	{
		dec.jobs[i].uav = NULL;
		dec.jobs[i].target = NULL;
	}
}

void dx11_texdec_flush(void)
{
	ID3D11DeviceContext *ctx = dec.device_context;
	ID3D11UnorderedAccessView *noUav = NULL;
	bool drawn = false, overlapped;
	u32 dispatches = 0, out, i;
	if (dec.mapped != NULL)
	{
		ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)dec.ring, 0);
		dec.mapped = NULL;
	}
	if (dec.job_count == 0)
		return;
	dec.serial++;
	/* Textures may be bound to be read by the pixel shader. Direct3D
	 * unbinds a texture from there when it is bound to be written, and
	 * what the context knows of what is bound would be wrong: they are
	 * unbound first. */
	dx11c_unbind_ps_resources(4);

	ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &dec.ring_view);
	ID3D11DeviceContext_CSSetConstantBuffers(ctx, 0, 1, &dec.constants);
	/* The kernels of a flush do not depend on one another: each reads the
	 * ring and its constants alone, which nothing here writes but the CPU
	 * before it is unmapped, and writes one level of one texture through
	 * its own view - no two of them the same level, since a texture made
	 * again, given back to the pool or uploaded by the CPU takes back what
	 * it asked for before (dx11_texdec_cancel(), which nulls those jobs).
	 * With NVAPI's UAV overlap the driver does not wait for each to be
	 * done before the next starts; the run ends before anything that
	 * reads the levels (the views unbound below, then the draws). */
	if (dec.overlap)
		for (i = 0; i < dec.job_count; i++)
			if (dec.jobs[i].uav != NULL)
				dispatches++;
	overlapped = dispatches > 1;
	if (overlapped)
		dx11_nvapi_begin_uav_overlap(ctx);
	for (out = TEXD_OUT8; out <= TEXD_OUT32; out += TEXD_OUT32 - TEXD_OUT8)
	{
		bool bound = false;
		for (i = 0; i < dec.job_count; i++)
		{
			const dx11_texdec_job *job = &dec.jobs[i];
			if (job->target != NULL)
				drawn = true;
			if (job->uav == NULL || ((job->pc[5] >> 6) & 3) != out)
				continue;
			if (!bound)
			{
				ID3D11DeviceContext_CSSetShader(ctx, out == TEXD_OUT32 ? dec.kernel32 : dec.kernel8, NULL, 0);
				bound = true;
			}
			ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)dec.constants, 0, NULL, job->pc, 0, 0);
			ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, (ID3D11UnorderedAccessView **)&job->uav, NULL);
			ID3D11DeviceContext_Dispatch(ctx, ((job->pc[3] & 0xFFFF) + 7) / 8, ((job->pc[3] >> 16) + 7) / 8, 1);
		}
	}
	if (overlapped)
		dx11_nvapi_end_uav_overlap(ctx);
	/* (unbound, to be sampled) */
	ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 1, &noUav, NULL);

	if (drawn)
	{
		dx11c_set_input_layout(NULL);
		dx11c_set_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		dx11c_set_vs(dec.vertex_shader);
		dx11c_set_ps(dec.pixel_shader);
		dx11c_set_rasterizer(dec.rasterizer);
		dx11c_set_blend(NULL, NULL);
		dx11c_set_depth_stencil(dec.depth_stencil, 0);
		dx11c_set_ps_resource(0, dec.ring_view);
		dx11c_set_ps_constants(0, dec.constants);
		for (i = 0; i < dec.job_count; i++)
		{
			const dx11_texdec_job *job = &dec.jobs[i];
			if (job->target == NULL)
				continue;
			ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)dec.constants, 0, NULL, job->pc, 0, 0);
			dx11c_set_targets(job->target, NULL);
			dx11c_set_viewport((float)(job->pc[3] & 0xFFFF), (float)(job->pc[3] >> 16));
			ID3D11DeviceContext_Draw(ctx, 3, 0);
		}
		dx11c_set_targets(NULL, NULL);
		dx11c_set_ps_resource(0, NULL);
	}
	dec.job_count = 0;
}
