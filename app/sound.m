// GLMediaPlayer (the game's Java sound player) natively: emitters that play the game's Ogg files, mixed on
// iOS's audio engine. Mirrors the Java behaviour: emitters are created per sound index, can autoplay,
// auto-destroy when done, pause/resume/stop, have a volume; "music" calls act on every emitter of an index;
// group 1 = music (index < BGM count), group 2 = everything else.
#import <AVFoundation/AVFoundation.h>
#include <os/lock.h>
#include "jni.h"
#include "elfload.h"
#include "sound_names.h"   // generated from SoundDefines.a

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define GAME "com/gameloft/android/GAND/GloftSMHP/ML/"
#define RATE 44100
#define MAX_EMITTERS 96

enum { E_FREE, E_PLAYING, E_PAUSED, E_STOPPED };
typedef struct {
    int state, id, sound, autoDestroy, loop, done;   // done: finished but still "playing" (Java keeps it there)
    float volume;
    float pos[3];
    // decoded source: either a cached whole sample or a streaming decoder
    const float *pcm; int frames, channels, srcRate;
    stb_vorbis *stream;
    float buf[4096]; int bufFrames, bufPos;
    double cursor;   // position in source frames (cached samples)
} Emitter;

static Emitter g_em[MAX_EMITTERS];
static os_unfair_lock g_lock = OS_UNFAIR_LOCK_INIT;
static int g_nextId = 1, g_bgmCount = 24;
static float g_groupVol[3] = {1, 1, 1};
static float g_listener[3];
static char g_soundDir[1100];
static AVAudioEngine *g_engine;

// Small sound effects are decoded once and kept (like the revival's SoundPool); music streams.
typedef struct { float *pcm; int frames, channels, rate, failed; } Sample;
static Sample g_cache[SOUND_COUNT];

static void path_for(int sound, char *out, size_t n) { snprintf(out, n, "%s/%s", g_soundDir, g_soundNames[sound]); }

static Sample *sample(int sound) {
    Sample *s = &g_cache[sound];
    if (s->pcm || s->failed) return s;
    char p[1300];
    path_for(sound, p, sizeof p);
    short *data = NULL;
    int ch = 0, rate = 0;
    int frames = stb_vorbis_decode_filename(p, &ch, &rate, &data);
    if (frames <= 0) { s->failed = 1; elf_log("sound: cannot decode %s", p); return s; }
    float *f = malloc(sizeof(float) * frames * ch);
    for (int i = 0; i < frames * ch; i++) f[i] = data[i] / 32768.0f;
    free(data);
    s->channels = ch; s->rate = rate; s->frames = frames;
    __atomic_store_n(&s->pcm, f, __ATOMIC_RELEASE);
    return s;
}

static Emitter *find(int id) {
    for (int i = 0; i < MAX_EMITTERS; i++) if (g_em[i].state != E_FREE && g_em[i].id == id) return &g_em[i];
    return NULL;
}
static void release(Emitter *e) {
    if (e->stream) { stb_vorbis_close(e->stream); e->stream = NULL; }
    e->state = E_FREE;
}

// ---------------------------------------------------------------- mixing (audio thread)
static int fill_stream(Emitter *e) {
    int got = stb_vorbis_get_samples_float_interleaved(e->stream, e->channels, e->buf, 4096 / e->channels * e->channels);
    if (got == 0 && e->loop) { stb_vorbis_seek_start(e->stream); got = stb_vorbis_get_samples_float_interleaved(e->stream, e->channels, e->buf, 4096); }
    e->bufFrames = got;
    e->bufPos = 0;
    return got;
}

