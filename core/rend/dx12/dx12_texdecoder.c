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
#include "dx12_texdecoder.h"
#include <string.h>
#include "dx12context_lr.h"
#include "dx12_texture.h"
#include "dx12_shaders.h"

static const IID iid_pipeline_state = { 0x765a30f3, 0xf624, 0x4c6f, { 0xa8, 0x28, 0xac, 0xe9, 0x48, 0x62, 0x24, 0x45 } };

/* the 16-bit formats the kernel draws into, by TEXD_D3D_565, 5551, 4444 */
static const DXGI_FORMAT drawnFormats[3] = { DXGI_FORMAT_B5G6R5_UNORM, DXGI_FORMAT_B5G5R5A1_UNORM, DXGI_FORMAT_B4G4R4A4_UNORM };

#define TEXDEC_MAX_JOBS 2048
#define TEXDEC_UAV_MARKS 16

typedef struct texdec_job
{
	dx12_pooled_texture *texture;
	u32 level;
	u32 pc[8];		/* (eight root constants) */
} texdec_job;

typedef struct texdec_uav_mark
{
	u64 fence;
	u32 head;
} texdec_uav_mark;

static struct
{
	bool ready;
	bool takes[5];
	ID3D12PipelineState *kernel32, *kernel8;
	ID3D12PipelineState *drawn[3];	/* 565, 5551, 4444 */

	texdec_job jobs[TEXDEC_MAX_JOBS];
	u32 jobCount;
	u32 serial;

	/* The unordered access views' places: a ring, a place used again only
	 * once the GPU is done with the list it was made for. Positions,
	 * modulo DX12C_UAV_COUNT for the place. */
	u32 uavHead, uavTail;
	texdec_uav_mark uavMarks[TEXDEC_UAV_MARKS];
	u32 uavMarkFirst, uavMarkCount;
} dec;

void dx12_texdec_init(void)
{
	ID3D12Device *device = dx12c.device;
	D3D12_COMPUTE_PIPELINE_STATE_DESC cs;
	bool ok = true;
	int f;

	dx12_texdec_term();
	if (device == NULL)
		return;

	dec.takes[TEX_TYPE_8888] = dx12c_can_write_uav(DXGI_FORMAT_R8G8B8A8_UINT);
	dec.takes[TEX_TYPE_8] = dx12c_can_write_uav(DXGI_FORMAT_R8_UINT);
	dec.takes[TEX_TYPE_565] = dx12c_format_supported(TEX_TYPE_565) && dx12c_can_render_to(DXGI_FORMAT_B5G6R5_UNORM);
	dec.takes[TEX_TYPE_5551] = dx12c_format_supported(TEX_TYPE_5551) && dx12c_can_render_to(DXGI_FORMAT_B5G5R5A1_UNORM);
	dec.takes[TEX_TYPE_4444] = dx12c_format_supported(TEX_TYPE_4444) && dx12c_can_render_to(DXGI_FORMAT_B4G4R4A4_UNORM);

	/* every pipeline there is, now: none is made while a game runs */
	memset(&cs, 0, sizeof(cs));
	cs.pRootSignature = dx12c.decode_root;
	cs.CS = dx12_texdecode_kernel(1);
	ok = ok && SUCCEEDED(ID3D12Device_CreateComputePipelineState(device, &cs, &iid_pipeline_state, (void **)&dec.kernel32));
	cs.CS = dx12_texdecode_kernel(2);
	ok = ok && SUCCEEDED(ID3D12Device_CreateComputePipelineState(device, &cs, &iid_pipeline_state, (void **)&dec.kernel8));
	for (f = 0; ok && f < 3; f++)
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
		if (!dec.takes[f])
			continue;
		memset(&desc, 0, sizeof(desc));
		desc.pRootSignature = dx12c.decode_root;
		desc.VS = dx12_texdecode_kernel(3);
		desc.PS = dx12_texdecode_kernel(4);
		desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
		desc.SampleMask = UINT_MAX;
		desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		desc.RasterizerState.DepthClipEnable = TRUE;
		desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		desc.NumRenderTargets = 1;
		desc.RTVFormats[0] = drawnFormats[f];
		desc.SampleDesc.Count = 1;
		ok = SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(device, &desc, &iid_pipeline_state, (void **)&dec.drawn[f]));
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: the texture kernels could not be made: textures are converted on the CPU\n");
		dx12_texdec_term();
		return;
	}
	dec.jobCount = 0;
	dec.uavHead = dec.uavTail = 0;
	dec.uavMarkFirst = dec.uavMarkCount = 0;
	dec.ready = true;
	INFO_LOG(RENDERER, "DX12: textures are made on the GPU (32-bit %d, 8-bit %d, 16-bit drawn %d%d%d)\n",
			dec.takes[TEX_TYPE_8888], dec.takes[TEX_TYPE_8],
			dec.takes[TEX_TYPE_565], dec.takes[TEX_TYPE_5551], dec.takes[TEX_TYPE_4444]);
}

