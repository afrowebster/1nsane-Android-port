/* DirectDraw 7 + Direct3D 7 on OpenGL ES 3.
 *
 * Surfaces live in guest memory (what Lock returns). Surfaces the game renders into (primary, back
 * buffer, 3D targets) are also GL framebuffers and the GPU copy is the up-to-date one; memory copies are
 * refreshed on demand (Lock, Blt to a memory surface). Other surfaces are uploaded to GL textures when
 * used after a change. Vertices are transformed (and lit) on the CPU; the fragment shader implements
 * the fixed-function texture stages, fog, alpha test and colour keys. */
#include <math.h>
#include "win32.h"
#include "../platform/plat.h"

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_INVALIDPARAMS 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_SURFACEBUSY 0x887601aeu
#define DDERR_UNSUPPORTED 0x80004001u

/* ---------------------------------------------------------------- pixel formats */
typedef struct { uint32_t flags, bits, r, g, b, a; } PixFmt;
static const PixFmt PF_565 = { 0x40, 16, 0xf800, 0x07e0, 0x001f, 0 };
static const PixFmt PF_555 = { 0x40, 16, 0x7c00, 0x03e0, 0x001f, 0 };
static const PixFmt PF_1555 = { 0x41, 16, 0x7c00, 0x03e0, 0x001f, 0x8000 };
static const PixFmt PF_4444 = { 0x41, 16, 0x0f00, 0x00f0, 0x000f, 0xf000 };
static const PixFmt PF_8888 = { 0x41, 32, 0xff0000, 0xff00, 0xff, 0xff000000u };
static const PixFmt PF_X888 = { 0x40, 32, 0xff0000, 0xff00, 0xff, 0 };

static void pf_write(uint32_t p, const PixFmt *f) {
    memset(GPTR(p), 0, 32);
    gw32(p, 32); gw32(p + 4, f->flags); gw32(p + 12, f->bits);
    gw32(p + 16, f->r); gw32(p + 20, f->g); gw32(p + 24, f->b); gw32(p + 28, f->a);
}
static void pf_read(uint32_t p, PixFmt *f) {
    f->flags = gr32(p + 4); f->bits = gr32(p + 12);
    f->r = gr32(p + 16); f->g = gr32(p + 20); f->b = gr32(p + 24); f->a = gr32(p + 28);
    if (!(f->flags & 0x41)) f->a = 0;
    if (!(f->flags & 1)) f->a = 0;
}
static int mask_shift(uint32_t m) { return m ? __builtin_ctz(m) : 0; }
static int mask_bits(uint32_t m) { return m ? __builtin_popcount(m) : 0; }

/* ---------------------------------------------------------------- objects */
typedef struct Surf {
    uint32_t obj;
    int w, h, pitch, bpp;
    PixFmt pf;
    uint32_t caps, caps2;
    int is_z;
    uint32_t mem;               /* guest pixels */
    int rt;                     /* has a GL framebuffer (render target) */
    int gpu_newer;              /* rt: GL has drawing that memory lacks */
    int mem_newer;              /* memory changed since the GL copy (texture or rt) */
    GLuint tex, fbo, depth;
    int tw, th;                 /* GL texture/framebuffer size (rt: scaled) */
    int tex_ck;                 /* texture was converted with the colour key */
    struct Surf *back, *zbuf, *mip, *parent;
    int has_ck; uint32_t ck_lo, ck_hi;
    int refs;
    uint32_t pal;
    int lock_rt_write;
    uint32_t uniq;
} Surf;

typedef struct VB { uint32_t obj, fvf, n, stride, mem; int refs; } VB;

static uint32_t vt_dd1, vt_dd7, vt_surf7, vt_surf1, vt_d3d7, vt_dev7, vt_vb7, vt_clip, vt_pal, vt_gamma;
static uint32_t g_dd_obj, g_dd1_obj, g_d3d_obj, g_dev_obj, g_gamma_obj;
static Surf *g_primary;
static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 16;
static int g_scale = 1;
static int g_frame, g_logframe = -1;
/* races draw fogged 3D, menus don't: a race is on screen while recent frames had fogged draws */
static int g_fog_draws, g_fog_frames;
static int g_hud_phase;
static int g_ndraws, g_nrecorded;   /* GL draw calls / D3D draws (C2_PERF log) */
static int nvar;                    /* shader variants (defined with variant()) */
   /* a 2D draw happened this frame: later perspective draws are HUD models */
static uint32_t g_race_ms;   /* frame counter; C2_D3DLOG=frame logs that frame */          /* render targets are this many times the surface size */

/* ---------------------------------------------------------------- GL helpers */
static GLuint prog2d, vbo, ibo, vao;
static void q_flush(void);
static GLint U2_tex, U2_ck, U2_rect;
static int gl_ready;

/* performance readout: GL submission and swap time per frame (game time is the rest) */
#include <time.h>
static uint64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000u + t.tv_nsec / 1000; }
static uint64_t perf_gl_us;
static float perf_fps, perf_game_ms, perf_gl_ms, perf_swap_ms;
void d3d_perf(float *fps, float *game, float *gl, float *swap) { *fps = perf_fps; *game = perf_game_ms; *gl = perf_gl_ms; *swap = perf_swap_ms; }
static GLuint compile(GLenum t, const char *src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(s, sizeof log, NULL, log); rt_fatal("shader: %s", log); }
    return s;
}
static GLuint link(const char *vs, const char *fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "a_pos"); glBindAttribLocation(p, 1, "a_col"); glBindAttribLocation(p, 2, "a_spec");
    glBindAttribLocation(p, 3, "a_uv0"); glBindAttribLocation(p, 4, "a_uv1"); glBindAttribLocation(p, 5, "a_fog");
    glLinkProgram(p);
    GLint ok; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[2048]; glGetProgramInfoLog(p, sizeof log, NULL, log); rt_fatal("link: %s", log); }
    return p;
}

static const char *VS3D =
    "#version 300 es\n"
    "in vec4 a_pos; in vec4 a_col; in vec4 a_spec; in vec2 a_uv0; in vec2 a_uv1; in float a_fog;\n"
    "out vec4 v_col; out vec4 v_spec; out vec2 v_uv0; out vec2 v_uv1; out float v_fog;\n"
    "void main() { gl_Position = a_pos; v_col = a_col; v_spec = a_spec; v_uv0 = a_uv0; v_uv1 = a_uv1; v_fog = a_fog; }\n";
static const char *FS3D_BODY =   /* after the per-variant constants (see variant()) */
    "in vec4 v_col; in vec4 v_spec; in vec2 v_uv0; in vec2 v_uv1; in float v_fog;\n"
    "uniform sampler2D u_t0, u_t1, u_t2, u_t3;\n"
    "uniform vec4 u_tfactor; uniform vec3 u_fogcol; uniform float u_aref;\n"
    "out vec4 o;\n"
    "vec4 arg(int a, vec4 cur, vec4 tex) {\n"
    "  int s = a & 15; vec4 r = s == 0 ? v_col : s == 1 ? cur : s == 2 ? tex : s == 3 ? u_tfactor : v_spec;\n"
    "  if ((a & 32) != 0) r = vec4(r.a); if ((a & 16) != 0) r = 1.0 - r; return r; }\n"
    "vec4 comb(int op, vec4 a1, vec4 a2, vec4 cur, vec4 tex) {\n"
    "  if (op == 2) return a1; if (op == 3) return a2; if (op == 4) return a1 * a2; if (op == 5) return 2.0 * a1 * a2;\n"
    "  if (op == 6) return 4.0 * a1 * a2; if (op == 7) return a1 + a2; if (op == 8) return a1 + a2 - 0.5;\n"
    "  if (op == 9) return 2.0 * (a1 + a2 - 0.5); if (op == 10) return a1 - a2; if (op == 11) return a1 + a2 - a1 * a2;\n"
    "  if (op == 12) return mix(a2, a1, v_col.a); if (op == 13) return mix(a2, a1, tex.a); if (op == 14) return mix(a2, a1, u_tfactor.a);\n"
    "  if (op == 15) return a1 + a2 * (1.0 - tex.a); if (op == 16) return mix(a2, a1, cur.a);\n"
    "  if (op == 18) return vec4(a1.rgb + a1.a * a2.rgb, a1.a * a2.a); if (op == 19) return vec4(a1.rgb * a2.rgb + a1.a, a1.a * a2.a);\n"
    "  if (op == 20) return vec4((1.0 - a1.a) * a2.rgb + a1.rgb, a1.a * a2.a); if (op == 21) return vec4((1.0 - a1.rgb) * a2.rgb + a1.a, a1.a * a2.a);\n"
    "  if (op == 24) { float d = clamp(4.0 * dot(a1.rgb - 0.5, a2.rgb - 0.5), 0.0, 1.0); return vec4(d); }\n"
    "  return a1; }\n"
    "vec4 samp(int i) {\n"
    "  vec2 uv = u_tci[i] == 1 ? v_uv1 : v_uv0;\n"
    "  if (u_hastex[i] == 0) return vec4(1.0);\n"
    "  if (i == 0) return texture(u_t0, uv); if (i == 1) return texture(u_t1, uv); if (i == 2) return texture(u_t2, uv); return texture(u_t3, uv); }\n"
    "void main() {\n"
    "  vec4 cur = v_col; vec4 t0 = vec4(1.0);\n"
    "  for (int i = 0; i < 4; i++) {\n"
    "    if (i >= u_nstage || u_top[i] == 1) break;\n"
    "    vec4 tex = samp(i); if (i == 0) t0 = tex;\n"
    "    vec4 c = comb(u_top[i], arg(u_ca1[i], cur, tex), arg(u_ca2[i], cur, tex), cur, tex);\n"
    "    float a = u_aop[i] == 1 ? cur.a : comb(u_aop[i], arg(u_aa1[i], cur, tex), arg(u_aa2[i], cur, tex), cur, tex).a;\n"
    "    cur = clamp(vec4(c.rgb, a), 0.0, 1.0);\n"
    "  }\n"
    "  if (u_ckon != 0 && t0.a < 0.004) discard;\n"
    "  if (u_spec != 0) cur.rgb = clamp(cur.rgb + v_spec.rgb, 0.0, 1.0);\n"
    "  if (u_fogon != 0) cur.rgb = mix(u_fogcol, cur.rgb, clamp(v_fog, 0.0, 1.0));\n"
    "  if (u_atest != 0) {\n"
    "    float a = cur.a; bool pass = u_atest == 2 ? a < u_aref : u_atest == 3 ? abs(a - u_aref) < 0.002 : u_atest == 4 ? a <= u_aref + 0.002 :\n"
    "      u_atest == 5 ? a > u_aref : u_atest == 6 ? abs(a - u_aref) >= 0.002 : u_atest == 7 ? a >= u_aref - 0.002 : u_atest == 8;\n"
    "    if (!pass) discard; }\n"
    "  o = cur; }\n";
static const char *VS2D =
    "#version 300 es\n"
    "in vec4 a_pos; in vec2 a_uv0; out vec2 v_uv;\n"
    "void main() { gl_Position = a_pos; v_uv = a_uv0; }\n";
static const char *FS2D =
    "#version 300 es\n"
    "precision mediump float; in vec2 v_uv; uniform sampler2D u_tex; uniform int u_ck; out vec4 o;\n"
    "void main() { vec4 c = texture(u_tex, v_uv); if (u_ck != 0 && c.a < 0.004) discard; o = vec4(c.rgb, 1.0); }\n";

static void gl_init(void) {
    if (gl_ready) return;
    gl_ready = 1;
    plat_create_gl(g_mode_w, g_mode_h);
    prog2d = link(VS2D, FS2D);
    glUseProgram(prog2d);
    U2_tex = glGetUniformLocation(prog2d, "u_tex"); U2_ck = glGetUniformLocation(prog2d, "u_ck");
    glUniform1i(U2_tex, 0);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    glGenBuffers(1, &vbo); glGenBuffers(1, &ibo);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_PACK_ALIGNMENT, 1);
    int dw, dh; plat_drawable_size(&dw, &dh);
    LOG("ddraw: GL ready, drawable %dx%d", dw, dh);
}

/* ---------------------------------------------------------------- pixel conversion */
static inline uint32_t ext(uint32_t v, uint32_t m) {
    if (!m) return 255;
    int s = mask_shift(m), b = mask_bits(m);
    uint32_t x = (v & m) >> s;
    if (b >= 8) return x >> (b - 8);
    /* replicate bits */
    uint32_t r = x << (8 - b);
    for (int k = b; k < 8; k += b) r |= r >> b;
    return r & 255;
}
static inline uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a, const PixFmt *f) {
    uint32_t v = 0;
#define PK(c, m) if (m) { int s_ = mask_shift(m), b_ = mask_bits(m); v |= (b_ >= 8 ? (c) << (b_ - 8) : (c) >> (8 - b_)) << s_ & (m); }
    PK(r, f->r) PK(g, f->g) PK(b, f->b) PK(a, f->a)
    return v;
}
static inline uint32_t px_get(const uint8_t *p, int bpp) {
    return bpp == 2 ? *(const uint16_t *)p : bpp == 4 ? *(const uint32_t *)p : bpp == 1 ? *p : (p[0] | p[1] << 8 | p[2] << 16);
}
static inline void px_put(uint8_t *p, int bpp, uint32_t v) {
    if (bpp == 2) *(uint16_t *)p = v; else if (bpp == 4) *(uint32_t *)p = v; else if (bpp == 1) *p = v;
    else { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; }
}
/* memory rows -> RGBA8 (colour-keyed pixels get alpha 0 when ck) */
static void to_rgba(Surf *s, int x0, int y0, int w, int h, uint8_t *out, int ck) {
    const PixFmt *f = &s->pf;
    for (int y = 0; y < h; y++) {
        const uint8_t *src = GPTR(s->mem + (y0 + y) * s->pitch + x0 * s->bpp);
        uint8_t *d = out + (size_t)y * w * 4;
        if (f->bits == 16 && f->r == 0xf800 && f->g == 0x07e0 && f->b == 0x1f && !ck) {
            const uint16_t *q = (const uint16_t *)src;
            for (int x = 0; x < w; x++) { uint32_t v = q[x]; d[0] = (v >> 8 & 0xf8) | v >> 13; d[1] = (v >> 3 & 0xfc) | (v >> 9 & 3); d[2] = (v << 3 & 0xf8) | (v >> 2 & 7); d[3] = 255; d += 4; }
            continue;
        }
        for (int x = 0; x < w; x++) {
            uint32_t v = px_get(src + x * s->bpp, s->bpp);
            d[0] = ext(v, f->r); d[1] = ext(v, f->g); d[2] = ext(v, f->b); d[3] = f->a ? ext(v, f->a) : 255;
            if (ck && v >= s->ck_lo && v <= s->ck_hi) d[3] = 0;
            d += 4;
        }
    }
}
static void from_rgba(Surf *s, int x0, int y0, int w, int h, const uint8_t *in) {
    for (int y = 0; y < h; y++) {
        uint8_t *dst = GPTR(s->mem + (y0 + y) * s->pitch + x0 * s->bpp);
        const uint8_t *q = in + (size_t)y * w * 4;
        for (int x = 0; x < w; x++, q += 4) px_put(dst + x * s->bpp, s->bpp, pack(q[0], q[1], q[2], q[3], &s->pf));
    }
}

