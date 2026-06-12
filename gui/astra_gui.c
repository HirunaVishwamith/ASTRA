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
#include "mat4.h"
#include "hud.h"
#include "ui.h"
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

/* Project an ECI-km point to screen px via the render MVP. Returns 1 if in
 * front of the camera. (sx,sy) in pixels, top-left origin. */
static int project(const float m[16], double kx, double ky, double kz,
                   int w, int h, float *sx, float *sy) {
    float s = render_world_scale();
    float X=(float)kx*s, Y=(float)ky*s, Z=(float)kz*s;
    float cx = m[0]*X + m[4]*Y + m[8]*Z + m[12];
    float cy = m[1]*X + m[5]*Y + m[9]*Z + m[13];
    float cw = m[3]*X + m[7]*Y + m[11]*Z + m[15];
    if (cw <= 1e-4f) return 0;
    *sx = (cx/cw*0.5f + 0.5f) * (float)w;
    *sy = (1.0f - (cy/cw*0.5f + 0.5f)) * (float)h;
    return 1;
}

/* Is satellite P (ECI km) hidden behind the Earth from camera eye E (world)? */
static int occluded(fv3 eye, double px, double py, double pz) {
    float s = render_world_scale(), R = 6378.137f*s;
    fv3 P = { (float)px*s, (float)py*s, (float)pz*s };
    fv3 d = { P.x-eye.x, P.y-eye.y, P.z-eye.z };
    float L = sqrtf(d.x*d.x+d.y*d.y+d.z*d.z); if (L<1e-4f) return 0;
    d.x/=L; d.y/=L; d.z/=L;
    float tca = -(eye.x*d.x+eye.y*d.y+eye.z*d.z);
    float d2 = (eye.x*eye.x+eye.y*eye.y+eye.z*eye.z) - tca*tca;
    if (d2 >= R*R) return 0;
    float thc = sqrtf(R*R-d2), t0 = tca-thc;
    return (t0 > 0.01f && t0 < L-0.01f);
}

static fv3 cam_eye(Camera cam) {
    float ce=cosf(cam.el),se=sinf(cam.el),ca=cosf(cam.az),sa=sinf(cam.az);
    return (fv3){ cam.dist*ce*ca, cam.dist*se, cam.dist*ce*sa };
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
    UI  *ui  = ui_create();
    Hud *hud = hud_create();
    if (!ui || !hud) { fprintf(stderr, "HUD init failed\n"); return 1; }

    /* sim on its own thread, paced to wall-clock */
    if (!astra_sim_thread_start(&TH, &SIM, 1 /* realtime */)) {
        fprintf(stderr, "sim thread start failed\n"); return 1;
    }

    Camera cam = { 0.7f, 0.35f, 22.0f, 0.8f };
    const RenderSnapshot *snap = NULL;
    int frames = 0, running = 1;
    int selected = 0;
    int route_dv = 0;

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
        if (in.strike && selected >= 0) { Command cmd = { CMD_STRIKE, (uint32_t)selected, 0 }; astra_cmd_push(&SIM, cmd); }
        if (in.route_mode)   { route_dv = !route_dv; Command cmd = { CMD_ROUTE_MODE, (uint32_t)route_dv, 0 }; astra_cmd_push(&SIM, cmd); }
        if (snap) {
            int ns = (int)snap->sat_count;
            if (in.sel_next) selected = (selected+1) % ns;
            if (in.sel_prev) selected = (selected-1+ns) % ns;
        }

        int w, h; glctx_size(c, &w, &h);
        render_resize(r, w, h);

        /* projection for picking + callout */
        float mvp[16]; render_view_proj(cam, w, h, mvp);
        fv3 eye = cam_eye(cam);

        if (snap && in.click) {        /* pick nearest visible satellite */
            float best = 18.0f; int hit = -1;
            for (uint32_t i = 0; i < snap->sat_count; ++i) {
                if (!snap->sat[i].alive) continue;
                if (occluded(eye, snap->sat[i].r.x, snap->sat[i].r.y, snap->sat[i].r.z)) continue;
                float sx, sy;
                if (!project(mvp, snap->sat[i].r.x, snap->sat[i].r.y, snap->sat[i].r.z, w, h, &sx, &sy)) continue;
                float d = fabsf(sx-in.click_x) + fabsf(sy-in.click_y);
                if (d < best) { best = d; hit = (int)i; }
            }
            if (hit >= 0) selected = hit;
        }

        if (snap) {
            render_frame(r, snap, cam);
            /* selected asset screen position for the callout */
            float ssx=0, ssy=0; int son=0;
            if (selected >= 0 && selected < (int)snap->sat_count && snap->sat[selected].alive) {
                son = project(mvp, snap->sat[selected].r.x, snap->sat[selected].r.y, snap->sat[selected].r.z, w, h, &ssx, &ssy);
                if (son && occluded(eye, snap->sat[selected].r.x, snap->sat[selected].r.y, snap->sat[selected].r.z)) son = 0;
            }
            ui_begin(ui, w, h);
            hud_draw(hud, ui, snap, w, h, selected, SIM.paused, route_dv, SIM.speed, ssx, ssy, son);
            ui_end(ui);
        } else {
            glClearColor(0.01f,0.01f,0.03f,1.0f); glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        }

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
    hud_destroy(hud); ui_destroy(ui);
    render_destroy(r);
    glctx_destroy(c);
    printf("exited cleanly\n");
    return 0;
}
