/* ui.c — batched 2D overlay + FreeType glyph atlas. */
#include "ui.h"
#include "glfn.h"
#include "shader.h"
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ft2build.h>
#include FT_FREETYPE_H

/* ---- font ---------------------------------------------------------------- */
typedef struct { float u0,v0,u1,v1; int w,h,bx,by,adv; } Glyph;
struct UIFont { GLuint tex; int px, ascent, line_h; Glyph g[96]; };

static FT_Library FT = NULL;

UIFont *ui_font_load(const char *path, int px) {
    if (!FT && FT_Init_FreeType(&FT)) return NULL;
    FT_Face face;
    if (FT_New_Face(FT, path, 0, &face)) return NULL;
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);

    const int W = 512, pad = 1;
    /* pass 1: packing positions (glyph w/h recomputed in pass 2) */
    int gx[96], gy[96];
    int cx = pad, cy = pad, shelf = 0;
    for (int i = 0; i < 96; ++i) {
        if (FT_Load_Char(face, (FT_ULong)(32+i), FT_LOAD_RENDER)) { gx[i]=cx; gy[i]=cy; continue; }
        int w = (int)face->glyph->bitmap.width, h = (int)face->glyph->bitmap.rows;
        if (cx + w + pad > W) { cx = pad; cy += shelf + pad; shelf = 0; }
        gx[i]=cx; gy[i]=cy;
        cx += w + pad; if (h > shelf) shelf = h;
    }
    int H = 1; int need = cy + shelf + pad; while (H < need) H <<= 1;

    unsigned char *atlas = (unsigned char *)calloc((size_t)W*(size_t)H, 1);
    UIFont *f = (UIFont *)calloc(1, sizeof(*f));
    f->px = px;
    f->ascent = (int)(face->size->metrics.ascender >> 6);
    f->line_h = (int)(face->size->metrics.height  >> 6);

    /* pass 2: render glyphs into the atlas + record metrics */
    for (int i = 0; i < 96; ++i) {
        if (FT_Load_Char(face, (FT_ULong)(32+i), FT_LOAD_RENDER)) continue;
        FT_GlyphSlot s = face->glyph;
        int w = (int)s->bitmap.width, h = (int)s->bitmap.rows;
        for (int row = 0; row < h; ++row)
            memcpy(atlas + (size_t)(gy[i]+row)*(size_t)W + (size_t)gx[i],
                   s->bitmap.buffer + (size_t)row*(size_t)s->bitmap.pitch, (size_t)w);
        Glyph *G = &f->g[i];
        G->w=w; G->h=h; G->bx=s->bitmap_left; G->by=s->bitmap_top; G->adv=(int)(s->advance.x >> 6);
        G->u0=(float)gx[i]/(float)W; G->v0=(float)gy[i]/(float)H;
        G->u1=(float)(gx[i]+w)/(float)W; G->v1=(float)(gy[i]+h)/(float)H;
    }

    glGenTextures(1, &f->tex);
    glBindTexture(GL_TEXTURE_2D, f->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, W, H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(atlas);
    FT_Done_Face(face);
    return f;
}
void ui_font_free(UIFont *f) { if (f) free(f); }
int  ui_font_line_height(const UIFont *f) { return f->line_h; }

/* ---- UI batches ---------------------------------------------------------- */
#define SHAPE_CAP 120000   /* verts (pos2+col4) */
#define TEXT_CAP   90000   /* verts per font batch (pos2+uv2+col4) */
#define MAX_FONTS  8

typedef struct { const UIFont *f; float *buf; int n, cap; } TextBatch;

struct UI {
    int w, h;
    GLuint shape_prog, text_prog;
    GLint  sh_screen, tx_screen, tx_tex;
    GLuint shape_vao, shape_vbo, text_vao, text_vbo;
    float *shape; int shape_n;
    TextBatch tb[MAX_FONTS]; int ntb;
};

static const char *SHAPE_VS =
    "#version 330 core\nlayout(location=0) in vec2 pos; layout(location=1) in vec4 col;\n"
    "uniform vec2 uScreen; out vec4 vcol;\n"
    "void main(){ vcol=col; gl_Position=vec4(2.0*pos.x/uScreen.x-1.0, 1.0-2.0*pos.y/uScreen.y, 0.0, 1.0); }\n";