static void mix(float *L, float *R, int frames) {
    memset(L, 0, frames * 4);
    memset(R, 0, frames * 4);
    os_unfair_lock_lock(&g_lock);
    for (int k = 0; k < MAX_EMITTERS; k++) {
        Emitter *e = &g_em[k];
        if (e->state != E_PLAYING || e->done) continue;
        float vol = e->volume;
        if (vol < 0.001f) continue;
        int finished = 0;
        if (e->stream) {
            for (int i = 0; i < frames; i++) {
                if (e->bufPos >= e->bufFrames && !fill_stream(e)) { finished = 1; break; }
                float *f = &e->buf[e->bufPos++ * e->channels];
                L[i] += f[0] * vol;
                R[i] += f[e->channels > 1 ? 1 : 0] * vol;
            }
        } else if (e->pcm) {
            double step = (double)e->srcRate / RATE;
            for (int i = 0; i < frames; i++) {
                int idx = (int)e->cursor;
                if (idx >= e->frames) {
                    if (e->loop) { e->cursor = 0; idx = 0; } else { finished = 1; break; }
                }
                const float *f = &e->pcm[idx * e->channels];
                L[i] += f[0] * vol;
                R[i] += f[e->channels > 1 ? 1 : 0] * vol;
                e->cursor += step;
            }
        }
        if (finished) {   // MediaPlayer.onCompletion: auto-destroy emitters go away, others stay "playing"
            if (e->autoDestroy) release(e);
            else e->done = 1;
        }
    }
    os_unfair_lock_unlock(&g_lock);
    for (int i = 0; i < frames; i++) {   // soft clip
        L[i] = L[i] > 1 ? 1 : L[i] < -1 ? -1 : L[i];
        R[i] = R[i] > 1 ? 1 : R[i] < -1 ? -1 : R[i];
    }
}

static void start_engine(void) {
    g_engine = [AVAudioEngine new];
    AVAudioFormat *fmt = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:RATE channels:2];
    AVAudioSourceNode *src = [[AVAudioSourceNode alloc] initWithFormat:fmt renderBlock:^OSStatus(BOOL *silence, const AudioTimeStamp *ts, AVAudioFrameCount n, AudioBufferList *out) {
        mix(out->mBuffers[0].mData, out->mBuffers[1].mData, (int)n);
        return noErr;
    }];
    [g_engine attachNode:src];
    [g_engine connect:src to:g_engine.mainMixerNode format:fmt];
    NSError *err;
    if (![g_engine startAndReturnError:&err]) elf_log("sound: engine failed: %s", err.localizedDescription.UTF8String);
}

// ---------------------------------------------------------------- the Java API
static JValue ri(int v) { JValue r = {0}; r.i = v; return r; }
static JValue rf(float v) { JValue r = {0}; r.f = v; return r; }
static JValue rv(void) { JValue r = {0}; return r; }

static int create(int sound, int autoplay, int autoDestroy, float vol, int loop) {
    if (sound < 0 || sound >= SOUND_COUNT) return -1;
    int music = sound < g_bgmCount;
    Sample *s = music ? NULL : sample(sound);   // decode outside the lock
    stb_vorbis *st = NULL;
    if (music) {
        char p[1300];
        path_for(sound, p, sizeof p);
        st = stb_vorbis_open_filename(p, NULL, NULL);
        if (!st) { elf_log("sound: cannot open %s", p); return -1; }
    }
    os_unfair_lock_lock(&g_lock);
    Emitter *e = NULL;
    for (int i = 0; i < MAX_EMITTERS && !e; i++) if (g_em[i].state == E_FREE) e = &g_em[i];
    if (!e) {   // all busy: reuse the first finished one
        for (int i = 0; i < MAX_EMITTERS && !e; i++) if (g_em[i].state == E_STOPPED) { release(&g_em[i]); e = &g_em[i]; }
    }
    int id = -1;
    if (e) {
        memset(e, 0, sizeof *e);
        id = e->id = g_nextId++;
        e->sound = sound; e->autoDestroy = autoDestroy; e->loop = loop; e->volume = vol;
        if (music) {
            e->stream = st;
            stb_vorbis_info info = stb_vorbis_get_info(st);
            e->channels = info.channels; e->srcRate = (int)info.sample_rate;
        } else if (s && s->pcm) {
            e->pcm = s->pcm; e->frames = s->frames; e->channels = s->channels; e->srcRate = s->rate;
        }
        e->state = autoplay ? E_PLAYING : E_STOPPED;
        st = NULL;
    }
    os_unfair_lock_unlock(&g_lock);
    if (st) stb_vorbis_close(st);
    return id;
}