/* ---------------------------------------------------------------- surface <-> GL */
static uint32_t g_uniq = 1;
static void surf_alloc_gl_rt(Surf *s) {
    gl_init();
    if (s->fbo) return;
    s->tw = s->w * g_scale; s->th = s->h * g_scale;
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->tw, s->th, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &s->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, 0);
    glGenRenderbuffers(1, &s->depth);
    glBindRenderbuffer(GL_RENDERBUFFER, s->depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s->tw, s->th);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s->depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) rt_fatal("framebuffer incomplete");
    glClearColor(0, 0, 0, 1); glClearDepthf(1); glDisable(GL_SCISSOR_TEST); glDepthMask(GL_TRUE); glColorMask(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    LOG("ddraw: render target %dx%d -> %dx%d", s->w, s->h, s->tw, s->th);
}
/* make the memory copy current (rt) */
static void surf_download(Surf *s) {
    if (!s->rt || !s->gpu_newer || !s->fbo) return;
    q_flush();
    uint8_t *b = malloc((size_t)s->tw * s->th * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, s->fbo);
    glReadPixels(0, 0, s->tw, s->th, GL_RGBA, GL_UNSIGNED_BYTE, b);
    uint8_t *row = malloc((size_t)s->w * 4);
    for (int y = 0; y < s->h; y++) {
        const uint8_t *src = b + (size_t)(s->th - 1 - y * g_scale) * s->tw * 4;   /* GL rows are bottom-up */
        for (int x = 0; x < s->w; x++) memcpy(row + x * 4, src + x * g_scale * 4, 4);
        from_rgba(s, 0, y, s->w, 1, row);
    }
    free(row); free(b);
    s->gpu_newer = 0;
}
/* texture of a memory surface (uploaded when changed) */
static int g_ck_enable;
static GLuint surf_texture(Surf *s, int ck) {
    gl_init();
    ck = ck && s->has_ck;
    if (s->rt) { return s->tex; }
    if (s->tex && !s->mem_newer && s->tex_ck == ck) return s->tex;
    q_flush();   /* queued draws may use the old contents */
    if (!s->tex) {
        glGenTextures(1, &s->tex);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    }
    glBindTexture(GL_TEXTURE_2D, s->tex);
    int lvl = 0;
    for (Surf *m = s; m; m = m->mip, lvl++) {
        uint8_t *b = malloc((size_t)m->w * m->h * 4);
        m->has_ck = s->has_ck; m->ck_lo = s->ck_lo; m->ck_hi = s->ck_hi;
        to_rgba(m, 0, 0, m->w, m->h, b, ck);
        glTexImage2D(GL_TEXTURE_2D, lvl, GL_RGBA8, m->w, m->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
        free(b);
        m->mem_newer = 0;
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, lvl - 1);
    s->tw = s->w; s->th = s->h; s->tex_ck = ck; s->mem_newer = 0;
    return s->tex;
}

/* ---------------------------------------------------------------- 2D drawing (Blt) */
typedef struct { float x, y, z, w; uint8_t c[4], s[4]; float u0, v0, u1, v1, fog; } GVert;

static void bind_rt(Surf *d) {
    q_flush();
    glBindFramebuffer(GL_FRAMEBUFFER, d->fbo);
    glViewport(0, 0, d->tw, d->th);
}
/* draw texture 'tex' (src rect in texel coords of a tw x th texture, flip_v for rt sources) to rt dst rect */
static void draw_quad(Surf *d, GLuint tex, int tw, int th, int src_rt, float sx0, float sy0, float sx1, float sy1,
                      float dx0, float dy0, float dx1, float dy1, int ck, int linear) {
    bind_rt(d);
    glUseProgram(prog2d);
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE); glColorMask(1, 1, 1, 1);
    glUniform1i(U2_ck, ck);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    float u0 = sx0 / tw, u1 = sx1 / tw, v0 = sy0 / th, v1 = sy1 / th;
    if (src_rt) { v0 = 1 - v0; v1 = 1 - v1; }
    float W = d->w, H = d->h;
    float X0 = dx0 / W * 2 - 1, X1 = dx1 / W * 2 - 1, Y0 = 1 - dy0 / H * 2, Y1 = 1 - dy1 / H * 2;
    GVert q[4] = {
        { X0, Y0, 0, 1, {0}, {0}, u0, v0 }, { X1, Y0, 0, 1, {0}, {0}, u1, v0 },
        { X0, Y1, 0, 1, {0}, {0}, u0, v1 }, { X1, Y1, 0, 1, {0}, {0}, u1, v1 } };
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(GVert), (void *)0);
    for (int i = 1; i < 6; i++) glDisableVertexAttribArray(i);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, 0, sizeof(GVert), (void *)offsetof(GVert, u0));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    d->gpu_newer = 1;
}

/* present a render target on the screen */
static void present(Surf *s) {
    gl_init();
    q_flush();
    if (!s->fbo) return;
    int dw, dh; plat_drawable_size(&dw, &dh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, dw, dh);
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1); glColorMask(1, 1, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    /* fit, keeping the aspect ratio */
    float sa = (float)s->w / s->h, da = (float)dw / dh;
    int vw = dw, vh = dh;
    if (da > sa) vw = (int)(dh * sa + 0.5f); else vh = (int)(dw / sa + 0.5f);
    int vx = (dw - vw) / 2, vy = (dh - vh) / 2;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
    glBlitFramebuffer(0, 0, s->tw, s->th, vx, vy, vx + vw, vy + vh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    uint64_t ts = now_us();
    plat_swap();
    uint64_t te = now_us();
    {   static uint64_t last;
        if (last) {
            float frame = (te - last) / 1000.0f, gl = perf_gl_us / 1000.0f, sw = (te - ts) / 1000.0f, a = 0.1f;
            perf_fps += a * (1000.0f / (frame > 0.1f ? frame : 0.1f) - perf_fps);
            perf_gl_ms += a * (gl - perf_gl_ms);
            perf_swap_ms += a * (sw - perf_swap_ms);
            perf_game_ms += a * (frame - gl - sw - perf_game_ms);
        }
        last = te; perf_gl_us = 0;
    }
    plat_pump_events();
    {   static uint32_t t0, n;
        uint32_t now = plat_ticks(); n++;
        if (!t0) t0 = now;
        if (now - t0 >= 5000) { LOG("fps %.1f (frame %d), per frame: %d D3D draws -> %d GL draw calls, %d shader variants", n * 1000.0 / (now - t0), g_frame,
            g_nrecorded / (int)n, g_ndraws / (int)n, nvar); t0 = now; n = 0; g_nrecorded = g_ndraws = 0; }
    }
}
/* window coords (0..1) -> game coords */
int d3d_to_game(float fx, float fy, int *gx, int *gy) {
    int dw, dh; plat_drawable_size(&dw, &dh);
    if (dw <= 0 || dh <= 0) return 0;
    float sa = (float)g_mode_w / g_mode_h, da = (float)dw / dh;
    float vw = 1, vh = 1;
    if (da > sa) vw = sa / da; else vh = da / sa;
    float x = (fx - (1 - vw) / 2) / vw, y = (fy - (1 - vh) / 2) / vh;
    if (x < 0) x = 0; if (x > 1) x = 1; if (y < 0) y = 0; if (y > 1) y = 1;
    *gx = (int)(x * g_mode_w); *gy = (int)(y * g_mode_h);
    if (*gx >= g_mode_w) *gx = g_mode_w - 1;
    if (*gy >= g_mode_h) *gy = g_mode_h - 1;
    return 1;
}

/* ---------------------------------------------------------------- surface creation */
#define S() COM_HOST(Surf)
static Surf *surf_new(int w, int h, const PixFmt *pf, uint32_t caps) {
    Surf *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->pf = *pf; s->caps = caps; s->refs = 1;
    s->bpp = pf->bits / 8; if (!s->bpp) s->bpp = 2;
    s->pitch = (w * s->bpp + 3) & ~3;
    s->mem = gcalloc(s->pitch * h + 16);
    s->obj = com_new(vt_surf7, 0, s);
    s->uniq = g_uniq++;
    s->mem_newer = 1;
    return s;
}
static Surf *surf_of(uint32_t obj) { return obj ? (Surf *)com_host(obj) : NULL; }

static void desc_write(Surf *s, uint32_t d, int v7) {
    uint32_t size = gr32(d);
    if (size < 108) size = v7 ? 124 : 108;
    memset(GPTR(d), 0, size);
    gw32(d, size);
    gw32(d + 4, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | (s->has_ck ? 0x10000 : 0));
    gw32(d + 8, s->h); gw32(d + 12, s->w); gw32(d + 16, s->pitch);
    if (s->has_ck) { gw32(d + 64, s->ck_lo); gw32(d + 68, s->ck_hi); }
    if (s->is_z) {
        memset(GPTR(d + 72), 0, 32);
        gw32(d + 72, 32); gw32(d + 76, 0x400); gw32(d + 84, s->pf.bits); gw32(d + 92, s->pf.bits == 16 ? 0xffff : 0xffffff);
    } else pf_write(d + 72, &s->pf);
    gw32(d + 104, s->caps);
    if (v7 && size >= 124) gw32(d + 108, s->caps2);
}

static uint32_t create_surface(CPU *c, uint32_t d, uint32_t out, int v7) {
    uint32_t fl = gr32(d + 4), caps = gr32(d + 104);
    int w = (fl & 4) ? (int)gr32(d + 12) : g_mode_w, h = (fl & 2) ? (int)gr32(d + 8) : g_mode_h;
    PixFmt pf = g_mode_bpp == 32 ? PF_X888 : PF_565;
    int is_z = 0;
    if (fl & 0x1000) {
        uint32_t pfl = gr32(d + 76);
        if (pfl & 0x400) { is_z = 1; pf.bits = gr32(d + 84); pf.flags = 0x400; pf.r = pf.g = pf.b = pf.a = 0; }
        else pf_read(d + 72, &pf);
    }
    if (caps & 0x20000) is_z = 1;
    if (is_z && !(fl & 0x1000)) { pf.bits = 16; pf.flags = 0x400; pf.r = pf.g = pf.b = pf.a = 0; }
    if (caps & 0x200) { w = g_mode_w; h = g_mode_h; pf = g_mode_bpp == 32 ? PF_X888 : PF_565; }
    Surf *s = surf_new(w, h, &pf, caps);
    s->is_z = is_z;
    if (v7) s->caps2 = gr32(d + 108);
    if (fl & 0x10000) { s->has_ck = 1; s->ck_lo = gr32(d + 64); s->ck_hi = gr32(d + 68); }
    LOG("CreateSurface %dx%d %dbpp caps=%x caps2=%x flags=%x%s -> %08x", w, h, pf.bits, caps, s->caps2, fl, is_z ? " (z)" : "", s->obj);
    if (caps & 0x200) {   /* primary */
        s->rt = 1; s->caps |= 0x8000 | 0x4000 | 0x20;
        surf_alloc_gl_rt(s);
        g_primary = s;
        int nb = (fl & 0x20) ? gr32(d + 20) : 0;
        Surf *prev = s;
        for (int i = 0; i < nb; i++) {
            Surf *b = surf_new(w, h, &pf, (caps & ~0x200 & ~0x20) | 0x4 | 0x4000);
            b->rt = 1; surf_alloc_gl_rt(b);
            b->parent = s;
            prev->back = b; prev = b;
            LOG("  back buffer %08x", b->obj);
        }
    } else if (!is_z && (caps & 0x2000)) {   /* offscreen 3D render target */
        s->rt = 1; surf_alloc_gl_rt(s);
    }
    if (caps & 0x400000 && (fl & 0x20000) && !is_z) {   /* mip chain */
        int n = gr32(d + 24);
        Surf *prev = s;
        for (int i = 1; i < n; i++) {
            int mw = w >> i, mh = h >> i;
            if (mw < 1) mw = 1;
            if (mh < 1) mh = 1;
            Surf *m = surf_new(mw, mh, &pf, caps);
            m->caps2 |= 0x1000;   /* DDSCAPS2_MIPMAPSUBLEVEL */
            m->parent = s;
            prev->mip = m; prev = m;
        }
    }
    gw32(out, s->obj);
    return DD_OK;
}

/* ---------------------------------------------------------------- IDirectDraw */
static uint32_t ok(CPU *c) { return DD_OK; }
static uint32_t one(CPU *c) { return 1; }
static uint32_t notimpl(CPU *c) { return DDERR_UNSUPPORTED; }

static const uint8_t IID_DD7[16] = { 0xc0,0x5e,0xe6,0x15,0x9c,0x3b,0xd2,0x11,0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b };
static const uint8_t IID_D3D7[16] = { 0x77,0x9e,0x04,0xf5,0x61,0x48,0xd2,0x11,0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8 };
static const uint8_t IID_GAMMA[16] = { 0x3e,0x1c,0xc1,0x69,0x6b,0xb4,0xd1,0x11,0xad,0x7a,0x00,0xc0,0x4f,0xc2,0x9b,0x4e };
static int iid_is(uint32_t p, const uint8_t *g) { return p && !memcmp(GPTR(p), g, 16); }
static void iid_log(const char *who, uint32_t p) {
    const uint8_t *g = GPTR(p);
    LOG("%s: QueryInterface %02x%02x%02x%02x-%02x%02x-%02x%02x-...", who, g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6]);
}

static void d3d_build(void);
static uint32_t dd_QueryInterface(CPU *c) {
    uint32_t riid = ARG(1), out = ARG(2);
    if (iid_is(riid, IID_DD7)) { gw32(out, g_dd_obj); return DD_OK; }
    if (iid_is(riid, IID_D3D7)) {
        d3d_build();
        gw32(out, g_d3d_obj); return DD_OK;
    }
    iid_log("IDirectDraw", riid);
    gw32(out, 0);
    return E_NOINTERFACE;
}
static uint32_t dd_SetCooperativeLevel(CPU *c) { LOG("SetCooperativeLevel(%x, %x)", ARG(1), ARG(2)); return DD_OK; }
static uint32_t dd_SetDisplayMode(CPU *c) {
    g_mode_w = ARG(1); g_mode_h = ARG(2); g_mode_bpp = ARG(3);
    LOG("SetDisplayMode %dx%dx%d", g_mode_w, g_mode_h, g_mode_bpp);
    if (gl_ready) {} else plat_create_gl(g_mode_w, g_mode_h);
    int dw, dh; plat_drawable_size(&dw, &dh);
    g_scale = dh / g_mode_h; if (dw / g_mode_w < g_scale) g_scale = dw / g_mode_w;
    if (g_scale < 1) g_scale = 1;
    if (g_scale > 4) g_scale = 4;
    if (getenv("C2_RSCALE")) g_scale = atoi(getenv("C2_RSCALE"));
    LOG("render scale %d", g_scale);
    return DD_OK;
}
/* display modes: the screen's aspect ratio at a few heights (see the registry setup in extra.c);
   640x480 too, which the menus always use */
