/* DirectShow multimedia streaming (amstream: IAMMultiMediaStream / IDirectDrawMediaStream /
 * IDirectDrawStreamSample), the way the game plays its intro videos. The Indeo AVIs are re-encoded at
 * build time to MPEG-1 + MP2 (tools/recode_video.py, "<name>.mpg" next to the .avi) and decoded here
 * with pl_mpeg. The soundtrack is decoded up front and mixed by the DirectSound mixer; it is also the
 * playback clock, so picture and sound stay in sync. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PL_MPEG_IMPLEMENTATION
#include "../third_party/pl_mpeg.h"
#include <pthread.h>
#include "win32.h"
#include "../platform/plat.h"

#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define MS_S_ENDOFSTREAM 0x00040003u

int host_path(const char *winpath, char *out, size_t outsz, int for_create);
int dsound_ensure_audio(void);
int ddraw_surface_size(uint32_t obj, int *w, int *h);
void ddraw_surface_write(uint32_t obj, int x, int y, int w, int h, const uint8_t *rgba);
void plat_audio_lock(void);
void plat_audio_unlock(void);

typedef struct {
    uint32_t mms, vstream, astream, sample;   /* guest COM objects */
    plm_t *plm;
    int w, h;
    double fps;
    int16_t *pcm; uint32_t nsamples;          /* stereo frames, 44.1 kHz */
    volatile uint64_t apos;                   /* audio position (stereo frames) */
    volatile int playing;
    uint32_t t0;                              /* wall clock start when there is no audio */
    int frame;                                /* frames shown */
    uint8_t *rgba;
    uint32_t surf; int sx, sy;                /* sample target */
    int ended;
    int refs;
} Movie;

static uint32_t vt_mms, vt_ddms, vt_ams, vt_samp;
static Movie *g_mov;          /* the movie whose sound is mixed */

/* ---- audio: called by the DirectSound mixer ---- */
void video_mix(int32_t *acc, int frames, int rate) {
    Movie *m = g_mov;
    if (!m || !m->playing || !m->pcm || rate != 44100) return;
    uint64_t p = m->apos;
    for (int i = 0; i < frames && p < m->nsamples; i++, p++) {
        acc[2 * i] += m->pcm[2 * p]; acc[2 * i + 1] += m->pcm[2 * p + 1];
    }
    m->apos = p;
}
static double movie_clock(Movie *m) {
    if (m->pcm && m->apos < m->nsamples) return m->apos / 44100.0;
    return (plat_ticks() - m->t0) / 1000.0;
}

static Movie *mov_of(CPU *c) { return (Movie *)com_host(ARG(0)); }
static uint32_t ok(CPU *c) { return S_OK; }
static uint32_t one(CPU *c) { return 1; }

static int open_movie(Movie *m, const char *winpath) {
    char hp[1200], alt[1200];
    snprintf(alt, sizeof alt, "%s", winpath);
    char *dot = strrchr(alt, '.');
    if (dot) strcpy(dot, ".mpg");
    if (!host_path(alt, hp, sizeof hp, 0)) { LOG("video: %s not found (re-encoded videos missing?)", alt); return 0; }
    m->plm = plm_create_with_filename(hp);
    if (!m->plm || !plm_probe(m->plm, 5000 * 1024)) { LOG("video: cannot open %s", hp); return 0; }
    m->w = plm_get_width(m->plm); m->h = plm_get_height(m->plm); m->fps = plm_get_framerate(m->plm);
    if (m->fps <= 0) m->fps = 25;
    plm_set_audio_enabled(m->plm, 0);
    m->rgba = malloc((size_t)m->w * m->h * 4);
    /* the soundtrack, decoded up front */
    plm_t *a = plm_create_with_filename(hp);
    if (a && plm_get_num_audio_streams(a) > 0) {
        plm_set_video_enabled(a, 0);
        plm_set_audio_enabled(a, 1);
        size_t cap = 44100 * 2 * 8, n = 0;
        int16_t *pcm = malloc(cap * 2);
        plm_samples_t *s;
        while ((s = plm_decode_audio(a))) {
            if (n + s->count * 2 > cap) { cap = cap * 2 + s->count * 2; pcm = realloc(pcm, cap * 2); }
            for (unsigned i = 0; i < s->count * 2; i++) {
                float f = s->interleaved[i] * 32767.0f;
                pcm[n++] = f > 32767 ? 32767 : f < -32768 ? -32768 : (int16_t)f;
            }
        }
        m->pcm = pcm; m->nsamples = n / 2;
    }
    if (a) plm_destroy(a);
    LOG("video: %s %dx%d %.2f fps, %.1f s of sound", hp, m->w, m->h, m->fps, m->nsamples / 44100.0);
    return 1;
}

