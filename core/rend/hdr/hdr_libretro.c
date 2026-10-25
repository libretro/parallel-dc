/* See hdr_libretro.h. C89. */
#include <stdlib.h>
#include <string.h>

#include <libretro.h>

#include "../../libretro/libretro_core_option_defines.h"
#include "hdr_libretro.h"
#include "hdr_output.h"
#include "hdr_display.h"
#include "hdr_transform.h"

/* What Color Format asks for: 0 24-bit, 1 HDR10, 2 scRGB */
static int      hdr_wanted;
/* HDR Display Peak Luminance: 0 the frontend's, -1 the panel's, else nits */
static float    hdr_peak_option;
static float    hdr_black_nits;
static unsigned hdr_meta_cmd = RETRO_ENVIRONMENT_GET_DISPLAY_METADATA;

/* Value keys of the Display Transform option in enum hdr_dt_curve order;
 * the two knees are hdr.glsl's own roll-off */
static const char *const dt_keys[HDR_DT_COUNT] = {
   "reinhard", "aces", "hejl", "gt", "hable", "lottes", "drago", "unreal",
   "filmicalu", "rplain", "rext", "expo", "hable2017", "aces2", "tumblin",
   "ward", "schlick", "devlin", "filmiclog", "jodie", "acesfit", "neutral",
   "agx", "aces2full"
};

/* Options that tune one curve, shown only while it is selected */
static const struct
{
   const char *key;
   const char *curve;
} dt_curve_options[] = {
   { CORE_OPTION_NAME "_hdr_aces2_surround",     "aces2full" },
   { CORE_OPTION_NAME "_hdr_aces2_gamut",        "aces2full" },
   { CORE_OPTION_NAME "_hdr_agx_look",           "agx" },
   { CORE_OPTION_NAME "_hdr_gt_toe",             "gt" },
   { CORE_OPTION_NAME "_hdr_gt_linear",          "gt" },
   { CORE_OPTION_NAME "_hdr_filmiclog_contrast", "filmiclog" },
   { CORE_OPTION_NAME "_hdr_filmiclog_range",    "filmiclog" }
};
#define DT_CURVE_OPTIONS (sizeof(dt_curve_options) / sizeof(dt_curve_options[0]))

/* The options shown only with an HDR Color Format */
static const char *const hdr_only_options[] = {
   CORE_OPTION_NAME "_hdr_display_transform",
   CORE_OPTION_NAME "_hdr_expansion",
   CORE_OPTION_NAME "_hdr_peak",
   CORE_OPTION_NAME "_hdr_highlight_hue",
   CORE_OPTION_NAME "_hdr_sdr_gamma",
   CORE_OPTION_NAME "_hdr_bloom",
   CORE_OPTION_NAME "_hdr_bloom_select",
   CORE_OPTION_NAME "_hdr_bloom_range",
   CORE_OPTION_NAME "_hdr_bloom_convolution",
   CORE_OPTION_NAME "_hdr_halation",
   CORE_OPTION_NAME "_hdr_chromatic",
   CORE_OPTION_NAME "_hdr_post_compact"
};
#define HDR_ONLY_OPTIONS (sizeof(hdr_only_options) / sizeof(hdr_only_options[0]))

/* bit i: dt_curve_options[i] shown; bit 31: the HDR options shown. All
 * start out shown. */
static unsigned options_shown = 0xffffffffu;

static const char *value_of(retro_environment_t env, const char *key)
{
   struct retro_variable var;
   var.key   = key;
   var.value = NULL;
   if (env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      return var.value;
   return NULL;
}

static void show(retro_environment_t env, const char *key, bool visible)
{
   struct retro_core_option_display d;
   d.key     = key;
   d.visible = visible;
   env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &d);
}

