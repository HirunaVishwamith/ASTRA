/* shader.c — GLSL compile/link helpers. */
#include "shader.h"
#include "glfn.h"
#include <stdio.h>

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetShaderInfoLog(s, (GLsizei)sizeof(log), &n, log);
        fprintf(stderr, "shader compile error (%s):\n%s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint shader_build(const char *vs_src, const char *fs_src) {
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetProgramInfoLog(prog, (GLsizei)sizeof(log), &n, log);
        fprintf(stderr, "program link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}
