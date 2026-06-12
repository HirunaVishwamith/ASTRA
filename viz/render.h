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

/* Draw one frame into the currently-bound framebuffer. */
void      render_frame(Renderer *r, const RenderSnapshot *snap, Camera cam);

#endif /* ASTRA_RENDER_H */
