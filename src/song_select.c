#include "pumpy.h"
#include "testbga.h"
#include <string.h>

bool g_cdLoaded = false;   /* mantido fora do #if 0: resource.c:1398 */

#if 0  /* DESATIVADO (27/09/2026): código só do Prex3, fora do fluxo do Exceed
 * (LOGO/INTRO/IDLE/TITLE -> CSelect). Preservado; o bloco tem comentários
 * internos, por isso #if 0 em vez de envolver em comentário. */

/* ── Command detection ──────────────────────────────────────────────────────
 * Buffer5 (5-botões): velocidade e Vanish/NonStep
 *   Speed:   UL UR UL UR CN → x1→x2→x3→x4→RV→x1
 *            (RV = Random Velocity, o mesmo bit 0x200 do Buffer9 — PUMPY.EXE 0x40747e)
 *   Vanish:  UL UR DL DR CN → Vanish→NonStep→Vanish+NonStep→OFF (PUMPY.EXE 0x407464)
 *
 * Buffer9 (9-botões): cheats longos
 *   Random Velocity: UL UR UL UR UL UR UL UR CN
 *   Mirror:          DR DL UR UL DR DL UR UL CN
 *   Random Step:     UL UR UL UR DL DR DL DR CN
 *   Freedom:         UL DL UR DR DR UL UR DL CN  (só som, sem ícone)
 *   Earthworm:       DR DL UR UL DR UR DL UL CN  (só som, sem ícone)
 *
 * Buffer6 (sequencial): DL DR × 3 = Reset todos os cheats do player
 */
#define CMD_BUF_LEN  5
#define CMD_BUF9_LEN 9

static PadButton g_cmdBuf[2][CMD_BUF_LEN];
static int       g_cmdBufCount[2]  = {0, 0};

static PadButton g_cmdBuf9[2][CMD_BUF9_LEN];
static int       g_cmdBuf9Count[2] = {0, 0};

static int       g_cmdBuf6Count[2] = {0, 0}; /* pares DL+DR detectados por player */
static int       g_cmdBuf6State[2] = {0, 0}; /* 0=idle, 1=DL recebido aguardando DR */

/* Auto-scroll DL/DR (hold): replicado de SongSelect_UpdateRender @ 00409720
 *   g_holdAnimTimer : frames desde o último scroll (reseta em cada scroll automático)
 *   g_holdCarouselPos: acumula enquanto segura; > 0xb4 (180) → timer soma 2/frame (rápido)
 *   g_holdDir        : direção atual: +1=DR(próxima), -1=DL(anterior), 0=nenhuma
 * Thresholds do Ghidra:
 *   0x14 (20 frames @ 60 fps ≈ 333 ms) = tempo entre repetições
 *   0x3c (60)  = carouselPos inicial ao primeiro press
 *   0xb4 (180) = carouselPos que ativa o modo rápido (≈ 2 s de hold)
 */
static int g_holdAnimTimer   = 0;
static int g_holdCarouselPos = 0;
static int g_holdDir         = 0;

/* Buffer5 sequências */
static const PadButton k_speedSeq[CMD_BUF_LEN] = {
    PAD_UL, PAD_UR, PAD_UL, PAD_UR, PAD_C
};
static const PadButton k_vanishSeq[CMD_BUF_LEN] = {
    PAD_UL, PAD_UR, PAD_DL, PAD_DR, PAD_C
};

/* Buffer9 sequências [0=RV, 1=Mirror, 2=RandomStep, 3=Freedom, 4=Earthworm, 5=TestBGA] */
static const PadButton k_seq9[6][CMD_BUF9_LEN] = {
    { PAD_UL, PAD_UR, PAD_UL, PAD_UR, PAD_UL, PAD_UR, PAD_UL, PAD_UR, PAD_C }, /* Random Velocity */
    { PAD_DR, PAD_DL, PAD_UR, PAD_UL, PAD_DR, PAD_DL, PAD_UR, PAD_UL, PAD_C }, /* Mirror          */
    { PAD_UL, PAD_UR, PAD_UL, PAD_UR, PAD_DL, PAD_DR, PAD_DL, PAD_DR, PAD_C }, /* Random Step     */
    { PAD_UL, PAD_DL, PAD_UR, PAD_DR, PAD_DR, PAD_UL, PAD_UR, PAD_DL, PAD_C }, /* Freedom         */
    { PAD_DR, PAD_DL, PAD_UR, PAD_UL, PAD_DR, PAD_UR, PAD_DL, PAD_UL, PAD_C }, /* Earthworm       */
    { PAD_DL, PAD_DL, PAD_DL, PAD_DL, PAD_DR, PAD_DR, PAD_DR, PAD_DR, PAD_C }, /* TestBGA (extra do port) */
};

/* Reseta todos os cheats de um player (Buffer6 trigger) */
static void Cmd_ResetAllCheats(int player) {
    g_game.cmdSpeedMult[player]      = 1;
    g_game.cmdMirror[player]         = false;
    g_game.cmdRandomStep[player]     = false;
    g_game.cmdRandomVelocity[player] = false;
    g_game.cmdEarthworm[player]      = false;
    g_game.cmdFreedom[player]        = false;
    g_game.cmdVanish[player]         = false;
    g_game.cmdNonStep[player]        = false;
    g_game.cmdTestBGA[player]        = false;
    g_cmdBufCount[player]            = 0;
    g_cmdBuf9Count[player]           = 0;
    g_cmdBuf6Count[player]           = 0;
    g_cmdBuf6State[player]           = 0;
    Log_Print("CMD P%d: RESET todos os cheats\n", player + 1);
    Audio_Play(g_waveSoundIds[SND_2_1], false);
}

/* Empurra um botão nos buffers do player e verifica todas as sequências.
 * Retorna true se algum cheat foi detectado (para tocar o som de confirmação).
 *
 * ORDEM DE PRIORIDADE: Buffer9 checa ANTES do Buffer5.
 * Motivo: a sequência RV do Buffer9 (UL UR UL UR UL UR UL UR CN) termina com
 * (UL UR UL UR CN) que é exatamente k_speedSeq — se Buffer5 checasse primeiro,
 * ele dispararia no 9º botão e resetaria buf9Count=0, impedindo Buffer9 de
 * reconhecer a sequência completa. */
static bool Cmd_Push(int player, PadButton btn) {
    bool cheatFired = false;

    /* ── Alimentar ambos os buffers (sliding window independente) ──────── */
    if (g_cmdBufCount[player] < CMD_BUF_LEN) {
        g_cmdBuf[player][g_cmdBufCount[player]++] = btn;
    } else {
        memmove(g_cmdBuf[player], g_cmdBuf[player] + 1, (CMD_BUF_LEN - 1) * sizeof(PadButton));
        g_cmdBuf[player][CMD_BUF_LEN - 1] = btn;
    }
    if (g_cmdBuf9Count[player] < CMD_BUF9_LEN) {
        g_cmdBuf9[player][g_cmdBuf9Count[player]++] = btn;
    } else {
        memmove(g_cmdBuf9[player], g_cmdBuf9[player] + 1, (CMD_BUF9_LEN - 1) * sizeof(PadButton));
        g_cmdBuf9[player][CMD_BUF9_LEN - 1] = btn;
    }

    /* ── Buffer9: cheats de 9 botões — PRIORIDADE ALTA ────────────────── */
    if (g_cmdBuf9Count[player] >= CMD_BUF9_LEN) {
        for (int seq = 0; seq < 6; seq++) {
            if (memcmp(g_cmdBuf9[player], k_seq9[seq], CMD_BUF9_LEN * sizeof(PadButton)) == 0) {
                g_cmdBuf9Count[player] = 0;
                g_cmdBufCount[player]  = 0;
                switch (seq) {
                case 0: /* Random Velocity — PUMPY.EXE 0x4073dc: limpa x2/x3/x4, alterna 0x200 */
                    g_game.cmdSpeedMult[player]      = 1;
                    g_game.cmdRandomVelocity[player] = !g_game.cmdRandomVelocity[player];
                    Log_Print("CMD P%d: RandomVelocity %s\n", player+1,
                              g_game.cmdRandomVelocity[player] ? "ON" : "OFF");
                    break;
                case 1: /* Mirror */
                    g_game.cmdMirror[player] = !g_game.cmdMirror[player];
                    Log_Print("CMD P%d: Mirror %s\n", player+1,
                              g_game.cmdMirror[player] ? "ON" : "OFF");
                    break;
                case 2: /* Random Step */
                    g_game.cmdRandomStep[player] = !g_game.cmdRandomStep[player];
                    Log_Print("CMD P%d: RandomStep %s\n", player+1,
                              g_game.cmdRandomStep[player] ? "ON" : "OFF");
                    break;
                case 3: /* Freedom — oculta receptor */
                    g_game.cmdFreedom[player] = !g_game.cmdFreedom[player];
                    Log_Print("CMD P%d: Freedom %s\n", player+1,
                              g_game.cmdFreedom[player] ? "ON" : "OFF");
                    break;
                case 4: /* Earthworm — PUMPY.EXE 0x407405: limpa x2/x3/x4 e RV, alterna 0x800 */
                    g_game.cmdEarthworm[player]      = !g_game.cmdEarthworm[player];
                    g_game.cmdSpeedMult[player]      = 1;
                    g_game.cmdRandomVelocity[player] = false;
                    Log_Print("CMD P%d: Earthworm %s\n", player+1,
                              g_game.cmdEarthworm[player] ? "ON" : "OFF");
                    break;
                case 5: /* TestBGA — extra do port (X1Rus testbga.cpp) */
                    g_game.cmdTestBGA[player] = true;
                    InitS();
                    Log_Print("CMD P%d: TestBGA ON\n", player+1);
                    break;
                }
                cheatFired = true;
                break;
            }
        }
    }

    /* ── Buffer5: velocidade + Vanish/NonStep (só se Buffer9 não disparou) */
    if (!cheatFired && g_cmdBufCount[player] >= CMD_BUF_LEN) {
        if (memcmp(g_cmdBuf[player], k_speedSeq, CMD_BUF_LEN * sizeof(PadButton)) == 0) {
            g_cmdBufCount[player]  = 0;
            g_cmdBuf9Count[player] = 0;
            /* PUMPY.EXE 0x40747e: o próximo estado sai dos bits atuais
             * (RV→x1, x4→RV, x3→x4, x2→x3, x1→x2); Earthworm não é tocado. */
            int*  mult = &g_game.cmdSpeedMult[player];
            bool* rv   = &g_game.cmdRandomVelocity[player];
            if (*rv)             { *rv = false; *mult = 1; }
            else if (*mult >= 4) { *rv = true;  *mult = 1; }
            else                 { *mult = (*mult < 1 ? 1 : *mult) + 1; }
            if (*rv) Log_Print("CMD P%d: Speed -> RV\n", player + 1);
            else     Log_Print("CMD P%d: Speed -> x%d\n", player + 1, *mult);
            cheatFired = true;
        } else if (memcmp(g_cmdBuf[player], k_vanishSeq, CMD_BUF_LEN * sizeof(PadButton)) == 0) {
            g_cmdBufCount[player]  = 0;
            g_cmdBuf9Count[player] = 0;
            /* PUMPY.EXE 0x407464: com Vanish (0x10) ligado, desliga-o e alterna
             * NonStep (0x80); sem Vanish, liga Vanish.
             * Ciclo: OFF → Vanish → NonStep → Vanish+NonStep → OFF */
            bool* v  = &g_game.cmdVanish[player];
            bool* ns = &g_game.cmdNonStep[player];
            if (*v) { *ns = !*ns; *v = false; }
            else    { *v = true; }
            Log_Print("CMD P%d: Vanish=%d NonStep=%d\n", player + 1, *v, *ns);
            cheatFired = true;
        }
    }

    return cheatFired;
}

