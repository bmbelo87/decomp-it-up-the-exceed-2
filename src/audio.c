#include "pumpy.h"
#include "resource_ids.h"
#include <SDL.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

/* ------------------------------------------------------------------------- *
 * Linux audio backend (SDL).
 *
 * Every source is decoded once to signed 16-bit PCM at the device rate in
 * stereo. A small software mixer (SDL callback) sums the playing channels.
 * The DirectSound / DirectShow / MCI backends do not exist here.
 * ------------------------------------------------------------------------- */

#define DEV_RATE 44100
#define DEV_CHANNELS 2

typedef struct {
    int16_t* pcm;          /* interleaved stereo at DEV_RATE        */
    uint64_t frames;       /* total frames (stereo pair samples)    */
    uint64_t pos;          /* playhead                              */
    bool     playing;
    bool     loop;
    float    vol;          /* 0..1                                 */
    bool     inUse;
    char     name[64];
} MixChannel;

static MixChannel g_channels[MAX_SOUNDS];
static MixChannel g_bgmChan = {0};
static SDL_AudioDeviceID g_dev = 0;

/* Interpolação do relógio da BGM: g_bgmChan.pos só anda em blocos de
 * want.samples (1024 frames ≈ 23 ms) dentro do mix_cb. Sem interpolar, o
 * songTime (e a rolagem das setas) avança em degraus e trava visualmente. */
static volatile uint64_t g_bgmCbPos = 0;    /* pos ao fim do último callback */
static volatile uint64_t g_bgmCbTicks = 0;  /* SDL_GetPerformanceCounter nesse instante */
static uint64_t g_bgmCbFrames = 1024;       /* tamanho do bloco do callback */
static double   g_bgmLastMs = 0.0;          /* último valor devolvido (monotônico) */

static void mix_cb(void* userdata, Uint8* stream, int len) {
    (void)userdata;
    int16_t* out = (int16_t*)stream;
    int frames = len / (2 * (int)sizeof(int16_t));

    for (int f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        for (int i = 0; i < MAX_SOUNDS; i++) {
            MixChannel* c = &g_channels[i];
            if (!c->inUse || !c->playing) continue;
            if (c->pos >= c->frames) {
                if (c->loop) c->pos = 0;
                else { c->playing = false; continue; }
            }
            int idx = (int)(c->pos * 2);
            l += (int32_t)(c->pcm[idx]     * c->vol);
            r += (int32_t)(c->pcm[idx + 1] * c->vol);
            c->pos++;
        }
        {
            MixChannel* c = &g_bgmChan;
            if (c->inUse && c->playing) {
                if (c->pos >= c->frames) {
                    if (c->loop) c->pos = 0;
                    else { c->playing = false; }
                }
                if (c->playing) {
                    int idx = (int)(c->pos * 2);
                    l += (int32_t)(c->pcm[idx]     * c->vol);
                    r += (int32_t)(c->pcm[idx + 1] * c->vol);
                    c->pos++;
                }
            }
        }
        if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
        if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
        out[f * 2 + 0] = (int16_t)l;
        out[f * 2 + 1] = (int16_t)r;
    }
    g_bgmCbFrames = (uint64_t)frames;
    g_bgmCbPos = g_bgmChan.pos;
    g_bgmCbTicks = SDL_GetPerformanceCounter();
}

