/* xbrz.c — ampliação de texturas xBRZ (2x/3x/4x) ao carregar.
 *
 * Recurso do port, fora do original (GRAPHICS SETTINGS > UPSCALE, padrão OFF).
 * Porte em C do algoritmo xBRZ (Zenju, GPLv3), variante RGBA: distância YCbCr
 * (BT.2020) ponderada pelo alfa e mistura "alphaGrad" que respeita
 * transparência. Entrada/saída em RGBA 8 bits, linha 0 = topo.
 *
 * Etapas:
 *  1. Para cada janela 2x2 (f g / j k) com vizinhança 4x4, decide qual
 *     diagonal é borda e marca o canto de cada pixel voltado para o centro
 *     da janela (NONE / NORMAL / DOMINANT).
 *  2. Cada pixel vira um bloco NxN preenchido com a própria cor; para cada
 *     canto marcado (4 rotações), aplica o padrão do escalador: linha rasa,
 *     íngreme, as duas, diagonal ou só o canto. */
#ifdef _MSC_VER
/* Otimiza este arquivo mesmo no build Debug (/Od /RTC1): sem isso a Select em
 * 4X levava ~20 s de tela preta e o Windows marcava o jogo como travado
 * (AppHang). Medido: 22.6 ms -> 10.6 ms por textura 256² em 4x (Release: 7.4). */
#pragma optimize("gty", on)
#pragma runtime_checks("", off)
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#define XB_NONE     0
#define XB_NORMAL   1
#define XB_DOMINANT 2

static const double kEqualTol   = 30.0;
static const double kCenterBias = 4.0;
static const double kDominant   = 3.6;
static const double kSteep      = 2.2;

typedef uint32_t px_t;   /* R | G<<8 | B<<16 | A<<24 (bytes RGBA em little-endian) */
#define PR(c) ((int)((c) & 0xFF))
#define PG(c) ((int)(((c) >> 8) & 0xFF))
#define PB(c) ((int)(((c) >> 16) & 0xFF))
#define PA(c) ((int)((c) >> 24))

static double distYCbCr(px_t p1, px_t p2)
{
    const double kB = 0.0593, kR = 0.2627, kG = 1.0 - kB - kR;
    const double sB = 0.5 / (1.0 - kB), sR = 0.5 / (1.0 - kR);
    int r = PR(p1) - PR(p2), g = PG(p1) - PG(p2), b = PB(p1) - PB(p2);
    double y  = kR * r + kG * g + kB * b;
    double cb = sB * (b - y);
    double cr = sR * (r - y);
    return sqrt(y * y + cb * cb + cr * cr);
}

static double dist(px_t p1, px_t p2)
{
    double a1 = PA(p1) / 255.0, a2 = PA(p2) / 255.0;
    double d = distYCbCr(p1, p2);
    return (a1 < a2) ? a1 * d + 255.0 * (a2 - a1) : a2 * d + 255.0 * (a1 - a2);
}

static bool eq(px_t a, px_t b) { return dist(a, b) < kEqualTol; }

/* Mistura M/N de col sobre *dst, respeitando o alfa das duas cores. */
static void alphaGrad(int M, int N, px_t* dst, px_t col)
{
    double wF = (double)PA(col) * M;
    double wB = (double)PA(*dst) * (N - M);
    double ws = wF + wB;
    if (ws <= 0.0) { *dst = 0; return; }
    int r = (int)((PR(col) * wF + PR(*dst) * wB) / ws + 0.5);
    int g = (int)((PG(col) * wF + PG(*dst) * wB) / ws + 0.5);
    int b = (int)((PB(col) * wF + PB(*dst) * wB) / ws + 0.5);
    int a = (int)(ws / N + 0.5);
    if (a > 255) a = 255;
    *dst = (px_t)r | ((px_t)g << 8) | ((px_t)b << 16) | ((px_t)a << 24);
}

