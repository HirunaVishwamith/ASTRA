/* hud.h — the ASTRA mission-control dashboard, drawn on top of the 3D scene.
 * Owns its fonts + metric history; lays out panels/gauges/plots from a
 * RenderSnapshot. Pure presentation — reads the snapshot, never the sim. */
#ifndef ASTRA_HUD_H
#define ASTRA_HUD_H

#include "ui.h"
#include "astra/sim.h"   /* RenderSnapshot */

typedef struct Hud Hud;

Hud  *hud_create(void);
void  hud_destroy(Hud *h);

/* Draw the whole dashboard. selected_sat highlights an asset (-1 = none);
 * paused/route_dv/speed reflect sim control state for the readouts. */
void  hud_draw(Hud *h, UI *u, const RenderSnapshot *snap,
               int screen_w, int screen_h,
               int selected_sat, int paused, int route_dv, double speed);

#endif /* ASTRA_HUD_H */
