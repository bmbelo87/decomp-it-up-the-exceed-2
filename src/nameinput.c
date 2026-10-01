/* Exceed: CNameInput (ctor 0x413F08, proc "NAMEINPUT").
 * Entrada de nome do ranking depois do estágio extra. Port do nameinput.cpp
 * do src do Exceed (snapshot 2003-12-29), conferido contra o exe:
 *   Begin 0x413F40: ClearForNewStage (0x401328) e hsGetGrade(TOTAL_SCORE
 *         [player+4]) (0x41213C) para cada jogador ativo. Se nenhum entra
 *         no ranking vai para "IR" (0x413FD8) — o src ia para "GAMEOVER".
 *         BFontInit (0x41E58C) + BGA\085.DAT (0x414012); tempo 0x1E = 30 s.
 *   Start: 56 quadros (0x4142A9), contador descendo em (590, 504 -> 414 -> 424).
 *   Run:   slots (35,4) (35,22) (180,24|25) (180,26) (35,15) (210+n,12|10)
 *          (230,20) (230,18); repetição 24/8 quadros, aceleração 2/20.
 *   Fim:   _hsAddScore(TOTAL_SCORE, nome) e, com 1 jogador, "IR"; com 2, GAMEOVER.
 * O ranking fica no eeprom.bin (imagem de 2048 bytes lida/gravada por
 * 0x426060/0x426000): valores u32 em 0x74B, nomes 4 bytes em 0x79B, 20 linhas.
 * O Adler-32 da imagem cobre só os 8 bytes de configuração (setupmgr.cpp
 * _adler32(&game_mode, 8)), então o ranking pode ser regravado direto. */
#include "pumpy.h"
#include "bga.h"

#define NI_BGA         0
#define NI_TIME        30          /* NAMEINPUT_TIME / 0x1E */
#define NI_START_LEN   56          /* 0x38 */
#define HS_NUM         20
#define HS_OFF_VALUE   0x74B
#define HS_OFF_NAME    0x79B
#define EEP_SIZE       0x800

#define BLOCK_MAX 59
#define CNV_END   0
#define CNV_BS    1

enum { MOVE_NONE, MOVE_LEFT, MOVE_RIGHT };

extern uint32_t g_exIrTotal[2];

/* bfont.cpp cnvTable */
static const char k_cnv[BLOCK_MAX] = {
    'A','B','C','D','E','F','G','H','I','J','K','L','M','N','O','P',
    'Q','R','S','T','U','V','W','X','Y','Z','0','1','2','3','4','5',
    '6','7','8','9','~','!','@','#','$','%','^','&','*','(',')','_',
    '-','+','=','\\',':',';','/','?',' ', CNV_BS, CNV_END,
};

static uint8_t g_eep[EEP_SIZE];
static int   g_bfontTex = -1, g_ifontTex[4] = { -1, -1, -1, -1 }, g_timerTex = -1;
static bool  g_flag[2];
static int   g_curPlayer;
static int   g_phase;              /* 0 = Start, 1 = Run */
static int   g_cnt, g_moveCnt, g_accel, g_moveDir;
static int   g_curChar, g_cursor;
static float g_elapsed;
static int   g_time;
static char  g_name[2][5];

static int cnvValueToChar(int v) { return (v < 0 || v >= BLOCK_MAX) ? 0 : k_cnv[v]; }
static int cnvCharToValue(int c)
{
    int i;
    for (i = 0; i < BLOCK_MAX; i++) if (k_cnv[i] == c) break;
    return i;
}
static int clampChar(int c)
{
    if (c >= BLOCK_MAX) c -= BLOCK_MAX;
    if (c < 0) c += BLOCK_MAX;
    return c;
}

/* ── ranking no eeprom.bin ─────────────────────────────────────────────── */
/* static void eepPath(char* out, size_t n) { snprintf(out, n, "%s/PIUEXCEED2.INI", g_game.currentDirectory); } */   /* DESATIVADO: eeprom_x2.c */