bool Audio_Init(void) {
    SDL_AudioSpec want;
    memset(&want, 0, sizeof(want));
    want.freq = DEV_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = DEV_CHANNELS;
    want.samples = 1024;
    want.callback = mix_cb;

    SDL_AudioSpec got;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (g_dev == 0) {
        Log_Print("Audio: SDL_OpenAudioDevice failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    Log_Print("Audio: initialized (%d Hz, %d ch)\n", got.freq, got.channels);
    return true;
}

/* ---------------------------------------------------------------- helpers */

static int16_t clamp16(int64_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

/* Convert a source PCM buffer (any rate, 1/2 channels, s16) to device format.
 * Returns a malloc'd interleaved stereo buffer; sets *outFrames. */
static int16_t* mix_to_device(const int16_t* in, uint64_t inFrames,
                              uint32_t inRate, int inCh, uint64_t* outFrames) {
    if (!in || inFrames == 0) { *outFrames = 0; return NULL; }
    if (inFrames == 0 || inRate == 0) { *outFrames = 0; return NULL; }

    if (inRate == DEV_RATE && inCh == DEV_CHANNELS) {
        size_t bytes = (size_t)(inFrames * DEV_CHANNELS * sizeof(int16_t));
        int16_t* copy = (int16_t*)malloc(bytes);
        if (!copy) { *outFrames = 0; return NULL; }
        memcpy(copy, in, bytes);
        *outFrames = inFrames;
        return copy;
    }

    uint64_t of = (uint64_t)((unsigned long long)inFrames * DEV_RATE / inRate);
    if (of == 0) of = 1;
    int16_t* out = (int16_t*)malloc((size_t)of * DEV_CHANNELS * sizeof(int16_t));
    if (!out) { *outFrames = 0; return NULL; }

    for (uint64_t t = 0; t < of; t++) {
        double srcPos = (double)t * (double)inRate / (double)DEV_RATE;
        uint64_t i0 = (uint64_t)srcPos;
        double frac = srcPos - (double)i0;
        if (i0 >= inFrames - 1) { i0 = inFrames - 1; frac = 0; }
        uint64_t i1 = i0 + 1;
        for (int ch = 0; ch < DEV_CHANNELS; ch++) {
            int srcCh = (inCh == 1) ? 0 : (ch < inCh ? ch : inCh - 1);
            int at0 = (int)(i0 * inCh + srcCh);
            int at1 = (int)(i1 * inCh + srcCh);
            double s0 = in[at0];
            double s1 = in[at1];
            out[t * 2 + ch] = clamp16((int64_t)(s0 + (s1 - s0) * frac));
        }
    }
    *outFrames = of;
    return out;
}

/* Parse a RIFF/WAVE file body in memory -> raw s16 PCM. Handles PCM 8/16/24-bit
 * and IEEE float 32-bit. Returns malloc'd interleaved (rate channels); sets
 * *outFrames (frames), *outRate, *outCh. Caller frees *outData. */
static bool wav_bytes_to_pcm(const uint8_t* p, size_t size,
                             int16_t** outData, uint64_t* outFrames,
                             uint32_t* outRate, int* outCh) {
    *outData = NULL; *outFrames = 0;
    if (size < 12) return false;
    if (memcmp(p, "RIFF", 4) != 0 || memcmp(p + 8, "WAVE", 4) != 0) return false;

    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    uint16_t tag = 0;
    const uint8_t* data = NULL;
    size_t dataLen = 0;

    size_t off = 12;
    while (off + 8 <= size) {
        char id[5];
        memcpy(id, p + off, 4); id[4] = '\0';
        uint32_t chunkSize = (uint32_t)(p[off+4] | (p[off+5]<<8) | (p[off+6]<<16) | ((uint32_t)p[off+7]<<24));
        if (chunkSize > size - off - 8) chunkSize = (uint32_t)(size - off - 8);

        if (memcmp(id, "fmt ", 4) == 0 && chunkSize >= 16) {
            const uint8_t* f = p + off + 8;
            tag      = (uint16_t)(f[0] | (f[1] << 8));
            channels = (uint16_t)(f[2] | (f[3] << 8));
            rate     = (uint32_t)(f[4] | (f[5]<<8) | (f[6]<<16) | ((uint32_t)f[7]<<24));
            bits     = (uint16_t)(f[14] | (f[15] << 8));
        } else if (memcmp(id, "data", 4) == 0) {
            data = p + off + 8;
            dataLen = chunkSize;
        }
        off += 8 + chunkSize;
        if (chunkSize & 1) off++;
    }

    if (!data || dataLen == 0 || channels == 0 || channels > 2 || rate == 0 || bits == 0)
        return false;

    uint64_t n = dataLen / (bits / 8);
    uint64_t frames = n / channels;
    if (frames == 0) return false;

    int16_t* raw = (int16_t*)malloc((size_t)frames * channels * sizeof(int16_t));
    if (!raw) return false;

    for (uint64_t f = 0; f < frames; f++) {
        for (int ch = 0; ch < channels; ch++) {
            const uint8_t* src = data + (f * channels + ch) * (bits / 8);
            int64_t sample = 0;
            switch (bits) {
            case 8:  sample = ((int64_t)src[0] - 128) * 256; break;
            case 16: sample = (int16_t)(src[0] | (src[1] << 8)); break;
            case 24: sample = (int32_t)(src[0] | (src[1] << 8) | (src[2] << 16));
                     sample = (sample << 8) >> 8; break;
            case 32:
                if (tag == 3) {
                    float fl;
                    memcpy(&fl, src, 4);
                    if (fl > 1.0f) fl = 1.0f; if (fl < -1.0f) fl = -1.0f;
                    sample = (int64_t)(fl * 32767.0);
                } else {
                    sample = (int32_t)(src[0] | (src[1]<<8) | (src[2]<<16) | ((uint32_t)src[3]<<24));
                }
                break;
            default: sample = 0; break;
            }
            raw[f * channels + ch] = clamp16(sample);
        }
    }

    *outData = raw;
    *outFrames = frames;
    *outRate = rate;
    *outCh = channels;
    return true;
}

static bool load_file_to_pcm(const char* path, int16_t** pcm, uint64_t* frames,
                             uint32_t* rate, int* ch) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }

    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return false; }

    bool ok = wav_bytes_to_pcm(buf, (size_t)sz, pcm, frames, rate, ch);
    free(buf);
    return ok;
}