void hdr_libretro_options(retro_environment_t env)
{
   static const char *const contrast[7] = {
      "verylow", "low", "medium", "base", "medhigh", "high", "veryhigh" };
   struct hdr_dt_settings   dt;
   struct hdr_post_settings post;
   const char              *v;
   int                    i;

   hdr_wanted = 0;
   if ((v = value_of(env, CORE_OPTION_NAME "_color_format")))
      hdr_wanted = !strcmp(v, "30bit_hdr") ? 1 : !strcmp(v, "64bit_scrgb") ? 2 : 0;

   memset(&dt, 0, sizeof(dt));
   dt.curve           = HDR_DT_KNEE;
   dt.aces2_surround  = 1;
   dt.aces2_gamut     = 3;
   dt.gt_toe          = 0.22f;
   dt.gt_linear       = 0.40f;
   dt.filmic_contrast = 3;
   dt.filmic_max_ev   = 4.026068811f;
   hdr_output.shoulder = 0;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_display_transform")))
   {
      if (!strcmp(v, "knee_exp"))
         hdr_output.shoulder = 1;
      for (i = 1; i < HDR_DT_COUNT; i++)
         if (!strcmp(v, dt_keys[i]))
            dt.curve = i;
   }
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_aces2_surround")))
      dt.aces2_surround = !strcmp(v, "dark") ? 0 : !strcmp(v, "average") ? 2 : 1;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_aces2_gamut")))
      dt.aces2_gamut = !strcmp(v, "rec709") ? 0 : !strcmp(v, "p3") ? 1
                     : !strcmp(v, "rec2020") ? 2 : 3;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_agx_look")))
      dt.agx_punchy = !strcmp(v, "punchy");
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_gt_toe")))
      dt.gt_toe = (float)atof(v);
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_gt_linear")))
      dt.gt_linear = (float)atof(v);
   if (!(dt.gt_toe > 0.0f && dt.gt_toe < 1.0f))
      dt.gt_toe = 0.22f;
   if (!(dt.gt_linear > 0.0f && dt.gt_linear < 1.0f))
      dt.gt_linear = 0.40f;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_filmiclog_contrast")))
      for (i = 0; i < 7; i++)
         if (!strcmp(v, contrast[i]))
            dt.filmic_contrast = i;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_filmiclog_range")))
   {
      float ev = (float)atof(v);
      dt.filmic_max_ev = (ev > 4.5f) ? ev : 4.026068811f;
   }
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_highlight_hue")))
      dt.hue_preserve = !strcmp(v, "enabled");
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_expansion")))
      dt.expansion = !strcmp(v, "inverse") ? 1 : !strcmp(v, "hue") ? 2 : 0;
   hdr_dt_configure(&dt);

   /* the post effects: bloom, halation, chromatic aberration */
   memset(&post, 0, sizeof(post));
   post.firefly_knee = 1.0f;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_bloom")))
      post.bloom = !strcmp(v, "subtle") ? 0.55f : !strcmp(v, "standard") ? 1.0f
                 : !strcmp(v, "intense") ? 1.7f : 0.0f;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_bloom_select")))
      post.bloom_knee = !strcmp(v, "knee");
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_bloom_range")))
      post.firefly_knee = !strcmp(v, "wide") ? 0.25f : !strcmp(v, "widest") ? 0.0625f : 1.0f;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_bloom_convolution")))
      post.convolution = !strcmp(v, "enabled");
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_halation")))
      post.halation = !strcmp(v, "subtle") ? 1 : !strcmp(v, "strong") ? 2 : 0;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_post_compact")))
      post.compact = !strcmp(v, "enabled");
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_chromatic")))
      post.chromatic = !strcmp(v, "subtle") ? 1 : !strcmp(v, "strong") ? 2
                     : !strcmp(v, "heavy") ? 3 : 0;
   hdr_post_configure(&post);

   hdr_output.sdr_eotf = 0;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_sdr_gamma")))
      hdr_output.sdr_eotf = !strcmp(v, "2.2") ? 1 : !strcmp(v, "srgb") ? 2 : 0;

   /* the per-frame refresh applies it over what the frontend reports */
   hdr_peak_option = 0.0f;
   if ((v = value_of(env, CORE_OPTION_NAME "_hdr_peak")) && strcmp(v, "auto"))
      hdr_peak_option = !strcmp(v, "display") ? -1.0f : (float)atof(v);
   if (hdr_peak_option > 0.0f)
      hdr_output.max_nits = hdr_peak_option;
   else if (hdr_peak_option < 0.0f && hdr_output.panel_peak_nits > 0.0f)
      hdr_output.max_nits = hdr_output.panel_peak_nits;
}

