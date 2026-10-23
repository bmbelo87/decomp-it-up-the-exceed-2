/*
 * battle_lua.c — scripts Lua do modo de jogo do Exceed2 (PIU32.EXE).
 *
 * O PIU32.EXE embute Lua 5.0.2 e cada jogador tem a sua lua_State
 * ([player+0x234], criada em 0x40B6A0):
 *   - battle ([0x484FB4] == 2, provável) -> script/battlemode.lua
 *   - senão                             -> script/normalmode.lua
 * Depois lê SCORE_PERFECT..SCORE_NIGHTMARE_BONUS do script ([+0x23C..+0x254]).
 *
 * Os scripts são o bytecode 5.0 original. A pasta game\SCRIPT da instalação
 * tem um BATTLEMODE.LUA decompilado/modificado (texto, 24/06/2026); por isso
 * o LUA_PATH aponta para game\SCRIPT (cópia original de 2013).
 *
 * Funções C registradas pelo EXE (0x41D4xx..0x41D65C):
 *   GetBGAInstance GetBattleBGAInstance BGADraw BGALoad DrawScene DrawSceneEx
 *   ResetAllScene  ResetScene IsSceneEnd SetTextureGroup UnloadTextureGroup
 *   SendMessageToPlayer (0x41D2B0) ItemProc (0x41D350) SetProcState (0x41D290)
 *   EFFECT (0x41D3D0)
 * Por enquanto as de cena/efeito são stubs com log; SendMessageToPlayer e
 * ItemProc seguem o original (0x40C120 / 0x40C2B0).
 */
#include <stdio.h>
#include <string.h>
#include "pumpy.h"
#include "bga.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

/* Original: putenv("LUA_PATH=?;?.lua;script/?.lua;script/?")
 * Port: tenta game/SCRIPT (cópia original de 2013, layout do repo) e depois
 * SCRIPT/ da pasta do jogo (layout da instalação, igual ao original). */
static const char *k_bluaDirs[] = { "game/SCRIPT/", "SCRIPT/" };

static lua_State *g_L[2];
static int        g_battle;          /* 1 = battlemode.lua carregado */

/* [+0x23C..+0x254] */
int g_bluaScore[2][7];

/* Despacho por estado: o EXE usa o jogador "corrente" ([0x484FE8]) dentro
 * dos callbacks. Aqui basta achar o índice pela lua_State. */
static int playerOf(lua_State *L) {
    return (L == g_L[1]) ? 1 : 0;
}

static int blua_print(lua_State *L) {
    int n = lua_gettop(L);
    char buf[512];
    size_t len = 0;
    buf[0] = 0;
    for (int i = 1; i <= n && len < sizeof(buf) - 1; i++) {
        const char *s;
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        s = lua_tostring(L, -1);
        len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s%s", i > 1 ? "\t" : "", s ? s : "?");
        lua_pop(L, 1);
    }
    Log_Print("LUA P%d: %s\n", playerOf(L) + 1, buf);
    return 0;
}

static int callMsgProc(int p, int m1, int m2, int m3, int m4);

/* 0x41D2B0: SendMessageToPlayer(player, m1, m2, m3, m4) -> 0x40C120 */
static int blua_SendMessageToPlayer(lua_State *L) {
    int p  = (int)lua_tonumber(L, 1);
    int m1 = (int)lua_tonumber(L, 2);
    int m2 = (int)lua_tonumber(L, 3);
    int m3 = (int)lua_tonumber(L, 4);
    int m4 = (int)lua_tonumber(L, 5);
    if (p < 0 || p > 1) return 0;
    callMsgProc(p, m1, m2, m3, m4);
    return 0;
}

/* arcade 0x806DB70 / 0x805FE48: SetBattleEffectFont(n) grava n em [PLAY+0x44778]
 * (um só para os dois jogadores) e reinicia a cena k_btFont[n] do BT_GUI
 * ([0x80BED20] = GetBattleBGAInstance(0)). Não existe no PIU32: lá o script
 * dava erro na linha 659 e o ItemStatus não voltava para WAITING. */
