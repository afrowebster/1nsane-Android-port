/* Touch controls (Android, or desktop with C2_TOUCH=1), mapped to 1nsane's default keys.
 *  races: steer left/right, rear view / map / DIFF LOCK (left), gas / brake / handbrake,
 *         rescue / repair / rollover, camera, pause (right); all clear of the HUD
 *  menus: left/right/up/down arrows, OK (Enter), back (Esc)
 *  anywhere else: Enter (the pre-race instructions and result screens wait for it)
 *  three fingers: performance readout on/off; tap the readout: next frame rate cap
 * They hide while a game controller is
 * in use and come back on the next touch. Artwork: platform/touch_atlas.h (tools/make_touch_atlas.py). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "plat.h"
#include "../win32/win32.h"
#include "touch_atlas.h"

extern void dinput_key(int, int);
extern int dinput_from_sdl(int);
extern int d3d_racing(void);

/* mode: 1 race only, 2 menu only, 3 both */
typedef struct { float x, y, r; int sc, vk, art, mode; } Btn;
enum { NB = 24 };
static Btn btns[NB];
static int nbtn;
static float scr_a = 16.0f / 9.0f;
static int visible, pad_mode;
static GLuint prog, vbo, vao, tex;
static GLint u_sc;
typedef struct { SDL_FingerID id; int active, btn; } Finger;
/* performance readout: a three-finger tap toggles it, a tap on it steps the frame rate cap
   (30 / 60 / 120 / unlimited); C2_PERF=1 shows it on the desktop */
static int perf_on;
extern void d3d_perf(float *fps, float *game, float *gl, float *swap);
static GLuint prog_c, vbo_c, vao_c;
static GLint u_sc_c;
static Finger fingers[10];
static int held[NB];
/* every press lasts at least MIN_HOLD ms: a quick tap released before the game polled would be lost */
#define MIN_HOLD 120
static Uint32 down_at[NB], release_at[NB];
static int down_frame[NB];
extern int d3d_frame_count(void);
#define MIN_FRAMES 3   /* ... and at least this many frames, however slow the device */

static void press(int b, int down) {
    if (b < 0) return;
    Btn *t = &btns[b];
    if (down) {
        if (release_at[b]) { release_at[b] = 0; return; }   /* pressed again while a release was pending */
        down_at[b] = SDL_GetTicks(); down_frame[b] = d3d_frame_count();
        if (held[b]++ == 0) { dinput_key(dinput_from_sdl(t->sc), 1); if (t->vk) user32_key_event(t->vk, 1); }
    } else if (held[b] == 1 && (SDL_GetTicks() - down_at[b] < MIN_HOLD || d3d_frame_count() - down_frame[b] < MIN_FRAMES)) {
        release_at[b] = down_at[b] + MIN_HOLD;   /* touch_tick releases it */
    } else if (held[b] > 0 && --held[b] == 0) {
        dinput_key(dinput_from_sdl(t->sc), 0); if (t->vk) user32_key_event(t->vk, 0);
    }
}

