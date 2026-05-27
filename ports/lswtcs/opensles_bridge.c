/* Minimal OpenSL ES -> OpenAL bridge for libTTapp.so.
 *
 * libTTapp uses a small slice of OpenSL ES for streaming audio:
 *   - slCreateEngine() -> SLObjectItf engine
 *   - engine.GetInterface(SL_IID_ENGINE) -> SLEngineItf
 *   - engine.CreateOutputMix() -> output mix object (we ignore the body)
 *   - engine.CreateAudioPlayer(source=AndroidSimpleBufferQueue+PCM, sink=OutputMix)
 *       -> player object
 *   - player.GetInterface(SL_IID_PLAY)                       -> Play state
 *   - player.GetInterface(SL_IID_VOLUME)                     -> Volume
 *   - player.GetInterface(SL_IID_ANDROIDSIMPLEBUFFERQUEUE)   -> queue
 *   - queue.RegisterCallback(cb, ctx)
 *   - queue.Enqueue(buf, size)  // streams PCM in chunks; cb fires when consumed
 *
 * Everything else (env reverb, capabilities, etc.) is stubbed to "no".
 *
 * Backed by OpenAL: each AudioPlayer maps to one alSource, the buffer
 * queue maps to alSourceQueueBuffers + a tiny pool of alBuffers we
 * recycle as the source consumes them. A single 100Hz pump thread
 * drains finished buffers across all players and invokes their
 * callbacks (the engine uses those callbacks to refill the queue).
 *
 * Headers are intentionally NOT included for AL/SL - we just forward-
 * declare the symbols we use, so the file builds in any chroot that
 * has libopenal-dev installed at link time. */

#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <pthread.h>

#include "platform.h"
#include "so_util.h"

/* ---- OpenAL forward decls ---------------------------------------- */
typedef int    ALint;
typedef unsigned int ALuint;
typedef int    ALsizei;
typedef int    ALenum;
typedef float  ALfloat;
typedef void   ALvoid;

#define AL_FORMAT_MONO16     0x1101
#define AL_FORMAT_STEREO16   0x1103
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_SOURCE_STATE      0x1010
#define AL_PLAYING           0x1012
#define AL_PAUSED            0x1013
#define AL_GAIN              0x100A
#define AL_LOOPING           0x1007

extern void  alGenSources(ALsizei, ALuint *);
extern void  alDeleteSources(ALsizei, const ALuint *);
extern void  alGenBuffers(ALsizei, ALuint *);
extern void  alDeleteBuffers(ALsizei, const ALuint *);
extern void  alBufferData(ALuint, ALenum, const ALvoid *, ALsizei, ALsizei);
extern void  alSourceQueueBuffers(ALuint, ALsizei, const ALuint *);
extern void  alSourceUnqueueBuffers(ALuint, ALsizei, ALuint *);
extern void  alSourcei(ALuint, ALenum, ALint);
extern void  alSourcef(ALuint, ALenum, ALfloat);
extern void  alGetSourcei(ALuint, ALenum, ALint *);
extern void  alSourcePlay(ALuint);
extern void  alSourcePause(ALuint);
extern void  alSourceStop(ALuint);

extern void *alcOpenDevice(const char *);
extern void *alcCreateContext(void *, const int *);
extern int   alcMakeContextCurrent(void *);
extern void  alcDestroyContext(void *);
extern int   alcCloseDevice(void *);

/* ---- OpenSL ES forward decls (only what we need) ----------------- */
typedef unsigned int  SLuint32;
typedef int           SLint32;
typedef int           SLresult;
typedef unsigned char SLboolean;
typedef float         SLmillibel;   /* gain in 1/100 dB */
typedef unsigned char SLuint8;
typedef short         SLint16;

#define SL_RESULT_SUCCESS               0
#define SL_RESULT_PARAMETER_INVALID     5
#define SL_RESULT_FEATURE_UNSUPPORTED   12
#define SL_BOOLEAN_TRUE                 1
#define SL_BOOLEAN_FALSE                0

#define SL_PLAYSTATE_STOPPED            1
#define SL_PLAYSTATE_PAUSED             2
#define SL_PLAYSTATE_PLAYING            3