/* ------------------------------------------------------- SFX channels */

static int Audio_FindFree(void) {
    for (int i = 0; i < MAX_SOUNDS; i++)
        if (!g_game.sounds[i].inUse) return i;
    return -1;
}

/* Decode a raw WAV (with headers) held in memory into an SFX channel. */
int Audio_LoadWAV(const char* name, const uint8_t* data, DWORD size) {
    int idx = Audio_FindFree();
    if (idx < 0) return -1;

    int16_t* raw = NULL;
    uint64_t sframes = 0;
    uint32_t srate = 0;
    int sch = 0;
    if (!wav_bytes_to_pcm(data, size, &raw, &sframes, &srate, &sch)) {
        Log_Print("Audio: failed to parse WAV '%s'\n", name);
        return -1;
    }

    uint64_t dframes = 0;
    int16_t* dev = mix_to_device(raw, sframes, srate, sch, &dframes);
    free(raw);
    if (!dev) return -1;

    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_channels[idx].pcm = dev;
    g_channels[idx].frames = dframes;
    g_channels[idx].pos = 0;
    g_channels[idx].playing = false;
    g_channels[idx].loop = false;
    g_channels[idx].vol = 1.0f;
    g_channels[idx].inUse = true;
    g_channels[idx].name[0] = '\0';
    strncat(g_channels[idx].name, name, sizeof(g_channels[idx].name) - 1);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);

    g_game.sounds[idx].buffer = (void*)1; /* sentinel for the debug HUD */
    g_game.sounds[idx].inUse = true;
    strncat(g_game.sounds[idx].name, name, sizeof(g_game.sounds[idx].name) - 1);
    g_game.soundCount++;

    Log_Print("Audio: loaded '%s' (%llu frames, %u Hz, %dch)\n",
              name, (unsigned long long)dframes, srate, sch);
    return idx;
}

int Audio_LoadFromResource(const char* name, int resId) {
    (void)resId;
    Log_Print("Audio: resources not available on Linux, skipping '%s'\n", name);
    return -1;
}

/* WAVs embutidos no executável (build/generated/wave_embedded.c, gerado por
 * tools/embed_waves.py). O original os guarda como recursos do PUMPY.EXE.
 * DESATIVADO no Exceed: o exceed.exe le WAVE/ do disco (strings "WAVE/xxx.wav"). */
/*
typedef struct { const char* name; const uint8_t* data; uint32_t size; } EmbeddedWave;
extern const EmbeddedWave g_embeddedWaves[];

static int wave_name_eq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a++, cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return 0;
    }
    return *a == *b;
}
*/

int Audio_LoadWaveFile(const char* filename) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/WAVE/%s", g_game.currentDirectory, filename);
    FILE* f = fopen(path, "rb");
    if (!f) {
        /* Sem WAVE/ no disco: usa a cópia embutida. (desativado no Exceed)
        for (int i = 0; g_embeddedWaves[i].name; i++) {
            if (wave_name_eq(g_embeddedWaves[i].name, filename))
                return Audio_LoadWAV(filename, g_embeddedWaves[i].data,
                                     (DWORD)g_embeddedWaves[i].size);
        }
        */
        Log_Print("Audio: failed to open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc((size_t)sz);
    if (!data) { fclose(f); return -1; }
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(data); return -1; }
    int idx = Audio_LoadWAV(filename, data, (DWORD)sz);
    free(data);
    return idx;
}

