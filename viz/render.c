/* render.c — ASTRA scene renderer (globe + satellites + links). */
#include "render.h"
#include "glctx.h"
#include "glfn.h"
#include "shader.h"
#include "mat4.h"
#include <GL/gl.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define WORLD_SCALE   0.001f                  /* km -> world units            */
#define EARTH_R_WORLD (6378.137f * WORLD_SCALE)
#define SPH_STACKS    64
#define SPH_SLICES    128

/* ---- shaders ------------------------------------------------------------- */
static const char *GLOBE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos;\n"
    "uniform mat4 uMVP;\n"
    "out vec3 vn;\n"
    "void main(){ vn = normalize(pos); gl_Position = uMVP*vec4(pos,1.0); }\n";
static const char *GLOBE_FS =
    "#version 330 core\n"
    "in vec3 vn; out vec4 frag;\n"
    "uniform vec3 uLight;\n"
    "void main(){\n"
    "  vec3 n = normalize(vn);\n"
    "  float d = max(dot(n, normalize(uLight)), 0.0);\n"
    "  float lat = asin(clamp(n.y,-1.0,1.0));\n"
    "  float lon = atan(n.z, n.x);\n"
    "  float gla = abs(fract(lat*(12.0/3.14159265)+0.5)-0.5);\n"
    "  float glo = abs(fract(lon*(12.0/3.14159265)+0.5)-0.5);\n"
    "  float grid = 1.0 - smoothstep(0.0,0.025, min(gla,glo));\n"
    "  vec3 base = vec3(0.05,0.11,0.26);\n"
    "  vec3 col = base*(0.16 + 1.0*d);\n"
    "  col = mix(col, vec3(0.22,0.45,0.65), grid*0.55);\n"
    "  float rim = pow(1.0 - max(n.z*0.0+ d,0.0), 1.0);\n"
    "  col += vec3(0.02,0.05,0.10)*rim;\n"
    "  frag = vec4(col, 1.0);\n"
    "}\n";

static const char *LINE_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos;\n"
    "layout(location=1) in vec3 col;\n"
    "uniform mat4 uMVP; out vec3 vcol;\n"
    "void main(){ vcol = col; gl_Position = uMVP*vec4(pos,1.0); }\n";
static const char *LINE_FS =
    "#version 330 core\n"
    "in vec3 vcol; out vec4 frag;\n"
    "void main(){ frag = vec4(vcol, 0.85); }\n";

static const char *PT_VS =
    "#version 330 core\n"
    "layout(location=0) in vec3 pos;\n"
    "layout(location=1) in vec3 col;\n"
    "uniform mat4 uMVP; uniform float uScale; out vec3 vcol;\n"
    "void main(){ vcol = col; vec4 p = uMVP*vec4(pos,1.0);\n"
    "  gl_Position = p; gl_PointSize = clamp(uScale/p.w, 3.0, 64.0); }\n";
static const char *PT_FS =
    "#version 330 core\n"
    "in vec3 vcol; out vec4 frag;\n"
    "void main(){ vec2 d = gl_PointCoord-vec2(0.5);\n"
    "  float r2 = dot(d,d); if (r2 > 0.25) discard;\n"
    "  float a = smoothstep(0.25,0.02,r2);\n"
    "  vec3 c = mix(vec3(1.0), vcol, smoothstep(0.0,0.16,r2));\n"
    "  frag = vec4(c, a); }\n";

/* ---- renderer state ------------------------------------------------------ */
struct Renderer {
    int w, h;
    GLuint globe_prog, line_prog, pt_prog;
    GLint  g_mvp, g_light, l_mvp, p_mvp, p_scale;

    GLuint globe_vao, globe_vbo, globe_ebo; int globe_index_count;
    GLuint line_vao, line_vbo;
    GLuint pt_vao, pt_vbo;

    fv3   *node_pos;     /* world position by node id (ASTRA_MAX_NODES) */
    float *line_buf;     /* 2 verts/link * 6 floats                     */
    float *pt_buf;       /* 1 vert/node  * 6 floats                     */
};

