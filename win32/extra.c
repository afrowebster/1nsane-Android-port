/* APIs 1nsane needs beyond the common set: registry (persistent), mmio (RIFF/WAV reading), timers,
   keyboard state, version info, ole32 */
#include <pthread.h>
#include <time.h>
#include <ctype.h>
#include <strings.h>
#include <unistd.h>
#include "win32.h"
#include "../platform/plat.h"

static uint32_t ret0(CPU *c) { return 0; }
static uint32_t ret1(CPU *c) { return 1; }

/* ================= registry =================
   One flat store: "key\\path" -> values, saved to <save>/registry.txt as lines
   [key path]  then  name=type:hexdata                                       */
typedef struct { char key[256]; char name[128]; uint32_t type, len; uint8_t *data; } RegVal;
static RegVal regs[512];
static int nregs, reg_loaded;
static char regkeys[256][256];   /* open handles -> key path */
static int nregkeys;
static pthread_mutex_t regmx = PTHREAD_MUTEX_INITIALIZER;

static void reg_path(char *out) { snprintf(out, 1200, "%s/registry.txt", g_save_dir); }
static void reg_load_file(void);
static void reg_set_dword(const char *key, const char *name, uint32_t v) {
    RegVal *r = NULL;
    for (int i = 0; i < nregs; i++) if (!strcasecmp(regs[i].key, key) && !strcasecmp(regs[i].name, name)) r = &regs[i];
    if (!r && nregs < 512) { r = &regs[nregs++]; snprintf(r->key, sizeof r->key, "%s", key); snprintf(r->name, sizeof r->name, "%s", name); }
    if (!r) return;
    free(r->data); r->type = 4; r->len = 4; r->data = malloc(4); memcpy(r->data, &v, 4);
}
/* Widescreen: the game renders races at whatever resolution the registry names (menus are always
   640x480), with a Hor+ view and the HUD anchored to the edges. Its resolution is always set to the
   screen's aspect ratio (the height the player chose is kept); ddraw only offers such modes. */