static const int mode_h[] = { 480, 600, 720, 768, 960 };
int ddraw_wide_width(int h) {
    float a = plat_screen_aspect();
    if (a < 4.0f / 3.0f) a = 4.0f / 3.0f;
    return ((int)(h * a + 4) / 8) * 8;
}
static int modes[6][2];
static void make_modes(void) {
    modes[0][0] = 640; modes[0][1] = 480;
    for (int i = 0; i < 5; i++) { modes[i + 1][0] = ddraw_wide_width(mode_h[i]); modes[i + 1][1] = mode_h[i]; }
}
static uint32_t enum_modes(CPU *c, uint32_t cb, uint32_t ctx, int v7) {
    uint32_t d = gmalloc(124);
    make_modes();
    int nm = sizeof modes / sizeof *modes;
    int ex_w = 0, ex_h = 0;
    if (getenv("C2_EXTRAMODE")) sscanf(getenv("C2_EXTRAMODE"), "%dx%d", &ex_w, &ex_h);
    for (int i = 0; i < nm + (ex_w > 0); i++)
        for (int bpp = 16; bpp <= 32; bpp += 16) {
            int mw = i < nm ? modes[i][0] : ex_w, mh = i < nm ? modes[i][1] : ex_h;
            memset(GPTR(d), 0, 124);
            gw32(d, v7 ? 124 : 108); gw32(d + 4, 0x1006 | 0x8 | 0x40000);
            gw32(d + 12, mw); gw32(d + 8, mh); gw32(d + 16, mw * bpp / 8);
            gw32(d + 24, 60);
            pf_write(d + 72, bpp == 32 ? &PF_X888 : &PF_565);
            if (!rt_invoke(c, cb, 2, d, ctx)) { gfree(d); return DD_OK; }
        }
    gfree(d);
    return DD_OK;
}
static uint32_t dd7_EnumDisplayModes(CPU *c) { return enum_modes(c, ARG(4), ARG(3), 1); }
static uint32_t dd1_EnumDisplayModes(CPU *c) { return enum_modes(c, ARG(4), ARG(3), 0); }
static void caps_write(uint32_t p) {
    uint32_t n = gr32(p); if (n < 316 || n > 380) n = n ? n : 380;
    memset(GPTR(p + 4), 0, n - 4);
    gw32(p + 4, 0x1 | 0x40 | 0x200 | 0x400000 | 0x4000000 | 0x10000000 | 0x80000000u);
    gw32(p + 8, 0x1000 | 0x20000 | 0x80000);
    gw32(p + 12, 0x200);
    gw32(p + 56, 0x400 | 0x100);   /* DDBD_16 | DDBD_24 */
    gw32(p + 60, 64u << 20); gw32(p + 64, 60u << 20);
    gw32(p + 132, 0x1000 | 0x2000 | 0x20000 | 0x4000 | 0x200 | 0x10 | 0x4 | 0x40 | 0x400000);
    if (n >= 380) gw32(p + 364, 0x1000 | 0x2000 | 0x20000 | 0x4000 | 0x200 | 0x10 | 0x4 | 0x40 | 0x400000);
}
static uint32_t dd_GetCaps(CPU *c) { if (ARG(1)) caps_write(ARG(1)); if (ARG(2)) caps_write(ARG(2)); return DD_OK; }
static uint32_t dd_GetDisplayMode(CPU *c) {
    uint32_t d = ARG(1); uint32_t n = gr32(d) >= 124 ? 124 : 108;
    memset(GPTR(d), 0, n); gw32(d, n);
    gw32(d + 4, 0x1006 | 0x8); gw32(d + 12, g_mode_w); gw32(d + 8, g_mode_h); gw32(d + 16, g_mode_w * g_mode_bpp / 8);
    pf_write(d + 72, g_mode_bpp == 32 ? &PF_X888 : &PF_565);
    return DD_OK;
}
static uint32_t dd7_CreateSurface(CPU *c) { return create_surface(c, ARG(1), ARG(2), 1); }
static uint32_t dd1_CreateSurface(CPU *c) { return create_surface(c, ARG(1), ARG(2), 0); }
static uint32_t dd_GetAvailableVidMem(CPU *c) { if (ARG(2)) gw32(ARG(2), 64u << 20); if (ARG(3)) gw32(ARG(3), 60u << 20); return DD_OK; }
static uint32_t dd_GetDeviceIdentifier(CPU *c) {
    uint32_t p = ARG(1);
    memset(GPTR(p), 0, 1024 + 8 + 16 + 16 + 4);
    gstrcpy(p, "gles3.dll", 512); gstrcpy(p + 512, "OpenGL ES 3 accelerator", 512);
    gw32(p + 1024, 0x00010004); gw32(p + 1028, 0x0004000d);
    gw32(p + 1032, 0x10de); gw32(p + 1036, 0x0110);
    return DD_OK;
}
static uint32_t dd_GetMonitorFrequency(CPU *c) { gw32(ARG(1), 60); return DD_OK; }
static uint32_t dd_GetVerticalBlankStatus(CPU *c) { gw32(ARG(1), 1); return DD_OK; }
static uint32_t dd_GetScanLine(CPU *c) { gw32(ARG(1), 0); return DD_OK; }
static uint32_t dd_GetFourCCCodes(CPU *c) { gw32(ARG(1), 0); return DD_OK; }
static uint32_t dd_Release(CPU *c) { return 1; }

typedef struct { int dummy; } Clip;
static uint32_t dd_CreateClipper(CPU *c) { static Clip cl; gw32(ARG(2), com_new(vt_clip, 0, &cl)); return DD_OK; }
typedef struct { uint32_t e[256]; } Pal;
static uint32_t dd_CreatePalette(CPU *c) {
    Pal *p = calloc(1, sizeof *p);
    if (ARG(2)) for (int i = 0; i < 256; i++) p->e[i] = gr32(ARG(2) + 4 * i);
    gw32(ARG(3), com_new(vt_pal, 0, p));
    return DD_OK;
}
static uint32_t pal_GetEntries(CPU *c) { Pal *p = COM_HOST(Pal); for (uint32_t i = 0; i < ARG(3) && ARG(2) + i < 256; i++) gw32(ARG(4) + 4 * i, p->e[ARG(2) + i]); return DD_OK; }
static uint32_t pal_SetEntries(CPU *c) { Pal *p = COM_HOST(Pal); for (uint32_t i = 0; i < ARG(3) && ARG(2) + i < 256; i++) p->e[ARG(2) + i] = gr32(ARG(4) + 4 * i); return DD_OK; }

/* ---------------------------------------------------------------- IDirectDrawSurface7 */
static uint32_t ds_QueryInterface(CPU *c) {
    uint32_t riid = ARG(1), out = ARG(2);
    if (iid_is(riid, IID_GAMMA)) {
        if (!g_gamma_obj) g_gamma_obj = com_new(vt_gamma, 0, NULL);
        gw32(out, g_gamma_obj); return DD_OK;
    }
    iid_log("IDirectDrawSurface", riid);
    gw32(out, 0);
    return E_NOINTERFACE;
}
static uint32_t ds_AddRef(CPU *c) { return ++S()->refs; }
static uint32_t ds_Release(CPU *c) {
    Surf *s = S();
    if (--s->refs > 0) return s->refs;
    TRACE("surface %08x released", s->obj);
    if (s->tex || s->fbo) q_flush();
    if (s->tex) glDeleteTextures(1, &s->tex);
    if (s->fbo) glDeleteFramebuffers(1, &s->fbo);
    if (s->depth) glDeleteRenderbuffers(1, &s->depth);
    s->tex = s->fbo = s->depth = 0;
    if (s->mem) gfree(s->mem);
    s->mem = 0;
    if (g_primary == s) g_primary = NULL;
    return 0;
}
static uint32_t ds_GetSurfaceDesc(CPU *c) { desc_write(S(), ARG(1), 1); return DD_OK; }
static uint32_t ds1_GetSurfaceDesc(CPU *c) { desc_write(S(), ARG(1), 0); return DD_OK; }
static uint32_t ds_GetPixelFormat(CPU *c) {
    Surf *s = S();
    if (s->is_z) { memset(GPTR(ARG(1)), 0, 32); gw32(ARG(1), 32); gw32(ARG(1) + 4, 0x400); gw32(ARG(1) + 12, s->pf.bits); }
    else pf_write(ARG(1), &s->pf);
    return DD_OK;
}
static uint32_t ds_GetCaps(CPU *c) { Surf *s = S(); memset(GPTR(ARG(1)), 0, 16); gw32(ARG(1), s->caps); gw32(ARG(1) + 4, s->caps2); return DD_OK; }
static uint32_t ds1_GetCaps(CPU *c) { gw32(ARG(1), S()->caps); return DD_OK; }
static uint32_t ds_GetAttachedSurface(CPU *c) {
    Surf *s = S(); uint32_t caps = gr32(ARG(1)), out = ARG(2);
    Surf *r = NULL;
    if (caps & 0x4) r = s->back;
    else if (caps & 0x20000) r = s->zbuf;
    else if (caps & 0x400000) r = s->mip;
    else if (caps & 0x20) r = s->parent ? s->parent : NULL;
    if (!r) { TRACE("GetAttachedSurface(%x) of %08x: not found", caps, s->obj); gw32(out, 0); return DDERR_NOTFOUND; }
    r->refs++;
    gw32(out, r->obj);
    return DD_OK;
}
static uint32_t ds_AddAttachedSurface(CPU *c) {
    Surf *s = S(), *a = surf_of(ARG(1));
    if (!a) return DDERR_INVALIDPARAMS;
    if (a->is_z) s->zbuf = a; else s->back = a;
    a->refs++;
    return DD_OK;
}
static uint32_t ds_DeleteAttachedSurface(CPU *c) {
    Surf *s = S(), *a = surf_of(ARG(2));
    if (!a || s->zbuf == a) s->zbuf = NULL;
    return DD_OK;
}
static uint32_t ds_EnumAttachedSurfaces(CPU *c) {
    Surf *s = S();
    Surf *list[3] = { s->back, s->zbuf, s->mip };
    uint32_t d = gmalloc(124);
    for (int i = 0; i < 3; i++) if (list[i]) {
        list[i]->refs++;
        memset(GPTR(d), 0, 124); gw32(d, 124); desc_write(list[i], d, 1);
        if (!rt_invoke(c, ARG(2), 3, list[i]->obj, d, ARG(1))) break;
    }
    gfree(d);
    return DD_OK;
}
static uint32_t ds_IsLost(CPU *c) { return DD_OK; }
static uint32_t ds_GetColorKey(CPU *c) {
    Surf *s = S();
    if (!s->has_ck) return 0x887600edu; /* DDERR_NOCOLORKEY */
    gw32(ARG(2), s->ck_lo); gw32(ARG(2) + 4, s->ck_hi);
    return DD_OK;
}
static uint32_t ds_SetColorKey(CPU *c) {
    Surf *s = S();
    if (!(ARG(1) & 0x8)) return DD_OK;   /* only DDCKEY_SRCBLT matters */
    if (!ARG(2)) s->has_ck = 0;
    else { s->has_ck = 1; s->ck_lo = gr32(ARG(2)); s->ck_hi = gr32(ARG(2) + 4); if (s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo; }
    s->mem_newer = 1;
    return DD_OK;
}
static uint32_t ds_Lock(CPU *c) {
    Surf *s = S();
    uint32_t r = ARG(1), d = ARG(2), fl = ARG(3);
    if (!s->mem) return DDERR_GENERIC;
    if (s->rt) {
        surf_download(s);
        s->lock_rt_write = !(fl & 0x10);
    }
    int x = 0, y = 0;
    if (r) { x = gr32(r); y = gr32(r + 4); }
    desc_write(s, d, gr32(d) >= 124);
    gw32(d + 4, gr32(d + 4) | 0x800);
    gw32(d + 36, s->mem + y * s->pitch + x * s->bpp);
    return DD_OK;
}
static uint32_t ds_Unlock(CPU *c) {
    Surf *s = S();
    s->uniq = g_uniq++;
    if (s->rt) {
        if (s->lock_rt_write && s->fbo) {
            /* push the memory copy back to the framebuffer */
            uint8_t *b = malloc((size_t)s->w * s->h * 4);
            to_rgba(s, 0, 0, s->w, s->h, b, 0);
            static GLuint t;
            if (!t) glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
            free(b);
            draw_quad(s, t, s->w, s->h, 0, 0, 0, s->w, s->h, 0, 0, s->w, s->h, 0, 0);
            s->gpu_newer = 0;
        }
        s->lock_rt_write = 0;
        if (s == g_primary) present(s);
        return DD_OK;
    }
    s->mem_newer = 1;
    if (s->parent && !s->is_z) s->parent->mem_newer = 1;
    return DD_OK;
}

/* CPU blit between memory surfaces (same format; else converted through RGBA) */
static void cpu_blt(Surf *d, int dx0, int dy0, int dx1, int dy1, Surf *s, int sx0, int sy0, int sx1, int sy1, int ck, uint32_t cklo, uint32_t ckhi) {
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int same = d->bpp == s->bpp && d->pf.r == s->pf.r && d->pf.g == s->pf.g && d->pf.b == s->pf.b && d->pf.a == s->pf.a;
    for (int y = 0; y < dh; y++) {
        int yy = dy0 + y; if (yy < 0 || yy >= d->h) continue;
        int sy = sy0 + (int)((int64_t)y * sh / dh); if (sy < 0 || sy >= s->h) continue;
        uint8_t *dr = GPTR(d->mem + yy * d->pitch);
        const uint8_t *sr = GPTR(s->mem + sy * s->pitch);
        if (same && !ck && sw == dw && dx0 >= 0 && dx1 <= d->w && sx0 >= 0 && sx1 <= s->w) {
            memmove(dr + dx0 * d->bpp, sr + sx0 * s->bpp, (size_t)dw * d->bpp);
            continue;
        }
        for (int x = 0; x < dw; x++) {
            int xx = dx0 + x; if (xx < 0 || xx >= d->w) continue;
            int sx = sx0 + (int)((int64_t)x * sw / dw); if (sx < 0 || sx >= s->w) continue;
            uint32_t v = px_get(sr + sx * s->bpp, s->bpp);
            if (ck && v >= cklo && v <= ckhi) continue;
            if (!same) v = pack(ext(v, s->pf.r), ext(v, s->pf.g), ext(v, s->pf.b), s->pf.a ? ext(v, s->pf.a) : 255, &d->pf);
            px_put(dr + xx * d->bpp, d->bpp, v);
        }
    }
}
static void fill_rect(Surf *d, int x0, int y0, int x1, int y1, uint32_t color, int depth) {
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > d->w) x1 = d->w; if (y1 > d->h) y1 = d->h;
    if (x1 <= x0 || y1 <= y0) return;
    if (d->rt || depth) {
        Surf *t = d;
        if (depth) { /* a z buffer is the depth of the render target it is attached to */
            t = NULL;
            if (g_primary && g_primary->back && g_primary->back->zbuf == d) t = g_primary->back;
            else if (g_primary && g_primary->zbuf == d) t = g_primary;
            if (!t) return;
        }
        bind_rt(t);
        glEnable(GL_SCISSOR_TEST);
        glScissor(x0 * g_scale, (t->h - y1) * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
        if (depth) { glDepthMask(GL_TRUE); glClearDepthf(1); glClear(GL_DEPTH_BUFFER_BIT); }
        else {
            const PixFmt *f = &d->pf;
            glColorMask(1, 1, 1, 1);
            glClearColor(ext(color, f->r) / 255.f, ext(color, f->g) / 255.f, ext(color, f->b) / 255.f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            d->gpu_newer = 1;
        }
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    for (int y = y0; y < y1; y++) {
        uint8_t *r = GPTR(d->mem + y * d->pitch);
        for (int x = x0; x < x1; x++) px_put(r + x * d->bpp, d->bpp, color);
    }
    d->mem_newer = 1;
}
static void do_blt(Surf *d, int dx0, int dy0, int dx1, int dy1, Surf *s, int sx0, int sy0, int sx1, int sy1, int ck) {
    if (!s) return;
    if (d->rt) {
        if (s->rt) {
            if (s == d) {
                surf_download(s);
                static GLuint t;
                if (!t) glGenTextures(1, &t);
                uint8_t *b = malloc((size_t)s->w * s->h * 4);
                to_rgba(s, 0, 0, s->w, s->h, b, 0);
                glBindTexture(GL_TEXTURE_2D, t);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s->w, s->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, b);
                free(b);
                draw_quad(d, t, s->w, s->h, 0, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, 0, 0);
            } else {
                q_flush();
                glBindFramebuffer(GL_READ_FRAMEBUFFER, s->fbo);
                glBindFramebuffer(GL_DRAW_FRAMEBUFFER, d->fbo);
                glDisable(GL_SCISSOR_TEST);
                int S = g_scale;
                glBlitFramebuffer(sx0 * S, (s->h - sy0) * S, sx1 * S, (s->h - sy1) * S, dx0 * S, (d->h - dy0) * S, dx1 * S, (d->h - dy1) * S,
                                  GL_COLOR_BUFFER_BIT, GL_NEAREST);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                d->gpu_newer = 1;
            }
        } else {
            GLuint t = surf_texture(s, ck);
            draw_quad(d, t, s->w, s->h, 0, sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, ck && s->has_ck, (sx1 - sx0) != (dx1 - dx0));
        }
        return;
    }
    if (s->rt) surf_download(s);
    cpu_blt(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, ck && s->has_ck, s->ck_lo, s->ck_hi);
    d->mem_newer = 1;
    if (d->parent) d->parent->mem_newer = 1;
}
static void rect_or(uint32_t r, int w, int h, int *x0, int *y0, int *x1, int *y1) {
    if (r) { *x0 = gr32(r); *y0 = gr32(r + 4); *x1 = gr32(r + 8); *y1 = gr32(r + 12); }
    else { *x0 = 0; *y0 = 0; *x1 = w; *y1 = h; }
}
static uint32_t ds_Blt(CPU *c) {
    Surf *d = S(), *s = surf_of(ARG(2));
    uint32_t fl = ARG(4), fx = ARG(5);
    int dx0, dy0, dx1, dy1; rect_or(ARG(1), d->w, d->h, &dx0, &dy0, &dx1, &dy1);
    if (g_frame == g_logframe) LOG("Blt %08x (%d,%d,%d,%d) <- %08x fl=%x", d->obj, dx0, dy0, dx1, dy1, s ? s->obj : 0, fl);
    TRACE("Blt %08x (%d,%d,%d,%d) <- %08x fl=%x", d->obj, dx0, dy0, dx1, dy1, s ? s->obj : 0, fl);
    if (fl & 0x400) fill_rect(d, dx0, dy0, dx1, dy1, gr32(fx + 80), 0);
    else if (fl & 0x2000000) fill_rect(d, dx0, dy0, dx1, dy1, gr32(fx + 80), 1);
    else if (s) {
        int sx0, sy0, sx1, sy1; rect_or(ARG(3), s->w, s->h, &sx0, &sy0, &sx1, &sy1);
        int ck = (fl & 0x8000) != 0;
        if (fl & 0x10000) { s->has_ck = 1; s->ck_lo = gr32(fx + 92); s->ck_hi = gr32(fx + 96); ck = 1; s->mem_newer = 1; }
        do_blt(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, ck);
    }
    if (d == g_primary) present(d);
    return DD_OK;
}
static uint32_t ds_BltFast(CPU *c) {
    Surf *d = S(), *s = surf_of(ARG(3));
    if (!s) return DDERR_INVALIDPARAMS;
    int x = ARG(1), y = ARG(2);
    int sx0, sy0, sx1, sy1; rect_or(ARG(4), s->w, s->h, &sx0, &sy0, &sx1, &sy1);
    do_blt(d, x, y, x + sx1 - sx0, y + sy1 - sy0, s, sx0, sy0, sx1, sy1, (ARG(5) & 1) != 0);
    if (d == g_primary) present(d);
    return DD_OK;
}
extern void plat_vclock_frame(void);
static uint32_t ds_Flip(CPU *c) {
    Surf *s = S();
    Surf *b = s->back;
    if (ARG(1)) b = surf_of(ARG(1));
    if (g_fog_draws >= 8) { if (++g_fog_frames >= 3) g_race_ms = plat_ticks(); } else g_fog_frames = 0;
    g_fog_draws = 0;
    g_hud_phase = 0;
    if (b) present(b);
    plat_vclock_frame();
    g_frame++;
    return DD_OK;
}
static uint32_t ds_GetDC(CPU *c) { LOG("Surface GetDC (unsupported)"); return DDERR_UNSUPPORTED; }
static uint32_t ds_SetPalette(CPU *c) { S()->pal = ARG(1); return DD_OK; }
static uint32_t ds_GetPalette(CPU *c) { Surf *s = S(); if (!s->pal) return 0x887600f5u; gw32(ARG(1), s->pal); return DD_OK; }
static uint32_t ds_GetClipper(CPU *c) { return 0x887600f6u; }
static uint32_t ds_GetUniquenessValue(CPU *c) { gw32(ARG(1), S()->uniq); return DD_OK; }
static uint32_t ds_ChangeUniquenessValue(CPU *c) { S()->uniq = g_uniq++; return DD_OK; }
static uint32_t ds_GetDDInterface(CPU *c) { gw32(ARG(1), g_dd_obj); return DD_OK; }
static uint32_t ds_GetFlipStatus(CPU *c) { return DD_OK; }
static uint32_t ds_GetPriority(CPU *c) { gw32(ARG(1), 0); return DD_OK; }
static uint32_t ds_GetLOD(CPU *c) { gw32(ARG(1), 0); return DD_OK; }

/* gamma */
static uint16_t g_ramp[3][256];
static uint32_t gc_GetGammaRamp(CPU *c) { memcpy(GPTR(ARG(2)), g_ramp, sizeof g_ramp); return DD_OK; }
static uint32_t gc_SetGammaRamp(CPU *c) { memcpy(g_ramp, GPTR(ARG(2)), sizeof g_ramp); return DD_OK; }

/* ================================================================ Direct3D 7 */
static void devdesc_write(uint32_t p, int tnl) {
    memset(GPTR(p), 0, 236);
    gw32(p, 0x1 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x4000 | 0x8000 | 0x10000 | 0x80000 | (tnl ? 0x10000 : 0));
    for (int k = 0; k < 2; k++) {
        uint32_t pc = p + 4 + k * 56;
        gw32(pc, 56);
        gw32(pc + 4, 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40);            /* misc: masking, culls */
        gw32(pc + 8, 0x1 | 0x2 | 0x10 | 0x80 | 0x100 | 0x8000 | 0x10000 | 0x80000 | 0x100000 | 0x200000);   /* raster: dither, zbias, fog... */
        gw32(pc + 12, 0xff); gw32(pc + 16, 0x1fff); gw32(pc + 20, 0x1fff); gw32(pc + 24, 0xff);
        gw32(pc + 28, 0x1 | 0x2 | 0x8 | 0x20 | 0x80 | 0x200 | 0x800 | 0x1000 | 0x2000 | 0x80000 | 0x200000 | 0x800000 | 0x1000000);
        gw32(pc + 32, 0x1 | 0x2 | 0x4 | 0x20 | 0x40 | 0x400 | 0x800 | 0x2000);  /* tex: perspective, pow2, alpha, transparency, ... */
        gw32(pc + 36, 0x3f | 0x200 | 0x2000000 | 0x1000000 | 0x200 | 0x10000 | 0x20000 | 0x100 | 0x200000);
        gw32(pc + 40, 0xff);
        gw32(pc + 44, 0x1 | 0x2 | 0x4 | 0x10);
    }
    gw32(p + 116, 0x400 | 0x100); gw32(p + 120, 0x400 | 0x100);
    gw32(p + 124, 1); gw32(p + 128, 1); gw32(p + 132, 2048); gw32(p + 136, 2048);
    gw32(p + 140, 8192); gw32(p + 144, 2048); gw32(p + 148, 1);
    gwf(p + 152, -2048); gwf(p + 156, -2048); gwf(p + 160, 2048); gwf(p + 164, 2048);
    gw32(p + 176, 0x80000 | 8);   /* FVF caps: DONOTSTRIPELEMENTS, 8 tex coords */
    gw32(p + 180, 0x3ffffff);
    gw16(p + 184, 4); gw16(p + 186, 2);
    gw32(p + 188, 8); gwf(p + 192, 1e10f);
    gw16(p + 212, 0); gw16(p + 214, 0);
    gw32(p + 216, 0x1 | 0x2 | 0x8 | 0x10 | 0x20);
}
/* device GUIDs */
static const uint8_t IID_HAL[16] = { 0xe0,0x3d,0xe6,0x84,0xaa,0x46,0xcf,0x11,0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e };
static const uint8_t IID_TNL[16] = { 0x78,0x9e,0x04,0xf5,0x61,0x48,0xd2,0x11,0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8 };
static uint32_t d3d_EnumDevices(CPU *c) {
    uint32_t cb = ARG(1), ctx = ARG(2);
    uint32_t dd = gmalloc(236), n1 = gstrdup("Direct3D HAL"), n2 = gstrdup("Direct3D T&L HAL"), name = gstrdup("Direct3D HAL");
    devdesc_write(dd, 0); memcpy(GPTR(dd + 196), IID_HAL, 16);
    if (rt_invoke(c, cb, 4, n1, name, dd, ctx)) {
        devdesc_write(dd, 1); memcpy(GPTR(dd + 196), IID_TNL, 16);
        rt_invoke(c, cb, 4, n2, n2, dd, ctx);
    }
    return DD_OK;
}
static uint32_t d3d_EnumZBufferFormats(CPU *c) {
    uint32_t cb = ARG(2), ctx = ARG(3);
    uint32_t p = gmalloc(32);
    static const int bits[][2] = { {16, 0xffff}, {24, 0xffffff}, {32, 0xffffff} };
    for (int i = 0; i < 3; i++) {
        memset(GPTR(p), 0, 32); gw32(p, 32); gw32(p + 4, 0x400); gw32(p + 12, bits[i][0]); gw32(p + 20, bits[i][1]);
        if (!rt_invoke(c, cb, 2, p, ctx)) break;
    }
    return DD_OK;
}

/* ---- device state ---- */
typedef struct {
    uint32_t rs[256];
    uint32_t tss[8][32];
    Surf *tex[8];
    Surf *rt;
    float world[16], view[16], proj[16];
    struct { uint32_t x, y, w, h; float mn, mx; } vp;
    float mat[17];                    /* diffuse 4, ambient 4, specular 4, emissive 4, power */
    struct { uint32_t type; float d[4], s[4], a[4], pos[3], dir[3], range, falloff, att0, att1, att2, theta, phi; int on; } light[16];
    int in_scene;
} Dev;
static Dev dev;

static void ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void dev_defaults(void) {
    memset(&dev, 0, sizeof dev);
    uint32_t *rs = dev.rs;
    rs[4] = 1; rs[7] = 1; rs[8] = 3; rs[9] = 2; rs[14] = 1; rs[16] = 1; rs[19] = 2; rs[20] = 1; rs[22] = 3; rs[23] = 4;
    rs[25] = 8; rs[29] = 0; rs[34] = 0; rs[38] = 0x3f800000u; rs[37] = 0x3f800000u; rs[60] = 0xffffffffu;
    rs[136] = 1; rs[137] = 1; rs[141] = 1; rs[145] = 1; rs[146] = 2;
    for (int i = 0; i < 8; i++) {
        uint32_t *t = dev.tss[i];
        t[1] = i == 0 ? 4 : 1; t[2] = 2; t[3] = 1; t[4] = i == 0 ? 2 : 1; t[5] = 2; t[6] = 1; t[11] = i;
        t[12] = t[13] = t[14] = 1; t[16] = 1; t[17] = 1; t[18] = 1;
    }
    ident(dev.world); ident(dev.view); ident(dev.proj);
}

/* ---- vertex processing ---- */
static GVert *vbuf;
/* the frame's vertex/index queue (batched submission, see q_flush) */
static GVert *qv; static int nqv, qvcap;
static void q_reserve_v(int n) {
    if (nqv + n > qvcap) { qvcap = (nqv + n) * 2 + 4096; qv = realloc(qv, (size_t)qvcap * sizeof(GVert)); }
    vbuf = qv + nqv;
}
static float *wvp_cache; static float wvp[16];
static void matmul(const float *a, const float *b, float *o) {   /* row-vector convention: o = a * b */
    float r[16];
    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++)
        r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, 64);
}
static int fvf_stride(uint32_t fvf) {
    int n = 0;
    switch (fvf & 0xe) { case 2: n = 12; break; case 4: n = 16; break; case 6: n = 16; break; case 8: n = 20; break; case 0xa: n = 24; break; case 0xc: n = 28; break; case 0xe: n = 32; break; }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;   /* D3DFVF_RESERVED1 */
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    int nt = (fvf >> 8) & 0xf;
    for (int i = 0; i < nt; i++) { int f = (fvf >> (16 + 2 * i)) & 3; n += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4; }
    return n;
}
static inline void col_unpack(uint32_t v, uint8_t *o) { o[0] = v >> 16; o[1] = v >> 8; o[2] = v; o[3] = v >> 24; }
static float rsf(int i) { float f; memcpy(&f, &dev.rs[i], 4); return f; }