static const char *const k_btFont[8] = {          /* tabela 0x80874A0 */
    "fantastic!!", "yeah!!", "ooops!!", "yahpp", "ouch", "marvelous!!", "argggg!!", "kaboom!!!"
};
static int g_btFont = -1;
static int blua_SetBattleEffectFont(lua_State *L) {
    int n = (int)lua_tonumber(L, 1);
    if (n < 0 || n > 7) return 0;
    g_btFont = n;
    if (g_btBga[0] >= 0) BGA_SceneReset(g_btBga[0], k_btFont[n]);
    return 0;
}

/* arcade 0x80594DD..0x8059552: com battle e sem HYBRID, desenha a cena do
 * texto; no fim reinicia e volta para -1 */
void BLua_DrawEffectFont(void) {
    if (g_btFont < 0 || g_btBga[0] < 0) return;
    if (g_exBattleModeBit & 0x10000) return;
    BGA_ScenePlay(g_btBga[0], k_btFont[g_btFont], true);
    if (BGA_SceneDone(g_btBga[0], k_btFont[g_btFont])) {
        BGA_SceneReset(g_btBga[0], k_btFont[g_btFont]);
        g_btFont = -1;
    }
}
void BLua_ResetEffectFont(void) { g_btFont = -1; }   /* 0x8058B52 */

/* 0x41D350: ItemProc(player, item, level, x) -> 0x40C2B0 no jogador "player" */
static int blua_ItemProc(lua_State *L) {
    int p     = (int)lua_tonumber(L, 1);
    int item  = (int)lua_tonumber(L, 2);
    int level = (int)lua_tonumber(L, 3);
    if (p < 0 || p > 1) return 0;
    Gameplay_BattleItemProc(p, item, level);
    return 0;
}

/* Sons do EFFECT(n) (0x41D3D0 toca o buffer [0xCA0DE0 + 4n]). Só os do battle
 * estão mapeados: 0x423D7B carrega WAVE/B%02d.WAV (1..19) nos slots 18..36. */
static int g_effWave[64];
static int g_effInit;
void BLua_PlayEffect(int n) {
    if (!g_effInit) {
        for (int i = 0; i < 64; i++) g_effWave[i] = -2;
        g_effInit = 1;
    }
    if (n < 0 || n >= 64) return;
    if (g_effWave[n] == -2) {
        g_effWave[n] = -1;
        if (n >= 18 && n <= 36) {
            char nm[32];
            snprintf(nm, sizeof(nm), "B%02d.WAV", n - 17);
            g_effWave[n] = Audio_LoadWaveFile(nm);
        } else {
            Log_Print("BLUA: EFFECT %d sem WAV mapeado\n", n);
        }
    }
    if (g_effWave[n] >= 0) Audio_Play(g_effWave[n], false);
}
static int blua_EFFECT(lua_State *L) {
    BLua_PlayEffect((int)lua_tonumber(L, 1));
    return 0;
}

/* BGAs do battle ([0x484FC8..0x484FD4], carregados em 0x405C1D):
 * 0 BT_GUI, 1 BT_ITEM, 2 BT_C1, 3 BT_C2. Índice em g_game.bgaPics. */
int g_btBga[4] = { -1, -1, -1, -1 };
static int g_curBga[2] = { -1, -1 };   /* instância corrente (0x41D060) */

/* GetBattleBGAInstance(n): instância corrente = BGA n do battle */
static int blua_GetBattleBGAInstance(lua_State *L) {
    int n = (int)lua_tonumber(L, 1);
    g_curBga[playerOf(L)] = (n >= 0 && n < 4) ? g_btBga[n] : -1;
    return 0;
}
/* 0x41D120 -> 0x41F0F0(nome, 1) */
static int blua_DrawScene(lua_State *L) {
    const char *s = lua_tostring(L, 1);
    int b = g_curBga[playerOf(L)];
    if (s && b >= 0) BGA_ScenePlay(b, s, true);
    return 0;
}
/* 0x41D150 -> 0x41F200(nome, deslocamento) */
static int blua_DrawSceneEx(lua_State *L) {
    const char *s = lua_tostring(L, 1);
    int off = (int)lua_tonumber(L, 2);
    int b = g_curBga[playerOf(L)];
    if (s && b >= 0) BGA_ScenePlayAt(b, s, off);
    return 0;
}
/* 0x41D1B0 -> 0x41F390 */
static int blua_ResetScene(lua_State *L) {
    const char *s = lua_tostring(L, 1);
    int b = g_curBga[playerOf(L)];
    if (s && b >= 0) BGA_SceneReset(b, s);
    return 0;
}
/* 0x41D1E0 -> 0x41F2E0 */
static int blua_IsSceneEnd(lua_State *L) {
    const char *s = lua_tostring(L, 1);
    int b = g_curBga[playerOf(L)];
    lua_pushnumber(L, (s && b >= 0 && BGA_SceneDone(b, s)) ? 1 : 0);
    return 1;
}