int ddraw_wide_width(int h);
static void reg_load(void) {
    if (reg_loaded) return;
    reg_load_file();
    const char *k = "HKLM\\Software\\Codemasters\\Insane";
    uint32_t h = 480;
    for (int i = 0; i < nregs; i++)
        if (!strcasecmp(regs[i].key, k) && !strcasecmp(regs[i].name, "RenderHeight") && regs[i].len == 4) memcpy(&h, regs[i].data, 4);
    if (h != 480 && h != 600 && h != 720 && h != 768 && h != 960) h = 480;
    reg_set_dword(k, "RenderWidth", ddraw_wide_width(h));
    reg_set_dword(k, "RenderHeight", h);
    LOG("display: races at %dx%u", ddraw_wide_width(h), h);
}
static void reg_load_file(void) {
    reg_loaded = 1;
    char p[1200]; reg_path(p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[4096], key[256] = "";
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '[') { char *e = strrchr(line, ']'); if (e) *e = 0; snprintf(key, sizeof key, "%s", line + 1); continue; }
        char *eq = strchr(line, '='); if (!eq || nregs >= 512) continue;
        *eq = 0;
        RegVal *v = &regs[nregs++];
        snprintf(v->key, sizeof v->key, "%s", key); snprintf(v->name, sizeof v->name, "%s", line);
        char *q = eq + 1; v->type = strtoul(q, &q, 10); if (*q == ':') q++;
        v->len = strlen(q) / 2; v->data = malloc(v->len + 1);
        for (uint32_t i = 0; i < v->len; i++) { unsigned b; sscanf(q + 2 * i, "%2x", &b); v->data[i] = b; }
    }
    fclose(f);
}
static void reg_save(void) {
    char p[1200], t[1210]; reg_path(p); snprintf(t, sizeof t, "%s.part", p);
    FILE *f = fopen(t, "w");
    if (!f) return;
    for (int i = 0; i < nregs; i++) {
        int first = 1;
        for (int k = 0; k < i; k++) if (!strcasecmp(regs[k].key, regs[i].key)) first = 0;
        if (!first) continue;
        fprintf(f, "[%s]\n", regs[i].key);
        for (int k = i; k < nregs; k++) if (!strcasecmp(regs[k].key, regs[i].key)) {
            fprintf(f, "%s=%u:", regs[k].name, regs[k].type);
            for (uint32_t b = 0; b < regs[k].len; b++) fprintf(f, "%02x", regs[k].data[b]);
            fputc('\n', f);
        }
    }
    fclose(f);
    rename(t, p);
}
#define HKEY_BASE 0x00700000u
static uint32_t reg_open(uint32_t parent, const char *sub, char *full) {
    char base[256] = "";
    if (parent >= HKEY_BASE && parent < HKEY_BASE + 256) snprintf(base, sizeof base, "%s", regkeys[parent - HKEY_BASE]);
    else snprintf(base, sizeof base, "%s", parent == 0x80000001u ? "HKCU" : parent == 0x80000002u ? "HKLM" : "HK");
    snprintf(full, 256, "%s%s%s", base, sub && *sub ? "\\" : "", sub ? sub : "");
    for (int i = 0; i < nregkeys; i++) if (!strcasecmp(regkeys[i], full)) return HKEY_BASE + i;
    if (nregkeys >= 256) rt_fatal("too many registry keys");
    snprintf(regkeys[nregkeys], 256, "%s", full);
    return HKEY_BASE + nregkeys++;
}
static int key_exists(const char *full) {
    size_t n = strlen(full);
    for (int i = 0; i < nregs; i++) if (!strncasecmp(regs[i].key, full, n) && (!regs[i].key[n] || regs[i].key[n] == '\\')) return 1;
    return 0;
}
static uint32_t api_RegOpenKeyExA(CPU *c) {
    char full[256];
    pthread_mutex_lock(&regmx); reg_load();
    uint32_t h = reg_open(ARG(0), gstr(ARG(1)), full);
    int ok = key_exists(full);
    pthread_mutex_unlock(&regmx);
    TRACE("RegOpenKeyExA(%s) -> %s", full, ok ? "ok" : "missing");
    if (!ok) return 2;
    gw32(ARG(4), h);
    return 0;
}
static uint32_t api_RegCreateKeyExA(CPU *c) {
    char full[256];
    pthread_mutex_lock(&regmx); reg_load();
    uint32_t h = reg_open(ARG(0), gstr(ARG(1)), full);
    int ex = key_exists(full);
    pthread_mutex_unlock(&regmx);
    gw32(ARG(7), h);
    if (ARG(8)) gw32(ARG(8), ex ? 2 : 1);
    return 0;
}
static RegVal *reg_find(uint32_t h, const char *name) {
    if (h < HKEY_BASE || h >= HKEY_BASE + 256) return NULL;
    const char *k = regkeys[h - HKEY_BASE];
    for (int i = 0; i < nregs; i++) if (!strcasecmp(regs[i].key, k) && !strcasecmp(regs[i].name, name ? name : "")) return &regs[i];
    return NULL;
}
static uint32_t api_RegQueryValueExA(CPU *c) {
    uint32_t type = ARG(3), data = ARG(4), pcb = ARG(5);
    pthread_mutex_lock(&regmx); reg_load();
    RegVal *v = reg_find(ARG(0), gstr(ARG(1)));
    uint32_t r = 2;
    if (v) {
        r = 0;
        if (type) gw32(type, v->type);
        if (pcb) {
            if (data) { if (gr32(pcb) < v->len) r = 234; else memcpy(GPTR(data), v->data, v->len); }
            gw32(pcb, v->len);
        }
    }
    pthread_mutex_unlock(&regmx);
    TRACE("RegQueryValueExA(%s) -> %u", gstr(ARG(1)) ? gstr(ARG(1)) : "", r);
    return r;
}
static uint32_t api_RegSetValueExA(CPU *c) {
    pthread_mutex_lock(&regmx); reg_load();
    RegVal *v = reg_find(ARG(0), gstr(ARG(1)));
    if (!v && nregs < 512 && ARG(0) >= HKEY_BASE && ARG(0) < HKEY_BASE + 256) {
        v = &regs[nregs++];
        snprintf(v->key, sizeof v->key, "%s", regkeys[ARG(0) - HKEY_BASE]);
        snprintf(v->name, sizeof v->name, "%s", gstr(ARG(1)) ? gstr(ARG(1)) : "");
    }
    if (v) {
        free(v->data);
        v->type = ARG(3); v->len = ARG(5);
        v->data = malloc(v->len + 1); memcpy(v->data, GPTR(ARG(4)), v->len);
        reg_save();
    }
    pthread_mutex_unlock(&regmx);
    return v ? 0 : 5;
}

/* ================= kernel32 odds ================= */
static uint32_t api_lstrlenA(CPU *c) { return ARG(0) ? strlen(gstr(ARG(0))) : 0; }
static uint32_t api_lstrcmpA(CPU *c) { int r = strcmp(gstr(ARG(0)) ? gstr(ARG(0)) : "", gstr(ARG(1)) ? gstr(ARG(1)) : ""); return r < 0 ? -1 : r > 0; }
static uint32_t api_GetTempFileNameA(CPU *c) {
    static int n;
    char b[600]; snprintf(b, sizeof b, "%s\\%s%04X.TMP", gstr(ARG(0)), gstr(ARG(1)) ? gstr(ARG(1)) : "", ++n + (int)(ARG(2) & 0xffff));
    gstrcpy(ARG(3), b, 260);
    return n;
}
static uint32_t api_GlobalLock(CPU *c) { return ARG(0); }
static uint32_t api_GlobalHandle(CPU *c) { return ARG(0); }
static uint32_t api_HeapSize(CPU *c) { return gmsize(ARG(2)); }
static uint32_t api_GetUserNameA(CPU *c) { gstrcpy(ARG(0), "Player", gr32(ARG(1))); gw32(ARG(1), 7); return 1; }