/* Exceed2: a imagem é a do eeprom_x2.c (PIUEXCEED2.INI), compartilhada com a
 * Select (contagem de jogos, Canon-D) para uma gravação não apagar a outra.
 * As versões anteriores liam/gravavam o arquivo direto (eeprom.bin). */
static bool eepLoad(void)
{
    memcpy(g_eep, Eeprom2_Data(), sizeof(g_eep));
    return true;
}

static void eepSave(void)
{
    memcpy(Eeprom2_Data(), g_eep, sizeof(g_eep));
    Eeprom2_Save();
}

static uint32_t hsValue(int i)
{
    const uint8_t* p = g_eep + HS_OFF_VALUE + 4 * i;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void hsSetValue(int i, uint32_t v)
{
    uint8_t* p = g_eep + HS_OFF_VALUE + 4 * i;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* 0x41213C hsGetGrade: primeira linha com valor < score (unsigned), senão -1 */
static int hsGetGrade(uint32_t score)
{
    for (int i = 0; i < HS_NUM; i++) if (score > hsValue(i)) return i;
    return -1;
}

/* highscore.cpp _hsAddScore: '\0' vira ' ', desloca as linhas e insere */
static void hsAddScore(uint32_t score, char* name)
{
    for (int i = 0; i < 4; i++) if (name[i] == '\0') name[i] = ' ';
    int line = hsGetGrade(score);
    if (line < 0) return;
    for (int i = HS_NUM - 1; i >= line; i--) {
        if (i + 1 < HS_NUM) {
            hsSetValue(i + 1, hsValue(i));
            memcpy(g_eep + HS_OFF_NAME + 4 * (i + 1), g_eep + HS_OFF_NAME + 4 * i, 4);
        }
    }
    hsSetValue(line, score);
    memcpy(g_eep + HS_OFF_NAME + 4 * line, name, 4);
    eepSave();
    Log_Print("NAMEINPUT: %c%c%c%c %u -> posição %d\n", name[0], name[1], name[2], name[3],
              (unsigned)score, line + 1);
}

/* ── fontes (bfont.cpp) ────────────────────────────────────────────────── */
static void bindTex(int t)
{
    if (t < 0 || !g_game.textures[t].inUse) return;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_game.textures[t].id);
}

/* _BFontPutCharCenterPivot: bfont.tga 8x8, célula 128/1024 */
static void bfontCenter(float x, float y, int ch, float w, float h)
{
    int v = cnvCharToValue(ch);
    if (v == BLOCK_MAX || g_bfontTex < 0) return;
    bindTex(g_bfontTex);
    float u0 = (float)(v % 8) / 8.0f, v0 = (float)(v / 8) / 8.0f;
    float u1 = u0 + 0.125f, v1 = v0 + 0.125f;
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2f(x - w / 2, y + h / 2);
    glTexCoord2f(u0, v1); glVertex2f(x - w / 2, y - h / 2);
    glTexCoord2f(u1, v1); glVertex2f(x + w / 2, y - h / 2);
    glTexCoord2f(u1, v0); glVertex2f(x + w / 2, y + h / 2);
    glEnd();
}

/* _IFontPutChar / IFontPrintf2: font1..4.tga (16 caracteres cada, 4x4 de 64/256) */
static void ifontPrint(float x, float y, float w, float h, float step, const char* s)
{
    for (; *s; s++, x += step) {
        int v = cnvCharToValue((unsigned char)*s);
        if (v == BLOCK_MAX) continue;
        int t = g_ifontTex[(v / 16) > 3 ? 0 : v / 16];
        if (t < 0) continue;
        bindTex(t);
        float u0 = (float)((v % 4) * 64) / 256.0f, v0 = (float)(((v % 16) / 4) * 64) / 256.0f;
        float u1 = u0 + 0.25f, v1 = v0 + 0.25f;
        glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x,     y + h);
        glTexCoord2f(u0, v1); glVertex2f(x,     y);
        glTexCoord2f(u1, v1); glVertex2f(x + w, y);
        glTexCoord2f(u1, v0); glVertex2f(x + w, y + h);
        glEnd();
    }
}