/* 0x41D190 -> 0x41F350 */
static int blua_ResetAllScene(lua_State *L) {
    int b = g_curBga[playerOf(L)];
    if (b >= 0) BGA_SceneResetAll(b);
    return 0;
}

/* Stubs: textura, efeito sonoro, estado do proc, BGA genérico. */
static int blua_stub_nil(lua_State *L)  { (void)L; return 0; }
static int blua_stub_zero(lua_State *L) { lua_pushnumber(L, 0); return 1; }

static const struct { const char *name; lua_CFunction fn; } g_hostFuncs[] = {
    { "GetBGAInstance",       blua_stub_nil },
    { "GetBattleBGAInstance", blua_GetBattleBGAInstance },
    { "BGADraw",              blua_stub_nil },
    { "BGALoad",              blua_stub_zero },
    { "DrawScene",            blua_DrawScene },
    { "DrawSceneEx",          blua_DrawSceneEx },
    { "ResetAllScene",        blua_ResetAllScene },
    { "ResetScene",           blua_ResetScene },
    { "IsSceneEnd",           blua_IsSceneEnd },
    { "SetTextureGroup",      blua_stub_nil },
    { "UnloadTextureGroup",   blua_stub_nil },
    { "SendMessageToPlayer",  blua_SendMessageToPlayer },
    { "ItemProc",             blua_ItemProc },
    { "SetProcState",         blua_stub_nil },
    { "SetBattleEffectFont",  blua_SetBattleEffectFont },   /* só no arcade */
    { "EFFECT",               blua_EFFECT },
};

/* Erro de script: o original abre MessageBox("Lua Error") e encerra
 * (0x40C120). Aqui só registra no log. */