/* ================= user32 keyboard ================= */
extern int g_vk_state[256], g_vk_tapped[256];
/* a key tapped and released between two polls still reads as down once */
static uint32_t api_GetKeyboardState(CPU *c) {
    for (int i = 0; i < 256; i++) { gw8(ARG(0) + i, g_vk_state[i] || g_vk_tapped[i] ? 0x80 : 0); g_vk_tapped[i] = 0; }
    return 1;
}
static uint32_t api_GetAsyncKeyState(CPU *c) {
    int k = ARG(0) & 0xff, r = g_vk_state[k] || g_vk_tapped[k] ? 0x8000 : 0;
    if (g_vk_tapped[k]) r |= 1;
    g_vk_tapped[k] = 0;
    return r;
}
static uint32_t api_GetKeyboardLayout(CPU *c) { return 0x04090409u; }
/* scan code <-> virtual key (US layout, the subset the game's key names need) */
static const struct { uint8_t sc, vk; const char *name; } keymap[] = {
    {1, 0x1B, "Esc"}, {2, '1', "1"}, {3, '2', "2"}, {4, '3', "3"}, {5, '4', "4"}, {6, '5', "5"}, {7, '6', "6"}, {8, '7', "7"},
    {9, '8', "8"}, {10, '9', "9"}, {11, '0', "0"}, {12, 0xBD, "-"}, {13, 0xBB, "="}, {14, 0x08, "Backspace"}, {15, 0x09, "Tab"},
    {16, 'Q', "Q"}, {17, 'W', "W"}, {18, 'E', "E"}, {19, 'R', "R"}, {20, 'T', "T"}, {21, 'Y', "Y"}, {22, 'U', "U"}, {23, 'I', "I"},
    {24, 'O', "O"}, {25, 'P', "P"}, {26, 0xDB, "["}, {27, 0xDD, "]"}, {28, 0x0D, "Enter"}, {29, 0x11, "Ctrl"}, {30, 'A', "A"},
    {31, 'S', "S"}, {32, 'D', "D"}, {33, 'F', "F"}, {34, 'G', "G"}, {35, 'H', "H"}, {36, 'J', "J"}, {37, 'K', "K"}, {38, 'L', "L"},
    {39, 0xBA, ";"}, {40, 0xDE, "'"}, {41, 0xC0, "`"}, {42, 0x10, "Shift"}, {43, 0xDC, "\\"}, {44, 'Z', "Z"}, {45, 'X', "X"},
    {46, 'C', "C"}, {47, 'V', "V"}, {48, 'B', "B"}, {49, 'N', "N"}, {50, 'M', "M"}, {51, 0xBC, ","}, {52, 0xBE, "."},
    {53, 0xBF, "/"}, {54, 0x10, "Right Shift"}, {55, 0x6A, "Num *"}, {56, 0x12, "Alt"}, {57, 0x20, "Space"}, {58, 0x14, "Caps Lock"},
    {59, 0x70, "F1"}, {60, 0x71, "F2"}, {61, 0x72, "F3"}, {62, 0x73, "F4"}, {63, 0x74, "F5"}, {64, 0x75, "F6"}, {65, 0x76, "F7"},
    {66, 0x77, "F8"}, {67, 0x78, "F9"}, {68, 0x79, "F10"}, {71, 0x67, "Num 7"}, {72, 0x68, "Num 8"}, {73, 0x69, "Num 9"},
    {74, 0x6D, "Num -"}, {75, 0x64, "Num 4"}, {76, 0x65, "Num 5"}, {77, 0x66, "Num 6"}, {78, 0x6B, "Num +"}, {79, 0x61, "Num 1"},
    {80, 0x62, "Num 2"}, {81, 0x63, "Num 3"}, {82, 0x60, "Num 0"}, {83, 0x6E, "Num Del"}, {87, 0x7A, "F11"}, {88, 0x7B, "F12"},
};
static const struct { uint8_t sc, vk; const char *name; } keymap_ext[] = {   /* extended (E0) keys */
    {28, 0x0D, "Num Enter"}, {29, 0x11, "Right Ctrl"}, {53, 0x6F, "Num /"}, {56, 0x12, "Right Alt"}, {71, 0x24, "Home"},
    {72, 0x26, "Up"}, {73, 0x21, "Page Up"}, {75, 0x25, "Left"}, {77, 0x27, "Right"}, {79, 0x23, "End"}, {80, 0x28, "Down"},
    {81, 0x22, "Page Down"}, {82, 0x2D, "Insert"}, {83, 0x2E, "Delete"},
};
#define NK(a) (int)(sizeof a / sizeof *a)
static uint32_t api_MapVirtualKeyExA(CPU *c) {
    uint32_t code = ARG(0), type = ARG(1);
    if (type == 0) {   /* vk -> scan */
        for (int i = 0; i < NK(keymap); i++) if (keymap[i].vk == code) return keymap[i].sc;
        for (int i = 0; i < NK(keymap_ext); i++) if (keymap_ext[i].vk == code) return keymap_ext[i].sc;
        return 0;
    }
    if (type == 1 || type == 3) {   /* scan -> vk */
        for (int i = 0; i < NK(keymap); i++) if (keymap[i].sc == code) return keymap[i].vk;
        return 0;
    }
    if (type == 2) return (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') || code == ' ' ? code : 0;
    return 0;
}
static uint32_t api_ToAsciiEx(CPU *c) {
    uint32_t vk = ARG(0);
    int shift = gr8(ARG(2) + 0x10) & 0x80;
    int ch = 0;
    if (vk >= 'A' && vk <= 'Z') ch = shift ? vk : vk + 32;
    else if (vk >= '0' && vk <= '9') ch = vk;
    else if (vk == ' ' || vk == 0x0D || vk == 0x08 || vk == 0x1B) ch = vk;
    else if (vk >= 0x60 && vk <= 0x69) ch = '0' + vk - 0x60;
    else switch (vk) { case 0xBD: ch = shift ? '_' : '-'; break; case 0xBE: ch = '.'; break; case 0xBC: ch = ','; break; }
    if (!ch) return 0;
    gw16(ARG(3), ch);
    return 1;
}
static uint32_t api_GetKeyNameTextA(CPU *c) {
    uint32_t lp = ARG(0); int sc = (lp >> 16) & 0xff, ext = (lp >> 24) & 1;
    const char *n = NULL;
    if (ext) { for (int i = 0; i < NK(keymap_ext); i++) if (keymap_ext[i].sc == sc) n = keymap_ext[i].name; }
    if (!n) for (int i = 0; i < NK(keymap); i++) if (keymap[i].sc == sc) n = keymap[i].name;
    if (!n || !ARG(2)) return 0;
    gstrcpy(ARG(1), n, ARG(2));
    return strlen(n) < ARG(2) ? strlen(n) : ARG(2) - 1;
}
static uint32_t api_AdjustWindowRect(CPU *c) { return 1; }
static uint32_t api_OffsetRect(CPU *c) {
    uint32_t r = ARG(0); int dx = ARG(1), dy = ARG(2);
    gw32(r, gr32(r) + dx); gw32(r + 8, gr32(r + 8) + dx); gw32(r + 4, gr32(r + 4) + dy); gw32(r + 12, gr32(r + 12) + dy);
    return 1;
}

/* ================= winmm: timers ================= */
typedef struct { int used, periodic; volatile int stop; uint32_t ms, fn, user, id; pthread_t th; } MMTimer;
static MMTimer timers[16];
static void *timer_main(void *arg) {
    MMTimer *t = arg;
    CPU *c = rt_new_cpu(0x40000);
    c->thread_id = 900 + t->id;
    t_cpu = c;
    uint64_t next = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); next = ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
    do {
        next += t->ms * 1000ull;
        for (;;) {
            if (t->stop) return NULL;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            uint64_t now = ts.tv_sec * 1000000ull + ts.tv_nsec / 1000;
            if (now >= next) { if (now - next > 200000) next = now; break; }
            uint64_t w = next - now; if (w > 2000) w = 2000;
            struct timespec d = { 0, (long)w * 1000 }; nanosleep(&d, NULL);
        }
        if (t->stop) return NULL;
        rt_invoke(c, t->fn, 5, t->id, 0, t->user, 0, 0);
    } while (t->periodic);
    t->used = 0;
    return NULL;
}
static uint32_t api_timeSetEvent(CPU *c) {
    for (int i = 0; i < 16; i++) if (!timers[i].used) {
        MMTimer *t = &timers[i];
        memset(t, 0, sizeof *t);
        t->used = 1; t->ms = ARG(0) ? ARG(0) : 1; t->fn = ARG(2); t->user = ARG(3); t->periodic = ARG(4) & 1; t->id = i + 1;
        if (ARG(4) & 0x10) LOG("timeSetEvent: callback-event flags %x unsupported", ARG(4));
        LOG("timeSetEvent(%u ms, fn %08x, %s)", t->ms, t->fn, t->periodic ? "periodic" : "once");
        pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&t->th, &at, timer_main, t);
        return t->id;
    }
    return 0;
}
static uint32_t api_timeKillEvent(CPU *c) {
    uint32_t id = ARG(0);
    if (id < 1 || id > 16 || !timers[id - 1].used) return 97;
    timers[id - 1].stop = 1; timers[id - 1].used = 0;
    return 0;
}
static uint32_t api_mciSendCommandA(CPU *c) { LOG("mciSendCommandA(%x, %x) unsupported", ARG(0), ARG(1)); return 263; }

