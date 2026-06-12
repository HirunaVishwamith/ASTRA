/* ctx_common.c — public context API, dispatched to the active backend. */
#include "glctx_internal.h"

void glctx_make_current(GLCtx *c)            { c->vt->make_current(c); }
int  glctx_load_gl(GLCtx *c)                 { return c->vt->load_gl(c); }
int  glctx_swap_and_poll(GLCtx *c)           { return c->vt->swap_and_poll(c); }
void glctx_get_input(GLCtx *c, VizInput *in) { c->vt->get_input(c, in); }
int  glctx_read_rgb(GLCtx *c, Image *im)     { return c->vt->read_rgb(c, im); }
void glctx_size(GLCtx *c, int *w, int *h)    { if (w) *w = c->w; if (h) *h = c->h; }
void glctx_destroy(GLCtx *c)                 { if (c) c->vt->destroy(c); }
