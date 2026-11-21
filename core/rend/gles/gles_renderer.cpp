/* The OpenGL renderers (gles.c, gl4/gles.c: renderer_ops) as the core's
 * renderers' C++ interface has them: until that interface is C */
#include "hw/pvr/Renderer_if.h"

Renderer* rend_GLES2() { return renderer_from_ops(&gles_renderer_ops); }
#ifdef HAVE_GL4
Renderer* rend_GL4() { return renderer_from_ops(&gl4_renderer_ops); }
#endif
