/*
 * Nuklear - 1.32.0 - public domain
 * no warrenty implied; use at your own risk.
 * authored from 2015-2017 by Micha Mettke
 */
/*
 * ==============================================================
 *
 *                              API
 *
 * ===============================================================
 */
#ifndef NK_SDL_GL2_H_
#define NK_SDL_GL2_H_

#include <SDL2/SDL.h>
NK_API struct nk_context*   nk_sdl_init(SDL_Window *win);
NK_API void                 nk_sdl_font_stash_begin(struct nk_font_atlas **atlas);
NK_API void                 nk_sdl_font_stash_end(void);
NK_API int                  nk_sdl_handle_event(SDL_Event *evt);
NK_API void                 nk_sdl_render(enum nk_anti_aliasing);
NK_API void                 nk_sdl_shutdown(void);
NK_API void                 nk_sdl_handle_grab(void);

#endif
/*
 * ==============================================================
 *
 *                          IMPLEMENTATION
 *
 * ===============================================================
 */
#ifdef NK_SDL_GL2_IMPLEMENTATION
#include <string.h>
#include <stdlib.h>
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>   /* the web render path below is shader GL: WebGL has no fixed function */
#endif

struct nk_sdl_device {
    struct nk_buffer cmds;
    struct nk_draw_null_texture tex_null;
    GLuint font_tex;
};

struct nk_sdl_vertex {
    float position[2];
    float uv[2];
    nk_byte col[4];
};

static struct nk_sdl {
    SDL_Window *win;
    struct nk_sdl_device ogl;
    struct nk_context ctx;
    struct nk_font_atlas atlas;
    Uint64 time_of_last_frame;
} sdl;

NK_INTERN void
nk_sdl_device_upload_atlas(const void *image, int width, int height)
{
    struct nk_sdl_device *dev = &sdl.ogl;
    glGenTextures(1, &dev->font_tex);
    glBindTexture(GL_TEXTURE_2D, dev->font_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, image);
}

#ifdef __EMSCRIPTEN__
/* THE WEB RENDER PATH: the same nk_convert output, drawn with a small shader program and real GL buffers --
 * WebGL2 has no client-side arrays and no fixed-function pipeline. Everything above this function is shared. */
static GLuint nk_web_prog, nk_web_vbo, nk_web_ebo;
static GLint nk_web_uproj, nk_web_utex;
static int nk_web_init(void)
{
    if (nk_web_prog) return 1;
    static const char *vs_src =
        "attribute vec2 a_pos; attribute vec2 a_uv; attribute vec4 a_col;\n"
        "uniform vec2 u_proj;\n"                                 /* 2 / window width, 2 / window height */
        "varying vec2 v_uv; varying vec4 v_col;\n"
        "void main() { gl_Position = vec4(a_pos.x * u_proj.x - 1.0, 1.0 - a_pos.y * u_proj.y, 0.0, 1.0); v_uv = a_uv; v_col = a_col; }\n";
    static const char *fs_src =
        "precision mediump float; uniform sampler2D u_tex; varying vec2 v_uv; varying vec4 v_col;\n"
        "void main() { gl_FragColor = v_col * texture2D(u_tex, v_uv); }\n";
    const GLuint vs = glCreateShader(GL_VERTEX_SHADER), fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(vs, 1, &vs_src, NULL); glCompileShader(vs);
    glShaderSource(fs, 1, &fs_src, NULL); glCompileShader(fs);
    nk_web_prog = glCreateProgram();
    glAttachShader(nk_web_prog, vs); glAttachShader(nk_web_prog, fs);
    glBindAttribLocation(nk_web_prog, 0, "a_pos"); glBindAttribLocation(nk_web_prog, 1, "a_uv"); glBindAttribLocation(nk_web_prog, 2, "a_col");
    glLinkProgram(nk_web_prog);
    GLint good = 0; glGetProgramiv(nk_web_prog, GL_LINK_STATUS, &good);
    if (!good) { nk_web_prog = 0; return 0; }
    glDeleteShader(vs); glDeleteShader(fs);
    nk_web_uproj = glGetUniformLocation(nk_web_prog, "u_proj");
    nk_web_utex = glGetUniformLocation(nk_web_prog, "u_tex");
    glGenBuffers(1, &nk_web_vbo); glGenBuffers(1, &nk_web_ebo);
    return 1;
}
#endif

