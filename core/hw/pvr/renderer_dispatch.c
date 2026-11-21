/* The renderer in use: what Renderer_if.c asks of it (Renderer_if.h) */
#include "Renderer_if.h"

static const renderer_ops *rend;
static const renderer_ops *fallback;

static void rend_release(const renderer_ops *ops)
{
	if (ops->release != NULL)
		ops->release();
}

void renderer_create(void)
{
	const renderer_ops *fb = NULL;

	switch (settings.pvr.rend)
	{
	default:
	case 0:
		NOTICE_LOG(PVR, "Creating Open GL per-triangle/strip renderer\n");
		rend = rend_GLES2();
		break;
#if defined(HAVE_GL4)
	case 3:
		NOTICE_LOG(PVR, "Creating Open GL per-pixel renderer\n");
		rend = rend_GL4();
		fb = rend_GLES2();
		break;
#endif
#ifdef HAVE_VULKAN
	case 4:
		NOTICE_LOG(PVR, "Creating Vulkan per-triangle/strip renderer\n");
		rend = rend_Vulkan();
		break;
	case 5:
		NOTICE_LOG(PVR, "Creating Vulkan per-pixel renderer\n");
		rend = rend_OITVulkan();
		break;
#endif
#ifdef HAVE_D3D11
	case 6:
		NOTICE_LOG(PVR, "Creating Direct3D 11 per-triangle/strip renderer\n");
		rend = rend_D3D11();
		break;
	case 7:
		NOTICE_LOG(PVR, "Creating Direct3D 11 per-pixel renderer\n");
		rend = rend_OITD3D11();
		fb = rend_D3D11();
		break;
#endif
#ifdef HAVE_D3D9
	case 8:
		NOTICE_LOG(PVR, "Creating Direct3D 9 per-triangle/strip renderer\n");
		rend = rend_D3D9();
		break;
#endif
#ifdef HAVE_D3D12
	case 9:
		NOTICE_LOG(PVR, "Creating Direct3D 12 per-triangle/strip renderer\n");
		rend = rend_D3D12();
		break;
	case 10:
		NOTICE_LOG(PVR, "Creating Direct3D 12 per-pixel renderer\n");
		rend = rend_OITD3D12();
		fb = rend_D3D12();
		break;
#endif
	}
	if (rend->create != NULL)
		rend->create();
	if (fb != NULL)
	{
		fallback = fb;
		if (fb->create != NULL)
			fb->create();
	}
}

bool renderer_init(void)
{
	if (!rend->init())
	{
		rend_release(rend);
		rend = NULL;
		if (fallback == NULL || !fallback->init())
		{
			if (fallback != NULL)
				rend_release(fallback);
			fallback = NULL;
			return false;
		}
		INFO_LOG(PVR, "Selected renderer initialization failed. Falling back to default renderer.\n");
		rend = fallback;
		fallback = NULL;
	}
	return true;
}

void renderer_destroy(void)
{
	if (rend != NULL)
	{
		rend->term();
		rend_release(rend);
		rend = NULL;
	}
	if (fallback != NULL)
	{
		rend_release(fallback);
		fallback = NULL;
	}
}

bool renderer_exists(void)
{
	return rend != NULL;
}

void renderer_keep_picture(void)
{
	if (rend != NULL && rend->keep_picture != NULL)
		rend->keep_picture();
}

bool renderer_present_kept_picture(void)
{
	return rend != NULL && rend->present_kept_picture != NULL
		&& rend->present_kept_picture();
}

void renderer_resize(int width, int height)
{
	if (rend != NULL)
		rend->resize(width, height);
}

bool renderer_process(TA_context *ctx)
{
	return rend->process(ctx);
}

u64 renderer_get_texture(u32 tsp, u32 tcw, int area)
{
	return rend->get_texture != NULL ? rend->get_texture(tsp, tcw, area) : 0;
}

bool renderer_render(void)
{
	return rend->render();
}

void renderer_present(void)
{
	if (rend->present != NULL)
		rend->present();
}

bool renderer_ready(void)
{
	return rend == NULL || rend->ready == NULL || rend->ready();
}

void renderer_options_changed(void)
{
	if (rend != NULL && rend->options_changed != NULL)
		rend->options_changed();
}
