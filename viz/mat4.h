/* mat4.h — minimal column-major 4x4 float matrices + float vec3 for rendering.
 * Column-major to match GL's uniform layout (transpose=GL_FALSE). */
#ifndef ASTRA_MAT4_H
#define ASTRA_MAT4_H

#include <math.h>

typedef struct { float x, y, z; } fv3;
typedef struct { float m[16]; } mat4;   /* column-major: m[col*4+row] */

static inline fv3 fv3_make(float x, float y, float z) { return (fv3){x,y,z}; }
static inline fv3 fv3_sub(fv3 a, fv3 b) { return (fv3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline float fv3_dot(fv3 a, fv3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline fv3 fv3_cross(fv3 a, fv3 b) {
    return (fv3){ a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x };
}
static inline fv3 fv3_norm(fv3 a) {
    float n = sqrtf(fv3_dot(a,a)); if (n < 1e-20f) n = 1e-20f;
    return (fv3){a.x/n, a.y/n, a.z/n};
}

static inline mat4 mat4_identity(void) {
    mat4 r = {{0}};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

/* a*b (applies b first, then a) */
static inline mat4 mat4_mul(mat4 a, mat4 b) {
    mat4 r = {{0}};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a.m[k*4+row] * b.m[c*4+k];
            r.m[c*4+row] = s;
        }
    return r;
}

static inline mat4 mat4_perspective(float fovy_rad, float aspect, float zn, float zf) {
    float f = 1.0f / tanf(fovy_rad * 0.5f);
    mat4 r = {{0}};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zf * zn) / (zn - zf);
    return r;
}

static inline mat4 mat4_look_at(fv3 eye, fv3 center, fv3 up) {
    fv3 f = fv3_norm(fv3_sub(center, eye));
    fv3 s = fv3_norm(fv3_cross(f, up));
    fv3 u = fv3_cross(s, f);
    mat4 r = mat4_identity();
    r.m[0]=s.x; r.m[4]=s.y; r.m[8]=s.z;
    r.m[1]=u.x; r.m[5]=u.y; r.m[9]=u.z;
    r.m[2]=-f.x; r.m[6]=-f.y; r.m[10]=-f.z;
    r.m[12] = -fv3_dot(s, eye);
    r.m[13] = -fv3_dot(u, eye);
    r.m[14] =  fv3_dot(f, eye);
    return r;
}

#endif /* ASTRA_MAT4_H */
