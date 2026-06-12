/* hud.c — ASTRA mission-control dashboard layout. */
#include "hud.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SANS  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define SANSB "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
#define MONO  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define HIST  256

struct Hud {
    UIFont *f_logo, *f_big, *f_hdr, *f_body, *f_small;
    float deliv[HIST], delay[HIST], util[HIST];
    int   hn, hhead;
    uint64_t last_frame;
};

/* palette */
static const UIColor C_PANEL = {0.035f,0.065f,0.105f,0.82f};
static const UIColor C_HDR   = {0.07f,0.14f,0.21f,0.92f};
static const UIColor C_EDGE  = {0.16f,0.42f,0.60f,0.55f};
static const UIColor C_CYAN  = {0.30f,0.85f,0.97f,1.0f};
static const UIColor C_AMBER = {1.0f,0.66f,0.20f,1.0f};
static const UIColor C_GREEN = {0.32f,0.92f,0.52f,1.0f};
static const UIColor C_RED   = {0.96f,0.36f,0.34f,1.0f};
static const UIColor C_WHITE = {0.90f,0.94f,1.0f,1.0f};
static const UIColor C_GREY  = {0.52f,0.62f,0.73f,1.0f};
static const UIColor C_DIM   = {0.32f,0.40f,0.50f,1.0f};

Hud *hud_create(void) {
    Hud *h = (Hud *)calloc(1, sizeof(*h));
    h->f_logo  = ui_font_load(SANSB, 26);
    h->f_big   = ui_font_load(SANS,  34);
    h->f_hdr   = ui_font_load(SANSB, 12);
    h->f_body  = ui_font_load(MONO,  14);
    h->f_small = ui_font_load(SANS,  11);
    if (!h->f_logo || !h->f_big || !h->f_hdr || !h->f_body || !h->f_small) { free(h); return NULL; }
    return h;
}
void hud_destroy(Hud *h) {
    if (!h) return;
    ui_font_free(h->f_logo); ui_font_free(h->f_big); ui_font_free(h->f_hdr);
    ui_font_free(h->f_body); ui_font_free(h->f_small);
    free(h);
}

/* ---- widgets ------------------------------------------------------------- */
/* panel with title bar; returns the y where content starts */
static float panel(Hud *h, UI *u, float x, float y, float w, float hgt, const char *title) {
    ui_rect(u, x, y, w, hgt, C_PANEL);
    ui_rect(u, x, y, w, 22, C_HDR);
    ui_rect_outline(u, x, y, w, hgt, 1.0f, C_EDGE);
    ui_rect(u, x, y, 3, 22, C_CYAN);                 /* accent tab */
    ui_text(u, h->f_hdr, x+12, y+5, C_CYAN, title);
    return y + 30;
}

/* label : value row */
static void row(Hud *h, UI *u, float x, float y, float w, const char *k, const char *v, UIColor vc) {
    ui_text(u, h->f_body, x, y, C_GREY, k);
    float vw = ui_text_measure(h->f_body, v);
    ui_text(u, h->f_body, x + w - vw, y, vc, v);
}

/* circular gauge: value01 in [0,1]; big centre number = vtext */
static void gauge(Hud *h, UI *u, float cx, float cy, float rad,
                  float value01, UIColor col, const char *vtext, const char *label) {
    const float A0 = 4.18879f;        /* 240 deg */
    const float SPAN = -5.23599f;     /* -300 deg (clockwise) */
    if (value01 < 0) value01 = 0;
    if (value01 > 1) value01 = 1;
    ui_arc(u, cx, cy, rad, A0, A0+SPAN, 6.0f, ui_rgba(0.13f,0.20f,0.27f,1.0f));
    ui_arc(u, cx, cy, rad, A0, A0+SPAN*value01, 6.0f, col);
    float bw = ui_text_measure(h->f_big, vtext);
    ui_text(u, h->f_big, cx-bw*0.5f, cy-22, C_WHITE, vtext);
    float lw = ui_text_measure(h->f_small, label);
    ui_text(u, h->f_small, cx-lw*0.5f, cy+12, C_GREY, label);
}

/* small horizontal bar meter */
static void meter(Hud *h, UI *u, float x, float y, float w, const char *label,
                  float v01, UIColor col, const char *vtext) {
    ui_text(u, h->f_small, x, y, C_GREY, label);
    float vw = ui_text_measure(h->f_small, vtext);
    ui_text(u, h->f_small, x+w-vw, y, col, vtext);
    float by = y + 15;
    ui_rect(u, x, by, w, 5, ui_rgba(0.12f,0.18f,0.24f,1.0f));
    if (v01 < 0) v01 = 0;
    if (v01 > 1) v01 = 1;
    ui_rect(u, x, by, w*v01, 5, col);
}