/* ---- sphere mesh --------------------------------------------------------- */
static void build_globe(Renderer *r) {
    int nv = (SPH_STACKS + 1) * (SPH_SLICES + 1);
    float *pos = (float *)malloc((size_t)nv * 3u * sizeof(float));
    int vi = 0;
    for (int i = 0; i <= SPH_STACKS; ++i) {
        float v = (float)i / SPH_STACKS;
        float phi = v * (float)M_PI;            /* 0..pi */
        for (int j = 0; j <= SPH_SLICES; ++j) {
            float u = (float)j / SPH_SLICES;
            float th = u * 2.0f * (float)M_PI;  /* 0..2pi */
            float x = sinf(phi) * cosf(th);
            float y = cosf(phi);
            float z = sinf(phi) * sinf(th);
            pos[vi++] = x * EARTH_R_WORLD;
            pos[vi++] = y * EARTH_R_WORLD;
            pos[vi++] = z * EARTH_R_WORLD;
        }
    }
    int ni = SPH_STACKS * SPH_SLICES * 6;
    unsigned *idx = (unsigned *)malloc((size_t)ni * sizeof(unsigned));
    int ii = 0;
    for (int i = 0; i < SPH_STACKS; ++i)
        for (int j = 0; j < SPH_SLICES; ++j) {
            unsigned a = (unsigned)(i * (SPH_SLICES + 1) + j);
            unsigned b = a + (unsigned)(SPH_SLICES + 1);
            idx[ii++] = a; idx[ii++] = b; idx[ii++] = a + 1;
            idx[ii++] = a + 1; idx[ii++] = b; idx[ii++] = b + 1;
        }
    r->globe_index_count = ni;

    glGenVertexArrays(1, &r->globe_vao); glBindVertexArray(r->globe_vao);
    glGenBuffers(1, &r->globe_vbo); glBindBuffer(GL_ARRAY_BUFFER, r->globe_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)nv*3u*sizeof(float)), pos, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3*sizeof(float), (void*)0);
    glGenBuffers(1, &r->globe_ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, r->globe_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr_t)((size_t)ni*sizeof(unsigned)), idx, GL_STATIC_DRAW);
    free(pos); free(idx);
}

static GLuint dyn_vertbuf(GLuint *vao, GLuint *vbo, size_t bytes) {
    glGenVertexArrays(1, vao); glBindVertexArray(*vao);
    glGenBuffers(1, vbo); glBindBuffer(GL_ARRAY_BUFFER, *vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr_t)bytes, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6*sizeof(float), (void*)(3*sizeof(float)));
    return *vbo;
}

Renderer *render_create(int w, int h) {
    Renderer *r = (Renderer *)calloc(1, sizeof(*r));
    r->w = w; r->h = h;
    r->globe_prog = shader_build(GLOBE_VS, GLOBE_FS);
    r->line_prog  = shader_build(LINE_VS, LINE_FS);
    r->pt_prog    = shader_build(PT_VS, PT_FS);
    if (!r->globe_prog || !r->line_prog || !r->pt_prog) { free(r); return NULL; }
    r->g_mvp   = glGetUniformLocation(r->globe_prog, "uMVP");
    r->g_light = glGetUniformLocation(r->globe_prog, "uLight");
    r->l_mvp   = glGetUniformLocation(r->line_prog, "uMVP");
    r->p_mvp   = glGetUniformLocation(r->pt_prog, "uMVP");
    r->p_scale = glGetUniformLocation(r->pt_prog, "uScale");

    build_globe(r);
    dyn_vertbuf(&r->line_vao, &r->line_vbo, (size_t)ASTRA_MAX_LINKS*2u*6u*sizeof(float));
    dyn_vertbuf(&r->pt_vao,   &r->pt_vbo,   (size_t)ASTRA_MAX_NODES*6u*sizeof(float));

    r->node_pos = (fv3 *)malloc((size_t)ASTRA_MAX_NODES * sizeof(fv3));
    r->line_buf = (float *)malloc((size_t)ASTRA_MAX_LINKS*2u*6u*sizeof(float));
    r->pt_buf   = (float *)malloc((size_t)ASTRA_MAX_NODES*6u*sizeof(float));
    return r;
}

void render_destroy(Renderer *r) {
    if (!r) return;
    free(r->node_pos); free(r->line_buf); free(r->pt_buf);
    free(r);
}

/* link colour ramp by utilisation (or dim red if down) */
static void link_color(int up, float util, float *c) {
    if (!up) { c[0]=0.35f; c[1]=0.05f; c[2]=0.05f; return; }
    float u = util < 0 ? 0 : (util > 1 ? 1 : util);
    c[0] = 0.15f + 0.80f*u;            /* red rises with load   */
    c[1] = 0.55f - 0.35f*u;            /* green fades           */
    c[2] = 0.55f*(1.0f - u) + 0.10f;   /* cool when idle        */
}