/* _time_nums: 2 dígitos do font4.tga (grade de 7, célula 35x32), unidade primeiro */
static void timeNums(float x, float y, int value)
{
    if (g_timerTex < 0) return;
    if (value < 0) value = 0;
    bindTex(g_timerTex);
    for (int i = 0; i < 2; i++, value /= 10, x -= 35.0f) {
        int d = value % 10;
        float u0 = (float)(d % 7) * 0.13671875f, u1 = u0 + 0.13671875f;
        float v0 = (float)(d / 7) * 0.125f + 0.75f, v1 = v0 + 0.125f;
        glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2f(x,         y + 32.0f);
        glTexCoord2f(u0, v1); glVertex2f(x,         y);
        glTexCoord2f(u1, v1); glVertex2f(x + 35.0f, y);
        glTexCoord2f(u1, v0); glVertex2f(x + 35.0f, y + 32.0f);
        glEnd();
    }
}

/* S3DSetProjection(43.603) / S3DSetOrtho — mesmo espaço do exceed_select.c */
static void setProjection(void)
{
    const float n = 10.0f, f = 4000.0f, k = n / 600.0f;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-320.0f * k, 320.0f * k, -240.0f * k, 240.0f * k, n, f);
    glTranslatef(-320.0f, -240.0f, -600.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}
static void setOrtho(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 640, 0, 480, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* CNameInput::DrawCharBar: 7 letras numa roda (23° entre elas) */
static void drawCharBar(int center)
{
    const float depth = -55.586f - 20.0f, y = 130.452f + 240.0f, radius = 312.449f - 10.0f;
    setProjection();
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);            /* S3D_CULL_CCW: descarta as faces anti-horárias viradas para trás */
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    for (int i = -3; i < 4; i++) {
        glPushMatrix();
        glTranslatef(320.0f, y, depth);
        glRotatef(15.2763f, 1.0f, 0.0f, 0.0f);
        int ofs = 0;
        if (g_moveDir == MOVE_NONE)       glRotatef(23.0f * i, 0.0f, 1.0f, 0.0f);
        else if (g_moveDir == MOVE_LEFT)  { glRotatef(23.0f * i + g_moveCnt * 3, 0.0f, 1.0f, 0.0f); ofs = 1; }
        else                              { glRotatef(23.0f * i - g_moveCnt * 3, 0.0f, 1.0f, 0.0f); ofs = -1; }
        glTranslatef(0.0f, 0.0f, radius);
        glRotatef(-34.596f, 1.0f, 0.0f, 0.0f);
        bfontCenter(0.0f, 0.0f, cnvValueToChar(clampChar(center + i + ofs)), 80.0f, 116.0f);
        glPopMatrix();
    }
    glDisable(GL_CULL_FACE);
    setOrtho();
}

/* ── ciclo ─────────────────────────────────────────────────────────────── */
static void resetForPlayer(int p)
{
    g_curPlayer = p;
    g_cursor = 0;
    g_curChar = 0;
    g_moveDir = MOVE_NONE;
    g_moveCnt = 0;
    g_accel = 0;
    g_cnt = 0;
    g_phase = 0;
    g_elapsed = 0.0f;
    g_time = NI_TIME;
}

static void NameInput_Enter(void)
{
    char path[MAX_PATH];
    eepLoad();

    g_flag[0] = (g_game.activePlayerMask & 1) && hsGetGrade(g_exIrTotal[0]) != -1;
    g_flag[1] = (g_game.activePlayerMask & 2) && hsGetGrade(g_exIrTotal[1]) != -1;
    if (!g_flag[0] && !g_flag[1]) {                      /* 0x413FD8 */
        Resource_ClearBGA();
        Game_ChangeState(STATE_IR);
        return;
    }

    snprintf(path, sizeof(path), "%s/BGA/BFONT.DAT", g_game.currentDirectory);
    g_bfontTex = g_timerTex = -1;
    for (int i = 0; i < 4; i++) g_ifontTex[i] = -1;
    if (RES_Open(path)) {
        static const char* k_if[4] = { "font1.tga", "font2.tga", "font3.tga", "font4.tga" };
        g_bfontTex = loadTextureFromRES("bfont.tga");
        for (int i = 0; i < 4; i++) g_ifontTex[i] = loadTextureFromRES(k_if[i]);
        g_timerTex = g_ifontTex[3];
        RES_Close();
    }
    if (g_bfontTex < 0) Log_Print("NAMEINPUT: BFONT.DAT não carregou\n");

    memset(g_name, ' ', sizeof(g_name));
    g_name[0][4] = g_name[1][4] = '\0';
    resetForPlayer(g_flag[0] ? 0 : 1);
    BGA_SetColor(NI_BGA, 1.0f, 1.0f);
}

