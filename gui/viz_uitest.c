/* viz_uitest.c — milestone B: verify the 2D UI framework (panels/lines/arc/text). */
#include "glctx.h"
#include "ui.h"
#include "image.h"
#include <GL/gl.h>
#include <stdio.h>
#include <math.h>

#define SANS "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define MONO "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "viz_uitest.png";
    const int W = 900, H = 540;
    GLCtx *c = glctx_egl_create(W, H);
    if (!c || !glctx_load_gl(c)) { fprintf(stderr, "GL init failed\n"); return 1; }

    UI *u = ui_create();
    UIFont *title = ui_font_load(SANS, 30);
    UIFont *body  = ui_font_load(MONO, 16);
    UIFont *small = ui_font_load(SANS, 13);
    if (!u || !title || !body || !small) { fprintf(stderr, "ui/font init failed\n"); return 1; }

    glViewport(0,0,W,H);
    glClearColor(0.02f,0.03f,0.05f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    UIColor cyan = ui_rgba(0.25f,0.85f,0.95f,1.0f);
    UIColor amber= ui_rgba(1.0f,0.65f,0.2f,1.0f);
    UIColor pan  = ui_rgba(0.06f,0.10f,0.16f,0.85f);
    UIColor edge = ui_rgba(0.2f,0.5f,0.7f,0.7f);
    UIColor white= ui_rgba(0.9f,0.95f,1.0f,1.0f);
    UIColor grey = ui_rgba(0.55f,0.65f,0.75f,1.0f);

    ui_begin(u, W, H);
    /* a panel */
    ui_rect(u, 24, 24, 360, 200, pan);
    ui_rect_outline(u, 24, 24, 360, 200, 1.5f, edge);
    ui_rect(u, 24, 24, 360, 28, ui_rgba(0.1f,0.2f,0.3f,0.9f));
    ui_text(u, title, 360, 14, cyan, "ASTRA");
    ui_text(u, small, 34, 30, grey, "GLOBAL ASSET LIST");
    ui_text(u, body, 34, 64, white,  "STARLINK-110   ACTIVE");
    ui_text(u, body, 34, 86, white,  "STARLINK-111   ACTIVE");
    ui_text(u, body, 34, 108, amber, "STARLINK-112   DEGRADED");
    ui_text(u, body, 34, 130, grey,  "alt   550.2 km");
    ui_text(u, body, 34, 152, grey,  "vel   7.59 km/s");

    /* a circular gauge */
    float gx=560, gy=160, rad=70;
    ui_arc(u, gx, gy, rad, 3.6651f, -0.5236f, 6.0f, ui_rgba(0.15f,0.25f,0.32f,1.0f)); /* track */
    ui_arc(u, gx, gy, rad, 3.6651f, 1.2f, 6.0f, cyan);                                 /* value */
    ui_text(u, title, gx-26, gy-22, white, "50");
    ui_text(u, small, gx-34, gy+14, grey, "LATENCY ms");

    /* a sparkline plot */
    float px=520, py=300, pw=320, ph=120;
    ui_rect(u, px, py, pw, ph, pan);
    ui_rect_outline(u, px, py, pw, ph, 1.0f, edge);
    float prevx=px, prevy=py+ph*0.5f;
    for (int i=1;i<=60;i++){ float x=px+pw*(float)i/60.0f; float y=py+ph*0.5f - sinf((float)i*0.3f)*ph*0.35f; ui_line(u,prevx,prevy,x,y,1.5f,amber); prevx=x; prevy=y; }
    ui_text(u, small, px+6, py+4, grey, "THROUGHPUT");

    ui_end(u);
    glFinish();

    Image im = {0};
    if (!glctx_read_rgb(c, &im) || !image_write_png(&im, out)) { fprintf(stderr,"write failed\n"); return 1; }
    /* sanity: some non-background pixels exist */
    long bright = 0; for (size_t i=0;i<(size_t)im.w*(size_t)im.h*3;i++) if (im.rgb[i]>80) bright++;
    printf("wrote %s (%dx%d), bright px=%ld\n", out, W, H, bright);
    printf("%s\n", bright > 1000 ? "PASS" : "FAIL");
    image_free(&im); ui_font_free(title); ui_font_free(body); ui_font_free(small);
    ui_destroy(u); glctx_destroy(c);
    return bright > 1000 ? 0 : 1;
}