static float fog_factor(float d) {
    uint32_t mode = dev.rs[35] ? dev.rs[35] : dev.rs[140];
    float st = rsf(36), en = rsf(37), de = rsf(38);
    switch (mode) {
    case 3: return en != st ? (en - d) / (en - st) : 1;   /* linear */
    case 1: return expf(-d * de);
    case 2: return expf(-(d * de) * (d * de));
    }
    return 1;
}

static void light_vertex(const float *p, const float *n, uint32_t vdiff, uint32_t vspec, int hasd, int hass, uint8_t *outd, uint8_t *outs) {
    /* D3D7 fixed-function lighting, in world space (view-independent parts approximated) */
    float wp[3], wn[3];
    const float *W = dev.world;
    for (int i = 0; i < 3; i++) wp[i] = p[0] * W[i] + p[1] * W[4 + i] + p[2] * W[8 + i] + W[12 + i];
    for (int i = 0; i < 3; i++) wn[i] = n ? n[0] * W[i] + n[1] * W[4 + i] + n[2] * W[8 + i] : 0;
    float l = sqrtf(wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2]); if (l > 0) for (int i = 0; i < 3; i++) wn[i] /= l;
    float md[4], ma[4], me[4], ms[4];
    float vd[4] = { (vdiff >> 16 & 255) / 255.f, (vdiff >> 8 & 255) / 255.f, (vdiff & 255) / 255.f, (vdiff >> 24) / 255.f };
    float vs[4] = { (vspec >> 16 & 255) / 255.f, (vspec >> 8 & 255) / 255.f, (vspec & 255) / 255.f, (vspec >> 24) / 255.f };
    int cv = dev.rs[141];
#define SRC(dst, rsidx, mo) { uint32_t src_ = cv ? dev.rs[rsidx] : 0; const float *f_ = src_ == 1 && hasd ? vd : src_ == 2 && hass ? vs : dev.mat + mo; memcpy(dst, f_, 16); }
    SRC(md, 145, 0) SRC(ma, 147, 4) SRC(ms, 146, 8) SRC(me, 148, 12)
    uint32_t amb = dev.rs[139];
    float A[3] = { (amb >> 16 & 255) / 255.f, (amb >> 8 & 255) / 255.f, (amb & 255) / 255.f };
    float r[3], s[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) r[i] = me[i] + ma[i] * A[i];
    for (int k = 0; k < 16; k++) {
        if (!dev.light[k].on) continue;
        typeof(dev.light[0]) *L = &dev.light[k];
        float ld[3], att = 1;
        if (L->type == 3) { for (int i = 0; i < 3; i++) ld[i] = -L->dir[i]; }
        else {
            for (int i = 0; i < 3; i++) ld[i] = L->pos[i] - wp[i];
            float dist = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]);
            if (L->range > 0 && dist > L->range) continue;
            if (dist > 0) for (int i = 0; i < 3; i++) ld[i] /= dist;
            float q = L->att0 + L->att1 * dist + L->att2 * dist * dist;
            att = q > 0 ? 1 / q : 1;
            if (L->type == 2) {
                float sd = -(ld[0] * L->dir[0] + ld[1] * L->dir[1] + ld[2] * L->dir[2]);
                float ct = cosf(L->theta / 2), cp = cosf(L->phi / 2);
                if (sd <= cp) att = 0; else if (sd < ct) att *= powf((sd - cp) / (ct - cp), L->falloff > 0 ? L->falloff : 1);
            }
        }
        float ll = sqrtf(ld[0] * ld[0] + ld[1] * ld[1] + ld[2] * ld[2]); if (ll > 0) for (int i = 0; i < 3; i++) ld[i] /= ll;
        float nd = wn[0] * ld[0] + wn[1] * ld[1] + wn[2] * ld[2];
        for (int i = 0; i < 3; i++) r[i] += att * (ma[i] * L->a[i] + (nd > 0 ? nd : 0) * md[i] * L->d[i]);
        if (nd > 0 && dev.rs[29] && dev.mat[16] > 0) {
            float h[3] = { ld[0], ld[1] + 1, ld[2] };   /* non-local viewer approximation */
            float hl = sqrtf(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
            float hd = hl > 0 ? (wn[0] * h[0] + wn[1] * h[1] + wn[2] * h[2]) / hl : 0;
            if (hd > 0) { float p_ = powf(hd, dev.mat[16]); for (int i = 0; i < 3; i++) s[i] += att * p_ * ms[i] * L->s[i]; }
        }
    }
    for (int i = 0; i < 3; i++) { float v = r[i]; outd[i] = v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255); }
    outd[3] = md[3] <= 0 ? 0 : md[3] >= 1 ? 255 : (uint8_t)(md[3] * 255);
    for (int i = 0; i < 3; i++) { float v = s[i]; outs[i] = v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255); }
    outs[3] = hass ? vspec >> 24 : 255;
}