/* the last slot: an invisible "button" for touches on empty screen (Enter) */
#define B_TAP (NB - 1)
static void add(float x, float y, float r, int sc, int vk, int art, int mode) {
    if (nbtn < B_TAP) btns[nbtn++] = (Btn){x, y, r, sc, vk, art, mode};
}
static void layout(void) {
    int w, h;
    plat_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) return;
    float a = (float)w / h;   /* coordinates: y in [0,1], x in [0,a] */
    if (a < 1.4f) a = 1.4f;
    scr_a = a;
    nbtn = 0;
    btns[B_TAP] = (Btn){-1, -1, 0, SDL_SCANCODE_RETURN, 0x0D, 0, 0};
    /* Races fill the screen (widescreen); the HUD runs along the bottom (map and standings left,
       checkpoint centre, speedometer right) and the lap count sits top left, so the race buttons stay
       between those. Menus are 4:3 with free side bars. */
    /* race, left: steering, and a row of rear view / map / diff lock under the lap count */
    add(0.13f, 0.58f, 0.11f, SDL_SCANCODE_LEFT, 0x25, TA_LEFT, 1);
    add(0.37f, 0.58f, 0.11f, SDL_SCANCODE_RIGHT, 0x27, TA_RIGHT, 1);
    add(0.075f, 0.26f, 0.06f, SDL_SCANCODE_RSHIFT, 0, TA_REAR, 1);
    add(0.215f, 0.26f, 0.06f, SDL_SCANCODE_LALT, 0, TA_MAP, 1);
    add(0.37f, 0.26f, 0.075f, SDL_SCANCODE_L, 'L', TA_DIFF, 1);
    /* race, right: pedals, handbrake, rescue / repair / rollover row, camera and pause on top */
    add(a - 0.14f, 0.58f, 0.13f, SDL_SCANCODE_UP, 0x26, TA_GAS, 1);
    add(a - 0.40f, 0.64f, 0.10f, SDL_SCANCODE_DOWN, 0x28, TA_BRAKE, 1);
    add(a - 0.37f, 0.43f, 0.075f, SDL_SCANCODE_SPACE, 0, TA_HAND, 1);
    add(a - 0.075f, 0.26f, 0.06f, SDL_SCANCODE_R, 'R', TA_RESCUE, 1);
    add(a - 0.215f, 0.26f, 0.06f, SDL_SCANCODE_BACKSPACE, 0, TA_REPAIR, 1);
    add(a - 0.355f, 0.26f, 0.06f, SDL_SCANCODE_KP_0, 0, TA_ROLL, 1);
    add(a - 0.075f, 0.085f, 0.06f, SDL_SCANCODE_ESCAPE, 0x1B, TA_PAUSE, 1);
    add(a - 0.215f, 0.085f, 0.06f, SDL_SCANCODE_F9, 0x78, TA_CAM, 1);
    /* menus (4:3, side bars free): left/right bottom left, up above down on the right, OK beside them */
    add(0.13f, 0.82f, 0.11f, SDL_SCANCODE_LEFT, 0x25, TA_LEFT, 2);
    add(0.37f, 0.82f, 0.11f, SDL_SCANCODE_RIGHT, 0x27, TA_RIGHT, 2);
    add(a - 0.13f, 0.47f, 0.10f, SDL_SCANCODE_UP, 0x26, TA_UP, 2);
    add(a - 0.13f, 0.80f, 0.10f, SDL_SCANCODE_DOWN, 0x28, TA_DOWN, 2);
    add(a - 0.36f, 0.635f, 0.10f, SDL_SCANCODE_RETURN, 0x0D, TA_OK, 2);
    add(a - 0.075f, 0.085f, 0.06f, SDL_SCANCODE_ESCAPE, 0x1B, TA_BACK, 2);
}

static int active_mode(void) { return d3d_racing() ? 1 : 2; }
static int hit(float x, float y) {
    int m = active_mode(), best = -1; float bd = 1e9f;
    for (int i = 0; i < nbtn; i++) {
        if (!(btns[i].mode & m)) continue;
        float dx = x - btns[i].x, dy = y - btns[i].y, d = sqrtf(dx * dx + dy * dy);
        if (d < btns[i].r * 1.2f && d < bd) { bd = d; best = i; }
    }
    return best;
}

void touch_init(void) {
    perf_on = getenv("C2_PERF") != NULL;
#ifdef __ANDROID__
    visible = 1;
#else
    visible = getenv("C2_TOUCH") != NULL;
#endif
    layout();
}
static void release_all(void) {
    for (int i = 0; i < 10; i++) if (fingers[i].active && fingers[i].btn >= 0) { press(fingers[i].btn, 0); fingers[i].btn = -1; }
}
void touch_gamepad(int active) {
    if (active && !pad_mode) { release_all(); LOG("touch: hidden (game controller)"); }
    pad_mode = active;
}
void touch_tick(void) {
    Uint32 now = SDL_GetTicks();
    for (int b = 0; b < NB; b++)
        if (release_at[b] && (int32_t)(now - release_at[b]) >= 0 && d3d_frame_count() - down_frame[b] >= MIN_FRAMES) { release_at[b] = 0; press(b, 0); }
    /* buttons of the other mode can't stay held when the screen changes */
    static int last = 0;
    int m = active_mode();
    if (m != last) {
        for (int i = 0; i < 10; i++) if (fingers[i].active && fingers[i].btn >= 0 && fingers[i].btn != B_TAP && !(btns[fingers[i].btn].mode & m)) {
            press(fingers[i].btn, 0); fingers[i].btn = -1;
        }
        last = m;
    }
}
void touch_sensor(const float *d) { (void)d; }

