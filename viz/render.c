/* render.c — ASTRA scene renderer: photoreal Earth + constellation.
 *
 * Layers (back to front): starfield, textured globe (day/night terminator with
 * city lights + ocean specular), additive atmosphere limb, ISL/ground links
 * (additive, util-coloured), satellites + ground stations as glowing points. */
#include "render.h"
#include "glctx.h"
#include "glfn.h"
#include "shader.h"
#include "mat4.h"
#include "image.h"
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WORLD_SCALE   0.001f
#define EARTH_R_WORLD (6378.137f * WORLD_SCALE)
#define SPH_STACKS    96
#define SPH_SLICES    192
#define N_STARS       2600
#define TRAIL_LEN     80
#define EARTH_TEX_PATH "img/earth_texture.jpg"

/* ---- shaders ------------------------------------------------------------- */
/* Globe vertices are authored in ECEF axes (z = north pole, x = lon 0) so the
 * texture maps by lat/lon directly; uModel carries Earth rotation + the
 * ECEF->render axis change. */
static const char *GLOBE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos;\n"
    "layout(location=1) in vec2 uv;\n"
    "uniform mat4 uMVP; uniform mat4 uModel; uniform float uScale;\n"
    "out vec3 vN; out vec3 vWorld; out vec2 vUV;\n"
    "void main(){ vN=normalize(mat3(uModel)*pos); vUV=uv; vec3 p=pos*uScale;\n"
    "  vWorld=(uModel*vec4(p,1.0)).xyz; gl_Position=uMVP*vec4(p,1.0); }\n";
static const char *GLOBE_FS =
    "#version 330 core\n"
    "in vec3 vN; in vec3 vWorld; in vec2 vUV; out vec4 frag;\n"
    "uniform sampler2D uTex; uniform int uHasTex;\n"
    "uniform vec3 uSun; uniform vec3 uCam;\n"
    "void main(){\n"
    "  vec3 n = normalize(vN);\n"
    "  vec3 day = (uHasTex==1) ? texture(uTex, vUV).rgb : vec3(0.07,0.13,0.27);\n"
    "  vec3 sun = normalize(uSun);\n"
    "  float ndl = dot(n, sun);\n"
    "  float dayAmt = smoothstep(-0.18, 0.22, ndl);\n"
    "  vec3 viewDir = normalize(uCam - vWorld);\n"
    "  float ocean = clamp((day.b - max(day.r,day.g))*4.0, 0.0, 1.0);\n"
    "  vec3 h = normalize(sun + viewDir);\n"
    "  float spec = pow(max(dot(n,h),0.0), 60.0) * ocean * dayAmt;\n"
    "  float land = clamp((max(day.r,day.g)-day.b)*5.0, 0.0, 1.0);\n"
    "  float bright = dot(day, vec3(0.33));\n"
    "  float cities = land * smoothstep(0.13,0.42,bright);\n"
    "  vec3 night = day*0.04 + vec3(1.0,0.74,0.40)*cities*(1.0-dayAmt)*1.5;\n"
    "  vec3 lit = day*(0.12 + 1.10*max(ndl,0.0));\n"
    "  vec3 col = mix(night, lit, dayAmt);\n"
    "  col += vec3(1.0,0.95,0.82)*spec*0.7;\n"
    "  float term = 1.0-abs(dayAmt*2.0-1.0);\n"
    "  col += vec3(0.55,0.28,0.10)*term*0.12;\n"
    "  float fres = pow(1.0-max(dot(n,viewDir),0.0), 3.0);\n"
    "  col += vec3(0.20,0.45,0.95)*fres*(0.30+0.70*dayAmt);\n"
    "  frag = vec4(col, 1.0);\n"
    "}\n";

static const char *ATMO_FS =
    "#version 330 core\n"
    "in vec3 vN; in vec3 vWorld; in vec2 vUV; out vec4 frag;\n"
    "uniform vec3 uSun; uniform vec3 uCam;\n"
    "void main(){\n"
    "  vec3 n = normalize(vN); vec3 vd = normalize(uCam - vWorld);\n"
    "  float fres = pow(1.0-max(dot(n,vd),0.0), 2.2);\n"
    "  float ndl = max(dot(n, normalize(uSun)), 0.0);\n"
    "  float i = fres*(0.18 + 0.95*ndl);\n"
    "  frag = vec4(vec3(0.30,0.55,1.0)*i, i);\n"
    "}\n";