/* D3D clips z to [0, w], GL to [-w, w] (z' = 2z - w). Geometry exactly on D3D's near or far plane (2D
   overlays at z = 0) would sit on GL's clip boundary and be dropped, so pull z very slightly inside. */
#define ZIN 0.99999f
/* widescreen: how much the game stretched the 4:3 layout (1 at 4:3 and in the 640x480 menus) */
static float wide_k(void) {
    float W = dev.rt ? dev.rt->w : g_mode_w, H = dev.rt ? dev.rt->h : g_mode_h;
    return W * 3 > H * 4 ? (4.0f / 3.0f) / (W / H) : 1.0f;
}
static int wide_subvp(void) {
    float W = dev.rt ? dev.rt->w : g_mode_w, H = dev.rt ? dev.rt->h : g_mode_h;
    return dev.vp.w && (dev.vp.w != (uint32_t)W || dev.vp.h != (uint32_t)H || dev.vp.x || dev.vp.y);
}
/* the viewport with the game's stretch undone (sub-viewports: the checkpoint arrow, the menu car) */
static void wide_vp(float *x, float *y, float *w, float *h) {
    float W = dev.rt ? dev.rt->w : g_mode_w, H = dev.rt ? dev.rt->h : g_mode_h;
    *x = dev.vp.x; *y = dev.vp.y; *w = dev.vp.w ? dev.vp.w : W; *h = dev.vp.h ? dev.vp.h : H;
    float k = wide_k();
    if (k != 1 && wide_subvp()) { *x = W / 2 + (*x - W / 2) * k; *w *= k; }
}
/* 2D quads into the centre 4:3 area; ones covering (nearly) the whole width (fades, backdrops) stay */
static void wide_flat(int n) {
    float k = wide_k();
    if (k == 1 || wide_subvp()) return;   /* inside a sub-viewport the viewport itself is corrected */
    int step = n % 4 == 0 ? 4 : n;
    for (int q = 0; q < n; q += step) {
        float a = 1e9f, b = -1e9f;
        for (int i = q; i < q + step; i++) { float x = vbuf[i].w != 0 ? vbuf[i].x / vbuf[i].w : 0; if (x < a) a = x; if (x > b) b = x; }
        if (b - a > 1.9f) continue;
        for (int i = q; i < q + step; i++) vbuf[i].x *= k;
    }
}
static int g_fvf_logged[64]; static uint32_t g_fvf_seen[64]; static int g_nfvf;
static void process_(uint32_t fvf, uint32_t base, int stride, int first, int n);
/* vertex processing is the renderer's work: counted with GL in the performance readout */
static void process(uint32_t fvf, uint32_t base, int stride, int first, int n) {
    uint64_t t0 = now_us();
    process_(fvf, base, stride, first, n);
    perf_gl_us += now_us() - t0;
}
static void process_(uint32_t fvf, uint32_t base, int stride, int first, int n) {
    q_reserve_v(n);
    {   int k; for (k = 0; k < g_nfvf && g_fvf_seen[k] != fvf; k++) {}
        if (k == g_nfvf && g_nfvf < 64) { g_fvf_seen[g_nfvf++] = fvf; LOG("d3d: vertex format %x (stride %d)", fvf, stride); } }
    int rhw = (fvf & 0xe) == 4;
    int pos_n = (fvf & 0xe) == 2 ? 3 : rhw ? 4 : 3 + (((fvf & 0xe) - 4) / 2);
    int off_n = pos_n * 4, off = off_n;
    int hasn = fvf & 0x10; if (hasn) off += 12;
    if (fvf & 0x20) off += 4;
    int offd = -1, offs = -1;
    if (fvf & 0x40) { offd = off; off += 4; }
    if (fvf & 0x80) { offs = off; off += 4; }
    int nt = (fvf >> 8) & 0xf, offt[8];
    for (int i = 0; i < nt && i < 8; i++) { offt[i] = off; int f = (fvf >> (16 + 2 * i)) & 3; off += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4; }
    int tci0 = dev.tss[0][11] & 7, tci1 = dev.tss[1][11] & 7;
    float W = dev.rt ? dev.rt->w : g_mode_w, H = dev.rt ? dev.rt->h : g_mode_h;
    int lighting = !rhw && dev.rs[137];
    float wv[16];
    /* Widescreen. The game lays races out for 4:3 and stretches them to the screen width: its perspective
       projections are always 4:3, its 2D HUD and HUD viewports are scaled by width/640. Undo the stretch:
       - full-screen perspective draws (world, HUD models): horizontal scale from the real aspect,
         keeping the vertical field of view (Hor+)
       - 2D draws and sub-viewports: un-stretched into the centre 4:3 area (see wide_k / wide_vp)
       so the HUD keeps its 4:3 layout in the middle and lines up with the 3D view (name tags). */
    int flat = rhw;
    if (!rhw) {
        float pr[16]; memcpy(pr, dev.proj, 64);
        int persp = pr[11] != 0 && pr[0] != pr[5];
        if (!persp) flat = 1;
        if (persp && fabsf(pr[0] * 4 - pr[5] * 3) < 1e-3f * fabsf(pr[5]) && !wide_subvp() && W * 3 != H * 4)
            pr[0] = pr[5] * H / W;
        /* a sub-viewport is un-stretched as a whole (wide_vp), so whatever is drawn in it stays 4:3 */
        if (persp && wide_subvp() && W * 3 != H * 4) pr[0] = pr[5] * 0.75f;
        matmul(dev.world, dev.view, wv); matmul(wv, pr, wvp);
    }
    int fogon = dev.rs[28];
    int tablefog = fogon && dev.rs[35];
    int vertfog = fogon && !dev.rs[35] && dev.rs[140];
    for (int i = 0; i < n; i++) {
        const uint8_t *v = GPTR(base + (uint32_t)(first + i) * stride);
        const float *p = (const float *)v;
        GVert *o = &vbuf[i];
        float eyez = 0;
        if (rhw) {
            float rw = p[3]; float w = rw != 0 ? 1.0f / rw : 1.0f;
            if (!(w > 0) || w > 1e30f) w = 1.0f;
            float x = (p[0] / W) * 2 - 1, y = 1 - (p[1] / H) * 2, z = (p[2] * 2 - 1) * ZIN;
            o->x = x * w; o->y = y * w; o->z = z * w; o->w = w;
            eyez = w;
        } else {
            float x = p[0], y = p[1], z = p[2];
            float cx = x * wvp[0] + y * wvp[4] + z * wvp[8] + wvp[12];
            float cy = x * wvp[1] + y * wvp[5] + z * wvp[9] + wvp[13];
            float cz = x * wvp[2] + y * wvp[6] + z * wvp[10] + wvp[14];
            float cw = x * wvp[3] + y * wvp[7] + z * wvp[11] + wvp[15];
            /* viewport: D3D maps clip x/y to the viewport rectangle */
            float vpx, vpy, vx, vy; wide_vp(&vpx, &vpy, &vx, &vy);
            float sx = vx / W, sy = vy / H, ox = (vpx + vx / 2) / W * 2 - 1, oy = 1 - (vpy + vy / 2) / H * 2;
            o->x = cx * sx + ox * cw; o->y = cy * sy + oy * cw; o->z = (cz * 2 - cw) * ZIN; o->w = cw;
            eyez = x * wv[2] + y * wv[6] + z * wv[10] + wv[14];
        }
        uint32_t dc = offd >= 0 ? *(const uint32_t *)(v + offd) : 0xffffffffu;
        uint32_t sc = offs >= 0 ? *(const uint32_t *)(v + offs) : 0xff000000u;
        if (lighting) light_vertex(p, hasn ? (const float *)(v + off_n) : NULL, dc, sc, offd >= 0, offs >= 0, o->c, o->s);
        else { col_unpack(dc, o->c); col_unpack(sc, o->s); }
        if (tablefog || vertfog) {
            float d = eyez; if (d < 0) d = -d;
            if (rhw && tablefog) d = dev.rs[35] == 3 ? (rsf(36) <= 1.0f && rsf(37) <= 1.0f ? p[2] : d) : d;
            o->fog = fog_factor(d);
        } else o->fog = fogon ? o->s[3] / 255.f : 1;
        if (tci0 < nt) { const float *t = (const float *)(v + offt[tci0]); o->u0 = t[0]; o->v0 = t[1]; } else o->u0 = o->v0 = 0;
        if (tci1 < nt) { const float *t = (const float *)(v + offt[tci1]); o->u1 = t[0]; o->v1 = t[1]; } else o->u1 = o->v1 = 0;
    }
    if (flat && g_frame == g_logframe && n % 4 == 0)
        for (int q = 0; q < n; q += 4) {
            float a = 1e9f, b = -1e9f, c0 = 1e9f, c1 = -1e9f;
            for (int i = q; i < q + 4; i++) { float x = vbuf[i].x / vbuf[i].w, y = vbuf[i].y / vbuf[i].w; if (x < a) a = x; if (x > b) b = x; if (y < c0) c0 = y; if (y > c1) c1 = y; }
            LOG("quad2d tex %08x px x %.1f..%.1f y %.1f..%.1f", dev.tex[0] ? dev.tex[0]->obj : 0, (a + 1) * W / 2, (b + 1) * W / 2, (1 - c1) * H / 2, (1 - c0) * H / 2);
        }
    if (flat) wide_flat(n);
}

static const GLenum BLEND[] = { GL_ONE, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
    GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
static const GLenum CMP[] = { GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS };

/* ---------------------------------------------------------------- batched submission
   Draws are recorded (state + index range into one frame-wide vertex/index array) and submitted
   together at the next sync point (present, clear, blit, readback, texture upload): one vertex and one
   index upload, consecutive draws with the same state merged into one call (strips and fans become
   triangle lists), and only the GL state that changes between draws is touched. Each texture-stage
   setup gets its own fragment shader with the setup compiled in (no per-pixel branching on it). */
typedef struct {
    Surf *rt;
    int var;
    GLuint tex[4], samp[4];
    uint8_t zon, zwrite, blend, cull, scis, pad[3];
    GLenum zfunc, bs, bd;
    int32_t zbias;
    int sc[4];
    float dmn, dmx;
    uint32_t tfactor, fogcol;
    float aref;
} DState;
typedef struct { DState st; GLenum prim; uint32_t first, count; } DRec;
static uint32_t *qi; static int nqi, qicap;
static DRec *qr; static int nqr, qrcap;
static int g_flushes;

/* fragment shader variants */
typedef struct { uint8_t key[40]; GLuint prog; GLint u_tf, u_fc, u_ar; uint32_t tf, fc; float ar; int set; } Variant;
static Variant var_tab[256];
static int nvar;
static int variant(const uint8_t *key) {
    static int last = -1;
    if (last >= 0 && !memcmp(var_tab[last].key, key, 40)) return last;
    for (int i = 0; i < nvar; i++) if (!memcmp(var_tab[i].key, key, 40)) return last = i;
    if (nvar >= 256) return last = 0;
    Variant *v = &var_tab[nvar];
    memcpy(v->key, key, 40);
    const uint8_t *k = key;   /* 4 x (top ca1 ca2 aop aa1 aa2 hastex), nstage, fogon, atest, ckon, spec */
    char *src = malloc(16384), *o = src;
    o += sprintf(o, "#version 300 es\nprecision mediump float;\n");
    static const char *nm[7] = { "u_top", "u_ca1", "u_ca2", "u_aop", "u_aa1", "u_aa2", "u_hastex" };
    for (int f = 0; f < 7; f++) o += sprintf(o, "const int %s[4] = int[4](%d, %d, %d, %d);\n", nm[f], k[f], k[7 + f], k[14 + f], k[21 + f]);
    o += sprintf(o, "const int u_tci[4] = int[4](0, 1, 1, 1);\nconst int u_nstage = %d, u_fogon = %d, u_atest = %d, u_ckon = %d, u_spec = %d;\n",
                 k[28], k[29], k[30], k[31], k[32]);
    strcpy(o, FS3D_BODY);
    v->prog = link(VS3D, src);
    free(src);
    glUseProgram(v->prog);
    char n[16];
    for (int i = 0; i < 4; i++) { snprintf(n, sizeof n, "u_t%d", i); glUniform1i(glGetUniformLocation(v->prog, n), i); }
    v->u_tf = glGetUniformLocation(v->prog, "u_tfactor"); v->u_fc = glGetUniformLocation(v->prog, "u_fogcol");
    v->u_ar = glGetUniformLocation(v->prog, "u_aref");
    TRACE("shader variant %d", nvar);
    return last = nvar++;
}
/* sampler objects (filter / wrap combinations) */
static struct { int key; GLuint s; } samp_tab[64];
static int nsamp;
static GLuint sampler(GLenum mag, GLenum min, GLenum ws, GLenum wt) {
    int mi = min == GL_NEAREST ? 0 : min == GL_LINEAR ? 1 : min == GL_NEAREST_MIPMAP_NEAREST ? 2 : min == GL_LINEAR_MIPMAP_NEAREST ? 3
           : min == GL_NEAREST_MIPMAP_LINEAR ? 4 : 5;
    int key = (mag == GL_LINEAR) | mi << 1 | (ws == GL_REPEAT ? 0 : ws == GL_MIRRORED_REPEAT ? 1 : 2) << 4 | (wt == GL_REPEAT ? 0 : wt == GL_MIRRORED_REPEAT ? 1 : 2) << 6;
    for (int i = 0; i < nsamp; i++) if (samp_tab[i].key == key) return samp_tab[i].s;
    GLuint sm; glGenSamplers(1, &sm);
    glSamplerParameteri(sm, GL_TEXTURE_MAG_FILTER, mag); glSamplerParameteri(sm, GL_TEXTURE_MIN_FILTER, min);
    glSamplerParameteri(sm, GL_TEXTURE_WRAP_S, ws); glSamplerParameteri(sm, GL_TEXTURE_WRAP_T, wt);
    if (nsamp < 64) { samp_tab[nsamp].key = key; samp_tab[nsamp].s = sm; nsamp++; }
    return sm;
}

/* the state a draw needs, from the D3D device state */
static void make_state(DState *st) {
    memset(st, 0, sizeof *st);
    Surf *rt = dev.rt;
    uint32_t *rs = dev.rs;
    st->rt = rt;
    st->zon = rs[7] != 0; st->zfunc = st->zon ? CMP[rs[23] <= 8 ? rs[23] : 4] : GL_ALWAYS; st->zwrite = rs[14] != 0;
    if (rs[27]) {
        uint32_t sb = rs[19] <= 13 ? rs[19] : 2, db = rs[20] <= 13 ? rs[20] : 1;
        st->blend = 1; st->bs = BLEND[sb]; st->bd = BLEND[db];
        if (sb == 12) { st->bs = GL_SRC_ALPHA; st->bd = GL_ONE_MINUS_SRC_ALPHA; }
        if (sb == 13) { st->bs = GL_ONE_MINUS_SRC_ALPHA; st->bd = GL_SRC_ALPHA; }
    }
    /* culling: D3DCULL_CCW (3) removes what GL sees as front faces here (verified on the menu) */
    st->cull = rs[22] == 1 ? 0 : rs[22] == 3 ? 1 : 2;
    st->zbias = rs[47];
    uint8_t key[40]; memset(key, 0, sizeof key);
    int ns = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t *t = dev.tss[i];
        if (t[1] == 1) break;
        ns = i + 1;
        uint8_t *k = key + 7 * i;
        k[0] = t[1]; k[1] = t[2]; k[2] = t[3]; k[3] = t[4]; k[4] = t[5]; k[5] = t[6];
        Surf *tx = dev.tex[i];
        k[6] = tx != NULL;
        if (tx) {
            st->tex[i] = surf_texture(tx, rs[41]);
            GLenum mag = t[16] == 1 ? GL_NEAREST : GL_LINEAR; int minf = t[17] != 1, mip = tx->mip && !tx->rt ? t[18] : 1;
            GLenum mn = mip <= 1 ? (minf ? GL_LINEAR : GL_NEAREST) : mip == 2 ? (minf ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST)
                                                                             : (minf ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
            static const GLenum AD[] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE };
            st->samp[i] = sampler(mag, mn, AD[t[13] <= 4 ? t[13] : 1], AD[t[14] <= 4 ? t[14] : 1]);
        }
    }
    key[28] = ns; key[29] = rs[28] != 0; key[30] = rs[15] ? (uint8_t)rs[25] : 0;
    key[31] = rs[41] && dev.tex[0] && dev.tex[0]->has_ck; key[32] = rs[29] != 0;
    st->var = variant(key);
    st->tfactor = rs[60]; st->fogcol = rs[34] & 0xffffff; st->aref = (rs[24] & 255) / 255.f;
    /* D3D viewports are applied in the vertex transform; a partial one is clipped with the scissor */
    if (dev.vp.w && (dev.vp.w != (uint32_t)rt->w || dev.vp.h != (uint32_t)rt->h || dev.vp.x || dev.vp.y)) {
        float vx, vy, vw, vh; wide_vp(&vx, &vy, &vw, &vh);
        st->scis = 1;
        st->sc[0] = (int)(vx * g_scale); st->sc[1] = (int)((rt->h - vy - vh) * g_scale);
        st->sc[2] = (int)(vw * g_scale + 0.5f); st->sc[3] = (int)(vh * g_scale + 0.5f);
    }
    st->dmn = dev.vp.mx > dev.vp.mn ? dev.vp.mn : 0; st->dmx = dev.vp.mx > dev.vp.mn ? dev.vp.mx : 1;
}

