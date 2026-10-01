/*
 * station.c — CStation do Exceed2 (PIU32.EXE, vtable 0x452334, proc "STATION").
 *
 * O CTitle entra aqui assim que alguém entra (0x41acfe -> 0x41af1f). Fundo: o
 * CREDIT.MOV do título continua tocando ([+0x50] -> 0x420c80); o TITLE.AUD também.
 * BGA: BGA\STATION.DAT (carregado pelo CTitle::Begin em 0x41a961), formato BGA3
 * com cenas nomeadas (src/resource.c parseBGA3, src/bga.c BGA_Scene*).
 *
 *   0x419bd0 Begin   salva as texturas dos slots 3/4 (BATTLE), 6/7 (ARCADE),
 *                    10/11 (REMIX), 25/26; seleção = 1 (ARCADE); contador = 20;
 *                    toca stationselect.wav; fase 1.
 *   0x419d10 fase 1  cenas top_down, in, arr_left, arr_right, arro_l, arro_r, TIME;
 *                    contador; entrada. Fim de "in" -> zera o cronômetro, fase 2.
 *   0x419e20 fase 2  top_down; hold / pan_left / pan_right ([+0xC]); select (com
 *                    [+0x10]); setas; TIME; contador de 20 s; entrada.
 *   0x41a080 fase 3  fade preto c*0.025 (0x452348), top_up, out; c >= 40 -> proc.
 *   0x419fe0 End     consome crédito por jogador, para BGM e vídeo, [0x484fb4].
 *   0x41a250 entrada DL (bit 0x1) / DR (0x2) / C (0x4) de quem entrou (P2 << 8).
 *   0x41a160 entrada tardia (CENTER de quem não entrou).
 *   0x41a500 qual textura (BATTLE/ARCADE/REMIX) vai em cada slot.
 *
 * Seleção [+8]: 0 = BATTLE STATION, 1 = ARCADE STATION, 2 = REMIX STATION.
 * Saída original: 1 -> "SELECT" 1, 2 -> "SELECT 3", 0 -> "HELP 1".
 * Só o ARCADE está implementado: REMIX e BATTLE mostram "Ainda nao funcional"
 * e, por enquanto, também seguem para a Select do ARCADE.
 */
#include "pumpy.h"
#include "bga.h"
#include "movie.h"

#define ST_BGA 0

enum { ST_BATTLE = 0, ST_ARCADE = 1, ST_REMIX = 2 };

static int  g_phase;          /* [0x484ff1]: 1 in, 2 principal, 3 saída */
static int  g_sel;            /* [+0x08] */
static int  g_anim;           /* [+0x0C]: 0 hold, 1 pan_left, 2 pan_right */
static bool g_confirmed;      /* [+0x10] */
static int  g_timer;          /* [+0x14] */
static int  g_prevTimer;      /* [+0x18] */
static DWORD g_startMs;       /* [+0x1C] */
static int  g_fade;           /* [+0x5C] */
static int  g_fadeDraw;       /* valor de [+0x5C] usado no quadro (antes do ++) */
static unsigned g_joined;     /* [0x484f7c] bits 0/1 */
static int  g_timerTex = -1;  /* [0x46b594]: font4 do BGA\BFONT.DAT */

static BGALayerSrc g_item[3][2];   /* [+0x48]/[+0x4C], [+0x30]/[+0x34], [+0x38]/[+0x3C] */

static int  g_sndSelect = -1, g_sndArcade = -1, g_sndBattle = -1, g_sndRemix = -1, g_sndSelected = -1;

/* desenho adiado: 0x41f0f0 desenha no quadro atual e avança; o Update guarda
 * o quadro de cada cena tocada e o Render desenha na mesma ordem */
#define ST_MAX_DRAW 16
static int  g_drawFrames[ST_MAX_DRAW];
static int  g_drawItems[ST_MAX_DRAW][4];   /* texturas dos slots 3/6/10/13 no momento do desenho */
static int  g_drawCount;
static int  g_slotItem[4];                 /* item atual dos slots 3, 6, 10, 13 */

/* No original a cena é desenhada na hora (0x41f0f0) e só depois a entrada /
 * 0x41a500 trocam as texturas; aqui o desenho fica para o Render, então o
 * layout vigente é guardado junto (sem isso o painel pisca com a textura
 * nova um quadro antes, no fim de cada pan e a cada toque). */