static int prevSongId = -1;
static int previewState = 0;
static int selectedState = 0;
static int g_cdTexIds[45];
bool g_cdLoaded = false;
static float g_songAnimPos = 0.0f;
static int g_songAnimCounter = 0;

static int g_carrosselFrame = 588;
static int g_carrosselDir = 0;
static int g_carrosselTarget = 588;
static bool g_carrosselIntro = true;
static int g_introFrame = 0;
static int g_pendingMove = 0;
static float g_previewDelay = 0.0f;
/* Atraso do preview (PUMPY.EXE 0x40a6d5): toca quando [0xd5fd34] > 0x28 E
 * [0xd5e394] > 0x50, dois contadores de +1 por frame (0x40ab4c/0x40ab5f).
 *   - Entrada / troca de modo (0x40a4be, 0x40a69b): os dois zerados -> 81 frames.
 *   - Navegação (0x40a87e): [0xd5fd34]=0 e [0xd5e394]>=0x3c -> 41 frames.
 * Os contadores correm mesmo durante as animações; só o disparo é bloqueado. */
#define PREVIEW_DELAY_ENTER (81.0f / 60.0f)
#define PREVIEW_DELAY_NAV   (41.0f / 60.0f)

static const int g_slotFrameOffset[7] = { -48, -32, -16, 0, +16, +32, +48 };

// Modos 1 jogador: NORMAL(0), HARD(1), CRAZY(2), HALFDOUBLE(3), DOUBLE(4), NIGHTMARE(5)
// Modos 2 jogadores (P1+P2 juntos): NORMAL(0), HARD(1), CRAZY(2), BATTLE(3)
// BATTLE usa steps HARD e só aparece quando activePlayerMask == 0x3 (ambos ativos)
/* static const char* g_modeNames1P[6] = {"NORMAL","HARD","CRAZY","HALFDOUBLE","DOUBLE","NIGHTMARE"};
static const int   g_modeLayers1P[6] = {31, 32, 30, 27, 28, 8}; */
/* DIVISION: extra deste port. No PUMPY.EXE o modo existe (Stage.cfg, run -dv,
 * SongSelect_Enter 0x40ad2e) mas nenhuma tela o seleciona; o rótulo
 * "DIVISION / WILD MODE" existe no MODE.PNG sem SPR. Layer -2 = clone criado
 * em tempo de execução por ensureDivisionLayer(). */
#define MODE_COUNT_1P 7
static const char* g_modeNames1P[MODE_COUNT_1P] = {"NORMAL","HARD","CRAZY","HALFDOUBLE","DOUBLE","NIGHTMARE","DIVISION"};
static const int   g_modeLayers1P[MODE_COUNT_1P] = {31, 32, 30, 27, 28, 8, -2};
static const char* g_modeNames2P[4] = {"NORMAL","HARD","CRAZY","BATTLE"};
static const int   g_modeLayers2P[4] = {31, 32, 30, -1}; /* -1=BATTLE: layer buscado por nome */

/* Array dinâmico de layer indices — preenchido por rebuildModeList().
 * Para BATTLE, o layer do BATTLE.SPR é descoberto pelo nome no BGAPicture. */
static int g_modeLayersDyn[MODE_COUNT_1P];

static int g_modeCount = MODE_COUNT_1P;    // número de modos ativos (6 em 1P/P2-solo, 4 em P1+P2)
static int g_modeDBIdx[MODE_COUNT_1P];     // indices correspondentes no SongDB
static int g_modeTileIdx[MODE_COUNT_1P];   // indices dos primeiros tiles SPR de cada modo
static int g_modeSongIndex[MODE_COUNT_1P]; // última posição de música lembrada por modo
static bool g_isBattleMode = false; // true quando slot selecionado é BATTLE (P1+P2)

static int g_selDispIdx = 0;          // indice de exibicao atual (0-5)
static bool g_modeAnimActive = false;
static int g_modeAnimFrame = 0;
static int g_modeAnimDir = 0;         // +1=UR, -1=UL
/* Modo e índice de música a usar na RENDERIZAÇÃO dos CDs.
 * Atualizado imediatamente ao pressionar UR/UL, para os CDs
 * já mostrarem o novo modo desde o início da animação dos boxes. */
static int g_displayModeDBIdx = 0;
static int g_displaySongIndex = 0;
#define MODE_ANIM_DURATION 15
#define MODE_LEFT_X   107.0f
#define MODE_CENTER_X 315.0f
#define MODE_RIGHT_X  537.0f
#define MODE_Y         90.0f

typedef struct {
    int frame;
    float x, sx, alpha;
} BoxKeyframe;

static const BoxKeyframe g_boxKF[7] = {
    {540, -235.0f, 0.50f, 0.0f},
    {556, -175.0f, 0.65f, 0.6f},
    {572, -110.0f, 0.80f, 1.0f},
    {588,    0.0f, 1.00f, 1.0f},
    {604,  110.0f, 0.80f, 1.0f},
    {620,  175.0f, 0.65f, 0.6f},
    {636,  235.0f, 0.50f, 0.0f},
};

static void EvalBoxFrame(float frame, float* outX, float* outSc, float* outAlpha) {
    if (frame <= 540.0f) { *outX = -235.0f; *outSc = 0.50f; *outAlpha = 0.0f; return; }
    if (frame >= 636.0f) { *outX =  235.0f; *outSc = 0.50f; *outAlpha = 0.0f; return; }
    for (int i = 0; i < 6; i++) {
        if (frame >= g_boxKF[i].frame && frame <= g_boxKF[i+1].frame) {
            float t = (frame - g_boxKF[i].frame) / (float)(g_boxKF[i+1].frame - g_boxKF[i].frame);
            *outX     = g_boxKF[i].x     + t * (g_boxKF[i+1].x     - g_boxKF[i].x);
            *outSc    = g_boxKF[i].sx    + t * (g_boxKF[i+1].sx    - g_boxKF[i].sx);
            *outAlpha = g_boxKF[i].alpha + t * (g_boxKF[i+1].alpha  - g_boxKF[i].alpha);
            return;
        }
    }
}

static int g_cdSongs[45][2] = {
    {108,109},{112,205},{212,301},{312,318},{320,321},
    {401,402},{403,404},{405,413},{503,505},{508,516},
    {701,703},{704,705},{707,711},{712,714},{719,720},
    {721,722},{730,733},{734,735},{736,801},{802,803},
    {804,805},{806,807},{808,809},{810,811},{812,813},
    {814,815},{816,817},{818,819},{820,911},{916,921},
    {101,104},{202,203},{204,212},{302,303},{305,306},
    {310,311},{312,414},{501,504},{507,517},{902,906},
    {913,915},{922,102},{821,822},{823,824},{825,826}
};

static int findCdForSong(int songId, int* outHalf) {
    for (int i = 0; i < 45; i++) {
        if (g_cdSongs[i][0] == songId) { *outHalf = 0; return i; }
        if (g_cdSongs[i][1] == songId) { *outHalf = 1; return i; }
    }
    return -1;
}

static void cacheModeTileIndices(void) {
    if (g_game.bgaPicCount <= 0) return;
    BGAPicture* pic = &g_game.bgaPics[0];
    for (int m = 0; m < g_modeCount; m++) {
        int layer = g_modeLayersDyn[m];
        if (layer >= 0 && layer < pic->layerCount) {
            g_modeTileIdx[m] = pic->layers[layer].sprTileStart;
        } else {
            g_modeTileIdx[m] = -1;
        }
    }
}

/* Encontra o índice de layer de um SPR pelo nome (busca substring, case-insensitive não
 * disponível em C puro — usa strstr; nomes no BGA são uppercase). Retorna -1 se não achar. */
static int findLayerByName(const char* keyword) {
    if (g_game.bgaPicCount <= 0) return -1;
    BGAPicture* pic = &g_game.bgaPics[0];
    for (int li = 0; li < pic->layerCount; li++) {
        if (strstr(pic->layers[li].filename, keyword))
            return li;
    }
    return -1;
}

/* Clona o layer do NIGHTMARE (mesma animação/posição no carrossel) com um tile
 * novo apontando para o rótulo DIVISION do MODE.PNG. NIGHTMAR.SPR:
 *   T mode.tga 43 16 125 55 0 182 125 237  -> DIVISION: u 132..256, mesmo v.
 * Devolve o índice do layer, ou -1. Reaproveita o clone se já existir. */
static int ensureDivisionLayer(void) {
    if (g_game.bgaPicCount <= 0) return -1;
    BGAPicture* pic = &g_game.bgaPics[0];
    for (int li = 0; li < pic->layerCount; li++)
        if (strcmp(pic->layers[li].filename, "division.spr") == 0) return li;
    int src = 8; /* layer do NIGHTMARE (g_modeLayers1P) */
    if (src >= pic->layerCount || pic->layerCount >= MAX_BGA_LAYERS) return -1;
    if (g_game.sprTileCount >= MAX_SPR_TILES) return -1;
    int nt = pic->layers[src].sprTileStart;
    if (nt < 0 || nt >= g_game.sprTileCount) return -1;
    int ti = g_game.sprTileCount++;
    g_game.sprTiles[ti] = g_game.sprTiles[nt];
    strncpy(g_game.sprTiles[ti].name, "division", sizeof(g_game.sprTiles[ti].name) - 1);
    float du = g_game.sprTiles[nt].u2 - g_game.sprTiles[nt].u1;   /* 125/256 */
    float px = (du != 0.0f) ? du / 125.0f : (1.0f / 256.0f);
    g_game.sprTiles[ti].u1 = g_game.sprTiles[nt].u1 + 132.0f * px;
    g_game.sprTiles[ti].u2 = g_game.sprTiles[ti].u1 + 124.0f * px;
    g_game.sprTiles[ti].srcW = 124;
    int li = pic->layerCount++;
    pic->layers[li] = pic->layers[src];
    strncpy(pic->layers[li].filename, "division.spr", sizeof(pic->layers[li].filename) - 1);
    pic->layers[li].sprTileStart = ti;
    pic->layers[li].sprTileCount = 1;
    Log_Print("SongSelect: layer DIVISION clonado (layer %d, tile %d)\n", li, ti);
    return li;
}

/* Reconstrói a lista de modos disponíveis de acordo com activePlayerMask.
 * BATTLE aparece apenas quando P1+P2 estão ambos ativos (mask == 0x3).
 * P2 sozinho (0x2) ou P1 sozinho (0x1) = 6 modos normais. */