/* Cantos no byte de blend: TL bits 0-1, TR 2-3, BR 4-5, BL 6-7. */
static int cornerShift(int cx, int cy)
{
    if (cx < 0) return (cy < 0) ? 0 : 6;
    return (cy < 0) ? 2 : 4;
}

/* Gira (x,y) 90° no sentido horário k vezes (x p/ direita, y p/ baixo). */
static void rot(int k, int* x, int* y)
{
    for (int i = 0; i < k; i++) { int t = *x; *x = -*y; *y = t; }
}

typedef struct {
    const px_t* src; int w, h;
    const uint8_t* blend;
    px_t* blk; int N; int k;       /* bloco de saída NxN e rotação atual */
} XbCtx;

static px_t nb(const XbCtx* c, int x, int y, int lx, int ly)
{
    rot(c->k, &lx, &ly);
    x += lx; y += ly;
    if (x < 0) x = 0; else if (x >= c->w) x = c->w - 1;
    if (y < 0) y = 0; else if (y >= c->h) y = c->h - 1;
    return c->src[y * c->w + x];
}

static int corner(const XbCtx* c, uint8_t bi, int lx, int ly)
{
    rot(c->k, &lx, &ly);
    return (bi >> cornerShift(lx, ly)) & 3;
}

/* Célula (I linha, J coluna) do bloco no referencial girado. */
static px_t* ref(const XbCtx* c, int I, int J)
{
    int X = 2 * J - (c->N - 1), Y = 2 * I - (c->N - 1);
    rot(c->k, &X, &Y);
    return &c->blk[((Y + c->N - 1) / 2) * c->N + (X + c->N - 1) / 2];
}

static void blendShallow(const XbCtx* c, px_t col)
{
    switch (c->N) {
    case 2: alphaGrad(1, 4, ref(c, 1, 0), col); alphaGrad(3, 4, ref(c, 1, 1), col); break;
    case 3: alphaGrad(1, 4, ref(c, 2, 0), col); alphaGrad(1, 4, ref(c, 1, 2), col);
            alphaGrad(3, 4, ref(c, 2, 1), col); *ref(c, 2, 2) = col; break;
    default:alphaGrad(1, 4, ref(c, 3, 0), col); alphaGrad(1, 4, ref(c, 2, 2), col);
            alphaGrad(3, 4, ref(c, 3, 1), col); alphaGrad(3, 4, ref(c, 2, 3), col);
            *ref(c, 3, 2) = col; *ref(c, 3, 3) = col; break;
    }
}

static void blendSteep(const XbCtx* c, px_t col)
{
    switch (c->N) {
    case 2: alphaGrad(1, 4, ref(c, 0, 1), col); alphaGrad(3, 4, ref(c, 1, 1), col); break;
    case 3: alphaGrad(1, 4, ref(c, 0, 2), col); alphaGrad(1, 4, ref(c, 2, 1), col);
            alphaGrad(3, 4, ref(c, 1, 2), col); *ref(c, 2, 2) = col; break;
    default:alphaGrad(1, 4, ref(c, 0, 3), col); alphaGrad(1, 4, ref(c, 2, 2), col);
            alphaGrad(3, 4, ref(c, 1, 3), col); alphaGrad(3, 4, ref(c, 3, 2), col);
            *ref(c, 2, 3) = col; *ref(c, 3, 3) = col; break;
    }
}

static void blendSteepShallow(const XbCtx* c, px_t col)
{
    switch (c->N) {
    case 2: alphaGrad(1, 4, ref(c, 1, 0), col); alphaGrad(1, 4, ref(c, 0, 1), col);
            alphaGrad(5, 6, ref(c, 1, 1), col); break;
    case 3: alphaGrad(1, 4, ref(c, 2, 0), col); alphaGrad(1, 4, ref(c, 0, 2), col);
            alphaGrad(3, 4, ref(c, 2, 1), col); alphaGrad(3, 4, ref(c, 1, 2), col);
            *ref(c, 2, 2) = col; break;
    default:alphaGrad(3, 4, ref(c, 3, 1), col); alphaGrad(3, 4, ref(c, 1, 3), col);
            alphaGrad(1, 4, ref(c, 3, 0), col); alphaGrad(1, 4, ref(c, 0, 3), col);
            alphaGrad(1, 3, ref(c, 2, 2), col);
            *ref(c, 3, 3) = col; *ref(c, 3, 2) = col; *ref(c, 2, 3) = col; break;
    }
}

