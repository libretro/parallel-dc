/*
	Copyright 2021 flyinghead

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
#include "dx11_overlay.h"
#include "dx11context_lr.h"

typedef struct dx11_picture
{
	ID3D11Texture2D *texture;
	ID3D11ShaderResourceView *view;
} dx11_picture;

static dx11_picture vmus[4];
static dx11_picture crosshairs[4];

static void picture_free(dx11_picture *picture)
{
	DX11_RELEASE(picture->view);
	DX11_RELEASE(picture->texture);
}

static bool upload(dx11_picture *picture, int width, int height, const u8 *rgba)
{
	if (picture->texture == NULL)
	{
		D3D11_TEXTURE2D_DESC desc;
		memset(&desc, 0, sizeof(desc));
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(ID3D11Device_CreateTexture2D(dx11c.device, &desc, NULL, &picture->texture)))
			picture->texture = NULL;
		else if (FAILED(ID3D11Device_CreateShaderResourceView(dx11c.device, (ID3D11Resource *)picture->texture, NULL, &picture->view)))
			picture->view = NULL;
		if (picture->view == NULL)
		{
			picture_free(picture);
			return false;
		}
	}
	ID3D11DeviceContext_UpdateSubresource(dx11c.device_context, (ID3D11Resource *)picture->texture, 0, NULL,
			rgba, width * 4, width * height * 4);
	return true;
}

void dx11_overlay_prepare(void)
{
	int i, n;
	for (i = 0; i < 4; i++)
	{
		vmu_screen_params_t *param = &vmu_screen_params[i];
		/* a new picture from the game, if there is one */
		const bool changed = vmu_lcd_refresh(i);
		u8 pixels[VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH * 4];
		u8 *dst = pixels;
		const u8 *src;

		if (!param->vmu_screen_display || vmu_lcd_pixels(i) == NULL)
		{
			picture_free(&vmus[i]);
			continue;
		}
		if (vmus[i].texture != NULL && !changed && !param->vmu_screen_needs_update)
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
		if (upload(&vmus[i], VMU_SCREEN_WIDTH, VMU_SCREEN_HEIGHT, pixels))
			param->vmu_screen_needs_update = false;
	}

	for (i = 0; i < 4; i++)
	{
		lightgun_params_t *gun = &lightgun_params[i];
		u8 pixels[LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE * 4];
		u8 *dst = pixels;
		const u8 *colour;
		const u8 *src;
		if (gun->offscreen || gun->colour == 0)
		{
			picture_free(&crosshairs[i]);
			continue;
		}
		if (crosshairs[i].texture != NULL && !gun->dirty)
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
		if (upload(&crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, pixels))
			gun->dirty = false;
	}
}

/* (pixels to clip space, y going up; the pictures' first line is their
 * bottom one, as the other renderers draw them) */
static void draw(ID3D11ShaderResourceView *view, int width, int height, float x, float y, float w, float h,
		ID3D11BlendState *blend, const float *factor)
{
	dx11c_draw_quad_at(view, dx11_sampler_point(), x * 2.f / width - 1.f, 1.f - (y + h) * 2.f / height,
			w * 2.f / width, h * 2.f / height, 0.f, 1.f, 1.f, 0.f, blend, factor);
}

void dx11_overlay_draw(int width, int height)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		const vmu_screen_params_t *param = &vmu_screen_params[i];
		const float w = (float)(VMU_SCREEN_WIDTH * param->vmu_screen_size_mult);
		const float h = (float)(VMU_SCREEN_HEIGHT * param->vmu_screen_size_mult);
		float x = 0.f;
		float y = 0.f;
		float alpha, factor[4];
		if (vmus[i].view == NULL)
			continue;
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
		draw(vmus[i].view, width, height, x, y, w, h, dx11_blend_factor_alpha(), factor);
	}

	for (i = 0; i < 4; i++)
	{
		const float w = LIGHTGUN_CROSSHAIR_SIZE;
		const float h = LIGHTGUN_CROSSHAIR_SIZE;
		/* (the game's 640x480 is in the middle of a widescreen picture) */
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		const float x = sidebar + lightgun_params[i].x * height / 480 - w / 2;
		const float y = lightgun_params[i].y * height / 480 - h / 2;
		if (crosshairs[i].view == NULL)
			continue;
		draw(crosshairs[i].view, width, height, x, y, w, h, dx11_blend_alpha(), NULL);
	}
}

bool dx11_overlay_empty(void)
{
	int i;
	for (i = 0; i < 4; i++)
		if (vmus[i].view != NULL || crosshairs[i].view != NULL)
			return false;
	return true;
}

void dx11_overlay_term(void)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		picture_free(&vmus[i]);
		picture_free(&crosshairs[i]);
	}
}