static const char* g_waveFiles[SND_COUNT] = {
    "3-2.wav",    // SND_3_2
    "2-1.wav",    // SND_2_1
    "4-2.wav",    // SND_4_2
    "8-1.wav",    // SND_8_1
    "5-1.wav",    // SND_5_1
    "RANK_A.wav", // SND_RANK_A
    "RANK_B.wav", // SND_RANK_B
    "RANK_C.wav", // SND_RANK_C
    "RANK_D.wav", // SND_RANK_D
    "RANK_F.wav", // SND_RANK_F
    "10-2.wav",   // SND_10_2
    "7-1.wav",    // SND_7_1
    "01-1.wav",   // SND_COIN_PARTIAL
    "COIN2.wav",  // SND_COIN_CREDIT
    "10-1.wav",   // SND_10_1
    "CHGMOD.wav",     // SND_CHGMOD
    "START.wav",      // SND_START
    "TIME_LIMIT.wav", // SND_TIME_LIMIT
    "PUSHPANEL.wav",  // SND_PUSHPANEL
    "GAMESTOP.wav",   // SND_GAMESTOP
    "9-5.wav",        // SND_9_5
    "NEXTSTAGE.wav"   // SND_NEXTSTAGE
};

int g_waveSoundIds[SND_COUNT];

void Audio_LoadAllWaves(void) {
    for (int i = 0; i < SND_COUNT; i++)
        g_waveSoundIds[i] = Audio_LoadWaveFile(g_waveFiles[i]);
}

static float ds_volume_to_linear(long dsVol) {
    if (dsVol >= 0) return 1.0f;
    return powf(10.0f, (float)dsVol / 20000.0f);
}