static const char *STAR_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos;\n"
    "layout(location=1) in float mag;\n"
    "uniform mat4 uVP; out float vm;\n"
    "void main(){ vm=mag; gl_Position=uVP*vec4(pos,1.0); gl_PointSize=mag*2.2+0.4; }\n";
static const char *STAR_FS =
    "#version 330 core\n"
    "in float vm; out vec4 frag;\n"
    "void main(){ vec2 d=gl_PointCoord-0.5; if(dot(d,d)>0.25) discard;\n"
    "  frag=vec4(vec3(0.8,0.85,1.0), vm); }\n";

static const char *LINE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos; layout(location=1) in vec3 col;\n"
    "uniform mat4 uMVP; out vec3 vcol;\n"
    "void main(){ vcol=col; gl_Position=uMVP*vec4(pos,1.0); }\n";
static const char *LINE_FS =
    "#version 330 core\n"
    "in vec3 vcol; out vec4 frag; uniform float uAlpha;\n"
    "void main(){ frag=vec4(vcol, uAlpha); }\n";

static const char *PT_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos; layout(location=1) in vec3 col;\n"
    "uniform mat4 uMVP; uniform float uScale; out vec3 vcol;\n"
    "void main(){ vcol=col; vec4 p=uMVP*vec4(pos,1.0); gl_Position=p;\n"
    "  gl_PointSize=clamp(uScale/p.w, 2.0, 40.0); }\n";
static const char *PT_FS =
    "#version 330 core\n"
    "in vec3 vcol; out vec4 frag;\n"
    "void main(){ vec2 d=gl_PointCoord-0.5; float r2=dot(d,d); if(r2>0.25) discard;\n"
    "  float a=smoothstep(0.25,0.0,r2); vec3 c=mix(vec3(1.0),vcol,smoothstep(0.0,0.12,r2));\n"
    "  frag=vec4(c, a); }\n";

static const char *TRAIL_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos; layout(location=1) in vec4 col;\n"
    "uniform mat4 uMVP; out vec4 vcol;\n"
    "void main(){ vcol=col; gl_Position=uMVP*vec4(pos,1.0); }\n";
static const char *TRAIL_FS =
    "#version 330 core\n"
    "in vec4 vcol; out vec4 frag; void main(){ frag=vcol; }\n";

/* ---- renderer state ------------------------------------------------------ */
struct Renderer {
    int w, h;
    GLuint globe_prog, atmo_prog, star_prog, line_prog, pt_prog, trail_prog;
    GLint  g_mvp, g_model, g_scale, g_tex, g_hastex, g_sun, g_cam;
    GLint  a_mvp, a_model, a_scale, a_sun, a_cam;
    GLint  s_vp, l_mvp, l_alpha, p_mvp, p_scale, t_mvp;

    GLuint globe_vao, globe_vbo, globe_ebo; int globe_index_count;
    GLuint star_vao, star_vbo; int star_count;
    GLuint line_vao, line_vbo;
    GLuint pt_vao, pt_vbo;
    GLuint trail_vao, trail_vbo;
    GLuint earth_tex; int has_tex;

    fv3   *node_pos;
    float *line_buf;
    float *pt_buf;

    /* per-satellite orbit trail history (ring of recent ECI-km positions) */
    fv3      *trail;     /* [sat*TRAIL_LEN + slot] */
    uint16_t *tfill;     /* samples held per sat   */
    float    *trail_buf; /* scratch verts: pos3 + rgba4 */
    int       thead;
    uint64_t  last_trail_frame;
};

/* ---- sphere mesh in ECEF axes (z = pole), equirectangular uv -------------
 * u=0 is longitude 180 W (left edge of a standard Earth map), v=0 the north
 * pole, so geodetic lat/lon land on the correct pixels of the texture. */