typedef const struct SLInterfaceID_ *SLInterfaceID;
typedef const struct SLObjectItf_       *const *SLObjectItf;
typedef const struct SLEngineItf_       *const *SLEngineItf;
typedef const struct SLPlayItf_         *const *SLPlayItf;
typedef const struct SLVolumeItf_       *const *SLVolumeItf;
typedef const struct SLBufferQueueItf_  *const *SLBufferQueueItf;

/* The 6 SL_IID_* the .so references. Their addresses are looked up via
 * symtable_lswtcs so each must be a stable, distinct pointer. */
static const char tt_sl_iid_engine_d[16]              = "TT_SL_ENGINE___";
static const char tt_sl_iid_enginecapabilities_d[16]  = "TT_SL_ENGINECAP";
static const char tt_sl_iid_envreverb_d[16]           = "TT_SL_REVERB___";
static const char tt_sl_iid_volume_d[16]              = "TT_SL_VOLUME___";
static const char tt_sl_iid_androidsbq_d[16]          = "TT_SL_SBQ______";
static const char tt_sl_iid_play_d[16]                = "TT_SL_PLAY_____";

/* Public symbols overriden by symtable_lswtcs - we expose them so the
 * resolver can hand the .so the exact same pointer the engine code
 * later compares against in GetInterface. */
const void * tt_SL_IID_ENGINE                  = tt_sl_iid_engine_d;
const void * tt_SL_IID_ENGINECAPABILITIES      = tt_sl_iid_enginecapabilities_d;
const void * tt_SL_IID_ENVIRONMENTALREVERB     = tt_sl_iid_envreverb_d;
const void * tt_SL_IID_VOLUME                  = tt_sl_iid_volume_d;
const void * tt_SL_IID_ANDROIDSIMPLEBUFFERQUEUE = tt_sl_iid_androidsbq_d;
const void * tt_SL_IID_PLAY                    = tt_sl_iid_play_d;

/* ---- Common object layout ---------------------------------------- *
 * Every SL object is "vtable pointer at offset 0", same as the spec.
 * We embed the vtable directly so we can return &obj->itf_obj to the
 * caller. */

typedef void (*tt_sl_buf_cb)(SLBufferQueueItf, void *);

typedef enum { TT_OBJ_ENGINE, TT_OBJ_OUTPUTMIX, TT_OBJ_PLAYER } tt_obj_kind;

typedef struct tt_obj {
    /* The SLObjectItf the caller gets after Realize/GetInterface. */
    const struct SLObjectItf_ *itf_obj;
    /* For SLEngineItf / SLPlayItf / SLVolumeItf / SLBufferQueueItf,
     * each is a small struct holding its vtable pointer; GetInterface
     * just returns its address. */
    const struct SLEngineItf_       *itf_engine;
    const struct SLPlayItf_         *itf_play;
    const struct SLVolumeItf_       *itf_volume;
    const struct SLBufferQueueItf_  *itf_queue;

    tt_obj_kind kind;

    /* AudioPlayer-only state */
    ALuint      al_source;
    int         channels;     /* 1 or 2 */
    int         sample_rate;
    int         bits;         /* always 16 for now */
    SLuint32    play_state;
    SLmillibel  volume_mb;
    /* AL buffers we own. free_bufs[] is a stack of currently-unused
     * buffer IDs; bq_Enqueue pops, the pump pushes when a buffer is
     * drained. Reusing a buffer while AL is still playing from it
     * corrupts the stream - so the free list is mandatory. */
    ALuint      al_bufs[8];
    ALuint      free_bufs[8];
    int         buf_count;
    int         free_count;
    /* callback the engine sets via RegisterCallback */
    tt_sl_buf_cb cb;
    void        *cb_ctx;
    /* pending engine-buffer count - used to satisfy GetState */
    int         queued;

    struct tt_obj *next;       /* for the global player list (pump thread) */
} tt_obj;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static tt_obj *s_players_head = NULL;
static void *s_al_dev = NULL;
static void *s_al_ctx = NULL;
static pthread_t s_pump_tid;
static int s_pump_running = 0;

/* Drain finished AL buffers across every active player and invoke the
 * engine's "buffer consumed, refill me" callback for each. */