/* ---- IAMMultiMediaStream ---- */
static uint32_t mms_QueryInterface(CPU *c) { mov_of(c)->refs++; gw32(ARG(2), ARG(0)); return S_OK; }
static uint32_t mms_AddRef(CPU *c) { return ++mov_of(c)->refs; }
static uint32_t mms_Release(CPU *c) {
    Movie *m = mov_of(c);
    if (--m->refs > 0) return m->refs;
    LOG("video: released after %d frames, %.1f s (%s)", m->frame, movie_clock(m), m->ended ? "ended" : "stopped");
    if (g_mov == m) { plat_audio_lock(); g_mov = NULL; plat_audio_unlock(); }
    m->playing = 0;
    if (m->plm) plm_destroy(m->plm);
    m->plm = NULL;
    free(m->pcm); m->pcm = NULL;
    free(m->rgba); m->rgba = NULL;
    return 0;
}
static uint32_t mms_AddMediaStream(CPU *c) {
    Movie *m = mov_of(c);
    uint32_t purpose = ARG(2), out = ARG(4);
    uint32_t d1 = gr32(purpose);
    /* MSPID_PrimaryVideo a35ff56a-..., MSPID_PrimaryAudio a35ff56b-... */
    uint32_t s = 0;
    if (d1 == 0xa35ff56au) { if (!m->vstream) m->vstream = com_new(vt_ddms, 0, m); s = m->vstream; }
    else if (d1 == 0xa35ff56bu) { if (!m->astream) m->astream = com_new(vt_ams, 0, m); s = m->astream; }
    else { LOG("video: AddMediaStream purpose %08x", d1); return E_FAIL; }
    if (out) gw32(out, s);
    return S_OK;
}
static uint32_t mms_OpenFile(CPU *c) {
    Movie *m = mov_of(c);
    char path[600]; int i = 0;
    for (uint32_t p = ARG(1); i < 599 && gr16(p); p += 2) path[i++] = (char)gr16(p);
    path[i] = 0;
    return open_movie(m, path) ? S_OK : E_FAIL;
}
static uint32_t mms_GetMediaStream(CPU *c) {
    Movie *m = mov_of(c);
    uint32_t d1 = gr32(ARG(1)), s = d1 == 0xa35ff56au ? m->vstream : d1 == 0xa35ff56bu ? m->astream : 0;
    gw32(ARG(2), s);
    return s ? S_OK : 0x80040201u; /* MS_E_NOSTREAM */
}
static uint32_t mms_SetState(CPU *c) {
    Movie *m = mov_of(c);
    if (ARG(1) == 1) {   /* STREAMSTATE_RUN */
        if (!m->playing) {
            m->t0 = plat_ticks();
            if (m->pcm && dsound_ensure_audio()) { plat_audio_lock(); g_mov = m; m->playing = 1; plat_audio_unlock(); }
            else { m->playing = 1; free(m->pcm); m->pcm = NULL; }
        }
    } else {
        plat_audio_lock(); m->playing = 0; plat_audio_unlock();
    }
    return S_OK;
}
static uint32_t mms_GetState(CPU *c) { gw32(ARG(1), mov_of(c)->playing ? 1 : 0); return S_OK; }
static uint32_t mms_GetTime(CPU *c) {
    uint64_t t = (uint64_t)(movie_clock(mov_of(c)) * 1e7);
    gw32(ARG(1), (uint32_t)t); gw32(ARG(1) + 4, (uint32_t)(t >> 32));
    return S_OK;
}
static uint32_t mms_GetDuration(CPU *c) {
    Movie *m = mov_of(c);
    uint64_t t = m->plm ? (uint64_t)(plm_get_duration(m->plm) * 1e7) : 0;
    gw32(ARG(1), (uint32_t)t); gw32(ARG(1) + 4, (uint32_t)(t >> 32));
    return S_OK;
}
static uint32_t mms_GetInformation(CPU *c) { if (ARG(1)) gw32(ARG(1), 0x3); if (ARG(2)) gw32(ARG(2), 0); return S_OK; }