static void playScene(const char* name) {
    int f = BGA_SceneFrame(ST_BGA, name);
    if (f >= 0 && g_drawCount < ST_MAX_DRAW) {
        g_drawFrames[g_drawCount] = f;
        memcpy(g_drawItems[g_drawCount], g_slotItem, sizeof(g_slotItem));
        g_drawCount++;
    }
    BGA_ScenePlay(ST_BGA, name, false);
}

static void playWave(int id) {
    if (id >= 0) Audio_Play(id, false);
}

/* 0x41a500: slots 3/4, 6/7, 10/11, 13/14 recebem as texturas dos itens.
 * Tabela tirada literalmente dos 9 ramos (anim x seleção). */
static const int k_layout[3][3][4] = {
    /* anim 0 (0x41a6e6) */ { { 2, 0, 1, 0 }, { 0, 1, 2, 1 }, { 1, 2, 0, 0 } },
    /* anim 1 (0x41a5af) */ { { 1, 2, 0, 1 }, { 2, 0, 1, 2 }, { 0, 1, 2, 0 } },
    /* anim 2 (0x41a52a) */ { { 2, 0, 1, 2 }, { 0, 1, 2, 0 }, { 1, 2, 0, 1 } },
};
static const int k_slots[4] = { 3, 6, 10, 13 };

static void applyItems(const int items[4]) {
    for (int i = 0; i < 4; i++) {
        BGA_SetLayerSrc(ST_BGA, k_slots[i], &g_item[items[i]][0]);
        BGA_SetLayerSrc(ST_BGA, k_slots[i] + 1, &g_item[items[i]][1]);
    }
}

static void setLayout(int anim) {
    if (anim < 0 || anim > 2) return;
    for (int i = 0; i < 4; i++) g_slotItem[i] = k_layout[anim][g_sel][i];
}

static int loadWave(const char* name) {
    int id = Audio_LoadWaveFile(name);
    if (id < 0) Log_Print("STATION: falha ao carregar WAVE/%s\n", name);
    return id;
}

/* 0x419bd0 */
static void Station_Enter(void) {
    if (!Resource_LoadBGAByName("STATION"))
        Log_Print("STATION: falha ao carregar BGA\\STATION.DAT\n");
    BGA_Reset();

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/BGA/BFONT.DAT", g_game.currentDirectory);
    g_timerTex = -1;
    if (RES_Open(path)) {
        g_timerTex = loadTextureFromRES("font4.tga");
        RES_Close();
    }
    if (g_timerTex < 0) Log_Print("STATION: font4 do BFONT.DAT não carregou\n");

    /* 0x4231c0.. (carregados uma vez no início do jogo no original) */
    if (g_sndSelect < 0)   g_sndSelect   = loadWave("stationselect.wav");
    if (g_sndArcade < 0)   g_sndArcade   = loadWave("arcadestation.wav");
    if (g_sndBattle < 0)   g_sndBattle   = loadWave("battlestation.wav");
    if (g_sndRemix < 0)    g_sndRemix    = loadWave("remixstation.wav");
    if (g_sndSelected < 0) g_sndSelected = loadWave("selected.wav");

    BGA_GetLayerSrc(ST_BGA, 3,  &g_item[ST_BATTLE][0]);
    BGA_GetLayerSrc(ST_BGA, 4,  &g_item[ST_BATTLE][1]);
    BGA_GetLayerSrc(ST_BGA, 6,  &g_item[ST_ARCADE][0]);
    BGA_GetLayerSrc(ST_BGA, 7,  &g_item[ST_ARCADE][1]);
    BGA_GetLayerSrc(ST_BGA, 10, &g_item[ST_REMIX][0]);
    BGA_GetLayerSrc(ST_BGA, 11, &g_item[ST_REMIX][1]);

    g_sel = ST_ARCADE;
    g_anim = 0;
    g_confirmed = false;
    g_timer = 20;
    g_prevTimer = 20;
    g_fade = 0;
    g_joined = Title_GetJoinedMask() & 3;
    if (g_joined == 0) g_joined = 1;
    setLayout(0);
    g_startMs = timeGetTime();
    playWave(g_sndSelect);
    g_phase = 1;
}