static const char *SHAPE_FS =
    "#version 330 core\nin vec4 vcol; out vec4 frag; void main(){ frag=vcol; }\n";
static const char *TEXT_VS =
    "#version 330 core\nlayout(location=0) in vec2 pos; layout(location=1) in vec2 uv; layout(location=2) in vec4 col;\n"
    "uniform vec2 uScreen; out vec2 vuv; out vec4 vcol;\n"
    "void main(){ vuv=uv; vcol=col; gl_Position=vec4(2.0*pos.x/uScreen.x-1.0, 1.0-2.0*pos.y/uScreen.y, 0.0, 1.0); }\n";
static const char *TEXT_FS =
    "#version 330 core\nin vec2 vuv; in vec4 vcol; out vec4 frag; uniform sampler2D uTex;\n"
    "void main(){ float a=texture(uTex,vuv).r; frag=vec4(vcol.rgb, vcol.a*a); }\n";

UI *ui_create(void) {
    UI *u = (UI *)calloc(1, sizeof(*u));
    u->shape_prog = shader_build(SHAPE_VS, SHAPE_FS);
    u->text_prog  = shader_build(TEXT_VS, TEXT_FS);
    if (!u->shape_prog || !u->text_prog) { free(u); return NULL; }
    u->sh_screen = glGetUniformLocation(u->shape_prog, "uScreen");
    u->tx_screen = glGetUniformLocation(u->text_prog, "uScreen");
    u->tx_tex    = glGetUniformLocation(u->text_prog, "uTex");

    glGenVertexArrays(1, &u->shape_vao); glBindVertexArray(u->shape_vao);
    glGenBuffers(1, &u->shape_vbo); glBindBuffer(GL_ARRAY_BUFFER, u->shape_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)SHAPE_CAP*6u*sizeof(float)), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(2*sizeof(float)));

    glGenVertexArrays(1, &u->text_vao); glBindVertexArray(u->text_vao);
    glGenBuffers(1, &u->text_vbo); glBindBuffer(GL_ARRAY_BUFFER, u->text_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)TEXT_CAP*8u*sizeof(float)), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(2*sizeof(float)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,8*sizeof(float),(void*)(4*sizeof(float)));

    u->shape = (float *)malloc((size_t)SHAPE_CAP*6u*sizeof(float));
    for (int i = 0; i < MAX_FONTS; ++i) { u->tb[i].buf = (float *)malloc((size_t)TEXT_CAP*8u*sizeof(float)); u->tb[i].cap = TEXT_CAP; }
    return u;
}

void ui_destroy(UI *u) {
    if (!u) return;
    free(u->shape);
    for (int i = 0; i < MAX_FONTS; ++i) free(u->tb[i].buf);
    free(u);
}

void ui_begin(UI *u, int w, int h) {
    u->w = w; u->h = h; u->shape_n = 0; u->ntb = 0;
    for (int i = 0; i < MAX_FONTS; ++i) { u->tb[i].n = 0; u->tb[i].f = NULL; }
}

static void push_vert(UI *u, float x, float y, UIColor c) {
    if (u->shape_n >= SHAPE_CAP) return;
    float *v = &u->shape[u->shape_n*6];
    v[0]=x; v[1]=y; v[2]=c.r; v[3]=c.g; v[4]=c.b; v[5]=c.a;
    u->shape_n++;
}

