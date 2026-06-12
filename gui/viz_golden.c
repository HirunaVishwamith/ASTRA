/* viz_golden.c — visual-correctness profile (golden-image test).
 *
 * Renders a fully deterministic scene (fixed seed, fixed step count, fixed
 * camera) offscreen and checks two things:
 *   1. DETERMINISM: rendering the same frame twice is pixel-identical
 *      (mean-abs-diff ~ 0). This is environment-independent and is the real
 *      correctness guarantee -- the renderer is a pure function of the snapshot.
 *   2. GOLDEN regression: compare against a committed reference PNG. The golden
 *      is auto-created on first run (or with --update). Because exact pixels are
 *      GPU/driver-dependent, a golden mismatch is reported but only fails when
 *      it exceeds a tolerance, and an _actual.png is written for inspection.
 *
 * Usage: viz_golden [--update] [golden.png]
 */
#include "astra/sim.h"
#include "glctx.h"
#include "render.h"
#include "image.h"
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static SimState SIM;

static void render_deterministic(GLCtx *c, Renderer *r, Image *out) {
    astra_sim_init_cfg(&SIM, 0xA57121u, ASTRA_NUM_PLANES, ASTRA_NUM_SATS_PER_PLANE, 5000.0);
    for (int i = 0; i < 120; ++i) astra_sim_tick(&SIM);
    const RenderSnapshot *snap = astra_snapshot_acquire(&SIM);
    Camera cam = { 0.7f, 0.35f, 22.0f, 0.8f };
    render_frame(r, snap, cam);
    glFinish();
    out->rgb = NULL;
    glctx_read_rgb(c, out);
}

int main(int argc, char **argv) {
    const int W = 800, H = 600;
    int update = 0;
    const char *golden = "tests/golden/constellation.png";
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--update")) update = 1;
        else golden = argv[i];
    }

    GLCtx *c = glctx_egl_create(W, H);
    if (!c || !glctx_load_gl(c)) { fprintf(stderr, "GL init failed\n"); return 1; }
    Renderer *r = render_create(W, H);
    if (!r) { fprintf(stderr, "renderer init failed\n"); return 1; }

    int fails = 0;

    /* 1) determinism: two independent renders must match exactly */
    Image a = {0}, b = {0};
    render_deterministic(c, r, &a);
    render_deterministic(c, r, &b);
    double self = image_mad(&a, &b);
    printf("determinism mean-abs-diff: %.4f (expect ~0)\n", self);
    if (self > 0.01) { printf("  RENDER NOT DETERMINISTIC\n"); fails++; }

    /* 2) golden regression */
    Image g = {0};
    if (update || !image_read_png(&g, golden)) {
        if (image_write_png(&a, golden))
            printf("golden: wrote reference %s (%dx%d)\n", golden, W, H);
        else { printf("golden: could not write %s (does tests/golden/ exist?)\n", golden); fails++; }
    } else {
        double d = image_mad(&a, &g);
        printf("golden mean-abs-diff: %.4f vs %s\n", d, golden);
        if (d > 3.0) {     /* generous: GPU/driver pixel variation */
            printf("  GOLDEN MISMATCH > 3.0; wrote _actual.png for inspection\n");
            image_write_png(&a, "tests/golden/_actual.png");
            fails++;
        }
        image_free(&g);
    }

    printf("%s\n", fails == 0 ? "PASS" : "FAIL");
    image_free(&a); image_free(&b);
    render_destroy(r); glctx_destroy(c);
    return fails == 0 ? 0 : 1;
}
