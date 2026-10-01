#include "pumpy.h"
#include "font8x8_basic.h"

#define FONT_LIST_BASE 1000
#define FONT_GLYPH_W 8
#define FONT_GLYPH_H 16   /* font8x8 rows doubled vertically */

static bool g_fontInit = false;

/* font8x8_basic guarda cada linha com o BIT 0 (LSB) como pixel da ESQUERDA.
 * Quem lê MSB-primeiro espelha o glifo na horizontal — era o que deixava o
 * overlay amarelo do Render_StateInfo com as letras invertidas. glBitmap, ao
 * contrário, quer o bit 7 à esquerda, então aqui a linha é invertida bit a bit. */
static uint8_t font_row_msb(unsigned char ch, int r) {
    uint8_t in = (uint8_t)font8x8_basic[ch][r];
    uint8_t out = 0;
    for (int b = 0; b < 8; b++)
        if (in & (uint8_t)(1u << b)) out |= (uint8_t)(0x80u >> b);
    return out;
}

/* Build one display list per ASCII character: a GL bitmap 8 wide x 16 high,
 * made by doubling each source row (8x8 -> 8x16) and flipping vertically so
 * the first bitmap row is the bottom of the glyph (glBitmap is bottom-up). */
static void build_glyph_list(unsigned char ch) {
    if (ch >= 128) {
        /* No glyph data past ASCII; make an empty list so indices stay 1:1. */
        glNewList(FONT_LIST_BASE + ch, GL_COMPILE);
        static const uint8_t blank[FONT_GLYPH_H] = {0}; /* NULL crasha o driver NVIDIA */
        glBitmap(FONT_GLYPH_W, FONT_GLYPH_H, 0.0f, 0.0f, 0.0f, 0.0f, blank);
        glEndList();
        return;
    }
    uint8_t rows[FONT_GLYPH_H];
    for (int r = 0; r < 8; r++) {
        uint8_t row = font_row_msb(ch, r);
        rows[15 - 2 * r]     = row;   /* top glyph row -> top bitmap raw */
        rows[14 - 2 * r]     = row;
    }
    glNewList(FONT_LIST_BASE + ch, GL_COMPILE);
    glBitmap(FONT_GLYPH_W, FONT_GLYPH_H, 0.0f, 0.0f, 0.0f, 0.0f, rows);
    glEndList();
}

bool Font_Init(void) {
    if (g_fontInit) return true;

    for (int i = 0; i < 256; i++)
        build_glyph_list((unsigned char)i);

    g_fontInit = true;
    Log_Print("Font: initialized (font8x8 bitmaps, %dx%d)\n", FONT_GLYPH_W, FONT_GLYPH_H);
    return true;
}

void Font_DrawChar(int x, int y, unsigned char c, float r, float g, float b, float a) {
    Font_DrawString(x, y, (char[]){ (char)c, '\0' }, r, g, b, a);
}

void Font_DrawString(int x, int y, const char* str, float r, float g, float b, float a) {
    if (!str || !g_fontInit) return;
    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(r, g, b, a);
    int ox = x;
    glBegin(GL_QUADS);
    while (*str) {
        if (*str == '\n') { y += 16; x = ox; }
        else {
            unsigned char c = (unsigned char)*str;
            if (c < 128) {
                for (int row = 0; row < 8; row++) {
                    uint8_t bits = (uint8_t)font8x8_basic[c][row];
                    for (int col = 0; col < 8; col++) {
                        /* bit 0 = pixel da esquerda (ver font_row_msb) */
                        if (bits & (uint8_t)(1u << col)) {
                            float px = (float)(x + col);
                            float py = 480.0f - (float)(y + row * 2 + 2);
                            glVertex2f(px, py);
                            glVertex2f(px + 1.0f, py);
                            glVertex2f(px + 1.0f, py + 2.0f);
                            glVertex2f(px, py + 2.0f);
                        }
                    }
                }
            }
            x += 8;
        }
        str++;
    }
    glEnd();
    glPopAttrib();
}

