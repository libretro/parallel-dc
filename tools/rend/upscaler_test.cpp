/* rend/upscaler.cpp on frames made here: the strips of a frame matched
 * with the frame before's by header and order, their vertices given where
 * they were; a strip with no match, or one that jumped, standing still;
 * a reset forgetting the frame before; the request's size and the
 * jitter's range; the inputs handed over with the frame's jitter and the
 * reset of the first. See upscaler_test.sh. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include "rend/upscaler.h"
#include "hw/pvr/ta_ctx.h"

static int failures;

// (the lists log when they overrun, which these do not)
void GenericLog(LogTypes::LOG_LEVELS level, LogTypes::LOG_TYPE type, const char *file, int line, const char *fmt, ...)
{
   (void)level; (void)type; (void)file; (void)line; (void)fmt;
}
#define CHECK(cond) \
   do { \
      if (!(cond)) \
      { \
         printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
         failures++; \
      } \
   } while (0)

/* --- a frontend --- */
static unsigned request_w = 1440, request_h = 1080, request_rw = 960, request_rh = 720;
static int request_refused;
static struct retro_hw_upscaler_inputs got;
static int got_count;

static bool get_request(void *handle, struct retro_hw_upscaler_request *r)
{
   (void)handle;
   if (request_refused)
      return false;
   memset(r, 0, sizeof(*r));
   r->output_width  = request_w;
   r->output_height = request_h;
   r->render_width  = request_rw;
   r->render_height = request_rh;
   return true;
}
static void set_inputs(void *handle, const struct retro_hw_upscaler_inputs *inputs)
{
   (void)handle;
   got = *inputs;
   got_count++;
}
static const struct retro_hw_render_upscaler_interface iface = {
   RETRO_HW_RENDER_UPSCALER_INTERFACE_VERSION, NULL, get_request, set_inputs
};

/* --- frames --- */
static rend_context ctx;
static bool overrun;

static void frame_begin()
{
   ctx.verts.Clear();
   ctx.idx.Clear();
   ctx.global_param_op.Clear();
   ctx.global_param_pt.Clear();
}

/* a strip of @n vertices at @x, @y (a row, a step apart), sent under header words @tsp, @tcw */
static void strip(List<PolyParam>& list, u32 tsp, u32 tcw, int n, float x, float y)
{
   PolyParam *pp = list.Append();
   memset(pp, 0, sizeof(*pp));
   pp->first = ctx.idx.used();
   pp->count = n;
   pp->tsp.full = tsp;
   pp->tcw.full = tcw;
   pp->isp.full = 0x90800000u;
   pp->texid = tcw;
   for (int i = 0; i < n; i++)
   {
      Vertex *v = ctx.verts.Append();
      memset(v, 0, sizeof(*v));
      v->x = x + 10.f * i;
      v->y = y + (i & 1) * 10.f;
      v->z = 1.f;
      *ctx.idx.Append() = ctx.verts.used() - 1;
   }
}

static float prev_x(const std::vector<float>& prev, const PolyParam& pp, int v) { return prev[ctx.idx.head()[pp.first + v] * 2]; }
static float prev_y(const std::vector<float>& prev, const PolyParam& pp, int v) { return prev[ctx.idx.head()[pp.first + v] * 2 + 1]; }