bool hdr_libretro_update_display(retro_environment_t env)
{
   const char *v    = value_of(env, CORE_OPTION_NAME "_hdr_display_transform");
   const char *fmt  = value_of(env, CORE_OPTION_NAME "_color_format");
   bool        hdr  = fmt != NULL && strcmp(fmt, "24bit") != 0;
   unsigned    mask = 0;
   unsigned    i;

   if (!v)
      v = "reinhard";
   if (hdr)
   {
      mask = 1u << 31;
      for (i = 0; i < DT_CURVE_OPTIONS; i++)
         if (!strcmp(v, dt_curve_options[i].curve))
            mask |= 1u << i;
   }
   if (mask == options_shown)
      return false;
   if ((mask ^ options_shown) & (1u << 31))
      for (i = 0; i < HDR_ONLY_OPTIONS; i++)
         show(env, hdr_only_options[i], hdr);
   for (i = 0; i < DT_CURVE_OPTIONS; i++)
      if ((mask ^ options_shown) & (1u << i))
         show(env, dt_curve_options[i].key, (mask >> i) & 1);
   options_shown = mask;
   return true;
}

/* ---- the frontend's metadata ---- */

static void metadata_init(struct retro_display_metadata *d)
{
   memset(d, 0, sizeof(*d));
   d->version     = 1;
   d->size        = sizeof(*d);
   d->hdr.version = 1;
   d->hdr.size    = sizeof(d->hdr);
}

/* GET_DISPLAY_METADATA, which carries the HDR block and the panel's own
 * capabilities; GET_HDR_METADATA, the block alone, on a frontend that only
 * has that. False when neither is answered. */
static bool metadata_query(retro_environment_t env, struct retro_display_metadata *d)
{
   metadata_init(d);
   if (hdr_meta_cmd == RETRO_ENVIRONMENT_GET_DISPLAY_METADATA)
   {
      if (env(RETRO_ENVIRONMENT_GET_DISPLAY_METADATA, d))
         return true;
      metadata_init(d);
      hdr_meta_cmd = RETRO_ENVIRONMENT_GET_HDR_METADATA;
   }
   if (hdr_meta_cmd == RETRO_ENVIRONMENT_GET_HDR_METADATA)
   {
      if (env(RETRO_ENVIRONMENT_GET_HDR_METADATA, &d->hdr))
         return true;
      hdr_meta_cmd = 0;
   }
   return false;
}

/* An answered query into what the encodes work from. A zero paper white
 * or peak is not taken; the frontend's own peak and black are its targets
 * and come first, the panel's fill in where it reports none. */