void Audio_Play(int id, bool loop) {
    if (id < 0 || id >= MAX_SOUNDS || !g_channels[id].inUse) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_channels[id].pos = 0;
    g_channels[id].playing = true;
    g_channels[id].loop = loop;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

void Audio_Stop(int id) {
    if (id < 0 || id >= MAX_SOUNDS || !g_channels[id].inUse) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_channels[id].playing = false;
    g_channels[id].pos = 0;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

bool Audio_IsPlaying(int id) {
    if (id < 0 || id >= MAX_SOUNDS || !g_channels[id].inUse) return false;
    return g_channels[id].playing;
}

void Audio_StopAll(void) {
    for (int i = 0; i < MAX_SOUNDS; i++) Audio_Stop(i);
}

void Audio_SetVolume(int id, long volume) {
    if (id < 0 || id >= MAX_SOUNDS || !g_channels[id].inUse) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_channels[id].vol = ds_volume_to_linear(volume);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

void Audio_Shutdown(void) {
    for (int i = 0; i < MAX_SOUNDS; i++) {
        if (g_channels[i].inUse) {
            free(g_channels[i].pcm);
            g_channels[i].pcm = NULL;
            g_channels[i].inUse = false;
        }
    }
    if (g_bgmChan.inUse) {
        free(g_bgmChan.pcm);
        g_bgmChan.pcm = NULL;
        g_bgmChan.inUse = false;
        g_bgmChan.playing = false;
    }
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    memset(g_game.sounds, 0, sizeof(g_game.sounds));
    g_game.soundCount = 0;
    Log_Print("Audio: shutdown\n");
}

/* ------------------------------------------------------------- BGM path */

static void bgm_set_pcm(int16_t* dev, uint64_t devFrames) {
    if (g_dev) SDL_LockAudioDevice(g_dev);
    if (g_bgmChan.inUse) free(g_bgmChan.pcm);
    g_bgmChan.pcm = dev;
    g_bgmChan.frames = devFrames;
    g_bgmChan.pos = 0;
    g_bgmChan.playing = false;
    g_bgmChan.loop = false;
    g_bgmChan.vol = 1.0f;
    g_bgmChan.inUse = (dev != NULL);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);

    g_game.bgm.buffer = dev ? (void*)1 : NULL;
}

bool BGM_LoadWAV(const char* path) {
    int16_t* raw = NULL;
    uint64_t sframes = 0;
    uint32_t srate = 0;
    int sch = 0;
    if (!load_file_to_pcm(path, &raw, &sframes, &srate, &sch)) {
        Log_Print("BGM: failed to read WAV '%s'\n", path);
        return false;
    }
    uint64_t dframes = 0;
    int16_t* dev = mix_to_device(raw, sframes, srate, sch, &dframes);
    free(raw);
    if (!dev) return false;

    bgm_set_pcm(dev, dframes);
    g_game.bgm.playing = false;
    g_game.bgm.looping = false;
    g_game.bgm.useMCI = false;
    g_game.bgm.dataSize = (DWORD)(dframes * DEV_CHANNELS * sizeof(int16_t));
    strncpy(g_game.bgm.name, path, sizeof(g_game.bgm.name) - 1);
    Log_Print("BGM: loaded WAV '%s' (%llu frames)\n", path, (unsigned long long)dframes);
    return true;
}

bool BGM_LoadMP3(const char* path) {
    drmp3 mp3;
    if (!drmp3_init_file(&mp3, path, NULL)) {
        Log_Print("BGM: failed to decode MP3 '%s'\n", path);
        return false;
    }

    drmp3_uint64 totalFrames = drmp3_get_pcm_frame_count(&mp3);
    if (totalFrames == 0) { drmp3_uninit(&mp3); return false; }

    drmp3_uint64 totalBytes = totalFrames * mp3.channels * sizeof(drmp3_int16);
    drmp3_int16* pcmData = (drmp3_int16*)malloc((size_t)totalBytes);
    if (!pcmData) { drmp3_uninit(&mp3); return false; }

    drmp3_uint64 framesRead = drmp3_read_pcm_frames_s16(&mp3, totalFrames, pcmData);
    drmp3_uninit(&mp3);
    if (framesRead == 0) { free(pcmData); return false; }

    uint64_t dframes = 0;
    int16_t* dev = mix_to_device(pcmData, framesRead, mp3.sampleRate,
                                 mp3.channels, &dframes);
    free(pcmData);
    if (!dev) return false;

    bgm_set_pcm(dev, dframes);
    g_game.bgm.playing = false;
    g_game.bgm.looping = false;
    g_game.bgm.useMCI = false;
    g_game.bgm.dataSize = (DWORD)(dframes * DEV_CHANNELS * sizeof(int16_t));
    strncpy(g_game.bgm.name, path, sizeof(g_game.bgm.name) - 1);
    Log_Print("BGM: loaded MP3 '%s' (%llu frames)\n", path, (unsigned long long)dframes);
    return true;
}

bool BGM_Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[12];
    size_t got = fread(hdr, 1, 12, f);
    fclose(f);

    if (got >= 12 && memcmp(hdr, "RIFF", 4) == 0 && memcmp(hdr + 8, "WAVE", 4) == 0)
        return BGM_LoadWAV(path);
    return BGM_LoadMP3(path);
}

bool BGM_LoadAUD(int songId, bool preview) {
    char path[MAX_PATH];
    const char* exts[] = { ".mp3", ".MP3", ".wav", ".WAV" };

    if (preview) {
        for (int ei = 0; ei < 4; ei++) {
            snprintf(path, sizeof(path), "%s/AUDIO/D%d%s", g_game.currentDirectory, songId, exts[ei]);
            if (BGM_Load(path)) return true;
        }
        for (int ei = 0; ei < 4; ei++) {
            snprintf(path, sizeof(path), "%s/AUDIO/%d%s", g_game.currentDirectory, songId, exts[ei]);
            if (BGM_Load(path)) return true;
        }
    } else {
        for (int ei = 0; ei < 4; ei++) {
            snprintf(path, sizeof(path), "%s/AUDIO/%d%s", g_game.currentDirectory, songId, exts[ei]);
            if (BGM_Load(path)) return true;
        }
        for (int ei = 0; ei < 4; ei++) {
            snprintf(path, sizeof(path), "%s/AUDIO/D%d%s", g_game.currentDirectory, songId, exts[ei]);
            if (BGM_Load(path)) return true;
        }
    }

    snprintf(path, sizeof(path), "%s/AUDIO/%d.AUD", g_game.currentDirectory, songId);
    return BGM_LoadAUDDirect(path);
}

/* XOR decrypt for .AUD (from the original binary; table is portable C). */
static const uint8_t g_audXorTable[1024] = {
    56,30,183,73,105,12,1,136,165,200,96,0,26,59,3,145,76,41,199,203,187,119,216,20,
    168,17,155,233,237,151,255,171,185,197,164,12,87,50,160,206,144,53,108,55,10,224,
    102,166,141,206,26,243,236,157,46,234,220,93,158,74,133,163,23,8,195,86,36,24,232,
    190,39,148,77,236,113,110,111,200,82,18,118,23,162,242,140,74,57,205,80,126,69,58,
    212,113,42,151,137,155,37,253,48,136,171,232,71,174,99,13,140,245,121,109,251,120,
    86,82,18,193,176,239,114,7,207,192,207,79,191,170,152,139,213,158,64,232,175,54,
    18,16,76,186,67,114,123,218,243,45,45,45,60,125,164,113,205,89,219,135,8,25,149,
    152,92,94,94,175,144,199,6,204,46,124,109,240,71,104,54,178,45,37,42,83,254,83,92,
    57,208,93,189,169,250,48,134,157,242,225,181,223,146,96,236,36,28,98,171,39,14,2,
    13,77,215,139,128,130,4,209,153,28,203,217,189,199,40,33,43,197,80,222,189,90,197,
    226,52,58,190,255,187,242,57,34,124,108,238,131,26,191,233,52,129,84,217,210,65,
    157,49,104,249,30,165,103,165,130,245,86,156,218,228,111,173,169,240,201,113,92,
    105,206,115,35,68,157,100,167,156,67,32,64,115,73,10,52,10,36,72,235,124,93,104,
    244,254,41,188,150,240,229,196,193,68,135,255,13,117,163,167,200,213,107,213,216,
    22,107,166,255,106,106,181,247,56,78,186,164,61,65,117,90,84,198,125,103,9,125,
    247,215,23,196,148,132,227,11,15,1,32,171,219,77,79,38,147,172,21,80,245,193,149,
    147,1,231,101,131,182,160,177,4,100,168,244,134,168,198,33,225,119,74,161,240,221,
    110,99,73,7,105,131,198,176,224,13,228,144,154,166,143,212,184,4,91,116,72,249,
    182,63,94,214,8,252,31,59,46,47,5,230,207,236,174,194,174,102,243,35,6,178,219,88,
    175,56,18,152,149,16,108,214,231,102,127,233,195,81,159,172,123,174,21,101,118,17,
    51,246,252,147,31,30,92,210,225,231,27,5,155,60,222,4,172,140,3,243,253,44,250,81,
    128,87,83,67,184,163,74,126,57,97,175,103,188,146,116,159,166,89,223,5,222,123,36,
    164,25,137,170,220,153,20,35,3,76,72,69,50,63,129,100,109,3,34,133,9,192,61,108,
    55,98,216,207,230,168,184,80,197,53,250,129,202,82,225,119,62,167,167,40,188,90,
    49,47,29,186,183,253,143,202,146,72,63,185,137,227,65,146,55,170,9,6,70,126,64,
    247,15,229,43,115,109,202,130,241,83,116,201,88,28,138,244,119,118,211,170,27,42,
    112,62,154,150,68,120,234,179,52,141,39,66,183,133,122,40,232,228,184,173,22,61,
    95,204,20,249,145,28,19,73,179,194,8,221,163,127,172,14,204,220,7,16,248,47,234,
    131,165,179,238,116,59,111,19,43,208,241,179,106,53,192,203,237,70,87,221,32,107,
    159,218,121,67,56,39,61,50,89,24,176,152,188,239,19,95,44,44,65,27,95,237,17,237,
    201,117,93,122,55,115,26,182,200,194,212,114,143,241,162,136,99,114,69,226,38,25,
    250,91,12,84,2,2,87,98,196,191,214,238,130,5,161,162,142,239,37,118,224,21,125,
    231,88,16,235,254,63,135,95,180,71,238,0,41,60,185,217,102,1,48,142,204,132,169,
    78,202,190,19,209,169,77,221,248,121,148,222,211,244,248,89,40,211,210,128,186,17,
    96,215,128,153,135,15,35,151,215,11,41,132,161,177,176,44,123,234,193,139,162,43,
    120,66,189,38,85,112,218,248,11,106,104,97,229,209,64,161,66,54,191,205,62,96,82,
    24,2,66,173,12,78,154,185,158,195,33,203,81,100,30,69,84,235,148,246,90,22,6,68,
    212,241,98,252,0,101,32,195,14,62,226,239,177,107,144,133,7,112,27,213,75,190,149,
    37,117,38,24,11,226,143,75,127,246,34,173,58,150,129,21,153,15,205,71,85,235,9,
    160,10,254,217,79,42,147,223,219,124,49,227,199,251,141,121,29,53,139,97,214,70,
    79,91,201,134,198,46,134,141,224,88,48,247,99,196,178,29,228,81,230,51,180,111,
    209,158,105,142,127,187,159,210,140,110,145,156,180,230,126,155,0,47,75,211,85,
    180,70,97,75,78,246,251,23,31,138,76,181,29,25,20,252,194,181,208,142,103,14,91,
    54,34,223,60,242,122,86,50,110,112,251,59,22,227,216,187,137,49,33,253,154,183,
    138,151,120,51,208,178,51,182,245,145,156,122,206,160,150,177,220,138,58,31,249,
    192,229,101,136,85,132,94,233
};

static uint8_t bit_reverse(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        r = (r << 1) | (b & 1);
        b >>= 1;
    }
    return r;
}