static void rebuildModeList(void) {
    SongDB* db = &g_game.songDB;
    if (g_game.activePlayerMask == 0x3) {
        /* P1 + P2 juntos: 4 modos. BATTLE usa steps HARD */
        g_modeCount = 4;
        for (int m = 0; m < 4; m++) {
            const char* dbName = (m == 3) ? "HARD" : g_modeNames2P[m];
            g_modeDBIdx[m] = Song_FindMode(db, dbName);
            if (g_modeDBIdx[m] < 0) g_modeDBIdx[m] = 0;
            /* Layer dinâmico: BATTLE busca BATTLE.SPR pelo nome */
            if (m == 3) {
                int bl = findLayerByName("battle"); /* bga.c converte filenames para minúsculo */
                g_modeLayersDyn[m] = (bl >= 0) ? bl : g_modeLayers2P[1]; /* fallback=HARD */
            } else {
                g_modeLayersDyn[m] = g_modeLayers2P[m];
            }
        }
        g_isBattleMode = false; /* atualizado quando slot 3 é selecionado */
    } else {
        /* 1P solo (P1 ou P2 separado): 6 modos com HD/Double/Nightmare */
        g_modeCount = MODE_COUNT_1P;
        for (int m = 0; m < MODE_COUNT_1P; m++) {
            g_modeDBIdx[m] = Song_FindMode(db, g_modeNames1P[m]);
            if (g_modeDBIdx[m] < 0) g_modeDBIdx[m] = 0;
            g_modeLayersDyn[m] = (g_modeLayers1P[m] == -2) ? ensureDivisionLayer() : g_modeLayers1P[m];
        }
        /* Sem DIVISION no Stage.cfg ou sem o clone: volta aos 6 modos. */
        if (Song_FindMode(db, "DIVISION") < 0 || g_modeLayersDyn[MODE_COUNT_1P - 1] < 0)
            g_modeCount = MODE_COUNT_1P - 1;
        g_isBattleMode = false;
    }
    /* Se o selDispIdx atual está fora do novo g_modeCount, volta para 0 */
    if (g_selDispIdx >= g_modeCount) {
        g_selDispIdx = 0;
        g_game.selectedModeIndex = g_modeDBIdx[0];
        g_displayModeDBIdx = g_modeDBIdx[0];
    } else {
        g_game.selectedModeIndex = g_modeDBIdx[g_selDispIdx];
        g_displayModeDBIdx = g_modeDBIdx[g_selDispIdx];
    }
    cacheModeTileIndices();
}

/* ─── Setas de canto: "Next MODE" (UL/UR) e "Next MUSIC" (DL/DR) ────────────
 *
 * AR-LT/RT/LD/RD  → arrow01.tga, TYPE ani, NUM 6 — a seta animada (81x81)
 * ARR-LT/RT/LD/RD → etc.tga,     TYPE tile, NUM 1 — o rótulo de texto
 *
 * Estas não são carregadas por nome no binário (nenhuma string "ar-lt" no
 * PUMPY.EXE): vêm como layers do 099.BGA. Por isso são desenhadas via
 * BGA_SetEventLayer, e os índices são resolvidos por nome em tempo de execução
 * em vez de hardcoded — bga.c guarda os filenames em minúsculo.
 *
 * As posições já estão nos .spr e são absolutas, então não há translate:
 *   AR-LT  (45, 85)   AR-RT  (514, 85)   AR-LD  (45, 361)   AR-RD  (514, 361)
 *   ARR-LT (43, 81)   ARR-RT (514, 81)   ARR-LD (43, 368)   ARR-RD (511, 363)
 */
#define CORNER_ARROW_COUNT 12
#define CORNER_WAVE_COUNT   8   /* só os 8 primeiros reagem ao input */

static int  g_cornerLayer[CORNER_ARROW_COUNT];
static bool g_cornerResolved = false;

/* A ordem importa: os índices 0..3 e 4..7 formam os pares seta+rótulo de cada
 * canto (LT, RT, LD, RD) e são os que fazem a onda. Os M-* (8..11) têm os mesmos
 * 9 keyframes estáticos — entram no fade e ficam parados, sem deslocamento. */
static const char* k_cornerNames[CORNER_ARROW_COUNT] = {
    "ar-lt",  "ar-rt",  "ar-ld",  "ar-rd",    /* setas animadas, 6 frames */
    "arr-lt", "arr-rt", "arr-ld", "arr-rd",   /* rótulos, com onda        */
    "m-lt",   "m-rt",   "m-ld",   "m-rd"      /* rótulos, sem onda        */
};

/* Resolve os índices e registra o alcance de keyframes de cada layer. O dump é
 * único por carga do BGA: sem ele não dá para saber que frame passar para o
 * BGA_SetEventLayer, já que fora do alcance a layer sai com alpha 0. */
static void resolveCornerArrows(void) {
    if (g_cornerResolved) return;
    if (g_game.bgaPicCount <= 0) return;
    g_cornerResolved = true;

    BGAPicture* pic = &g_game.bgaPics[0];
    for (int i = 0; i < CORNER_ARROW_COUNT; i++) {
        g_cornerLayer[i] = findLayerByName(k_cornerNames[i]);
        int li = g_cornerLayer[i];
        if (li < 0) {
            Log_Print("CORNER: '%s' NAO ENCONTRADA\n", k_cornerNames[i]);
            continue;
        }
        BGALayer* L = &pic->layers[li];
        int f0 = (L->kfCount > 0) ? L->keyframes[0].frame : -1;
        int f1 = (L->kfCount > 0) ? L->keyframes[L->kfCount - 1].frame : -1;
        Log_Print("CORNER: '%s' -> layer %d  kf=%d  frames %d..%d  aniFC=%d  tiles=%d\n",
                  k_cornerNames[i], li, L->kfCount, f0, f1,
                  L->aniFrameCount, L->sprTileCount);
    }
}

/* Frames extraídos dos keyframes reais do 099.BGA. As oito layers compartilham
 * exatamente a mesma linha do tempo de 13 keyframes:
 *
 *   8  → 29   intro: alpha 0→1. Nos rótulos (ARR-*) o x,y sai de ±50 e chega
 *             a 0, então eles deslizam de fora da tela para o lugar.
 *   29 … 495  repouso: todos com x=0, y=0, alpha=1 — estado parado.
 *   495 → 512 onda: o pico é o keyframe 504, que desloca ±15 na diagonal do
 *             próprio canto (ar-lt vai a -15,-15; ar-rd a +15,+15), e o 512
 *             devolve a 0,0.
 *
 * O keyframe 420 tem type=0 (invisível) e por isso a passagem de repouso nunca
 * avança sozinha: ficamos parados em 495, que é idêntico ao 29 mas deixa a onda
 * contígua.
 */
#define CORNER_F_INTRO      8    /* alpha 0 — início do fade de entrada       */
#define CORNER_F_REST_A     29   /* alpha 1 — início do trecho de repouso     */
#define CORNER_F_REST_B     360  /* fim do trecho: 29,60,115,180,241,300,360  */
#define CORNER_F_WAVE_A     495  /* início da onda                            */
#define CORNER_F_WAVE_B     512  /* fim da onda, de volta a 0,0               */

enum { CORNER_INTRO, CORNER_REST, CORNER_WAVE };

static int g_cornerFrame[CORNER_ARROW_COUNT];
static int g_cornerState[CORNER_ARROW_COUNT];

/* Dispara a onda de um canto. which: 0=LT 1=RT 2=LD 3=RD.
 * Move a seta (AR-*) e o rótulo (ARR-*) do mesmo canto juntos. Os M-* ficam de
 * fora: seus keyframes não têm o deslocamento de ±15, então não há onda neles. */
static void triggerCornerArrow(int which) {
    if (which < 0 || which > 3) return;
    for (int i = which; i < CORNER_WAVE_COUNT; i += 4) {
        g_cornerFrame[i] = CORNER_F_WAVE_A;
        g_cornerState[i] = CORNER_WAVE;
    }
}

/* Coloca as oito na intro (ao entrar na tela) */
static void resetCornerArrows(void) {
    for (int i = 0; i < CORNER_ARROW_COUNT; i++) {
        g_cornerFrame[i] = CORNER_F_INTRO;
        g_cornerState[i] = CORNER_INTRO;
    }
}

static void renderCornerArrows(void) {
    if (g_game.bgaPicCount <= 0) return;
    BGAPicture* pic = &g_game.bgaPics[0];
    for (int i = 0; i < CORNER_ARROW_COUNT; i++) {
        int li = g_cornerLayer[i];
        if (li < 0 || li >= pic->layerCount) continue;

        g_cornerFrame[i]++;
        switch (g_cornerState[i]) {
        case CORNER_INTRO:
            if (g_cornerFrame[i] >= CORNER_F_REST_A) {
                g_cornerFrame[i] = CORNER_F_REST_A;
                g_cornerState[i] = CORNER_REST;
            }
            break;
        case CORNER_REST:
            /* Laço no trecho parado. A seta não sai do lugar (x,y,alpha são
             * iguais nos sete keyframes), mas o animT varre 0→1 a cada segmento
             * e é ele que avança os 6 frames do AR-*.SPR. Nunca passamos de
             * 360: o keyframe 420 tem type=0 e apagaria a seta até o 495. */
            if (g_cornerFrame[i] >= CORNER_F_REST_B)
                g_cornerFrame[i] = CORNER_F_REST_A;
            break;
        case CORNER_WAVE:
            if (g_cornerFrame[i] >= CORNER_F_WAVE_B) {
                g_cornerFrame[i] = CORNER_F_REST_A;
                g_cornerState[i] = CORNER_REST;
            }
            break;
        default:
            break;
        }
        BGA_SetEventLayer(0, g_cornerFrame[i], li);
    }
}

/* ─── Indicador de dificuldade (DIFFLCUL.SPR / DIFFNIGH.SPR do 099.DAT) ──────
 *
 * Reconstrução de Judge_RenderGradeRow (0x004071b0) — o nome do Ghidra engana,
 * essa função desenha a linha de bolinhas de nível embaixo do disc.
 *
 * Como funciona no original:
 *   - SongSelect_Enter (0x0040ac65/0x0040ac74) carrega os dois SPRs
 *   - SongSelect_RenderCarouselMove (0x00409140) só chama o desenho quando
 *     [0x00d5fd34] > 0x14 — o mesmo timer de estabilização do cursor que
 *     dispara o preview. Aqui o equivalente é previewState.
 *   - Posição: x = 0x140 (320), y = 0xfffffe7f (-385) na matriz do carrossel
 *   - O nível vem de um byte com sinal por música (SongDB_ParseUnlockEntry
 *     grava `(byte)atol(...)` cru do Stage.cfg)
 *
 * O laço do original é `while (i < nivel)` e vale para os dois sprites: o nível
 * do Stage.cfg é sempre a contagem, sem exceção. NIGHTMARE e DIVISION carregam
 * 99, então desenham 99 caveiras espalhadas por ±1470px — a maior parte sai da
 * tela, e é assim mesmo no original. O 99 não é sentinela de "sem nível": ele
 * apenas escolhe o sprite de caveira no lugar da bolinha.
 */