/* ================= winmm: mmio =================
   Files are read through a window in guest memory (what mmioGetInfo exposes for direct buffer access).
   Sources: a caller-supplied memory buffer ('MEM '), a host file read on demand, or "<name>.mp2" in
   place of a missing "<name>.wav": the original 44-byte WAV header followed by MP2 audio (music,
   tools/recode_music.py), decoded as it is read so the game still sees its PCM WAV. */
#include "../third_party/pl_mpeg.h"
enum { MM_MEM, MM_FILE, MM_MP2 };
#define MM_WIN (256 * 1024)
typedef struct {
    int kind;
    uint32_t size, pos;              /* virtual file */
    uint32_t win, wcap, woff, wlen;  /* window (guest memory) */
    FILE *f;
    uint8_t hdr[44];                 /* MP2: the WAV header */
    uint8_t *mp2; size_t mp2len;
    plm_buffer_t *pb; plm_audio_t *au;
    uint32_t au_pos;                 /* next stereo frame the decoder produces */
    int16_t pend[1152 * 2]; int npend, ipend;   /* decoded frames not yet consumed */
} MMFile;
static MMFile *mmf[64];
#define HMMIO_BASE 0x00720000u
static pthread_mutex_t mmmx = PTHREAD_MUTEX_INITIALIZER;
static MMFile *mm_get(uint32_t h) { return h >= HMMIO_BASE && h < HMMIO_BASE + 64 ? mmf[h - HMMIO_BASE] : NULL; }
int host_path(const char *winpath, char *out, size_t outsz, int for_create);