NK_API void
nk_sdl_render(enum nk_anti_aliasing AA)
{
    /* setup global state */
    struct nk_sdl_device *dev = &sdl.ogl;
    int width, height;
    int display_width, display_height;
    struct nk_vec2 scale;

    Uint64 now = SDL_GetTicks64();
    sdl.ctx.delta_time_seconds = (float)(now - sdl.time_of_last_frame) / 1000;
    sdl.time_of_last_frame = now;

    SDL_GetWindowSize(sdl.win, &width, &height);
    SDL_GL_GetDrawableSize(sdl.win, &display_width, &display_height);
    scale.x = (float)display_width/(float)width;
    scale.y = (float)display_height/(float)height;

#ifdef __EMSCRIPTEN__
    if (!nk_web_init()) return;
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0,0,(GLsizei)display_width,(GLsizei)display_height);
    glUseProgram(nk_web_prog);
    glUniform2f(nk_web_uproj, 2.0f / (float)width, 2.0f / (float)height);
    glUniform1i(nk_web_utex, 0);
    glActiveTexture(GL_TEXTURE0);
#else
    glPushAttrib(GL_ENABLE_BIT|GL_COLOR_BUFFER_BIT|GL_TRANSFORM_BIT);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* setup viewport/project */
    glViewport(0,0,(GLsizei)display_width,(GLsizei)display_height);
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
#endif
    {
        GLsizei vs = sizeof(struct nk_sdl_vertex);
        size_t vp = offsetof(struct nk_sdl_vertex, position);
        size_t vt = offsetof(struct nk_sdl_vertex, uv);
        size_t vc = offsetof(struct nk_sdl_vertex, col);

        /* convert from command queue into draw list and draw to screen */
        const struct nk_draw_command *cmd;
        const nk_draw_index *offset = NULL;
        struct nk_buffer vbuf, ebuf;

        /* fill converting configuration */
        struct nk_convert_config config;
        static const struct nk_draw_vertex_layout_element vertex_layout[] = {
            {NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_sdl_vertex, position)},
            {NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_sdl_vertex, uv)},
            {NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nk_sdl_vertex, col)},
            {NK_VERTEX_LAYOUT_END}
        };
        memset(&config, 0, sizeof(config));
        config.vertex_layout = vertex_layout;
        config.vertex_size = sizeof(struct nk_sdl_vertex);
        config.vertex_alignment = NK_ALIGNOF(struct nk_sdl_vertex);
        config.tex_null = dev->tex_null;
        config.circle_segment_count = 22;
        config.curve_segment_count = 22;
        config.arc_segment_count = 22;
        config.global_alpha = 1.0f;
        config.shape_AA = AA;
        config.line_AA = AA;

        /* convert shapes into vertices */
        nk_buffer_init_default(&vbuf);
        nk_buffer_init_default(&ebuf);
        nk_convert(&sdl.ctx, &dev->cmds, &vbuf, &ebuf, &config);

#ifdef __EMSCRIPTEN__
        /* the vertices and indices into GL buffers; the commands index into them by byte offset */
        glBindBuffer(GL_ARRAY_BUFFER, nk_web_vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nk_buffer_total(&vbuf), nk_buffer_memory_const(&vbuf), GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, nk_web_ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)nk_buffer_total(&ebuf), nk_buffer_memory_const(&ebuf), GL_STREAM_DRAW);
        glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, vs, (const void*)vp);
        glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, vs, (const void*)vt);
        glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, vs, (const void*)vc);
        {
        size_t eoff = 0;
        nk_draw_foreach(cmd, &sdl.ctx, &dev->cmds)
        {
            if (!cmd->elem_count) continue;
            glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
            glScissor(
                (GLint)(cmd->clip_rect.x * scale.x),
                (GLint)((height - (GLint)(cmd->clip_rect.y + cmd->clip_rect.h)) * scale.y),
                (GLint)(cmd->clip_rect.w * scale.x),
                (GLint)(cmd->clip_rect.h * scale.y));
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_SHORT, (const void*)eoff);
            eoff += cmd->elem_count * sizeof(nk_draw_index);
        }
        }
        (void)offset;