#define DIFF_LEVEL_NIGHT    99    /* nível que troca DIFFLCUL por DIFFNIGH */
#define DIFF_DOT_SPACING    15.0f /* glTranslatef(fVar1 * 15.0, ...) no original */
/* O original translada y = 0xfffffe7f (-385) e SPR_RenderTile emite
 * glVertex2i(x, 480 - y). Combinando: a linha fica em Y-DOWN a partir de 385.
 *   DIFFLCUL (y1=0,  y2=43) → 385..428, centro 406.5
 *   DIFFNIGH (y1=6,  y2=31) → 391..416, centro 403.5 */
#define DIFF_ROW_Y          385.0f
/* Escala da caveira. Atenção: este número NÃO vem do binário — Judge_RenderGradeRow
 * não tem nenhum glScalef, desenha o sprite no tamanho do .spr (19x25). O valor
 * aqui é ajuste visual para bater com o original, então é o primeiro lugar a
 * mexer se o tamanho parecer errado. */
#define DIFF_NIGHT_ZOOM     1.0f

static int g_diffLculIdx = -1;    /* tile do DIFFLCUL.SPR (TYPE tile, NUM 1)  */
static int g_diffNighIdx = -1;    /* tiles do DIFFNIGH.SPR (TYPE ani, NUM 2)  */
static int g_timeSprIdx  = -1;    /* tile do TIME.SPR    (TYPE tile, NUM 1)  */

/* ── Contador TIME ────────────────────────────────────────────────────────────
 * SongSelect_UpdateRender (0x00409720) no original:
 *
 *   g_nCountdownTimer = 0x3c - Frame_GetDelta() / 0xf0;
 *   if (timer != prev && timer < 0xb && buffer)  -> Stop/SetCurrentPosition(0)/Play
 *   Font_DrawNumberSimple(0x159, 0x1bd, g_nCountdownTimer, 2);
 *   if (0 < g_nCountdownTimer) { ...avança frames...; return; }
 *   Frame_ResetDelta(); ...; g_dwState = 0x1e;
 *
 * Ou seja: 60 s, bip a cada segundo enquanto o valor for < 11 (10..0) e, ao
 * chegar a zero, sai da tela pelo mesmo caminho do confirm — o próprio original
 * força g_nCountdownTimer = 0 quando o jogador confirma com o carrossel travado.
 *
 * Os dois números vêm de Font_DrawNumberSimple: x = 0x159 (345), y = 0x1bd (445,
 * já em Y-UP porque Font_RenderDigit emite glVertex2i cru), 2 dígitos, passo de
 * -0x18 (24 px) do menos significativo para a esquerda. */
#define TIME_LIMIT_SECONDS  60
#define TIME_BEEP_BELOW     11    /* 0x0b: bipa quando o valor cai abaixo disso */
#define TIME_DIGIT_X       345    /* 0x159 */
#define TIME_DIGIT_Y       445    /* 0x1bd — Y-UP */
#define TIME_DIGIT_STEP     24    /* 0x18  */
#define TIME_DIGIT_W        36    /* 0x24  */
#define TIME_DIGIT_H        39    /* 0x27  */
#define TIME_DIGIT_COUNT     2

static float g_timeRemain = (float)TIME_LIMIT_SECONDS;  /* segundos, fracionário */
static int   g_timeShown  = TIME_LIMIT_SECONDS;         /* valor inteiro exibido */
static int   g_timePrev   = TIME_LIMIT_SECONDS;         /* valor do frame anterior */

static void resetTimeCounter(void) {
    g_timeRemain = (float)TIME_LIMIT_SECONDS;
    g_timeShown  = TIME_LIMIT_SECONDS;
    g_timePrev   = TIME_LIMIT_SECONDS;
}

/* Mesmo atlas do DanceGrade: Font_RenderDigit (0x0040c8b0) faz col = d & 7,
 * row = d >> 3, u = col*0.125, v = row*0.12109375 + 0.28515625 — os três floats
 * estão em 0x00434780 / 0x0043477c / 0x00434778 e batem com o drawDig do
 * result.c. A diferença é só o Y: lá é Y-DOWN e convertido, aqui o original
 * passa a coordenada GL direto.
 *
 * Nesta textura V=0 é o TOPO da imagem, então vTop (o menor V) é a borda de
 * cima do glifo e tem de ir no vértice de cima do quad (yUp + H), que em
 * glOrtho(0,w,0,h) é o mais alto na tela. O código original amarrava vTop ao
 * vértice de baixo — daí o contador sair de ponta-cabeça. */
static void drawTimeDigit(int x, int yUp, int d) {
    if (g_fontTexId < 0 || d < 0 || d > 9) return;
    int col = d % 8, row = d / 8;
    Texture_Bind(g_fontTexId);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
    float u0 = (float)col * 0.125f;
    float u1 = u0 + 0.125f;
    float vTop = (float)row * 0.12109375f + 0.28515625f;  /* borda de cima do glifo */
    float vBot = vTop + 0.12109375f;                      /* borda de baixo do glifo */
    glBegin(GL_QUADS);
     glTexCoord2f(u0, vBot); glVertex2f((float)x,                  (float)yUp);
     glTexCoord2f(u1, vBot); glVertex2f((float)(x + TIME_DIGIT_W), (float)yUp);
     glTexCoord2f(u1, vTop); glVertex2f((float)(x + TIME_DIGIT_W), (float)(yUp + TIME_DIGIT_H));
     glTexCoord2f(u0, vTop); glVertex2f((float)x,                  (float)(yUp + TIME_DIGIT_H));
    glEnd();
}

/* Font_DrawNumberSimple: desenha do dígito menos significativo para a esquerda,
 * sempre `count` dígitos (com zero à esquerda, como no original). */
static void drawTimeNumber(int x, int yUp, int value, int count) {
    if (value < 0) value = 0;
    for (int i = 0; i < count; i++) {
        drawTimeDigit(x - i * TIME_DIGIT_STEP, yUp, value % 10);
        value /= 10;
    }
}

static void loadDifficultySprites(void) {
    /* Resource_ClearBGA zera g_game.sprTileCount a cada troca de tela. Se o
     * índice cacheado caiu fora do fim, ele está stale e precisa recarregar —
     * mesmo problema que g_fontArrow541 tem, mas resolvido aqui sem precisar
     * mexer no resource.c, já que estes são static deste arquivo. */
    if (g_diffLculIdx >= 0 && g_diffLculIdx < g_game.sprTileCount &&
        g_diffNighIdx >= 0 && g_diffNighIdx < g_game.sprTileCount &&
        g_timeSprIdx  >= 0 && g_timeSprIdx  < g_game.sprTileCount)
        return;
    g_diffLculIdx = -1;
    g_diffNighIdx = -1;
    g_timeSprIdx  = -1;

    char datPath[MAX_PATH];
    snprintf(datPath, sizeof(datPath), "%s/BGA/099.DAT", g_game.currentDirectory);
    if (!RES_Open(datPath)) {
        Log_Print("Diff: falha ao abrir '%s'\n", datPath);
        return;
    }
    int startCount = g_game.sprTileCount;
    g_diffLculIdx = g_game.sprTileCount;
    SPR_LoadSPR("DIFFLCUL.SPR", NULL, NULL, NULL);
    g_diffNighIdx = g_game.sprTileCount;
    SPR_LoadSPR("DIFFNIGH.SPR", NULL, NULL, NULL);
    /* Rótulo "TIME" — layer time.spr do 099.BGA, keyframes 0..420 sempre
     * visíveis, x=0 y=0, blend=1 (aditivo). O tile do .spr já traz a posição:
     * T topline.tga 241 -2 75 37 ... */
    g_timeSprIdx = g_game.sprTileCount;
    SPR_LoadSPR("TIME.SPR", NULL, NULL, NULL);

    /* Nada de trocar v1/v2: o 099.DAT também só tem PNG (TOPLINE.PNG), e o
     * decoder de PNG não inverte linhas, então V=0 é o topo da imagem — a
     * convenção que o Texture_DrawUV espera. A troca que existia aqui era o
     * que punha o rótulo TIME (e o DIFFLCUL/DIFFNIGH, que usam a mesma
     * topline) de ponta-cabeça. Ver o comentário equivalente no resource.c. */
    (void)startCount;
    RES_Close();
    Log_Print("Diff: difflcul=%d diffnigh=%d time=%d (sprTileCount=%d)\n",
              g_diffLculIdx, g_diffNighIdx, g_timeSprIdx, g_game.sprTileCount);
}

static void loadCdTextures(void) {
    if (g_cdLoaded) return;
    char datPath[MAX_PATH];
    snprintf(datPath, sizeof(datPath), "%s/BGA/90.DAT", g_game.currentDirectory);
    if (!RES_Open(datPath)) { g_cdLoaded = true; return; }
    for (int i = 0; i < 45; i++) {
        char name[32];
        snprintf(name, sizeof(name), "CD%02d.png", i + 1);
        g_cdTexIds[i] = loadTextureFromRES(name);
    }
    RES_Close();
    g_cdLoaded = true;
}

void SongSelect_ResetCreditIndices(void) {
    memset(g_modeSongIndex, 0, sizeof(g_modeSongIndex));
}

void SongSelect_Reset(void) {
    prevSongId = -1;
    previewState = 0;
    selectedState = 0;
    g_isBattleMode = false;
    g_game.isBattleMode = false;
    g_game.selectedSongIndex = 0;
    g_game.songSelectHighlighted = 0;
    g_game.previewSongId = -1;
    memset(g_modeSongIndex, 0, sizeof(g_modeSongIndex));

    g_cmdBufCount[0]  = 0; g_cmdBufCount[1]  = 0;
    g_cmdBuf9Count[0] = 0; g_cmdBuf9Count[1] = 0;
    g_cmdBuf6Count[0] = 0; g_cmdBuf6Count[1] = 0;
    g_cmdBuf6State[0] = 0; g_cmdBuf6State[1] = 0;

    /* Inicializa lista de modos de acordo com activePlayerMask */
    g_modeCount = MODE_COUNT_1P;
    g_selDispIdx = 0;
    g_displaySongIndex = 0;
    g_modeAnimActive = false;
    g_modeAnimFrame = 0;
    g_modeAnimDir = 0;
    rebuildModeList();

    g_songAnimCounter = 0;
    g_songAnimPos = 0.0f;
    g_carrosselFrame = 588;
    g_carrosselDir = 0;
    g_carrosselTarget = 588;
    g_carrosselIntro = true;
    g_introFrame = 0;
    g_previewDelay = PREVIEW_DELAY_ENTER; /* era 0.0f */
    resetTimeCounter();
    loadCdTextures();
    loadDifficultySprites();
    g_cornerResolved = false;  /* BGA recarregou: reindexar as layers de canto */
    resetCornerArrows();
    cacheModeTileIndices();

    /* Carrega font/arrows (00.DAT) se ainda não estiverem carregados.
     * Necessário para os ícones de Command (X, R, M, V, NS) na HUD do SongSelect. */
    if (g_fontArrow541 < 0) {
        char datPath[MAX_PATH];
        snprintf(datPath, sizeof(datPath), "%s/BGA/00.DAT", g_game.currentDirectory);
        Resource_LoadFontAndArrows(datPath);
    }
}