void touch_event(SDL_Event *e) {
    if (!visible) return;
    if (e->type == SDL_FINGERDOWN) {
        int nact = 0; for (int i = 0; i < 10; i++) nact += fingers[i].active;
        if (nact == 2) {   /* third finger: toggle the readout, and let go of what the other two hold */
            perf_on = !perf_on; LOG("performance readout %s", perf_on ? "on" : "off");
            for (int i = 0; i < 10; i++) if (fingers[i].active && fingers[i].btn >= 0) { press(fingers[i].btn, 0); fingers[i].btn = -1; }
            for (int i = 0; i < 10; i++) if (!fingers[i].active) { fingers[i] = (Finger){e->tfinger.fingerId, 1, -1}; break; }
            return;
        }
        if (perf_on && e->tfinger.y < 0.07f && e->tfinger.x * scr_a < 1.0f) {   /* tap the readout */
            plat_cycle_fps_cap();
            for (int i = 0; i < 10; i++) if (!fingers[i].active) { fingers[i] = (Finger){e->tfinger.fingerId, 1, -1}; break; }
            return;
        }
    }
    if (pad_mode) { pad_mode = 0; LOG("touch: shown"); }
    layout();
    float x = e->tfinger.x * scr_a, y = e->tfinger.y;
    int slot = -1;
    for (int i = 0; i < 10; i++) if (fingers[i].active && fingers[i].id == e->tfinger.fingerId) slot = i;
    if (e->type == SDL_FINGERDOWN) {
        if (slot < 0) for (int i = 0; i < 10; i++) if (!fingers[i].active) { slot = i; break; }
        if (slot < 0) return;
        fingers[slot] = (Finger){e->tfinger.fingerId, 1, hit(x, y)};
        if (fingers[slot].btn < 0) fingers[slot].btn = B_TAP;   /* empty screen: Enter (start a race, skip screens) */
        press(fingers[slot].btn, 1);
    } else if (e->type == SDL_FINGERMOTION) {
        if (slot < 0) return;
        /* sliding between buttons (steer left <-> right, gas <-> brake) */
        int b = hit(x, y);
        if (fingers[slot].btn == B_TAP) return;   /* a tap stays a tap */
        if (b != fingers[slot].btn) { press(fingers[slot].btn, 0); fingers[slot].btn = b; press(b, 1); }
    } else {
        if (slot < 0) return;
        press(fingers[slot].btn, 0);
        fingers[slot].active = 0; fingers[slot].btn = -1;
    }
}

static const char *VS = "#version 300 es\nin vec2 p; in vec2 t; in float k; out vec2 v; out float kk; uniform vec2 sc;\n"
    "void main() { gl_Position = vec4(p.x * sc.x - 1.0, 1.0 - p.y * sc.y, 0.0, 1.0); v = t; kk = k; }\n";
static const char *FS = "#version 300 es\nprecision mediump float; in vec2 v; in float kk; uniform sampler2D tx; out vec4 o;\n"
    "void main() { vec4 c = texture(tx, v); o = vec4(c.rgb * (1.0 + 0.6 * kk), c.a * (0.55 + 0.4 * kk)); }\n";