void dx12_texdec_term(void)
{
	int i;
	dec.ready = false;
	dec.jobCount = 0;
	for (i = 0; i < 3; i++)
		DX12_RELEASE(dec.drawn[i]);
	DX12_RELEASE(dec.kernel8);
	DX12_RELEASE(dec.kernel32);
	for (i = 0; i < 5; i++)
		dec.takes[i] = false;
}

bool dx12_texdec_ready(void)
{
	return dec.ready;
}

bool dx12_texdec_takes(int type)
{
	return dec.ready && (unsigned)type < 5 && dec.takes[type];
}

u32 dx12_texdec_next(void)
{
	return dec.jobCount;
}

u32 dx12_texdec_serial(void)
{
	return dec.serial;
}

u8 *dx12_texdec_begin(u32 bytes, u32 levels, u32 *base)
{
	dx12_upload upload;
	bytes = (bytes + 15) & ~15u;
	if (!dec.ready || bytes > dx12c.ring_size || levels > TEXDEC_MAX_JOBS)
		return NULL;
	if (dec.jobCount + levels > TEXDEC_MAX_JOBS)
		dx12_texdec_flush();
	if (!dx12c_upload(bytes, 16, &upload))
		return NULL;
	*base = (u32)(upload.offset / 4);
	return upload.cpu;
}

void dx12_texdec_add(dx12_pooled_texture *texture, u32 level, const u32 pc[6])
{
	texdec_job *job = &dec.jobs[dec.jobCount++];
	job->texture = texture;
	job->level = level;
	memcpy(job->pc, pc, sizeof(u32) * 6);
	job->pc[6] = job->pc[7] = 0;
}

void dx12_texdec_cancel(u32 job, u32 count, u32 serial)
{
	u32 i;
	if (serial != dec.serial)
		/* recorded already, and then written over by what came after */
		return;
	for (i = job; i < job + count && i < dec.jobCount; i++)
		dec.jobs[i].texture = NULL;
}

static u32 take_uav(void)
{
	/* what the lists done have used is free again */
	const u64 done = dx12c_completed();
	while (dec.uavMarkCount != 0 && dec.uavMarks[dec.uavMarkFirst].fence <= done)
	{
		dec.uavTail = dec.uavMarks[dec.uavMarkFirst].head;
		dec.uavMarkFirst = (dec.uavMarkFirst + 1) % TEXDEC_UAV_MARKS;
		dec.uavMarkCount--;
	}
	while (dec.uavHead - dec.uavTail >= DX12C_UAV_COUNT)
	{
		/* every place is a list's the GPU has not done: as many lists
		 * behind as there are marks */
		if (dec.uavMarkCount == 0)
			break;
		dx12c_wait_for(dec.uavMarks[dec.uavMarkFirst].fence);
		dec.uavTail = dec.uavMarks[dec.uavMarkFirst].head;
		dec.uavMarkFirst = (dec.uavMarkFirst + 1) % TEXDEC_UAV_MARKS;
		dec.uavMarkCount--;
		dx12c.stat_stalls++;
	}
	return DX12C_UAV_FIRST + (dec.uavHead++ % DX12C_UAV_COUNT);
}