/* time-series plot from a ring buffer (newest at head-1) */
static void plot(Hud *h, UI *u, float x, float y, float w, float hgt,
                 const float *ring, int n, int head, float vmin, float vmax,
                 UIColor col, const char *label, const char *cur) {
    (void)panel(h, u, x, y, w, hgt, label);
    float gx=x+8, gy=y+30, gw=w-16, gh=y+hgt-8 - (y+30);
    /* gridlines */
    for (int i=1;i<3;i++) ui_line(u, gx, gy+gh*(float)i/3.0f, gx+gw, gy+gh*(float)i/3.0f, 1.0f, ui_rgba(0.12f,0.18f,0.24f,0.7f));
    if (n >= 2) {
        float span = vmax-vmin; if (span < 1e-6f) span = 1.0f;
        float prevx=0, prevy=0; int first=1;
        for (int i = 0; i < n; ++i) {
            int idx = (head - n + i + 2*HIST) % HIST;
            float val = (ring[idx]-vmin)/span; if (val<0)val=0; if(val>1)val=1;
            float px = gx + gw*(float)i/(float)(n-1);
            float py = gy + gh*(1.0f-val);
            if (!first) ui_line(u, prevx, prevy, px, py, 1.6f, col);
            prevx=px; prevy=py; first=0;
        }
    }
    if (cur) { float cw=ui_text_measure(h->f_small,cur); ui_text(u, h->f_small, x+w-cw-8, y+5, col, cur); }
}

