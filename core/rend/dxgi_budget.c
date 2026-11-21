/* The video memory a DXGI adapter has for a renderer to use: see
 * dxgi_budget.h */
#include <string.h>
#include "rend/dxgi_budget.h"

/* (IDXGIAdapter3's, as dxgi1_4.h has it: here, so that nothing more is
 * linked for it) */
static const IID dxgi_iid_adapter3 = { 0x645967a4, 0x1392, 0x4310, { 0xa7, 0x98, 0x80, 0x53, 0xce, 0x3e, 0x93, 0xfd } };

VramBudget dxgi_vram_budget(IDXGIAdapter *adapter, const DXGI_ADAPTER_DESC *desc)
{
	VramBudget budget;
	IDXGIAdapter3 *adapter3 = NULL;
	memset(&budget, 0, sizeof(budget));
	if (SUCCEEDED(IDXGIAdapter_QueryInterface(adapter, &dxgi_iid_adapter3, (void **)&adapter3)))
	{
		DXGI_QUERY_VIDEO_MEMORY_INFO info;
		if (SUCCEEDED(IDXGIAdapter3_QueryVideoMemoryInfo(adapter3, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)) && info.Budget != 0)
		{
			budget.budget_kib = vram_kib(info.Budget);
			budget.usage_kib = vram_kib(info.CurrentUsage);
			budget.exact = true;
		}
		IDXGIAdapter3_Release(adapter3);
	}
	if (!budget.exact && desc != NULL)
		budget.budget_kib = vram_kib(desc->DedicatedVideoMemory != 0 ? (u64)desc->DedicatedVideoMemory
				: (u64)desc->SharedSystemMemory / 2);
	return budget;
}
