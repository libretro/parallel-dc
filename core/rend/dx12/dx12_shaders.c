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
#include "dx12_shaders.h"
#include <stddef.h>
#include <string.h>
#include "dx12_dxbc.inc"
#include "dx12_hdr_dxbc.inc"
#include "dx12_dxil.inc"
#include "rend/texdecode_dxbc.inc"
#include <encodings/xxh32.h>

/* DXIL, where the device takes it: the raster shaders all of one kind,
 * since a pipeline's are */
static bool useDxil;

static D3D12_SHADER_BYTECODE bytecode(const void *code, size_t size)
{
	D3D12_SHADER_BYTECODE b;
	b.pShaderBytecode = code;
	b.BytecodeLength = size;
	return b;
}

D3D12_SHADER_BYTECODE dx12_shader(int n)
{
	return useDxil ? bytecode(dx12_dxil[n], dx12_dxil_size[n]) : bytecode(dx12_dxbc[n], dx12_dxbc_size[n]);
}

D3D12_SHADER_BYTECODE dx12_hdr_shader(int n)
{
	return useDxil ? bytecode(dx12_hdr_dxil[n], dx12_hdr_dxil_size[n]) : bytecode(dx12_hdr_dxbc[n], dx12_hdr_dxbc_size[n]);
}

D3D12_SHADER_BYTECODE dx12_texdecode_kernel(int kernel)
{
	return bytecode(texdecode_dxbc[kernel], texdecode_dxbc_size[kernel]);
}

static u32 hash;

bool dx12_shaders_choose(ID3D12Device *device)
{
	D3D12_FEATURE_DATA_SHADER_MODEL model;
	memset(&model, 0, sizeof(model));
	model.HighestShaderModel = D3D_SHADER_MODEL_6_0;
	useDxil = SUCCEEDED(ID3D12Device_CheckFeatureSupport(device, D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)))
			&& model.HighestShaderModel >= D3D_SHADER_MODEL_6_0;
	hash = 0;
	return useDxil;
}

u32 dx12_shaders_hash(void)
{
	if (hash == 0)
	{
		xxh32_state_t state;
		xxh32_state_t *st = &state;
		size_t i;
		xxh32_reset(st, useDxil ? 1 : 0);
		for (i = 1; i < sizeof(dx12_dxbc) / sizeof(dx12_dxbc[0]); i++)
		{
			const D3D12_SHADER_BYTECODE code = dx12_shader((int)i);
			xxh32_update(st, code.pShaderBytecode, code.BytecodeLength);
		}
		for (i = 1; i < sizeof(dx12_hdr_dxbc) / sizeof(dx12_hdr_dxbc[0]); i++)
		{
			const D3D12_SHADER_BYTECODE code = dx12_hdr_shader((int)i);
			xxh32_update(st, code.pShaderBytecode, code.BytecodeLength);
		}
		for (i = 1; i < sizeof(texdecode_dxbc) / sizeof(texdecode_dxbc[0]); i++)
			xxh32_update(st, texdecode_dxbc[i], texdecode_dxbc_size[i]);
		hash = xxh32_digest(st) | 1;	/* (never 0: not worked out yet) */
	}
	return hash;
}
