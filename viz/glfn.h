/* glfn.h — runtime loader for modern (GL 2.0+) entry points, since GLEW is not
 * installed. GL 1.1 functions (glClear, glViewport, glDrawArrays, glGenTextures,
 * glTexImage2D, ...) resolve directly through -lGL and are used as-is. Only the
 * shader/VAO/VBO/instancing/texture-unit calls are loaded here via the context's
 * getproc (eglGetProcAddress / glXGetProcAddressARB). Pointers are prefixed pgl*
 * and aliased with macros to avoid clashing with any libGL-exported symbols. */
#ifndef ASTRA_GLFN_H
#define ASTRA_GLFN_H

#include <GL/gl.h>
#include <stddef.h>

#ifndef GLchar
typedef char GLchar;
#endif
#ifndef GLsizeiptr_defined
typedef ptrdiff_t GLsizeiptr_t;
typedef ptrdiff_t GLintptr_t;
#endif

/* loader: returns 1 if every required pointer resolved. */
int glfn_load(void *(*getproc)(const char *name));

/* ---- shaders / programs -------------------------------------------------- */
typedef GLuint (*PFN_glCreateShader)(GLenum);
typedef void   (*PFN_glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
typedef void   (*PFN_glCompileShader)(GLuint);
typedef void   (*PFN_glGetShaderiv)(GLuint, GLenum, GLint *);
typedef void   (*PFN_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (*PFN_glDeleteShader)(GLuint);
typedef GLuint (*PFN_glCreateProgram)(void);
typedef void   (*PFN_glAttachShader)(GLuint, GLuint);
typedef void   (*PFN_glLinkProgram)(GLuint);
typedef void   (*PFN_glGetProgramiv)(GLuint, GLenum, GLint *);
typedef void   (*PFN_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
typedef void   (*PFN_glUseProgram)(GLuint);
typedef void   (*PFN_glDeleteProgram)(GLuint);
typedef GLint  (*PFN_glGetUniformLocation)(GLuint, const GLchar *);
typedef void   (*PFN_glUniform1f)(GLint, GLfloat);
typedef void   (*PFN_glUniform1i)(GLint, GLint);
typedef void   (*PFN_glUniform2f)(GLint, GLfloat, GLfloat);
typedef void   (*PFN_glUniform3f)(GLint, GLfloat, GLfloat, GLfloat);
typedef void   (*PFN_glUniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat);
typedef void   (*PFN_glUniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat *);

/* ---- buffers / vertex arrays -------------------------------------------- */
typedef void (*PFN_glGenVertexArrays)(GLsizei, GLuint *);
typedef void (*PFN_glBindVertexArray)(GLuint);
typedef void (*PFN_glDeleteVertexArrays)(GLsizei, const GLuint *);
typedef void (*PFN_glGenBuffers)(GLsizei, GLuint *);
typedef void (*PFN_glBindBuffer)(GLenum, GLuint);
typedef void (*PFN_glBufferData)(GLenum, GLsizeiptr_t, const void *, GLenum);
typedef void (*PFN_glBufferSubData)(GLenum, GLintptr_t, GLsizeiptr_t, const void *);
typedef void (*PFN_glDeleteBuffers)(GLsizei, const GLuint *);
typedef void (*PFN_glEnableVertexAttribArray)(GLuint);
typedef void (*PFN_glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
typedef void (*PFN_glVertexAttribDivisor)(GLuint, GLuint);
typedef void (*PFN_glDrawArraysInstanced)(GLenum, GLint, GLsizei, GLsizei);
typedef void (*PFN_glDrawElementsInstanced)(GLenum, GLsizei, GLenum, const void *, GLsizei);

/* ---- texture unit / mipmaps --------------------------------------------- */
typedef void (*PFN_glActiveTexture)(GLenum);
typedef void (*PFN_glGenerateMipmap)(GLenum);

/* declared pointers (defined in glfn.c) */
#define GLFN(type, name) extern type pgl_##name;
#include "glfn_list.h"
#undef GLFN

/* alias modern calls to the loaded pointers */
#define glCreateShader            pgl_glCreateShader
#define glShaderSource            pgl_glShaderSource
#define glCompileShader           pgl_glCompileShader
#define glGetShaderiv             pgl_glGetShaderiv
#define glGetShaderInfoLog        pgl_glGetShaderInfoLog
#define glDeleteShader            pgl_glDeleteShader
#define glCreateProgram           pgl_glCreateProgram
#define glAttachShader            pgl_glAttachShader
#define glLinkProgram             pgl_glLinkProgram
#define glGetProgramiv            pgl_glGetProgramiv
#define glGetProgramInfoLog       pgl_glGetProgramInfoLog
#define glUseProgram              pgl_glUseProgram
#define glDeleteProgram           pgl_glDeleteProgram
#define glGetUniformLocation      pgl_glGetUniformLocation
#define glUniform1f               pgl_glUniform1f
#define glUniform1i               pgl_glUniform1i
#define glUniform2f               pgl_glUniform2f
#define glUniform3f               pgl_glUniform3f
#define glUniform4f               pgl_glUniform4f
#define glUniformMatrix4fv        pgl_glUniformMatrix4fv
#define glGenVertexArrays         pgl_glGenVertexArrays
#define glBindVertexArray         pgl_glBindVertexArray
#define glDeleteVertexArrays      pgl_glDeleteVertexArrays
#define glGenBuffers              pgl_glGenBuffers
#define glBindBuffer              pgl_glBindBuffer
#define glBufferData              pgl_glBufferData
#define glBufferSubData           pgl_glBufferSubData
#define glDeleteBuffers           pgl_glDeleteBuffers
#define glEnableVertexAttribArray pgl_glEnableVertexAttribArray
#define glVertexAttribPointer     pgl_glVertexAttribPointer
#define glVertexAttribDivisor     pgl_glVertexAttribDivisor
#define glDrawArraysInstanced     pgl_glDrawArraysInstanced
#define glDrawElementsInstanced   pgl_glDrawElementsInstanced
#define glActiveTexture           pgl_glActiveTexture
#define glGenerateMipmap          pgl_glGenerateMipmap

#endif /* ASTRA_GLFN_H */