void SongSelect_ResetIntro(void) {
    prevSongId = -1;
    previewState = 0;
    selectedState = 0;
    g_songAnimCounter = 0;
    g_songAnimPos = 0.0f;
    g_carrosselFrame = 588;
    g_carrosselDir = 0;
    g_carrosselTarget = 588;
    g_carrosselIntro = true;
    g_introFrame = 0;
    g_previewDelay = PREVIEW_DELAY_ENTER; /* era 0.0f */
    resetTimeCounter();
    loadCdTextures();
    loadDifficultySprites();
    g_cornerResolved = false;  /* BGA recarregou: reindexar as layers de canto */
    resetCornerArrows();
    cacheModeTileIndices();
    /* Resource_ClearBGA (chamado antes pelo Game_ChangeState) reseta g_fontArrow541.
     * Recarregar 00.DAT para que os ícones de Command continuem aparecendo. */
    if (g_fontArrow541 < 0) {
        char datPath[MAX_PATH];
        snprintf(datPath, sizeof(datPath), "%s/BGA/00.DAT", g_game.currentDirectory);
        Resource_LoadFontAndArrows(datPath);
    }
}

static void stopPreview(void) {
    if (previewState) {
        BGM_Stop();
        previewState = 0;
    }
}

static void playPreview(int songId) {
    stopPreview();
    if (songId < 0) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/AUDIO/D%d.AUD", g_game.currentDirectory, songId);

    /* previewState é o gate do estado visual do cursor — brilho do Box2 e linha
     * de nível — e não deve depender do áudio existir. Música fora do range, sem
     * DXXX.AUD, tem que se comportar igual às demais; só a reprodução é que fica
     * condicional ao arquivo. Antes o previewState só subia dentro do if, então
     * essas músicas ficavam sem brilho e sem nível. */
    if (BGM_LoadAUDDirect(path)) {
        BGM_Play(false);
    } else {
        Log_Print("SongSelect: sem D%d.AUD — preview silencioso\n", songId);
    }
    previewState = 1;
    prevSongId = songId;
}