static JValue createEmitter3(JObject *o, const JValue *a) {
    (void)o;
    int sound = a[0].i;
    return ri(create(sound, a[1].i, a[2].i, g_groupVol[sound < g_bgmCount ? 1 : 2], 0));
}
static JValue createEmitter5(JObject *o, const JValue *a) { (void)o; return ri(create(a[0].i, a[1].i, a[2].i, a[3].f, a[4].i)); }
static JValue playMusic(JObject *o, const JValue *a) { (void)o; return ri(create(a[0].i, 1, 1, a[1].f, a[2].i)); }
static JValue playSound(JObject *o, const JValue *a) { (void)o; create(a[0].i, 1, 1, a[2].f, 0); return rv(); }

#define WITH(id, body) do { os_unfair_lock_lock(&g_lock); Emitter *e = find(id); if (e) { body; } os_unfair_lock_unlock(&g_lock); } while (0)
#define EACH(cond, body) do { os_unfair_lock_lock(&g_lock); for (int k = 0; k < MAX_EMITTERS; k++) { Emitter *e = &g_em[k]; if (e->state != E_FREE && (cond)) { body; } } os_unfair_lock_unlock(&g_lock); } while (0)

static void stop_one(Emitter *e) {   // stopEmitter: stop, auto-destroy emitters are released
    if (e->autoDestroy) release(e);
    else { e->state = E_STOPPED; e->done = 0; e->cursor = 0; if (e->stream) { stb_vorbis_seek_start(e->stream); e->bufFrames = e->bufPos = 0; } }
}

static JValue destroyEmitter(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, release(e)); return rv(); }
static JValue stopEmitter(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, stop_one(e)); return rv(); }
static JValue pauseEmitter(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, if (e->state == E_PLAYING) e->state = E_PAUSED); return rv(); }
static JValue resumeEmitter(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, if (e->state == E_PAUSED) e->state = E_PLAYING); return rv(); }
static JValue playEmitter(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, if (e->state == E_STOPPED) { e->state = E_PLAYING; e->loop = a[1].i; }); return rv(); }
static JValue setEmitterVolume(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, e->volume = a[1].f); return rv(); }
static JValue getEmitterVolume(JObject *o, const JValue *a) { (void)o; float v = 0; WITH(a[0].i, v = e->volume); return rf(v); }
static JValue isEmitterPlaying(JObject *o, const JValue *a) { (void)o; int v = 0; WITH(a[0].i, v = e->state == E_PLAYING); return ri(v); }
static JValue isEmitterPaused(JObject *o, const JValue *a) { (void)o; int v = 0; WITH(a[0].i, v = e->state == E_PAUSED); return ri(v); }
static JValue isEmitterExist(JObject *o, const JValue *a) { (void)o; int v = 0; WITH(a[0].i, v = 1); return ri(v); }
static JValue isEmitterStopped(JObject *o, const JValue *a) { (void)o; int v = 1; WITH(a[0].i, v = e->state == E_STOPPED); return ri(v); }
static JValue setEmitterPos(JObject *o, const JValue *a) { (void)o; WITH(a[0].i, e->pos[0] = a[1].f; e->pos[1] = a[2].f; e->pos[2] = a[3].f); return rv(); }
static JValue getEmitterPosX(JObject *o, const JValue *a) { (void)o; float v = 0; WITH(a[0].i, v = e->pos[0]); return rf(v); }
static JValue getEmitterPosY(JObject *o, const JValue *a) { (void)o; float v = 0; WITH(a[0].i, v = e->pos[1]); return rf(v); }
static JValue getEmitterPosZ(JObject *o, const JValue *a) { (void)o; float v = 0; WITH(a[0].i, v = e->pos[2]); return rf(v); }
static JValue setListenerPos(JObject *o, const JValue *a) { (void)o; g_listener[0] = a[0].f; g_listener[1] = a[1].f; g_listener[2] = a[2].f; return rv(); }
static JValue getListenerPosX(JObject *o, const JValue *a) { (void)o; (void)a; return rf(g_listener[0]); }
static JValue getListenerPosY(JObject *o, const JValue *a) { (void)o; (void)a; return rf(g_listener[1]); }
static JValue getListenerPosZ(JObject *o, const JValue *a) { (void)o; (void)a; return rf(g_listener[2]); }