void Font_DrawStringCentered(int x, int y, const char* str, float r, float g, float b, float a) {
    int len = (int)strlen(str);
    x -= (len * 8) / 2;
    Font_DrawString(x, y, str, r, g, b, a);
}

void Font_DrawStringScaled(int x, int y, const char* str, float r, float g, float b, float a, float scale) {
    if (!str || !g_fontInit) return;
    glPushAttrib(GL_ENABLE_BIT | GL_CURRENT_BIT | GL_DEPTH_BUFFER_BIT | GL_TRANSFORM_BIT);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(r, g, b, a);

    int ox = x;
    glBegin(GL_QUADS);
    while (*str) {
        if (*str == '\n') { y += (int)(16.0f * scale); x = ox; }
        else {
            unsigned char c = (unsigned char)*str;
            if (c < 128) {
                for (int row = 0; row < 8; row++) {
                    uint8_t bits = (uint8_t)font8x8_basic[c][row];
                    for (int col = 0; col < 8; col++) {
                        /* bit 0 = pixel da esquerda (ver font_row_msb) */
                        if (bits & (uint8_t)(1u << col)) {
                            float px = (float)x + col * scale;
                            float py = 480.0f - ((float)y + (row + 1) * 2.0f * scale);
                            float pw = scale;
                            float ph = 2.0f * scale;
                            glVertex2f(px, py);
                            glVertex2f(px + pw, py);
                            glVertex2f(px + pw, py + ph);
                            glVertex2f(px, py + ph);
                        }
                    }
                }
            }
            x += (int)(8.0f * scale);
        }
        str++;
    }
    glEnd();
    glPopAttrib();
}

void Font_DrawStringCenteredScaled(int x, int y, const char* str, float r, float g, float b, float a, float scale) {
    int len = (int)strlen(str);
    x -= (int)((len * 8 * scale) / 2);
    Font_DrawStringScaled(x, y, str, r, g, b, a, scale);
}

int g_fontTexId = -1;

int Font_LoadTexture(void)
{
    if (g_fontTexId >= 0) return g_fontTexId;

    // Method 1: load from 00.DAT via RES system
    char datPath[MAX_PATH];
    snprintf(datPath, sizeof(datPath), "%s/BGA/00.DAT", g_game.currentDirectory);
    Log_Print("Font: trying RES load from '%s'\n", datPath);

    if (RES_Open(datPath)) {
        g_fontTexId = loadTextureFromRES("font.png");
        RES_Close();
        if (g_fontTexId >= 0) {
            Log_Print("Font: loaded via RES, texId=%d\n", g_fontTexId);
            return g_fontTexId;
        }
        Log_Print("Font: loadTextureFromRES failed\n");
    } else {
        Log_Print("Font: RES_Open failed for '%s'\n", datPath);
    }

    // Method 2: load directly from extracted file
    char extPath[MAX_PATH];
    snprintf(extPath, sizeof(extPath), "%s/BGA_extracted/00/FONT.PNG", g_game.currentDirectory);
    Log_Print("Font: trying direct load from '%s'\n", extPath);
    g_fontTexId = Texture_Load(extPath);
    if (g_fontTexId >= 0) {
        Log_Print("Font: loaded via direct file, texId=%d\n", g_fontTexId);
        return g_fontTexId;
    }

    Log_Print("Font: ALL loading methods failed for font texture\n");
    return -1;
}

void Font_DrawDigit(int texId, int digit, int x, int y, float scale)
{
    if (texId < 0 || digit < 0 || digit > 9) return;

    int col = digit % 8;
    int row = digit / 8;

    int u1 = col * 32;
    int v1 = row * 31 + 73;
    int u2 = u1 + 32;
    int v2 = v1 + 31;

    int w = (int)(36 * scale);
    int h = (int)(39 * scale);

    /* V=0 é o topo da imagem (FONT.PNG, sem inversão no decoder), e o
     * Texture_DrawUV manda o primeiro V para o vértice de cima — então v1/v2
     * vão diretos. O complemento texH-v amostrava a metade errada do atlas. */
    Texture_DrawUV(texId, (float)x, (float)y, (float)w, (float)h,
                   (float)u1, (float)v1, (float)u2, (float)v2,
                   1.0f, 1.0f, 1.0f, 1.0f);
}