void Gamestate_UpdateSongSelect(float dt) {
    if (g_game.state == STATE_SONG_SELECT_B) {
        float a = g_game.globalColorA - dt;
        if (a <= 0.0f) {
            Render_SetGlobalColor(0, 0, 0, 0);
            if (g_game.stateFrame > 60) {
                g_game.state = STATE_SONG_SELECT;
                g_game.stateFrame = 0;
                previewState = 0;
                prevSongId = -1;
                selectedState = 0;
            }
        } else {
            Render_SetGlobalColor(0, 0, 0, a);
        }
        return;
    }

    SongDB* db = &g_game.songDB;

    /* ── Jogadores entrando (CN de quem não está ativo ainda) ──────────── */
    if ((g_game.activePlayerMask & 0x2) == 0 && Input_IsPadHit(1, PAD_C)) {
        g_game.activePlayerMask |= 0x2;
        Log_Print("SONGSEL: P2 entrou (mask=0x%x)\n", g_game.activePlayerMask);
        Audio_Play(g_waveSoundIds[SND_3_2], false);
        rebuildModeList();
        selectedState = 0;
        stopPreview();
    }
    if ((g_game.activePlayerMask & 0x1) == 0 && Input_IsPadHit(0, PAD_C)) {
        g_game.activePlayerMask |= 0x1;
        Log_Print("SONGSEL: P1 entrou (mask=0x%x)\n", g_game.activePlayerMask);
        Audio_Play(g_waveSoundIds[SND_3_2], false);
        rebuildModeList();
        selectedState = 0;
        stopPreview();
    }

    /* ── Captura todos os botões para detecção de Commands (por jogador) ── */
    {
        static const PadButton all5[] = {PAD_UL, PAD_UR, PAD_C, PAD_DL, PAD_DR};
        for (int _p = 0; _p < 2; _p++) {
            if (!(g_game.activePlayerMask & (1 << _p))) continue;

            bool dlHit = Input_IsPadHit(_p, PAD_DL);
            bool drHit = Input_IsPadHit(_p, PAD_DR);

            /* Buffer6: 3 pares DL+DR CONSECUTIVOS sem outros botões no meio = Reset
             * Qualquer UL/UR/CN interrompendo a sequência reseta estado e contador,
             * garantindo que Buffer9/Buffer5 com DL ou DR não disparem o reset. */
            bool ulHit = Input_IsPadHit(_p, PAD_UL);
            bool urHit = Input_IsPadHit(_p, PAD_UR);
            bool  cHit = Input_IsPadHit(_p, PAD_C);
            if (ulHit || urHit || cHit) {
                /* botão fora da sequência DL DR → quebra a cadeia */
                g_cmdBuf6State[_p] = 0;
                g_cmdBuf6Count[_p] = 0;
            } else if (dlHit && drHit) {
                /* simultâneo: par imediato */
                g_cmdBuf6State[_p] = 0;
                g_cmdBuf6Count[_p]++;
                Log_Print("CMD P%d: DL+DR par (simult.) #%d\n", _p+1, g_cmdBuf6Count[_p]);
                if (g_cmdBuf6Count[_p] >= 3) Cmd_ResetAllCheats(_p);
            } else if (dlHit) {
                if (g_cmdBuf6State[_p] == 1) {
                    /* DL repetido sem DR: reinicia par (reseta contador) */
                    g_cmdBuf6Count[_p] = 0;
                }
                g_cmdBuf6State[_p] = 1; /* aguarda DR */
            } else if (drHit) {
                if (g_cmdBuf6State[_p] == 1) {
                    /* DL→DR completo */
                    g_cmdBuf6State[_p] = 0;
                    g_cmdBuf6Count[_p]++;
                    Log_Print("CMD P%d: DL→DR par (seq.) #%d\n", _p+1, g_cmdBuf6Count[_p]);
                    if (g_cmdBuf6Count[_p] >= 3) Cmd_ResetAllCheats(_p);
                } else {
                    /* DR sem DL precedente: reseta */
                    g_cmdBuf6Count[_p] = 0;
                }
            }

            /* Buffer5 + Buffer9: todos os 5 botões individualmente */
            for (int _ci = 0; _ci < 5; _ci++) {
                if (Input_IsPadHit(_p, all5[_ci])) {
                    if (Cmd_Push(_p, all5[_ci]))
                        Audio_Play(g_waveSoundIds[SND_2_1], false);
                }
            }
        }
    }

    /* Qualquer player ativo pode trocar de modo (UR=próximo, UL=anterior) */
    {
        bool urHit = ((g_game.activePlayerMask & 0x1) && Input_IsPadHit(0, PAD_UR))
                  || ((g_game.activePlayerMask & 0x2) && Input_IsPadHit(1, PAD_UR));
        bool ulHit = ((g_game.activePlayerMask & 0x1) && Input_IsPadHit(0, PAD_UL))
                  || ((g_game.activePlayerMask & 0x2) && Input_IsPadHit(1, PAD_UL));

        if (urHit) {
            if (g_modeAnimActive) {
                g_modeSongIndex[g_selDispIdx] = g_game.selectedSongIndex;
                g_selDispIdx = (g_selDispIdx + g_modeAnimDir + g_modeCount) % g_modeCount;
                g_game.selectedModeIndex = g_modeDBIdx[g_selDispIdx];
                g_game.selectedSongIndex = g_modeSongIndex[g_selDispIdx];
                g_modeAnimActive = false;
            }
            g_modeSongIndex[g_selDispIdx] = g_game.selectedSongIndex;
            {
                int nextDispIdx = (g_selDispIdx + 1) % g_modeCount;
                g_displayModeDBIdx = g_modeDBIdx[nextDispIdx];
                g_displaySongIndex = g_modeSongIndex[nextDispIdx];
            }
            loadCdTextures(); cacheModeTileIndices();
            Audio_Play(g_waveSoundIds[SND_3_2], false);
            selectedState = 0; stopPreview();
            g_modeAnimActive = true; g_modeAnimFrame = 0; g_modeAnimDir = 1;
            triggerCornerArrow(1);  /* UR -> seta superior direita */
            prevSongId = -1; g_songAnimCounter = 0;
            g_carrosselIntro = true; g_introFrame = 0;
            g_carrosselFrame = 588; g_carrosselDir = 0; g_carrosselTarget = 588;
            g_previewDelay = PREVIEW_DELAY_ENTER;
        }

        if (ulHit) {
            if (g_modeAnimActive) {
                g_modeSongIndex[g_selDispIdx] = g_game.selectedSongIndex;
                g_selDispIdx = (g_selDispIdx + g_modeAnimDir + g_modeCount) % g_modeCount;
                g_game.selectedModeIndex = g_modeDBIdx[g_selDispIdx];
                g_game.selectedSongIndex = g_modeSongIndex[g_selDispIdx];
                g_modeAnimActive = false;
            }
            g_modeSongIndex[g_selDispIdx] = g_game.selectedSongIndex;
            {
                int nextDispIdx = (g_selDispIdx - 1 + g_modeCount) % g_modeCount;
                g_displayModeDBIdx = g_modeDBIdx[nextDispIdx];
                g_displaySongIndex = g_modeSongIndex[nextDispIdx];
            }
            loadCdTextures(); cacheModeTileIndices();
            Audio_Play(g_waveSoundIds[SND_3_2], false);
            selectedState = 0; stopPreview();
            g_modeAnimActive = true; g_modeAnimFrame = 0; g_modeAnimDir = -1;
            triggerCornerArrow(0);  /* UL -> seta superior esquerda */
            prevSongId = -1; g_songAnimCounter = 0;
            g_carrosselIntro = true; g_introFrame = 0;
            g_carrosselFrame = 588; g_carrosselDir = 0; g_carrosselTarget = 588;
            g_previewDelay = PREVIEW_DELAY_ENTER;
        }
    }

    // Atualiza animacao dos modos
    if (g_modeAnimActive) {
        g_modeAnimFrame++;
        if (g_modeAnimFrame >= MODE_ANIM_DURATION) {
            g_modeAnimActive = false;
            g_selDispIdx = (g_selDispIdx + g_modeAnimDir + g_modeCount) % g_modeCount;
            g_game.selectedModeIndex = g_modeDBIdx[g_selDispIdx];
            /* Restaura última posição de música do modo destino */
            g_game.selectedSongIndex = g_modeSongIndex[g_selDispIdx];
            /* Sincroniza display vars (devem já estar iguais, mas por segurança) */
            g_displayModeDBIdx = g_game.selectedModeIndex;
            g_displaySongIndex = g_game.selectedSongIndex;
            g_pendingMove = 0;
            g_carrosselDir = 0;
            g_carrosselFrame = 588;
            g_carrosselTarget = 588;
            /* Atualiza flag de BATTLE */
            g_isBattleMode = (g_game.activePlayerMask == 0x3) && (g_selDispIdx == 3);
        }
    }

    SongMode* mode = &db->modes[g_game.selectedModeIndex];
    int songCount = mode->songCount;
    if (songCount == 0) return;

    if (g_carrosselIntro) {
        g_introFrame++;
        if (g_introFrame >= 50) {
            g_carrosselIntro = false;
        }
        /* Não retorna: DL/DR abaixo cancela a intro e move o carrossel normalmente */
    }

    /* Qualquer player ativo navega músicas (DR=próxima, DL=anterior).
     * Auto-scroll ao segurar: replicado de SongSelect_UpdateRender @ 00409720.
     * - Press: scroll imediato + SND_3_2
     * - Repetição a cada 21 frames (timer > 0x14) + SND_10_2
     * - Contador > 0x50 (80): repetição cai para 17 frames (timer > 0x10)
     * - Contador >= 0xb4 (180): timer avança 2×/frame → repetição em 9 frames
     * Resultado: 21 → 17 → 9 frames, uma aceleração em três estágios. */
    {
        bool drHit = ((g_game.activePlayerMask & 0x1) && Input_IsPadHit(0, PAD_DR))
                  || ((g_game.activePlayerMask & 0x2) && Input_IsPadHit(1, PAD_DR));
        bool dlHit = ((g_game.activePlayerMask & 0x1) && Input_IsPadHit(0, PAD_DL))
                  || ((g_game.activePlayerMask & 0x2) && Input_IsPadHit(1, PAD_DL));
        bool drDown = ((g_game.activePlayerMask & 0x1) && Input_IsPadDown(0, PAD_DR))
                   || ((g_game.activePlayerMask & 0x2) && Input_IsPadDown(1, PAD_DR));
        bool dlDown = ((g_game.activePlayerMask & 0x1) && Input_IsPadDown(0, PAD_DL))
                   || ((g_game.activePlayerMask & 0x2) && Input_IsPadDown(1, PAD_DL));

/* Aplica um passo de navegação de carrossel (sem tocar som — quem chama decide o som). */
#define DO_SONG_NAV(dir_) do {                                                      \
    selectedState = 0; stopPreview(); prevSongId = -1;                             \
    g_carrosselIntro = false;                                                       \
    if (g_carrosselDir != 0) {                                                      \
        g_carrosselFrame = g_carrosselTarget;                                       \
        if (g_pendingMove != 0) {                                                   \
            g_game.selectedSongIndex += g_pendingMove;                              \
            if (g_game.selectedSongIndex >= songCount) g_game.selectedSongIndex = 0;\
            if (g_game.selectedSongIndex < 0) g_game.selectedSongIndex = songCount - 1; \
            g_pendingMove = 0;                                                      \
        }                                                                           \
        g_carrosselFrame = 588; g_carrosselDir = 0;                                 \
    }                                                                               \
    g_pendingMove         = (dir_);                                                 \
    g_carrosselTarget     = g_carrosselFrame - (dir_) * 16;                        \
    g_carrosselDir        = -(dir_);                                                \
    g_previewDelay        = PREVIEW_DELAY_NAV;                                      \
} while (0)

        /* Press inicial: scroll imediato + SND_3_2 + inicializa hold state */
        if (drHit) {
            Audio_Play(g_waveSoundIds[SND_3_2], false);
            DO_SONG_NAV(+1);
            g_holdDir = +1; g_holdAnimTimer = 0; g_holdCarouselPos = 0x3c;
            triggerCornerArrow(3);  /* DR -> seta inferior direita */
        }
        if (dlHit) {
            Audio_Play(g_waveSoundIds[SND_3_2], false);
            DO_SONG_NAV(-1);
            g_holdDir = -1; g_holdAnimTimer = 0; g_holdCarouselPos = 0x3c;
            triggerCornerArrow(2);  /* DL -> seta inferior esquerda */
        }

        /* Hold-scroll: enquanto nenhum press novo, incrementa timer e dispara auto-scroll */
        if (!drHit && !dlHit) {
            if (!drDown && !dlDown) {
                /* Solto — zera estado */
                g_holdDir = 0; g_holdAnimTimer = 0; g_holdCarouselPos = 0;
            } else if (!g_modeAnimActive && g_holdDir != 0) {
                /* Ordem replicada do original: o teste de disparo acontece no
                 * começo de SongSelect_UpdateRender (0x0040a820) e o incremento
                 * só no fim da função (0x0040ab29). */

                /* Piso de 60 reaplicado a cada frame — 0x0040a832:
                 *   CMP EAX,0x3c / JGE / MOV EAX,0x3c */
                if (g_holdCarouselPos < 0x3c) g_holdCarouselPos = 0x3c;

                /* Disparo por OU de dois limiares — 0x0040a841..0x0040a852:
                 *   CMP EBP,0x14 / JG  dispara          (20 frames)
                 *   CMP EAX,0x50 / JLE sai              (contador > 80 ...)
                 *   CMP EBP,0x10 / JLE sai              (... e 16 frames)
                 * Sem o segundo termo a rolagem fica presa em 21 frames por
                 * música até o contador chegar a 180. */
                if (g_holdAnimTimer > 0x14 ||
                    (g_holdCarouselPos > 0x50 && g_holdAnimTimer > 0x10)) {
                    g_holdAnimTimer = 0;
                    DO_SONG_NAV(g_holdDir);
                    Audio_Play(g_waveSoundIds[SND_10_2], false);
                }

                /* Incremento do fim da função — 0x0040ab29..0x0040ab67.
                 * O original condiciona o +2 também a [0x00d5fd88] != 0, flag
                 * que ele mesmo liga no primeiro disparo; como o auto-scroll
                 * sempre dispara antes de o contador chegar a 180, aqui ela
                 * seria sempre verdadeira. */
                g_holdAnimTimer   += (g_holdCarouselPos >= 0xb4) ? 2 : 1;
                g_holdCarouselPos++;
            }
        }

#undef DO_SONG_NAV
    }

    // Avanca o frame do carrossel — velocidade 1 = mais lento
    if (g_carrosselDir != 0) {
        g_carrosselFrame += g_carrosselDir * 1;

        if ((g_carrosselDir == +1 && g_carrosselFrame >= g_carrosselTarget) ||
            (g_carrosselDir == -1 && g_carrosselFrame <= g_carrosselTarget)) {
            // Aplicar movimento pendente ao índice só quando a animação terminar
            if (g_pendingMove != 0) {
                g_game.selectedSongIndex += g_pendingMove;
                if (g_game.selectedSongIndex >= songCount) g_game.selectedSongIndex = 0;
                if (g_game.selectedSongIndex < 0) g_game.selectedSongIndex = songCount - 1;
                g_pendingMove = 0;
            }
            // Reset base frame para centro
            g_carrosselFrame = 588;
            g_carrosselTarget = 588;
            g_carrosselDir = 0;
        }
    }

    int songId = mode->songIds[g_game.selectedSongIndex];
    /* Preview audio: não inicia enquanto animação de modo ou intro do carrossel estiver ativa.
     * Ghidra (SongSelect_RenderCarouselMove @ 0x4086c0): no original, Box2 (layer 0x18) só
     * aparece quando local_30 == 20.0 (carrossel de DL/DR no pico). Ao pressionar UL/UR,
     * g_nCarouselSpeed=0 → local_30=0.0 → Box2 nunca aparece e audio para.
     * Aqui replicamos: bloqueamos o preview durante troca de modo e intro do carrossel. */
    /* Contagem corre sempre (como os contadores do original); só o disparo
     * espera o fim das animações.
    if (!g_modeAnimActive && !g_carrosselIntro) {
        if (g_previewDelay > 0.0f) {
            g_previewDelay -= dt;
            if (g_previewDelay <= 0.0f) {
                if (songId != prevSongId) {
                    playPreview(songId);
                }
            }
        } else {
            if (songId != prevSongId)
                playPreview(songId);
        }
    } */
    if (g_previewDelay > 0.0f)
        g_previewDelay -= dt;
    if (!g_modeAnimActive && !g_carrosselIntro && g_previewDelay <= 0.0f
        && songId != prevSongId)
        playPreview(songId);

    /* Confirmação de música: CN de qualquer jogador ativo */
    {
        bool cnHit = (Input_IsPadHit(0, PAD_C) && (g_game.activePlayerMask & 0x1)) ||
                     (Input_IsPadHit(1, PAD_C) && (g_game.activePlayerMask & 0x2));
        if (cnHit) {
            if (!selectedState)
                Audio_Play(g_waveSoundIds[SND_3_2], false);
            if (selectedState) {
                Audio_Play(g_waveSoundIds[SND_4_2], false);
                stopPreview();
                g_game.selectedDifficulty = mode->difficulties[g_game.selectedSongIndex];
                selectedState = 0;
                g_game.isBattleMode = g_isBattleMode; /* propaga flag BATTLE para gameplay */
                Loading_Enter(songId);
            } else {
                selectedState = 1;
            }
        }
    }

    /* ── TIME ────────────────────────────────────────────────────────────────
     * Ordem igual à do original (0x00409720): primeiro recalcula o valor, depois
     * compara com o do frame anterior para o bip, e só então testa o zero.
     * O bip toca a cada troca de segundo enquanto o valor for < 11, ou seja em
     * 10, 9, ... 1, 0 — exatamente o teste `g_nCountdownTimer < 0xb`. */
    {
        if (g_timeRemain > 0.0f) {
            g_timeRemain -= dt;
            if (g_timeRemain < 0.0f) g_timeRemain = 0.0f;
        }
        g_timeShown = (int)g_timeRemain;   /* trunca, como a divisão inteira do original */

        if (g_timeShown != g_timePrev && g_timeShown < TIME_BEEP_BELOW)
            Audio_Play(g_waveSoundIds[SND_10_1], false);
        g_timePrev = g_timeShown;

        /* Zerou: entra na música sob o cursor. No original o contador em zero cai
         * no mesmo caminho do confirm (g_dwState = 0x1e) — tanto que confirmar
         * com o carrossel travado apenas força g_nCountdownTimer = 0. */
        if (g_timeShown <= 0) {
            Audio_Play(g_waveSoundIds[SND_4_2], false);
            stopPreview();
            g_game.selectedDifficulty = mode->difficulties[g_game.selectedSongIndex];
            selectedState = 0;
            g_game.isBattleMode = g_isBattleMode;
            resetTimeCounter();   /* evita disparar de novo antes da troca de estado */
            Loading_Enter(songId);
            return;
        }
    }
}

// Frames dos keyframes do BGA para cada posicao (extraidos do 099.DAT)
#define FRAME_LEFT    14   // Event 1: x=0,   y=0,   hx=0,   sx=1.0
#define FRAME_CENTER  74   // Event 3: x=210, y=35,  hx=110, sx=1.3
#define FRAME_RIGHT   134  // Event 5: x=430, y=0,   hx=110, sx=1.0
#define FRAME_OFF     194  // Event 7: x=210, y=-130, hx=110, sx=1.0