static JValue stopMusic(JObject *o, const JValue *a) { (void)o; EACH(e->sound == a[0].i, stop_one(e)); return rv(); }
static JValue destroyMusic(JObject *o, const JValue *a) { (void)o; EACH(e->sound == a[0].i, release(e)); return rv(); }
static JValue pauseMusic(JObject *o, const JValue *a) { (void)o; EACH(e->sound == a[0].i && e->state == E_PLAYING, e->state = E_PAUSED); return rv(); }
static JValue resumeMusic(JObject *o, const JValue *a) { (void)o; EACH(e->sound == a[0].i && e->state == E_PAUSED, e->state = E_PLAYING); return rv(); }
static JValue setMusicVolume(JObject *o, const JValue *a) { (void)o; EACH(e->sound == a[0].i, e->volume = a[1].f); return rv(); }
static JValue isMusicPlaying(JObject *o, const JValue *a) { (void)o; int n = 0; EACH(e->sound == a[0].i, n++); return ri(n); }

#define GROUP(e, g) ((g) == 1 ? (e)->sound < g_bgmCount : (e)->sound >= g_bgmCount)
static JValue setGroupVolume(JObject *o, const JValue *a) {
    (void)o;
    int g = a[0].i;
    if (g == 1 || g == 2) { g_groupVol[g] = a[1].f; EACH(GROUP(e, g), e->volume = a[1].f); }
    return rv();
}
static JValue getGroupVolume(JObject *o, const JValue *a) { (void)o; int g = a[0].i; return rf(g == 1 || g == 2 ? g_groupVol[g] : 0); }
static JValue stopGroup(JObject *o, const JValue *a) { (void)o; int g = a[0].i; EACH(GROUP(e, g), stop_one(e)); return rv(); }
static JValue pauseGroup(JObject *o, const JValue *a) { (void)o; int g = a[0].i; EACH(GROUP(e, g) && e->state == E_PLAYING, e->state = E_PAUSED); return rv(); }
static JValue resumeGroup(JObject *o, const JValue *a) { (void)o; int g = a[0].i; EACH(GROUP(e, g) && e->state == E_PAUSED, e->state = E_PLAYING); return rv(); }
static JValue pauseAll(JObject *o, const JValue *a) { (void)o; (void)a; EACH(e->state == E_PLAYING, e->state = E_PAUSED); return rv(); }
static JValue resumeAll(JObject *o, const JValue *a) { (void)o; (void)a; EACH(e->state == E_PAUSED, e->state = E_PLAYING); return rv(); }
static JValue stopAll(JObject *o, const JValue *a) { (void)o; (void)a; EACH(1, stop_one(e)); return rv(); }
static JValue destroyAll(JObject *o, const JValue *a) { (void)o; (void)a; EACH(1, release(e)); return rv(); }
static JValue loadSound(JObject *o, const JValue *a) { (void)o; if (a[0].i >= g_bgmCount && a[0].i < SOUND_COUNT) sample(a[0].i); return rv(); }
static JValue nothing(JObject *o, const JValue *a) { (void)o; (void)a; return rv(); }
static JValue findFarthestEmitter(JObject *o, const JValue *a) {
    (void)o;
    int best = -1;
    float bd = -1;
    os_unfair_lock_lock(&g_lock);
    for (int k = 0; k < MAX_EMITTERS; k++) {
        Emitter *e = &g_em[k];
        if (e->state == E_FREE || e->sound != a[0].i) continue;
        float dx = e->pos[0] - g_listener[0], dy = e->pos[1] - g_listener[1], dz = e->pos[2] - g_listener[2];
        float d = dx * dx + dy * dy + dz * dz;
        if (d > bd) { bd = d; best = e->id; }
    }
    os_unfair_lock_unlock(&g_lock);
    return ri(best);
}

