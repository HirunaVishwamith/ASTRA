/* hud.h — the ASTRA mission-control dashboard, drawn on top of the 3D scene.
 * Owns its fonts, metric history, and panel-local UI state (list filter/scroll,
 * orbital-elements toggle); lays out the full operator console from a
 * RenderSnapshot. Pure presentation — reads the snapshot, never the sim.
 *
 * Interaction is immediate-mode: pass this frame's click in HudInput; the HUD
 * hit-tests its widgets and reports what the app should do in HudActions
 * (consumed=1 means the click landed on chrome, so don't 3D-pick with it). */
#ifndef ASTRA_HUD_H
#define ASTRA_HUD_H

#include "ui.h"
#include "astra/sim.h"   /* RenderSnapshot */

typedef struct Hud Hud;

typedef struct {
    int   click;        /* left click this frame            */
    float cx, cy;       /* click position, top-left origin  */
    int   toggle_focus; /* F pressed: collapse/expand panels */
    int   toggle_help;  /* H pressed: show/hide controls overlay */
} HudInput;

typedef struct {
    int    consumed;        /* click was on a HUD element             */
    int    select_sat;      /* -1 = no change                         */
    int    toggle_pause;
    int    strike;          /* strike the selected satellite          */
    int    reboot;
    int    set_route_mode;  /* -1 none | 0 Dijkstra | 1 DV            */
    int    set_cost_mode;   /* -1 none | 0 latency  | 1 hops          */
    double set_speed;       /* 0 = no change                          */
} HudActions;

Hud  *hud_create(void);
void  hud_destroy(Hud *h);

/* Draw the dashboard. mvp/eye are the 3D frame's view-projection and camera
 * position (render_view_proj / world units) for projecting station labels,
 * route callouts, and the selection ring. in/out may be NULL. */
void  hud_draw(Hud *h, UI *u, const RenderSnapshot *snap,
               int screen_w, int screen_h,
               int selected_sat, int paused, int route_dv, int cost_hops,
               double speed, const float mvp[16], const float eye[3],
               const HudInput *in, HudActions *out);

#endif /* ASTRA_HUD_H */