static void *tt_audio_pump(void *unused)
{
    (void)unused;
    while (s_pump_running) {
        pthread_mutex_lock(&s_lock);
        for (tt_obj *p = s_players_head; p; p = p->next) {
            if (!p->al_source) continue;
            ALint processed = 0;
            alGetSourcei(p->al_source, AL_BUFFERS_PROCESSED, &processed);
            while (processed-- > 0) {
                ALuint b = 0;
                alSourceUnqueueBuffers(p->al_source, 1, &b);
                if (p->queued > 0) p->queued--;
                if (p->free_count < p->buf_count)
                    p->free_bufs[p->free_count++] = b;
                if (p->cb) p->cb(&p->itf_queue, p->cb_ctx);
            }
            /* Restart the source if it underran while we were sleeping. */
            if (p->play_state == SL_PLAYSTATE_PLAYING) {
                ALint state = 0;
                alGetSourcei(p->al_source, AL_SOURCE_STATE, &state);
                if (state != AL_PLAYING && p->queued > 0)
                    alSourcePlay(p->al_source);
            }
        }
        pthread_mutex_unlock(&s_lock);
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* ---- BufferQueueItf vtable --------------------------------------- */

ABI_ATTR static SLresult bq_Enqueue(SLBufferQueueItf self, const void *buf, SLuint32 size)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_queue));
    if (!p->al_source || !buf || !size || p->buf_count <= 0)
        return SL_RESULT_PARAMETER_INVALID;
    pthread_mutex_lock(&s_lock);
    if (p->free_count == 0) {
        /* Queue is saturated - drop this enqueue. Better than overwriting
         * an in-flight buffer; the engine will retry on the next callback. */
        pthread_mutex_unlock(&s_lock);
        return SL_RESULT_PARAMETER_INVALID;
    }
    ALuint b = p->free_bufs[--p->free_count];
    ALenum fmt = (p->channels == 2) ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    alBufferData(b, fmt, buf, (ALsizei)size, p->sample_rate);
    alSourceQueueBuffers(p->al_source, 1, &b);
    p->queued++;
    if (p->play_state == SL_PLAYSTATE_PLAYING) {
        ALint state = 0;
        alGetSourcei(p->al_source, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING) alSourcePlay(p->al_source);
    }
    pthread_mutex_unlock(&s_lock);
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult bq_Clear(SLBufferQueueItf self)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_queue));
    pthread_mutex_lock(&s_lock);
    if (p->al_source) {
        alSourceStop(p->al_source);
        /* Stopping marks every queued buffer "processed", so we can
         * unqueue them all and push back onto the free list. */
        ALint processed = 0;
        alGetSourcei(p->al_source, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint b = 0;
            alSourceUnqueueBuffers(p->al_source, 1, &b);
            if (p->free_count < p->buf_count)
                p->free_bufs[p->free_count++] = b;
        }
        p->queued = 0;
    }
    pthread_mutex_unlock(&s_lock);
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult bq_GetState(SLBufferQueueItf self, void *state)
{
    /* state is { SLuint32 count, SLuint32 playIndex }. Fill both. */
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_queue));
    if (state) {
        SLuint32 *out = (SLuint32 *)state;
        out[0] = p->queued;
        out[1] = 0;
    }
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult bq_RegisterCallback(SLBufferQueueItf self,
                                             tt_sl_buf_cb cb, void *ctx)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_queue));
    p->cb = cb;
    p->cb_ctx = ctx;
    return SL_RESULT_SUCCESS;
}

struct SLBufferQueueItf_ {
    SLresult (*Enqueue)(SLBufferQueueItf, const void *, SLuint32);
    SLresult (*Clear)(SLBufferQueueItf);
    SLresult (*GetState)(SLBufferQueueItf, void *);
    SLresult (*RegisterCallback)(SLBufferQueueItf, tt_sl_buf_cb, void *);
};

static const struct SLBufferQueueItf_ tt_bq_vtable = {
    .Enqueue = bq_Enqueue,
    .Clear = bq_Clear,
    .GetState = bq_GetState,
    .RegisterCallback = bq_RegisterCallback,
};

/* ---- PlayItf vtable --------------------------------------------- */

