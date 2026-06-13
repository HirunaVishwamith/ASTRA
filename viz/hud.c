/* hud.c — ASTRA mission-control dashboard (operator console layout).
 *
 * Visual structure (matches realistic_interface.png):
 *   top    : brand + TOTAL OBJECTS / COVERAGE / ACTIVE NETWORK + title + clock
 *   left   : GLOBAL ASSET LIST (search header, filter chips, clickable rows)
 *            SELECTED ASSET (live orbital data, role, bandwidth, signal,
 *            VIEW ORBITAL ELEMENTS toggle)
 *   right  : NETWORK PERFORMANCE (Space Resilience Index + metric grid)
 *            SIMULATION CONTROL & CONFIG (state, speed, routing chips, actions)
 *            ACTIVE ROUTE (hop-by-hop list + stream latency sparkline)
 *   centre : floating sim clock, station labels, route callout, selection
 *            ring, constellation legend
 */
#include "hud.h"
#include "astra/orbit.h"
#include "astra/rf.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define SANS  "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#define SANSB "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
#define MONO  "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define HIST  192

struct Hud {
    UIFont *f_logo, *f_tiny, *f_small, *f_body, *f_med, *f_big, *f_clock;
    /* metric history rings (per published frame) */
    float deliv[HIST], delay_ms[HIST], util[HIST], drop[HIST],
          links[HIST], churn[HIST], route_ms[HIST];
    int   hn, hhead;
    uint64_t last_frame;
    /* panel-local UI state */
    int  filter;       /* 0 all | 1 active | 2 down */
    int  scroll;       /* first visible asset-list entry */
    int  show_elems;   /* SELECTED ASSET shows orbital elements */
    int  focus_mode;   /* hide side panels to maximise the viewport */
    /* scratch, recomputed each frame */
    uint8_t deg[ASTRA_MAX_SATS];
    uint8_t gs_link[ASTRA_MAX_SATS];
    float   bw[ASTRA_MAX_SATS];
    float   gs_bw[ASTRA_MAX_GROUND];   /* aggregate ground-link capacity */
    uint8_t gs_deg[ASTRA_MAX_GROUND];  /* live sat links per ground stn  */
};

/* ---- palette -------------------------------------------------------------- */
static const UIColor C_PANEL = {0.016f,0.030f,0.050f,0.90f};
static const UIColor C_INSET = {0.030f,0.052f,0.080f,0.95f};
static const UIColor C_HDR   = {0.045f,0.085f,0.125f,0.96f};
static const UIColor C_EDGE  = {0.13f,0.25f,0.36f,0.80f};
static const UIColor C_CYAN  = {0.30f,0.85f,0.97f,1.0f};
static const UIColor C_AMBER = {1.0f,0.66f,0.20f,1.0f};
static const UIColor C_GREEN = {0.34f,0.90f,0.52f,1.0f};
static const UIColor C_RED   = {0.96f,0.36f,0.34f,1.0f};
static const UIColor C_GOLD  = {1.0f,0.78f,0.25f,1.0f};
static const UIColor C_WHITE = {0.92f,0.95f,1.0f,1.0f};
static const UIColor C_GREY  = {0.55f,0.64f,0.74f,1.0f};
static const UIColor C_DIM   = {0.33f,0.41f,0.50f,1.0f};
static const UIColor C_SHADE = {0.0f,0.0f,0.0f,0.45f};

/* plane colours: keep in sync with PLANE_COL in render.c */
static const UIColor PLANE_COL[6] = {
    {0.36f,0.95f,0.62f,1}, {0.36f,0.80f,1.00f,1}, {1.00f,0.72f,0.30f,1},
    {0.93f,0.47f,0.85f,1}, {0.55f,0.64f,1.00f,1}, {0.93f,0.90f,0.50f,1},
};

Hud *hud_create(void) {
    Hud *h = (Hud *)calloc(1, sizeof(*h));
    h->f_logo  = ui_font_load(SANSB, 21);
    h->f_tiny  = ui_font_load(SANS,  10);
    h->f_small = ui_font_load(SANS,  11);
    h->f_body  = ui_font_load(MONO,  12);
    h->f_med   = ui_font_load(SANSB, 13);
    h->f_big   = ui_font_load(SANSB, 34);
    h->f_clock = ui_font_load(MONO,  15);
    if (!h->f_logo||!h->f_tiny||!h->f_small||!h->f_body||!h->f_med||!h->f_big||!h->f_clock)
        { free(h); return NULL; }
    return h;
}
void hud_destroy(Hud *h) {
    if (!h) return;
    ui_font_free(h->f_logo);  ui_font_free(h->f_tiny); ui_font_free(h->f_small);
    ui_font_free(h->f_body);  ui_font_free(h->f_med);  ui_font_free(h->f_big);
    ui_font_free(h->f_clock);
    free(h);
}

/* ---- geometry helpers ------------------------------------------------------ */
static int hit(const HudInput *in, float x, float y, float w, float hgt) {
    return in && in->click && in->cx >= x && in->cx <= x+w && in->cy >= y && in->cy <= y+hgt;
}

/* hexagon outline centred at (cx,cy), the ground-station marker glyph */
static void hexagon(UI *u, float cx, float cy, float rad, float t, UIColor c) {
    float px = 0, py = 0;
    for (int k = 0; k <= 6; ++k) {
        float a = (float)k * 1.04719755f;            /* 60 deg steps */
        float x = cx + rad*cosf(a), y = cy + rad*sinf(a);
        if (k) ui_line(u, px, py, x, y, t, c);
        px = x; py = y;
    }
}

/* world km (ECI) -> screen px through the frame MVP (axis change matches
 * render.c eci_to_world). Returns 0 if behind the camera. */
static int project(const float m[16], vec3 km, int w, int hh, float *sx, float *sy) {
    float X=(float)km.x*0.001f, Y=(float)km.z*0.001f, Z=(float)-km.y*0.001f;
    float cx = m[0]*X + m[4]*Y + m[8]*Z + m[12];
    float cy = m[1]*X + m[5]*Y + m[9]*Z + m[13];
    float cw = m[3]*X + m[7]*Y + m[11]*Z + m[15];
    if (cw <= 1e-4f) return 0;
    *sx = (cx/cw*0.5f + 0.5f) * (float)w;
    *sy = (1.0f - (cy/cw*0.5f + 0.5f)) * (float)hh;
    return 1;
}