void pause_audio(int paused) {
    if (!g_engine) return;
    if (paused) [g_engine pause];
    else { NSError *err; [g_engine startAndReturnError:&err]; }
}

void sound_setup(const char *dataDir, int bgmCount) {
    snprintf(g_soundDir, sizeof g_soundDir, "%s/sound", dataDir);
    if (bgmCount > 0) g_bgmCount = bgmCount;
    start_engine();
#define DEF(n, sig, f) jni_define(GAME "GLMediaPlayer", n, sig, true, f)
    DEF("createEmitter", "(III)I", createEmitter3);
    DEF("createEmitter", "(IIIFI)I", createEmitter5);
    DEF("playMusic", "(IFI)I", playMusic);
    DEF("playSound", "(IIF)V", playSound);
    DEF("playEmitter", "(II)V", playEmitter);
    DEF("destroyEmitter", "(I)V", destroyEmitter);
    DEF("stopEmitter", "(I)V", stopEmitter);
    DEF("pauseEmitter", "(I)V", pauseEmitter);
    DEF("resumeEmitter", "(I)V", resumeEmitter);
    DEF("setEmitterVolume", "(IF)V", setEmitterVolume);
    DEF("getEmitterVolume", "(I)F", getEmitterVolume);
    DEF("isEmitterPlaying", "(I)I", isEmitterPlaying);
    DEF("isEmitterPaused", "(I)I", isEmitterPaused);
    DEF("isEmitterExist", "(I)I", isEmitterExist);
    DEF("isEmitterStopped", "(I)I", isEmitterStopped);
    DEF("setEmitterPos", "(IFFF)V", setEmitterPos);
    DEF("getEmitterPosX", "(I)F", getEmitterPosX);
    DEF("getEmitterPosY", "(I)F", getEmitterPosY);
    DEF("getEmitterPosZ", "(I)F", getEmitterPosZ);
    DEF("setListenerPos", "(FFF)V", setListenerPos);
    DEF("getListenerPosX", "()F", getListenerPosX);
    DEF("getListenerPosY", "()F", getListenerPosY);
    DEF("getListenerPosZ", "()F", getListenerPosZ);
    DEF("stopMusic", "(I)V", stopMusic);
    DEF("destroyMusic", "(I)V", destroyMusic);
    DEF("pauseMusic", "(I)V", pauseMusic);
    DEF("resumeMusic", "(I)V", resumeMusic);
    DEF("setMusicVolume", "(IF)V", setMusicVolume);
    DEF("isMusicPlaying", "(I)I", isMusicPlaying);
    DEF("setGroupVolume", "(IF)V", setGroupVolume);
    DEF("getGroupVolume", "(I)F", getGroupVolume);
    DEF("stopGroup", "(I)V", stopGroup);
    DEF("pauseGroup", "(I)V", pauseGroup);
    DEF("resumeGroup", "(I)V", resumeGroup);
    DEF("pauseAll", "()V", pauseAll);
    DEF("resumeAll", "()V", resumeAll);
    DEF("stopAll", "()V", stopAll);
    DEF("destroyAll", "()V", destroyAll);
    DEF("destroyPools", "()V", nothing);
    DEF("destroySoundPool", "()V", nothing);
    DEF("initPools", "()V", nothing);
    DEF("loadSound", "(I)V", loadSound);
    DEF("findFarthestEmitter", "(I)I", findFarthestEmitter);
    DEF("Paused", "()V", nothing);
    DEF("PausedVideo", "()V", nothing);
}