/* GL state cache: apply only what differs from the previous draw of this flush */
static DState cur; static int cur_ok;
static void apply(const DState *s) {
    int all = !cur_ok;
    if (all || s->rt != cur.rt) { glBindFramebuffer(GL_FRAMEBUFFER, s->rt->fbo); glViewport(0, 0, s->rt->tw, s->rt->th); }
    Variant *v = &var_tab[s->var];
    if (all || s->var != cur.var) glUseProgram(v->prog);
    if (!v->set || v->tf != s->tfactor) { uint32_t tf = s->tfactor; v->tf = tf;
        glUniform4f(v->u_tf, (tf >> 16 & 255) / 255.f, (tf >> 8 & 255) / 255.f, (tf & 255) / 255.f, (tf >> 24) / 255.f); }
    if (!v->set || v->fc != s->fogcol) { uint32_t fc = s->fogcol; v->fc = fc;
        glUniform3f(v->u_fc, (fc >> 16 & 255) / 255.f, (fc >> 8 & 255) / 255.f, (fc & 255) / 255.f); }
    if (!v->set || v->ar != s->aref) { v->ar = s->aref; glUniform1f(v->u_ar, s->aref); }
    v->set = 1;
    for (int i = 0; i < 4; i++) {
        if (all || s->tex[i] != cur.tex[i]) { glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, s->tex[i]); }
        if (all || s->samp[i] != cur.samp[i]) glBindSampler(i, s->samp[i]);
    }
    if (all || s->zon != cur.zon) { if (s->zon) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST); }
    if (all || s->zfunc != cur.zfunc) glDepthFunc(s->zfunc);
    if (all || s->zwrite != cur.zwrite) glDepthMask(s->zwrite ? GL_TRUE : GL_FALSE);
    if (all || s->blend != cur.blend) { if (s->blend) glEnable(GL_BLEND); else glDisable(GL_BLEND); }
    if (s->blend && (all || s->bs != cur.bs || s->bd != cur.bd || !cur.blend)) glBlendFunc(s->bs, s->bd);
    if (all || s->cull != cur.cull) {
        if (!s->cull) glDisable(GL_CULL_FACE);
        else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(s->cull == 1 ? GL_FRONT : GL_BACK); }
    }
    if (all || s->zbias != cur.zbias) {
        if (s->zbias) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(-1.0f, -(float)s->zbias); } else glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (all || s->scis != cur.scis || (s->scis && memcmp(s->sc, cur.sc, sizeof s->sc))) {
        if (s->scis) { glEnable(GL_SCISSOR_TEST); glScissor(s->sc[0], s->sc[1], s->sc[2], s->sc[3]); } else glDisable(GL_SCISSOR_TEST);
    }
    if (all || s->dmn != cur.dmn || s->dmx != cur.dmx) glDepthRangef(s->dmn, s->dmx);
    cur = *s; cur_ok = 1;
}

static void q_flush(void) {
    if (!nqr) { nqv = nqi = 0; return; }
    uint64_t t0 = now_us();
    g_flushes++;
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nqv * sizeof(GVert), qv, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)nqi * 4, qi, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 4, GL_FLOAT, 0, sizeof(GVert), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, 1, sizeof(GVert), (void *)offsetof(GVert, c));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, 1, sizeof(GVert), (void *)offsetof(GVert, s));
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 2, GL_FLOAT, 0, sizeof(GVert), (void *)offsetof(GVert, u0));
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 2, GL_FLOAT, 0, sizeof(GVert), (void *)offsetof(GVert, u1));
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 1, GL_FLOAT, 0, sizeof(GVert), (void *)offsetof(GVert, fog));
    glColorMask(1, 1, 1, 1);
    cur_ok = 0;
    for (int i = 0; i < nqr; i++) {
        apply(&qr[i].st);
        glDrawElements(qr[i].prim, qr[i].count, GL_UNSIGNED_INT, (void *)((size_t)qr[i].first * 4));
    }
    g_ndraws += nqr;
    /* leave neutral state for the other GL users (2D blits, the overlay): no samplers, unit 0 */
    for (int i = 0; i < 4; i++) glBindSampler(i, 0);
    glActiveTexture(GL_TEXTURE0);
    glDisable(GL_POLYGON_OFFSET_FILL); glDepthRangef(0, 1);
    nqv = nqi = nqr = 0;
    perf_gl_us += now_us() - t0;
}

