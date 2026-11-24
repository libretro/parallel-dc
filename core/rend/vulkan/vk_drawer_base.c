/* See vk_drawer_base.h */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "vk_drawer_base.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/ta_words.h"
#include "hw/pvr/pvr_regs.h"
#include "rend/tileclip.h"
#include "rend/xform.h"
#include "rend/depth_scale.h"
#include "rend/ta_gpu.h"

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

void vk_base_drawer_construct(vk_base_drawer *b)
{
	memset(b, 0, sizeof(*b));
	transform_matrix_init(&b->matrices, false);
	b->gpuDraws = VK_GPU_DRAWS_GROUPS;
}

void vk_base_drawer_term(vk_base_drawer *b)
{
	vk_base_drawer_free_vertex_uniforms(b);
}

int vk_base_drawer_tile_clip(const vk_base_drawer *b, u32 val, VkRect2D *clipRect)
{
	int rect[4];
	int clipmode;
	memset(rect, 0, sizeof(rect));
	clipmode = tile_clip(val, &b->matrices.viewport, rect);
	if (clipmode == TILECLIP_OUTSIDE)
		intersect_clip_rect(rect, b->baseScissor.offset.x, b->baseScissor.offset.y,
				(int)b->baseScissor.extent.width, (int)b->baseScissor.extent.height);
	clipRect->offset.x = rect[0];
	clipRect->offset.y = rect[1];
	clipRect->extent.width = (u32)rect[2];
	clipRect->extent.height = (u32)rect[3];
	return clipmode;
}

void vk_base_drawer_set_base_scissor(vk_base_drawer *b)
{
	vk_drawer_base_scissor(&b->matrices, &b->baseScissor);
	memset(&b->currentScissor, 0, sizeof(b->currentScissor));
}

void vk_base_drawer_fragment_uniforms(FragmentShaderUniforms *u)
{
	/* VERT and RAM fog color constants */
	const u8 *fog_colvert_bgra = (const u8 *)&FOG_COL_VERT;
	const u8 *fog_colram_bgra = (const u8 *)&FOG_COL_RAM;
	/* Fog density constant */
	const u8 *fog_density = (const u8 *)&FOG_DENSITY;
	const float fog_den_mant = fog_density[1] / 128.0f;	/* bit 7 -> x. bit, so [6:0] -> fraction -> /128 */
	const s32 fog_den_exp = (s8)fog_density[0];
	memset(u, 0, sizeof(*u));
	u->sp_FOG_COL_VERT[0] = fog_colvert_bgra[2] / 255.0f;
	u->sp_FOG_COL_VERT[1] = fog_colvert_bgra[1] / 255.0f;
	u->sp_FOG_COL_VERT[2] = fog_colvert_bgra[0] / 255.0f;

	u->sp_FOG_COL_RAM[0] = fog_colram_bgra[2] / 255.0f;
	u->sp_FOG_COL_RAM[1] = fog_colram_bgra[1] / 255.0f;
	u->sp_FOG_COL_RAM[2] = fog_colram_bgra[0] / 255.0f;

	/* (a power of two: exact as a float too) */
	u->sp_FOG_DENSITY = fog_den_mant * (float)pow(2.0, (double)fog_den_exp);

	u->colorClampMin[0] = ((pvrrc.fog_clamp_min >> 16) & 0xFF) / 255.0f;
	u->colorClampMin[1] = ((pvrrc.fog_clamp_min >> 8) & 0xFF) / 255.0f;
	u->colorClampMin[2] = ((pvrrc.fog_clamp_min >> 0) & 0xFF) / 255.0f;
	u->colorClampMin[3] = ((pvrrc.fog_clamp_min >> 24) & 0xFF) / 255.0f;

	u->colorClampMax[0] = ((pvrrc.fog_clamp_max >> 16) & 0xFF) / 255.0f;
	u->colorClampMax[1] = ((pvrrc.fog_clamp_max >> 8) & 0xFF) / 255.0f;
	u->colorClampMax[2] = ((pvrrc.fog_clamp_max >> 0) & 0xFF) / 255.0f;
	u->colorClampMax[3] = ((pvrrc.fog_clamp_max >> 24) & 0xFF) / 255.0f;

	/* half a step under the reference: see the same in gles/gles.c */
	u->cp_AlphaTestValue = ((PT_ALPHA_REF & 0xFF) - 0.5f) / 255.0f;
}

