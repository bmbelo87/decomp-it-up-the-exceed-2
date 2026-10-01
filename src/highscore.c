/*
 * highscore.c — CHighscore do Exceed (vtable 0x1282B88), parte da atração.
 *
 * Begin 0x412648: com crédito vai para TITLE; senão carrega BGA\HS.DAT,
 *   a fonte do BGA\BFONT.DAT (0x41E58C) e aponta para o ranking da EEPROM
 *   ([0xA69038] = eeprom.bin, 2048 bytes):
 *     +0x74B + 4k  pontuação da posição k (u32)
 *     +0x79B + 4k  nome da posição k (4 caracteres)
 *   20 posições.
 * Entrada 0x412894 (t = [+0x10] de 0 a 120): HS.DAT inteiro no quadro t e as
 *   posições 15..20 entrando com fade.
 * Update 0x412B0C: a lista rola até a 1ª posição; fade a partir de t=1200 e
 *   IDLE com t > 1260. Com crédito, TITLE.
 * Coordenadas do original em Y-UP (espaço do GL do projeto): entram direto.
 */
#include "pumpy.h"
#include "bga.h"

#define HS_BGA 0

static uint8_t g_eeprom[2048];
static int g_bfontTex = -1;     /* [0x568F90]: bfont.tga do BFONT.DAT */
static int g_t;                 /* [+0x10] */
static int g_phase;             /* 0 = entrada (0x412894), 1 = Update (0x412B0C) */
static float g_color;           /* [+0x6C] */

static uint32_t hsScore(int k) {
    uint32_t v;
    memcpy(&v, g_eeprom + 0x74B + 4 * k, 4);
    return v;
}
static const uint8_t* hsName(int k) { return g_eeprom + 0x79B + 4 * k; }

/* 0x41E63C: índice na tabela 0x456FAC; 0x3B = não desenha */
static int glyphIndex(int ch) {
    static const char k_set[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789~!@#$%^&*()_-+=\\:;/? ";
    for (int i = 0; k_set[i]; i++)
        if ((unsigned char)k_set[i] == (unsigned)ch) return i;
    return 0x3B;
}

/* 0x41E6EC(x, y, ch, w, h): bfont.tga em grade 8x8, y na base */
static void drawGlyph(float x, float y, int ch, float w, float h) {
    int idx = glyphIndex(ch);
    if (idx == 0x3B) return;
    float u0 = (float)(idx % 8) * 0.125f, u1 = u0 + 0.125f;
    float v0 = (float)(idx / 8) * 0.125f, v1 = v0 + 0.125f;
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x,     y + h);
    glTexCoord2f(u0, v1); glVertex2f(x,     y);
    glTexCoord2f(u1, v1); glVertex2f(x + w, y);
    glTexCoord2f(u1, v0); glVertex2f(x + w, y + h);
    glEnd();
}

/* 0x41E9E0(x, y, w, h, passo, fmt, ...) */
static void drawText(float x, float y, const char* s) {
    if (g_bfontTex < 0 || !g_game.textures[g_bfontTex].inUse) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_game.textures[g_bfontTex].id);
    for (; *s; s++, x += 35.0f)
        drawGlyph(x, y, (unsigned char)*s, 38.0f, 48.0f);
}

/* Uma linha: posição (%02d) em x=50, nome em 142, pontuação (%08d) em 306 */
static void drawRow(int k, float y) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d", k + 1);
    drawText(50.0f, y, buf);
    const uint8_t* n = hsName(k);
    snprintf(buf, sizeof(buf), "%c%c%c%c", n[0], n[1], n[2], n[3]);
    drawText(142.0f, y, buf);
    snprintf(buf, sizeof(buf), "%08u", (unsigned)hsScore(k));
    drawText(306.0f, y, buf);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);   /* 0x412FE4 */
}

