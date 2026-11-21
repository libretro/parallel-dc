/* The per-triangle OpenGL renderer (gles.c, a renderer_ops) as the core's
 * renderers' C++ interface has them: until that interface is C */
#include "hw/pvr/Renderer_if.h"

Renderer* rend_GLES2() { return renderer_from_ops(&gles_renderer_ops); }