void dx12_texdec_flush(void)
{
	ID3D12Device *device = dx12c.device;
	ID3D12GraphicsCommandList *cmd;
	bool drawnAny = false, computeBound = false;
	u32 out, i;

	if (dec.jobCount == 0)
		return;
	dec.serial++;
	cmd = dx12c_cmd();

	/* each texture written: into the state its levels are made in */
	for (i = 0; i < dec.jobCount; i++)
	{
		dx12_pooled_texture *t = dec.jobs[i].texture;
		bool wide;
		if (t == NULL)
			continue;
		wide = ((dec.jobs[i].pc[5] >> 6) & 3) != TEXD_OUT16;
		dx12c_transition_resource(t->resource, &t->state, wide ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	}

	/* the compute kernels: the 32-bit levels, then the 8-bit ones */
	for (out = TEXD_OUT32;; out = TEXD_OUT8)
	{
		bool bound = false;
		for (i = 0; i < dec.jobCount; i++)
		{
			const texdec_job *job = &dec.jobs[i];
			const u32 kind = (job->pc[5] >> 6) & 3;
			D3D12_UNORDERED_ACCESS_VIEW_DESC view;
			u32 uav;
			if (job->texture == NULL)
				continue;
			if (kind == TEXD_OUT16)
			{
				drawnAny = true;
				continue;
			}
			if (kind != out)
				continue;
			if (!computeBound)
			{
				ID3D12GraphicsCommandList_SetComputeRootSignature(cmd, dx12c.decode_root);
				ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cmd, 1, dx12c.ring_gpu);
				computeBound = true;
			}
			if (!bound)
			{
				ID3D12GraphicsCommandList_SetPipelineState(cmd, out == TEXD_OUT32 ? dec.kernel32 : dec.kernel8);
				bound = true;
			}
			uav = take_uav();
			memset(&view, 0, sizeof(view));
			view.Format = out == TEXD_OUT32 ? DXGI_FORMAT_R8G8B8A8_UINT : DXGI_FORMAT_R8_UINT;
			view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipSlice = job->level;
			ID3D12Device_CreateUnorderedAccessView(device, job->texture->resource, NULL, &view, dx12_heap_cpu(&dx12c.srv_heap, uav));
			ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cmd, 0, 8, job->pc, 0);
			ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cmd, 2, dx12_heap_gpu(&dx12c.srv_heap, uav));
			ID3D12GraphicsCommandList_Dispatch(cmd, ((job->pc[3] & 0xFFFF) + 7) / 8, ((job->pc[3] >> 16) + 7) / 8, 1);
		}
		if (out == TEXD_OUT8)
			break;
	}
	if (computeBound)
	{
		texdec_uav_mark *mark;
		/* (the pipeline bound is not one the context knows) */
		dx12c_forget_pipeline();
		/* the places used are the list's until it is done */
		if (dec.uavMarkCount == TEXDEC_UAV_MARKS)
		{
			/* (the oldest mark merged into the next: its places freed later, not sooner) */
			dec.uavMarkFirst = (dec.uavMarkFirst + 1) % TEXDEC_UAV_MARKS;
			dec.uavMarkCount--;
		}
		mark = &dec.uavMarks[(dec.uavMarkFirst + dec.uavMarkCount) % TEXDEC_UAV_MARKS];
		mark->fence = dx12c.fence_value + 1;
		mark->head = dec.uavHead;
		dec.uavMarkCount++;
	}

	/* the 16-bit levels, drawn */
	if (drawnAny)
	{
		ID3D12GraphicsCommandList_SetGraphicsRootSignature(cmd, dx12c.decode_root);
		ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(cmd, 1, dx12c.ring_gpu);
		dx12c_forget_raster();
		dx12c_set_topology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		for (i = 0; i < dec.jobCount; i++)
		{
			const texdec_job *job = &dec.jobs[i];
			D3D12_RENDER_TARGET_VIEW_DESC view;
			D3D12_RECT scissor;
			float w, h;
			u32 f, rtv;
			if (job->texture == NULL || ((job->pc[5] >> 6) & 3) != TEXD_OUT16)
				continue;
			f = (job->pc[5] >> 20) & 3;
			if (f > 2 || dec.drawn[f] == NULL)
				continue;
			dx12c_set_pipeline(dec.drawn[f]);
			rtv = dx12c_transient_rtv();
			memset(&view, 0, sizeof(view));
			view.Format = drawnFormats[f];
			view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			view.Texture2D.MipSlice = job->level;
			ID3D12Device_CreateRenderTargetView(device, job->texture->resource, &view, dx12_heap_cpu(&dx12c.rtv_heap, rtv));
			dx12c_set_targets(rtv, ~0u, 0);
			w = (float)(job->pc[3] & 0xFFFF);
			h = (float)(job->pc[3] >> 16);
			dx12c_set_viewport(w, h);
			scissor.left = 0;
			scissor.top = 0;
			scissor.right = (LONG)w;
			scissor.bottom = (LONG)h;
			dx12c_set_scissor(&scissor);
			ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cmd, 0, 8, job->pc, 0);
			ID3D12GraphicsCommandList_DrawInstanced(cmd, 3, 1, 0, 0);
		}
		dx12c_set_targets(~0u, ~0u, 0);
		dx12c_forget_root_constants();
	}

	/* to be sampled */
	for (i = 0; i < dec.jobCount; i++)
	{
		dx12_pooled_texture *t = dec.jobs[i].texture;
		if (t != NULL)
			dx12c_transition_resource(t->resource, &t->state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
	}
	dec.jobCount = 0;
}
