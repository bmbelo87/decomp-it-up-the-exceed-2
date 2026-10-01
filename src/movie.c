/*
 * movie.c — player dos .MOV (formato MOV2) do Exceed.
 *
 * Original (exceed.exe):
 *   0x4225A8  abre: "MOV2", pula hdr[0x88], chave 16 B, nome cifrado 32 B, byte G
 *   0x4226A0  lê 0x1000 B e desembaralha: b = bitrev(b) ^ bitrev(G) (0x4223C4/0x422420)
 *   0x422A68  decodifica até o quadro-alvo (tempo x fps) com a libmpeg2:
 *             estado 1 (SEQUENCE) -> mpeg2_convert(rgb16); 0 (BUFFER) -> mais dados;
 *             7/8 (SLICE/END) -> quadro pronto. No fim do arquivo volta a hdr[0x88]+0xC0.
 *   Quadro final RGB16 640x480.
 *
 * O port usa a própria MPEG2.dll do jogo (mesma API), carregada com LoadLibrary.
 */
#include "pumpy.h"
#include "movie.h"

#if defined(_WIN32)
#include <windows.h>

#ifndef GL_UNSIGNED_SHORT_5_6_5
#define GL_UNSIGNED_SHORT_5_6_5 0x8363
#endif

/* libmpeg2 0.3/0.4 — só os campos lidos */
typedef struct { uint8_t* buf[3]; void* id; } mp2_fbuf;
typedef struct {
    unsigned int width, height, chroma_width, chroma_height;
} mp2_sequence;
typedef struct {
    const mp2_sequence* sequence;
    const void* gop;
    const void* current_picture;
    const void* current_picture_2nd;
    const mp2_fbuf* current_fbuf;
    const void* display_picture;
    const void* display_picture_2nd;
    const mp2_fbuf* display_fbuf;
} mp2_info;

typedef void* (*fn_init)(uint32_t accel);
typedef void  (*fn_close)(void* dec);
typedef const mp2_info* (*fn_info)(void* dec);
typedef int   (*fn_parse)(void* dec);
typedef void  (*fn_buffer)(void* dec, uint8_t* start, uint8_t* end);
typedef int   (*fn_convert)(void* dec, void* conv, void* arg);

static HMODULE    g_mpegDll;
static fn_init    p_init;
static fn_close   p_close;
static fn_info    p_info;
static fn_parse   p_parse;
static fn_buffer  p_buffer;
static fn_convert p_convert;
static void*      p_rgb16;

static struct {
    FILE*    f;
    void*    dec;
    const mp2_info* info;
    uint32_t dataStart;     /* hdr[0x88] + 0xC0 */
    uint8_t  table[256];    /* bitrev(b) ^ bitrev(G) */
    uint8_t  buf[0x1000];
    double   fps;
    double   time;
    int      target;        /* [+0x2c] no original */
    int      decoded;       /* [+0x30] */
    bool     loop;
    bool     ended;
    GLuint   tex;
    bool     hasFrame;
} g_mov;

static uint8_t bitrev8(uint8_t b) {                                /* 0x4223C4 */
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) if (b & (1 << i)) r |= (uint8_t)(0x80 >> i);
    return r;
}

static bool movie_load_dll(void) {
    if (g_mpegDll) return true;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\MPEG2.dll", g_game.currentDirectory);
    g_mpegDll = LoadLibraryA(path);
    if (!g_mpegDll) { Log_Print("MOVIE: falha ao carregar '%s'\n", path); return false; }
    p_init    = (fn_init)   GetProcAddress(g_mpegDll, "mpeg2_init");
    p_close   = (fn_close)  GetProcAddress(g_mpegDll, "mpeg2_close");
    p_info    = (fn_info)   GetProcAddress(g_mpegDll, "mpeg2_info");
    p_parse   = (fn_parse)  GetProcAddress(g_mpegDll, "mpeg2_parse");
    p_buffer  = (fn_buffer) GetProcAddress(g_mpegDll, "mpeg2_buffer");
    p_convert = (fn_convert)GetProcAddress(g_mpegDll, "mpeg2_convert");
    p_rgb16   = (void*)     GetProcAddress(g_mpegDll, "mpeg2convert_rgb16");
    if (!p_init || !p_close || !p_info || !p_parse || !p_buffer || !p_convert || !p_rgb16) {
        Log_Print("MOVIE: MPEG2.dll sem as funcoes esperadas\n");
        FreeLibrary(g_mpegDll); g_mpegDll = NULL;
        return false;
    }
    return true;
}