void Highscore_Enter(void) {
    char path[MAX_PATH];
    /* Exceed2: ranking da imagem do eeprom_x2.c (PIUEXCEED2.INI, +0x74B/+0x79B) */
    memcpy(g_eeprom, Eeprom2_Data(), sizeof(g_eeprom));

    snprintf(path, sizeof(path), "%s/BGA/BFONT.DAT", g_game.currentDirectory);
    g_bfontTex = -1;
    if (RES_Open(path)) {
        g_bfontTex = loadTextureFromRES("bfont.tga");
        RES_Close();
    }
    if (g_bfontTex < 0) Log_Print("HIGHSCORE: bfont.tga do BFONT.DAT não carregou\n");

    g_t = 0;
    g_phase = 0;
    g_color = 1.0f;
    BGA_SetColor(HS_BGA, 1.0f, 1.0f);
}

void Highscore_Update(float dt) {
    (void)dt;
    if (g_game.stateFrame == 1) Highscore_Enter();

    if (Coin_HasCredit()) {                     /* 0x412B1A / 0x4128C1 */
        Game_ChangeState(STATE_CREDIT);
        return;
    }
    if (g_phase == 0) {
        if (g_t >= 0x78) {                      /* 0x412AB5 */
            g_phase = 1;
            g_t = 0;
        }
        g_t++;
        return;
    }
    if (g_t > 0x4EC) {                          /* 0x413037 */
        Attract_Idle();
        return;
    }
    g_t++;
}

void Highscore_Render(void) {
    if (g_game.bgaPicCount <= 0) return;
    int t = g_t;

    if (g_phase == 0) {
        BGA_SetEventFrame(HS_BGA, t);           /* 0x4128F9 */
        for (int i = 0; i < 6; i++) {
            int s = i * 7 + 0x18;
            float a;
            if (t < s)          a = 0.0f;
            else if (t < s + 4) a = (float)(t - s) / 4.0f;
            else                a = 1.0f;
            glColor4f(1.0f, 1.0f, 1.0f, a);
            drawRow(14 + i, (float)i * -60.0f + 305.0f);
        }
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    BGA_DrawSlot(HS_BGA, t % 90 + 0x78, 0);     /* 0x412B42 */

    float yBase;
    int first;
    bool stopped = false;
    if (t <= 0x78) {
        yBase = 1145.0f;
        first = 14;
    } else {
        yBase = (float)(t - 0x78) * -1.3333334f + 1145.0f;
        first = 14 - (t - 0x5C) / 45;
        if (first <= 0 && yBase <= 305.0f) {    /* 0x41306C */
            yBase = 305.0f;
            first = 0;
            stopped = true;
        }
        if (first < 0) first = 0;
    }

    static const int k_rowSlots[7] = { 8, 0x0D, 0x11, 0x15, 0x19, 0x1D, 0x21 };
    if (t > 0x78 && !stopped) {
        int f = (t - 0x78) % 45 + 300;
        BGA_DrawSlot(HS_BGA, f, 8);
        BGA_DrawSlot(HS_BGA, f, 9);
        for (int i = 1; i < 7; i++) BGA_DrawSlot(HS_BGA, f, k_rowSlots[i]);
    } else {
        for (int i = 0; i < 7; i++) BGA_DrawSlot(HS_BGA, 300, k_rowSlots[i]);
    }
    BGA_DrawSlot(HS_BGA, t % 40 + 0x78, 0x25);
    BGA_DrawSlot(HS_BGA, t % 40 + 0x78, 0x2B);
    BGA_DrawSlot(HS_BGA, t % 61 + 0x1E, 0x2C);

    glColor4f(g_color, g_color, g_color, 1.0f);
    for (int k = first; k < 20; k++)
        drawRow(k, (float)k * -60.0f + yBase);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    if (t > 0x4B0) {                            /* 0x412FFB: fade de saída */
        g_color = (float)(t - 0x4B0) / -60.0f + 1.0f;
        BGA_SetColor(HS_BGA, g_color, 1.0f);
    }
}