void vk_base_drawer_bind_geometry(const vk_base_drawer *b, VkCommandBuffer cmd, VkDeviceSize indexOffset)
{
	const VkDeviceSize zero = 0;
	if (pvrrc.gpu)
	{
		vkCmdBindVertexBuffers(cmd, 0, 1, &b->gpuDraw->vertices, &zero);
		vkCmdBindIndexBuffer(cmd, b->gpuDraw->indices, 0, VK_INDEX_TYPE_UINT32);
	}
	else
	{
		vkCmdBindVertexBuffers(cmd, 0, 1, &b->geometry, &b->geometryVertices);
		vkCmdBindIndexBuffer(cmd, b->geometry, indexOffset, VK_INDEX_TYPE_UINT32);
	}
}

bool vk_base_drawer_use_parsed(vk_base_drawer *b, u32 sortedBytes)
{
	b->geometryParsed = !pvrrc.gpu && b->parsed.buffer != VK_NULL_HANDLE && sortedBytes <= b->parsed.sortRoom;
	return b->geometryParsed;
}

void vk_base_drawer_set_geometry(vk_base_drawer *b, VkBuffer mainBuffer)
{
	b->geometry = b->geometryParsed ? b->parsed.buffer : mainBuffer;
	b->geometryVertices = b->geometryParsed ? b->parsed.vertices : 0;
}

bool vk_base_drawer_modvols_on_cpu(void)
{
	return !pvrrc.gpu || TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0;
}

const ModTriangle *vk_base_drawer_modvol_list(u32 *bytes)
{
	if (TA_LIST_USED(pvrrc.mv_clip, ModTriangle) != 0)
	{
		*bytes = TA_LIST_BYTES(pvrrc.mv_clip);
		return TA_LIST_HEAD(pvrrc.mv_clip, ModTriangle);
	}
	*bytes = TA_LIST_BYTES(pvrrc.modtrig);
	return TA_LIST_HEAD(pvrrc.modtrig, ModTriangle);
}

void vk_base_drawer_bind_modvol_triangles(const vk_base_drawer *b, VkCommandBuffer cmd, VkBuffer mainBuffer, VkDeviceSize modVolOffset)
{
	const VkDeviceSize zero = 0;
	if (!vk_base_drawer_modvols_on_cpu())
		vkCmdBindVertexBuffers(cmd, 0, 1, &b->gpuDraw->modtrig, &zero);
	/* (where the parse wrote them, with the frame's geometry: geometryParsed) */
	else if (b->geometryParsed)
		vkCmdBindVertexBuffers(cmd, 0, 1, &b->parsed.buffer, &b->parsed.modVols);
	else
		vkCmdBindVertexBuffers(cmd, 0, 1, &mainBuffer, &modVolOffset);
}

void vk_base_drawer_bind_vertices(const vk_base_drawer *b, VkCommandBuffer cmd)
{
	const VkDeviceSize zero = 0;
	if (pvrrc.gpu)
		vkCmdBindVertexBuffers(cmd, 0, 1, &b->gpuDraw->vertices, &zero);
	else
		vkCmdBindVertexBuffers(cmd, 0, 1, &b->geometry, &b->geometryVertices);
}

u32 vk_base_drawer_modvol_triangles(void)
{
	u32 bytes;
	if (!vk_base_drawer_modvols_on_cpu())
		return pvrrc.gpu_modtrig;
	vk_base_drawer_modvol_list(&bytes);
	return bytes / (u32)sizeof(ModTriangle);
}