ABI_ATTR static SLresult play_SetPlayState(SLPlayItf self, SLuint32 state)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_play));
    p->play_state = state;
    if (!p->al_source) return SL_RESULT_SUCCESS;
    switch (state) {
        case SL_PLAYSTATE_PLAYING: alSourcePlay(p->al_source);  break;
        case SL_PLAYSTATE_PAUSED:  alSourcePause(p->al_source); break;
        case SL_PLAYSTATE_STOPPED: alSourceStop(p->al_source);  break;
    }
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult play_GetPlayState(SLPlayItf self, SLuint32 *out)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_play));
    if (out) *out = p->play_state;
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult sl_unused(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

/* SLPlayItf - 12 slots per the spec. Slots we don't implement still
 * have to exist or the engine indexes past the vtable into garbage. */
struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf, SLuint32);
    SLresult (*GetPlayState)(SLPlayItf, SLuint32 *);
    SLresult (*GetDuration)(SLPlayItf, void *);
    SLresult (*GetPosition)(SLPlayItf, void *);
    SLresult (*RegisterCallback)(SLPlayItf, void *, void *);
    SLresult (*SetCallbackEventsMask)(SLPlayItf, SLuint32);
    SLresult (*GetCallbackEventsMask)(SLPlayItf, SLuint32 *);
    SLresult (*SetMarkerPosition)(SLPlayItf, SLuint32);
    SLresult (*ClearMarkerPosition)(SLPlayItf);
    SLresult (*GetMarkerPosition)(SLPlayItf, SLuint32 *);
    SLresult (*SetPositionUpdatePeriod)(SLPlayItf, SLuint32);
    SLresult (*GetPositionUpdatePeriod)(SLPlayItf, SLuint32 *);
};

static const struct SLPlayItf_ tt_play_vtable = {
    .SetPlayState            = play_SetPlayState,
    .GetPlayState            = play_GetPlayState,
    .GetDuration             = (void *)sl_unused,
    .GetPosition             = (void *)sl_unused,
    .RegisterCallback        = (void *)sl_unused,
    .SetCallbackEventsMask   = (void *)sl_unused,
    .GetCallbackEventsMask   = (void *)sl_unused,
    .SetMarkerPosition       = (void *)sl_unused,
    .ClearMarkerPosition     = (void *)sl_unused,
    .GetMarkerPosition       = (void *)sl_unused,
    .SetPositionUpdatePeriod = (void *)sl_unused,
    .GetPositionUpdatePeriod = (void *)sl_unused,
};

/* ---- VolumeItf vtable ------------------------------------------- */

ABI_ATTR static SLresult vol_SetVolumeLevel(SLVolumeItf self, SLmillibel level)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_volume));
    p->volume_mb = level;
    if (p->al_source) {
        /* OpenSL ES level is in 1/100 dB, range -9600..0. Convert to gain. */
        float gain = (level <= -9600) ? 0.0f : powf(10.0f, level / 2000.0f);
        if (gain > 1.0f) gain = 1.0f;
        alSourcef(p->al_source, AL_GAIN, gain);
    }
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult vol_GetVolumeLevel(SLVolumeItf self, SLmillibel *out)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_volume));
    if (out) *out = p->volume_mb;
    return SL_RESULT_SUCCESS;
}

/* SLVolumeItf - 9 slots per the spec. */
struct SLVolumeItf_ {
    SLresult (*SetVolumeLevel)(SLVolumeItf, SLmillibel);
    SLresult (*GetVolumeLevel)(SLVolumeItf, SLmillibel *);
    SLresult (*GetMaxVolumeLevel)(SLVolumeItf, SLmillibel *);
    SLresult (*SetMute)(SLVolumeItf, SLboolean);
    SLresult (*GetMute)(SLVolumeItf, SLboolean *);
    SLresult (*EnableStereoPosition)(SLVolumeItf, SLboolean);
    SLresult (*IsEnabledStereoPosition)(SLVolumeItf, SLboolean *);
    SLresult (*SetStereoPosition)(SLVolumeItf, SLint16);
    SLresult (*GetStereoPosition)(SLVolumeItf, SLint16 *);
};

ABI_ATTR static SLresult vol_GetMaxVolumeLevel(SLVolumeItf s, SLmillibel *out)
{ (void)s; if (out) *out = 0; return SL_RESULT_SUCCESS; }

static const struct SLVolumeItf_ tt_volume_vtable = {
    .SetVolumeLevel          = vol_SetVolumeLevel,
    .GetVolumeLevel          = vol_GetVolumeLevel,
    .GetMaxVolumeLevel       = vol_GetMaxVolumeLevel,
    .SetMute                 = (void *)sl_unused,
    .GetMute                 = (void *)sl_unused,
    .EnableStereoPosition    = (void *)sl_unused,
    .IsEnabledStereoPosition = (void *)sl_unused,
    .SetStereoPosition       = (void *)sl_unused,
    .GetStereoPosition       = (void *)sl_unused,
};