void render_frame(Renderer *r, const RenderSnapshot *snap, Camera cam) {
    /* camera */
    float ce = cosf(cam.el), se = sinf(cam.el), ca = cosf(cam.az), sa = sinf(cam.az);
    fv3 eye = { cam.dist*ce*ca, cam.dist*se, cam.dist*ce*sa };
    mat4 proj = mat4_perspective(cam.fov, (float)r->w/(float)r->h, 0.4f, 400.0f);
    mat4 view = mat4_look_at(eye, fv3_make(0,0,0), fv3_make(0,1,0));
    mat4 mvp  = mat4_mul(proj, view);

    glViewport(0, 0, r->w, r->h);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.01f, 0.01f, 0.03f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    /* node positions by id (sats then ground) */
    for (uint32_t i = 0; i < snap->sat_count; ++i)
        r->node_pos[i] = fv3_make((float)snap->sat[i].r.x*WORLD_SCALE,
                                  (float)snap->sat[i].r.y*WORLD_SCALE,
                                  (float)snap->sat[i].r.z*WORLD_SCALE);
    for (uint32_t i = 0; i < snap->gs_count; ++i) {
        node_id gid = snap->gs[i].gid;
        if (gid < ASTRA_MAX_NODES)
            r->node_pos[gid] = fv3_make((float)snap->gs[i].r.x*WORLD_SCALE,
                                        (float)snap->gs[i].r.y*WORLD_SCALE,
                                        (float)snap->gs[i].r.z*WORLD_SCALE);
    }

    /* ---- globe ---- */
    glUseProgram(r->globe_prog);
    glUniformMatrix4fv(r->g_mvp, 1, GL_FALSE, mvp.m);
    glUniform3f(r->g_light, 0.5f, 0.7f, 0.55f);
    glBindVertexArray(r->globe_vao);
    glDrawElements(GL_TRIANGLES, r->globe_index_count, GL_UNSIGNED_INT, 0);

    /* ---- links ---- */
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    int lv = 0;
    for (uint32_t e = 0; e < snap->link_count; ++e) {
        const SnapLink *L = &snap->link[e];
        if (L->u >= ASTRA_MAX_NODES || L->v >= ASTRA_MAX_NODES) continue;
        float c[3]; link_color(L->up, L->util, c);
        fv3 a = r->node_pos[L->u], b = r->node_pos[L->v];
        float *p = &r->line_buf[lv*6];
        p[0]=a.x; p[1]=a.y; p[2]=a.z; p[3]=c[0]; p[4]=c[1]; p[5]=c[2];
        p[6]=b.x; p[7]=b.y; p[8]=b.z; p[9]=c[0]; p[10]=c[1]; p[11]=c[2];
        lv += 2;
    }
    if (lv > 0) {
        glBindVertexArray(r->line_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->line_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)lv*6u*sizeof(float)), r->line_buf);
        glUseProgram(r->line_prog);
        glUniformMatrix4fv(r->l_mvp, 1, GL_FALSE, mvp.m);
        glDrawArrays(GL_LINES, 0, lv);
    }

    /* ---- satellites (round points) ---- */
    glEnable(GL_PROGRAM_POINT_SIZE);
    int pv = 0;
    for (uint32_t i = 0; i < snap->sat_count; ++i) {
        if (!snap->sat[i].alive) continue;
        fv3 a = r->node_pos[i];
        float *p = &r->pt_buf[pv*6];
        p[0]=a.x; p[1]=a.y; p[2]=a.z; p[3]=0.75f; p[4]=0.92f; p[5]=1.0f;
        pv++;
    }
    if (pv > 0) {
        glBindVertexArray(r->pt_vao);
        glBindBuffer(GL_ARRAY_BUFFER, r->pt_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)pv*6u*sizeof(float)), r->pt_buf);
        glUseProgram(r->pt_prog);
        glUniformMatrix4fv(r->p_mvp, 1, GL_FALSE, mvp.m);
        glUniform1f(r->p_scale, 360.0f);
        glDrawArrays(GL_POINTS, 0, pv);
    }

    /* ---- ground stations (larger orange points) ---- */
    pv = 0;
    for (uint32_t i = 0; i < snap->gs_count; ++i) {
        node_id gid = snap->gs[i].gid;
        if (gid >= ASTRA_MAX_NODES) continue;
        fv3 a = r->node_pos[gid];
        float *p = &r->pt_buf[pv*6];
        p[0]=a.x; p[1]=a.y; p[2]=a.z; p[3]=1.0f; p[4]=0.6f; p[5]=0.12f;
        pv++;
    }
    if (pv > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, r->pt_vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr_t)((size_t)pv*6u*sizeof(float)), r->pt_buf);
        glUseProgram(r->pt_prog);
        glUniform1f(r->p_scale, 620.0f);
        glDrawArrays(GL_POINTS, 0, pv);
    }

    glDisable(GL_BLEND);
}
