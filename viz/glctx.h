/* glctx.h — minimal OpenGL context abstraction with two backends:
 *   - EGL  : offscreen (headless golden-image rendering, no display)
 *   - GLX  : on-screen X11 window (interactive)
 * Both create a 3.3 core profile context and share the renderer. This replaces
 * GLFW (unavailable here): on Linux GLFW is just a wrapper over exactly these. */
#ifndef ASTRA_GLCTX_H
#define ASTRA_GLCTX_H

#include "image.h"

typedef enum { GLCTX_EGL, GLCTX_GLX } GLCtxKind;

typedef struct GLCtx GLCtx;   /* opaque; backend-specific */

/* Offscreen EGL context (w x h). Returns NULL on failure. */
GLCtx *glctx_egl_create(int w, int h);

/* On-screen GLX window (w x h) with a title. Returns NULL on failure. */
GLCtx *glctx_glx_create(int w, int h, const char *title);

void   glctx_make_current(GLCtx *c);
void   glctx_size(GLCtx *c, int *w, int *h);

/* Load modern GL entry points using this backend's getproc. 1 on success. */
int    glctx_load_gl(GLCtx *c);

/* On-screen only: present the back buffer / pump events. Returns 0 if the
 * window asked to close. Offscreen: no-op, returns 1. */
int    glctx_swap_and_poll(GLCtx *c);

/* Read the current framebuffer into im (allocated to ctx size, top-down). */
int    glctx_read_rgb(GLCtx *c, Image *im);

void   glctx_destroy(GLCtx *c);

#endif /* ASTRA_GLCTX_H */