/* hidden behind the globe from camera eye (world units)? */
static int occluded(const float eye[3], vec3 km) {
    float R = 6378.137f*0.001f;
    float P[3] = { (float)km.x*0.001f, (float)km.z*0.001f, (float)-km.y*0.001f };
    float d[3] = { P[0]-eye[0], P[1]-eye[1], P[2]-eye[2] };
    float L = sqrtf(d[0]*d[0]+d[1]*d[1]+d[2]*d[2]); if (L < 1e-4f) return 0;
    d[0]/=L; d[1]/=L; d[2]/=L;
    float tca = -(eye[0]*d[0]+eye[1]*d[1]+eye[2]*d[2]);
    float dd = (eye[0]*eye[0]+eye[1]*eye[1]+eye[2]*eye[2]) - tca*tca;
    if (dd >= R*R) return 0;
    float thc = sqrtf(R*R-dd), t0 = tca-thc;
    return (t0 > 0.01f && t0 < L-0.01f);
}

/* ---- widget helpers -------------------------------------------------------- */
static float panel(Hud *h, UI *u, float x, float y, float w, float hgt,
                   const char *title, const char *tag, UIColor tagc) {
    ui_rect(u, x+3, y+3, w, hgt, C_SHADE);
    ui_rect(u, x, y, w, hgt, C_PANEL);
    ui_rect(u, x, y, w, 22, C_HDR);
    ui_rect_outline(u, x, y, w, hgt, 1.0f, C_EDGE);
    ui_rect(u, x, y, 2.5f, 22, C_CYAN);
    ui_text(u, h->f_med, x+10, y+4, C_WHITE, title);
    if (tag) {
        float tw = ui_text_measure(h->f_tiny, tag);
        ui_text(u, h->f_tiny, x+w-tw-10, y+6, tagc, tag);
    }
    return y + 28;
}

static void kv(Hud *h, UI *u, float x, float y, float w,
               const char *k, const char *v, UIColor vc) {
    ui_text(u, h->f_tiny, x, y, C_DIM, k);
    ui_text(u, h->f_body, x, y+12, vc, v);
    (void)w;
}

static int button(Hud *h, UI *u, const HudInput *in, float x, float y,
                  float w, float hgt, const char *label, UIColor col, int filled) {
    int hv = hit(in, x, y, w, hgt);
    if (filled) ui_rect(u, x, y, w, hgt, ui_rgba(col.r*0.22f, col.g*0.22f, col.b*0.22f, 0.9f));
    ui_rect_outline(u, x, y, w, hgt, 1.0f, col);
    float tw = ui_text_measure(h->f_small, label);
    ui_text(u, h->f_small, x+(w-tw)*0.5f, y+(hgt-14)*0.5f, col, label);
    return hv;
}

static int chip(Hud *h, UI *u, const HudInput *in, float x, float y,
                float w, float hgt, const char *label, int active) {
    int hv = hit(in, x, y, w, hgt);
    if (active) ui_rect(u, x, y, w, hgt, ui_rgba(0.10f,0.34f,0.46f,1.0f));
    else        ui_rect(u, x, y, w, hgt, C_INSET);
    ui_rect_outline(u, x, y, w, hgt, 1.0f, active ? C_CYAN : C_EDGE);
    float tw = ui_text_measure(h->f_tiny, label);
    ui_text(u, h->f_tiny, x+(w-tw)*0.5f, y+(hgt-12)*0.5f, active ? C_WHITE : C_GREY, label);
    return hv;
}

/* bar sparkline of the newest samples in a ring */
static void spark_bars(UI *u, float x, float y, float w, float hgt,
                       const float *ring, int n, int head, float vmax, UIColor col) {
    int bars = (int)(w / 4.0f); if (bars > n) bars = n;
    if (vmax < 1e-6f) vmax = 1.0f;
    for (int i = 0; i < bars; ++i) {
        int idx = (head - bars + i + 2*HIST) % HIST;
        float v = ring[idx] / vmax; if (v < 0.04f) v = 0.04f; if (v > 1) v = 1;
        float bh = hgt * v;
        UIColor c = col; c.a = 0.35f + 0.65f*(float)i/(float)bars;
        ui_rect(u, x + (float)i*4.0f, y + hgt - bh, 3.0f, bh, c);
    }
}

static void spark_line(UI *u, float x, float y, float w, float hgt,
                       const float *ring, int n, int head, float vmax, UIColor col) {
    if (n < 2) return;
    if (vmax < 1e-6f) vmax = 1.0f;
    float px = 0, py = 0;
    for (int i = 0; i < n; ++i) {
        int idx = (head - n + i + 2*HIST) % HIST;
        float v = ring[idx]/vmax; if (v < 0) v = 0; if (v > 1) v = 1;
        float qx = x + w*(float)i/(float)(n-1), qy = y + hgt*(1.0f-v);
        if (i) ui_line(u, px, py, qx, qy, 1.4f, col);
        px = qx; py = qy;
    }
}

/* segmented horizontal gauge, 24 cells */
static void seg_bar(UI *u, float x, float y, float w, float hgt, float frac, UIColor col) {
    int n = 24;
    float cw = w/(float)n;
    for (int i = 0; i < n; ++i) {
        int onn = ((float)i + 0.5f)/(float)n <= frac;
        UIColor c = onn ? col : ui_rgba(0.10f,0.16f,0.22f,1.0f);
        ui_rect(u, x + (float)i*cw, y, cw-2.0f, hgt, c);
    }
}

static const char *sat_name(int id, char *buf, size_t n) {
    snprintf(buf, n, "STARLINK-%03d", id);
    return buf;
}

/* short airport-style code for a ground station: word initials ("New York" ->
 * "NY"), else first letter + following consonants ("Tokyo" -> "TKY"). */
static void gs_code(const RenderSnapshot *snap, node_id gid, char *out) {
    const char *nm = NULL;
    for (uint32_t i = 0; i < snap->gs_count; ++i)
        if (snap->gs[i].gid == gid) { nm = snap->gs[i].name; break; }
    if (!nm) { out[0]='-'; out[1]=0; return; }
    int n = 0, words = 0;
    for (const char *p = nm; *p && n < 3; ++p)
        if ((p == nm || p[-1] == ' ') && *p != ' ') { out[n++] = (char)((*p>='a'&&*p<='z')?*p-32:*p); words++; }
    if (words < 2) {
        n = 1;
        for (const char *p = nm+1; *p && n < 3; ++p) {
            char c = (char)((*p>='a'&&*p<='z')?*p-32:*p);
            if (c==' '||c=='A'||c=='E'||c=='I'||c=='O'||c=='U') continue;
            out[n++] = c;
        }
    }
    out[n] = 0;
}

/* slant range (km) of the snapshot link between u and v, or -1 if none */
static float snap_link_dist(const RenderSnapshot *snap, node_id u, node_id v) {
    for (uint32_t e = 0; e < snap->link_count; ++e) {
        const SnapLink *L = &snap->link[e];
        if ((L->u==u && L->v==v) || (L->u==v && L->v==u)) return L->dist_km;
    }
    return -1.0f;
}

