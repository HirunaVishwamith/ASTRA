/* astra_render.c — headless: run the sim N steps, render the snapshot to PNG.
 * This is the visual-correctness path (golden images) and a quick way to eyeball
 * the constellation without an interactive window. Offscreen via EGL. */
#include "astra/sim.h"
#include "glctx.h"
#include "render.h"
#include "image.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SimState SIM;

int main(int argc, char **argv) {
    uint32_t steps = 120, planes = ASTRA_NUM_PLANES, per = ASTRA_NUM_SATS_PER_PLANE;
    double range = 5000.0;          /* connected shell makes a better picture */
    uint64_t seed = 0xA57121u;
    int W = 1280, H = 960;
    const char *out = "astra_frame.png";
    Camera cam = { 0.7f, 0.35f, 22.0f, 0.8f };

    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i],"--steps") && i+1<argc) steps = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i],"--planes")&& i+1<argc) planes= (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i],"--per")   && i+1<argc) per   = (uint32_t)strtoul(argv[++i],0,10);
        else if (!strcmp(argv[i],"--range") && i+1<argc) range = strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--seed")  && i+1<argc) seed  = strtoull(argv[++i],0,0);
        else if (!strcmp(argv[i],"--w")     && i+1<argc) W     = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--h")     && i+1<argc) H     = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--az")    && i+1<argc) cam.az  = (float)strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--el")    && i+1<argc) cam.el  = (float)strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--dist")  && i+1<argc) cam.dist= (float)strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--out")   && i+1<argc) out   = argv[++i];
        else { fprintf(stderr,"unknown arg %s\n", argv[i]); return 2; }
    }

    astra_sim_init_cfg(&SIM, seed, planes, per, range);
    for (uint32_t i = 0; i < steps; ++i) astra_sim_tick(&SIM);
    const RenderSnapshot *snap = astra_snapshot_acquire(&SIM);
    if (!snap) { fprintf(stderr, "no snapshot\n"); return 1; }

    GLCtx *c = glctx_egl_create(W, H);
    if (!c || !glctx_load_gl(c)) { fprintf(stderr, "GL init failed\n"); return 1; }
    Renderer *r = render_create(W, H);
    if (!r) { fprintf(stderr, "renderer init failed\n"); return 1; }

    render_frame(r, snap, cam);
    glFinish();

    Image im = {0};
    if (!glctx_read_rgb(c, &im) || !image_write_png(&im, out)) {
        fprintf(stderr, "readback/write failed\n"); return 1;
    }
    printf("rendered %u sats, %u links, %u ground -> %s (%dx%d)\n",
           snap->sat_count, snap->link_count, snap->gs_count, out, W, H);

    image_free(&im);
    render_destroy(r);
    glctx_destroy(c);
    return 0;
}