void Gamestate_RenderSongSelect(void) {
    if (g_game.state != STATE_SONG_SELECT && g_game.state != STATE_SONG_SELECT_B)
        return;
    if (g_game.state != STATE_SONG_SELECT) return;

    if (g_game.bgaPicCount > 0) {
        int f = g_game.bgaFrame;
        BGA_SetEventLayer(0, f % 60, 0);

        BGA_SetEventLayer(0, 0, 0x1a);
        BGA_SetEventLayer(0, 0, 0x24);

        // Mantém camadas de fundo/overlay; os boxes serão desenhados
        // intercalados com os CDs abaixo para garantir a ordem desejada.
    }

    SongDB* db = &g_game.songDB;
    /* Durante animação de modo, usa vars de display (atualizadas imediatamente
     * no press de UR/UL) para que os CDs já mostrem o modo destino. */
    int renderModeIdx = g_modeAnimActive ? g_displayModeDBIdx : g_game.selectedModeIndex;
    int renderSongIdx = g_modeAnimActive ? g_displaySongIndex : g_game.selectedSongIndex;
    SongMode* mode = &db->modes[renderModeIdx];
    int songCount = mode->songCount;
    if (songCount == 0) return;

    // Renderiza os 7 slots do carrossel em ordem de distância do centro,
    // garantindo que os discos mais distantes fiquem atrás e os mais próximos fiquem por cima.
    typedef struct {
        int slotIndex;
        float screenX;
        float bsc;
        float balpha;
        int texId;
        int cdHalf;
        float tw;
        float th;
        int bgaSlotFrame;
        float introXOff;
        float cdYOff;   /* Y offset por posicao: far=10, mid=5, center=0 */
    } SlotRender;
    /* Y offset proporcional ao bx do slot: max 4px nas posições left-2/right-2 (bx=±175),
     * interpola continuamente durante animação — sem jump ao final do carrossel. */
    static const float kCdYOffScale = 4.0f / 175.0f;

    SlotRender slots[7];
    int slotCount = 0;

    for (int si = 0; si < 7; si++) {
        float slotFrame;
        float introXOff = 0.0f;

        if (g_carrosselIntro) {
            /* Cascata de entrada — segue SongSelect_RenderBoxesStatic (00408290):
             * todos os BCDs entram simultaneamente da direita a 106px/frame,
             * mas cada posição começa a uma distância diferente (incremento de 636):
             *   far-left(556): 640px → chega ao fim em ~6 frames
             *   left(572):    1276px → ~12 frames
             *   center(588):  1912px → ~18 frames
             *   right(604):   2548px → ~24 frames
             *   far-right(620): 3184px → ~30 frames
             * Valor 0x6a=106 = velocidade original exata. */
            int rawFrame = 588 + g_slotFrameOffset[si];
            float cascadeStart;
            if      (rawFrame <= 556) cascadeStart = 640.0f;
            else if (rawFrame <= 572) cascadeStart = 1276.0f;
            else if (rawFrame <= 588) cascadeStart = 1912.0f;
            else if (rawFrame <= 604) cascadeStart = 2548.0f;
            else                      cascadeStart = 3184.0f;
            float remaining = cascadeStart - 106.0f * (float)g_introFrame;
            introXOff = (remaining > 0.0f) ? remaining : 0.0f;
            slotFrame = (float)rawFrame;
        } else {
            slotFrame = (float)(g_carrosselFrame + g_slotFrameOffset[si]);
        }
        float bx, bsc, balpha;
        EvalBoxFrame(slotFrame, &bx, &bsc, &balpha);
        if (balpha < 0.01f) continue;

        float screenX = 320.0f + bx;
        int slotOffset = si - 3;
        int idx = (renderSongIdx + slotOffset + songCount) % songCount;
        int sid = mode->songIds[idx];
        int si2 = Song_FindByID(db, sid);

        int cdHalf = 0;
        int cdIdx = (si2 >= 0) ? findCdForSong(sid, &cdHalf) : -1;
        int texId = (cdIdx >= 0) ? g_cdTexIds[cdIdx] : -1;

        if (slotCount < 7) {
            int bgaSlotFrame = (int)slotFrame;
            if (bgaSlotFrame < 540) bgaSlotFrame = 540;
            if (bgaSlotFrame > 636) bgaSlotFrame = 636;
            slots[slotCount].slotIndex = si;
            slots[slotCount].screenX = screenX + introXOff;
            slots[slotCount].bsc = bsc;
            slots[slotCount].balpha = balpha;
            slots[slotCount].texId = texId;
            slots[slotCount].cdHalf = cdHalf;
            slots[slotCount].tw = 256.0f * bsc;
            slots[slotCount].th = 128.0f * bsc;
            slots[slotCount].bgaSlotFrame = bgaSlotFrame;
            slots[slotCount].introXOff = introXOff;
            slots[slotCount].cdYOff = fabsf(bx) * kCdYOffScale;
            slotCount++;
        }
    }

    /* Ordena pela posição de REPOUSO (sem introXOff), maior distância primeiro.
     * Para mesma distância, esquerda antes de direita — segue a ordem do Ghidra:
     * far-left, far-right, left, right, center.
     * Importante: durante a cascata screenX inclui introXOff (posição atual),
     * mas a profundidade deve ser determinada pela posição final de cada slot. */
    for (int i = 0; i < slotCount - 1; i++) {
        for (int j = i + 1; j < slotCount; j++) {
            float restI = slots[i].screenX - slots[i].introXOff;
            float restJ = slots[j].screenX - slots[j].introXOff;
            float distI = fabsf(restI - 320.0f);
            float distJ = fabsf(restJ - 320.0f);
            bool swap = false;
            if (distI < distJ) {
                swap = true;
            } else if (distI == distJ) {
                /* Mesma distância: esquerda (< 320) antes de direita */
                if (restI > 320.0f && restJ < 320.0f)
                    swap = true;
            }
            if (swap) {
                SlotRender tmp = slots[i];
                slots[i] = slots[j];
                slots[j] = tmp;
            }
        }
    }

    /* Loop único de renderização — segue SongSelect_RenderBoxesStatic (00408290):
     * para cada slot, CD primeiro (atrás), depois Box (na frente).
     * Ordem dos slots: far-left, far-right, left, right, center.
     *
     * Estado estático (g_carrosselDir==0, !g_carrosselIntro):
     *   Layers dedicadas por posição (não-compactadas), frame=0:
     *     556 → 0x0b(11) far-left, 572 → 0x11(17) left,
     *     588 → 0x17(23) center,   604 → 0x14(20) right,
     *     620 → 0x0e(14) far-right
     * Estado animando: todos usam layer 0x0b com frame 540-636.
     */
    bool bgaIsAnimating = (g_carrosselDir != 0) || g_carrosselIntro;

    for (int i = 0; i < slotCount; i++) {
        SlotRender* slot = &slots[i];

        /* --- CD art (atrás, igual ao glPushMatrix inner do Ghidra) --- */
        if (slot->texId >= 0) {
            float cdScaleX, cdScaleY, cdOffX, cdOffY;
            if (selectedState && slot->slotIndex == 3 && !bgaIsAnimating) {
                cdScaleX = 1.1f; cdScaleY = 1.1f; cdOffX = 0.0f; cdOffY = -6.0f;
            } else {
                cdScaleX = 1.00f; cdScaleY = 0.95f; cdOffX = 0.0f; cdOffY = -1.0f;
            }
            float sw = slot->tw * cdScaleX;
            float sh = slot->th * cdScaleY;
            /* Cada CDxx.PNG traz duas capas empilhadas: a música da coluna 0 de
             * g_cdSongs em cima (linhas 0..128) e a da coluna 1 embaixo. Como
             * V=0 é o topo da imagem, cdHalf entra direto. O (1-cdHalf) que
             * havia aqui escolhia sempre a metade errada — por isso a capa
             * mostrada era a da música seguinte. */
            float u1 = 0.0f;
            float v1 = (float)slot->cdHalf * 128.0f;
            float u2 = 256.0f;
            float v2 = v1 + 128.0f;
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            Texture_DrawUV(slot->texId,
                slot->screenX - sw * 0.5f + cdOffX,
                240.0f - sh * 0.5f - 10.0f + cdOffY + slot->cdYOff,
                sw, sh * 1.575f,
                u1, v1, u2, v2,
                1.0f, 1.0f, 1.0f, slot->balpha);
        }

        /* --- Box frame (na frente, BGA_SetEventLayer) --- */
        if (bgaIsAnimating) {
            /* Carousel em movimento: layer 0x0b, frame interpolado */
            if (g_carrosselIntro && slot->introXOff != 0.0f) {
                glPushMatrix();
                glTranslatef(slot->introXOff, 0.0f, 0.0f);
                BGA_SetEventLayer(0, slot->bgaSlotFrame, 0x0b);
                glPopMatrix();
            } else {
                BGA_SetEventLayer(0, slot->bgaSlotFrame, 0x0b);
            }
        } else {
            /* Estático: layer dedicada, frame=0 */
            int staticLayer = -1;
            switch (slot->bgaSlotFrame) {
                case 556: staticLayer = 0x0b; break; /* far-left  */
                case 572: staticLayer = 0x11; break; /* left      */
                case 588: staticLayer = 0x17; break; /* center    */
                case 604: staticLayer = 0x14; break; /* right     */
                case 620: staticLayer = 0x0e; break; /* far-right */
            }
            if (staticLayer >= 0) {
                if (selectedState && slot->slotIndex == 3) {
                    glPushMatrix();
                    glTranslatef(320.0f, 240.0f, 0.0f);
                    glScalef(1.1f, 1.1f, 1.0f);
                    glTranslatef(-320.0f, -240.0f, 0.0f);
                    BGA_SetEventLayer(0, 0, staticLayer);
                    glPopMatrix();
                } else {
                    BGA_SetEventLayer(0, 0, staticLayer);
                }
            }
        }
    }

    // Desenha os sprites dos modos (esquerda, centro, direita)
    // g_modeLayersDyn já contém os layer indices corretos (incl. BATTLE.SPR para slot 3)
    if (g_game.bgaPicCount > 0) {
        int leftIdx  = (g_selDispIdx - 1 + g_modeCount) % g_modeCount;
        int centIdx  = g_selDispIdx;
        int rightIdx = (g_selDispIdx + 1) % g_modeCount;

        if (g_modeAnimActive) {
            float t = g_modeAnimFrame / (float)MODE_ANIM_DURATION;
            int oldLeft   = (g_selDispIdx - 1 + g_modeCount) % g_modeCount;
            int oldCenter = g_selDispIdx;
            int oldRight  = (g_selDispIdx + 1) % g_modeCount;
            if (g_modeAnimDir == 1) {
                int newRight = (g_selDispIdx + 2) % g_modeCount;
                BGA_SetEventLayer(0, (int)Math_Lerp(420.0f, 434.0f, t), g_modeLayersDyn[oldLeft]);
                BGA_SetEventLayer(0, (int)Math_Lerp(74.0f, 60.0f, t),   g_modeLayersDyn[oldCenter]);
                BGA_SetEventLayer(0, (int)Math_Lerp(134.0f, 120.0f, t), g_modeLayersDyn[oldRight]);
                BGA_SetEventLayer(0, (int)Math_Lerp(194.0f, 180.0f, t), g_modeLayersDyn[newRight]);
            } else {
                int newLeft = (g_selDispIdx - 2 + g_modeCount) % g_modeCount;
                BGA_SetEventLayer(0, (int)Math_Lerp(180.0f, 194.0f, t), g_modeLayersDyn[oldRight]);
                BGA_SetEventLayer(0, (int)Math_Lerp(120.0f, 134.0f, t), g_modeLayersDyn[oldCenter]);
                BGA_SetEventLayer(0, (int)Math_Lerp(60.0f, 74.0f, t),   g_modeLayersDyn[oldLeft]);
                BGA_SetEventLayer(0, (int)Math_Lerp(434.0f, 420.0f, t), g_modeLayersDyn[newLeft]);
            }
        } else {
            BGA_SetEventLayer(0, FRAME_LEFT,   g_modeLayersDyn[leftIdx]);
            BGA_SetEventLayer(0, FRAME_CENTER, g_modeLayersDyn[centIdx]);
            BGA_SetEventLayer(0, FRAME_RIGHT,  g_modeLayersDyn[rightIdx]);
        }
    }

    // box2.spr glow pulsante — só mostra quando preview está ativo E fora de animação de modo/intro.
    // Ghidra: Box2 (0x18) aparece apenas quando carrossel DL/DR chegou ao pico (local_30==20.0).
    // Durante UL/UR (troca de modo), local_30=0.0 → Box2 nunca renderizado no original.
    if (g_game.bgaPicCount > 0 && previewState && !g_modeAnimActive && !g_carrosselIntro) {
        if (selectedState) {
            glPushMatrix();
            glTranslatef(320.0f, 240.0f, 0.0f);
            glScalef(1.1f, 1.1f, 1.0f);
            glTranslatef(-320.0f, -240.0f, 0.0f);
            BGA_SetEventLayer(0, g_game.bgaFrame % 55, 0x18);
            glPopMatrix();
        } else {
            BGA_SetEventLayer(0, g_game.bgaFrame % 55, 0x18);
        }
    }

    /* Setas de canto "Next MODE" / "Next MUSIC" (layers do 099.BGA) */
    resolveCornerArrows();
    renderCornerArrows();

    /* Indicador de dificuldade — Judge_RenderGradeRow (0x004071b0).
     * Mesmo gate do Box2: só com o preview tocando e fora de animação. */
    if (previewState && !g_modeAnimActive && !g_carrosselIntro) {
        if (songCount > 0 && g_diffLculIdx >= 0) {
            /* Usa exatamente o mesmo `mode` e `renderSongIdx` do carrossel.
             * g_game.selectedModeIndex diverge de renderModeIdx durante a troca
             * de modo, e ler do array errado dava o nível de outra música. */
            int di    = ((renderSongIdx % songCount) + songCount) % songCount;
            int level = mode->difficulties[di];

            /* NIGHTMARE e DIVISION carregam 99: trocam a bolinha pela caveira
             * animada, mas a contagem continua vindo do nível — 99 caveiras,
             * como no original, mesmo que a maioria saia da tela. */
            bool isNight = (level == DIFF_LEVEL_NIGHT);
            int  count   = level;
            if (count > 0) {
                int sprBase = isNight ? g_diffNighIdx : g_diffLculIdx;

                /* A fileira nasce do centro da tela: contagem ímpar tem um
                 * sprite exatamente em X=320 e os demais saem em pares para os
                 * lados; contagem par fica meio passo deslocada para que o
                 * conjunto continue centrado. Equivale ao par de translates do
                 * original (-30 / -15), que com o offset interno do .spr
                 * resultava em 319.5 para o caso ímpar. */
                float centerOff = (count % 2 == 0) ? -DIFF_DOT_SPACING * 0.5f : 0.0f;

                /* O DIFFNIGH é desenhado com o dobro do tamanho. */
                float zoom = isNight ? DIFF_NIGHT_ZOOM : 1.0f;

                /* 2 frames alternando, como o DIFFNIGH (TYPE ani, NUM 2) pede */
                int nightFrame = (int)((g_game.frameCounter / 15) % 2);

                for (int i = 0; i < count; i++) {
                    /* Posições alternam expandindo do centro: 0, +30, -30, +60…
                     * No original: i par → (-i)*15, i ímpar → (i+1)*15 */
                    float off = (i % 2 == 0) ? (float)(-i) : (float)(i + 1);
                    float tx  = 320.0f + centerOff + off * DIFF_DOT_SPACING;

                    int idx = sprBase + (isNight ? nightFrame : 0);
                    if (idx < 0 || idx >= g_game.sprTileCount) continue;

                    /* Os quatro campos do .spr são x, y, w, h — confirmado no
                     * ParseSPR_TileDefinition do original (0x0040baa0), que
                     * guarda x2 = x + w e y2 = y + h. Portanto srcW/srcH do
                     * nosso parser são mesmo largura e altura.
                     *   DIFFLCUL: 0,0,46,43   DIFFNIGH: 9,6,28,31 */
                    SPRTileDef* t = &g_game.sprTiles[idx];
                    float w = (float)t->srcW * zoom;
                    float h = (float)t->srcH * zoom;
                    /* tx já é o centro: a fileira parte do centro da tela. */
                    float cx = tx;
                    /* SPR_RenderTile emite glVertex2i(x, 480 - y), ou seja o
                     * SPR é Y-DOWN. Somado ao translate de -385 do original, o
                     * topo fica em DIFF_ROW_Y + y e o centro meia altura abaixo.
                     * Dá 406.5 para os dois sprites. */
                    float cy = DIFF_ROW_Y + (float)t->srcY + h * 0.5f;

                    if (isNight) {
                        /* Balanço do original: triângulo 0→15→0 sobre 30 frames,
                         * ângulo = tri*2 - 30 (varia de -30° a 0°), pivô no
                         * centro do próprio sprite. glRotatef opera em Y-UP. */
                        int tri = (int)(g_game.frameCounter % 30);
                        if (tri > 15) tri = 30 - tri;
                        float ang  = (float)(tri * 2) - 30.0f;
                        float cyUp = 480.0f - cy;
                        glPushMatrix();
                        glTranslatef(cx, cyUp, 0.0f);
                        glRotatef(ang, 0.0f, 0.0f, 1.0f);
                        glTranslatef(-cx, -cyUp, 0.0f);
                        Sprite_DrawTileUV(idx, cx, cy, w, h, 1.0f);
                        glPopMatrix();
                    } else {
                        Sprite_DrawTileUV(idx, cx, cy, w, h, 1.0f);
                    }
                }
            }
        }
    }

    /* ── TIME: rótulo + contador ─────────────────────────────────────────────
     * A layer time.spr do 099.BGA fica em x=0 y=0 com keyframes type=1 no frame
     * 0 e type=0 no 420, isto é, visível o tempo todo — quem posiciona é o
     * próprio tile do .spr (241, -2, 75x37). blend=1 no keyframe é aditivo. */
    if (g_timeSprIdx >= 0 && g_timeSprIdx < g_game.sprTileCount) {
        SPRTileDef* t = &g_game.sprTiles[g_timeSprIdx];
        float w = (float)t->srcW;
        float h = (float)t->srcH;
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        Sprite_DrawTileUV(g_timeSprIdx,
                          (float)t->srcX + w * 0.5f,
                          (float)t->srcY + h * 0.5f,
                          w, h, 1.0f);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    drawTimeNumber(TIME_DIGIT_X, TIME_DIGIT_Y, g_timeShown, TIME_DIGIT_COUNT);

    if (previewState) {
        char buf[64];
        snprintf(buf, sizeof(buf), "D%d.AUD", prevSongId);
        Font_DrawStringCentered(320, 410, buf, 0.5f, 1.0f, 0.5f, 1.0f);
    }

    if (selectedState) {
        Font_DrawStringCentered(320, 380, "SELECTED", 0.0f, 1.0f, 0.0f, 1.0f);
    }

    /* ── Ícones de Command na HUD do SongSelect ────────────────────────────
     * Velocidade (Y=184):
     *   accel1=36, accel2=7, accel3=8, accel4=9, raccel(RV)=12  (ARROW541.SP2)
     *   Quando Random Velocity ativo: exibe raccel(12) independente do speedMult.
     * Modificadores (Y=216,248,280,312): RandomStep, Mirror, Vanish, NonStep
     *   Inativo: _randm=34, _mirrr=35, _vanis=37, _nnstp=38
     *   Ativo:    random=10,  mirror=11,  vanish=13,  nonstp=14
     * P1 lado esquerdo (hx = 18 + sw/2), P2 lado direito (hx = 640-18-sw/2).
     */
    if (g_fontArrow541 >= 0) {
        /* Tabela de offsets: OFF e ON para [RandomStep, Mirror, Vanish, NonStep] */
        static const int   kModOff_OFF[4] = { 34, 35, 37, 38 }; /* _randm,_mirrr,_vanis,_nnstp */
        static const int   kModOff_ON[4]  = { 10, 11, 13, 14 }; /* random,mirror,vanish,nonstp  */
        static const float kModY[4]       = { 216.0f, 248.0f, 280.0f, 312.0f };

        for (int _p = 0; _p < 2; _p++) {
            if (!(g_game.activePlayerMask & (1 << _p))) continue;

            /* ── Velocidade ──────────────────────────────────────────────── */
            int speedOff;
            if (g_game.cmdRandomVelocity[_p]) {
                speedOff = 12; /* raccel — RV do ciclo (após x4) ou Random Velocity (Buffer9) */
            } else {
                speedOff = 36; /* accel1 */
                if      (g_game.cmdSpeedMult[_p] >= 4) speedOff = 9;
                else if (g_game.cmdSpeedMult[_p] >= 3) speedOff = 8;
                else if (g_game.cmdSpeedMult[_p] >= 2) speedOff = 7;
            }
            int speedIdx = g_fontArrow541 + speedOff;
            if (speedIdx < g_game.sprTileCount) {
                float sw = (float)g_game.sprTiles[speedIdx].srcW;
                float sh = (float)g_game.sprTiles[speedIdx].srcH;
                float hx = (_p == 0) ? (18.0f + sw/2.0f) : (640.0f - 18.0f - sw/2.0f);
                Sprite_DrawTileUV(speedIdx, hx, 184.0f + sh/2.0f, sw, sh, 1.0f);
            }

            /* ── Modificadores: R, M, V, NS ──────────────────────────────── */
            bool modActive[4] = {
                g_game.cmdRandomStep[_p],
                g_game.cmdMirror[_p],
                g_game.cmdVanish[_p],
                g_game.cmdNonStep[_p]
            };
            for (int di = 0; di < 4; di++) {
                int tileOff = modActive[di] ? kModOff_ON[di] : kModOff_OFF[di];
                int didx = g_fontArrow541 + tileOff;
                if (didx < g_game.sprTileCount) {
                    float sw = (float)g_game.sprTiles[didx].srcW;
                    float sh = (float)g_game.sprTiles[didx].srcH;
                    float hx = (_p == 0) ? (18.0f + sw/2.0f) : (640.0f - 18.0f - sw/2.0f);
                    Sprite_DrawTileUV(didx, hx, kModY[di] + sh/2.0f, sw, sh, 1.0f);
                }
            }
        }
    }
}
#endif  /* DESATIVADO */