void ui_tri(UI *u, float x0,float y0,float x1,float y1,float x2,float y2, UIColor c) {
    push_vert(u,x0,y0,c); push_vert(u,x1,y1,c); push_vert(u,x2,y2,c);
}
void ui_rect(UI *u, float x, float y, float w, float h, UIColor c) {
    ui_tri(u, x,y, x+w,y, x+w,y+h, c);
    ui_tri(u, x,y, x+w,y+h, x,y+h, c);
}
void ui_rect_outline(UI *u, float x, float y, float w, float h, float t, UIColor c) {
    ui_rect(u, x, y, w, t, c);             /* top    */
    ui_rect(u, x, y+h-t, w, t, c);         /* bottom */
    ui_rect(u, x, y, t, h, c);             /* left   */
    ui_rect(u, x+w-t, y, t, h, c);         /* right  */
}
void ui_line(UI *u, float x0,float y0,float x1,float y1, float t, UIColor c) {
    float dx=x1-x0, dy=y1-y0, len=sqrtf(dx*dx+dy*dy);
    if (len < 1e-4f) return;
    float nx=-dy/len*t*0.5f, ny=dx/len*t*0.5f;
    ui_tri(u, x0+nx,y0+ny, x1+nx,y1+ny, x1-nx,y1-ny, c);
    ui_tri(u, x0+nx,y0+ny, x1-nx,y1-ny, x0-nx,y0-ny, c);
}
void ui_arc(UI *u, float cx, float cy, float rad, float a0, float a1, float t, UIColor c) {
    int seg = (int)(fabsf(a1-a0)/0.18f) + 2;
    float prevx = cx + rad*cosf(a0), prevy = cy - rad*sinf(a0);
    for (int i = 1; i <= seg; ++i) {
        float a = a0 + (a1-a0)*(float)i/(float)seg;
        float x = cx + rad*cosf(a), y = cy - rad*sinf(a);
        ui_line(u, prevx, prevy, x, y, t, c);
        prevx = x; prevy = y;
    }
}

static TextBatch *batch_for(UI *u, const UIFont *f) {
    for (int i = 0; i < u->ntb; ++i) if (u->tb[i].f == f) return &u->tb[i];
    if (u->ntb >= MAX_FONTS) return &u->tb[0];
    TextBatch *b = &u->tb[u->ntb++]; b->f = f; b->n = 0; return b;
}

float ui_text(UI *u, const UIFont *f, float x, float y, UIColor c, const char *s) {
    TextBatch *b = batch_for(u, f);
    float pen = x, base = y + (float)f->ascent;
    for (; *s; ++s) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 127) { ch = '?'; }
        const Glyph *g = &f->g[ch-32];
        if (g->w > 0 && b->n + 6 <= b->cap) {
            float x0 = pen + (float)g->bx, y0 = base - (float)g->by;
            float x1 = x0 + (float)g->w, y1 = y0 + (float)g->h;
            float *V = &b->buf[b->n*8];
            #define TV(px,py,uu,vv) do{ V[0]=px;V[1]=py;V[2]=uu;V[3]=vv;V[4]=c.r;V[5]=c.g;V[6]=c.b;V[7]=c.a; V+=8; }while(0)
            TV(x0,y0,g->u0,g->v0); TV(x1,y0,g->u1,g->v0); TV(x1,y1,g->u1,g->v1);
            TV(x0,y0,g->u0,g->v0); TV(x1,y1,g->u1,g->v1); TV(x0,y1,g->u0,g->v1);
            #undef TV
            b->n += 6;
        }
        pen += (float)g->adv;
    }
    return pen - x;
}

float ui_text_measure(const UIFont *f, const char *s) {
    float pen = 0;
    for (; *s; ++s) { int ch=(unsigned char)*s; if (ch<32||ch>127) ch='?'; pen += (float)f->g[ch-32].adv; }
    return pen;
}

void ui_end(UI *u) {
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (u->shape_n > 0) {
        glUseProgram(u->shape_prog);
        glUniform2f(u->sh_screen, (float)u->w, (float)u->h);
        glBindVertexArray(u->shape_vao);
        glBindBuffer(GL_ARRAY_BUFFER, u->shape_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)u->shape_n*6u*sizeof(float)), u->shape);
        glDrawArrays(GL_TRIANGLES, 0, u->shape_n);
    }
    glUseProgram(u->text_prog);
    glUniform2f(u->tx_screen, (float)u->w, (float)u->h);
    glUniform1i(u->tx_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(u->text_vao);
    for (int i = 0; i < u->ntb; ++i) {
        TextBatch *b = &u->tb[i];
        if (b->n == 0) continue;
        glBindTexture(GL_TEXTURE_2D, b->f->tex);
        glBindBuffer(GL_ARRAY_BUFFER, u->text_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)b->n*8u*sizeof(float)), b->buf);
        glDrawArrays(GL_TRIANGLES, 0, b->n);
    }
}
