/* What the Vulkan drawers share: the base scissor, the flat polygons'
 * provoking vertices, and what a drawer draws a frame's geometry from
 * (vk_base_drawer) */
#ifndef VK_DRAWER_BASE_H
#define VK_DRAWER_BASE_H
#include "vk_context.h"
#include "vk_buffer.h"
#include "vk_cmd_pool.h"
#include "vk_ta_decoder.h"
#include "shaders.h"
#include "rend/transform_matrix.h"
#include "rend/tileclip.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame's (pvrrc's) scissor, by its matrices @m: the pixel clip, or
 * the whole screen with widescreen and no clip, or the framebuffer's */
void vk_drawer_base_scissor(const transform_matrix_t *m, VkRect2D *out);
/* Vulkan uses the colour values of the first vertex for flat shaded
 * triangle strips. On Dreamcast the last vertex is the provoking one so it
 * is copied onto the first. */
void vk_drawer_provoking_vertices(void);

/* Where a frame the parse wrote where the card reads it is
 * (rend/parse_arena.h): its vertices, its indices, and the room after them
 * for the sorted passes' */
typedef struct vk_parsed
{
	VkBuffer buffer;
	VkDeviceSize vertices, indices;
	VkDeviceSize modVols;	/* the volumes' triangles drawn (vk_base_drawer_modvol_list()'s) */
	u32 sortRoom;			/* bytes */
} vk_parsed;

/* How vk_base_drawer_draw_indices() draws a frame decoded on the GPU: its
 * draw groups; the sorted translucent triangles, whose indices the CPU
 * wrote (TA_GPU_SORT); or the draw groups drawn again for shadows, by the
 * commands the GPU wrote for that (TA_GPU_SHADOWS) */
enum { VK_GPU_DRAWS_GROUPS, VK_GPU_DRAWS_SORTED, VK_GPU_DRAWS_SHADOWS };

typedef struct vk_base_drawer
{
	VkRect2D baseScissor;
	VkRect2D currentScissor;
	transform_matrix_t matrices;
	vk_cmd_pool *pool;
	const vk_ta_gpu_draw *gpuDraw;	/* what a frame decoded on the GPU is drawn from (pvrrc.gpu) */
	int gpuDraws;
	/* every swap chain image's vertex uniform buffer (shaders/position.glsl) */
	vk_buffer *vertexUniformBuffers;
	u32 vertexUniformCount;
	VkBuffer vertexUniformBuffer;	/* this render's */
	/* Where this render's vertices and indices are drawn from: the main
	 * buffer, or a frame the parse wrote where the card reads it (parsed) */
	VkBuffer geometry;
	VkDeviceSize geometryVertices;
	bool geometryParsed;
	vk_parsed parsed;
} vk_base_drawer;

void vk_base_drawer_construct(vk_base_drawer *b);
void vk_base_drawer_term(vk_base_drawer *b);
static INLINE void vk_base_drawer_set_scissor(vk_base_drawer *b, VkCommandBuffer cmd, const VkRect2D *scissor)
{
	if (scissor->offset.x != b->currentScissor.offset.x || scissor->offset.y != b->currentScissor.offset.y
			|| scissor->extent.width != b->currentScissor.extent.width || scissor->extent.height != b->currentScissor.extent.height)
	{
		vkCmdSetScissor(cmd, 0, 1, scissor);
		b->currentScissor = *scissor;
	}
}
/* A polygon's tile clip (TILECLIP_*), and the rectangle it clips to */
int vk_base_drawer_tile_clip(const vk_base_drawer *b, u32 val, VkRect2D *clipRect);
/* the frame's base scissor (vk_drawer_base_scissor()), and none set yet */
void vk_base_drawer_set_base_scissor(vk_base_drawer *b);
/* the fragment shaders' uniforms of the frame, but shade_scale_factor */
void vk_base_drawer_fragment_uniforms(FragmentShaderUniforms *u);
/* The frame's vertices and indices: where they are (geometry: the main
 * buffer, or where the parse wrote them), or the GPU's decode's (pvrrc.gpu) */
void vk_base_drawer_bind_geometry(const vk_base_drawer *b, VkCommandBuffer cmd, VkDeviceSize indexOffset);
/* The geometry for this render, the main buffer's or the parse's: the
 * parse's if it wrote the frame where the card reads it and the sorted
 * passes' @sortedBytes fit after its indices */
bool vk_base_drawer_use_parsed(vk_base_drawer *b, u32 sortedBytes);
/* ...and with the main buffer laid out, where the draws bind it */
void vk_base_drawer_set_geometry(vk_base_drawer *b, VkBuffer mainBuffer);
/* ...the modifier volumes' triangles, as the vertex buffer: the main
 * buffer's (vk_base_drawer_modvol_list()), or those of the GPU's decode
 * where the frame has none cut (rend/modvol_clip.h) */
void vk_base_drawer_bind_modvol_triangles(const vk_base_drawer *b, VkCommandBuffer cmd, VkBuffer mainBuffer, VkDeviceSize modVolOffset);
bool vk_base_drawer_modvols_on_cpu(void);
/* the volumes' triangles the main buffer has: the cut ones when the frame has them, else the parse's; and their bytes */
const ModTriangle *vk_base_drawer_modvol_list(u32 *bytes);
/* ...and back to the vertices */
void vk_base_drawer_bind_vertices(const vk_base_drawer *b, VkCommandBuffer cmd);
u32 vk_base_drawer_modvol_triangles(void);
/* A polygon parameter's indices drawn: @count from @first; or, of a frame
 * decoded on the GPU, a draw group's (PolyParam::first and count: see
 * TA_GPU_DRAW), by its command, or for a translucent one by its draw
 * records, with their count or all of its room */
void vk_base_drawer_draw_indices(const vk_base_drawer *b, VkCommandBuffer cmd, u32 listType, u32 first, u32 count);
/* every swap chain image's vertex uniform buffer, before the first frame:
 * nothing allocated while the game runs; false if one could not be made */
bool vk_base_drawer_make_vertex_uniforms(vk_base_drawer *b, u32 count);
void vk_base_drawer_free_vertex_uniforms(vk_base_drawer *b);
/* The vertex shaders' uniforms for the render being recorded, in the
 * buffer of swap chain image @image: the matrix and 1/fZ_max from the
 * host; for a frame decoded on the GPU, 1/fZ_max is the decode's word,
 * copied over the host's before the render pass (so this is called before
 * the render pass begins). A swap chain the frontend made longer while the
 * game runs has the new images' made here. */
VkBuffer vk_base_drawer_write_vertex_uniforms(vk_base_drawer *b, VkCommandBuffer cmd, int image);

#ifdef __cplusplus
}
#endif

#endif