/* ---- Object vtable: shared by engine/mix/player ----------------- */

static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *ifOut);
ABI_ATTR static SLresult obj_Realize(SLObjectItf self, SLboolean async)
{ (void)self; (void)async; return SL_RESULT_SUCCESS; }

ABI_ATTR static SLresult obj_Resume(SLObjectItf self, SLboolean async)
{ (void)self; (void)async; return SL_RESULT_SUCCESS; }

ABI_ATTR static SLresult obj_GetState(SLObjectItf self, SLuint32 *out)
{ (void)self; if (out) *out = 2 /* REALIZED */; return SL_RESULT_SUCCESS; }

ABI_ATTR static void obj_Destroy(SLObjectItf self)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_obj));
    pthread_mutex_lock(&s_lock);
    if (p->kind == TT_OBJ_PLAYER) {
        /* unlink from the player list */
        tt_obj **pp = &s_players_head;
        while (*pp && *pp != p) pp = &(*pp)->next;
        if (*pp) *pp = p->next;
        if (p->al_source) {
            alSourceStop(p->al_source);
            alDeleteSources(1, &p->al_source);
            alDeleteBuffers(p->buf_count, p->al_bufs);
        }
    }
    pthread_mutex_unlock(&s_lock);
    free(p);
}

ABI_ATTR static SLresult obj_SetPriority(SLObjectItf s, SLint32 a, SLboolean b)
{ (void)s; (void)a; (void)b; return SL_RESULT_SUCCESS; }
ABI_ATTR static SLresult obj_GetPriority(SLObjectItf s, SLint32 *a, SLboolean *b)
{ (void)s; if (a) *a = 0; if (b) *b = 0; return SL_RESULT_SUCCESS; }
ABI_ATTR static SLresult obj_SetLossOfControlInterfaces(SLObjectItf s, SLint16 n,
                                                        SLInterfaceID *ids, SLboolean en)
{ (void)s; (void)n; (void)ids; (void)en; return SL_RESULT_SUCCESS; }
ABI_ATTR static SLresult obj_RegisterCallback(SLObjectItf s, void *cb, void *ctx)
{ (void)s; (void)cb; (void)ctx; return SL_RESULT_SUCCESS; }
ABI_ATTR static void     obj_AbortAsyncOperation(SLObjectItf s) { (void)s; }

/* SLObjectItf spec order:
 *   Realize, Resume, GetState, GetInterface,
 *   RegisterCallback, AbortAsyncOperation, Destroy,
 *   SetPriority, GetPriority, SetLossOfControlInterfaces. */
struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf, SLboolean);
    SLresult (*Resume)(SLObjectItf, SLboolean);
    SLresult (*GetState)(SLObjectItf, SLuint32 *);
    SLresult (*GetInterface)(SLObjectItf, const SLInterfaceID, void *);
    SLresult (*RegisterCallback)(SLObjectItf, void *, void *);
    void     (*AbortAsyncOperation)(SLObjectItf);
    void     (*Destroy)(SLObjectItf);
    SLresult (*SetPriority)(SLObjectItf, SLint32, SLboolean);
    SLresult (*GetPriority)(SLObjectItf, SLint32 *, SLboolean *);
    SLresult (*SetLossOfControlInterfaces)(SLObjectItf, SLint16, SLInterfaceID *, SLboolean);
};

static const struct SLObjectItf_ tt_obj_vtable = {
    .Realize                    = obj_Realize,
    .Resume                     = obj_Resume,
    .GetState                   = obj_GetState,
    .GetInterface               = obj_GetInterface,
    .RegisterCallback           = obj_RegisterCallback,
    .AbortAsyncOperation        = obj_AbortAsyncOperation,
    .Destroy                    = obj_Destroy,
    .SetPriority                = obj_SetPriority,
    .GetPriority                = obj_GetPriority,
    .SetLossOfControlInterfaces = obj_SetLossOfControlInterfaces,
};

/* ---- EngineItf vtable ------------------------------------------- */

