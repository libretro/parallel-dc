/* See hdr_output.h. C89. */
#include <string.h>

#include "hdr_output.h"

struct hdr_output hdr_output;

void hdr_encode_params_fill(struct hdr_encode_params *p,
      const struct hdr_output *o, int deband)
{
   unsigned c, r;
   memset(p, 0, sizeof(*p));
   for (c = 0; c < 3; c++)
      for (r = 0; r < 3; r++)
         p->to_panel[c * 4 + r] = o->to_panel[c * 3 + r];
   p->paper_white  = o->paper_white_nits;
   p->peak         = o->max_nits;
   p->black_pq     = o->black_pq;
   p->expand_gamut = o->expand_gamut;
   p->shoulder     = o->shoulder;
   p->sdr_eotf     = o->sdr_eotf;
   p->scrgb        = o->format == HDR_OUTPUT_SCRGB;
   p->deband       = deband;
   p->ssaa_log2    = 0;
   p->pad[0] = p->pad[1] = p->pad[2] = 0;
}