void Font_DrawNumber(int texId, int x, int y, int number, int digits, float scale)
{
    int spacing = (int)(22 * scale);
    int totalW = digits * spacing;
    int startX = x - totalW / 2 + spacing;

    for (int i = digits - 1; i >= 0; i--)
    {
        int d = number % 10;
        number /= 10;
        Font_DrawDigit(texId, d, startX + i * spacing, y, scale);
    }
}

void Font_Shutdown(void) {
    g_fontTexId = -1;
    g_fontDec00Id = -1;
    if (g_fontInit) {
        glDeleteLists(FONT_LIST_BASE, 256);
        g_fontInit = false;
    }
}

void Font_LoadFontOnly(void) {
    if (g_fontTexId >= 0) return;

    char datPath[MAX_PATH];
    snprintf(datPath, sizeof(datPath), "%s/BGA/00.DAT", g_game.currentDirectory);
    Log_Print("Font_LoadFontOnly: opening '%s'\n", datPath);

    if (!RES_Open(datPath)) {
        Log_Print("Font_LoadFontOnly: RES_Open failed\n");
        return;
    }

    g_fontTexId = loadTextureFromRES("font.tga");
    RES_Close();
    Log_Print("Font_LoadFontOnly: texId=%d\n", g_fontTexId);
}

const char* GetVersionString(void) {
    return "X3.1.PC";
}

void Font_DrawText(float x, float y, const char* text) {
    if (g_fontTexId < 0 || !text) return;

    Texture_Bind(g_fontTexId);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // font.tga 256x256: primeiras 3 faixas horizontais contêm os glifos ASCII
    //   Bloco 0 (ASCII 32-63): linhas 0-13, glifo em pixels 3-11 (9px altura)
    //   Bloco 1 (ASCII 64-95): linhas 18-26, glifo em pixels 18-26 (9px altura)
    //   Bloco 2 (ASCII 96-127): linhas 31-44, glifo em pixels 35-41 (7px altura)
    static const int blkY[] = { 0, 18, 31 };
    static const int glyY[] = { 3, 0, 4 };
    static const int glyH[] = { 9, 9, 7 };

    while (*text) {
        unsigned char ch = (unsigned char)*text;
        if (ch >= 32 && ch < 128) {
            int idx = ch - 32;
            int row = idx / 32;
            int col = idx % 32;

            float u = (float)(col * 8) / 256.0f;
            float uw = 8.0f / 256.0f;
            int py = blkY[row] + glyY[row];
            int ph = glyH[row];

            // A fonte vem de FONT.PNG (o RES não tem TGA; loadTextureFromRES
            // resolve font.tga -> FONT.PNG) e o decoder de PNG não inverte
            // linhas, então V=0 é o topo da imagem: usa o V direto. Com o
            // complemento antigo (1-V) a amostragem caía na metade de baixo
            // do atlas, fora da área dos glifos.
            float v0 = (float)(py + ph) / 256.0f;   // base do glifo
            float v1 = (float)py / 256.0f;          // topo do glifo

            // Renderiza 8px largura × 12px altura
            float x0 = x;
            float x1 = x + 8.0f;
            float y0 = y;
            float y1 = y + 12.0f;

            glBegin(GL_QUADS);
            glTexCoord2f(u, v0); glVertex2f(x0, y0);
            glTexCoord2f(u + uw, v0); glVertex2f(x1, y0);
            glTexCoord2f(u + uw, v1); glVertex2f(x1, y1);
            glTexCoord2f(u, v1); glVertex2f(x0, y1);
            glEnd();
        }
        x += 8;
        text++;
    }
}

