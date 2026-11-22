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
#include "dx12_overlay.h"
#include <string.h>

typedef struct overlay_picture
{
	dx12_image image;
	bool shown;
} overlay_picture;

static overlay_picture vmus[4];
static overlay_picture crosshairs[4];
/* (a picture's pixels, made here before they go to the upload ring: the
 * larger of the two, off the stack) */
static u8 scratch[(VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH > LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE
		? VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH : LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE) * 4];

static bool make(overlay_picture *picture, u32 width, u32 height)
{
	dx12_image_init(&picture->image);
	picture->image.state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
	picture->image.width = width;
	picture->image.height = height;
	picture->shown = false;
	if (!dx12_make_texture(dx12c.device, width, height, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
			picture->image.state, &picture->image.resource, NULL))
		return false;
	picture->image.srv = dx12_heap_alloc(&dx12c.srv_heap);
	if (picture->image.srv == ~0u)
		return false;
	dx12c_make_srv(picture->image.resource, DXGI_FORMAT_R8G8B8A8_UNORM, 1, picture->image.srv);
	return true;
}

bool dx12_overlay_init(void)
{
	bool ok = true;
	int i;
	dx12_overlay_term();
	for (i = 0; i < 4; i++)
	{
		ok = ok && make(&vmus[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT);
		ok = ok && make(&crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE);
	}
	if (!ok)
	{
		WARN_LOG(RENDERER, "DX12: the overlay's pictures could not be made\n");
		dx12_overlay_term();
	}
	return ok;
}

static bool upload(overlay_picture *picture, int width, int height, const u8 *rgba)
{
	const u32 pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	dx12_upload up;
	D3D12_TEXTURE_COPY_LOCATION dst, src;
	int y;
	if (picture->image.resource == NULL)
		return false;
	if (!dx12c_upload(pitch * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &up))
		return false;
	for (y = 0; y < height; y++)
		memcpy(up.cpu + (size_t)y * pitch, rgba + (size_t)y * width * 4, (size_t)width * 4);
	dx12c_transition(&picture->image, D3D12_RESOURCE_STATE_COPY_DEST);
	memset(&dst, 0, sizeof(dst));
	dst.pResource = picture->image.resource;
	dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	memset(&src, 0, sizeof(src));
	src.pResource = up.buffer;
	src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	src.PlacedFootprint.Offset = up.offset;
	src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	src.PlacedFootprint.Footprint.Width = width;
	src.PlacedFootprint.Footprint.Height = height;
	src.PlacedFootprint.Footprint.Depth = 1;
	src.PlacedFootprint.Footprint.RowPitch = pitch;
	ID3D12GraphicsCommandList_CopyTextureRegion(dx12c_cmd(), &dst, 0, 0, 0, &src, NULL);
	dx12c_transition(&picture->image, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
	return true;
}

void dx12_overlay_prepare(void)
{
	int i, n;
	for (i = 0; i < 4; i++)
	{
		vmu_screen_params_t *param = &vmu_screen_params[i];
		/* a new picture from the game, if there is one */
		const bool changed = vmu_lcd_refresh(i);
		u8 *dst = scratch;
		const u8 *src;

		if (!param->vmu_screen_display || vmu_lcd_pixels(i) == NULL)
		{
			vmus[i].shown = false;
			continue;
		}
		if (vmus[i].shown && !changed && !param->vmu_screen_needs_update)
			continue;

		src = vmu_lcd_pixels(i);
		for (n = 0; n < VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH; n++)
		{
			if (*src++)
			{
				*dst++ = param->vmu_pixel_on_R;
				*dst++ = param->vmu_pixel_on_G;
				*dst++ = param->vmu_pixel_on_B;
			}
			else
			{
				*dst++ = param->vmu_pixel_off_R;
				*dst++ = param->vmu_pixel_off_G;
				*dst++ = param->vmu_pixel_off_B;
			}
			*dst++ = 0xff;
		}
		if (upload(&vmus[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, scratch))
		{
			param->vmu_screen_needs_update = false;
			vmus[i].shown = true;
		}
	}

	for (i = 0; i < 4; i++)
	{
		lightgun_params_t *gun = &lightgun_params[i];
		u8 *dst = scratch;
		const u8 *colour, *src;
		if (gun->offscreen || gun->colour == 0)
		{
			crosshairs[i].shown = false;
			continue;
		}
		if (crosshairs[i].shown && !gun->dirty)
			continue;

		colour = &lightgun_palette[gun->colour * 3];
		src = lightgun_img_crosshair;
		for (n = 0; n < LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE; n++)
		{
			const bool on = *src++ != 0;
			*dst++ = on ? colour[0] : 0;
			*dst++ = on ? colour[1] : 0;
			*dst++ = on ? colour[2] : 0;
			*dst++ = on ? 0xff : 0;
		}
		if (upload(&crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, scratch))
		{
			gun->dirty = false;
			crosshairs[i].shown = true;
		}
	}
}

/* (pixels to clip space, y going up; the pictures' first line is their
 * bottom one, as the other renderers draw them) */
static void draw(const overlay_picture *picture, int width, int height, float x, float y, float w, float h,
		u32 special, const float *factor)
{
	dx12c_draw_quad_at(picture->image.srv, false, x * 2.f / width - 1.f, 1.f - (y + h) * 2.f / height,
			w * 2.f / width, h * 2.f / height, 0.f, 1.f, 1.f, 0.f, special, factor);
}

void dx12_overlay_draw(int width, int height)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		const vmu_screen_params_t *param = &vmu_screen_params[i];
		float w, h, x = 0.f, y = 0.f, alpha, factor[4];
		if (!vmus[i].shown)
			continue;
		w = (float)(VMU_SCREEN_WIDTH * param->vmu_screen_size_mult);
		h = (float)(VMU_SCREEN_HEIGHT * param->vmu_screen_size_mult);
		switch (param->vmu_screen_position)
		{
		case UPPER_LEFT:
			break;
		case UPPER_RIGHT:
			x = width - w;
			break;
		case LOWER_LEFT:
			y = height - h;
			break;
		case LOWER_RIGHT:
			x = width - w;
			y = height - h;
			break;
		}
		alpha = param->vmu_screen_opacity / 255.f;
		factor[0] = factor[1] = factor[2] = factor[3] = alpha;
		draw(&vmus[i], width, height, x, y, w, h, DX12_BLEND_FACTOR, factor);
	}

	for (i = 0; i < 4; i++)
	{
		const float w = LIGHTGUN_CROSSHAIR_SIZE;
		const float h = LIGHTGUN_CROSSHAIR_SIZE;
		/* (the game's 640x480 is in the middle of a widescreen picture) */
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		float x, y;
		if (!crosshairs[i].shown)
			continue;
		x = sidebar + lightgun_params[i].x * height / 480 - w / 2;
		y = lightgun_params[i].y * height / 480 - h / 2;
		draw(&crosshairs[i], width, height, x, y, w, h, DX12_BLEND_ALPHA, NULL);
	}
}

bool dx12_overlay_empty(void)
{
	int i;
	for (i = 0; i < 4; i++)
		if (vmus[i].shown || crosshairs[i].shown)
			return false;
	return true;
}

static void release_picture(overlay_picture *p)
{
	/* (the GPU may still read it: given back once it is done) */
	if (p->image.resource != NULL)
		dx12c_release((IUnknown *)p->image.resource);
	p->image.resource = NULL;
	dx12c_release_srv(p->image.srv);
	dx12_image_init(&p->image);
	p->shown = false;
}

void dx12_overlay_term(void)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		release_picture(&vmus[i]);
		release_picture(&crosshairs[i]);
	}
}
