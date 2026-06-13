/* glctx_internal.h — shared layout + vtable so the EGL and GLX backends can
 * coexist in one binary. Public dispatch lives in ctx_common.c; each backend
 * supplies a creation function and its method table. */
#ifndef ASTRA_GLCTX_INTERNAL_H
#define ASTRA_GLCTX_INTERNAL_H

#include "glctx.h"

typedef struct GLCtxVT {
    void (*make_current)(GLCtx *);
    int  (*load_gl)(GLCtx *);
    int  (*swap_and_poll)(GLCtx *);
    void (*get_input)(GLCtx *, VizInput *);
    int  (*read_rgb)(GLCtx *, Image *);
    void (*destroy)(GLCtx *);
} GLCtxVT;

struct GLCtx {
    GLCtxKind      kind;
    int            w, h;
    const GLCtxVT *vt;
    void          *impl;   /* backend-specific state */
};

#endif /* ASTRA_GLCTX_INTERNAL_H */