static void build_globe(Renderer *r) {
    int nv = (SPH_STACKS + 1) * (SPH_SLICES + 1);
    float *vtx = (float *)malloc((size_t)nv * 5u * sizeof(float));
    int vi = 0;
    for (int i = 0; i <= SPH_STACKS; ++i) {
        float v = (float)i / SPH_STACKS;
        float lat = (0.5f - v) * (float)M_PI;
        for (int j = 0; j <= SPH_SLICES; ++j) {
            float u = (float)j / SPH_SLICES;
            float lon = (u - 0.5f) * 2.0f * (float)M_PI;
            float x = cosf(lat)*cosf(lon), y = cosf(lat)*sinf(lon), z = sinf(lat);
            vtx[vi++] = x*EARTH_R_WORLD; vtx[vi++] = y*EARTH_R_WORLD; vtx[vi++] = z*EARTH_R_WORLD;
            vtx[vi++] = u; vtx[vi++] = v;
        }
    }
    int ni = SPH_STACKS * SPH_SLICES * 6;
    unsigned *idx = (unsigned *)malloc((size_t)ni * sizeof(unsigned));
    int ii = 0;
    for (int i = 0; i < SPH_STACKS; ++i)
        for (int j = 0; j < SPH_SLICES; ++j) {
            unsigned a = (unsigned)(i*(SPH_SLICES+1)+j), b = a+(unsigned)(SPH_SLICES+1);
            idx[ii++]=a; idx[ii++]=b; idx[ii++]=a+1;
            idx[ii++]=a+1; idx[ii++]=b; idx[ii++]=b+1;
        }
    r->globe_index_count = ni;
    glGenVertexArrays(1, &r->globe_vao); glBindVertexArray(r->globe_vao);
    glGenBuffers(1, &r->globe_vbo); glBindBuffer(GL_ARRAY_BUFFER, r->globe_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)nv*5u*sizeof(float)), vtx, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,5*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,5*sizeof(float),(void*)(3*sizeof(float)));
    glGenBuffers(1, &r->globe_ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->globe_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)ni*sizeof(unsigned)), idx, GL_STATIC_DRAW);
    free(vtx); free(idx);
}

static unsigned star_rng = 0x1234567u;
static float frand(void) { star_rng = star_rng*1664525u + 1013904223u; return (float)(star_rng>>8)/16777216.0f; }

static void build_stars(Renderer *r) {
    float *s = (float *)malloc((size_t)N_STARS*4u*sizeof(float));
    for (int i = 0; i < N_STARS; ++i) {
        float u = frand()*2.0f-1.0f, t = frand()*2.0f*(float)M_PI;
        float rr = sqrtf(1.0f-u*u);
        float x = rr*cosf(t), y = u, z = rr*sinf(t);
        float R = 170.0f;
        float mag = 0.25f + 0.75f*frand()*frand();
        s[i*4+0]=x*R; s[i*4+1]=y*R; s[i*4+2]=z*R; s[i*4+3]=mag;
    }
    r->star_count = N_STARS;
    glGenVertexArrays(1, &r->star_vao); glBindVertexArray(r->star_vao);
    glGenBuffers(1, &r->star_vbo); glBindBuffer(GL_ARRAY_BUFFER, r->star_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)N_STARS*4u*sizeof(float)), s, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,1,GL_FLOAT,GL_FALSE,4*sizeof(float),(void*)(3*sizeof(float)));
    free(s);
}

static void dyn_vertbuf(GLuint *vao, GLuint *vbo, size_t bytes) {
    glGenVertexArrays(1, vao); glBindVertexArray(*vao);
    glGenBuffers(1, vbo); glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)bytes, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,6*sizeof(float),(void*)(3*sizeof(float)));
}

static void load_earth_texture(Renderer *r) {
    Image im = {0};
    r->has_tex = 0;
    if (!image_load(&im, EARTH_TEX_PATH)) return;
    glGenTextures(1, &r->earth_tex);
    glBindTexture(GL_TEXTURE_2D, r->earth_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, im.w, im.h, 0, GL_RGB, GL_UNSIGNED_BYTE, im.rgb);
    glGenerateMipmap(GL_TEXTURE_2D);
    image_free(&im);
    r->has_tex = 1;
}