void vk_base_drawer_draw_indices(const vk_base_drawer *b, VkCommandBuffer cmd, u32 listType, u32 first, u32 count)
{
	const u32 stride = sizeof(VkDrawIndexedIndirectCommand);	/* (rend/ta_gpu.h's records) */
	const vk_ta_gpu_draw *g = b->gpuDraw;
	if (!pvrrc.gpu || b->gpuDraws == VK_GPU_DRAWS_SORTED)
		vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
	else if (b->gpuDraws == VK_GPU_DRAWS_SHADOWS)
		vkCmdDrawIndexedIndirect(cmd, g->commands, g->shadowCommands + (VkDeviceSize)first * stride, 1, stride);
	else if (listType != ListType_Translucent)
		vkCmdDrawIndexedIndirect(cmd, g->commands, (VkDeviceSize)first * stride, 1, stride);
	else if (g->drawCount != NULL)
		g->drawCount(cmd, g->trDraws, (VkDeviceSize)(first + 1) * stride, g->trDraws, (VkDeviceSize)first * stride, count, stride);
	else
		vkCmdDrawIndexedIndirect(cmd, g->trDraws, (VkDeviceSize)(first + 1) * stride, count, stride);
}

bool vk_base_drawer_make_vertex_uniforms(vk_base_drawer *b, u32 count)
{
	vk_buffer *more;
	if (count <= b->vertexUniformCount)
		return true;
	more = (vk_buffer *)realloc(b->vertexUniformBuffers, count * sizeof(vk_buffer));
	if (more == NULL)
		return false;
	b->vertexUniformBuffers = more;
	while (b->vertexUniformCount < count)
	{
		if (vk_buffer_init(&more[b->vertexUniformCount], sizeof(VertexShaderUniforms),
				VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != VK_SUCCESS)
			return false;
		b->vertexUniformCount++;
	}
	return true;
}

void vk_base_drawer_free_vertex_uniforms(vk_base_drawer *b)
{
	u32 i;
	for (i = 0; i < b->vertexUniformCount; i++)
		vk_buffer_free(&b->vertexUniformBuffers[i]);
	free(b->vertexUniformBuffers);
	b->vertexUniformBuffers = NULL;
	b->vertexUniformCount = 0;
}

VkBuffer vk_base_drawer_write_vertex_uniforms(vk_base_drawer *b, VkCommandBuffer cmd, int image)
{
	VertexShaderUniforms uniforms;
	const vk_buffer *buffer;
	if (!vk_base_drawer_make_vertex_uniforms(b, (u32)image + 1))
		return VK_NULL_HANDLE;
	xform_to_mat4(&b->matrices.normal, uniforms.normal_matrix);
	uniforms.depth_scale = pvr_depth_scale(pvrrc.fZ_max);
	uniforms.depth_mul = 1.f / b->matrices.normal.sz;
	uniforms.pad[0] = uniforms.pad[1] = 0.f;
	buffer = &b->vertexUniformBuffers[image];
	vk_buffer_upload(buffer, sizeof(uniforms), &uniforms, 0);
	if (pvrrc.gpu && b->gpuDraw != NULL && b->gpuDraw->frame != VK_NULL_HANDLE && cmd != VK_NULL_HANDLE)
	{
		VkBufferCopy region;
		VkMemoryBarrier copied;
		region.srcOffset = TAG_O_ZSCALE * 4;
		region.dstOffset = offsetof(VertexShaderUniforms, depth_scale);
		region.size = 4;
		vkCmdCopyBuffer(cmd, b->gpuDraw->frame, buffer->buffer, 1, &region);
		memset(&copied, 0, sizeof(copied));
		copied.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		copied.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		copied.dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT;
		vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &copied, 0, NULL, 0, NULL);
	}
	return buffer->buffer;
}
