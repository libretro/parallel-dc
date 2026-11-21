/* ****************************************************************************
 * This file is part of the HqMAME project. It is distributed under         *
 * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0         *
 * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
 *                                                                          *
 * Additionally and as a special exception, the author gives permission     *
 * to link the code of this program with the MAME library (or with modified *
 * versions of MAME that use the same license as MAME), and distribute      *
 * linked combinations including the two. You must obey the GNU General     *
 * Public License in all respects for all of the code used other than MAME. *
 * If you modify this file, you may extend this exception to your version   *
 * of the file, but you are not obligated to do so. If you do not wish to   *
 * do so, delete this exception statement from your version.                *
 * ****************************************************************************/

/* xBRZ 1.5 ("scale by rules", a modified xBR: small image features kept,
 * alpha, slices of rows), in C */
#ifndef XBRZ_H
#define XBRZ_H

#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* @src (@src_width x @src_height, 8 bits a channel from the low byte: R,
 * G, B, A) @factor (2 to 6) times into @trg, the rows [@y_first, @y_last)
 * of @src only; @argb: alpha is blended, else the top byte is not looked
 * at. Slices that do not overlap can be scaled at once by different
 * threads (each reads 2 rows above and below it). */
void xbrz_scale(int factor, const uint32_t *src, uint32_t *trg,
		int src_width, int src_height, bool argb, int y_first, int y_last);

RETRO_END_DECLS

#endif