/* next PCM stereo frame from the MP2 decoder (silence past its end) */
static void mp2_frames(MMFile *m, int16_t *out, uint32_t n) {
    while (n) {
        if (m->ipend < m->npend) {
            uint32_t k = m->npend - m->ipend; if (k > n) k = n;
            memcpy(out, m->pend + 2 * m->ipend, k * 4);
            out += 2 * k; n -= k; m->ipend += k; m->au_pos += k;
            continue;
        }
        plm_samples_t *s = plm_audio_decode(m->au);
        if (!s) { memset(out, 0, n * 4); m->au_pos += n; return; }
        for (unsigned i = 0; i < s->count * 2; i++) {
            float f = s->interleaved[i] * 32767.0f;
            m->pend[i] = f > 32767 ? 32767 : f < -32768 ? -32768 : (int16_t)f;
        }
        m->npend = s->count; m->ipend = 0;
    }
}
static void mp2_seek(MMFile *m, uint32_t frame) {
    /* a window that ended mid-frame left the decoder one frame ahead: step back inside the decoded block */
    if (frame < m->au_pos && m->au_pos - frame <= (uint32_t)m->ipend) { m->ipend -= m->au_pos - frame; m->au_pos = frame; return; }
    if (frame < m->au_pos) { plm_audio_rewind(m->au); m->au_pos = 0; m->npend = m->ipend = 0; }
    static int16_t junk[4096 * 2];
    while (m->au_pos < frame) { uint32_t k = frame - m->au_pos; if (k > 4096) k = 4096; mp2_frames(m, junk, k); }
}
/* load the window with the file bytes starting at pos */
static void mm_fill(MMFile *m, uint32_t pos) {
    if (m->kind == MM_MEM) return;
    if (pos > m->size) pos = m->size;
    m->woff = pos;
    m->wlen = m->size - pos < m->wcap ? m->size - pos : m->wcap;
    uint8_t *w = GPTR(m->win);
    if (m->kind == MM_FILE) {
        fseek(m->f, pos, SEEK_SET);
        size_t r = fread(w, 1, m->wlen, m->f);
        if (r < m->wlen) memset(w + r, 0, m->wlen - r);
        return;
    }
    uint32_t i = 0;
    for (; i < m->wlen && pos + i < 44; i++) w[i] = m->hdr[pos + i];
    if (i < m->wlen) {
        uint32_t p = pos + i - 44, f0 = p / 4, skip = p % 4;
        mp2_seek(m, f0);
        uint32_t nbytes = m->wlen - i + skip, nfr = (nbytes + 3) / 4;
        static int16_t tmp[(MM_WIN / 4 + 2) * 2];
        mp2_frames(m, tmp, nfr);
        memcpy(w + i, (uint8_t *)tmp + skip, m->wlen - i);
        /* the decoder is now at f0 + nfr; a following window starts where this one ends */
    }
}
static int mm_in(MMFile *m, uint32_t pos) { return pos >= m->woff && pos < m->woff + m->wlen; }
static uint32_t mm_read(MMFile *m, uint8_t *dst, uint32_t n) {
    uint32_t done = 0;
    if (m->pos > m->size) m->pos = m->size;
    if (n > m->size - m->pos) n = m->size - m->pos;
    while (done < n) {
        if (!mm_in(m, m->pos)) mm_fill(m, m->pos);
        uint32_t k = m->woff + m->wlen - m->pos; if (k > n - done) k = n - done;
        if (!k) break;
        memcpy(dst + done, GPTR(m->win + (m->pos - m->woff)), k);
        done += k; m->pos += k;
    }
    return done;
}
static uint32_t mm_rd32(MMFile *m, uint32_t at) {
    uint32_t save = m->pos, v = 0; m->pos = at;
    if (mm_read(m, (uint8_t *)&v, 4) != 4) v = 0;
    m->pos = save;
    return v;
}
static uint32_t api_mmioOpenA(CPU *c) {
    uint32_t info = ARG(1), fl = ARG(2);
    MMFile *m = calloc(1, sizeof *m);
    const char *fn = gstr(ARG(0));
    if (info && gr32(info + 4) == 0x204d454du /* 'MEM ' */) {
        m->kind = MM_MEM; m->win = gr32(info + 24); m->size = m->wcap = m->wlen = gr32(info + 20);
    } else {
        char hp[1200];
        if (!fn || (fl & 0x3)) { free(m); if (info) gw16(info + 12, 257); return 0; }
        if (host_path(fn, hp, sizeof hp, 0) && (m->f = fopen(hp, "rb"))) {
            m->kind = MM_FILE;
            fseek(m->f, 0, SEEK_END); m->size = ftell(m->f);
        } else {
            char alt[600]; snprintf(alt, sizeof alt, "%s", fn);
            char *dot = strrchr(alt, '.');
            if (dot) strcpy(dot, ".mp2");
            FILE *f = NULL;
            if (dot && host_path(alt, hp, sizeof hp, 0)) f = fopen(hp, "rb");
            if (!f) { free(m); if (info) gw16(info + 12, 257); TRACE("mmioOpen %s: not found", fn); return 0; }
            fseek(f, 0, SEEK_END); m->mp2len = ftell(f); fseek(f, 0, SEEK_SET);
            m->mp2 = malloc(m->mp2len);
            if (fread(m->mp2, 1, m->mp2len, f) != m->mp2len || m->mp2len < 44) { fclose(f); free(m->mp2); free(m); return 0; }
            fclose(f);
            memcpy(m->hdr, m->mp2, 44);
            m->kind = MM_MP2;
            uint32_t datalen; memcpy(&datalen, m->hdr + 40, 4);
            m->size = 44 + datalen;
            m->pb = plm_buffer_create_with_memory(m->mp2 + 44, m->mp2len - 44, 0);
            m->au = plm_audio_create_with_buffer(m->pb, 0);
            LOG("mmio: %s streamed from %s (%u bytes of PCM)", fn, alt, datalen);
        }
        m->wcap = MM_WIN; m->win = gmalloc(MM_WIN + 16);
        m->woff = m->wlen = 0;
    }
    pthread_mutex_lock(&mmmx);
    for (int i = 0; i < 64; i++) if (!mmf[i]) { mmf[i] = m; pthread_mutex_unlock(&mmmx); return HMMIO_BASE + i; }
    pthread_mutex_unlock(&mmmx);
    return 0;
}
static uint32_t api_mmioClose(CPU *c) {
    MMFile *m = mm_get(ARG(0));
    if (!m) return 0;
    if (m->kind != MM_MEM) gfree(m->win);
    if (m->f) fclose(m->f);
    if (m->au) plm_audio_destroy(m->au);
    if (m->pb) plm_buffer_destroy(m->pb);
    free(m->mp2);
    free(m); mmf[ARG(0) - HMMIO_BASE] = NULL;
    return 0;
}
static uint32_t api_mmioRead(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 0xffffffffu;
    int32_t n = ARG(2);
    if (n < 0) return 0xffffffffu;
    if (m->kind == MM_MEM) {
        if ((uint32_t)n > m->size - m->pos) n = m->size - m->pos;
        memmove(GPTR(ARG(1)), GPTR(m->win + m->pos), n); m->pos += n;
        return n;
    }
    return mm_read(m, GPTR(ARG(1)), n);
}
static uint32_t api_mmioSeek(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 0xffffffffu;
    int32_t off = ARG(1); int64_t p = ARG(2) == 0 ? off : ARG(2) == 1 ? (int64_t)m->pos + off : (int64_t)m->size + off;
    if (p < 0 || p > m->size) return 0xffffffffu;
    m->pos = (uint32_t)p;
    return m->pos;
}
static uint32_t api_mmioDescend(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 257;
    uint32_t ck = ARG(1), parent = ARG(2), fl = ARG(3);
    uint32_t end = parent ? gr32(parent + 12) + gr32(parent + 4) : m->size;
    if (end > m->size) end = m->size;
    uint32_t want = gr32(ck), wantt = gr32(ck + 8);
    for (;;) {
        if (m->pos + 8 > end) return 265; /* MMIOERR_CHUNKNOTFOUND */
        uint32_t id = mm_rd32(m, m->pos), sz = mm_rd32(m, m->pos + 4), dat = m->pos + 8;
        int list = id == 0x46464952u /* RIFF */ || id == 0x5453494cu /* LIST */;
        uint32_t ty = list ? mm_rd32(m, dat) : 0;
        int match;
        if (fl & 0x20) match = id == 0x46464952u && ty == wantt;
        else if (fl & 0x40) match = id == 0x5453494cu && ty == wantt;
        else if (fl & 0x10) match = id == want;
        else match = 1;
        if (match) {
            gw32(ck, id); gw32(ck + 4, sz); gw32(ck + 8, ty); gw32(ck + 12, dat); gw32(ck + 16, 0);
            m->pos = dat + (list ? 4 : 0);
            return 0;
        }
        m->pos = dat + sz + (sz & 1);
    }
}
static uint32_t api_mmioAscend(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 257;
    uint32_t ck = ARG(1), e = gr32(ck + 12) + gr32(ck + 4);
    m->pos = e + (e & 1);
    if (m->pos > m->size) m->pos = m->size;
    return 0;
}
static void info_write(MMFile *m, uint32_t i) {
    gw32(i + 20, m->wcap); gw32(i + 24, m->win); gw32(i + 28, m->win + (m->pos - m->woff));
    gw32(i + 32, m->win + m->wlen); gw32(i + 36, m->win + m->wlen);
    gw32(i + 40, m->woff); gw32(i + 44, m->woff + m->wlen);
}
static uint32_t api_mmioGetInfo(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 257;
    uint32_t i = ARG(1);
    memset(GPTR(i), 0, 72);
    if (!mm_in(m, m->pos) && !(m->pos == m->woff + m->wlen && m->pos == m->size)) mm_fill(m, m->pos);
    info_write(m, i);
    gw32(i + 68, ARG(0));
    return 0;
}
static uint32_t api_mmioSetInfo(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 257;
    uint32_t nx = gr32(ARG(1) + 28);
    if (nx >= m->win && nx <= m->win + m->wlen) m->pos = m->woff + (nx - m->win);
    return 0;
}
static uint32_t api_mmioAdvance(CPU *c) {
    MMFile *m = mm_get(ARG(0)); if (!m) return 257;
    uint32_t i = ARG(1);
    uint32_t nx = gr32(i + 28);
    if (nx >= m->win && nx <= m->win + m->wlen) m->pos = m->woff + (nx - m->win);
    if (m->kind != MM_MEM) mm_fill(m, m->pos);
    info_write(m, i);   /* at the end of the file pchNext == pchEndRead, which the caller checks */
    return 0;
}