Renderer *render_create(int w, int h) {
    Renderer *r = (Renderer *)calloc(1, sizeof(*r));
    r->w = w; r->h = h;
    r->globe_prog = shader_build(GLOBE_VS, GLOBE_FS);
    r->atmo_prog  = shader_build(GLOBE_VS, ATMO_FS);
    r->star_prog  = shader_build(STAR_VS, STAR_FS);
    r->line_prog  = shader_build(LINE_VS, LINE_FS);
    r->pt_prog    = shader_build(PT_VS, PT_FS);
    r->trail_prog = shader_build(TRAIL_VS, TRAIL_FS);
    if (!r->globe_prog || !r->atmo_prog || !r->star_prog || !r->line_prog || !r->pt_prog || !r->trail_prog) { free(r); return NULL; }
    r->g_mvp=glGetUniformLocation(r->globe_prog,"uMVP"); r->g_scale=glGetUniformLocation(r->globe_prog,"uScale");
    r->g_model=glGetUniformLocation(r->globe_prog,"uModel");
    r->g_tex=glGetUniformLocation(r->globe_prog,"uTex"); r->g_hastex=glGetUniformLocation(r->globe_prog,"uHasTex");
    r->g_sun=glGetUniformLocation(r->globe_prog,"uSun"); r->g_cam=glGetUniformLocation(r->globe_prog,"uCam");
    r->a_mvp=glGetUniformLocation(r->atmo_prog,"uMVP"); r->a_scale=glGetUniformLocation(r->atmo_prog,"uScale");
    r->a_model=glGetUniformLocation(r->atmo_prog,"uModel");
    r->a_sun=glGetUniformLocation(r->atmo_prog,"uSun"); r->a_cam=glGetUniformLocation(r->atmo_prog,"uCam");
    r->s_vp=glGetUniformLocation(r->star_prog,"uVP");
    r->l_mvp=glGetUniformLocation(r->line_prog,"uMVP");
    r->l_alpha=glGetUniformLocation(r->line_prog,"uAlpha");
    r->p_mvp=glGetUniformLocation(r->pt_prog,"uMVP"); r->p_scale=glGetUniformLocation(r->pt_prog,"uScale");
    r->t_mvp=glGetUniformLocation(r->trail_prog,"uMVP");

    build_globe(r);
    build_stars(r);
    load_earth_texture(r);
    dyn_vertbuf(&r->line_vao, &r->line_vbo, (size_t)ASTRA_MAX_LINKS*2u*6u*sizeof(float));
    dyn_vertbuf(&r->pt_vao,   &r->pt_vbo,   (size_t)ASTRA_MAX_NODES*6u*sizeof(float));

    /* trail VAO: pos(3) + rgba(4) */
    size_t trail_verts = (size_t)ASTRA_MAX_SATS * (TRAIL_LEN-1) * 2u;
    glGenVertexArrays(1, &r->trail_vao); glBindVertexArray(r->trail_vao);
    glGenBuffers(1, &r->trail_vbo); glBindBuffer(GL_ARRAY_BUFFER, r->trail_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)(trail_verts*7u*sizeof(float)), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,7*sizeof(float),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,4,GL_FLOAT,GL_FALSE,7*sizeof(float),(void*)(3*sizeof(float)));

    r->node_pos = (fv3 *)malloc((size_t)ASTRA_MAX_NODES*sizeof(fv3));
    r->line_buf = (float *)malloc((size_t)ASTRA_MAX_LINKS*2u*6u*sizeof(float));
    r->pt_buf   = (float *)malloc((size_t)ASTRA_MAX_NODES*6u*sizeof(float));
    r->trail    = (fv3 *)calloc((size_t)ASTRA_MAX_SATS*TRAIL_LEN, sizeof(fv3));
    r->tfill    = (uint16_t *)calloc(ASTRA_MAX_SATS, sizeof(uint16_t));
    r->trail_buf= (float *)malloc(trail_verts*7u*sizeof(float));
    return r;
}

void render_destroy(Renderer *r) {
    if (!r) return;
    free(r->node_pos); free(r->line_buf); free(r->pt_buf);
    free(r->trail); free(r->tfill); free(r->trail_buf);
    free(r);
}
void render_resize(Renderer *r, int w, int h) { r->w = w; r->h = h; }

