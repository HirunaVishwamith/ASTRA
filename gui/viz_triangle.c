/* viz_triangle.c — milestone 2: GL loader + shader + VAO/VBO path.
 * Loads modern GL, compiles a trivial program, draws one coloured triangle to
 * an offscreen EGL target, reads it back, writes a PNG, and verifies that the
 * triangle actually rendered (centre pixel is not the clear colour). */
#include "glctx.h"
#include "glfn.h"
#include "shader.h"
#include "image.h"
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>

static const char *VS =
    "#version 330 core\n"
    "layout(location=0) in vec2 pos;\n"
    "layout(location=1) in vec3 col;\n"
    "out vec3 vcol;\n"
    "void main(){ vcol = col; gl_Position = vec4(pos, 0.0, 1.0); }\n";

static const char *FS =
    "#version 330 core\n"
    "in vec3 vcol; out vec4 frag;\n"
    "void main(){ frag = vec4(vcol, 1.0); }\n";

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "viz_triangle.png";
    const int W = 320, H = 320;

    GLCtx *c = glctx_egl_create(W, H);
    if (!c) { fprintf(stderr, "EGL ctx failed\n"); return 1; }
    if (!glctx_load_gl(c)) { fprintf(stderr, "GL loader failed\n"); return 1; }

    GLuint prog = shader_build(VS, FS);
    if (!prog) return 1;

    /* interleaved: x,y, r,g,b */
    const float verts[] = {
        0.0f,  0.8f,  1.0f, 0.2f, 0.2f,
       -0.8f, -0.7f,  0.2f, 1.0f, 0.2f,
        0.8f, -0.7f,  0.2f, 0.4f, 1.0f,
    };
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo);      glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5*sizeof(float), (void*)(2*sizeof(float)));

    glViewport(0, 0, W, H);
    glClearColor(0.04f, 0.04f, 0.07f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(prog);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    Image im = {0};
    if (!glctx_read_rgb(c, &im) || !image_write_png(&im, out)) {
        fprintf(stderr, "readback/write failed\n"); return 1;
    }
    size_t idx = ((size_t)(H/2) * (size_t)W + (size_t)(W/2)) * 3u;
    int r = im.rgb[idx], g = im.rgb[idx+1], b = im.rgb[idx+2];
    printf("wrote %s, centre pixel = (%d,%d,%d)\n", out, r, g, b);
    /* centre is inside the triangle -> clearly brighter than the clear colour */
    int ok = (r + g + b) > 90;
    printf("%s\n", ok ? "PASS" : "FAIL");

    image_free(&im);
    glctx_destroy(c);
    return ok ? 0 : 1;
}