/* ---- main layout --------------------------------------------------------- */
void hud_draw(Hud *h, UI *u, const RenderSnapshot *snap, int W, int Hh,
              int selected_sat, int paused, int route_dv, double speed) {
    char buf[128];

    /* advance history once per published frame */
    if (snap->frame_id != h->last_frame) {
        h->last_frame = snap->frame_id;
        h->deliv[h->hhead] = snap->delivery_ratio;
        h->delay[h->hhead] = snap->avg_delay_s;
        h->util [h->hhead] = snap->link_util;
        h->hhead = (h->hhead+1) % HIST;
        if (h->hn < HIST) h->hn++;
    }

    uint32_t alive = 0;
    for (uint32_t i = 0; i < snap->sat_count; ++i) alive += snap->sat[i].alive;

    /* ===== top status bar ===== */
    ui_rect(u, 0, 0, (float)W, 40, ui_rgba(0.02f,0.04f,0.07f,0.92f));
    ui_rect(u, 0, 40, (float)W, 1.5f, C_EDGE);
    ui_text(u, h->f_logo, 18, 7, C_CYAN, "ASTRA");
    ui_text(u, h->f_small, 110, 16, C_GREY, "AUTONOMOUS SATELLITE TRAFFIC & ROUTING ARCHITECTURE");
    int t = (int)snap->sim_time_s;
    snprintf(buf, sizeof buf, "T+ %02d:%02d:%02d", t/3600, (t/60)%60, t%60);
    float tw = ui_text_measure(h->f_body, buf);
    ui_text(u, h->f_body, (float)W - tw - 18, 12, C_WHITE, buf);
    snprintf(buf, sizeof buf, "STEP %u", snap->step_count);
    float sw = ui_text_measure(h->f_small, buf);
    ui_text(u, h->f_small, (float)W*0.5f - sw*0.5f, 14, C_GREY, buf);

    /* ===== left column ===== */
    float lx = 14, lw = 312, ly = 52;
    float aly = panel(h, u, lx, ly, lw, 320, "GLOBAL ASSET LIST");
    snprintf(buf, sizeof buf, "TOTAL %u", snap->sat_count);
    ui_text(u, h->f_small, lx+12, ly+34, C_GREY, buf);
    snprintf(buf, sizeof buf, "ACTIVE %u", alive);
    ui_text(u, h->f_small, lx+110, ly+34, C_GREEN, buf);
    snprintf(buf, sizeof buf, "DOWN %u", snap->sat_count-alive);
    ui_text(u, h->f_small, lx+200, ly+34, (snap->sat_count-alive)?C_RED:C_DIM, buf);
    /* scrolled list window around the selection */
    int rows = 11;
    int start = selected_sat >= 0 ? selected_sat - rows/2 : 0;
    if (start < 0) start = 0;
    if (start > (int)snap->sat_count - rows) start = (int)snap->sat_count - rows;
    if (start < 0) start = 0;
    float ry = aly + 14;
    for (int i = 0; i < rows && start+i < (int)snap->sat_count; ++i) {
        int id = start+i;
        int up = snap->sat[id].alive;
        if (id == selected_sat) ui_rect(u, lx+6, ry-2, lw-12, 18, ui_rgba(0.12f,0.28f,0.38f,0.8f));
        snprintf(buf, sizeof buf, "STARLINK-%03d", id);
        ui_text(u, h->f_body, lx+14, ry, up?C_WHITE:C_DIM, buf);
        const char *st = up ? "ACTIVE" : "OFFLINE";
        float stw = ui_text_measure(h->f_body, st);
        ui_text(u, h->f_body, lx+lw-stw-14, ry, up?C_GREEN:C_RED, st);
        ry += 19;
    }

    /* selected asset detail */
    float sy = ly + 332;
    float scy = panel(h, u, lx, sy, lw, 150, "SELECTED ASSET");
    if (selected_sat >= 0 && selected_sat < (int)snap->sat_count) {
        const SnapSat *s = &snap->sat[selected_sat];
        double rn = sqrt(s->r.x*s->r.x + s->r.y*s->r.y + s->r.z*s->r.z);
        double alt = rn - 6378.137;
        double vel = sqrt(398600.4418 / rn);                 /* circular-orbit speed */
        double lat = asin(s->r.y/rn) * 180.0/M_PI;
        double lon = atan2(s->r.z, s->r.x) * 180.0/M_PI;
        snprintf(buf, sizeof buf, "STARLINK-%03d", selected_sat);
        ui_text(u, h->f_body, lx+14, scy, s->alive?C_CYAN:C_RED, buf);
        ui_text(u, h->f_body, lx+lw-ui_text_measure(h->f_body,s->alive?"ONLINE":"OFFLINE")-14, scy,
                s->alive?C_GREEN:C_RED, s->alive?"ONLINE":"OFFLINE");
        float dy = scy+24;
        snprintf(buf,sizeof buf,"%.1f km",alt);  row(h,u,lx+14,dy,lw-28,"ALTITUDE", buf, C_WHITE); dy+=20;
        snprintf(buf,sizeof buf,"%.2f km/s",vel); row(h,u,lx+14,dy,lw-28,"VELOCITY", buf, C_WHITE); dy+=20;
        snprintf(buf,sizeof buf,"%+.2f deg",lat); row(h,u,lx+14,dy,lw-28,"LATITUDE", buf, C_WHITE); dy+=20;
        snprintf(buf,sizeof buf,"%+.2f deg",lon); row(h,u,lx+14,dy,lw-28,"LONGITUDE", buf, C_WHITE);
    } else {
        ui_text(u, h->f_body, lx+14, scy, C_DIM, "— no asset selected —");
    }

    /* ===== right column ===== */
    float rw = 320, rxp = (float)W - rw - 14, ryp = 52;
    float my = panel(h, u, rxp, ryp, rw, 224, "LIVE NETWORK PERFORMANCE");
    /* big latency gauge (show ms, or seconds when large) */
    float lat_ms = snap->avg_delay_s * 1000.0f;
    float lat01 = lat_ms / 80000.0f;           /* scale: 80 s -> full sweep */
    if (lat_ms > 9999.0f) snprintf(buf, sizeof buf, "%.1fs", snap->avg_delay_s);
    else                  snprintf(buf, sizeof buf, "%.0f", lat_ms);
    gauge(h, u, rxp+72, my+74, 56, lat01, C_CYAN, buf, "AVG DELAY");
    /* meters on the right of the gauge */
    float mx = rxp+150, mw = 150;
    meter(h, u, mx, my+18, mw, "DELIVERY",  snap->delivery_ratio,
          snap->delivery_ratio>0.5f?C_GREEN:C_AMBER,
          (snprintf(buf,sizeof buf,"%.0f%%",snap->delivery_ratio*100.0f),buf));
    char b2[32]; snprintf(b2,sizeof b2,"%.1f%%",snap->link_util*100.0f);
    meter(h, u, mx, my+54, mw, "LINK UTIL", snap->link_util*4.0f, C_CYAN, b2);
    char b3[32]; snprintf(b3,sizeof b3,"%.1f",snap->avg_hops);
    meter(h, u, mx, my+90, mw, "AVG HOPS", snap->avg_hops/12.0f, C_AMBER, b3);
    char b4[32]; snprintf(b4,sizeof b4,"%u",snap->link_count);
    meter(h, u, mx, my+126, mw, "ACTIVE LINKS", (float)snap->link_count/400.0f, C_GREEN, b4);

    /* simulation control */
    float cy2 = ryp + 236;
    float ccy = panel(h, u, rxp, cy2, rw, 120, "SIMULATION CONTROL");
    row(h,u,rxp+14,ccy,rw-28,"STATE", paused?"PAUSED":"RUNNING", paused?C_AMBER:C_GREEN);
    row(h,u,rxp+14,ccy+22,rw-28,"ROUTING", route_dv?"DISTANCE-VECTOR":"DIJKSTRA", C_WHITE);
    snprintf(buf,sizeof buf,"%.2fx",speed); row(h,u,rxp+14,ccy+44,rw-28,"SPEED", buf, C_WHITE);
    ui_text(u, h->f_small, rxp+14, ccy+70, C_DIM, "[P]ause  [R]eboot  [S]trike  drag/scroll");

    /* ===== bottom plots ===== */
    float bw = 300, bh = 116, by = (float)Hh - bh - 14, bx = 14 + 312 + 18;
    plot(h, u, bx, by, bw, bh, h->deliv, h->hn, h->hhead, 0.0f, 1.0f, C_GREEN, "DELIVERY RATIO",
         (snprintf(buf,sizeof buf,"%.0f%%",snap->delivery_ratio*100.0f),buf));
    plot(h, u, bx+bw+16, by, bw, bh, h->util, h->hn, h->hhead, 0.0f, 0.30f, C_CYAN, "LINK UTILISATION",
         (snprintf(buf,sizeof buf,"%.1f%%",snap->link_util*100.0f),buf));
}