/* ---- IDirectDrawMediaStream ---- */
static uint32_t ds_GetFormat(CPU *c) {
    Movie *m = mov_of(c);
    uint32_t d = ARG(1);
    if (d) {
        uint32_t n = gr32(d) >= 108 ? gr32(d) : 108;
        memset(GPTR(d), 0, n); gw32(d, n);
        gw32(d + 4, 0x1 | 0x2 | 0x4 | 0x1000);
        gw32(d + 8, m->h); gw32(d + 12, m->w);
        uint32_t pf = d + 72;
        gw32(pf, 32); gw32(pf + 4, 0x40); gw32(pf + 12, 16); gw32(pf + 16, 0xf800); gw32(pf + 20, 0x7e0); gw32(pf + 24, 0x1f);
        gw32(d + 104, 0x40 | 0x800);
    }
    if (ARG(2)) gw32(ARG(2), 0);
    if (ARG(3)) { uint32_t e = ARG(3); uint32_t n = gr32(e) >= 108 ? gr32(e) : 108; memcpy(GPTR(e), GPTR(d ? d : e), n); }
    if (ARG(4)) gw32(ARG(4), 0);
    return S_OK;
}
static uint32_t ds_SetFormat(CPU *c) { return S_OK; }
static uint32_t ds_CreateSample(CPU *c) {
    Movie *m = mov_of(c);
    m->surf = ARG(1);
    m->sx = m->sy = 0;
    if (!m->surf) { extern uint32_t ddraw_create_offscreen(int, int); m->surf = ddraw_create_offscreen(m->w, m->h); }
    if (ARG(2)) { m->sx = gr32(ARG(2)); m->sy = gr32(ARG(2) + 4); }
    if (!m->sample) m->sample = com_new(vt_samp, 0, m);
    gw32(ARG(4), m->sample);
    int sw = 0, sh = 0; ddraw_surface_size(m->surf, &sw, &sh);
    LOG("video: sample on surface %08x (%dx%d) at %d,%d", m->surf, sw, sh, m->sx, m->sy);
    return S_OK;
}
static uint32_t ds_GetTimePerFrame(CPU *c) {
    uint64_t t = (uint64_t)(1e7 / mov_of(c)->fps);
    gw32(ARG(1), (uint32_t)t); gw32(ARG(1) + 4, (uint32_t)(t >> 32));
    return S_OK;
}
/* the stream objects are already the interfaces asked for (IMediaStream -> IDirectDrawMediaStream etc.) */
static uint32_t self_QueryInterface(CPU *c) { gw32(ARG(2), ARG(0)); return S_OK; }
static uint32_t ds_GetMultiMediaStream(CPU *c) { gw32(ARG(1), mov_of(c)->mms); return S_OK; }

/* ---- IDirectDrawStreamSample ---- */
static uint32_t smp_Update(CPU *c) {
    Movie *m = mov_of(c);
    if (!m->plm || m->ended) return MS_S_ENDOFSTREAM;
    /* the frame due now (decode, dropping none: each frame depends on the previous ones) */
    double due = m->frame / m->fps;
    double now = movie_clock(m);
    while (now < due && m->playing) {
        uint32_t ms = (uint32_t)((due - now) * 1000);
        plat_sleep(ms > 20 ? 20 : ms ? ms : 1);
        plat_pump_events();
        now = movie_clock(m);
        if (m->pcm && m->apos >= m->nsamples) break;
    }
    plm_frame_t *f = plm_decode_video(m->plm);
    if (!f) { m->ended = 1; return MS_S_ENDOFSTREAM; }
    m->frame++;
    /* behind by more than a frame: skip ahead (decoding still happens, only showing is skipped) */
    while ((m->frame + 1) / m->fps < movie_clock(m) - 0.1) {
        plm_frame_t *g = plm_decode_video(m->plm);
        if (!g) break;
        f = g; m->frame++;
    }
    plm_frame_to_rgba(f, m->rgba, m->w * 4);
    ddraw_surface_write(m->surf, m->sx, m->sy, m->w, m->h, m->rgba);
    return S_OK;
}
static uint32_t smp_GetSurface(CPU *c) {
    Movie *m = mov_of(c);
    if (ARG(1)) gw32(ARG(1), m->surf);
    if (ARG(2)) { gw32(ARG(2), m->sx); gw32(ARG(2) + 4, m->sy); gw32(ARG(2) + 8, m->sx + m->w); gw32(ARG(2) + 12, m->sy + m->h); }
    return S_OK;
}
static uint32_t smp_CompletionStatus(CPU *c) { return S_OK; }

