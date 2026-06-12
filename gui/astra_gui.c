/* astra_gui.c — interactive ASTRA viewer.
 * Sim runs on its own thread (realtime ~60 Hz); the main thread pulls the
 * newest snapshot through the lock-free triple buffer and renders it in a GLX
 * window. Input -> commands via the SPSC ring (strike/reboot/pause).
 *
 * Controls: drag = orbit camera, wheel = zoom, P = pause, R = reboot all,
 *           S / left-click = strike a random satellite, Q/Esc = quit.
 *
 * --selftest N  opens the window, renders N frames, writes a PNG, and exits
 *               (non-interactive validation of the on-screen GL path).
 */
#include "astra/sim.h"
#include "astra/sim_thread.h"
#include "glctx.h"
#include "render.h"
#include "image.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static SimState  SIM;
static SimThread TH;

static void nap_ms(long ms) {
    struct timespec ts = { ms/1000, (ms%1000)*1000000L };
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    int W = 1280, H = 960, selftest = 0;
    double range = 5000.0;
    uint64_t seed = 0xA57121u;
    const char *shot = "astra_gui.png";
    for (int i = 1; i < argc; ++i) {
        if      (!strcmp(argv[i],"--selftest") && i+1<argc) selftest = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--range")    && i+1<argc) range = strtod(argv[++i],0);
        else if (!strcmp(argv[i],"--w")        && i+1<argc) W = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--h")        && i+1<argc) H = atoi(argv[++i]);
        else if (!strcmp(argv[i],"--shot")     && i+1<argc) shot = argv[++i];
        else { fprintf(stderr,"unknown arg %s\n", argv[i]); return 2; }
    }

    astra_sim_init_cfg(&SIM, seed, ASTRA_NUM_PLANES, ASTRA_NUM_SATS_PER_PLANE, range);

    GLCtx *c = glctx_glx_create(W, H, "ASTRA — LEO Constellation");
    if (!c || !glctx_load_gl(c)) { fprintf(stderr, "GL window init failed\n"); return 1; }
    Renderer *r = render_create(W, H);
    if (!r) { fprintf(stderr, "renderer init failed\n"); return 1; }

    /* sim on its own thread, paced to wall-clock */
    if (!astra_sim_thread_start(&TH, &SIM, 1 /* realtime */)) {
        fprintf(stderr, "sim thread start failed\n"); return 1;
    }

    Camera cam = { 0.7f, 0.35f, 22.0f, 0.8f };
    const RenderSnapshot *snap = NULL;
    int frames = 0, running = 1;

    while (running) {
        const RenderSnapshot *s = astra_snapshot_acquire(&SIM);
        if (s) snap = s;                       /* else reuse last */

        VizInput in; glctx_get_input(c, &in);
        cam.az += in.dx * 0.006f;
        cam.el += in.dy * 0.006f;
        if (cam.el >  1.5f) cam.el =  1.5f;
        if (cam.el < -1.5f) cam.el = -1.5f;
        cam.dist *= (1.0f - in.scroll * 0.08f);
        if (cam.dist < 8.0f)  cam.dist = 8.0f;
        if (cam.dist > 80.0f) cam.dist = 80.0f;

        if (in.toggle_pause) { Command cmd = { CMD_PAUSE, (uint32_t)(SIM.paused?0:1), 0 }; astra_cmd_push(&SIM, cmd); }
        if (in.reboot)       { Command cmd = { CMD_REBOOT_ALL, 0, 0 }; astra_cmd_push(&SIM, cmd); }
        if (in.strike)       { Command cmd = { CMD_STRIKE, (uint32_t)(rand()% (int)SIM.num_sats), 0 }; astra_cmd_push(&SIM, cmd); }

        int w, h; glctx_size(c, &w, &h);
        render_resize(r, w, h);

        if (snap) render_frame(r, snap, cam);
        else { glClearColor(0.01f,0.01f,0.03f,1.0f); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT); }

        if (!glctx_swap_and_poll(c) || in.quit) running = 0;

        if (selftest) {
            if (++frames >= selftest) {
                Image im = {0};
                if (glctx_read_rgb(c, &im) && image_write_png(&im, shot))
                    printf("selftest: %d frames, wrote %s (%dx%d)\n", frames, shot, im.w, im.h);
                image_free(&im);
                running = 0;
            }
        } else {
            nap_ms(8);   /* ~120 Hz cap; sim thread paces the data */
        }
    }

    astra_sim_thread_stop(&TH);
    render_destroy(r);
    glctx_destroy(c);
    printf("exited cleanly\n");
    return 0;
}
