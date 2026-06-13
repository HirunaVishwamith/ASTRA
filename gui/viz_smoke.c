/* viz_smoke.c — milestone 1: prove the headless GL + readback + PNG pipeline.
 * Creates an offscreen EGL context, clears to a known colour, reads it back,
 * writes a PNG, and verifies the centre pixel matches. No shaders yet. */
#include "glctx.h"
#include "image.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "viz_smoke.png";
    const int W = 320, H = 240;

    GLCtx *c = glctx_egl_create(W, H);
    if (!c) { fprintf(stderr, "EGL context creation failed\n"); return 1; }

    printf("GL_VERSION : %s\n", glGetString(GL_VERSION));
    printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));

    glViewport(0, 0, W, H);
    glClearColor(0.10f, 0.30f, 0.85f, 1.0f);     /* a recognisable blue */
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glFinish();

    Image im = {0};
    if (!glctx_read_rgb(c, &im)) { fprintf(stderr, "readback failed\n"); return 1; }
    if (!image_write_png(&im, out)) { fprintf(stderr, "png write failed\n"); return 1; }

    /* verify centre pixel ~ (26,76,217) */
    size_t idx = ((size_t)(H/2) * (size_t)W + (size_t)(W/2)) * 3u;
    int r = im.rgb[idx], g = im.rgb[idx+1], b = im.rgb[idx+2];
    printf("wrote %s (%dx%d), centre pixel = (%d,%d,%d)\n", out, W, H, r, g, b);

    int ok = (abs(r-26) <= 2) && (abs(g-76) <= 2) && (abs(b-217) <= 2);
    printf("%s\n", ok ? "PASS" : "FAIL");

    image_free(&im);
    glctx_destroy(c);
    return ok ? 0 : 1;
}