#define N(a) (int)(sizeof a / sizeof *a)
static void build(void) {
    if (vt_mms) return;
    static const ComMethod mms[] = {
        {"QueryInterface", mms_QueryInterface, 3}, {"AddRef", mms_AddRef, 1}, {"Release", mms_Release, 1},
        {"GetInformation", mms_GetInformation, 3}, {"GetMediaStream", mms_GetMediaStream, 3}, {"EnumMediaStreams", NULL, 3},
        {"GetState", mms_GetState, 2}, {"SetState", mms_SetState, 2}, {"GetTime", mms_GetTime, 2},
        {"GetDuration", mms_GetDuration, 2}, {"Seek", NULL, 3}, {"GetEndOfStreamEventHandle", NULL, 2},
        {"Initialize", ok, 4}, {"GetFilterGraph", NULL, 2}, {"GetFilter", NULL, 2},
        {"AddMediaStream", mms_AddMediaStream, 5}, {"OpenFile", mms_OpenFile, 3}, {"OpenMoniker", NULL, 4}, {"Render", ok, 2},
    };
    vt_mms = com_make_vtbl("IAMMultiMediaStream", mms, N(mms));
    static const ComMethod ddms[] = {
        {"QueryInterface", self_QueryInterface, 3}, {"AddRef", one, 1}, {"Release", ok, 1},
        {"GetMultiMediaStream", ds_GetMultiMediaStream, 2}, {"GetInformation", NULL, 3}, {"SetSameFormat", NULL, 3},
        {"AllocateSample", NULL, 3}, {"CreateSharedSample", NULL, 4}, {"SendEndOfStream", ok, 2},
        {"GetFormat", ds_GetFormat, 5}, {"SetFormat", ds_SetFormat, 3}, {"GetDirectDraw", NULL, 2}, {"SetDirectDraw", ok, 2},
        {"CreateSample", ds_CreateSample, 5}, {"GetTimePerFrame", ds_GetTimePerFrame, 2},
    };
    vt_ddms = com_make_vtbl("IDirectDrawMediaStream", ddms, N(ddms));
    static const ComMethod ams[] = {
        {"QueryInterface", self_QueryInterface, 3}, {"AddRef", one, 1}, {"Release", ok, 1},
        {"GetMultiMediaStream", ds_GetMultiMediaStream, 2}, {"GetInformation", NULL, 3}, {"SetSameFormat", NULL, 3},
        {"AllocateSample", NULL, 3}, {"CreateSharedSample", NULL, 4}, {"SendEndOfStream", ok, 2},
    };
    vt_ams = com_make_vtbl("IAudioMediaStream", ams, N(ams));
    static const ComMethod smp[] = {
        {"QueryInterface", self_QueryInterface, 3}, {"AddRef", one, 1}, {"Release", ok, 1},
        {"GetMediaStream", NULL, 2}, {"GetSampleTimes", NULL, 4}, {"SetSampleTimes", NULL, 3},
        {"Update", smp_Update, 5}, {"CompletionStatus", smp_CompletionStatus, 3},
        {"GetSurface", smp_GetSurface, 3}, {"SetRect", NULL, 2},
    };
    vt_samp = com_make_vtbl("IDirectDrawStreamSample", smp, N(smp));
}

uint32_t video_create_stream(CPU *c, uint32_t riid, uint32_t ppv) {
    if (getenv("C2_NOVIDEO")) { gw32(ppv, 0); return 0x80040154u; }   /* testing: skip the videos */
    build();
    Movie *m = calloc(1, sizeof *m);
    m->refs = 1;
    m->mms = com_new(vt_mms, 0, m);
    gw32(ppv, m->mms);
    return S_OK;
}