static bool hit(int bit) {
    /* bits de hardware na ordem DL, DR, C, UL, UR (ver exceed_select.c) */
    static const PadButton map[3] = { PAD_DL, PAD_DR, PAD_C };
    return ((g_joined & 1) && Input_IsPadHit(0, map[bit])) ||
           ((g_joined & 2) && Input_IsPadHit(1, map[bit]));
}

/* 0x41a160: entrada tardia. HIPÓTESE: o original testa 0x41fa30 >= 2
 * (contagem de créditos); aqui vale "tem crédito", como no título. */
static void lateJoin(void) {
    if (!Coin_HasCredit()) return;
    for (int p = 0; p < 2; p++) {
        if (!(g_joined & (1u << p)) && Input_IsPadHit(p, PAD_C)) {
            Coin_ConsumeCredit();
            g_startMs = timeGetTime();
            g_joined |= 1u << p;
            Title_SetJoinedMask(g_joined);
            playWave(g_waveSoundIds[SND_PUSHPANEL]);
            setLayout(0);
            return;
        }
    }
}

/* 0x41a250 */
static void processInput(void) {
    if (hit(0)) {                                   /* DL */
        g_confirmed = false;
        g_anim = 2;
        if (--g_sel < 0) g_sel = 2;
        BGA_SceneReset(ST_BGA, "arr_left");
        BGA_SceneReset(ST_BGA, "arro_l");
        BGA_SceneReset(ST_BGA, "pan_right");
        setLayout(g_anim);
        playWave(g_waveSoundIds[SND_3_2]);
    }
    if (hit(1)) {                                   /* DR */
        g_confirmed = false;
        g_anim = 1;
        if (++g_sel > 2) g_sel = 0;
        BGA_SceneReset(ST_BGA, "arr_right");
        BGA_SceneReset(ST_BGA, "arro_r");
        BGA_SceneReset(ST_BGA, "pan_left");
        setLayout(g_anim);
        playWave(g_waveSoundIds[SND_3_2]);
    }
    if (hit(2)) {                                   /* C */
        playWave(g_waveSoundIds[SND_3_2]);
        if (!g_confirmed) {
            g_confirmed = true;
            if (g_sel == ST_REMIX)       playWave(g_sndRemix);
            else if (g_sel == ST_ARCADE) playWave(g_sndArcade);
            else                         playWave(g_sndBattle);
        } else {
            g_timer = 0;
        }
    }
    if (g_timer <= 0) {
        playWave(g_sndSelected);
        g_fade = 0;
        g_fadeDraw = 0;
        setLayout(0);
        g_phase = 3;
    }
}

/* 0x419f09..0x419f6c: 20 s pelo relógio em ms; com a troca e <= 5, TIME_LIMIT */
static void updateTimer(void) {
    g_prevTimer = g_timer;
    g_timer = (int)((int)(g_startMs - timeGetTime()) + 20000) / 1000;
    if (g_timer != g_prevTimer && g_timer <= 5)
        playWave(g_waveSoundIds[SND_TIME_LIMIT]);
}