static uint32_t Audio_DecryptAUD(const uint8_t* fileData, uint32_t fileSize,
                                 uint8_t** outData, uint32_t* outSize) {
    if (fileSize < 140) return 0;
    if (memcmp(fileData, "ENC2", 4) == 0) {
        /* Exceed2 (PIU32.EXE 0x421390) / Exceed (exceed.exe 0x420390):
         * ver Resource_DecryptENC2 */
        uint32_t size = 0;
        uint8_t* out = Resource_DecryptENC2(fileData, fileSize, &size);
        if (!out || size == 0) { free(out); return 0; }
        *outData = out;
        *outSize = size;
        return size;
    }
    if (memcmp(fileData, "ENC1", 4) != 0) return 0;

    uint32_t dataSize = *(uint32_t*)(fileData + 0x84);
    uint32_t skip = *(uint32_t*)(fileData + 0x88);
    uint32_t dataOff = 140 + skip;
    (void)dataSize;

    if (dataOff + 4 > fileSize) return 0;
    uint32_t seed = *(uint32_t*)(fileData + dataOff);
    uint32_t encSize = fileSize - dataOff - 4;
    if (encSize == 0) return 0;

    uint8_t* buf = (uint8_t*)malloc(encSize);
    if (!buf) return 0;
    memcpy(buf, fileData + dataOff + 4, encSize);

    for (uint32_t i = 0; i < encSize; i++) {
        uint8_t br = bit_reverse(buf[i]);
        uint32_t idx = (seed + i) & 0x3FF;
        buf[i] = br ^ g_audXorTable[idx];
    }

    *outData = buf;
    *outSize = encSize;
    return encSize;
}