void render_view_proj(Camera cam, int w, int h, float out[16]) {
    float ce=cosf(cam.el), se=sinf(cam.el), ca=cosf(cam.az), sa=sinf(cam.az);
    fv3 eye = { cam.dist*ce*ca, cam.dist*se, cam.dist*ce*sa };
    mat4 proj = mat4_perspective(cam.fov, (float)w/(float)h, 0.4f, 600.0f);
    mat4 view = mat4_look_at(eye, fv3_make(0,0,0), fv3_make(0,1,0));
    mat4 mvp  = mat4_mul(proj, view);
    memcpy(out, mvp.m, 16*sizeof(float));
}
float render_world_scale(void) { return WORLD_SCALE; }

static void link_color(int up, float util, float *c) {
    if (!up) { c[0]=0.45f; c[1]=0.06f; c[2]=0.06f; return; }
    float u = util < 0 ? 0 : (util > 1 ? 1 : util);
    c[0] = 0.10f + 0.95f*u;
    c[1] = 0.65f - 0.30f*u;
    c[2] = 0.70f*(1.0f - u) + 0.20f;
}

/* ECI (z = pole) -> render world (y = up): (x, y, z) -> (x, z, -y). */
static inline fv3 eci_to_world(vec3 r) {
    return fv3_make((float)r.x*WORLD_SCALE, (float)r.z*WORLD_SCALE, (float)-r.y*WORLD_SCALE);
}

/* Earth model matrix: rotate ECEF by the true rotation angle (matches
 * astra_ecef_to_eci), then the same axis change as eci_to_world. */
static mat4 earth_model(double sim_time_s) {
    float th = (float)fmod(ASTRA_OMEGA_EARTH * sim_time_s, 2.0*M_PI);
    float c = cosf(th), s = sinf(th);
    mat4 M = mat4_identity();
    M.m[0]=c;  M.m[1]=0; M.m[2]=-s;     /* image of ECEF x */
    M.m[4]=-s; M.m[5]=0; M.m[6]=-c;     /* image of ECEF y */
    M.m[8]=0;  M.m[9]=1; M.m[10]=0;     /* image of ECEF z (pole -> up) */
    return M;
}

/* satellite point colour by orbital plane (matches the HUD legend) */
static const float PLANE_COL[6][3] = {
    {0.36f,0.95f,0.62f}, {0.36f,0.80f,1.00f}, {1.00f,0.72f,0.30f},
    {0.93f,0.47f,0.85f}, {0.55f,0.64f,1.00f}, {0.93f,0.90f,0.50f},
};
#define ROUTE_GOLD_R 1.00f
#define ROUTE_GOLD_G 0.78f
#define ROUTE_GOLD_B 0.25f

/* Append the current satellite positions to each trail (once per sim frame),
 * then draw the trails as fading polylines. Called after the globe so trails
 * are depth-occluded by the Earth. */
static void update_and_draw_trails(Renderer *r, const RenderSnapshot *snap, const float *mvp) {
    if (snap->frame_id != r->last_trail_frame) {
        r->last_trail_frame = snap->frame_id;
        int slot = r->thead;
        for (uint32_t i = 0; i < snap->sat_count; ++i) {
            if (snap->sat[i].alive) {
                r->trail[i*TRAIL_LEN + slot] = eci_to_world(snap->sat[i].r);
                if (r->tfill[i] < TRAIL_LEN) r->tfill[i]++;
            } else r->tfill[i] = 0;
        }
        r->thead = (r->thead + 1) % TRAIL_LEN;
    }

    int nv = 0;
    for (uint32_t i = 0; i < snap->sat_count; ++i) {
        if (!snap->sat[i].alive || r->tfill[i] < 2) continue;
        int fill = r->tfill[i];
        for (int k = 0; k < fill-1; ++k) {
            int s0 = (r->thead-1-k     + 2*TRAIL_LEN) % TRAIL_LEN;
            int s1 = (r->thead-1-(k+1) + 2*TRAIL_LEN) % TRAIL_LEN;
            fv3 a = r->trail[i*TRAIL_LEN+s0], b = r->trail[i*TRAIL_LEN+s1];
            float a0 = (1.0f - (float)k/(float)(fill-1)) * 0.60f;
            float a1 = (1.0f - (float)(k+1)/(float)(fill-1)) * 0.60f;
            float *p = &r->trail_buf[nv*7];
            p[0]=a.x;p[1]=a.y;p[2]=a.z; p[3]=0.35f;p[4]=0.72f;p[5]=1.0f;p[6]=a0;
            p[7]=b.x;p[8]=b.y;p[9]=b.z; p[10]=0.35f;p[11]=0.72f;p[12]=1.0f;p[13]=a1;
            nv += 2;
        }
    }
    if (nv == 0) return;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(r->trail_vao);
    glBindBuffer(GL_ARRAY_BUFFER, r->trail_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)nv*7u*sizeof(float)), r->trail_buf);
    glUseProgram(r->trail_prog);
    glUniformMatrix4fv(r->t_mvp, 1, GL_FALSE, mvp);
    glDrawArrays(GL_LINES, 0, nv);
}