/* C2_D3DLOG=frame: log every draw of that frame (and the transforms) */
static void log_draw(const char *what, uint32_t type, int nv, int ni) {
    if (g_logframe < 0) { const char *e = getenv("C2_D3DLOG"); g_logframe = e ? atoi(e) : 0; }
    if (g_frame != g_logframe) return;
    uint32_t *r = dev.rs;
    LOG("draw %s type %u nv %d ni %d tex0 %08x(%dx%d ck%d) z%u zw%u zf%u blend%u %u/%u at%u/%u ref%u cull%u light%u fog%u ck%u op0 %u/%x/%x a %u/%x/%x op1 %u",
        what, type, nv, ni, dev.tex[0] ? dev.tex[0]->obj : 0, dev.tex[0] ? dev.tex[0]->w : 0, dev.tex[0] ? dev.tex[0]->h : 0, dev.tex[0] ? dev.tex[0]->has_ck : 0,
        r[7], r[14], r[23], r[27], r[19], r[20], r[15], r[25], r[24], r[22], r[137], r[28], r[41],
        dev.tss[0][1], dev.tss[0][2], dev.tss[0][3], dev.tss[0][4], dev.tss[0][5], dev.tss[0][6], dev.tss[1][1]);
    if (dev.tex[0]) {
        Surf *t = dev.tex[0];
        LOG("   tex fmt bits %u masks %x %x %x %x flags %x mip %p rt %d px0 %08x px(128,128) %08x", t->pf.bits, t->pf.r, t->pf.g, t->pf.b, t->pf.a, t->pf.flags,
            (void *)t->mip, t->rt, px_get(GPTR(t->mem), t->bpp), px_get(GPTR(t->mem + 128 * t->pitch + 128 * t->bpp), t->bpp));
        static int dumped;
        if (!dumped++) {
            uint8_t *b = malloc((size_t)t->w * t->h * 4); to_rgba(t, 0, 0, t->w, t->h, b, 0);
            char p[1200]; snprintf(p, sizeof p, "%s/tex.ppm", g_save_dir);
            FILE *f = fopen(p, "wb"); fprintf(f, "P6\n%d %d\n255\n", t->w, t->h);
            for (int i = 0; i < t->w * t->h; i++) fwrite(b + i * 4, 1, 3, f);
            fclose(f); free(b);
        }
    }
    LOG("   proj %.3f %.3f  view row3 %.2f %.2f %.2f  world row3 %.2f %.2f %.2f  vp %u,%u %ux%u", dev.proj[0], dev.proj[5], dev.view[12], dev.view[13], dev.view[14],
        dev.world[12], dev.world[13], dev.world[14], dev.vp.x, dev.vp.y, dev.vp.w, dev.vp.h);
    if (nv > 0) LOG("   v0 clip %.2f %.2f %.2f %.2f col %02x%02x%02x%02x uv %.2f %.2f", vbuf[0].x, vbuf[0].y, vbuf[0].z, vbuf[0].w,
        vbuf[0].c[0], vbuf[0].c[1], vbuf[0].c[2], vbuf[0].c[3], vbuf[0].u0, vbuf[0].v0);
}
static const GLenum PRIM[] = { 0, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN };
int d3d_racing(void) { return g_race_ms && plat_ticks() - g_race_ms < 400; }
static void q_reserve_i(int n) { if (nqi + n > qicap) { qicap = (nqi + n) * 2 + 8192; qi = realloc(qi, (size_t)qicap * 4); } }
/* record a draw of the nv vertices just processed (at vbuf); idx: guest u16 indices (minus lo) or 0 */
static void draw(uint32_t type, int nv, uint32_t idx, int ni, int lo) {
    if (type < 1 || type > 6 || nv <= 0 || !dev.rt) return;
    if (dev.rs[28]) g_fog_draws++;
    log_draw("", type, nv, ni);
    uint64_t t0 = now_us();
    int vstart = (int)(vbuf - qv), fl = g_flushes;
    DState st; make_state(&st);   /* may upload a texture, which flushes the queue */
    if (fl != g_flushes) { memmove(qv + nqv, qv + vstart, (size_t)nv * sizeof(GVert)); vstart = nqv; }
    uint32_t base = vstart;
    nqv = vstart + nv;
    int m = idx ? ni : nv;
    const u16u *ix = idx ? (const u16u *)GPTR(idx) : NULL;
#define SRC(i) (base + (ix ? (uint32_t)(ix[i] - lo) : (uint32_t)(i)))
    GLenum prim = GL_TRIANGLES;
    uint32_t first = nqi;
    switch (type) {
    case 4: q_reserve_i(m); for (int i = 0; i + 2 < m; i += 3) { qi[nqi++] = SRC(i); qi[nqi++] = SRC(i + 1); qi[nqi++] = SRC(i + 2); } break;
    case 5: q_reserve_i(m * 3); for (int i = 0; i + 2 < m; i++) {   /* strip: odd triangles swap their first two (GL's order) */
                if (i & 1) { qi[nqi++] = SRC(i + 1); qi[nqi++] = SRC(i); } else { qi[nqi++] = SRC(i); qi[nqi++] = SRC(i + 1); }
                qi[nqi++] = SRC(i + 2); } break;
    case 6: q_reserve_i(m * 3); for (int i = 1; i + 1 < m; i++) { qi[nqi++] = SRC(0); qi[nqi++] = SRC(i); qi[nqi++] = SRC(i + 1); } break;
    case 1: prim = GL_POINTS; q_reserve_i(m); for (int i = 0; i < m; i++) qi[nqi++] = SRC(i); break;
    case 2: prim = GL_LINES; q_reserve_i(m); for (int i = 0; i + 1 < m; i += 2) { qi[nqi++] = SRC(i); qi[nqi++] = SRC(i + 1); } break;
    case 3: prim = GL_LINES; q_reserve_i(m * 2); for (int i = 0; i + 1 < m; i++) { qi[nqi++] = SRC(i); qi[nqi++] = SRC(i + 1); } break;
    }
#undef SRC
    uint32_t cnt = nqi - first;
    if (cnt) {
        DRec *l = nqr ? &qr[nqr - 1] : NULL;
        if (l && l->prim == prim && prim == GL_TRIANGLES && l->first + l->count == first && !memcmp(&l->st, &st, sizeof st)) l->count += cnt;
        else {
            if (nqr >= qrcap) { qrcap = qrcap * 2 + 256; qr = realloc(qr, (size_t)qrcap * sizeof(DRec)); }
            qr[nqr].st = st; qr[nqr].prim = prim; qr[nqr].first = first; qr[nqr].count = cnt; nqr++;
        }
    }
    dev.rt->gpu_newer = 1;
    g_nrecorded++;
    perf_gl_us += now_us() - t0;
}
/* indices relative to 'start'; returns the vertex range actually needed */
static void idx_range(uint32_t idx, int ni, int *lo, int *hi) {
    int a = 65535, b = 0;
    for (int i = 0; i < ni; i++) { int v = gr16(idx + 2 * i); if (v < a) a = v; if (v > b) b = v; }
    *lo = ni ? a : 0; *hi = ni ? b : -1;
}
#define DEV_OK DD_OK
static uint32_t dv_DrawPrimitive(CPU *c) {
    uint32_t type = ARG(1), fvf = ARG(2), v = ARG(3), n = ARG(4);
    process(fvf, v, fvf_stride(fvf), 0, n);
    draw(type, n, 0, 0, 0);
    return DEV_OK;
}
static uint32_t dv_DrawIndexedPrimitive(CPU *c) {
    uint32_t type = ARG(1), fvf = ARG(2), v = ARG(3), idx = ARG(5), ni = ARG(6);
    int lo, hi; idx_range(idx, ni, &lo, &hi);
    process(fvf, v, fvf_stride(fvf), lo, hi - lo + 1);
    draw(type, hi - lo + 1, idx, ni, lo);
    return DEV_OK;
}
static uint32_t dv_DrawPrimitiveVB(CPU *c) {
    VB *b = (VB *)com_host(ARG(2));
    uint32_t type = ARG(1), start = ARG(3), n = ARG(4);
    process(b->fvf, b->mem, b->stride, start, n);
    draw(type, n, 0, 0, 0);
    return DEV_OK;
}
static uint32_t dv_DrawIndexedPrimitiveVB(CPU *c) {
    VB *b = (VB *)com_host(ARG(2));
    uint32_t type = ARG(1), start = ARG(3), idx = ARG(5), ni = ARG(6);
    int lo, hi; idx_range(idx, ni, &lo, &hi);
    process(b->fvf, b->mem, b->stride, start + lo, hi - lo + 1);
    draw(type, hi - lo + 1, idx, ni, lo);
    return DEV_OK;
}
/* strided: gather into a contiguous temporary */
static uint32_t gather_strided(uint32_t fvf, uint32_t sd, int first, int n, int *stride_out) {
    int stride = fvf_stride(fvf);
    static uint32_t tmp; static int cap;
    if (n * stride > cap) { if (tmp) gfree(tmp); cap = n * stride + 65536; tmp = gmalloc(cap); }
    /* D3DDRAWPRIMITIVESTRIDEDDATA: position, normal, diffuse, specular, textureCoords[8]; each {ptr, stride} */
    int nt = (fvf >> 8) & 0xf;
    for (int i = 0; i < n; i++) {
        uint8_t *o = GPTR(tmp + i * stride);
        int k = 0;
        int pn = (fvf & 0xe) == 4 ? 16 : 12;
        memcpy(o, GPTR(gr32(sd) + (first + i) * gr32(sd + 4)), pn); k += pn;
        if (fvf & 0x10) { memcpy(o + k, GPTR(gr32(sd + 8) + (first + i) * gr32(sd + 12)), 12); k += 12; }
        if (fvf & 0x40) { memcpy(o + k, GPTR(gr32(sd + 16) + (first + i) * gr32(sd + 20)), 4); k += 4; }
        if (fvf & 0x80) { memcpy(o + k, GPTR(gr32(sd + 24) + (first + i) * gr32(sd + 28)), 4); k += 4; }
        for (int t = 0; t < nt; t++) {
            int f = (fvf >> (16 + 2 * t)) & 3, sz = f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4;
            memcpy(o + k, GPTR(gr32(sd + 32 + 8 * t) + (first + i) * gr32(sd + 36 + 8 * t)), sz); k += sz;
        }
    }
    *stride_out = stride;
    return tmp;
}
static uint32_t dv_DrawPrimitiveStrided(CPU *c) {
    int st; uint32_t fvf = ARG(2), n = ARG(4);
    uint32_t t = gather_strided(fvf, ARG(3), 0, n, &st);
    process(fvf & ~0x20u, t, st, 0, n);
    draw(ARG(1), n, 0, 0, 0);
    return DEV_OK;
}
static uint32_t dv_DrawIndexedPrimitiveStrided(CPU *c) {
    int st; uint32_t fvf = ARG(2), idx = ARG(5), ni = ARG(6);
    int lo, hi; idx_range(idx, ni, &lo, &hi);
    uint32_t t = gather_strided(fvf, ARG(3), lo, hi - lo + 1, &st);
    process(fvf & ~0x20u, t, st, 0, hi - lo + 1);
    draw(ARG(1), hi - lo + 1, idx, ni, lo);
    return DEV_OK;
}
static uint32_t dv_Clear(CPU *c) {
    uint32_t n = ARG(1), rects = ARG(2), fl = ARG(3), col = ARG(4);
    float z = ARGF(5);
    if (!dev.rt) return DEV_OK;
    Surf *rt = dev.rt;
    if (g_frame == g_logframe) LOG("Clear n=%u fl=%x col=%08x z=%f vp %u,%u %ux%u", n, fl, col, z, dev.vp.x, dev.vp.y, dev.vp.w, dev.vp.h);
    bind_rt(rt);
    GLbitfield m = 0;
    if (fl & 1) { m |= GL_COLOR_BUFFER_BIT; glColorMask(1, 1, 1, 1); glClearColor((col >> 16 & 255) / 255.f, (col >> 8 & 255) / 255.f, (col & 255) / 255.f, 1); }
    if (fl & 2) { m |= GL_DEPTH_BUFFER_BIT; glDepthMask(GL_TRUE); glClearDepthf(z); }
    if (fl & 4) { m |= GL_STENCIL_BUFFER_BIT; glStencilMask(0xff); glClearStencil(ARG(6)); }
    if (!n || !rects) {
        float fx, fy, fw, fh; wide_vp(&fx, &fy, &fw, &fh);
        int x = (int)fx, y = (int)fy, w = (int)(fw + 0.5f), h = (int)(fh + 0.5f);
        glEnable(GL_SCISSOR_TEST); glScissor(x * g_scale, (rt->h - y - h) * g_scale, w * g_scale, h * g_scale);
        glClear(m);
    } else for (uint32_t i = 0; i < n; i++) {
        uint32_t r = rects + 16 * i;
        int x0 = gr32(r), y0 = gr32(r + 4), x1 = gr32(r + 8), y1 = gr32(r + 12);
        glEnable(GL_SCISSOR_TEST); glScissor(x0 * g_scale, (rt->h - y1) * g_scale, (x1 - x0) * g_scale, (y1 - y0) * g_scale);
        glClear(m);
    }
    glDisable(GL_SCISSOR_TEST);
    if (fl & 1) rt->gpu_newer = 1;
    return DEV_OK;
}
static uint32_t dv_SetRenderState(CPU *c) {
    uint32_t s = ARG(1);
    if (s < 256) dev.rs[s] = ARG(2);
    return DEV_OK;
}
static uint32_t dv_GetRenderState(CPU *c) { uint32_t s = ARG(1); gw32(ARG(2), s < 256 ? dev.rs[s] : 0); return DEV_OK; }
static uint32_t dv_SetTextureStageState(CPU *c) {
    uint32_t st = ARG(1), t = ARG(2);
    if (st < 8 && t < 32) dev.tss[st][t] = ARG(3);
    if (st < 8 && t == 12) dev.tss[st][13] = dev.tss[st][14] = ARG(3);
    return DEV_OK;
}
static uint32_t dv_GetTextureStageState(CPU *c) { uint32_t st = ARG(1), t = ARG(2); gw32(ARG(3), st < 8 && t < 32 ? dev.tss[st][t] : 0); return DEV_OK; }
static uint32_t dv_SetTexture(CPU *c) { if (ARG(1) < 8) dev.tex[ARG(1)] = surf_of(ARG(2)); return DEV_OK; }
static uint32_t dv_GetTexture(CPU *c) { Surf *s = ARG(1) < 8 ? dev.tex[ARG(1)] : NULL; if (s) s->refs++; gw32(ARG(2), s ? s->obj : 0); return DEV_OK; }
static float *xf(uint32_t t) { return t == 1 ? dev.world : t == 2 ? dev.view : t == 3 ? dev.proj : t == 0x100 ? dev.world : NULL; }
static uint32_t dv_SetTransform(CPU *c) {
    float *m = xf(ARG(1)); if (m) memcpy(m, GPTR(ARG(2)), 64);
    if (g_frame == g_logframe && m && ARG(1) != 3) LOG("SetTransform %u: %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f", ARG(1),
        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    return DEV_OK;
}
static uint32_t dv_GetTransform(CPU *c) { float *m = xf(ARG(1)); if (m) memcpy(GPTR(ARG(2)), m, 64); else ident((float *)GPTR(ARG(2))); return DEV_OK; }
static uint32_t dv_MultiplyTransform(CPU *c) { float *m = xf(ARG(1)); if (m) matmul((float *)GPTR(ARG(2)), m, m); return DEV_OK; }
static uint32_t dv_SetViewport(CPU *c) {
    uint32_t v = ARG(1);
    if (g_frame == g_logframe) LOG("SetViewport %u,%u %ux%u z %f-%f", gr32(v), gr32(v + 4), gr32(v + 8), gr32(v + 12), grf(v + 16), grf(v + 20));
    dev.vp.x = gr32(v); dev.vp.y = gr32(v + 4); dev.vp.w = gr32(v + 8); dev.vp.h = gr32(v + 12); dev.vp.mn = grf(v + 16); dev.vp.mx = grf(v + 20);
    return DEV_OK;
}
static uint32_t dv_GetViewport(CPU *c) {
    uint32_t v = ARG(1);
    gw32(v, dev.vp.x); gw32(v + 4, dev.vp.y); gw32(v + 8, dev.vp.w); gw32(v + 12, dev.vp.h); gwf(v + 16, dev.vp.mn); gwf(v + 20, dev.vp.mx);
    return DEV_OK;
}
static uint32_t dv_SetMaterial(CPU *c) { memcpy(dev.mat, GPTR(ARG(1)), 68); return DEV_OK; }
static uint32_t dv_GetMaterial(CPU *c) { memcpy(GPTR(ARG(1)), dev.mat, 68); return DEV_OK; }
static uint32_t dv_SetLight(CPU *c) {
    uint32_t i = ARG(1), p = ARG(2);
    if (i >= 16) return DEV_OK;
    typeof(dev.light[0]) *L = &dev.light[i];
    L->type = gr32(p);
    for (int k = 0; k < 4; k++) { L->d[k] = grf(p + 4 + 4 * k); L->s[k] = grf(p + 20 + 4 * k); L->a[k] = grf(p + 36 + 4 * k); }
    for (int k = 0; k < 3; k++) { L->pos[k] = grf(p + 52 + 4 * k); L->dir[k] = grf(p + 64 + 4 * k); }
    float dl = sqrtf(L->dir[0] * L->dir[0] + L->dir[1] * L->dir[1] + L->dir[2] * L->dir[2]); if (dl > 0) for (int k = 0; k < 3; k++) L->dir[k] /= dl;
    L->range = grf(p + 76); L->falloff = grf(p + 80); L->att0 = grf(p + 84); L->att1 = grf(p + 88); L->att2 = grf(p + 92);
    L->theta = grf(p + 96); L->phi = grf(p + 100);
    return DEV_OK;
}
static uint32_t dv_LightEnable(CPU *c) { if (ARG(1) < 16) dev.light[ARG(1)].on = ARG(2) != 0; return DEV_OK; }
static uint32_t dv_GetLightEnable(CPU *c) { gw32(ARG(2), ARG(1) < 16 ? dev.light[ARG(1)].on : 0); return DEV_OK; }
static uint32_t dv_BeginScene(CPU *c) { dev.in_scene = 1; return DEV_OK; }
static uint32_t dv_EndScene(CPU *c) { dev.in_scene = 0; return DEV_OK; }
static uint32_t dv_GetCaps(CPU *c) { devdesc_write(ARG(1), 1); memcpy(GPTR(ARG(1) + 196), IID_HAL, 16); return DEV_OK; }
static uint32_t dv_EnumTextureFormats(CPU *c) {
    uint32_t cb = ARG(1), ctx = ARG(2), p = gmalloc(32);
    const PixFmt *f[] = { &PF_565, &PF_555, &PF_1555, &PF_4444, &PF_8888 };
    for (int i = 0; i < 5; i++) { pf_write(p, f[i]); if (!rt_invoke(c, cb, 2, p, ctx)) break; }
    gfree(p);
    return DEV_OK;
}
static uint32_t dv_GetDirect3D(CPU *c) { gw32(ARG(1), g_d3d_obj); return DEV_OK; }
static uint32_t dv_SetRenderTarget(CPU *c) {
    Surf *s = surf_of(ARG(1));
    if (s) { if (!s->rt) { s->rt = 1; surf_alloc_gl_rt(s); } dev.rt = s; dev.vp.w = s->w; dev.vp.h = s->h; dev.vp.x = dev.vp.y = 0; }
    return DEV_OK;
}
static uint32_t dv_GetRenderTarget(CPU *c) { if (dev.rt) dev.rt->refs++; gw32(ARG(1), dev.rt ? dev.rt->obj : 0); return DEV_OK; }
static uint32_t dv_ValidateDevice(CPU *c) { gw32(ARG(1), 1); return DEV_OK; }
static uint32_t dv_Release(CPU *c) { return 0; }
static uint32_t dv_PreLoad(CPU *c) { Surf *s = surf_of(ARG(1)); if (s) surf_texture(s, dev.rs[41]); return DEV_OK; }
static uint32_t dv_Load(CPU *c) {
    Surf *d = surf_of(ARG(1)), *s = surf_of(ARG(3));
    if (!d || !s) return DDERR_INVALIDPARAMS;
    for (; d && s; d = d->mip, s = s->mip) {
        do_blt(d, 0, 0, d->w, d->h, s, 0, 0, s->w, s->h, 0);
        d->has_ck = s->has_ck; d->ck_lo = s->ck_lo; d->ck_hi = s->ck_hi;
    }
    surf_of(ARG(1))->mem_newer = 1;
    return DEV_OK;
}
static uint32_t dv_SetClipStatus(CPU *c) { return DEV_OK; }
static uint32_t dv_GetClipStatus(CPU *c) { memset(GPTR(ARG(1)), 0, 28); return DEV_OK; }
static uint32_t dv_GetInfo(CPU *c) { return 1; /* S_FALSE: no info */ }

static uint32_t d3d_CreateDevice(CPU *c) {
    Surf *rt = surf_of(ARG(2));
    if (!rt) return DDERR_INVALIDPARAMS;
    if (!rt->rt) { rt->rt = 1; surf_alloc_gl_rt(rt); }
    dev_defaults();
    dev.rt = rt; dev.vp.w = rt->w; dev.vp.h = rt->h; dev.vp.mx = 1;
    if (!g_dev_obj) g_dev_obj = com_new(vt_dev7, 0, &dev);
    gw32(ARG(3), g_dev_obj);
    LOG("CreateDevice on %08x (%dx%d)", rt->obj, rt->w, rt->h);
    return DD_OK;
}

/* vertex buffers */
static uint32_t d3d_CreateVertexBuffer(CPU *c) {
    uint32_t d = ARG(1);
    VB *b = calloc(1, sizeof *b);
    b->fvf = gr32(d + 8); b->n = gr32(d + 12); b->stride = fvf_stride(b->fvf); b->refs = 1;
    b->mem = gcalloc(b->n * b->stride + 16);
    b->obj = com_new(vt_vb7, 0, b);
    gw32(ARG(2), b->obj);
    TRACE("CreateVertexBuffer fvf=%x n=%u caps=%x", b->fvf, b->n, gr32(d + 4));
    return DD_OK;
}
#define VBH() COM_HOST(VB)
static uint32_t vb_Lock(CPU *c) { VB *b = VBH(); gw32(ARG(2), b->mem); if (ARG(3)) gw32(ARG(3), b->n * b->stride); return DD_OK; }
static uint32_t vb_GetDesc(CPU *c) { VB *b = VBH(); uint32_t d = ARG(1); gw32(d + 4, 0); gw32(d + 8, b->fvf); gw32(d + 12, b->n); return DD_OK; }
static uint32_t vb_AddRef(CPU *c) { return ++VBH()->refs; }
static uint32_t vb_Release(CPU *c) {
    VB *b = VBH();
    if (--b->refs > 0) return b->refs;
    if (b->mem) gfree(b->mem);
    b->mem = 0;
    return 0;
}
static uint32_t vb_ProcessVertices(CPU *c) {
    /* (this, op, dstIndex, count, srcVB, srcIndex, device, flags): transform into an XYZRHW buffer */
    VB *d = VBH(), *s = (VB *)com_host(ARG(4));
    uint32_t di = ARG(2), n = ARG(3), si = ARG(5);
    if ((d->fvf & 0xe) != 4) return DDERR_INVALIDPARAMS;
    process(s->fvf, s->mem, s->stride, si, n);
    float W = dev.rt ? dev.rt->w : g_mode_w, H = dev.rt ? dev.rt->h : g_mode_h;
    for (uint32_t i = 0; i < n; i++) {
        GVert *g = &vbuf[i];
        uint8_t *o = GPTR(d->mem + (di + i) * d->stride);
        float w = g->w != 0 ? g->w : 1;
        float *p = (float *)o;
        p[0] = (g->x / w + 1) / 2 * W; p[1] = (1 - g->y / w) / 2 * H; p[2] = (g->z / w + 1) / 2; p[3] = 1 / w;
        int k = 16;
        if (d->fvf & 0x40) { uint32_t col = g->c[3] << 24 | g->c[0] << 16 | g->c[1] << 8 | g->c[2]; memcpy(o + k, &col, 4); k += 4; }
        if (d->fvf & 0x80) { uint32_t col = g->s[3] << 24 | g->s[0] << 16 | g->s[1] << 8 | g->s[2]; memcpy(o + k, &col, 4); k += 4; }
        if ((d->fvf >> 8) & 0xf) { memcpy(o + k, &g->u0, 8); k += 8; }
        if (((d->fvf >> 8) & 0xf) > 1) { memcpy(o + k, &g->u1, 8); k += 8; }
    }
    return DD_OK;
}

static uint32_t d3d_QueryInterface(CPU *c) { iid_log("IDirect3D7", ARG(1)); gw32(ARG(2), 0); return E_NOINTERFACE; }
static uint32_t d3d_Release(CPU *c) { return 1; }

/* ---------------------------------------------------------------- vtables */
#define N(a) (int)(sizeof a / sizeof *a)
static void dd_build(void) {
    if (vt_dd7) return;
    static const ComMethod dd7[] = {
        {"QueryInterface", dd_QueryInterface, 3}, {"AddRef", one, 1}, {"Release", dd_Release, 1},
        {"Compact", ok, 1}, {"CreateClipper", dd_CreateClipper, 4}, {"CreatePalette", dd_CreatePalette, 5},
        {"CreateSurface", dd7_CreateSurface, 4}, {"DuplicateSurface", NULL, 3}, {"EnumDisplayModes", dd7_EnumDisplayModes, 5},
        {"EnumSurfaces", NULL, 5}, {"FlipToGDISurface", ok, 1}, {"GetCaps", dd_GetCaps, 3},
        {"GetDisplayMode", dd_GetDisplayMode, 2}, {"GetFourCCCodes", dd_GetFourCCCodes, 3}, {"GetGDISurface", NULL, 2},
        {"GetMonitorFrequency", dd_GetMonitorFrequency, 2}, {"GetScanLine", dd_GetScanLine, 2},
        {"GetVerticalBlankStatus", dd_GetVerticalBlankStatus, 2}, {"Initialize", ok, 2}, {"RestoreDisplayMode", ok, 1},
        {"SetCooperativeLevel", dd_SetCooperativeLevel, 3}, {"SetDisplayMode", dd_SetDisplayMode, 6},
        {"WaitForVerticalBlank", ok, 3}, {"GetAvailableVidMem", dd_GetAvailableVidMem, 4}, {"GetSurfaceFromDC", NULL, 3},
        {"RestoreAllSurfaces", ok, 1}, {"TestCooperativeLevel", ok, 1}, {"GetDeviceIdentifier", dd_GetDeviceIdentifier, 3},
        {"StartModeTest", notimpl, 4}, {"EvaluateMode", notimpl, 3},
    };
    vt_dd7 = com_make_vtbl("IDirectDraw7", dd7, N(dd7));
    static const ComMethod dd1[] = {
        {"QueryInterface", dd_QueryInterface, 3}, {"AddRef", one, 1}, {"Release", dd_Release, 1},
        {"Compact", ok, 1}, {"CreateClipper", dd_CreateClipper, 4}, {"CreatePalette", dd_CreatePalette, 5},
        {"CreateSurface", dd1_CreateSurface, 4}, {"DuplicateSurface", NULL, 3}, {"EnumDisplayModes", dd1_EnumDisplayModes, 5},
        {"EnumSurfaces", NULL, 5}, {"FlipToGDISurface", ok, 1}, {"GetCaps", dd_GetCaps, 3},
        {"GetDisplayMode", dd_GetDisplayMode, 2}, {"GetFourCCCodes", dd_GetFourCCCodes, 3}, {"GetGDISurface", NULL, 2},
        {"GetMonitorFrequency", dd_GetMonitorFrequency, 2}, {"GetScanLine", dd_GetScanLine, 2},
        {"GetVerticalBlankStatus", dd_GetVerticalBlankStatus, 2}, {"Initialize", ok, 2}, {"RestoreDisplayMode", ok, 1},
        {"SetCooperativeLevel", dd_SetCooperativeLevel, 3}, {"SetDisplayMode", dd_SetDisplayMode, 4},
        {"WaitForVerticalBlank", ok, 3},
    };
    vt_dd1 = com_make_vtbl("IDirectDraw", dd1, N(dd1));
    static const ComMethod s7[] = {
        {"QueryInterface", ds_QueryInterface, 3}, {"AddRef", ds_AddRef, 1}, {"Release", ds_Release, 1},
        {"AddAttachedSurface", ds_AddAttachedSurface, 2}, {"AddOverlayDirtyRect", ok, 2}, {"Blt", ds_Blt, 6},
        {"BltBatch", NULL, 4}, {"BltFast", ds_BltFast, 6}, {"DeleteAttachedSurface", ds_DeleteAttachedSurface, 3},
        {"EnumAttachedSurfaces", ds_EnumAttachedSurfaces, 3}, {"EnumOverlayZOrders", NULL, 4}, {"Flip", ds_Flip, 3},
        {"GetAttachedSurface", ds_GetAttachedSurface, 3}, {"GetBltStatus", ok, 2}, {"GetCaps", ds_GetCaps, 2},
        {"GetClipper", ds_GetClipper, 2}, {"GetColorKey", ds_GetColorKey, 3}, {"GetDC", ds_GetDC, 2},
        {"GetFlipStatus", ds_GetFlipStatus, 2}, {"GetOverlayPosition", NULL, 3}, {"GetPalette", ds_GetPalette, 2},
        {"GetPixelFormat", ds_GetPixelFormat, 2}, {"GetSurfaceDesc", ds_GetSurfaceDesc, 2}, {"Initialize", ok, 3},
        {"IsLost", ds_IsLost, 1}, {"Lock", ds_Lock, 5}, {"ReleaseDC", ok, 2}, {"Restore", ok, 1},
        {"SetClipper", ok, 2}, {"SetColorKey", ds_SetColorKey, 3}, {"SetOverlayPosition", NULL, 3},
        {"SetPalette", ds_SetPalette, 2}, {"Unlock", ds_Unlock, 2}, {"UpdateOverlay", NULL, 6},
        {"UpdateOverlayDisplay", NULL, 2}, {"UpdateOverlayZOrder", NULL, 3}, {"GetDDInterface", ds_GetDDInterface, 2},
        {"PageLock", ok, 2}, {"PageUnlock", ok, 2}, {"SetSurfaceDesc", NULL, 3}, {"SetPrivateData", ok, 5},
        {"GetPrivateData", notimpl, 4}, {"FreePrivateData", ok, 2}, {"GetUniquenessValue", ds_GetUniquenessValue, 2},
        {"ChangeUniquenessValue", ds_ChangeUniquenessValue, 1}, {"SetPriority", ok, 2}, {"GetPriority", ds_GetPriority, 2},
        {"SetLOD", ok, 2}, {"GetLOD", ds_GetLOD, 2},
    };
    vt_surf7 = com_make_vtbl("IDirectDrawSurface7", s7, N(s7));
    static const ComMethod s1[] = {
        {"QueryInterface", ds_QueryInterface, 3}, {"AddRef", ds_AddRef, 1}, {"Release", ds_Release, 1},
        {"AddAttachedSurface", ds_AddAttachedSurface, 2}, {"AddOverlayDirtyRect", ok, 2}, {"Blt", ds_Blt, 6},
        {"BltBatch", NULL, 4}, {"BltFast", ds_BltFast, 6}, {"DeleteAttachedSurface", ds_DeleteAttachedSurface, 3},
        {"EnumAttachedSurfaces", ds_EnumAttachedSurfaces, 3}, {"EnumOverlayZOrders", NULL, 4}, {"Flip", ds_Flip, 3},
        {"GetAttachedSurface", ds_GetAttachedSurface, 3}, {"GetBltStatus", ok, 2}, {"GetCaps", ds1_GetCaps, 2},
        {"GetClipper", ds_GetClipper, 2}, {"GetColorKey", ds_GetColorKey, 3}, {"GetDC", ds_GetDC, 2},
        {"GetFlipStatus", ds_GetFlipStatus, 2}, {"GetOverlayPosition", NULL, 3}, {"GetPalette", ds_GetPalette, 2},
        {"GetPixelFormat", ds_GetPixelFormat, 2}, {"GetSurfaceDesc", ds1_GetSurfaceDesc, 2}, {"Initialize", ok, 3},
        {"IsLost", ds_IsLost, 1}, {"Lock", ds_Lock, 5}, {"ReleaseDC", ok, 2}, {"Restore", ok, 1},
        {"SetClipper", ok, 2}, {"SetColorKey", ds_SetColorKey, 3}, {"SetOverlayPosition", NULL, 3},
        {"SetPalette", ds_SetPalette, 2}, {"Unlock", ds_Unlock, 2}, {"UpdateOverlay", NULL, 6},
        {"UpdateOverlayDisplay", NULL, 2}, {"UpdateOverlayZOrder", NULL, 3},
    };
    vt_surf1 = com_make_vtbl("IDirectDrawSurface", s1, N(s1));
    static const ComMethod cl[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", one, 1}, {"Release", ok, 1}, {"GetClipList", NULL, 4},
        {"GetHWnd", NULL, 2}, {"Initialize", ok, 3}, {"IsClipListChanged", NULL, 2}, {"SetClipList", ok, 3}, {"SetHWnd", ok, 3},
    };
    vt_clip = com_make_vtbl("IDirectDrawClipper", cl, N(cl));
    static const ComMethod pl[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", one, 1}, {"Release", ok, 1}, {"GetCaps", NULL, 2},
        {"GetEntries", pal_GetEntries, 5}, {"Initialize", ok, 4}, {"SetEntries", pal_SetEntries, 5},
    };
    vt_pal = com_make_vtbl("IDirectDrawPalette", pl, N(pl));
    static const ComMethod gm[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", one, 1}, {"Release", ok, 1},
        {"GetGammaRamp", gc_GetGammaRamp, 3}, {"SetGammaRamp", gc_SetGammaRamp, 3},
    };
    vt_gamma = com_make_vtbl("IDirectDrawGammaControl", gm, N(gm));
    for (int i = 0; i < 256; i++) g_ramp[0][i] = g_ramp[1][i] = g_ramp[2][i] = i * 257;
}
static void d3d_build(void) {
    if (vt_d3d7) return;
    static const ComMethod d3[] = {
        {"QueryInterface", d3d_QueryInterface, 3}, {"AddRef", one, 1}, {"Release", d3d_Release, 1},
        {"EnumDevices", d3d_EnumDevices, 3}, {"CreateDevice", d3d_CreateDevice, 4},
        {"CreateVertexBuffer", d3d_CreateVertexBuffer, 4}, {"EnumZBufferFormats", d3d_EnumZBufferFormats, 4},
        {"EvictManagedTextures", ok, 1},
    };
    vt_d3d7 = com_make_vtbl("IDirect3D7", d3, N(d3));
    static const ComMethod dv[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", one, 1}, {"Release", dv_Release, 1},
        {"GetCaps", dv_GetCaps, 2}, {"EnumTextureFormats", dv_EnumTextureFormats, 3}, {"BeginScene", dv_BeginScene, 1},
        {"EndScene", dv_EndScene, 1}, {"GetDirect3D", dv_GetDirect3D, 2}, {"SetRenderTarget", dv_SetRenderTarget, 3},
        {"GetRenderTarget", dv_GetRenderTarget, 2}, {"Clear", dv_Clear, 7}, {"SetTransform", dv_SetTransform, 3},
        {"GetTransform", dv_GetTransform, 3}, {"SetViewport", dv_SetViewport, 2}, {"MultiplyTransform", dv_MultiplyTransform, 3},
        {"GetViewport", dv_GetViewport, 2}, {"SetMaterial", dv_SetMaterial, 2}, {"GetMaterial", dv_GetMaterial, 2},
        {"SetLight", dv_SetLight, 3}, {"GetLight", NULL, 3}, {"SetRenderState", dv_SetRenderState, 3},
        {"GetRenderState", dv_GetRenderState, 3}, {"BeginStateBlock", NULL, 1}, {"EndStateBlock", NULL, 2},
        {"PreLoad", dv_PreLoad, 2}, {"DrawPrimitive", dv_DrawPrimitive, 6}, {"DrawIndexedPrimitive", dv_DrawIndexedPrimitive, 8},
        {"SetClipStatus", dv_SetClipStatus, 2}, {"GetClipStatus", dv_GetClipStatus, 2},
        {"DrawPrimitiveStrided", dv_DrawPrimitiveStrided, 6}, {"DrawIndexedPrimitiveStrided", dv_DrawIndexedPrimitiveStrided, 8},
        {"DrawPrimitiveVB", dv_DrawPrimitiveVB, 6}, {"DrawIndexedPrimitiveVB", dv_DrawIndexedPrimitiveVB, 8},
        {"ComputeSphereVisibility", NULL, 6}, {"GetTexture", dv_GetTexture, 3}, {"SetTexture", dv_SetTexture, 3},
        {"GetTextureStageState", dv_GetTextureStageState, 4}, {"SetTextureStageState", dv_SetTextureStageState, 4},
        {"ValidateDevice", dv_ValidateDevice, 2}, {"ApplyStateBlock", NULL, 2}, {"CaptureStateBlock", NULL, 2},
        {"DeleteStateBlock", NULL, 2}, {"CreateStateBlock", NULL, 3}, {"Load", dv_Load, 6}, {"LightEnable", dv_LightEnable, 3},
        {"GetLightEnable", dv_GetLightEnable, 3}, {"SetClipPlane", ok, 3}, {"GetClipPlane", NULL, 3}, {"GetInfo", dv_GetInfo, 4},
    };
    vt_dev7 = com_make_vtbl("IDirect3DDevice7", dv, N(dv));
    static const ComMethod vb[] = {
        {"QueryInterface", NULL, 3}, {"AddRef", vb_AddRef, 1}, {"Release", vb_Release, 1}, {"Lock", vb_Lock, 4},
        {"Unlock", ok, 1}, {"ProcessVertices", vb_ProcessVertices, 8}, {"GetVertexBufferDesc", vb_GetDesc, 2},
        {"Optimize", ok, 3}, {"ProcessVerticesStrided", NULL, 8},
    };
    vt_vb7 = com_make_vtbl("IDirect3DVertexBuffer7", vb, N(vb));
    if (!g_d3d_obj) g_d3d_obj = com_new(vt_d3d7, 0, NULL);
}

/* ---------------------------------------------------------------- exports */
static uint32_t api_DirectDrawCreate(CPU *c) {
    dd_build();
    if (!g_dd1_obj) g_dd1_obj = com_new(vt_dd1, 0, NULL);
    if (!g_dd_obj) g_dd_obj = com_new(vt_dd7, 0, NULL);
    gw32(ARG(1), g_dd1_obj);
    LOG("DirectDrawCreate");
    return DD_OK;
}
static uint32_t api_DirectDrawCreateEx(CPU *c) {
    dd_build();
    if (!g_dd_obj) g_dd_obj = com_new(vt_dd7, 0, NULL);
    gw32(ARG(1), g_dd_obj);
    LOG("DirectDrawCreateEx");
    return DD_OK;
}
static uint32_t api_DirectDrawEnumerateExA(CPU *c) {
    uint32_t cb = ARG(0), ctx = ARG(1);
    uint32_t d = gstrdup("Primary Display Driver"), n = gstrdup("display");
    rt_invoke(c, cb, 5, 0, d, n, ctx, 0);
    return DD_OK;
}
static uint32_t api_DirectDrawEnumerateA(CPU *c) {
    uint32_t d = gstrdup("Primary Display Driver"), n = gstrdup("display");
    rt_invoke(c, ARG(0), 4, 0, d, n, ARG(1));
    return DD_OK;
}

const ApiDef ddraw_apis[] = {
    {"ddraw.dll", "DirectDrawCreate", api_DirectDrawCreate, 12}, {"ddraw.dll", "DirectDrawCreateEx", api_DirectDrawCreateEx, 16},
    {"ddraw.dll", "DirectDrawEnumerateExA", api_DirectDrawEnumerateExA, 12}, {"ddraw.dll", "DirectDrawEnumerateA", api_DirectDrawEnumerateA, 8},
    { 0 }
};

/* ---------------------------------------------------------------- for the video player */
/* write RGBA pixels into a surface at (x, y) */
int ddraw_surface_size(uint32_t obj, int *w, int *h) {
    Surf *s = surf_of(obj);
    if (!s) return 0;
    *w = s->w; *h = s->h;
    return 1;
}
void ddraw_surface_write(uint32_t obj, int x, int y, int w, int h, const uint8_t *rgba) {
    Surf *s = surf_of(obj);
    if (!s || !s->mem) return;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0) return;
    if (s->rt) {
        static GLuint t; gl_init();
        if (!t) glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        draw_quad(s, t, w, h, 0, 0, 0, w, h, x, y, x + w, y + h, 0, 1);
        if (s == g_primary) present(s);
        return;
    }
    for (int r = 0; r < h; r++) {
        uint8_t *dst = GPTR(s->mem + (y + r) * s->pitch + x * s->bpp);
        const uint8_t *q = rgba + (size_t)r * w * 4;
        if (s->bpp == 2 && s->pf.r == 0xf800 && s->pf.g == 0x7e0) {
            uint16_t *d = (uint16_t *)dst;
            for (int i = 0; i < w; i++, q += 4) d[i] = (q[0] >> 3) << 11 | (q[1] >> 2) << 5 | q[2] >> 3;
        } else for (int i = 0; i < w; i++, q += 4) px_put(dst + i * s->bpp, s->bpp, pack(q[0], q[1], q[2], 255, &s->pf));
    }
    s->mem_newer = 1;
    s->uniq = g_uniq++;
}
/* an offscreen RGB565 surface (video samples allocate their own when the game passes none) */
uint32_t ddraw_create_offscreen(int w, int h) {
    dd_build();
    Surf *s = surf_new(w, h, &PF_565, 0x40 | 0x800);
    return s->obj;
}
int d3d_frame_count(void) { return g_frame; }
float d3d_rt_aspect(void) { return dev.rt ? (float)dev.rt->w / dev.rt->h : (float)g_mode_w / g_mode_h; }
