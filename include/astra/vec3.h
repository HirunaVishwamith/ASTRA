/* vec3.h — double-precision 3-vectors. Double precision is required so the C
 * port matches NumPy float64 results to ~1e-9 during verification. */
#ifndef ASTRA_VEC3_H
#define ASTRA_VEC3_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { double x, y, z; } vec3;

static inline vec3   v3(double x, double y, double z) { return (vec3){x, y, z}; }
static inline vec3   v3_add(vec3 a, vec3 b)   { return (vec3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline vec3   v3_sub(vec3 a, vec3 b)   { return (vec3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline vec3   v3_scale(vec3 a, double s){ return (vec3){a.x*s, a.y*s, a.z*s}; }
static inline double v3_dot(vec3 a, vec3 b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline vec3   v3_cross(vec3 a, vec3 b) {
    return (vec3){ a.y*b.z - a.z*b.y,
                   a.z*b.x - a.x*b.z,
                   a.x*b.y - a.y*b.x };
}
static inline double v3_norm2(vec3 a) { return v3_dot(a, a); }
static inline double v3_norm(vec3 a)  { return sqrt(v3_norm2(a)); }

#endif /* ASTRA_VEC3_H */