/* 0x4226D8: fps pelo frame_rate_code do sequence header */
static double movie_fps(const uint8_t* s) {
    static const double rates[8] = { 24000.0/1001, 24, 25, 30000.0/1001, 30, 50, 60000.0/1001, 60 };
    if (s[0] == 0 && s[1] == 0 && s[2] == 1 && s[3] == 0xB3) {
        int code = s[7] & 0x0F;
        if (code >= 1 && code <= 8) return rates[code - 1];
    }
    return 30000.0 / 1001;
}

static int movie_read(void) {                                      /* 0x4226A0 */
    int n = (int)fread(g_mov.buf, 1, sizeof(g_mov.buf), g_mov.f);
    for (int i = 0; i < n; i++) g_mov.buf[i] = g_mov.table[g_mov.buf[i]];
    return n;
}

bool Movie_Open(const char* path, bool loop) {
    Movie_Close();
    if (!movie_load_dll()) return false;

    FILE* f = fopen(path, "rb");
    if (!f) { Log_Print("MOVIE: falha ao abrir '%s'\n", path); return false; }
    uint8_t hdr[0x8C];
    size_t hdrGot = fread(hdr, 1, sizeof(hdr), f);
    if (hdrGot >= 8 && hdr[0] == 0 && hdr[1] == 0 && hdr[2] == 1 && hdr[3] == 0xB3) {
        /* Exceed2 (PIU32.EXE 0x4209f0): MPEG elementar puro, sem cabeçalho nem
         * embaralhamento — fps pelos primeiros 0x40 bytes (0x420e60) e blocos de
         * 0x1000 desde o offset 0 direto no mpeg2_buffer. */
        for (int b = 0; b < 256; b++) g_mov.table[b] = (uint8_t)b;
        g_mov.f = f;
        g_mov.dataStart = 0;
        g_mov.fps = movie_fps(hdr);
        fseek(f, 0, SEEK_SET);
        g_mov.dec = p_init(0);
        if (!g_mov.dec) { fclose(f); g_mov.f = NULL; return false; }
        g_mov.info = p_info(g_mov.dec);
        g_mov.loop = loop;
        g_mov.time = 0;
        g_mov.target = 0;
        g_mov.decoded = 0;
        g_mov.ended = false;
        g_mov.hasFrame = false;
        if (!g_mov.tex) glGenTextures(1, &g_mov.tex);
        Log_Print("MOVIE: '%s' aberto (MPEG puro, %.3f fps, loop=%d)\n", path, g_mov.fps, loop);
        return true;
    }
    if (hdrGot != sizeof(hdr) || memcmp(hdr, "MOV2", 4) != 0) {
        Log_Print("MOVIE: '%s' nao e MOV2\n", path);
        fclose(f); return false;
    }
    uint32_t n = *(uint32_t*)(hdr + 0x88);
    uint8_t tail[0x34];
    fseek(f, (long)n, SEEK_CUR);
    if (fread(tail, 1, sizeof(tail), f) != sizeof(tail)) { fclose(f); return false; }
    uint8_t k = bitrev8(tail[0x30]);
    for (int b = 0; b < 256; b++) g_mov.table[b] = (uint8_t)(bitrev8((uint8_t)b) ^ k);

    g_mov.f = f;
    g_mov.dataStart = n + 0xC0;
    fseek(f, (long)g_mov.dataStart, SEEK_SET);
    uint8_t first[8];
    size_t got = fread(first, 1, sizeof(first), f);
    for (size_t i = 0; i < got; i++) first[i] = g_mov.table[first[i]];
    g_mov.fps = movie_fps(first);
    fseek(f, (long)g_mov.dataStart, SEEK_SET);

    g_mov.dec = p_init(0);
    if (!g_mov.dec) { fclose(f); g_mov.f = NULL; return false; }
    g_mov.info = p_info(g_mov.dec);
    g_mov.loop = loop;
    g_mov.time = 0;
    g_mov.target = 0;
    g_mov.decoded = 0;
    g_mov.ended = false;
    g_mov.hasFrame = false;
    if (!g_mov.tex) glGenTextures(1, &g_mov.tex);
    Log_Print("MOVIE: '%s' aberto (N=0x%X, %.3f fps, loop=%d)\n", path, n, g_mov.fps, loop);
    return true;
}