static void node_label(const RenderSnapshot *snap, node_id id, char *buf, size_t n) {
    if (id < snap->sat_count) { snprintf(buf, n, "SL-%03u", id); return; }
    for (uint32_t i = 0; i < snap->gs_count; ++i)
        if (snap->gs[i].gid == id) {
            snprintf(buf, n, "GS:%s", snap->gs[i].name);
            for (char *p = buf; *p; ++p) if (*p>='a'&&*p<='z') *p = (char)(*p-32);
            return;
        }
    snprintf(buf, n, "N-%u", id);
}

/* ---- main layout ------------------------------------------------------------ */
void hud_draw(Hud *h, UI *u, const RenderSnapshot *snap, int W, int Hh,
              int selected_sat, int paused, int route_dv, int cost_hops,
              double speed, const float mvp[16], const float eye[3],
              const HudInput *in, HudActions *out) {
    char buf[160], b2[64];
    HudActions act;
    memset(&act, 0, sizeof(act));
    act.select_sat = -1; act.set_route_mode = -1; act.set_cost_mode = -1;

    if (in && in->toggle_focus) h->focus_mode = !h->focus_mode;

    /* ---- derived stats ---- */
    uint32_t alive = 0;
    for (uint32_t i = 0; i < snap->sat_count; ++i) alive += snap->sat[i].alive;
    memset(h->deg, 0, snap->sat_count);
    memset(h->gs_link, 0, snap->sat_count);
    memset(h->bw, 0, snap->sat_count*sizeof(float));
    memset(h->gs_bw, 0, snap->gs_count*sizeof(float));
    memset(h->gs_deg, 0, snap->gs_count);
    uint32_t up_links = 0;
    for (uint32_t e = 0; e < snap->link_count; ++e) {
        const SnapLink *L = &snap->link[e];
        if (L->up) up_links++;
        int ugs = L->u >= snap->sat_count, vgs = L->v >= snap->sat_count;
        if (!ugs && L->u < ASTRA_MAX_SATS) { h->deg[L->u]++; h->bw[L->u]+=L->bw_mbps; if (vgs) h->gs_link[L->u]=1; }
        if (!vgs && L->v < ASTRA_MAX_SATS) { h->deg[L->v]++; h->bw[L->v]+=L->bw_mbps; if (ugs) h->gs_link[L->v]=1; }
        /* aggregate ground-station capacity (gs index = node id - sat_count) */
        if (ugs) { uint32_t gi = L->u - snap->sat_count; if (gi < ASTRA_MAX_GROUND) { h->gs_bw[gi]+=L->bw_mbps; h->gs_deg[gi]++; } }
        if (vgs) { uint32_t gi = L->v - snap->sat_count; if (gi < ASTRA_MAX_GROUND) { h->gs_bw[gi]+=L->bw_mbps; h->gs_deg[gi]++; } }
    }
    uint32_t connected = 0;
    for (uint32_t i = 0; i < snap->sat_count; ++i)
        if (snap->sat[i].alive && h->deg[i] > 0) connected++;
    float up_frac    = snap->link_count ? (float)up_links/(float)snap->link_count : 0.0f;
    float alive_frac = snap->sat_count ? (float)alive/(float)snap->sat_count : 0.0f;
    float cover_frac = snap->sat_count ? (float)connected/(float)snap->sat_count : 0.0f;
    float resil = 100.0f*(0.45f*snap->delivery_ratio + 0.35f*up_frac + 0.20f*alive_frac);

    /* ---- history (once per published frame) ---- */
    if (snap->frame_id != h->last_frame) {
        h->last_frame = snap->frame_id;
        h->deliv[h->hhead]    = snap->delivery_ratio*100.0f;
        h->delay_ms[h->hhead] = snap->avg_delay_s*1000.0f;
        h->util[h->hhead]     = snap->link_util*100.0f;
        h->drop[h->hhead]     = (1.0f - snap->delivery_ratio)*100.0f;
        h->links[h->hhead]    = (float)up_links;
        h->churn[h->hhead]    = (float)snap->route_updates;
        h->route_ms[h->hhead] = snap->route.valid ? snap->route.total_ms : 0.0f;
        h->hhead = (h->hhead+1) % HIST;
        if (h->hn < HIST) h->hn++;
    }

    const float TOP = 44.0f, LX = 12.0f, LW = 300.0f;
    const float RW = 320.0f, RX = (float)W - RW - 12.0f;
    /* viewport span: full width in focus mode, between the panels otherwise */
    const float VL = h->focus_mode ? 12.0f : LX+LW+12.0f;
    const float VR = h->focus_mode ? (float)W - 12.0f : RX-12.0f;

    /* ================= viewport overlays (under the side panels) ============ */
    if (mvp && eye) {
        /* ground stations: hexagon beacon + uplink/downlink capacity */
        for (uint32_t i = 0; i < snap->gs_count; ++i) {
            float sx, sy;
            if (!project(mvp, snap->gs[i].r, W, Hh, &sx, &sy)) continue;
            if (occluded(eye, snap->gs[i].r)) continue;
            if (sx < VL+12 || sx > VR-120 || sy < TOP+40 || sy > (float)Hh-60) continue;
            float   gbw    = h->gs_bw[i];
            uint8_t glinks = h->gs_deg[i];
            UIColor hexc   = glinks ? C_AMBER : C_DIM;
            /* pulsing hexagon glyph (per-station phase) */
            float beat = 0.5f + 0.5f*sinf((float)snap->sim_time_s*2.2f + (float)i);
            hexagon(u, sx, sy, 6.5f, 1.6f, hexc);
            ui_circle(u, sx, sy, 1.4f + 1.8f*beat,
                      ui_rgba(hexc.r, hexc.g, hexc.b, 0.45f + 0.55f*beat));
            /* leader to the data card */
            float bx = sx+13, by = sy-32;
            ui_line(u, sx+5, sy-5, bx, by+12, 1.0f, ui_rgba(1,0.62f,0.14f,0.7f));
            char cap[48];
            if (gbw >= 1000.0f)
                snprintf(cap, sizeof cap, "UL/DL %.1f Gbps  %u LINK%s",
                         gbw/1000.0f, glinks, glinks==1?"":"S");
            else
                snprintf(cap, sizeof cap, "UL/DL %.0f Mbps  %u LINK%s",
                         gbw, glinks, glinks==1?"":"S");
            float w1 = ui_text_measure(h->f_tiny, snap->gs[i].name);
            float w2 = ui_text_measure(h->f_tiny, cap);
            float cw = (w1 > w2 ? w1 : w2) + 14.0f;
            ui_rect(u, bx, by, cw, 30, ui_rgba(0.02f,0.04f,0.07f,0.88f));
            ui_rect(u, bx, by, 1.5f, 30, C_AMBER);
            ui_text(u, h->f_tiny, bx+8, by+3,  C_WHITE, snap->gs[i].name);
            ui_text(u, h->f_tiny, bx+8, by+16, glinks ? C_GREEN : C_DIM, cap);
        }

        /* route callout pinned to the middle hop */
        if (snap->route.valid && snap->route.count >= 3) {
            uint32_t mid = snap->route.count/2;
            node_id nid = snap->route.node[mid];
            vec3 p = (nid < snap->sat_count) ? snap->sat[nid].r : (vec3){0,0,0};
            float sx, sy;
            if (nid < snap->sat_count && project(mvp, p, W, Hh, &sx, &sy)
                && !occluded(eye, p)
                && sx > VL+20 && sx < VR-150 && sy > TOP+80 && sy < (float)Hh-90) {
                node_label(snap, nid, b2, sizeof b2);
                ui_line(u, sx, sy, sx+18, sy+18, 1.2f, ui_rgba(1.0f,0.78f,0.25f,0.8f));
                float bx = sx+18, by = sy+18;
                ui_rect(u, bx, by, 150, 48, ui_rgba(0.03f,0.05f,0.08f,0.92f));
                ui_rect_outline(u, bx, by, 150, 48, 1.0f, ui_rgba(1.0f,0.78f,0.25f,0.55f));
                snprintf(buf, sizeof buf, "%s  (HOP %u/%u)", b2, mid, snap->route.count-1);
                ui_text(u, h->f_tiny, bx+8, by+5, C_GOLD, buf);
                snprintf(buf, sizeof buf, "LATENCY %.1f ms", snap->route.hop_ms[mid]);
                ui_text(u, h->f_tiny, bx+8, by+18, C_WHITE, buf);
                snprintf(buf, sizeof buf, "BW %.0f Mbps", snap->route.min_bw_mbps);
                ui_text(u, h->f_tiny, bx+8, by+31, C_GREY, buf);
            }
        }

        /* selection ring + leader tag */
        if (selected_sat >= 0 && selected_sat < (int)snap->sat_count) {
            vec3 p = snap->sat[selected_sat].r;
            float sx, sy;
            if (project(mvp, p, W, Hh, &sx, &sy) && !occluded(eye, p)) {
                UIColor ring = snap->sat[selected_sat].alive ? C_CYAN : C_RED;
                ui_arc(u, sx, sy, 12.0f, 0.0f, 6.2832f, 1.6f, ring);
                ui_arc(u, sx, sy, 17.0f, 0.4f, 1.9f, 1.2f, ui_rgba(ring.r,ring.g,ring.b,0.55f));
                ui_arc(u, sx, sy, 17.0f, 3.5f, 5.0f, 1.2f, ui_rgba(ring.r,ring.g,ring.b,0.55f));
                float tx = sx+22, ty = sy-26;
                ui_line(u, sx+9, sy-9, tx, ty+14, 1.1f, ring);
                sat_name(selected_sat, b2, sizeof b2);
                float tw = ui_text_measure(h->f_small, b2);
                ui_rect(u, tx, ty, tw+14, 17, ui_rgba(0.02f,0.05f,0.09f,0.9f));
                ui_rect(u, tx, ty, 2, 17, ring);
                ui_text(u, h->f_small, tx+7, ty+2, C_WHITE, b2);
            }
        }
    }

    /* floating sim clock (top of viewport) */
    {
        int t = (int)snap->sim_time_s;
        snprintf(buf, sizeof buf, "T+ %02d:%02d:%02d", t/3600, (t/60)%60, t%60);
        ui_text(u, h->f_clock, VL+10, TOP+12, C_WHITE, buf);
        snprintf(buf, sizeof buf, "EPOCH 2026-06-13 00:00Z   STEP %u   DT 5.0 s%s",
                 snap->step_count, paused ? "   [PAUSED]" : "");
        ui_text(u, h->f_tiny, VL+10, TOP+32, C_GREY, buf);
    }

    /* legend (bottom of viewport) */
    {
        float ly = (float)Hh - 34.0f, lx = VL+10;
        ui_text(u, h->f_tiny, lx, ly-14, C_DIM, "ORBITAL PLANES");
        for (int i = 0; i < 6; ++i) {
            ui_circle(u, lx+4+(float)i*46.0f, ly+5, 3.0f, PLANE_COL[i]);
            snprintf(buf, sizeof buf, "P%d/%d", i+1, i+7);
            ui_text(u, h->f_tiny, lx+11+(float)i*46.0f, ly-1, C_GREY, buf);
        }
        float kx = lx + 6*46.0f + 16.0f;
        ui_circle(u, kx, ly+5, 3.0f, C_RED);    ui_text(u, h->f_tiny, kx+7,  ly-1, C_GREY, "OFFLINE");
        hexagon(u, kx+62, ly+5, 4.0f, 1.4f, C_AMBER); ui_text(u, h->f_tiny, kx+71, ly-1, C_GREY, "GROUND STN");
        ui_circle(u, kx+148, ly+5, 3.0f, C_GOLD); ui_text(u, h->f_tiny, kx+155, ly-1, C_GREY, "ACTIVE ROUTE");
    }

    /* focus-mode toggle: always visible (top-right of the viewport) so the
     * panels can be brought back even while collapsed */
    {
        float fbw = 108.0f, fbx = VR - fbw, fby = TOP + 10.0f;
        if (button(h, u, in, fbx, fby, fbw, 20,
                   h->focus_mode ? "SHOW PANELS [F]" : "FOCUS VIEW [F]",
                   C_CYAN, h->focus_mode)) {
            h->focus_mode = !h->focus_mode; act.consumed = 1;
        }
    }

    /* ======================= top status bar ================================= */
    ui_rect(u, 0, 0, (float)W, TOP, ui_rgba(0.012f,0.022f,0.038f,0.97f));
    ui_rect(u, 0, TOP, (float)W, 1.5f, C_EDGE);
    /* brand: orbit glyph + wordmark */
    ui_arc(u, 26, 22, 11.0f, 0.0f, 6.2832f, 1.4f, C_CYAN);
    ui_circle(u, 33.8f, 14.2f, 2.4f, C_WHITE);
    ui_text(u, h->f_logo, 46, 10, C_WHITE, "ASTRA");
    ui_rect(u, 126, 8, 1, TOP-16, C_EDGE);

    {   /* stat blocks */
        float bx = 142;
        ui_text(u, h->f_tiny, bx, 7, C_DIM, "TOTAL OBJECTS");
        snprintf(buf, sizeof buf, "%u", snap->sat_count + snap->gs_count);
        ui_text(u, h->f_med, bx, 20, C_WHITE, buf);
        bx += 110; ui_rect(u, bx-16, 8, 1, TOP-16, C_EDGE);
        ui_text(u, h->f_tiny, bx, 7, C_DIM, "COVERAGE");
        snprintf(buf, sizeof buf, "%.0f%%", cover_frac*100.0f);
        ui_text(u, h->f_med, bx, 20, cover_frac > 0.6f ? C_GREEN : C_AMBER, buf);
        bx += 100; ui_rect(u, bx-16, 8, 1, TOP-16, C_EDGE);
        ui_text(u, h->f_tiny, bx, 7, C_DIM, "ACTIVE NETWORK");
        snprintf(buf, sizeof buf, "%u / %u", up_links, snap->link_count);
        ui_text(u, h->f_med, bx, 20, C_CYAN, buf);
    }
    {   /* right: title + wall clock */
        int t = (int)snap->sim_time_s;
        snprintf(buf, sizeof buf, "T+%02d:%02d:%02d", t/3600, (t/60)%60, t%60);
        float cw = ui_text_measure(h->f_clock, buf);
        ui_text(u, h->f_clock, (float)W-cw-16, 14, C_WHITE, buf);
        const char *t1 = "ASTRA - Autonomous Satellite";
        const char *t2 = "Traffic & Routing Architecture";
        float tw1 = ui_text_measure(h->f_small, t1), tw2 = ui_text_measure(h->f_small, t2);
        ui_text(u, h->f_small, (float)W-cw-40-tw1, 7,  C_GREY, t1);
        ui_text(u, h->f_small, (float)W-cw-40-tw2, 22, C_GREY, t2);
    }

    /* side panels (hidden in focus mode to maximise the 3D viewport) */
    if (!h->focus_mode) {
    /* ======================= left column ==================================== */
    float list_y = TOP + 12.0f;
    float list_h = ((float)Hh - TOP - 36.0f) * 0.52f;
    float cy = panel(h, u, LX, list_y, LW, list_h, "GLOBAL ASSET LIST", "LIVE", C_GREEN);

    /* search field (visual) + tracked count */
    ui_rect(u, LX+10, cy-2, LW-20, 20, C_INSET);
    ui_rect_outline(u, LX+10, cy-2, LW-20, 20, 1.0f, C_EDGE);
    ui_arc(u, LX+21, cy+7, 4.0f, 0.0f, 6.2832f, 1.2f, C_DIM);
    ui_line(u, LX+24, cy+10, LX+28, cy+14, 1.2f, C_DIM);
    ui_text(u, h->f_tiny, LX+34, cy+2, C_DIM, "Search assets...");
    snprintf(buf, sizeof buf, "%u TRACKED", snap->sat_count);
    ui_text(u, h->f_tiny, LX+LW-10-ui_text_measure(h->f_tiny,buf), cy+2, C_DIM, buf);
    cy += 26;

    /* filter chips */
    snprintf(buf, sizeof buf, "ALL %u", snap->sat_count);
    if (chip(h, u, in, LX+10, cy, 64, 17, buf, h->filter==0)) { h->filter = 0; act.consumed = 1; }
    snprintf(buf, sizeof buf, "ACTIVE %u", alive);
    if (chip(h, u, in, LX+78, cy, 84, 17, buf, h->filter==1)) { h->filter = 1; act.consumed = 1; }
    snprintf(buf, sizeof buf, "DOWN %u", snap->sat_count-alive);
    if (chip(h, u, in, LX+166, cy, 74, 17, buf, h->filter==2)) { h->filter = 2; act.consumed = 1; }
    cy += 24;

    /* column header */
    ui_text(u, h->f_tiny, LX+24, cy, C_DIM, "ASSET");
    ui_text(u, h->f_tiny, LX+150, cy, C_DIM, "PLANE");
    ui_text(u, h->f_tiny, LX+LW-58, cy, C_DIM, "STATUS");
    cy += 14;
    ui_rect(u, LX+8, cy, LW-16, 1, C_EDGE);
    cy += 4;

    /* rows (filtered, selection kept in view) */
    {
        int ids[ASTRA_MAX_SATS]; int n = 0, sel_pos = -1;
        for (uint32_t i = 0; i < snap->sat_count; ++i) {
            int up = snap->sat[i].alive;
            if ((h->filter==1 && !up) || (h->filter==2 && up)) continue;
            if ((int)i == selected_sat) sel_pos = n;
            ids[n++] = (int)i;
        }
        int rows = (int)((list_y + list_h - 8 - cy) / 19.0f);
        if (rows < 1) rows = 1;
        if (sel_pos >= 0) {
            if (sel_pos <  h->scroll) h->scroll = sel_pos;
            if (sel_pos >= h->scroll + rows) h->scroll = sel_pos - rows + 1;
        }
        if (h->scroll > n - rows) h->scroll = n - rows;
        if (h->scroll < 0) h->scroll = 0;

        for (int k = 0; k < rows && h->scroll + k < n; ++k) {
            int id = ids[h->scroll + k];
            int up = snap->sat[id].alive;
            float ry = cy + (float)k*19.0f;
            if (id == selected_sat) {
                ui_rect(u, LX+6, ry-2, LW-12, 18, ui_rgba(0.10f,0.30f,0.42f,0.85f));
                ui_rect(u, LX+6, ry-2, 2, 18, C_CYAN);
            } else if (k & 1) ui_rect(u, LX+6, ry-2, LW-12, 18, ui_rgba(1,1,1,0.025f));
            if (hit(in, LX+6, ry-2, LW-12, 18)) { act.select_sat = id; act.consumed = 1; }
            ui_circle(u, LX+15, ry+6, 3.0f, up ? (h->deg[id] ? C_GREEN : C_AMBER) : C_RED);
            sat_name(id, b2, sizeof b2);
            ui_text(u, h->f_body, LX+24, ry, up ? C_WHITE : C_DIM, b2);
            snprintf(buf, sizeof buf, "P%u-S%u", snap->sat[id].plane+1u, snap->sat[id].slot+1u);
            ui_text(u, h->f_tiny, LX+150, ry+2, C_GREY, buf);
            const char *st = up ? (h->deg[id] ? "ACTIVE" : "NO LINK") : "DOWN";
            UIColor sc = up ? (h->deg[id] ? C_GREEN : C_AMBER) : C_RED;
            ui_text(u, h->f_tiny, LX+LW-58, ry+2, sc, st);
        }
        /* scrollbar */
        if (n > rows) {
            float sb_h = (float)rows/(float)n * (float)(rows*19);
            float sb_y = cy + (float)h->scroll/(float)n * (float)(rows*19);
            ui_rect(u, LX+LW-5, cy, 2, (float)(rows*19), ui_rgba(1,1,1,0.06f));
            ui_rect(u, LX+LW-5, sb_y, 2, sb_h, C_DIM);
        }
    }

    /* -------- SELECTED ASSET -------- */
    float sel_y = list_y + list_h + 10.0f;
    float sel_h = (float)Hh - sel_y - 12.0f;
    if (selected_sat >= 0 && selected_sat < (int)snap->sat_count) {
        const SnapSat *s = &snap->sat[selected_sat];
        sat_name(selected_sat, b2, sizeof b2);
        snprintf(buf, sizeof buf, "SELECTED ASSET: %s", b2);
        float sy2 = panel(h, u, LX, sel_y, LW, sel_h, buf,
                          s->alive ? "ONLINE" : "OFFLINE", s->alive ? C_GREEN : C_RED);
        snprintf(buf, sizeof buf, "CATALOG 2026-%03dA   PLANE %u   SLOT %u",
                 selected_sat, s->plane+1u, s->slot+1u);
        ui_text(u, h->f_tiny, LX+10, sy2-4, C_DIM, buf);
        sy2 += 14;

        double rn  = v3_norm(s->r);
        double vel = v3_norm(s->v);
        vec3 hh2   = v3_cross(s->r, s->v);
        double inc = acos(hh2.z / (v3_norm(hh2) + 1e-12)) * 180.0/M_PI;
        double lat = asin(s->r.z / rn) * 180.0/M_PI;
        double lon = atan2(s->r.y, s->r.x) - ASTRA_OMEGA_EARTH*snap->sim_time_s;
        lon = fmod(lon + M_PI, 2.0*M_PI); if (lon < 0) lon += 2.0*M_PI; lon -= M_PI;
        lon *= 180.0/M_PI;

        float colw = (LW-20)*0.5f, gx0 = LX+12, gx1 = LX+12+colw;
        if (!h->show_elems) {
            snprintf(buf,sizeof buf,"%.1f km", rn - ASTRA_EARTH_RADIUS_KM);
            kv(h,u,gx0,sy2,colw,"ALTITUDE",buf,C_WHITE);
            snprintf(buf,sizeof buf,"%.2f km/s", vel);
            kv(h,u,gx1,sy2,colw,"VELOCITY",buf,C_WHITE); sy2 += 30;
            snprintf(buf,sizeof buf,"%.1f deg", inc);
            kv(h,u,gx0,sy2,colw,"INCLINATION",buf,C_WHITE);
            kv(h,u,gx1,sy2,colw,"ORBIT","LEO 550",C_WHITE); sy2 += 30;
            snprintf(buf,sizeof buf,"%+.2f deg", lat);
            kv(h,u,gx0,sy2,colw,"LATITUDE",buf,C_WHITE);
            snprintf(buf,sizeof buf,"%+.2f deg", lon);
            kv(h,u,gx1,sy2,colw,"LONGITUDE",buf,C_WHITE); sy2 += 30;
            const char *role = !s->alive ? "OFFLINE"
                : (h->gs_link[selected_sat] ? "GROUND UPLINK"
                : (h->deg[selected_sat] ? "RELAY NODE" : "ISOLATED"));
            kv(h,u,gx0,sy2,colw,"NETWORK ROLE",role,
               s->alive ? (h->deg[selected_sat]?C_CYAN:C_AMBER) : C_RED);
            if (h->bw[selected_sat] >= 1000.0f)
                 snprintf(buf,sizeof buf,"%.1f Gbps", h->bw[selected_sat]/1000.0f);
            else snprintf(buf,sizeof buf,"%.0f Mbps", h->bw[selected_sat]);
            kv(h,u,gx1,sy2,colw,"AGG. BANDWIDTH",buf,C_WHITE); sy2 += 32;
            /* signal bars from ISL degree */
            ui_text(u, h->f_tiny, gx0, sy2, C_DIM, "LINK SIGNAL");
            int bars_on = h->deg[selected_sat]; if (bars_on > 5) bars_on = 5;
            for (int i = 0; i < 5; ++i) {
                float bh = 5.0f + (float)i*3.0f;
                UIColor c = i < bars_on ? C_GREEN : ui_rgba(0.13f,0.20f,0.27f,1.0f);
                ui_rect(u, gx0+78+(float)i*9.0f, sy2+14-bh, 6, bh, c);
            }
            snprintf(buf,sizeof buf,"%u ISL", h->deg[selected_sat]);
            ui_text(u, h->f_body, gx1, sy2+2, C_GREY, buf);
            sy2 += 24;
        } else {
            OrbitElements coe = astra_rv_to_coe(ASTRA_MU_EARTH, s->r, s->v);
            double period = 2.0*M_PI*sqrt(coe.a*coe.a*coe.a/ASTRA_MU_EARTH)/60.0;
            snprintf(buf,sizeof buf,"%.2f km",coe.a); kv(h,u,gx0,sy2,colw,"SEMI-MAJOR AXIS a",buf,C_WHITE);
            snprintf(buf,sizeof buf,"%.5f",coe.e);    kv(h,u,gx1,sy2,colw,"ECCENTRICITY e",buf,C_WHITE); sy2 += 30;
            snprintf(buf,sizeof buf,"%.2f deg",coe.i*180.0/M_PI);
            kv(h,u,gx0,sy2,colw,"INCLINATION i",buf,C_WHITE);
            snprintf(buf,sizeof buf,"%.2f deg",coe.raan*180.0/M_PI);
            kv(h,u,gx1,sy2,colw,"RAAN",buf,C_WHITE); sy2 += 30;
            snprintf(buf,sizeof buf,"%.2f deg",coe.argp*180.0/M_PI);
            kv(h,u,gx0,sy2,colw,"ARG OF PERIGEE",buf,C_WHITE);
            snprintf(buf,sizeof buf,"%.2f deg",coe.nu*180.0/M_PI);
            kv(h,u,gx1,sy2,colw,"TRUE ANOMALY",buf,C_WHITE); sy2 += 30;
            snprintf(buf,sizeof buf,"%.1f min",period);
            kv(h,u,gx0,sy2,colw,"PERIOD",buf,C_WHITE);
            snprintf(buf,sizeof buf,"%.2f km/s",vel);
            kv(h,u,gx1,sy2,colw,"VELOCITY",buf,C_WHITE); sy2 += 32;
        }
        float by2 = sel_y + sel_h - 30.0f;
        if (button(h,u,in, LX+10, by2, LW-20, 20,
                   h->show_elems ? "BACK TO NETWORK VIEW" : "VIEW ORBITAL ELEMENTS",
                   C_CYAN, 0)) { h->show_elems = !h->show_elems; act.consumed = 1; }
    } else {
        float sy2 = panel(h, u, LX, sel_y, LW, sel_h, "SELECTED ASSET", NULL, C_GREY);
        ui_text(u, h->f_body, LX+12, sy2, C_DIM, "Click a satellite to inspect");
    }

    /* ======================= right column =================================== */
    float perf_h = 296.0f;
    float ry0 = TOP + 12.0f;
    float py = panel(h, u, RX, ry0, RW, perf_h, "NETWORK PERFORMANCE", "TELEMETRY", C_CYAN);

    /* Space Resilience Index */
    ui_text(u, h->f_tiny, RX+12, py-4, C_GREY, "SPACE RESILIENCE INDEX");
    snprintf(buf, sizeof buf, "%.0f", resil);
    ui_text(u, h->f_big, RX+12, py+8, C_WHITE, buf);
    float bw2 = ui_text_measure(h->f_big, buf);
    ui_text(u, h->f_body, RX+16+bw2, py+28, C_DIM, "/100");
    const char *grade = resil < 40 ? "CRITICAL" : resil < 70 ? "DEGRADED" : "NOMINAL";
    UIColor gc = resil < 40 ? C_RED : resil < 70 ? C_AMBER : C_GREEN;
    float gw = ui_text_measure(h->f_small, grade);
    ui_rect(u, RX+RW-gw-30, py+12, gw+18, 18, ui_rgba(gc.r*0.2f,gc.g*0.2f,gc.b*0.2f,1.0f));
    ui_rect_outline(u, RX+RW-gw-30, py+12, gw+18, 18, 1.0f, gc);
    ui_text(u, h->f_small, RX+RW-gw-21, py+14, gc, grade);
    seg_bar(u, RX+12, py+50, RW-24, 6, resil/100.0f, gc);
    ui_text(u, h->f_tiny, RX+12, py+59, C_DIM, "CRITICAL");
    ui_text(u, h->f_tiny, RX+RW-12-ui_text_measure(h->f_tiny,"OPTIMAL"), py+59, C_DIM, "OPTIMAL");

    /* metric grid 2 x 3 */
    {
        struct { const char *name; const float *ring; float vmax; UIColor c; const char *fmt; float cur; } M[6] = {
            {"LINK RELIABILITY", h->links, 0, C_GREEN, "%.0f", (float)up_links},
            {"NETWORK LATENCY",  h->delay_ms, 0, C_CYAN, "%.0fms", snap->avg_delay_s*1000.0f},
            {"THROUGHPUT",       h->deliv, 100.0f, C_GREEN, "%.0f%%", snap->delivery_ratio*100.0f},
            {"PACKET DROP",      h->drop, 100.0f, C_RED, "%.0f%%", (1.0f-snap->delivery_ratio)*100.0f},
            {"LINK UTILISATION", h->util, 0, C_AMBER, "%.1f%%", snap->link_util*100.0f},
            {"ROUTE CHURN",      h->churn, 0, C_AMBER, "%.0f", (float)snap->route_updates},
        };
        float cellw = (RW-32)*0.5f, cellh = 52.0f;
        for (int i = 0; i < 6; ++i) {
            float cx2 = RX+12 + (float)(i%2)*(cellw+8.0f);
            float cy2 = py+78 + (float)(i/2)*(cellh+6.0f);
            ui_rect(u, cx2, cy2, cellw, cellh, C_INSET);
            ui_rect_outline(u, cx2, cy2, cellw, cellh, 1.0f, ui_rgba(0.10f,0.18f,0.26f,0.8f));
            ui_text(u, h->f_tiny, cx2+7, cy2+4, C_DIM, M[i].name);
            float vmax = M[i].vmax;
            if (vmax <= 0) {   /* auto-scale to ring max */
                for (int k2 = 0; k2 < h->hn; ++k2) if (M[i].ring[k2] > vmax) vmax = M[i].ring[k2];
                if (vmax <= 0) vmax = 1.0f;
            }
            spark_bars(u, cx2+7, cy2+18, cellw-58, cellh-26, M[i].ring, h->hn, h->hhead, vmax, M[i].c);
            snprintf(buf, sizeof buf, M[i].fmt, (double)M[i].cur);
            ui_text(u, h->f_body, cx2+cellw-7-ui_text_measure(h->f_body,buf), cy2+cellh-20, M[i].c, buf);
        }
    }

    /* -------- SIMULATION CONTROL & CONFIG -------- */
    float ctl_y = ry0 + perf_h + 10.0f, ctl_h = 188.0f;
    float cc = panel(h, u, RX, ctl_y, RW, ctl_h, "SIMULATION CONTROL & CONFIG",
                     paused ? "PAUSED" : "RUNNING", paused ? C_AMBER : C_GREEN);

    ui_text(u, h->f_tiny, RX+12, cc, C_DIM, "TRAFFIC LOAD");
    snprintf(buf, sizeof buf, "%.1f%%", snap->link_util*100.0f);
    ui_text(u, h->f_tiny, RX+RW-12-ui_text_measure(h->f_tiny,buf), cc, C_GREY, buf);
    seg_bar(u, RX+12, cc+13, RW-24, 5, snap->link_util*4.0f, C_AMBER);
    cc += 28;

    ui_text(u, h->f_tiny, RX+12, cc+3, C_DIM, "ROUTE OPTIMISATION");
    cc += 17;
    {
        int m_lat  = !route_dv && !cost_hops;
        int m_hop  = !route_dv &&  cost_hops;
        if (chip(h,u,in, RX+12,  cc, 92, 18, "MIN LATENCY",  m_lat)) { act.set_route_mode=0; act.set_cost_mode=0; act.consumed=1; }
        if (chip(h,u,in, RX+108, cc, 80, 18, "MIN HOPS",     m_hop)) { act.set_route_mode=0; act.set_cost_mode=1; act.consumed=1; }
        if (chip(h,u,in, RX+192, cc, 116, 18, "DISTANCE VECTOR", route_dv)) { act.set_route_mode=1; act.set_cost_mode=0; act.consumed=1; }
    }
    cc += 26;

    ui_text(u, h->f_tiny, RX+12, cc+4, C_DIM, "SIM SPEED");
    snprintf(buf, sizeof buf, "%.2fx", speed);
    ui_text(u, h->f_body, RX+86, cc+2, C_WHITE, buf);
    if (button(h,u,in, RX+150, cc, 22, 18, "-", C_GREY, 0))
        { act.set_speed = speed*0.5 < 0.25 ? 0.25 : speed*0.5; act.consumed = 1; }
    if (button(h,u,in, RX+176, cc, 22, 18, "+", C_GREY, 0))
        { act.set_speed = speed*2.0 > 32.0 ? 32.0 : speed*2.0; act.consumed = 1; }
    if (button(h,u,in, RX+RW-104, cc, 92, 18, paused ? "RESUME" : "PAUSE",
               paused ? C_GREEN : C_AMBER, 1)) { act.toggle_pause = 1; act.consumed = 1; }
    cc += 26;

    if (button(h,u,in, RX+12, cc, (RW-32)*0.5f, 20, "STRIKE SELECTED", C_RED, 1))
        { act.strike = 1; act.consumed = 1; }
    if (button(h,u,in, RX+20+(RW-32)*0.5f, cc, (RW-32)*0.5f, 20, "REBOOT ALL", C_GREEN, 1))
        { act.reboot = 1; act.consumed = 1; }
    cc += 28;
    ui_text(u, h->f_tiny, RX+12, cc, C_DIM, "FAILURE MODELLING");
    ui_text(u, h->f_tiny, RX+12+110, cc, C_GREEN, "LIVE COMMAND");
    cc += 13;
    ui_text(u, h->f_tiny, RX+12, cc, C_DIM,
            "DRAG ORBIT / WHEEL ZOOM / CLICK SELECT / S STRIKE / F FOCUS");

    /* -------- ACTIVE ROUTE -------- */
    float rt_y = ctl_y + ctl_h + 10.0f;
    float rt_h = (float)Hh - rt_y - 12.0f;
    {
        const SnapRoute *rt = &snap->route;
        char src[8] = "-", dst[8] = "-";
        if (rt->src_gid) gs_code(snap, rt->src_gid, src);
        if (rt->dst_gid) gs_code(snap, rt->dst_gid, dst);
        snprintf(buf, sizeof buf, "ACTIVE ROUTE: %s > %s", src, dst);
        float rr = panel(h, u, RX, rt_y, RW, rt_h, buf,
                         rt->valid ? "STREAMING" : "NO PATH", rt->valid ? C_GOLD : C_RED);
        if (rt->valid) {
            snprintf(buf, sizeof buf, "STREAM-%03u / END-TO-END %.1f ms / BW %.0f Mbps",
                     (unsigned)(snap->route.count*7u%997u), rt->total_ms, rt->min_bw_mbps);
            ui_text(u, h->f_tiny, RX+12, rr-4, C_GREY, buf);
            rr += 12;

            /* physical link budget of the route's binding hop: sat-sat hops are
             * optical (laser ISL), sat-ground hops are Ka RF. The hop with the
             * lowest achievable rate sets the route's real capacity ceiling. */
            {
                double bn_gbps = 1e30, bn_margin = 0; float bn_range = 0;
                char   bn_detail[28] = ""; int bn_found = 0;
                for (uint32_t kk = 0; kk + 1 < rt->count; ++kk) {
                    node_id a = rt->node[kk], b = rt->node[kk+1];
                    float d = snap_link_dist(snap, a, b);
                    if (d < 0.0f) continue;
                    int ground = (a >= snap->sat_count) || (b >= snap->sat_count);
                    double gbps, margin; char det[28];
                    if (ground) {
                        RfLink r = astra_rf_budget(&ASTRA_RF_KA_GW_UP, d);
                        gbps = r.rate_gbps; margin = r.margin_db;
                        snprintf(det, sizeof det, "Ka %s", r.modcod);
                    } else {
                        OpticalLink o = astra_optical_budget(&ASTRA_OPTICAL_ISL, d);
                        gbps = o.rate_gbps; margin = o.margin_db;
                        snprintf(det, sizeof det, "OPTICAL ISL");
                    }
                    if (gbps < bn_gbps || (gbps == bn_gbps && margin < bn_margin)) {
                        bn_gbps = gbps; bn_margin = margin; bn_range = d; bn_found = 1;
                        snprintf(bn_detail, sizeof bn_detail, "%s", det);
                    }
                }
                if (bn_found) {
                    UIColor mc = bn_gbps <= 0.0 ? C_RED : (bn_margin < 3.0 ? C_AMBER : C_GREEN);
                    if (bn_gbps <= 0.0)
                        snprintf(buf, sizeof buf, "RF CAP  OUTAGE  %s @ %.0f km  %+.1f dB",
                                 bn_detail, bn_range, bn_margin);
                    else
                        snprintf(buf, sizeof buf, "RF CAP %.2f Gbps  %s  M%+.1f dB",
                                 bn_gbps, bn_detail, bn_margin);
                    ui_text(u, h->f_tiny, RX+12, rr-4, mc, buf);
                    rr += 13;
                }
            }

            int maxrows = (int)((rt_y + rt_h - 64 - rr) / 17.0f);
            int nshow = (int)rt->count; int skipped = 0;
            if (nshow > maxrows) { skipped = nshow - maxrows; nshow = maxrows; }
            for (int k = 0; k < nshow; ++k) {
                uint32_t idx = (k == nshow-1) ? rt->count-1u : (uint32_t)k;
                if (k == nshow-1 && skipped) {
                    snprintf(buf, sizeof buf, "... %d more hops ...", skipped);
                    ui_text(u, h->f_tiny, RX+26, rr+2, C_DIM, buf);
                    rr += 17;
                }
                node_label(snap, rt->node[idx], b2, sizeof b2);
                UIColor bc = idx == 0 || idx == rt->count-1u ? C_AMBER : C_GOLD;
                ui_circle(u, RX+18, rr+7, 2.6f, bc);
                if (idx == 0) snprintf(buf, sizeof buf, "ORIGIN  %s", b2);
                else          snprintf(buf, sizeof buf, "HOP %-2u  %s", idx, b2);
                ui_text(u, h->f_body, RX+27, rr, C_WHITE, buf);
                if (idx > 0) {
                    snprintf(buf, sizeof buf, "%.1fms", rt->hop_ms[idx]);
                    ui_text(u, h->f_body, RX+RW-12-ui_text_measure(h->f_body,buf), rr, C_GOLD, buf);
                }
                rr += 17;
            }
            float sp_y = rt_y + rt_h - 50;
            ui_text(u, h->f_tiny, RX+12, sp_y-2, C_DIM, "STREAM LATENCY (RECENT)");
            float rmax = 1.0f;
            for (int k2 = 0; k2 < h->hn; ++k2) if (h->route_ms[k2] > rmax) rmax = h->route_ms[k2];
            spark_line(u, RX+12, sp_y+12, RW-24, 26, h->route_ms, h->hn, h->hhead, rmax*1.2f, C_GOLD);
        } else {
            ui_text(u, h->f_body, RX+12, rr+6, C_RED, "NETWORK PARTITIONED");
            ui_text(u, h->f_tiny, RX+12, rr+24, C_DIM, "No forwarding path between endpoints.");
            ui_text(u, h->f_tiny, RX+12, rr+38, C_DIM, "Try --range 5000 or REBOOT ALL.");
        }
    }
    }   /* end side panels */

    /* consume any other click that landed on panel chrome (only the top bar
     * is chrome in focus mode; the side columns are gone) */
    if (in && in->click && !act.consumed) {
        if (in->cy <= TOP) act.consumed = 1;
        else if (!h->focus_mode && (in->cx <= LX+LW+4 || in->cx >= RX-4)) act.consumed = 1;
    }
    if (out) *out = act;
}