bool BGM_LoadAUDDirect(const char* path) {
    Log_Print("BGM: loading .AUD '%s'\n", path);

    FILE* f = fopen(path, "rb");
    if (!f) { Log_Print("BGM: failed to open '%s'\n", path); return false; }
    fseek(f, 0, SEEK_END);
    uint32_t fileSize = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* fileData = (uint8_t*)malloc(fileSize);
    if (!fileData) { fclose(f); return false; }
    size_t got = fread(fileData, 1, fileSize, f);
    fclose(f);
    if (got != fileSize) { free(fileData); return false; }

    uint8_t* decData = NULL;
    uint32_t decSize = 0;
    if (!Audio_DecryptAUD(fileData, fileSize, &decData, &decSize)) {
        Log_Print("BGM: failed to decrypt .AUD '%s'\n", path);
        free(fileData);
        return false;
    }
    free(fileData);

    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    char tmpPath[MAX_PATH];
    snprintf(tmpPath, sizeof(tmpPath), "%spumpy_bgm.mp3", tmpDir);

    FILE* ftmp = fopen(tmpPath, "wb");
    if (!ftmp) {
        free(decData);
        Log_Print("BGM: failed to create temp file '%s'\n", tmpPath);
        return false;
    }
    fwrite(decData, 1, decSize, ftmp);
    fclose(ftmp);
    free(decData);

    bool ok = BGM_Load(tmpPath);
    remove(tmpPath);

    strncpy(g_game.bgm.name, path, sizeof(g_game.bgm.name) - 1);
    Log_Print("BGM: .AUD '%s' -> %s\n", path, ok ? "ok" : "failed");
    return ok;
}

