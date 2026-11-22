/* Quads: a picture drawn over the framebuffer (the last picture, the VMUs,
 * the crosshairs), and SSAA's resolve of its samples into the picture */
#ifndef VK_QUAD_H
#define VK_QUAD_H

#include "vk_context.h"
#include "vk_buffer.h"
#include "vk_cmd_pool.h"
#include "shaders.h"
#include "pso.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct QuadVertex
{
	f32 pos[3];
	f32 uv[2];
} QuadVertex;

/* the vertex input of a quad's pipeline: its positions, and its texture
 * coordinates if @uv */
void vk_quad_input_state(VkPipelineVertexInputStateCreateInfo *info, bool uv);

/* A picture of the console's drawn at @from (widescreen or not: the
 * console's 4:3 in the middle of a wider one) over one of size @to, its
 * 4:3 where the other's is, and the rest of a wider one's not drawn: the
 * quad's vertices into @v, and true; false when the two have the same
 * shape, which the whole quad draws */
static INLINE bool vk_quad_game_area(QuadVertex v[4], u32 fromWidth, u32 fromHeight, u32 toWidth, u32 toHeight)
{
	float f, t, u0, u1;
	if (fromWidth == 0 || fromHeight == 0 || toWidth == 0 || toHeight == 0)
		return false;
	/* (how much of each one's width the 4:3 is) */
	f = fromWidth * 3.f > fromHeight * 4.f ? fromHeight * 4.f / (fromWidth * 3.f) : 1.f;
	t = toWidth * 3.f > toHeight * 4.f ? toHeight * 4.f / (toWidth * 3.f) : 1.f;
	if (f == t)
		return false;
	u0 = 0.5f - f / 2.f;
	u1 = 0.5f + f / 2.f;
	v[0].pos[0] = -t; v[0].pos[1] = -1.f; v[0].pos[2] = 0.f; v[0].uv[0] = u0; v[0].uv[1] = 0.f;
	v[1].pos[0] =  t; v[1].pos[1] = -1.f; v[1].pos[2] = 0.f; v[1].uv[0] = u1; v[1].uv[1] = 0.f;
	v[2].pos[0] = -t; v[2].pos[1] =  1.f; v[2].pos[2] = 0.f; v[2].uv[0] = u0; v[2].uv[1] = 1.f;
	v[3].pos[0] =  t; v[3].pos[1] =  1.f; v[3].pos[2] = 0.f; v[3].uv[0] = u1; v[3].uv[1] = 1.f;
	return true;
}

/* The quads' pipelines, blended by a constant or by the picture's alpha.
 * Both are handed to the pipeline thread by init() (pso.h): a draw takes
 * one made, or makes it there and then. One the device cannot make is
 * made again where it is drawn, and fails there as it always did. */
typedef struct vk_quad_pipeline
{
	vk_pso_builder builder;	/* (first: the worker's key 1 the alpha-blended one) */
	VkRenderPass renderPass;
	int subpass;
	u32 samples;
	PsoSlot slots[2];
	VkSampler linearSampler;
	VkSampler nearestSampler;
	VkPipelineLayout pipelineLayout;
	VkDescriptorSetLayout descSetLayout;
	vk_raster_modules *modules;
} vk_quad_pipeline;

void vk_quad_pipeline_construct(vk_quad_pipeline *p);
/* @samples: of the subpass's attachments (SSAA). False if what it needs
 * could not be made. */
bool vk_quad_pipeline_init(vk_quad_pipeline *p, vk_raster_modules *modules, VkRenderPass renderPass, int subpass, u32 samples);
/* Its pipelines dropped, to be made again for whatever render pass the
 * next init() names (one destroyed may have the same handle) */
void vk_quad_pipeline_forget(vk_quad_pipeline *p);
/* Before the modules' term: the pipeline thread may be making one of them
 * with them */
void vk_quad_pipeline_term(vk_quad_pipeline *p);
/* VK_NULL_HANDLE if the device could not make it */
VkPipeline vk_quad_pipeline_get(vk_quad_pipeline *p, bool alpha);

/* For each of the frontend's images, a few descriptor sets, what the one
 * drawn with was last written with, and the frame each was last drawn in:
 * a set whose frames the GPU is done with is written again when the
 * picture changes, none made or given back. (More images than there are
 * slots share them.) */
enum { VK_QUAD_SLOTS = 16, VK_QUAD_SETS = 3 };

typedef struct vk_quad_slot
{
	VkDescriptorSet set[VK_QUAD_SETS];
	u64 used[VK_QUAD_SETS];
	u32 current;
	VkImageView view;
	VkSampler sampler;
} vk_quad_slot;

typedef struct vk_quad_drawer
{
	vk_quad_pipeline *pipeline;
	vk_buffer buffer;
	vk_quad_slot slots[VK_QUAD_SLOTS];
} vk_quad_drawer;

bool vk_quad_drawer_init(vk_quad_drawer *d, vk_quad_pipeline *pipeline);
void vk_quad_drawer_term(vk_quad_drawer *d);
/* @pool: the one @cmd is of. A descriptor set that has to be replaced is
 * left with it, to be destroyed once the frames that can be using it are
 * done. @vertices NULL: the whole framebuffer. */
void vk_quad_drawer_draw(vk_quad_drawer *d, VkCommandBuffer cmd, VkImageView view, const QuadVertex *vertices,
		bool nearestFilter, vk_cmd_pool *pool);

/* The resolve of an attachment of N samples into the subpass's single
 * sample colour attachment, as its input attachment 0: a triangle over
 * the whole framebuffer. Everything made in init() but its pipeline, which
 * init() hands to the pipeline thread (pso.h); the first draw takes it,
 * or makes it there and fails there as init() did when the device cannot. */
typedef struct vk_ssaa_resolve
{
	vk_pso_builder builder;
	VkDescriptorSetLayout setLayout;
	VkPipelineLayout pipelineLayout;
	VkDescriptorSet descSet;
	VkShaderModule vertexShader;
	VkShaderModule fragmentShader;
	VkRenderPass renderPass;
	u32 subpass;
	PsoSlot slot;
	u32 samples;
} vk_ssaa_resolve;

void vk_ssaa_resolve_construct(vk_ssaa_resolve *r);
bool vk_ssaa_resolve_init(vk_ssaa_resolve *r, VkRenderPass renderPass, u32 subpass, u32 samples, VkImageView samplesView);
/* false: its pipeline could not be made, and nothing was drawn */
bool vk_ssaa_resolve_draw(vk_ssaa_resolve *r, VkCommandBuffer cmd, u32 width, u32 height);
/* The attachment of samples made again: a new descriptor set for it, the
 * old one kept until the frames that use it are done */
void vk_ssaa_resolve_set_samples_view(vk_ssaa_resolve *r, VkImageView samplesView, vk_cmd_pool *pool);
void vk_ssaa_resolve_term(vk_ssaa_resolve *r);

#ifdef __cplusplus
}
#endif

#endif