static tt_obj *tt_make_obj(tt_obj_kind kind)
{
    tt_obj *o = calloc(1, sizeof(*o));
    o->kind         = kind;
    o->itf_obj      = &tt_obj_vtable;
    o->itf_engine   = NULL;
    o->itf_play     = &tt_play_vtable;
    o->itf_volume   = &tt_volume_vtable;
    o->itf_queue    = &tt_bq_vtable;
    o->volume_mb    = 0;
    o->play_state   = SL_PLAYSTATE_STOPPED;
    return o;
}

/* The engine handles two formal arg blocks: SLDataSource and SLDataSink.
 * libTTapp uses SLDataLocator_AndroidSimpleBufferQueue + SLDataFormat_PCM
 * for sources. We crack the format struct directly to recover sample
 * rate / channels / bits without pulling in the spec's typedefs. */
typedef struct {
    SLuint32 locatorType;
    SLuint32 numBuffers;       /* AndroidSimpleBufferQueue */
} SLDataLocator_AndroidSimpleBufferQueue;

typedef struct {
    SLuint32 formatType;       /* SL_DATAFORMAT_PCM = 2 */
    SLuint32 numChannels;
    SLuint32 samplesPerSec;    /* in milliHz on Android (e.g. 44100000) */
    SLuint32 bitsPerSample;
    SLuint32 containerSize;
    SLuint32 channelMask;
    SLuint32 endianness;
} SLDataFormat_PCM;

typedef struct {
    void *pLocator;
    void *pFormat;
} SLDataSource, SLDataSink;

ABI_ATTR static SLresult eng_CreateAudioPlayer(SLEngineItf self,
                                               SLObjectItf *playerOut,
                                               SLDataSource *src,
                                               SLDataSink *snk,
                                               SLuint32 numIfaces,
                                               const SLInterfaceID *ids,
                                               const SLboolean *req)
{
    (void)self; (void)snk; (void)numIfaces; (void)ids; (void)req;
    tt_obj *p = tt_make_obj(TT_OBJ_PLAYER);

    /* Pull PCM format out of the source. */
    int rate = 44100, channels = 2, bits = 16;
    if (src && src->pFormat) {
        SLDataFormat_PCM *fmt = (SLDataFormat_PCM *)src->pFormat;
        channels = (int)fmt->numChannels;
        bits     = (int)fmt->bitsPerSample;
        /* Android uses milliHz, real OpenSL ES uses Hz. Detect the magnitude. */
        SLuint32 sps = fmt->samplesPerSec;
        rate = (sps >= 8000000) ? (int)(sps / 1000) : (int)sps;
        if (rate < 4000 || rate > 192000) rate = 44100;
        if (channels != 1 && channels != 2) channels = 2;
    }
    p->channels    = channels;
    p->sample_rate = rate;
    p->bits        = bits;

    pthread_mutex_lock(&s_lock);
    alGenSources(1, &p->al_source);
    p->buf_count = 8;
    alGenBuffers(p->buf_count, p->al_bufs);
    /* All buffers start free. */
    for (int i = 0; i < p->buf_count; i++)
        p->free_bufs[i] = p->al_bufs[i];
    p->free_count = p->buf_count;
    alSourcef(p->al_source, AL_GAIN, 1.0f);
    p->next = s_players_head;
    s_players_head = p;
    pthread_mutex_unlock(&s_lock);

    *playerOut = (SLObjectItf)&p->itf_obj;
    return SL_RESULT_SUCCESS;
}

ABI_ATTR static SLresult eng_CreateOutputMix(SLEngineItf self,
                                             SLObjectItf *mixOut,
                                             SLuint32 numIfaces,
                                             const SLInterfaceID *ids,
                                             const SLboolean *req)
{
    (void)self; (void)numIfaces; (void)ids; (void)req;
    tt_obj *m = tt_make_obj(TT_OBJ_OUTPUTMIX);
    *mixOut = (SLObjectItf)&m->itf_obj;
    return SL_RESULT_SUCCESS;
}