static void blendDiagonal(const XbCtx* c, px_t col)
{
    switch (c->N) {
    case 2: alphaGrad(1, 2, ref(c, 1, 1), col); break;
    case 3: alphaGrad(1, 8, ref(c, 1, 2), col); alphaGrad(1, 8, ref(c, 2, 1), col);
            alphaGrad(7, 8, ref(c, 2, 2), col); break;
    default:alphaGrad(1, 2, ref(c, 3, 2), col); alphaGrad(1, 2, ref(c, 2, 3), col);
            *ref(c, 3, 3) = col; break;
    }
}

static void blendCorner(const XbCtx* c, px_t col)
{
    switch (c->N) {
    case 2: alphaGrad(21, 100, ref(c, 1, 1), col); break;   /* ~1 - pi/4 */
    case 3: alphaGrad(45, 100, ref(c, 2, 2), col); break;
    default:alphaGrad(68, 100, ref(c, 3, 3), col);
            alphaGrad(9, 100, ref(c, 3, 2), col); alphaGrad(9, 100, ref(c, 2, 3), col); break;
    }
}

/* Trata o canto inferior-direito (no referencial girado) do pixel (x,y). */
static void blendPixel(const XbCtx* c, int x, int y, uint8_t bi)
{
    int bBR = corner(c, bi, 1, 1);
    if (bBR < XB_NORMAL) return;

    px_t b = nb(c, x, y,  0, -1), cc = nb(c, x, y, 1, -1);
    px_t d = nb(c, x, y, -1,  0), e  = nb(c, x, y, 0,  0), f = nb(c, x, y, 1, 0);
    px_t g = nb(c, x, y, -1,  1), h  = nb(c, x, y, 0,  1), i = nb(c, x, y, 1, 1);

    bool doLine;
    if (bBR >= XB_DOMINANT) doLine = true;
    else if (corner(c, bi, 1, -1) != XB_NONE && !eq(e, g)) doLine = false;   /* TR */
    else if (corner(c, bi, -1, 1) != XB_NONE && !eq(e, cc)) doLine = false;  /* BL */
    else if (!eq(e, i) && eq(g, h) && eq(h, i) && eq(i, f) && eq(f, cc)) doLine = false;
    else doLine = true;

    px_t col = (dist(e, f) <= dist(e, h)) ? f : h;

    if (doLine) {
        double fg = dist(f, g), hc = dist(h, cc);
        bool shallow = kSteep * fg <= hc && e != g && d != g;
        bool steep   = kSteep * hc <= fg && e != cc && b != cc;
        if (shallow && steep) blendSteepShallow(c, col);
        else if (shallow)     blendShallow(c, col);
        else if (steep)       blendSteep(c, col);
        else                  blendDiagonal(c, col);
    } else {
        blendCorner(c, col);
    }
}

static px_t px(const px_t* s, int w, int h, int x, int y)
{
    if (x < 0) x = 0; else if (x >= w) x = w - 1;
    if (y < 0) y = 0; else if (y >= h) y = h - 1;
    return s[y * w + x];
}