/* ================= version.dll ================= */
static uint32_t api_GetFileVersionInfoSizeA(CPU *c) { if (ARG(1)) gw32(ARG(1), 0); return 0; }

/* ================= ole32 ================= */
uint32_t video_create_stream(CPU *c, uint32_t riid, uint32_t ppv);   /* video.c: CLSID_AMMultiMediaStream */
static uint32_t api_CoCreateInstance(CPU *c) {
    uint32_t clsid = ARG(0), ppv = ARG(4);
    uint32_t d1 = gr32(clsid);
    if (d1 == 0x49c47ce5u) return video_create_stream(c, ARG(3), ppv);
    if (d1 == 0x47d4d946u) { extern uint32_t dsound_cocreate(CPU *, uint32_t); return dsound_cocreate(c, ppv); }
    LOG("CoCreateInstance(%08x-...) -> class not registered", d1);
    if (ppv) gw32(ppv, 0);
    return 0x80040154u;   /* REGDB_E_CLASSNOTREG */
}
static uint32_t api_CoCreateGuid(CPU *c) {
    for (int i = 0; i < 16; i++) gw8(ARG(0) + i, rand());
    return 0;
}

/* ================= TLS / thread ids ================= */
static __thread uint32_t tls_slots[64];
static uint32_t tls_used[64];
static uint32_t api_TlsAlloc(CPU *c) {
    for (int i = 0; i < 64; i++) if (!__atomic_exchange_n(&tls_used[i], 1, __ATOMIC_SEQ_CST)) return i;
    return 0xffffffffu;
}
static uint32_t api_TlsFree(CPU *c) { if (ARG(0) < 64) tls_used[ARG(0)] = 0; return 1; }
static uint32_t api_TlsGetValue(CPU *c) { c->last_error = 0; return ARG(0) < 64 ? tls_slots[ARG(0)] : 0; }
static uint32_t api_TlsSetValue(CPU *c) { if (ARG(0) >= 64) return 0; tls_slots[ARG(0)] = ARG(1); return 1; }
static uint32_t api_GetCurrentThreadId(CPU *c) { return c->thread_id + 1; }
static uint32_t api_SetLastError(CPU *c) { c->last_error = ARG(0); return 0; }
static uint32_t api_CLSIDFromString(CPU *c) { return 0x800401f3u; }

