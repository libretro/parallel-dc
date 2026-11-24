/* See vk_drawer_base.h */
#include <string.h>
#include "vk_drawer_base.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_words.h"
#include "rend/tileclip.h"

static u32 round_at_least_0(float v)
{
	const long r = tileclip_round(v);
	return r > 0 ? (u32)r : 0;
}

void vk_drawer_base_scissor(const transform_matrix_t *m, VkRect2D *out)
{
	const bool wide_screen_on = settings.rend.WideScreen && !pvrrc.isRenderFramebuffer && !transform_matrix_clipped(m);
	if (wide_screen_on)
	{
		out->offset.x = 0;
		out->offset.y = 0;
		out->extent.width = (u32)screen_width;
		out->extent.height = (u32)screen_height;
	}
	else if (pvrrc.isRenderFramebuffer)
	{
		out->offset.x = 0;
		out->offset.y = 0;
		out->extent.width = 640;
		out->extent.height = 480;
	}
	else
	{
		const xform *scissor = &m->scissor;
		float min_x = xform_x(scissor, pvrrc.fb_X_CLIP.min);
		float min_y = xform_y(scissor, pvrrc.fb_Y_CLIP.min);
		float width = xform_w(scissor, pvrrc.fb_X_CLIP.max - pvrrc.fb_X_CLIP.min + 1);
		float height = xform_h(scissor, pvrrc.fb_Y_CLIP.max - pvrrc.fb_Y_CLIP.min + 1);
		if (width < 0)
		{
			min_x += width;
			width = -width;
		}
		if (height < 0)
		{
			min_y += height;
			height = -height;
		}
		out->offset.x = (s32)round_at_least_0(min_x);
		out->offset.y = (s32)round_at_least_0(min_y);
		out->extent.width = round_at_least_0(width);
		out->extent.height = round_at_least_0(height);
	}
}

static void provoking_vertex(const PolyParam *list, int used)
{
	Vertex *verts = TA_LIST_HEAD(pvrrc.verts, Vertex);
	const u32 *idx = TA_LIST_HEAD(pvrrc.idx, u32);
	int i;
	for (i = 0; i < used; i++)
	{
		const PolyParam *pp = &list[i];
		if (!TAW_GET(pp->pcw, PCW_Gouraud) && pp->count > 2)
		{
			u32 n;
			for (n = 0; n < pp->count - 2; n++)
			{
				Vertex *vertex = &verts[idx[pp->first + n]];
				const Vertex *lastVertex = &verts[idx[pp->first + n + 2]];
				memcpy(vertex->col, lastVertex->col, 4);
				memcpy(vertex->vtx_spc, lastVertex->vtx_spc, 4);
				memcpy(vertex->col1, lastVertex->col1, 4);
				memcpy(vertex->spc1, lastVertex->spc1, 4);
			}
		}
	}
}

void vk_drawer_provoking_vertices(void)
{
	provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_op, PolyParam), TA_LIST_USED(pvrrc.global_param_op, PolyParam));
	provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_pt, PolyParam), TA_LIST_USED(pvrrc.global_param_pt, PolyParam));
	provoking_vertex(TA_LIST_HEAD(pvrrc.global_param_tr, PolyParam), TA_LIST_USED(pvrrc.global_param_tr, PolyParam));
}