#else
        /* setup vertex buffer pointer */
        {const void *vertices = nk_buffer_memory_const(&vbuf);
        glVertexPointer(2, GL_FLOAT, vs, (const void*)((const nk_byte*)vertices + vp));
        glTexCoordPointer(2, GL_FLOAT, vs, (const void*)((const nk_byte*)vertices + vt));
        glColorPointer(4, GL_UNSIGNED_BYTE, vs, (const void*)((const nk_byte*)vertices + vc));}

        /* iterate over and execute each draw command */
        offset = (const nk_draw_index*)nk_buffer_memory_const(&ebuf);
        nk_draw_foreach(cmd, &sdl.ctx, &dev->cmds)
        {
            if (!cmd->elem_count) continue;
            glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
            glScissor(
                (GLint)(cmd->clip_rect.x * scale.x),
                (GLint)((height - (GLint)(cmd->clip_rect.y + cmd->clip_rect.h)) * scale.y),
                (GLint)(cmd->clip_rect.w * scale.x),
                (GLint)(cmd->clip_rect.h * scale.y));
            glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_SHORT, offset);
            offset += cmd->elem_count;
        }
#endif
        nk_clear(&sdl.ctx);
        nk_buffer_clear(&dev->cmds);
        nk_buffer_free(&vbuf);
        nk_buffer_free(&ebuf);
    }

#ifdef __EMSCRIPTEN__
    glDisableVertexAttribArray(0); glDisableVertexAttribArray(1); glDisableVertexAttribArray(2);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glUseProgram(0);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, 0);
#else
    /* default OpenGL state */
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    glDisable(GL_CULL_FACE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_TEXTURE_2D);

    glBindTexture(GL_TEXTURE_2D, 0);
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glPopAttrib();
#endif
}

static void
nk_sdl_clipboard_paste(nk_handle usr, struct nk_text_edit *edit)
{
    const char *text = SDL_GetClipboardText();
    if (text) {
        nk_textedit_paste(edit, text, nk_strlen(text));
        SDL_free((void *)text);
    }
    (void)usr;
}

static void
nk_sdl_clipboard_copy(nk_handle usr, const char *text, int len)
{
    char *str = 0;
    (void)usr;
    if (!len) return;
    str = (char*)malloc((size_t)len+1);
    if (!str) return;
    memcpy(str, text, (size_t)len);
    str[len] = '\0';
    SDL_SetClipboardText(str);
    free(str);
}

NK_API struct nk_context*
nk_sdl_init(SDL_Window *win)
{
    sdl.win = win;
    nk_init_default(&sdl.ctx, 0);
    sdl.ctx.clip.copy = nk_sdl_clipboard_copy;
    sdl.ctx.clip.paste = nk_sdl_clipboard_paste;
    sdl.ctx.clip.userdata = nk_handle_ptr(0);
    nk_buffer_init_default(&sdl.ogl.cmds);
    sdl.time_of_last_frame = SDL_GetTicks64();
    return &sdl.ctx;
}

NK_API void
nk_sdl_font_stash_begin(struct nk_font_atlas **atlas)
{
    nk_font_atlas_init_default(&sdl.atlas);
    nk_font_atlas_begin(&sdl.atlas);
    *atlas = &sdl.atlas;
}

