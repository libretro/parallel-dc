/* The compute renderer on Vulkan (Rendering Method: Compute): a frame
 * drawn to the screen by the kernels of rend/tile/pvr_compute.h, tile by
 * tile, into the per-pixel renderer's own picture, which goes to the
 * frontend as that renderer's does.
 *
 * - Its input is the CPU's parse of the frame (ta_parse_vdrc()): the
 *   vertices, indices and modifier volume triangles go up once, with the
 *   polygon parameters and the render passes, into this frame's piece of
 *   a ring of host-visible memory the kernels read in place. Or, for a
 *   frame the GPU decoded (TA processing: GPU, vulkan/vk_ta_decoder.h), the
 *   decode's own buffers, read in place on the device: only the polygon
 *   parameters of its draw groups and the render passes go up, and a first
 *   kernel numbers the triangles from the decode's commands and translucent
 *   draw records (PVRC_K_GPUSRC). Nothing of the frame is parsed or copied
 *   on the CPU; the decode's last barrier is for compute shaders' reads
 *   too, and the next decode's first one waits for them.
 * - Everything is made in vk_compute_drawer_init(), when the renderer starts or changes
 *   size: the kernels' pipelines (from SPIR-V made with the source,
 *   vulkan/compute_spv.inc), the ring, the primitives' setup and the bin
 *   pool on the device, sized for the largest frame the machine sends and
 *   the picture's size; nothing is allocated or compiled while a game runs.
 * - A frame is one command buffer of dispatches with barriers between
 *   them; nothing is read back and nothing waits. A few counts the GPU
 *   keeps (runs walked the slow way, triangles left out) are copied to
 *   host memory and read when the frame's slot comes round, after the wait
 *   for its fence the command pool does anyway.
 * - A render to a texture is drawn the same way, at one sample a pixel
 *   as the per-pixel renderer draws it, into the very texture that
 *   renderer would have drawn it in (OITTextureDrawer::ComputeBegin()):
 *   the size of the frame from its first pixel to the pixel clip's last,
 *   in powers of two, times Render To Texture Upscaling, cleared outside
 *   the clip. The texture cache, the lock on its video memory and the
 *   copy to video memory when the game touches it (rtt_read.h) are that
 *   renderer's, unchanged. Its bins are made for the largest such texture.
 * - A frame it cannot draw is left to the raster renderer (vk_compute_drawer_can_draw()):
 *   more textures than it has slots for, a texture rendered to that the
 *   device cannot write from a compute shader, the VMUs or crosshairs
 *   drawn over the picture, more render passes than the kernels take. */
#ifndef VK_COMPUTE_DRAWER_H
#define VK_COMPUTE_DRAWER_H
#include "vk_context.h"
#include "vk_cmd_pool.h"
#include "vk_image.h"
#include "vk_gpu_timer.h"
#include "vk_motion_pass.h"
#include "vk_ta_decoder.h"
#include "vk_texture.h"
#include "rend/transform_matrix.h"
#include "rend/tile/compute_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The frame's words for the kernels (PVRC_F_*), from the render context
 * (pvrrc), by the Vulkan renderers' transforms and clip rectangles. Also
 * what the tests build their frames with (tools/rend/tile_dump). */
typedef struct vk_compute_frame
{
	transform_matrix_t matrices;
} vk_compute_frame;

void vk_compute_frame_init(vk_compute_frame *f);
/* compute_frame_renderer's: the viewport matrix of the frame, the screen's
 * under the temporal upscaler's jitter (rend/upscaler.h), as the raster
 * renderers draw it; its base scissor; its provoking vertices */
const xform *vk_compute_frame_begin(vk_compute_frame *f);
void vk_compute_frame_base_scissor(vk_compute_frame *f, int *x, int *y, int *width, int *height);
void vk_compute_frame_provoking_vertices(vk_compute_frame *f);

struct vk_compute_res;

typedef struct vk_compute_drawer
{
	struct vk_compute_res *res;
	bool ready;
	bool failed;
	u32 samples;
	bool depth;
	VkExtent2D extent;
	vk_cmd_pool *pool;
	vk_motion_pass *motion;
	int recordedSlot;
	vk_samplers *samplers;
	const vk_ta_gpu_draw *gpuDraw;	/* what a frame decoded on the GPU is read from (pvrrc.gpu) */
	vk_compute_frame frame;
	vk_gpu_timer timer;
	/* what is logged at term */
	u64 statFrames;
	u64 statGpuFrames;
	u64 statRttFrames;
	u64 statFallback;
	u64 statWhy[CF_WHY_COUNT];
} vk_compute_drawer;

void vk_compute_drawer_construct(vk_compute_drawer *d);
/* Whether the device has what the kernels need (the reason it has not,
 * or NULL): what init checks before it looks at the frame size and the
 * video memory, for what is set up for the compute renderer before init
 * can be called */
const char *vk_compute_drawer_device_lacks(void);
/* Made for a picture of @width x @height and @samples a pixel, for @pool's
 * slots; false when the device cannot have it (logged). @depth: for the
 * frontend's temporal upscaler (rend/upscaler.h), the screen's walk stores
 * each pixel's depth in an image of the slot's, sampled after the frame. */
bool vk_compute_drawer_init(vk_compute_drawer *d, u32 width, u32 height, u32 samples, vk_cmd_pool *pool, vk_samplers *samplers,
		bool depth);
void vk_compute_drawer_term(vk_compute_drawer *d);
/* whether the frame in pvrrc is one this draws (before anything is recorded) */
bool vk_compute_drawer_can_draw(const vk_compute_drawer *d);
/* The frame's words and textures, @last: started from the picture before
 * (@scaled: one of another size, kept from another context); a render to
 * a texture, into one of @rttWidth x @rttHeight. False: it does not fit,
 * and the raster renderer draws it; nothing is recorded either way, and
 * nothing of the textures is looked at. */
bool vk_compute_drawer_prepare(vk_compute_drawer *d, bool last, bool scaled, const vk_texture *paletteTexture,
		u32 rttWidth, u32 rttHeight);
/* ...then drawn into @image (its view @view), left in the layout the raster
 * renderer leaves its picture in; @last, the picture before (or null). Its
 * own command buffer of the pool's, submitted with the frame's others. The
 * motion vector pass (vk_compute_drawer::motion), when there is one, drawn
 * over it with the frame's vertices and indices from its words. False if
 * a kernel's pipeline could not be made. */
bool vk_compute_drawer_record(vk_compute_drawer *d, VkImage image, VkImageView view, VkImageView last);
/* the slots' depth views (vk_compute_drawer::pool's count of them; none
 * without @depth): @views gets as many as there are, returned */
u32 vk_compute_drawer_depth_views(const vk_compute_drawer *d, VkImageView *views, u32 most);
/* the image and view of the slot the frame just recorded went to */
VkImage vk_compute_drawer_depth_image(const vk_compute_drawer *d);
VkImageView vk_compute_drawer_depth_view(const vk_compute_drawer *d);
int vk_compute_drawer_slot(const vk_compute_drawer *d);
/* With the HDR output, the channel sums of what the screen's frame just
 * recorded resolved, for the encode (vkc_hdr_encode()); null when it
 * stored none, or they would add nothing */
VkImageView vk_compute_drawer_sums(const vk_compute_drawer *d);

/* a frame left to the raster renderer, and why (CF_WHY_*) */
static INLINE void vk_compute_drawer_fallback(vk_compute_drawer *d, u32 why)
{
	d->statFallback++;
	d->statWhy[why < CF_WHY_COUNT ? why : 0]++;
}

#ifdef __cplusplus
}
#endif

#endif