int main()
{
   std::vector<float> prev;
   int w, h;
   float jx, jy;
   struct retro_hw_upscaler_image mv, depth;

   ctx.verts.Init(1024, &overrun, "verts");
   ctx.idx.Init(1024, &overrun, "idx");
   ctx.global_param_op.Init(64, &overrun, "op");
   ctx.global_param_pt.Init(64, &overrun, "pt");
   memset(&mv, 0, sizeof(mv));
   memset(&depth, 0, sizeof(depth));
   mv.image = 1;
   depth.image = 2;

   /* No interface: nothing asked, nothing drawn for */
   upscaler_set_interface(NULL);
   CHECK(!upscaler_poll(true) && !upscaler_active());
   upscaler_render_size(&w, &h);
   CHECK(w == 0 && h == 0);
   upscaler_jitter(&jx, &jy);
   CHECK(jx == 0.f && jy == 0.f);

   /* The request: the render size changes once, and holds */
   upscaler_set_interface(&iface);
   CHECK(!upscaler_poll(false) && !upscaler_active());   /* a renderer that does not draw for it */
   CHECK(upscaler_poll(true) && upscaler_active());
   upscaler_render_size(&w, &h);
   CHECK(w == 960 && h == 720);
   CHECK(!upscaler_poll(true));
   request_rw = 853; request_rh = 640;
   CHECK(upscaler_poll(true));
   upscaler_render_size(&w, &h);
   CHECK(w == 853 && h == 640);
   request_refused = 1;
   CHECK(upscaler_poll(true) && !upscaler_active());
   upscaler_render_size(&w, &h);
   CHECK(w == 0 && h == 0);
   request_refused = 0;
   request_rw = 960; request_rh = 720;
   CHECK(upscaler_poll(true) && upscaler_active());

   /* The first frame: nothing before it, every vertex where it is */
   frame_begin();
   strip(ctx.global_param_op, 0x11, 0x100, 4, 100.f, 100.f);
   strip(ctx.global_param_op, 0x22, 0x200, 6, 300.f, 100.f);
   strip(ctx.global_param_op, 0x11, 0x100, 4, 100.f, 300.f);   /* the same header again */
   strip(ctx.global_param_pt, 0x33, 0x300, 3, 500.f, 300.f);
   upscaler_match(ctx, prev);
   CHECK(prev.size() == (size_t)ctx.verts.used() * 2);
   for (int i = 0; i < ctx.verts.used(); i++)
      CHECK(prev[i * 2] == ctx.verts.head()[i].x && prev[i * 2 + 1] == ctx.verts.head()[i].y);
   upscaler_jitter(&jx, &jy);
   CHECK(jx >= -0.5f && jx <= 0.5f && jy >= -0.5f && jy <= 0.5f);
   upscaler_submit(mv, depth, RETRO_HW_UPSCALER_DEPTH_INVERTED);
   CHECK(got_count == 1);
   CHECK((got.flags & RETRO_HW_UPSCALER_RESET) && (got.flags & RETRO_HW_UPSCALER_DEPTH_INVERTED));
   CHECK(got.motion_vectors.image == 1 && got.depth.image == 2);
   CHECK(got.jitter_x == jx && got.jitter_y == jy);
   CHECK(got.mv_scale_x == 1.f && got.mv_scale_y == 1.f);

   /* The second: everything moved a little; the second strip under the
    * first header moved otherwise; the punch-through one went far; a new
    * strip has no match */
   frame_begin();
   strip(ctx.global_param_op, 0x11, 0x100, 4, 103.f, 102.f);
   strip(ctx.global_param_op, 0x22, 0x200, 6, 290.f, 100.f);
   strip(ctx.global_param_op, 0x11, 0x100, 4, 100.f, 310.f);
   strip(ctx.global_param_op, 0x44, 0x400, 4, 400.f, 400.f);
   strip(ctx.global_param_pt, 0x33, 0x300, 3, 100.f, 100.f);
   upscaler_match(ctx, prev);
   {
      const PolyParam *op = ctx.global_param_op.head();
      const PolyParam *pt = ctx.global_param_pt.head();
      for (int v = 0; v < 4; v++)
         CHECK(prev_x(prev, op[0], v) == 100.f + 10.f * v && prev_y(prev, op[0], v) == 100.f + (v & 1) * 10.f);
      for (int v = 0; v < 6; v++)
         CHECK(prev_x(prev, op[1], v) == 300.f + 10.f * v);
      for (int v = 0; v < 4; v++)
         CHECK(prev_y(prev, op[2], v) == 300.f + (v & 1) * 10.f);
      for (int v = 0; v < 4; v++)
         CHECK(prev_x(prev, op[3], v) == 400.f + 10.f * v && prev_y(prev, op[3], v) == 400.f + (v & 1) * 10.f);
      for (int v = 0; v < 3; v++)
         CHECK(prev_x(prev, pt[0], v) == 100.f + 10.f * v);   /* too far: still */
   }
   upscaler_jitter(&jx, &jy);
   upscaler_submit(mv, depth, 0);
   CHECK(got_count == 2 && !(got.flags & RETRO_HW_UPSCALER_RESET));
   CHECK(got.jitter_x == jx && got.jitter_y == jy);
   /* (the jitter moves on each frame) */
   {
      float nx, ny;
      upscaler_jitter(&nx, &ny);
      CHECK(nx != jx || ny != jy);
   }

   /* A strip whose vertex count changed is another strip */
   frame_begin();
   strip(ctx.global_param_op, 0x11, 0x100, 5, 110.f, 110.f);
   upscaler_match(ctx, prev);
   CHECK(prev_x(prev, ctx.global_param_op.head()[0], 0) == 110.f);
   upscaler_submit(mv, depth, 0);

   /* A reset: the frame after it matches nothing, and says so */
   frame_begin();
   strip(ctx.global_param_op, 0x11, 0x100, 5, 120.f, 110.f);
   upscaler_reset();
   upscaler_match(ctx, prev);
   CHECK(prev_x(prev, ctx.global_param_op.head()[0], 0) == 120.f);
   upscaler_submit(mv, depth, 0);
   CHECK(got_count == 4 && (got.flags & RETRO_HW_UPSCALER_RESET));
   frame_begin();
   strip(ctx.global_param_op, 0x11, 0x100, 5, 125.f, 110.f);
   upscaler_match(ctx, prev);
   CHECK(prev_x(prev, ctx.global_param_op.head()[0], 0) == 120.f);
   upscaler_submit(mv, depth, 0);
   CHECK(got_count == 5 && !(got.flags & RETRO_HW_UPSCALER_RESET));

   /* Without a request, nothing is handed over */
   upscaler_set_interface(NULL);
   upscaler_submit(mv, depth, 0);
   CHECK(got_count == 5);

   if (failures)
   {
      printf("upscaler_test: %d failure(s)\n", failures);
      return 1;
   }
   printf("upscaler_test: ok\n");
   return 0;
}