NK_API void
nk_sdl_font_stash_end(void)
{
    const void *image; int w, h;
    image = nk_font_atlas_bake(&sdl.atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
    nk_sdl_device_upload_atlas(image, w, h);
    nk_font_atlas_end(&sdl.atlas, nk_handle_id((int)sdl.ogl.font_tex), &sdl.ogl.tex_null);
    if (sdl.atlas.default_font)
        nk_style_set_font(&sdl.ctx, &sdl.atlas.default_font->handle);
}

NK_API void
nk_sdl_handle_grab(void)
{
    struct nk_context *ctx = &sdl.ctx;
    if (ctx->input.mouse.grab) {
        SDL_SetRelativeMouseMode(SDL_TRUE);
    } else if (ctx->input.mouse.ungrab) {
        /* better support for older SDL by setting mode first; causes an extra mouse motion event */
        SDL_SetRelativeMouseMode(SDL_FALSE);
        SDL_WarpMouseInWindow(sdl.win, (int)ctx->input.mouse.prev.x, (int)ctx->input.mouse.prev.y);
    } else if (ctx->input.mouse.grabbed) {
        ctx->input.mouse.pos.x = ctx->input.mouse.prev.x;
        ctx->input.mouse.pos.y = ctx->input.mouse.prev.y;
    }
}

NK_API int
nk_sdl_handle_event(SDL_Event *evt)
{
    struct nk_context *ctx = &sdl.ctx;
    int ctrl_down = SDL_GetModState() & KMOD_CTRL;
    static int insert_toggle = 0;

    switch(evt->type)
    {
        case SDL_KEYUP: /* KEYUP & KEYDOWN share same routine */
        case SDL_KEYDOWN:
            {
                int down = evt->type == SDL_KEYDOWN;
                switch(evt->key.keysym.sym)
                {
                    case SDLK_LALT:
                    case SDLK_RALT:      nk_input_key(ctx, NK_KEY_ALT, down); break;
                    case SDLK_RSHIFT: /* RSHIFT & LSHIFT share same routine */
                    case SDLK_LSHIFT:    nk_input_key(ctx, NK_KEY_SHIFT, down); break;
                    case SDLK_DELETE:    nk_input_key(ctx, NK_KEY_DEL, down); break;

                    case SDLK_KP_ENTER:
                    case SDLK_RETURN:    nk_input_key(ctx, NK_KEY_ENTER, down); break;

                    case SDLK_TAB:       nk_input_key(ctx, NK_KEY_TAB, down); break;
                    case SDLK_BACKSPACE: nk_input_key(ctx, NK_KEY_BACKSPACE, down); break;
                    case SDLK_HOME:      nk_input_key(ctx, NK_KEY_TEXT_START, down);
                                         nk_input_key(ctx, NK_KEY_SCROLL_START, down); break;
                    case SDLK_END:       nk_input_key(ctx, NK_KEY_TEXT_END, down);
                                         nk_input_key(ctx, NK_KEY_SCROLL_END, down); break;
                    case SDLK_PAGEDOWN:  nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down); break;
                    case SDLK_PAGEUP:    nk_input_key(ctx, NK_KEY_SCROLL_UP, down); break;
                    case SDLK_F1:        nk_input_key(ctx, NK_KEY_F1, down); break;
                    case SDLK_F2:        nk_input_key(ctx, NK_KEY_F2, down); break;
                    case SDLK_F3:        nk_input_key(ctx, NK_KEY_F3, down); break;
                    case SDLK_F4:        nk_input_key(ctx, NK_KEY_F4, down); break;
                    case SDLK_F5:        nk_input_key(ctx, NK_KEY_F5, down); break;
                    case SDLK_F6:        nk_input_key(ctx, NK_KEY_F6, down); break;
                    case SDLK_F7:        nk_input_key(ctx, NK_KEY_F7, down); break;
                    case SDLK_F8:        nk_input_key(ctx, NK_KEY_F8, down); break;
                    case SDLK_F9:        nk_input_key(ctx, NK_KEY_F9, down); break;
                    case SDLK_F10:       nk_input_key(ctx, NK_KEY_F10, down); break;
                    case SDLK_F11:       nk_input_key(ctx, NK_KEY_F11, down); break;
                    case SDLK_F12:       nk_input_key(ctx, NK_KEY_F12, down); break;
                    case SDLK_z:         nk_input_key(ctx, NK_KEY_TEXT_UNDO, down && ctrl_down); break;
                    case SDLK_r:         nk_input_key(ctx, NK_KEY_TEXT_REDO, down && ctrl_down); break;
                    case SDLK_c:         nk_input_key(ctx, NK_KEY_COPY, down && ctrl_down); break;
                    case SDLK_v:         nk_input_key(ctx, NK_KEY_PASTE, down && ctrl_down); break;
                    case SDLK_x:         nk_input_key(ctx, NK_KEY_CUT, down && ctrl_down); break;
                    case SDLK_b:         nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down && ctrl_down); break;
                    case SDLK_e:         nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down && ctrl_down); break;
                    case SDLK_UP:        nk_input_key(ctx, NK_KEY_UP, down); break;
                    case SDLK_DOWN:      nk_input_key(ctx, NK_KEY_DOWN, down); break;
                    case SDLK_ESCAPE:    nk_input_key(ctx, NK_KEY_TEXT_RESET_MODE, down); break;
                    case SDLK_INSERT:
                        if (down) insert_toggle = !insert_toggle;
                        if (insert_toggle) {
                            nk_input_key(ctx, NK_KEY_TEXT_INSERT_MODE, down);
                        } else {
                            nk_input_key(ctx, NK_KEY_TEXT_REPLACE_MODE, down);
                        }
                        break;
                    case SDLK_a:
                        if (ctrl_down)
                            nk_input_key(ctx,NK_KEY_TEXT_SELECT_ALL, down);
                        break;
                    case SDLK_LEFT:
                        if (ctrl_down)
                            nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down);
                        else nk_input_key(ctx, NK_KEY_LEFT, down);
                        break;
                    case SDLK_RIGHT:
                        if (ctrl_down)
                            nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down);
                        else nk_input_key(ctx, NK_KEY_RIGHT, down);
                        break;
                }
            }
            return 1;

        case SDL_MOUSEBUTTONUP: /* MOUSEBUTTONUP & MOUSEBUTTONDOWN share same routine */
        case SDL_MOUSEBUTTONDOWN:
            {
                int down = evt->type == SDL_MOUSEBUTTONDOWN;
                const int x = evt->button.x, y = evt->button.y;
                switch(evt->button.button)
                {
                    case SDL_BUTTON_LEFT:
                        if (evt->button.clicks > 1)
                            nk_input_button(ctx, NK_BUTTON_DOUBLE, x, y, down);
                        nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down); break;
                    case SDL_BUTTON_MIDDLE: nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down); break;
                    case SDL_BUTTON_RIGHT:  nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down); break;
                    case SDL_BUTTON_X1:     nk_input_button(ctx, NK_BUTTON_X1, x, y, down); break;
                    case SDL_BUTTON_X2:     nk_input_button(ctx, NK_BUTTON_X2, x, y, down); break;
                }
            }
            return 1;

        case SDL_MOUSEMOTION:
            if (ctx->input.mouse.grabbed) {
                int x = (int)ctx->input.mouse.prev.x, y = (int)ctx->input.mouse.prev.y;
                nk_input_motion(ctx, x + evt->motion.xrel, y + evt->motion.yrel);
            }
            else nk_input_motion(ctx, evt->motion.x, evt->motion.y);
            return 1;

        case SDL_TEXTINPUT:
            {
                nk_glyph glyph;
                memcpy(glyph, evt->text.text, NK_UTF_SIZE);
                nk_input_glyph(ctx, glyph);
            }
            return 1;

        case SDL_MOUSEWHEEL:
            nk_input_scroll(ctx,nk_vec2(evt->wheel.preciseX, evt->wheel.preciseY));
            return 1;
    }
    return 0;
}

NK_API
void nk_sdl_shutdown(void)
{
    struct nk_sdl_device *dev = &sdl.ogl;
    nk_font_atlas_clear(&sdl.atlas);
    nk_free(&sdl.ctx);
    glDeleteTextures(1, &dev->font_tex);
    nk_buffer_free(&dev->cmds);
    memset(&sdl, 0, sizeof(sdl));
}

#endif
