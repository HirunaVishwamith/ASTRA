/* ui.h — batched immediate-mode 2D overlay (screen-space) for the HUD.
 * Accumulate rects / lines / text during a frame, then flush in two draws
 * (shapes, then text via a FreeType glyph atlas). Pixel coordinates, origin
 * top-left. Colours are RGBA floats 0..1. */
#ifndef ASTRA_UI_H
#define ASTRA_UI_H

typedef struct UIFont UIFont;   /* opaque: FreeType-baked glyph atlas */
typedef struct UI UI;

typedef struct { float r, g, b, a; } UIColor;
static inline UIColor ui_rgba(float r,float g,float b,float a){ return (UIColor){r,g,b,a}; }

UI   *ui_create(void);
void  ui_destroy(UI *u);

/* Load a TTF at a pixel height; returns NULL on failure. Owned by caller. */
UIFont *ui_font_load(const char *ttf_path, int pixel_height);
void    ui_font_free(UIFont *f);
int     ui_font_line_height(const UIFont *f);

void  ui_begin(UI *u, int screen_w, int screen_h);

/* shapes */
void  ui_rect(UI *u, float x, float y, float w, float h, UIColor c);
void  ui_rect_outline(UI *u, float x, float y, float w, float h, float t, UIColor c);
void  ui_line(UI *u, float x0, float y0, float x1, float y1, float t, UIColor c);
/* arc from a0..a1 radians (0 = +x, CCW), radius rad, stroke t */
void  ui_arc(UI *u, float cx, float cy, float rad, float a0, float a1, float t, UIColor c);
/* filled circle (triangle fan) */
void  ui_circle(UI *u, float cx, float cy, float rad, UIColor c);
void  ui_tri(UI *u, float x0,float y0,float x1,float y1,float x2,float y2, UIColor c);

/* text: top-left at (x,y). Returns advance width in px. */
float ui_text(UI *u, const UIFont *f, float x, float y, UIColor c, const char *s);
float ui_text_measure(const UIFont *f, const char *s);

void  ui_end(UI *u);   /* flush all batched geometry */

#endif /* ASTRA_UI_H */