const ApiDef extra_apis[] = {
    {"kernel32.dll", "TlsAlloc", api_TlsAlloc, 0}, {"kernel32.dll", "TlsFree", api_TlsFree, 4},
    {"kernel32.dll", "TlsGetValue", api_TlsGetValue, 4}, {"kernel32.dll", "TlsSetValue", api_TlsSetValue, 8},
    {"kernel32.dll", "GetCurrentThreadId", api_GetCurrentThreadId, 0}, {"kernel32.dll", "SetLastError", api_SetLastError, 4},
    {"user32.dll", "KillTimer", ret1, 8}, {"gdi32.dll", "Rectangle", ret1, 20}, {"gdi32.dll", "CreateDCA", ret0, 16},
    {"gdi32.dll", "SetROP2", ret1, 8}, {"ole32.dll", "CoTaskMemFree", ret0, 4}, {"ole32.dll", "CLSIDFromString", api_CLSIDFromString, 8},
    {"ole32.dll", "StringFromCLSID", api_CLSIDFromString, 8},
    {"advapi32.dll", "RegOpenKeyExA", api_RegOpenKeyExA, 20}, {"advapi32.dll", "RegCreateKeyExA", api_RegCreateKeyExA, 36},
    {"advapi32.dll", "RegQueryValueExA", api_RegQueryValueExA, 24}, {"advapi32.dll", "RegSetValueExA", api_RegSetValueExA, 24},
    {"advapi32.dll", "RegCloseKey", ret0, 4}, {"advapi32.dll", "RegFlushKey", ret0, 4},
    {"advapi32.dll", "GetUserNameA", api_GetUserNameA, 8},
    {"kernel32.dll", "lstrlenA", api_lstrlenA, 4}, {"kernel32.dll", "lstrcmpA", api_lstrcmpA, 8},
    {"kernel32.dll", "GetTempFileNameA", api_GetTempFileNameA, 16}, {"kernel32.dll", "GlobalLock", api_GlobalLock, 4},
    {"kernel32.dll", "GlobalHandle", api_GlobalHandle, 4}, {"kernel32.dll", "GlobalUnlock", ret1, 4},
    {"kernel32.dll", "HeapSize", api_HeapSize, 12},
    {"user32.dll", "GetKeyboardState", api_GetKeyboardState, 4}, {"user32.dll", "GetAsyncKeyState", api_GetAsyncKeyState, 4},
    {"user32.dll", "GetKeyboardLayout", api_GetKeyboardLayout, 4}, {"user32.dll", "MapVirtualKeyExA", api_MapVirtualKeyExA, 12},
    {"user32.dll", "ToAsciiEx", api_ToAsciiEx, 24}, {"user32.dll", "GetKeyNameTextA", api_GetKeyNameTextA, 12},
    {"user32.dll", "AdjustWindowRect", api_AdjustWindowRect, 12}, {"user32.dll", "OffsetRect", api_OffsetRect, 12},
    {"user32.dll", "SetDlgItemTextA", ret1, 12}, {"user32.dll", "LoadBitmapA", ret0, 8},
    {"winmm.dll", "timeSetEvent", api_timeSetEvent, 20}, {"winmm.dll", "timeKillEvent", api_timeKillEvent, 4},
    {"winmm.dll", "timeBeginPeriod", ret0, 4}, {"winmm.dll", "timeEndPeriod", ret0, 4},
    {"winmm.dll", "mciSendCommandA", api_mciSendCommandA, 16},
    {"winmm.dll", "mmioOpenA", api_mmioOpenA, 12}, {"winmm.dll", "mmioClose", api_mmioClose, 8},
    {"winmm.dll", "mmioRead", api_mmioRead, 12}, {"winmm.dll", "mmioSeek", api_mmioSeek, 12},
    {"winmm.dll", "mmioDescend", api_mmioDescend, 16}, {"winmm.dll", "mmioAscend", api_mmioAscend, 12},
    {"winmm.dll", "mmioGetInfo", api_mmioGetInfo, 12}, {"winmm.dll", "mmioSetInfo", api_mmioSetInfo, 12},
    {"winmm.dll", "mmioAdvance", api_mmioAdvance, 12},
    {"version.dll", "GetFileVersionInfoSizeA", api_GetFileVersionInfoSizeA, 8},
    {"version.dll", "GetFileVersionInfoA", ret0, 16}, {"version.dll", "VerQueryValueA", ret0, 16},
    {"ole32.dll", "CoInitialize", ret0, 4}, {"ole32.dll", "CoUninitialize", ret0, 0},
    {"ole32.dll", "CoCreateInstance", api_CoCreateInstance, 20}, {"ole32.dll", "CoCreateGuid", api_CoCreateGuid, 4},
    { 0 }
};
const ApiDef winmm_apis[] = { { 0 } };
const ApiDef ole_apis[] = { { 0 } };