void render_frame(Renderer *r, const RenderSnapshot *snap, Camera cam, int selected_sat) {
    float ce=cosf(cam.el), se=sinf(cam.el), ca=cosf(cam.az), sa=sinf(cam.az);
    fv3 eye = { cam.dist*ce*ca, cam.dist*se, cam.dist*ce*sa };
    mat4 proj = mat4_perspective(cam.fov, (float)r->w/(float)r->h, 0.4f, 600.0f);
    mat4 view = mat4_look_at(eye, fv3_make(0,0,0), fv3_make(0,1,0));
    mat4 mvp  = mat4_mul(proj, view);
    mat4 model = earth_model(snap->sim_time_s);
    mat4 mvp_globe = mat4_mul(mvp, model);
    /* stars at infinity: view with translation removed */
    mat4 view_notrans = view; view_notrans.m[12]=view_notrans.m[13]=view_notrans.m[14]=0.0f;
    mat4 star_vp = mat4_mul(proj, view_notrans);

    /* sun fixed in inertial space (~June declination); the terminator sweeps
     * the surface because the Earth itself rotates at the true rate */
    fv3 sun = fv3_norm(fv3_make(0.917f, 0.398f, 0.0f));

    glViewport(0, 0, r->w, r->h);
    glEnable(GL_MULTISAMPLE);   /* anti-alias 3D edges (no-op on single-sample FBs) */
    glClearColor(0.005f, 0.006f, 0.013f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_PROGRAM_POINT_SIZE);

    /* ---- starfield (no depth) ---- */
    glDisable(GL_DEPTH_TEST); glDepthMask(GL_FALSE);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(r->star_prog);
    glUniformMatrix4fv(r->s_vp, 1, GL_FALSE, star_vp.m);
    glBindVertexArray(r->star_vao);
    glDrawArrays(GL_POINTS, 0, r->star_count);

    /* ---- globe ---- */
    glEnable(GL_DEPTH_TEST); glDepthMask(GL_TRUE); glDisable(GL_BLEND);
    glUseProgram(r->globe_prog);
    glUniformMatrix4fv(r->g_mvp, 1, GL_FALSE, mvp_globe.m);
    glUniformMatrix4fv(r->g_model, 1, GL_FALSE, model.m);
    glUniform1f(r->g_scale, 1.0f);
    glUniform1i(r->g_hastex, r->has_tex);
    glUniform3f(r->g_sun, sun.x, sun.y, sun.z);
    glUniform3f(r->g_cam, eye.x, eye.y, eye.z);
    if (r->has_tex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, r->earth_tex); glUniform1i(r->g_tex, 0); }
    glBindVertexArray(r->globe_vao);
    glDrawElements(GL_TRIANGLES, r->globe_index_count, GL_UNSIGNED_INT, 0);

    /* ---- atmosphere shell (additive, depth-tested, no write) ---- */
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); glDepthMask(GL_FALSE);
    glUseProgram(r->atmo_prog);
    glUniformMatrix4fv(r->a_mvp, 1, GL_FALSE, mvp_globe.m);
    glUniformMatrix4fv(r->a_model, 1, GL_FALSE, model.m);
    glUniform1f(r->a_scale, 1.035f);
    glUniform3f(r->a_sun, sun.x, sun.y, sun.z);
    glUniform3f(r->a_cam, eye.x, eye.y, eye.z);
    glBindVertexArray(r->globe_vao);
    glDrawElements(GL_TRIANGLES, r->globe_index_count, GL_UNSIGNED_INT, 0);

    /* node positions by id */
    for (uint32_t i = 0; i < snap->sat_count; ++i)
        r->node_pos[i] = eci_to_world(snap->sat[i].r);
    for (uint32_t i = 0; i < snap->gs_count; ++i) {
        node_id gid = snap->gs[i].gid;
        if (gid < ASTRA_MAX_NODES)
            r->node_pos[gid] = eci_to_world(snap->gs[i].r);
    }

    /* ---- orbit trails (depth-tested, no write) ---- */
    update_and_draw_trails(r, snap, mvp.m);

    /* ---- links (additive glow, occluded by globe) ---- */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    int lv = 0;
    for (uint32_t e = 0; e < snap->link_count; ++e) {
        const SnapLink *L = &snap->link[e];
        if (L->u >= ASTRA_MAX_NODES || L->v >= ASTRA_MAX_NODES) continue;
        float c[3]; link_color(L->up, L->util, c);
        fv3 a=r->node_pos[L->u], b=r->node_pos[L->v];
        float *p = &r->line_buf[lv*6];
        p[0]=a.x;p[1]=a.y;p[2]=a.z;p[3]=c[0];p[4]=c[1];p[5]=c[2];
        p[6]=b.x;p[7]=b.y;p[8]=b.z;p[9]=c[0];p[10]=c[1];p[11]=c[2];
        lv += 2;
    }
    if (lv > 0) {
        glBindVertexArray(r->line_vao); glBindBuffer(GL_ARRAY_BUFFER, r->line_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)lv*6u*sizeof(float)), r->line_buf);
        glUseProgram(r->line_prog);
        glUniformMatrix4fv(r->l_mvp, 1, GL_FALSE, mvp.m);
        glUniform1f(r->l_alpha, 0.30f);
        glDrawArrays(GL_LINES, 0, lv);
    }

    /* ---- active route: glowing gold arc over the constellation ---- */
    const SnapRoute *rt = &snap->route;
    uint8_t on_route[ASTRA_MAX_NODES];
    memset(on_route, 0, sizeof(on_route));
    if (rt->valid) {
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        int nv = 0;
        for (uint32_t k = 0; k + 1 < rt->count; ++k) {
            node_id u0 = rt->node[k], v0 = rt->node[k+1];
            if (u0 >= ASTRA_MAX_NODES || v0 >= ASTRA_MAX_NODES) break;
            on_route[u0] = on_route[v0] = 1;
            fv3 a = r->node_pos[u0], b = r->node_pos[v0];
            float *p = &r->line_buf[nv*6];
            p[0]=a.x;p[1]=a.y;p[2]=a.z;p[3]=ROUTE_GOLD_R;p[4]=ROUTE_GOLD_G;p[5]=ROUTE_GOLD_B;
            p[6]=b.x;p[7]=b.y;p[8]=b.z;p[9]=ROUTE_GOLD_R;p[10]=ROUTE_GOLD_G;p[11]=ROUTE_GOLD_B;
            nv += 2;
        }
        if (nv > 0) {
            glBindVertexArray(r->line_vao); glBindBuffer(GL_ARRAY_BUFFER, r->line_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)nv*6u*sizeof(float)), r->line_buf);
            glUseProgram(r->line_prog);
            glUniformMatrix4fv(r->l_mvp, 1, GL_FALSE, mvp.m);
            glUniform1f(r->l_alpha, 0.95f);
            glDrawArrays(GL_LINES, 0, nv);   /* additive: drawn twice = glow */
            glDrawArrays(GL_LINES, 0, nv);
        }
    }

    /* ---- satellites (coloured by plane; gold on the active route) ---- */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    int pv = 0;
    for (uint32_t i = 0; i < snap->sat_count; ++i) {
        fv3 a = r->node_pos[i]; float *p = &r->pt_buf[pv*6];
        p[0]=a.x;p[1]=a.y;p[2]=a.z;
        if (!snap->sat[i].alive)      { p[3]=0.55f; p[4]=0.12f; p[5]=0.10f; }
        else if (on_route[i])         { p[3]=ROUTE_GOLD_R; p[4]=ROUTE_GOLD_G; p[5]=ROUTE_GOLD_B; }
        else {
            const float *c = PLANE_COL[snap->sat[i].plane % 6u];
            p[3]=c[0]; p[4]=c[1]; p[5]=c[2];
        }
        pv++;
    }
    if (pv > 0) {
        glBindVertexArray(r->pt_vao); glBindBuffer(GL_ARRAY_BUFFER, r->pt_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)pv*6u*sizeof(float)), r->pt_buf);
        glUseProgram(r->pt_prog);
        glUniformMatrix4fv(r->p_mvp, 1, GL_FALSE, mvp.m);
        glUniform1f(r->p_scale, 150.0f);
        glDrawArrays(GL_POINTS, 0, pv);
    }

    /* ---- ground stations (pulsing amber beacons, distinct from sats) ---- */
    pv = 0;
    for (uint32_t i = 0; i < snap->gs_count; ++i) {
        node_id gid = snap->gs[i].gid; if (gid >= ASTRA_MAX_NODES) continue;
        fv3 a=r->node_pos[gid]; float *p=&r->pt_buf[pv*6];
        p[0]=a.x;p[1]=a.y;p[2]=a.z;p[3]=1.0f;p[4]=0.62f;p[5]=0.14f; pv++;
    }
    if (pv > 0) {
        float beat = 0.5f + 0.5f*sinf((float)snap->sim_time_s * 2.2f);
        glBindBuffer(GL_ARRAY_BUFFER, r->pt_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)pv*6u*sizeof(float)), r->pt_buf);
        glUseProgram(r->pt_prog);
        /* expanding additive halo = the "beacon" pulse */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glUniform1f(r->p_scale, 360.0f + 520.0f*beat);
        glDrawArrays(GL_POINTS, 0, pv);
        /* steady solid core on top */
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUniform1f(r->p_scale, 330.0f);
        glDrawArrays(GL_POINTS, 0, pv);
    }

    /* ---- route packet (animated dot riding the arc) + hop markers ---- */
    if (rt->valid && rt->count >= 2) {
        float tt = (float)fmod(snap->sim_time_s * 0.05, 1.0) * (float)(rt->count - 1);
        uint32_t seg = (uint32_t)tt; if (seg >= rt->count-1) seg = rt->count-2;
        float fr = tt - (float)seg;
        fv3 a = r->node_pos[rt->node[seg]], b = r->node_pos[rt->node[seg+1]];
        float *p = r->pt_buf;
        p[0]=a.x+(b.x-a.x)*fr; p[1]=a.y+(b.y-a.y)*fr; p[2]=a.z+(b.z-a.z)*fr;
        p[3]=1.0f; p[4]=0.92f; p[5]=0.65f;
        glBindBuffer(GL_ARRAY_BUFFER, r->pt_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)(6u*sizeof(float)), r->pt_buf);
        glUseProgram(r->pt_prog);
        glUniform1f(r->p_scale, 260.0f);
        glDrawArrays(GL_POINTS, 0, 1);
    }

    /* ---- selected satellite: bright marker on top ---- */
    if (selected_sat >= 0 && selected_sat < (int)snap->sat_count) {
        fv3 a = r->node_pos[selected_sat];
        float *p = r->pt_buf;
        p[0]=a.x; p[1]=a.y; p[2]=a.z; p[3]=1.0f; p[4]=1.0f; p[5]=1.0f;
        glBindBuffer(GL_ARRAY_BUFFER, r->pt_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)(6u*sizeof(float)), r->pt_buf);
        glUseProgram(r->pt_prog);
        glUniform1f(r->p_scale, 240.0f);
        glDrawArrays(GL_POINTS, 0, 1);
    }

    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
}
