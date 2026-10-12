/*
 * tc2_gl_web.c -- the browser's 2D leftovers: the window blit, the crosshair and the menu's draw path use fixed-function
 * GL on a PC (src/tc2_window.c, third_party/nuklear_sdl_gl2.h); the web build has no fixed function (WebGL2 only, no
 * legacy-GL emulation), so those few draws come through the small shader programs here instead. The 3D picture itself
 * (src/tc2_gl.c) is already shader GL on every target.
 */
#ifdef __EMSCRIPTEN__
#include <stdio.h>
#include <GLES3/gl3.h>
#include "tc2_gl_web.h"

static GLuint prog_tex, prog_col, vbo;
static GLint u_tex, u_col;

static GLuint mkprog(const char *vs_src, const char *fs_src)
{
    const GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL); glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_uv");
    glLinkProgram(p);
    GLint good = 0; glGetProgramiv(p, GL_LINK_STATUS, &good);
    if (!good) { char log[512]; glGetProgramInfoLog(p, sizeof log, NULL, log); fprintf(stderr, "[WEB2D] link: %s\n", log); return 0; }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

static int init(void)
{
    static int tried;
    if (tried) return prog_tex != 0;
    tried = 1;
    prog_tex = mkprog(
        "attribute vec2 a_pos; attribute vec2 a_uv; varying vec2 v_uv;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n",
        "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv;\n"
        "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n");
    prog_col = mkprog(
        "attribute vec2 a_pos;\n"
        "void main() { gl_Position = vec4(a_pos, 0.0, 1.0); }\n",
        "precision mediump float; uniform vec4 u_col;\n"
        "void main() { gl_FragColor = u_col; }\n");
    if (!prog_tex || !prog_col) { prog_tex = 0; return 0; }
    u_tex = glGetUniformLocation(prog_tex, "u_tex");
    u_col = glGetUniformLocation(prog_col, "u_col");
    glGenBuffers(1, &vbo);
    return 1;
}

/* the texture into the pixel rectangle x, y_top, w, h of a dw x dh drawable (texture row 0 lands at the rectangle's top) */
void web2d_blit(unsigned tex, int x, int y_top, int w, int h, int dw, int dh, int sharp)
{
    if (!init() || dw <= 0 || dh <= 0) return;
    const float x0 = 2.0f * x / dw - 1.0f, x1 = 2.0f * (x + w) / dw - 1.0f;
    const float y0 = 1.0f - 2.0f * y_top / dh, y1 = 1.0f - 2.0f * (y_top + h) / dh;   /* y0 = top edge in clip space */
    const float v[] = { x0, y0, 0, 0,  x1, y0, 1, 0,  x1, y1, 1, 1,  x0, y0, 0, 0,  x1, y1, 1, 1,  x0, y1, 0, 1 };
    glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
    const GLint f = sharp ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glDisable(GL_BLEND);
    glUseProgram(prog_tex); glUniform1i(u_tex, 0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glUseProgram(0);
}

/* n solid rectangles (x0 y0 x1 y1 in drawable pixels, y down) -- the crosshair's bars */
void web2d_rects(const float *r, int n, float cr, float cg, float cb, int dw, int dh)
{
    if (!init() || dw <= 0 || dh <= 0 || n <= 0) return;
    enum { MAXR = 16 };
    float v[MAXR * 12];
    if (n > MAXR) n = MAXR;
    for (int i = 0; i < n; i++) {
        const float x0 = 2.0f * r[4 * i] / dw - 1.0f, y0 = 1.0f - 2.0f * r[4 * i + 1] / dh;
        const float x1 = 2.0f * r[4 * i + 2] / dw - 1.0f, y1 = 1.0f - 2.0f * r[4 * i + 3] / dh;
        float *o = v + 12 * i;
        o[0] = x0; o[1] = y0; o[2] = x1; o[3] = y0; o[4] = x1; o[5] = y1;
        o[6] = x0; o[7] = y0; o[8] = x1; o[9] = y1; o[10] = x0; o[11] = y1;
    }
    glDisable(GL_BLEND);
    glUseProgram(prog_col); glUniform4f(u_col, cr, cg, cb, 1.0f);
    glBindBuffer(GL_ARRAY_BUFFER, vbo); glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(n * 12 * sizeof(float)), v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void *)0);
    glDrawArrays(GL_TRIANGLES, 0, n * 6);
    glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glUseProgram(0);
}
#endif