static int pcallChecked(lua_State *L, int nargs, int nres, const char *what) {
    int st = lua_pcall(L, nargs, nres, 0);
    if (st != 0) {
        Log_Print("BLUA: Lua Error em %s: %s\n", what, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return st;
}

static double getNumberGlobal(lua_State *L, const char *name) {
    double v;
    lua_getglobal(L, name);
    v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

static const char *const g_scoreNames[7] = {
    "SCORE_PERFECT", "SCORE_GREAT", "SCORE_GOOD", "SCORE_BAD",
    "SCORE_MISS", "SCORE_COMBO", "SCORE_NIGHTMARE_BONUS"
};

void BLua_Close(void) {
    for (int p = 0; p < 2; p++) {
        if (g_L[p]) lua_close(g_L[p]);
        g_L[p] = NULL;
    }
}

/* 0x40B6A0 (por jogador) + 0x401210 */
int BLua_Init(int battle) {
    BLua_Close();
    g_battle = battle;
    for (int p = 0; p < 2; p++) {
        lua_State *L = lua_open();
        char path[MAX_PATH], dir[MAX_PATH], luaPath[MAX_PATH * 2 + 16];
        FILE *fp = NULL;
        if (!L) return 0;
        for (size_t d = 0; d < sizeof(k_bluaDirs) / sizeof(k_bluaDirs[0]); d++) {
            snprintf(dir, sizeof(dir), "%s/%s", g_game.currentDirectory, k_bluaDirs[d]);
            snprintf(path, sizeof(path), "%s%s", dir,
                     battle ? "BATTLEMODE.LUA" : "NORMALMODE.LUA");
            if ((fp = fopen(path, "rb")) != NULL) { fclose(fp); break; }
        }
        snprintf(luaPath, sizeof(luaPath), "?;?.lua;%s?.lua;%s?", dir, dir);
        g_L[p] = L;
        /* Hipótese: bibliotecas abertas em 0x41D420/0x43F040. Os scripts
         * usam print, require e math.random. */
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        luaopen_math(L);
        lua_settop(L, 0);
        lua_register(L, "print", blua_print);
        lua_pushstring(L, luaPath);
        lua_setglobal(L, "LUA_PATH");
        for (size_t i = 0; i < sizeof(g_hostFuncs) / sizeof(g_hostFuncs[0]); i++)
            lua_register(L, g_hostFuncs[i].name, g_hostFuncs[i].fn);

        if (lua_dofile(L, path) != 0) {
            Log_Print("BLUA: falha ao carregar %s\n", path);
            BLua_Close();
            return 0;
        }
        for (int i = 0; i < 7; i++)
            g_bluaScore[p][i] = (int)getNumberGlobal(L, g_scoreNames[i]);

        lua_getglobal(L, "SetPlayerNum");
        lua_pushnumber(L, p);
        pcallChecked(L, 1, 0, "SetPlayerNum");
    }
    Log_Print("BLUA: %s carregado (PERFECT %d MISS %d)\n",
              battle ? "battlemode" : "normalmode", g_bluaScore[0][0], g_bluaScore[0][4]);
    return 1;
}

int BLua_IsActive(void) { return g_L[0] != NULL; }

/* 0x40C120: MSGPROC(m1..m4) na lua_State do jogador de destino. */
static int callMsgProc(int p, int m1, int m2, int m3, int m4) {
    lua_State *L = g_L[p];
    if (!L) return 0;
    lua_getglobal(L, "MSGPROC");
    lua_pushnumber(L, m1);
    lua_pushnumber(L, m2);
    lua_pushnumber(L, m3);
    lua_pushnumber(L, m4);
    pcallChecked(L, 4, 0, "MSGPROC");
    /* Pós-processamento do EXE (0x40C120): PM_SET_PLAY_LEVEL=4 grava o nível
     * m2 nas linhas a partir da atual ([+0x5C + i*4], i > [+0x128]);
     * PM_RESET_TO_DEFAULT=5 volta [+0x148] = [+0x258] e limpa 0x1000.
     * Ainda não aplicados no gameplay do projeto. */
    if (m1 == 4) Gameplay_BattleSetLevel(p, m2);
    else if (m1 == 5) Gameplay_BattleResetSpeed(p);
    return 1;
}

static void callVoid1(int p, const char *fn, double a) {
    lua_State *L = g_L[p];
    if (!L) return;
    lua_getglobal(L, fn);
    lua_pushnumber(L, a);
    pcallChecked(L, 1, 0, fn);
}

static double callNum0(int p, const char *fn) {
    lua_State *L = g_L[p];
    double v;
    if (!L) return 0.0;
    lua_getglobal(L, fn);
    if (pcallChecked(L, 0, 1, fn) != 0) return 0.0;
    v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return v;
}

/* 0x40C5E0: GetItemSlot(k) do script (1 = vazio) */
int BLua_GetItemSlot(int p, int k) {
    lua_State *L = g_L[p];
    double v;
    if (!L) return 1;
    lua_getglobal(L, "GetItemSlot");
    lua_pushnumber(L, k);
    if (pcallChecked(L, 1, 1, "GetItemSlot") != 0) return 1;
    v = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return (int)v;
}

/* 0x40C240: DrawItemBox() no script do jogador */
void BLua_DrawItemBox(int p) {
    lua_State *L = g_L[p];
    if (!L) return;
    lua_getglobal(L, "DrawItemBox");
    pcallChecked(L, 0, 0, "DrawItemBox");
}

void BLua_ResetData(int p, int level)  { callVoid1(p, "ResetData", level); }
void BLua_SetJudgeGauge(int p, int j)  { callVoid1(p, "SetJudgeGauge", j); }
void BLua_SetJudge(int p, int j)       { callVoid1(p, "SetJudge", j); }
void BLua_OnTick(int p, int timeMs)    { callVoid1(p, "OnTick", timeMs); }
int  BLua_GetLifeGaugeMeter(int p)     { return (int)callNum0(p, "GetLifeGaugeMeter"); }
int  BLua_GetLifeGaugeStatus(int p)    { return (int)callNum0(p, "GetLifeGaugeStatus"); }