static GLuint sh(GLenum t, const char *s) { GLuint x = glCreateShader(t); glShaderSource(x, 1, &s, NULL); glCompileShader(x); return x; }
static void gl_setup(void) {
    prog = glCreateProgram();
    glAttachShader(prog, sh(GL_VERTEX_SHADER, VS)); glAttachShader(prog, sh(GL_FRAGMENT_SHADER, FS));
    glBindAttribLocation(prog, 0, "p"); glBindAttribLocation(prog, 1, "t"); glBindAttribLocation(prog, 2, "k");
    glLinkProgram(prog);
    u_sc = glGetUniformLocation(prog, "sc");
    glGenVertexArrays(1, &vao); glGenBuffers(1, &vbo);
    uLongf n = TOUCH_ATLAS_W * TOUCH_ATLAS_H * 4;
    unsigned char *px = malloc(n);
    uncompress(px, &n, touch_atlas_z, touch_atlas_zlen);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TOUCH_ATLAS_W, TOUCH_ATLAS_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    free(px);
}
/* readout: coloured quads, a 3x5 pixel font */
static float vb[8192]; static int nvb;
static void quad(float x0, float y0, float x1, float y1) {
    if (nvb + 12 > 8192) return;
    float q[12] = { x0, y0, x1, y0, x0, y1, x1, y0, x1, y1, x0, y1 };
    memcpy(vb + nvb, q, sizeof q); nvb += 12;
}
static const char *FONT_CH = "0123456789.FPSGAMEWLC:UNIT";
static const unsigned char FONT[][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},{7,5,7,5,7},{7,5,7,1,7},
    {0,0,0,0,2},{7,4,6,4,4},{7,5,7,4,4},{7,4,7,1,7},{7,4,5,5,7},{7,5,7,5,5},{5,7,7,5,5},{7,4,6,4,7},{5,5,7,7,5},{4,4,4,4,7},
    {7,4,4,4,7},{0,2,0,2,0},{5,5,5,5,7},{5,7,7,7,5},{7,2,2,2,7},{7,2,2,2,2},
};
static void text(float x, float y, float px, const char *str) {
    for (const char *c = str; *c; c++, x += px * 4) {
        const char *f = strchr(FONT_CH, *c);
        if (!f || !*c) continue;
        const unsigned char *g = FONT[f - FONT_CH];
        for (int r = 0; r < 5; r++) for (int b = 0; b < 3; b++)
            if (g[r] & (4 >> b)) quad(x + b * px, y + r * px, x + (b + 1) * px, y + (r + 1) * px);
    }
}
static const char *VSC = "#version 300 es\nin vec2 p; uniform vec2 sc; void main() { gl_Position = vec4(p.x * sc.x - 1.0, 1.0 - p.y * sc.y, 0.0, 1.0); }\n";
static const char *FSC = "#version 300 es\nprecision mediump float; uniform vec4 col; out vec4 o; void main() { o = col; }\n";
static void flush_col(float r, float g, float b, float a) {
    if (!nvb) return;
    glUniform4f(glGetUniformLocation(prog_c, "col"), r, g, b, a);
    glBufferData(GL_ARRAY_BUFFER, nvb * sizeof(float), vb, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, nvb / 2);
    nvb = 0;
}
static GLuint sh(GLenum t, const char *s);
static void perf_draw(void) {
    if (!prog_c) {
        prog_c = glCreateProgram();
        glAttachShader(prog_c, sh(GL_VERTEX_SHADER, VSC)); glAttachShader(prog_c, sh(GL_FRAGMENT_SHADER, FSC));
        glBindAttribLocation(prog_c, 0, "p");
        glLinkProgram(prog_c);
        u_sc_c = glGetUniformLocation(prog_c, "sc");
        glGenVertexArrays(1, &vao_c); glGenBuffers(1, &vbo_c);
    }
    float fps, game, gl, sw; d3d_perf(&fps, &game, &gl, &sw);
    char cap[16], line[96];
    if (plat_fps_cap()) snprintf(cap, sizeof cap, "%d", plat_fps_cap()); else snprintf(cap, sizeof cap, "UNLIMIT");
    snprintf(line, sizeof line, "FPS %.0f  CAP %s  GAME %.1f  GL %.1f  SWAP %.1f", fps, cap, game, gl, sw);
    glUseProgram(prog_c);
    glUniform2f(u_sc_c, 2.0f / scr_a, 2.0f);
    glBindVertexArray(vao_c); glBindBuffer(GL_ARRAY_BUFFER, vbo_c);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 8, (void *)0);
    for (int i = 1; i < 6; i++) glDisableVertexAttribArray(i);
    quad(0.01f, 0.005f, 0.02f + strlen(line) * 0.032f, 0.06f); flush_col(0, 0, 0, 0.6f);
    text(0.02f, 0.015f, 0.008f, line); flush_col(1, 1, 0.3f, 1);
}
void touch_draw(void) {
    if (perf_on) {
        if (!prog) gl_setup();
        layout();
        int w, h; plat_drawable_size(&w, &h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0); glViewport(0, 0, w, h);
        glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
        glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        perf_draw();
        glDisable(GL_BLEND);
    }
    if (!visible || pad_mode) return;
    if (!prog) gl_setup();
    layout();
    int w, h; plat_drawable_size(&w, &h);
    int m = active_mode();
    float v[NB * 6 * 5]; int n = 0;
    for (int i = 0; i < nbtn; i++) {
        Btn *b = &btns[i];
        if (!(b->mode & m)) continue;
        float u0 = (float)(b->art % 8) * TOUCH_ATLAS_CELL / TOUCH_ATLAS_W, v0 = (float)(b->art / 8) * TOUCH_ATLAS_CELL / TOUCH_ATLAS_H;
        float u1 = u0 + (float)TOUCH_ATLAS_CELL / TOUCH_ATLAS_W, v1 = v0 + (float)TOUCH_ATLAS_CELL / TOUCH_ATLAS_H;
        float k = held[i] ? 1.0f : 0.0f;
        float q[6][5] = { {b->x - b->r, b->y - b->r, u0, v0, k}, {b->x + b->r, b->y - b->r, u1, v0, k}, {b->x - b->r, b->y + b->r, u0, v1, k},
                          {b->x + b->r, b->y - b->r, u1, v0, k}, {b->x + b->r, b->y + b->r, u1, v1, k}, {b->x - b->r, b->y + b->r, u0, v1, k} };
        memcpy(v + n * 5, q, sizeof q); n += 6;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(prog);
    glUniform2f(u_sc, 2.0f / scr_a, 2.0f);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, n * 5 * sizeof(float), v, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, 0, 20, (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, 0, 20, (void *)8);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, 0, 20, (void *)16);
    for (int i = 3; i < 6; i++) glDisableVertexAttribArray(i);
    glDrawArrays(GL_TRIANGLES, 0, n);
    glDisable(GL_BLEND);
}
