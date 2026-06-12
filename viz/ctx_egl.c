/* ctx_egl.c — offscreen OpenGL 3.3 core context via EGL pbuffer.
 * Used for headless golden-image rendering (no display required). */
#include "glctx.h"
#include <EGL/egl.h>
#include <GL/gl.h>
#include <stdlib.h>
#include <stdio.h>

struct GLCtx {
    GLCtxKind  kind;
    int        w, h;
    EGLDisplay dpy;
    EGLContext ctx;
    EGLSurface surf;
};

GLCtx *glctx_egl_create(int w, int h) {
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (dpy == EGL_NO_DISPLAY) { fprintf(stderr, "egl: no display\n"); return NULL; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) { fprintf(stderr, "egl: init failed\n"); return NULL; }
    if (!eglBindAPI(EGL_OPENGL_API)) { fprintf(stderr, "egl: bind OpenGL failed\n"); return NULL; }

    const EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_NONE
    };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
        fprintf(stderr, "egl: no matching config\n"); return NULL;
    }
    const EGLint pb_attr[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (surf == EGL_NO_SURFACE) { fprintf(stderr, "egl: pbuffer failed\n"); return NULL; }

    const EGLint ctx_attr[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE
    };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (ctx == EGL_NO_CONTEXT) { fprintf(stderr, "egl: context failed\n"); return NULL; }

    GLCtx *c = (GLCtx *)calloc(1, sizeof(*c));
    c->kind = GLCTX_EGL; c->w = w; c->h = h;
    c->dpy = dpy; c->ctx = ctx; c->surf = surf;
    eglMakeCurrent(dpy, surf, surf, ctx);
    return c;
}

void glctx_make_current(GLCtx *c) {
    eglMakeCurrent(c->dpy, c->surf, c->surf, c->ctx);
}

void glctx_size(GLCtx *c, int *w, int *h) { if (w) *w = c->w; if (h) *h = c->h; }

int glctx_swap_and_poll(GLCtx *c) { (void)c; return 1; }   /* offscreen: nothing */

int glctx_read_rgb(GLCtx *c, Image *im) {
    if (!im->rgb && !image_alloc(im, c->w, c->h)) return 0;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, c->w, c->h, GL_RGB, GL_UNSIGNED_BYTE, im->rgb);
    image_flip_y(im);            /* GL is bottom-up; PNG is top-down */
    return 1;
}

void glctx_destroy(GLCtx *c) {
    if (!c) return;
    eglMakeCurrent(c->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(c->dpy, c->surf);
    eglDestroyContext(c->dpy, c->ctx);
    eglTerminate(c->dpy);
    free(c);
}
