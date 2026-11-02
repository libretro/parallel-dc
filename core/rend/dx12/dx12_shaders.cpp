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
#include <cstddef>
#include "dx12_dxbc.inc"
#include "dx12_hdr_dxbc.inc"
#include "rend/texdecode_dxbc.inc"
#include "deps/xxhash/xxhash.h"

D3D12_SHADER_BYTECODE dx12_shader(DX12Shader shader)
{
	const int n = (int)shader;
	D3D12_SHADER_BYTECODE code = { dx12_dxbc[n], dx12_dxbc_size[n] };
	return code;
}

D3D12_SHADER_BYTECODE dx12_hdr_shader(DX12HdrShader shader)
{
	const int n = (int)shader;
	D3D12_SHADER_BYTECODE code = { dx12_hdr_dxbc[n], dx12_hdr_dxbc_size[n] };
	return code;
}

D3D12_SHADER_BYTECODE dx12_texdecode_kernel(int kernel)
{
	D3D12_SHADER_BYTECODE code = { texdecode_dxbc[kernel], texdecode_dxbc_size[kernel] };
	return code;
}

u32 dx12_shaders_hash()
{
	static u32 hash;
	if (hash == 0)
	{
		XXH32_state_t *st = XXH32_createState();
		XXH32_reset(st, 0);
		for (size_t i = 1; i < sizeof(dx12_dxbc) / sizeof(dx12_dxbc[0]); i++)
			XXH32_update(st, dx12_dxbc[i], dx12_dxbc_size[i]);
		for (size_t i = 1; i < sizeof(dx12_hdr_dxbc) / sizeof(dx12_hdr_dxbc[0]); i++)
			XXH32_update(st, dx12_hdr_dxbc[i], dx12_hdr_dxbc_size[i]);
		for (size_t i = 1; i < sizeof(texdecode_dxbc) / sizeof(texdecode_dxbc[0]); i++)
			XXH32_update(st, texdecode_dxbc[i], texdecode_dxbc_size[i]);
		hash = XXH32_digest(st) | 1;	// (never 0: not worked out yet)
		XXH32_freeState(st);
	}
	return hash;
}