/* SLEngineItf - 15 slots per the spec. */
struct SLEngineItf_ {
    SLresult (*CreateLEDDevice)(void *, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateVibraDevice)(void *, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateAudioPlayer)(SLEngineItf, SLObjectItf *, SLDataSource *, SLDataSink *,
                                  SLuint32, const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateAudioRecorder)(void);
    SLresult (*CreateMidiPlayer)(void);
    SLresult (*CreateListener)(void);
    SLresult (*Create3DGroup)(void);
    SLresult (*CreateOutputMix)(SLEngineItf, SLObjectItf *, SLuint32,
                                const SLInterfaceID *, const SLboolean *);
    SLresult (*CreateMetadataExtractor)(void);
    SLresult (*CreateExtensionObject)(void);
    SLresult (*QueryNumSupportedInterfaces)(SLEngineItf, SLuint32, SLuint32 *);
    SLresult (*QuerySupportedInterfaces)(SLEngineItf, SLuint32, SLuint32, SLInterfaceID *);
    SLresult (*QueryNumSupportedExtensions)(SLEngineItf, SLuint32 *);
    SLresult (*QuerySupportedExtension)(SLEngineItf, SLuint32, void *, SLuint32 *);
    SLresult (*IsExtensionSupported)(SLEngineItf, const void *, SLboolean *);
};

ABI_ATTR static SLresult eng_unused(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }

static const struct SLEngineItf_ tt_engine_vtable = {
    .CreateLEDDevice              = (void *)eng_unused,
    .CreateVibraDevice            = (void *)eng_unused,
    .CreateAudioPlayer            = eng_CreateAudioPlayer,
    .CreateAudioRecorder          = (void *)eng_unused,
    .CreateMidiPlayer             = (void *)eng_unused,
    .CreateListener               = (void *)eng_unused,
    .Create3DGroup                = (void *)eng_unused,
    .CreateOutputMix              = eng_CreateOutputMix,
    .CreateMetadataExtractor      = (void *)eng_unused,
    .CreateExtensionObject        = (void *)eng_unused,
    .QueryNumSupportedInterfaces  = (void *)eng_unused,
    .QuerySupportedInterfaces     = (void *)eng_unused,
    .QueryNumSupportedExtensions  = (void *)eng_unused,
    .QuerySupportedExtension      = (void *)eng_unused,
    .IsExtensionSupported         = (void *)eng_unused,
};

/* ---- GetInterface dispatch -------------------------------------- */

static SLresult obj_GetInterface(SLObjectItf self, const SLInterfaceID iid, void *ifOut)
{
    tt_obj *p = (tt_obj *)((char *)self - offsetof(tt_obj, itf_obj));
    const void *iidp = (const void *)iid;

    if (p->kind == TT_OBJ_ENGINE && iidp == tt_SL_IID_ENGINE) {
        *(const void **)ifOut = &p->itf_engine;
        p->itf_engine = &tt_engine_vtable;
        return SL_RESULT_SUCCESS;
    }
    if (p->kind == TT_OBJ_PLAYER) {
        if (iidp == tt_SL_IID_PLAY)                     { *(const void **)ifOut = &p->itf_play;   return SL_RESULT_SUCCESS; }
        if (iidp == tt_SL_IID_VOLUME)                   { *(const void **)ifOut = &p->itf_volume; return SL_RESULT_SUCCESS; }
        if (iidp == tt_SL_IID_ANDROIDSIMPLEBUFFERQUEUE) { *(const void **)ifOut = &p->itf_queue;  return SL_RESULT_SUCCESS; }
    }
    return SL_RESULT_FEATURE_UNSUPPORTED;
}

/* ---- slCreateEngine entry point --------------------------------- */

ABI_ATTR SLresult tt_slCreateEngine(SLObjectItf *engineOut,
                                    SLuint32 numOptions, void *options,
                                    SLuint32 numInterfaces, void *ids, void *req)
{
    (void)numOptions; (void)options; (void)numInterfaces; (void)ids; (void)req;

    pthread_mutex_lock(&s_lock);
    if (!s_al_dev) {
        s_al_dev = alcOpenDevice(NULL);
        if (s_al_dev) {
            s_al_ctx = alcCreateContext(s_al_dev, NULL);
            alcMakeContextCurrent(s_al_ctx);
        }
        if (!s_pump_running) {
            s_pump_running = 1;
            pthread_create(&s_pump_tid, NULL, tt_audio_pump, NULL);
            pthread_detach(s_pump_tid);
        }
    }
    pthread_mutex_unlock(&s_lock);

    if (!s_al_dev) return SL_RESULT_FEATURE_UNSUPPORTED;

    tt_obj *e = tt_make_obj(TT_OBJ_ENGINE);
    *engineOut = (SLObjectItf)&e->itf_obj;
    return SL_RESULT_SUCCESS;
}