static void finishPlayer(void)
{
    char* nm = g_name[g_curPlayer];
    if (g_waveSoundIds[SND_START] >= 0) Audio_Play(g_waveSoundIds[SND_START], false); /* EFF_HIDDEN_SELECTED (HIPÓTESE: START.WAV) */
    if (nm[0] == ' ' && nm[1] == ' ' && nm[2] == ' ' && nm[3] == ' ')
        memcpy(nm, "PUMP", 4);
    hsAddScore(g_exIrTotal[g_curPlayer], nm);

    bool two = (g_game.activePlayerMask & 3) == 3;
    if (g_curPlayer == 0 && g_flag[1]) {
        g_flag[0] = false;
        resetForPlayer(1);
        return;
    }
    Resource_ClearBGA();
    Game_ChangeState(two ? STATE_GAMEOVER_ENTER : STATE_IR);
}

static void playMove(void)
{
    int s = g_accel > 2 ? SND_10_2 : SND_PUSHPANEL;      /* EFF_NAME_REPEAT / EFF_MENU_PUSH_FOOT_PANEL (HIPÓTESE) */
    if (g_waveSoundIds[s] >= 0) Audio_Play(g_waveSoundIds[s], false);
}

void NameInput_Update(float dt)
{
    if (g_game.stateFrame == 1) {
        NameInput_Enter();
        if (g_game.state != STATE_NAMEINPUT) return;
    }
    int p = g_curPlayer;

    if (g_phase == 0) {
        if (++g_cnt >= NI_START_LEN) { g_phase = 1; g_cnt = 0; g_elapsed = 0.0f; }
        return;
    }

    g_elapsed += dt;
    g_time = NI_TIME - (int)g_elapsed;
    g_cnt++;
    g_moveCnt += (g_accel > 20) ? 4 : (g_accel > 2 ? 2 : 1);

    bool hitL = Input_IsPadHit(p, PAD_DL), downL = Input_IsPadDown(p, PAD_DL);
    bool hitR = Input_IsPadHit(p, PAD_DR), downR = Input_IsPadDown(p, PAD_DR);

    if (hitL || (downL && g_moveCnt > 24) || (downL && g_accel > 2 && g_moveCnt > 8)) {
        playMove();
        if (g_cursor >= 4) {
            if (g_curChar == cnvCharToValue(CNV_END)) {
                g_curChar = cnvCharToValue(CNV_BS);
                g_moveDir = MOVE_LEFT; g_moveCnt = 0;
            }
        } else {
            g_accel++;
            g_curChar = clampChar(g_curChar - 1);
            g_moveDir = MOVE_LEFT; g_moveCnt = 0;
        }
    }
    if (hitR || (downR && g_moveCnt > 24) || (downR && g_accel > 2 && g_moveCnt > 8)) {
        playMove();
        if (g_cursor >= 4) {
            if (g_curChar == cnvCharToValue(CNV_BS)) {
                g_curChar = cnvCharToValue(CNV_END);
                g_moveDir = MOVE_RIGHT; g_moveCnt = 0;
            }
        } else {
            g_accel++;
            g_curChar = clampChar(g_curChar + 1);
            g_moveDir = MOVE_RIGHT; g_moveCnt = 0;
        }
    }
    if (Input_IsPadHit(p, PAD_C)) {
        if (g_waveSoundIds[SND_PUSHPANEL] >= 0) Audio_Play(g_waveSoundIds[SND_PUSHPANEL], false);
        switch (cnvValueToChar(g_curChar)) {
        case CNV_BS:
            if (g_cursor > 0) { g_name[p][g_cursor] = ' '; g_cursor--; }
            break;
        case CNV_END:
            g_time = 0;
            break;
        default:
            g_cursor++;
            if (g_cursor == 4) g_curChar = cnvCharToValue(CNV_END);
            if (g_cursor > 4) g_time = 0;
            break;
        }
    }

    if (g_time <= 0) {
        /* Nome já tem o caractere atual gravado no quadro anterior. Como no
         * original, um END pendente vira ' ' em _hsAddScore e um BS (0x01)
         * fica no nome. */
        finishPlayer();
        return;
    }

    if (g_moveCnt >= 8) {
        g_moveDir = MOVE_NONE;
        if (!downL && !hitL && !downR && !hitR) g_accel = 0;
    }
    /* m_pszNameCurrent[m_CursorPos] = ...: com cursor 4 o original escreve
     * no byte seguinte ao nome (m_szName1p[4] é o [0] do 2P); aqui o buffer
     * tem 5 bytes e o [4] nunca entra no ranking. */
    g_name[p][g_cursor] = (char)cnvValueToChar(g_curChar);
}