// dec00.tga digit — igual FUN_0040c780. (x,y) em screen Y-DOWN
void Font_DrawDecDigit(int texId, float x, float y, int digit, float alpha, float scaleX, float scaleY, float r, float g, float b)
{
    if (texId < 0 || digit < 0 || digit > 9) return;
    int col = digit % 5;
    int row = digit / 5;
    float w = 48.4f * scaleX;
    float h = 49.5f * scaleY;
    float yUp = 480.0f - y - h;
    float u0 = (float)col * 0.171875f;
    float u1 = u0 + 0.171875f;
    /* DEC00.PNG: dígitos 0-4 nas linhas 168..213, 5-9 nas 213..258. V=0 é o
     * topo da imagem (o decoder de PNG não inverte linhas), então o V vai
     * direto — vTop no vértice de cima, vEnd no de baixo. O complemento
     * (1-V) que havia aqui amostrava as linhas 43..88, ou seja, a faixa do
     * GREAT em vez dos números do combo. */
    float vTop = (float)row * 0.17578125f + 0.65625f;
    /* Ajuste EMPÍRICO: no DEC00.PNG os dígitos 5-9 estão desenhados 1 px mais
     * baixos na célula (topo na linha 2, contra linha 1 nos 0-4). Nas capturas
     * do original (combos 019, 027) os dígitos aparecem alinhados, então a linha
     * 5-9 começa 1 px abaixo (214). A causa no original não foi localizada. */
    if (row == 1) vTop += 1.0f / 256.0f;
    float vEnd = vTop + 0.17578125f;
    /* Row 1 (dígitos 5-9): vEnd = 1.00781 > 1.0 → com GL_REPEAT mostra linha do topo da textura.
     * GL_CLAMP_TO_EDGE em Texture_CreateGL é o fix primário, mas clampamos aqui também por segurança. */
    /* if (vEnd > 1.0f) vEnd = 1.0f;
     * O corte encolhia a faixa da linha 5-9 para 43 px esticados em 45 px de
     * altura: os dígitos 5-9 desciam alguns pixels. O original (0x40c780) usa os
     * 45 px exatos; com CLAMP_TO_EDGE as 2 linhas além de 1.0 repetem a última
     * linha do DEC00.PNG, que é transparente (alfa 0). */
    /* Dígitos 5-9: a célula passa do fim da textura (213..258 de 256; com o +1px
     * acima, 214..259). Com CLAMP (o S3D original também usa, D3DTADDRESS_CLAMP)
     * as linhas além de 256 repetem a linha 255, que ainda tem pixels do dígito
     * — era a "sombra" esticada embaixo dos 5-9. Corta o quad na mesma
     * proporção do V que sobra: o topo e a escala ficam iguais, só a parte fora
     * da textura deixa de ser desenhada. */
    if (vEnd > 1.0f) {
        float keep = (1.0f - vTop) / (vEnd - vTop);
        yUp += h * (1.0f - keep);   /* sobe a base; o topo (yUp + h original) fica */
        h *= keep;
        vEnd = 1.0f;
    }
    Texture_Bind(texId);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(r, g, b, alpha);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, vEnd); glVertex2f(x, yUp);
    glTexCoord2f(u1, vEnd); glVertex2f(x + w, yUp);
    glTexCoord2f(u1, vTop); glVertex2f(x + w, yUp + h);
    glTexCoord2f(u0, vTop); glVertex2f(x, yUp + h);
    glEnd();
}

// Combo number — igual FUN_0040c840. (centerX, centerY) em screen Y-DOWN
void Font_DrawDecNumber(int texId, float centerX, float centerY, int value, float alpha)
{
    if (texId < 0) return;
    if (value < 0) value = 0;
    if (value > 999) value = 999;
    int d1 = value / 100;
    int d2 = (value / 10) % 10;
    int d3 = value % 10;
    glPushMatrix();
    glTranslatef(centerX, 480.0f - centerY, 0.0f);  // screen → OpenGL Y-UP
    glTranslatef(24.0f, 0.0f, 0.0f);
    glScalef(1.1f, 1.1f, 1.0f);
    Font_DrawDecDigit(texId, 0.0f, 0.0f, d1, alpha, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    glTranslatef(-10.0f, 0.0f, 0.0f);
    Font_DrawDecDigit(texId, 0.0f, 0.0f, d2, alpha, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    glTranslatef(-10.0f, 0.0f, 0.0f);
    Font_DrawDecDigit(texId, 0.0f, 0.0f, d3, alpha, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    glPopMatrix();
}