/* Etapa 1: janela f=(x,y) g=(x+1,y) j=(x,y+1) k=(x+1,y+1). */
static void preProcess(const px_t* s, int w, int h, uint8_t* bl)
{
    for (int y = -1; y < h; y++)
    for (int x = -1; x < w; x++) {
        px_t a = px(s,w,h,x-1,y-1), b = px(s,w,h,x,y-1), c = px(s,w,h,x+1,y-1), d = px(s,w,h,x+2,y-1);
        px_t e = px(s,w,h,x-1,y  ), f = px(s,w,h,x,y  ), g = px(s,w,h,x+1,y  ), hh= px(s,w,h,x+2,y  );
        px_t i = px(s,w,h,x-1,y+1), j = px(s,w,h,x,y+1), k = px(s,w,h,x+1,y+1), l = px(s,w,h,x+2,y+1);
        px_t m = px(s,w,h,x-1,y+2), n = px(s,w,h,x,y+2), o = px(s,w,h,x+1,y+2), p = px(s,w,h,x+2,y+2);
        (void)a; (void)d; (void)m; (void)p;

        if ((f == g && j == k) || (f == j && g == k)) continue;

        double jg = dist(i, f) + dist(f, c) + dist(n, k) + dist(k, hh) + kCenterBias * dist(j, g);
        double fk = dist(e, j) + dist(j, o) + dist(b, g) + dist(g, l) + kCenterBias * dist(f, k);

        int bF = XB_NONE, bG = XB_NONE, bJ = XB_NONE, bK = XB_NONE;
        if (jg < fk) {
            int t = (kDominant * jg < fk) ? XB_DOMINANT : XB_NORMAL;
            if (f != g && f != j) bF = t;
            if (k != j && k != g) bK = t;
        } else if (fk < jg) {
            int t = (kDominant * fk < jg) ? XB_DOMINANT : XB_NORMAL;
            if (j != f && j != k) bJ = t;
            if (g != f && g != k) bG = t;
        }
        /* f: canto BR, g: BL, j: TR, k: TL */
        if (x >= 0 && y >= 0)         bl[y * w + x]           |= (uint8_t)(bF << 4);
        if (x + 1 < w && y >= 0)      bl[y * w + x + 1]       |= (uint8_t)(bG << 6);
        if (x >= 0 && y + 1 < h)      bl[(y + 1) * w + x]     |= (uint8_t)(bJ << 2);
        if (x + 1 < w && y + 1 < h)   bl[(y + 1) * w + x + 1] |= (uint8_t)(bK << 0);
    }
}

/* Amplia src (w x h RGBA) por N (2..4). Devolve buffer novo (N*w x N*h) ou NULL. */
uint8_t* Xbrz_Scale(const uint8_t* srcRGBA, int w, int h, int N)
{
    if (N < 2 || N > 4 || w <= 0 || h <= 0) return NULL;
    px_t* s = (px_t*)malloc((size_t)w * h * sizeof(px_t));
    uint8_t* bl = (uint8_t*)calloc((size_t)w * h, 1);
    px_t* out = (px_t*)malloc((size_t)w * h * N * N * sizeof(px_t));
    if (!s || !bl || !out) { free(s); free(bl); free(out); return NULL; }
    for (int i = 0; i < w * h; i++) {
        const uint8_t* q = srcRGBA + (size_t)i * 4;
        s[i] = (px_t)q[0] | ((px_t)q[1] << 8) | ((px_t)q[2] << 16) | ((px_t)q[3] << 24);
    }
    preProcess(s, w, h, bl);

    px_t blk[16];
    XbCtx c = { s, w, h, bl, blk, N, 0 };
    int ow = w * N;
    for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++) {
        px_t e = s[y * w + x];
        for (int q = 0; q < N * N; q++) blk[q] = e;
        uint8_t bi = bl[y * w + x];
        if (bi)
            for (c.k = 0; c.k < 4; c.k++) blendPixel(&c, x, y, bi);
        for (int by = 0; by < N; by++)
            memcpy(&out[(size_t)(y * N + by) * ow + x * N], &blk[by * N], N * sizeof(px_t));
    }
    free(s); free(bl);

    uint8_t* o8 = (uint8_t*)out;   /* px_t little-endian = bytes RGBA */
    for (size_t i = 0; i < (size_t)ow * h * N; i++) {
        px_t v = out[i];
        o8[i * 4 + 0] = (uint8_t)PR(v); o8[i * 4 + 1] = (uint8_t)PG(v);
        o8[i * 4 + 2] = (uint8_t)PB(v); o8[i * 4 + 3] = (uint8_t)PA(v);
    }
    return o8;
}
