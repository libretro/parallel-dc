/* The renderer in use, behind its C++ class: what Renderer_if.c asks of
 * it (Renderer_if.h). */
#include "Renderer_if.h"

Renderer* renderer;
static Renderer* fallback_renderer;

void renderer_create(void)
{
	switch (settings.pvr.rend)
	{
	default:
	case 0:
		NOTICE_LOG(PVR, "Creating Open GL per-triangle/strip renderer\n");
		renderer = rend_GLES2();
		break;
#if defined(HAVE_GL4)
	case 3:
		NOTICE_LOG(PVR, "Creating Open GL per-pixel renderer\n");
		renderer = rend_GL4();
		fallback_renderer = rend_GLES2();
		break;
#endif
#ifdef HAVE_VULKAN
	case 4:
		NOTICE_LOG(PVR, "Creating Vulkan per-triangle/strip renderer\n");
		renderer = rend_Vulkan();
		break;
	case 5:
		NOTICE_LOG(PVR, "Creating Vulkan per-pixel renderer\n");
		renderer = rend_OITVulkan();
		break;
#endif
#ifdef HAVE_D3D11
	case 6:
		NOTICE_LOG(PVR, "Creating Direct3D 11 per-triangle/strip renderer\n");
		renderer = rend_D3D11();
		break;
	case 7:
		NOTICE_LOG(PVR, "Creating Direct3D 11 per-pixel renderer\n");
		renderer = rend_OITD3D11();
		fallback_renderer = rend_D3D11();
		break;
#endif
#ifdef HAVE_D3D9
	case 8:
		NOTICE_LOG(PVR, "Creating Direct3D 9 per-triangle/strip renderer\n");
		renderer = rend_D3D9();
		break;
#endif
#ifdef HAVE_D3D12
	case 9:
		NOTICE_LOG(PVR, "Creating Direct3D 12 per-triangle/strip renderer\n");
		renderer = rend_D3D12();
		break;
	case 10:
		NOTICE_LOG(PVR, "Creating Direct3D 12 per-pixel renderer\n");
		renderer = rend_OITD3D12();
		fallback_renderer = rend_D3D12();
		break;
#endif
	}
}

bool renderer_init(void)
{
	if (!renderer->Init())
	{
		delete renderer;
		renderer = NULL;
		if (fallback_renderer == NULL || !fallback_renderer->Init())
		{
			delete fallback_renderer;
			fallback_renderer = NULL;
			return false;
		}
		INFO_LOG(PVR, "Selected renderer initialization failed. Falling back to default renderer.\n");
		renderer = fallback_renderer;
		fallback_renderer = NULL;	/* avoid double-free */
	}
	return true;
}

void renderer_destroy(void)
{
	if (renderer != NULL)
	{
		renderer->Term();
		delete renderer;
		renderer = NULL;
	}
	if (fallback_renderer != NULL)
	{
		delete fallback_renderer;
		fallback_renderer = NULL;
	}
}

bool renderer_exists(void)
{
	return renderer != NULL;
}

void renderer_keep_picture(void)
{
	if (renderer != NULL)
		renderer->KeepPicture();
}

bool renderer_present_kept_picture(void)
{
	return renderer != NULL && renderer->PresentKeptPicture();
}

void renderer_resize(int width, int height)
{
	if (renderer != NULL)
		renderer->Resize(width, height);
}

bool renderer_process(TA_context *ctx)
{
	return renderer->Process(ctx);
}

bool renderer_render(void)
{
	return renderer->Render();
}

void renderer_present(void)
{
	renderer->Present();
}
