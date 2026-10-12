/* tc2_gl_web.h -- the browser build's shader-drawn 2D (src/tc2_gl_web.c): the blit, the crosshair. Web only. */
#ifndef TC2_GL_WEB_H
#define TC2_GL_WEB_H
void web2d_blit(unsigned tex, int x, int y_top, int w, int h, int dw, int dh, int sharp);
void web2d_rects(const float *r, int n, float cr, float cg, float cb, int dw, int dh);
#endif