void Movie_Close(void) {
    if (g_mov.dec) { p_close(g_mov.dec); g_mov.dec = NULL; }
    if (g_mov.f) { fclose(g_mov.f); g_mov.f = NULL; }
    g_mov.hasFrame = false;
}

bool Movie_IsOpen(void) { return g_mov.f != NULL; }
bool Movie_HasEnded(void) { return g_mov.ended; }
int  Movie_GetDecoded(void) { return g_mov.decoded; }

static void movie_upload(void) {
    const mp2_fbuf* fb = g_mov.info->display_fbuf;
    const mp2_sequence* sq = g_mov.info->sequence;
    if (!fb || !fb->buf[0] || !sq) return;
    glBindTexture(GL_TEXTURE_2D, g_mov.tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 2);
    if (!g_mov.hasFrame) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, (GLsizei)sq->width, (GLsizei)sq->height, 0,
                 GL_RGB, GL_UNSIGNED_SHORT_5_6_5, fb->buf[0]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    g_mov.hasFrame = true;
}

/* 0x422A68: decodifica até decoded > target */
void Movie_Update(float dt) {
    if (!g_mov.f || g_mov.ended) return;
    g_mov.time += dt;
    g_mov.target = (int)(g_mov.time * g_mov.fps);
    bool newFrame = false;
    while (g_mov.decoded <= g_mov.target) {
        int st = p_parse(g_mov.dec);
        if (st == 1) {
            p_convert(g_mov.dec, p_rgb16, NULL);
        } else if (st == 0) {
            int n = movie_read();
            if (n <= 0) {
                if (!g_mov.loop) {
                    Log_Print("MOVIE: fim do arquivo (%d quadros)\n", g_mov.decoded);
                    g_mov.ended = true; break;
                }
                fseek(g_mov.f, (long)g_mov.dataStart, SEEK_SET);
                n = movie_read();
                if (n <= 0) { g_mov.ended = true; break; }
            }
            p_buffer(g_mov.dec, g_mov.buf, g_mov.buf + n);
        } else if (st == 7 || st == 8) {
            g_mov.decoded++;
            newFrame = true;
        }
    }
    if (newFrame) movie_upload();
}

/* Tela cheia 640x480. Projeção Y-UP: linha 0 do quadro (topo) vai em y=480. */
void Movie_Render(void) {
    if (!g_mov.hasFrame) return;
    /* Restaura o blend no fim: o gameplay desenha por cima contando com o
     * estado que estava ligado (sem isso os sprites saem com fundo). */
    GLboolean blend = glIsEnabled(GL_BLEND);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
    glBindTexture(GL_TEXTURE_2D, g_mov.tex);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(0, 480);
    glTexCoord2f(1, 0); glVertex2f(640, 480);
    glTexCoord2f(1, 1); glVertex2f(640, 0);
    glTexCoord2f(0, 1); glVertex2f(0, 0);
    glEnd();
    if (blend) glEnable(GL_BLEND);
}

#else /* sem MPEG2.dll fora do Windows */

bool Movie_Open(const char* path, bool loop) {
    (void)loop;
    Log_Print("MOVIE: '%s' ignorado (MPEG2.dll so no Windows)\n", path);
    return false;
}
void Movie_Close(void) {}
bool Movie_IsOpen(void) { return false; }
bool Movie_HasEnded(void) { return true; }
int  Movie_GetDecoded(void) { return 0; }
void Movie_Update(float dt) { (void)dt; }
void Movie_Render(void) {}

#endif