static void metadata_apply(const struct retro_display_metadata *d)
{
   const struct retro_hdr_metadata *h = &d->hdr;
   float black = (h->min_nits > 0.0f) ? h->min_nits : d->caps.min_nits;

   if (h->paper_white_nits > 0.0f)
      hdr_output.paper_white_nits = h->paper_white_nits;
   if (h->max_nits > 0.0f)
      hdr_output.max_nits = h->max_nits;
   else if (d->caps.peak_nits > 0.0f)
      hdr_output.max_nits = d->caps.peak_nits;
   hdr_output.panel_peak_nits = d->caps.peak_nits;
   if (hdr_peak_option > 0.0f)
      hdr_output.max_nits = hdr_peak_option;
   else if (hdr_peak_option < 0.0f && hdr_output.panel_peak_nits > 0.0f)
      hdr_output.max_nits = hdr_output.panel_peak_nits;
   hdr_output.expand_gamut = (int)h->expand_gamut;

   /* (a black at or above paper white is not one) */
   if (!(black > 0.0f && black < hdr_output.paper_white_nits))
      black = 0.0f;
   if (black != hdr_black_nits)
   {
      hdr_black_nits      = black;
      hdr_output.black_pq = (black > 0.0f) ? hdr_display_pq(black) : 0.0f;
   }
   if (memcmp(hdr_output.panel_xy, d->caps.primaries_xy, sizeof(hdr_output.panel_xy)) != 0)
   {
      memcpy(hdr_output.panel_xy, d->caps.primaries_xy, sizeof(hdr_output.panel_xy));
      hdr_display_panel_matrix(hdr_output.panel_xy, hdr_output.to_panel);
   }
}

void hdr_libretro_reset(void)
{
   int shoulder = hdr_output.shoulder;
   int eotf     = hdr_output.sdr_eotf;
   memset(&hdr_output, 0, sizeof(hdr_output));
   hdr_output.format           = HDR_OUTPUT_NONE;
   /* what a frontend that says nothing gets: the HDR10 reference peak, and
    * the paper white RetroArch defaults to */
   hdr_output.paper_white_nits = 200.0f;
   hdr_output.max_nits         = 1000.0f;
   hdr_output.shoulder         = shoulder;
   hdr_output.sdr_eotf         = eotf;
   hdr_black_nits              = 0.0f;
   hdr_meta_cmd                = RETRO_ENVIRONMENT_GET_DISPLAY_METADATA;
}

bool hdr_libretro_negotiate(retro_environment_t env)
{
   struct retro_display_metadata meta;
   enum retro_pixel_format       fmt;

   hdr_libretro_reset();
   if (hdr_wanted == 0)
      return false;

   /* scRGB only to a frontend that lists it: one from before it existed
    * cannot, and must not be handed a format number it may take without
    * knowing it. It applies no SDR expansion and no paper white of its
    * own to it, so its taking it is enough. Refused, HDR10 is next. */
   if (hdr_wanted == 2 && metadata_query(env, &meta)
         && (meta.hdr.source_formats & RETRO_HDR_SOURCE_FORMAT_SCRGB_RGBA16F))
   {
      fmt = RETRO_PIXEL_FORMAT_SCRGB_RGBA16F;
      if (env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      {
         metadata_apply(&meta);
         hdr_output.format = HDR_OUTPUT_SCRGB;
         return true;
      }
   }

   /* A frontend that cannot present HDR10 refuses it rather than narrowing
    * it, so its taking it means the frame is presented as PQ. One that is
    * presenting HDR but says it would grade HDR10 input again (its own
    * paper white, curve or expansion on top) gets the 24-bit picture its
    * composition is made for. With output mode 0 nothing is presented yet
    * (RetroArch says so until its video driver is up), and passthrough
    * then says nothing. */
   fmt = RETRO_PIXEL_FORMAT_HDR10_2101010;
   if (env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      bool answered = metadata_query(env, &meta);
      if (answered && meta.hdr.output_mode != 0 && !meta.hdr.passthrough)
      {
         fmt = RETRO_PIXEL_FORMAT_XRGB8888;
         env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
         return false;
      }
      if (answered)
         metadata_apply(&meta);
      hdr_output.format = HDR_OUTPUT_HDR10;
      return true;
   }
   fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt);
   return false;
}

bool hdr_libretro_wanted(void)
{
   return hdr_wanted != 0;
}

void hdr_libretro_refresh(retro_environment_t env)
{
   struct retro_display_metadata meta;
   if (hdr_output.format != HDR_OUTPUT_NONE && metadata_query(env, &meta))
      metadata_apply(&meta);
}
