/* render.h — the ASTRA scene renderer: textured-look globe, instanced
 * satellites, and ISL/ground links coloured by utilisation. Consumes an
 * immutable RenderSnapshot; knows nothing about the simulation internals. */
#ifndef ASTRA_RENDER_H
#define ASTRA_RENDER_H

#include "astra/sim.h"   /* RenderSnapshot */

typedef struct {
    float az;     /* azimuth around +Y (rad)            */
    float el;     /* elevation (rad)                    */
    float dist;   /* distance from origin (world units) */
    float fov;    /* vertical FOV (rad)                 */
} Camera;

typedef struct Renderer Renderer;

Renderer *render_create(int w, int h);
void      render_destroy(Renderer *r);
void      render_resize(Renderer *r, int w, int h);   /* viewport/aspect only */

/* Draw one frame into the currently-bound framebuffer. selected_sat gets a
 * bright marker (-1 = none). */
void      render_frame(Renderer *r, const RenderSnapshot *snap, Camera cam,
                       int selected_sat);

/* The view-projection matrix (column-major, 16 floats) for a camera at (w,h).
 * Same transform render_frame uses, so callers can project world points to
 * screen for picking / 2D callouts. World units are ECI-km * 0.001. */
void      render_view_proj(Camera cam, int w, int h, float out_mvp[16]);
float     render_world_scale(void);   /* km -> world units (0.001) */

#endif /* ASTRA_RENDER_H */