void BGM_Play(bool loop) {
    if (!g_bgmChan.inUse) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_bgmChan.pos = 0;
    g_bgmChan.playing = true;
    g_bgmCbPos = 0; g_bgmCbTicks = 0; g_bgmLastMs = 0.0;
    g_bgmChan.loop = loop;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    g_game.bgm.playing = true;
    g_game.bgm.looping = loop;
}

void BGM_Stop(void) {
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_bgmChan.playing = false;
    g_bgmChan.pos = 0;
    g_bgmCbPos = 0; g_bgmCbTicks = 0; g_bgmLastMs = 0.0;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    g_game.bgm.playing = false;
}

/* uint32_t BGM_GetPositionMs(void) {
    if (!g_bgmChan.inUse) return 0;
    return (uint32_t)(g_bgmChan.pos * 1000ull / DEV_RATE);
} */
uint32_t BGM_GetPositionMs(void) {
    if (!g_bgmChan.inUse) return 0;
    if (!g_bgmChan.playing || g_bgmCbTicks == 0) {
        g_bgmLastMs = 0.0;
        return (uint32_t)(g_bgmChan.pos * 1000ull / DEV_RATE);
    }
    if (g_dev) SDL_LockAudioDevice(g_dev);
    uint64_t basePos = g_bgmCbPos;
    uint64_t baseTicks = g_bgmCbTicks;
    uint64_t blk = g_bgmCbFrames;
    if (g_dev) SDL_UnlockAudioDevice(g_dev);

    double baseMs = (double)basePos * 1000.0 / DEV_RATE;
    double elapsed = (double)(SDL_GetPerformanceCounter() - baseTicks) * 1000.0
                     / (double)SDL_GetPerformanceFrequency();
    double maxAhead = (double)blk * 1000.0 / DEV_RATE;   /* no máx. 1 bloco */
    if (elapsed > maxAhead) elapsed = maxAhead;
    double ms = baseMs + elapsed;

    /* Monotônico: jitter do callback não pode fazer as setas voltarem.
     * Um salto grande pra trás (BGM_Play/loop) é aceito como reinício. */
    if (ms < g_bgmLastMs && g_bgmLastMs - ms < 100.0) ms = g_bgmLastMs;
    g_bgmLastMs = ms;
    return (uint32_t)ms;
}

bool BGM_HasEnded(void) {
    if (!g_bgmChan.inUse) return true;
    return (!g_bgmChan.loop && g_bgmChan.pos >= g_bgmChan.frames);
}

/* bool BGM_IsDSActive(void) { return false; } */
/* A BGM toca no mixer SDL: a posicao (g_bgmChan.pos) e valida enquanto tocando. */
bool BGM_IsDSActive(void) { return BGM_IsPlaying(); }

bool BGM_IsPlaying(void) {
    return g_bgmChan.inUse && g_bgmChan.playing;
}

void BGM_SetVolume(long volume) {
    if (!g_bgmChan.inUse) return;
    if (g_dev) SDL_LockAudioDevice(g_dev);
    g_bgmChan.vol = ds_volume_to_linear(volume);
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
}

void BGM_Update(void) {
    /* The SDL mixer loops natively; nothing to do per-frame. */
}

void BGM_Shutdown(void) {
    if (g_dev) SDL_LockAudioDevice(g_dev);
    if (g_bgmChan.inUse) {
        free(g_bgmChan.pcm);
        g_bgmChan.pcm = NULL;
        g_bgmChan.inUse = false;
        g_bgmChan.playing = false;
        g_bgmChan.pos = 0;
    }
    if (g_dev) SDL_UnlockAudioDevice(g_dev);
    g_game.bgm.playing = false;
    g_game.bgm.looping = false;
    g_game.bgm.name[0] = '\0';
    g_game.bgm.buffer = NULL;
    Log_Print("BGM: shutdown\n");
}

uint32_t BGM_GetDurationMs(void) {
    if (!g_bgmChan.inUse || g_bgmChan.frames == 0) return 0;
    return (uint32_t)(g_bgmChan.frames * 1000ull / DEV_RATE);
}