void Station_Update(float dt) {
    if (g_game.stateFrame == 1) Station_Enter();
    Movie_Update(dt);
    g_drawCount = 0;

    if (g_phase == 1) {                             /* 0x419d10 */
        playScene("top_down");
        playScene("in");
        playScene("arr_left");
        playScene("arr_right");
        playScene("arro_l");
        playScene("arro_r");
        playScene("TIME");
        processInput();
        lateJoin();
        if (g_phase == 1 && BGA_SceneDone(ST_BGA, "in")) {
            g_startMs = timeGetTime();
            g_phase = 2;
        }
    } else if (g_phase == 2) {                      /* 0x419e20 */
        static const char* k_anim[3] = { "hold", "pan_left", "pan_right" };
        playScene("top_down");
        const char* an = k_anim[g_anim];
        playScene(an);
        if (g_anim != 0 && BGA_SceneDone(ST_BGA, an)) {
            g_anim = 0;
            setLayout(0);
            BGA_SceneReset(ST_BGA, "select");
        }
        if (g_anim == 0 && g_confirmed) playScene("select");
        playScene("arr_left");
        playScene("arr_right");
        playScene("arro_l");
        playScene("arro_r");
        playScene("TIME");
        updateTimer();
        processInput();
        lateJoin();
    } else if (g_phase == 3) {                      /* 0x41a080 */
        g_fadeDraw = g_fade;
        playScene("top_up");
        playScene("out");
        if (g_fade >= 40) {
            /* 0x419fe0 (End): [0x484FB4] = 0 ARCADE / 1 REMIX / 2 BATTLE.
             * 0x41a105: ARCADE -> "SELECT" (canal 0), REMIX -> "SELECT 3",
             * BATTLE -> "HELP 1" -> "SELECT 4" (0x411A63). O tutorial HELP
             * ainda não existe: o BATTLE vai direto para a Select. */
            if (g_sel == ST_REMIX)       ExSelect_SetStation(1, 3);
            else if (g_sel == ST_BATTLE) ExSelect_SetStation(2, 4);
            else                         ExSelect_SetStation(0, 0);
            Movie_Close();
            BGM_Stop();
            Title_SetJoinedMask(g_joined);
            Menu_ResetState();
            Game_ChangeState(STATE_EXSELECT);
            return;
        }
        g_fade++;
    }
}

/* 0x41dbc0 / 0x41dc10: 2 dígitos do font4, grade de 7 colunas, célula 35x32,
 * unidade primeiro recuando 35 px. Coordenadas do S3D (Y para cima). */
static void drawTimerNumber(int x, int y, int value) {
    if (g_timerTex < 0) return;
    Texture_Bind(g_timerTex);
    glEnable(GL_TEXTURE_2D);
    for (int i = 0; i < 2; i++) {
        int d = value % 10;
        float u0 = (float)(d % 7) * 0.13671875f;
        float u1 = u0 + 0.13671875f;
        float v0 = (float)(d / 7) * 0.125f + 0.75f;
        float v1 = v0 + 0.125f;
        glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex2i(x, y + 32);
        glTexCoord2f(u0, v1); glVertex2i(x, y);
        glTexCoord2f(u1, v1); glVertex2i(x + 35, y);
        glTexCoord2f(u1, v0); glVertex2i(x + 35, y + 32);
        glEnd();
        value /= 10;
        x -= 35;
    }
    glDisable(GL_TEXTURE_2D);
}

/* 0x419d8d..0x419de2: cor 1 -> (597,446); blend 6,1; cor 0 -> (595,448); blend 6,7 */
static void drawTimer(void) {
    int v = g_timer < 0 ? 0 : g_timer;
    glEnable(GL_BLEND);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    drawTimerNumber(0x255, 0x1BE, v);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
    drawTimerNumber(0x253, 0x1C0, v);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
}

/* Não existe no original: aviso sobre os painéis REMIX/BATTLE */
static void drawNotImplemented(void) {
    for (int d = 0; d < g_drawCount; d++) {
        for (int i = 0; i < 4; i++) {
            if (g_drawItems[d][i] == ST_ARCADE) continue;
            float x, y;
            if (!BGA_GetSlotPos(ST_BGA, g_drawFrames[d], k_slots[i], &x, &y)) continue;
            Font_DrawStringCenteredScaled((int)x + 1, (int)y - 7, "Ainda nao funcional", 0, 0, 0, 1, 1.5f);
            Font_DrawStringCenteredScaled((int)x, (int)y - 8, "Ainda nao funcional", 1, 0.2f, 0.2f, 1, 1.5f);
        }
    }
}

void Station_Render(void) {
    Movie_Render();
    if (g_phase == 3 && g_fadeDraw != 0) {              /* 0x41a0a2: antes das cenas */
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0, 0, 0, (float)g_fadeDraw * 0.025f);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f(640, 0); glVertex2f(640, 480); glVertex2f(0, 480);
        glEnd();
        glColor4f(1, 1, 1, 1);
    }
    for (int i = 0; i < g_drawCount; i++) {
        applyItems(g_drawItems[i]);
        BGA_DrawFrame(ST_BGA, g_drawFrames[i]);
    }
    if (g_phase == 1 || g_phase == 2) drawTimer();
    /* if (g_phase != 3) drawNotImplemented(); */   /* DESATIVADO: REMIX e BATTLE já levam à Select */
}
