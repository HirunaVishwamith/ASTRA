/* ctx_glx.c — on-screen OpenGL 3.3 core context via X11 + GLX.
 * Interactive window backend (what GLFW wraps on Linux). Pumps X events into a
 * VizInput the GUI app drains each frame. */
#include "glctx_internal.h"
#include "glfn.h"
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <GL/glx.h>
#include <GL/gl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

typedef GLXContext (*PFN_glXCreateContextAttribsARB)(
    Display *, GLXFBConfig, GLXContext, Bool, const int *);

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB  0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef struct {
    Display   *dpy;
    Window     win;
    GLXContext ctx;
    Atom       wm_delete;
    int        dragging, last_x, last_y;
    int        press_x, press_y, moved;   /* click-vs-drag discrimination */
    VizInput   acc;
} GlxImpl;

static void glx_make_current(GLCtx *c) {
    GlxImpl *g = (GlxImpl *)c->impl;
    glXMakeCurrent(g->dpy, g->win, g->ctx);
}
static void *glx_getproc(const char *name) {
    return (void *)glXGetProcAddressARB((const GLubyte *)name);
}
static int glx_load_gl(GLCtx *c) { (void)c; return glfn_load(glx_getproc); }

static void handle_key(GlxImpl *g, KeySym ks) {
    switch (ks) {
        case XK_Escape: case XK_q: case XK_Q: g->acc.quit = 1; break;
        case XK_p: case XK_P: g->acc.toggle_pause = 1; break;
        case XK_r: case XK_R: g->acc.reboot = 1; break;
        case XK_s: case XK_S: g->acc.strike = 1; break;
        case XK_m: case XK_M: g->acc.route_mode = 1; break;
        case XK_f: case XK_F: g->acc.toggle_focus = 1; break;
        case XK_Down: case XK_bracketright: g->acc.sel_next = 1; break;
        case XK_Up:   case XK_bracketleft:  g->acc.sel_prev = 1; break;
        default: break;
    }
}

static int glx_swap(GLCtx *c) {
    GlxImpl *g = (GlxImpl *)c->impl;
    glXSwapBuffers(g->dpy, g->win);
    int open = 1;
    while (XPending(g->dpy)) {
        XEvent ev; XNextEvent(g->dpy, &ev);
        switch (ev.type) {
        case ConfigureNotify:
            c->w = ev.xconfigure.width; c->h = ev.xconfigure.height; break;
        case ClientMessage:
            if ((Atom)ev.xclient.data.l[0] == g->wm_delete) { open = 0; g->acc.quit = 1; }
            break;
        case KeyPress: handle_key(g, XLookupKeysym(&ev.xkey, 0)); break;
        case ButtonPress:
            if (ev.xbutton.button == Button1) {
                g->dragging = 1; g->moved = 0;
                g->last_x = g->press_x = ev.xbutton.x;
                g->last_y = g->press_y = ev.xbutton.y;
            }
            else if (ev.xbutton.button == Button4) g->acc.scroll += 1.0f;
            else if (ev.xbutton.button == Button5) g->acc.scroll -= 1.0f;
            break;
        case ButtonRelease:
            if (ev.xbutton.button == Button1) {
                g->dragging = 0;
                if (!g->moved) {           /* a click, not a drag -> pick */
                    g->acc.click = 1;
                    g->acc.click_x = (float)ev.xbutton.x;
                    g->acc.click_y = (float)ev.xbutton.y;
                }
            }
            break;
        case MotionNotify:
            if (g->dragging) {
                g->acc.dx += (float)(ev.xmotion.x - g->last_x);
                g->acc.dy += (float)(ev.xmotion.y - g->last_y);
                g->last_x = ev.xmotion.x; g->last_y = ev.xmotion.y;
                if (abs(ev.xmotion.x - g->press_x) + abs(ev.xmotion.y - g->press_y) > 4) g->moved = 1;
            }
            break;
        default: break;
        }
    }
    return open;
}

static void glx_input(GLCtx *c, VizInput *in) {
    GlxImpl *g = (GlxImpl *)c->impl;
    *in = g->acc;
    g->acc = (VizInput){0};
}

static int glx_read_rgb(GLCtx *c, Image *im) {
    if (!im->rgb && !image_alloc(im, c->w, c->h)) return 0;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, c->w, c->h, GL_RGB, GL_UNSIGNED_BYTE, im->rgb);
    image_flip_y(im);
    return 1;
}

static void glx_destroy(GLCtx *c) {
    GlxImpl *g = (GlxImpl *)c->impl;
    glXMakeCurrent(g->dpy, None, NULL);
    glXDestroyContext(g->dpy, g->ctx);
    XDestroyWindow(g->dpy, g->win);
    XCloseDisplay(g->dpy);
    free(g); free(c);
}

static const GLCtxVT GLX_VT = {
    glx_make_current, glx_load_gl, glx_swap, glx_input, glx_read_rgb, glx_destroy
};

GLCtx *glctx_glx_create(int w, int h, const char *title) {
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "glx: cannot open display\n"); return NULL; }
    int screen = DefaultScreen(dpy);

    int fb_attr[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_DOUBLEBUFFER, True,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24,
        None
    };
    int ncfg = 0;
    GLXFBConfig *cfgs = glXChooseFBConfig(dpy, screen, fb_attr, &ncfg);
    if (!cfgs || ncfg < 1) { fprintf(stderr, "glx: no fbconfig\n"); XCloseDisplay(dpy); return NULL; }
    GLXFBConfig fbc = cfgs[0];
    XFree(cfgs);

    XVisualInfo *vi = glXGetVisualFromFBConfig(dpy, fbc);
    if (!vi) { fprintf(stderr, "glx: no visual\n"); XCloseDisplay(dpy); return NULL; }
    Colormap cmap = XCreateColormap(dpy, RootWindow(dpy, vi->screen), vi->visual, AllocNone);
    XSetWindowAttributes swa;
    memset(&swa, 0, sizeof(swa));
    swa.colormap = cmap;
    swa.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    Window win = XCreateWindow(dpy, RootWindow(dpy, vi->screen), 0, 0,
                               (unsigned)w, (unsigned)h, 0, vi->depth, InputOutput,
                               vi->visual, CWColormap | CWEventMask, &swa);
    XFree(vi);
    if (!win) { fprintf(stderr, "glx: cannot create window\n"); XCloseDisplay(dpy); return NULL; }
    XStoreName(dpy, win, title ? title : "ASTRA");
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    XMapWindow(dpy, win);

    PFN_glXCreateContextAttribsARB createctx =
        (PFN_glXCreateContextAttribsARB)glXGetProcAddressARB(
            (const GLubyte *)"glXCreateContextAttribsARB");
    if (!createctx) { fprintf(stderr, "glx: no ARB context creation\n"); XCloseDisplay(dpy); return NULL; }
    int ctx_attr[] = {
        GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
        GLX_CONTEXT_MINOR_VERSION_ARB, 3,
        GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
        None
    };
    GLXContext ctx = createctx(dpy, fbc, 0, True, ctx_attr);
    if (!ctx) { fprintf(stderr, "glx: context creation failed\n"); XCloseDisplay(dpy); return NULL; }
    glXMakeCurrent(dpy, win, ctx);

    GlxImpl *g = (GlxImpl *)calloc(1, sizeof(*g));
    g->dpy = dpy; g->win = win; g->ctx = ctx; g->wm_delete = wm_delete;
    GLCtx *c = (GLCtx *)calloc(1, sizeof(*c));
    c->kind = GLCTX_GLX; c->w = w; c->h = h; c->vt = &GLX_VT; c->impl = g;
    return c;
}
