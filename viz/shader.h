/* shader.h — compile/link GLSL programs with error reporting. */
#ifndef ASTRA_SHADER_H
#define ASTRA_SHADER_H

#include <GL/gl.h>

/* Compile vertex+fragment sources and link. Returns program id, or 0 on
 * failure (logs the compiler/linker error to stderr). */
GLuint shader_build(const char *vs_src, const char *fs_src);

#endif /* ASTRA_SHADER_H */
