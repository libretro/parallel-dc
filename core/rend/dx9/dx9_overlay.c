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
#include "dx9_overlay.h"
#include "dx9context_lr.h"

static IDirect3DTexture9 *vmus[4];
static IDirect3DTexture9 *crosshairs[4];

/* (B G R A, as the card reads it) */
static bool upload(IDirect3DTexture9 **texture, int width, int height, const u8 *bgra)
{
	D3DLOCKED_RECT rect;
	int y;
	if (*texture == NULL)
	{
		*texture = dx9c_make_texture(width, height, 1, dx9c_write_usage(), D3DFMT_A8R8G8B8);
		if (*texture == NULL)
			return false;
	}
	if (FAILED(IDirect3DTexture9_LockRect(*texture, 0, &rect, NULL, dx9c_write_lock_flags())))
		return false;
	for (y = 0; y < height; y++)
		memcpy((u8 *)rect.pBits + (size_t)y * rect.Pitch, bgra + (size_t)y * width * 4, (size_t)width * 4);
	IDirect3DTexture9_UnlockRect(*texture, 0);
	return true;
}

void dx9_overlay_prepare(void)
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
			dx9c_recycle(&vmus[i]);
			continue;
		}
		if (vmus[i] != NULL && !changed && !param->vmu_screen_needs_update)
			continue;

		src = vmu_lcd_pixels(i);
		for (n = 0; n < VMU_SCREEN_HEIGHT * VMU_SCREEN_WIDTH; n++)
		{
			if (*src++)
			{
				*dst++ = param->vmu_pixel_on_B;
				*dst++ = param->vmu_pixel_on_G;
				*dst++ = param->vmu_pixel_on_R;
			}
			else
			{
				*dst++ = param->vmu_pixel_off_B;
				*dst++ = param->vmu_pixel_off_G;
				*dst++ = param->vmu_pixel_off_R;
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
			dx9c_recycle(&crosshairs[i]);
			continue;
		}
		if (crosshairs[i] != NULL && !gun->dirty)
			continue;

		colour = &lightgun_palette[gun->colour * 3];
		src = lightgun_img_crosshair;
		for (n = 0; n < LIGHTGUN_CROSSHAIR_SIZE * LIGHTGUN_CROSSHAIR_SIZE; n++)
		{
			const bool on = *src++ != 0;
			*dst++ = on ? colour[2] : 0;
			*dst++ = on ? colour[1] : 0;
			*dst++ = on ? colour[0] : 0;
			*dst++ = on ? 0xff : 0;
		}
		if (upload(&crosshairs[i], LIGHTGUN_CROSSHAIR_SIZE, LIGHTGUN_CROSSHAIR_SIZE, pixels))
			gun->dirty = false;
	}
}

/* (pixels to clip space, y going up; the pictures' first line is their
 * bottom one, as the other renderers draw them) */
static void draw(IDirect3DTexture9 *texture, int width, int height, float x, float y, float w, float h, float alpha)
{
	dx9c_draw_quad_at(texture, false, false, x * 2.f / width - 1.f, 1.f - (y + h) * 2.f / height,
			w * 2.f / width, h * 2.f / height, 0.f, 1.f, 1.f, 0.f, true, alpha);
}

void dx9_overlay_draw(int width, int height)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		const vmu_screen_params_t *param = &vmu_screen_params[i];
		const float w = (float)(VMU_SCREEN_WIDTH * param->vmu_screen_size_mult);
		const float h = (float)(VMU_SCREEN_HEIGHT * param->vmu_screen_size_mult);
		float x = 0.f;
		float y = 0.f;
		if (vmus[i] == NULL)
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
		/* (the screen's pixels are opaque: blended by the opacity alone) */
		draw(vmus[i], width, height, x, y, w, h, param->vmu_screen_opacity / 255.f);
	}

	for (i = 0; i < 4; i++)
	{
		const float w = LIGHTGUN_CROSSHAIR_SIZE;
		const float h = LIGHTGUN_CROSSHAIR_SIZE;
		/* (the game's 640x480 is in the middle of a widescreen picture) */
		const float sidebar = (width - height * 4.f / 3.f) / 2;
		const float x = sidebar + lightgun_params[i].x * height / 480 - w / 2;
		const float y = lightgun_params[i].y * height / 480 - h / 2;
		if (crosshairs[i] == NULL)
			continue;
		draw(crosshairs[i], width, height, x, y, w, h, 1.f);
	}
}

bool dx9_overlay_empty(void)
{
	int i;
	for (i = 0; i < 4; i++)
		if (vmus[i] != NULL || crosshairs[i] != NULL)
			return false;
	return true;
}

void dx9_overlay_term(void)
{
	int i;
	for (i = 0; i < 4; i++)
	{
		DX9_RELEASE(vmus[i]);
		DX9_RELEASE(crosshairs[i]);
	}
}
