/* The video memory a DXGI adapter has for a renderer to use
 * (rend/vram_budget.h): its budget for its local memory, by
 * QueryVideoMemoryInfo() (DXGI 1.4, Windows 10); before that, its dedicated
 * memory, or half its shared memory for one that has none. The Direct3D 11
 * and Direct3D 12 renderers both ask their adapter this. Windows only. C89. */
#ifndef REND_DXGI_BUDGET_H
#define REND_DXGI_BUDGET_H

#include "rend/d3d_system.h"
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_4.h>
#include "rend/vram_budget.h"

#ifdef __cplusplus
extern "C" {
#endif

/* @desc: the adapter's, or NULL if it is not known (dxgi_budget.c) */
VramBudget dxgi_vram_budget(IDXGIAdapter *adapter, const DXGI_ADAPTER_DESC *desc);

#ifdef __cplusplus
}
#endif

#endif