void NameInput_Render(void)
{
    char buf[16];
    int p = g_curPlayer;
    if (g_game.bgaPicCount <= 0) return;

    if (g_phase == 0) {
        BGA_Render(NI_BGA, g_cnt);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        float ty = (g_cnt <= 10) ? (424.0f + 80.0f) - 9.0f * g_cnt
                 : (g_cnt <= 17) ? 414.0f + 1.42f * (g_cnt - 10) : 424.0f;
        glColor4f(1, 1, 1, 1);
        timeNums(590.0f, ty, NI_TIME);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        BGA_DrawSlot(NI_BGA, 180, p == 0 ? 24 : 25);
        BGA_DrawSlot(NI_BGA, 180, 26);
        drawCharBar(g_curChar);
        glColor4f(1, 1, 1, 1);
        return;
    }

    BGA_DrawSlot(NI_BGA, g_cnt % 56, 0);
    BGA_DrawSlot(NI_BGA, 35, 4);                         /* barra de baixo */
    BGA_DrawSlot(NI_BGA, 35, 22);                        /* barra de cima */
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glColor4f(1, 1, 1, 1);
    timeNums(590.0f, 424.0f, g_time);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    BGA_DrawSlot(NI_BGA, 180, p == 0 ? 24 : 25);
    BGA_DrawSlot(NI_BGA, 180, 26);

    if (g_moveDir == MOVE_NONE)      BGA_DrawSlot(NI_BGA, 35, 15);
    else if (g_moveDir == MOVE_LEFT) BGA_DrawSlot(NI_BGA, 210 + g_moveCnt, 12);
    else                             BGA_DrawSlot(NI_BGA, 210 + g_moveCnt, 10);

    drawCharBar(g_curChar);

    BGA_DrawSlot(NI_BGA, 230 + (g_moveDir == MOVE_LEFT ? g_moveCnt : 0), 20);
    BGA_DrawSlot(NI_BGA, 230 + (g_moveDir == MOVE_RIGHT ? g_moveCnt : 0), 18);

    glColor4f(1.0f, 1.0f, 1.0f, 0.7f);
    snprintf(buf, sizeof(buf), "%02d", hsGetGrade(g_exIrTotal[p]) + 1);
    ifontPrint(80.0f, 40.0f, 50.0f, 50.0f, 28.0f, buf);
    snprintf(buf, sizeof(buf), "%c%c%c%c", g_name[p][0], g_name[p][1], g_name[p][2], g_name[p][3]);
    ifontPrint(165.0f, 40.0f, 50.0f, 50.0f, 38.0f, buf);
    snprintf(buf, sizeof(buf), "%07u", (unsigned)g_exIrTotal[p]);
    ifontPrint(340.0f, 40.0f, 50.0f, 50.0f, 28.0f, buf);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}
