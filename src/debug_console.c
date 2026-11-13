/* debug_console.c — Console de debug
 *
 * Reconstrução do subsistema Debug_Console* do PUMPY.EXE.
 *
 * Mapa de origem:
 *   Debug_PrintString          0x00402fa0
 *   Debug_ConsoleHistoryPush   0x00402d60
 *   Debug_ConsoleHistoryNext   0x00402dc0
 *   Debug_ConsoleHistoryPrev   0x00402e30
 *   Debug_ConsoleExecute       0x00402e80
 *   Debug_ConsoleDrawLine      0x00402fe0
 *   Debug_ConsoleRender        0x00403050
 *   Debug_GetConsoleMode       0x00403180
 *   Debug_ConsoleKeyHandler    0x00403190
 *   Debug_ConsoleCmdSet        0x004035d0
 *   help handler               0x004034c0  (sem símbolo no Ghidra)
 *   tabela de comandos         0x0043d080  (pares {nome, função}, terminada em NULL)
 *
 * Buffers do original, com os tamanhos preservados:
 *   entrada          0x00d36288, 80 bytes (limite de digitação: 0x4f = 79)
 *   cursor           0x00d35f70
 *   modo inserção    0x00d367dc   (1 = insere, 0 = sobrescreve)
 *   contador de pisca 0x00d367e8
 *   saída            0x00d35f78, 10 linhas de 0x4e = 78 bytes (linha 0 = mais recente)
 *   histórico        0x00d362d8, 9 entradas de 0x50 = 80 bytes
 *   argv             0x00d36558, 8 slots de 80 bytes
 *
 * Desvio deliberado: teclas.
 * O original recebe scancodes (0x0e Backspace, 0x1c Enter, 0x66 Home, 0x69 Left,
 * 0x6e Insert, 0x6f Delete, 0x29 para a tecla que abre/fecha). Este projeto usa
 * códigos VK do Windows, então o handler recebe VK e a tabela abaixo documenta a
 * correspondência. O comportamento de cada tecla é o mesmo.
 */

#include "pumpy.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include "movie.h"

#define DC_INPUT_MAX    0x4f   /* 79 — limite de digitação do original */
#define DC_OUT_LINES    10
#define DC_OUT_LEN      0x4e   /* 78 */
#define DC_HIST_MAX     9
#define DC_HIST_LEN     0x50   /* 80 */
#define DC_ARGV_MAX     8
#define DC_ARG_LEN      0x50

static char g_dcInput[DC_HIST_LEN];
static int  g_dcCursor;
static int  g_dcInsert = 1;
static int  g_dcBlink;
static int  g_dcActive;

static char g_dcOut[DC_OUT_LINES][DC_OUT_LEN];
static char g_dcHist[DC_HIST_MAX][DC_HIST_LEN];
static int  g_dcHistCount;
static int  g_dcHistIdx = -1;

static char g_dcArgv[DC_ARGV_MAX][DC_ARG_LEN];
static int  g_dcArgc;

/* ------------------------------------------- Debug_PrintString 0x00402fa0
 * Desloca as linhas para baixo (9 recebe 8, 8 recebe 7, ...) e escreve o texto
 * novo na linha 0. Ou seja, linha 0 é sempre a mais recente.
 */
void Debug_PrintString(const char* fmt, ...)
{
    va_list ap;
    int i;
    for (i = DC_OUT_LINES - 1; i > 0; i--)
        memcpy(g_dcOut[i], g_dcOut[i - 1], DC_OUT_LEN);
    va_start(ap, fmt);
    vsnprintf(g_dcOut[0], DC_OUT_LEN, fmt, ap);
    va_end(ap);
    g_dcOut[0][DC_OUT_LEN - 1] = '\0';
}

/* ------------------------------------- Debug_ConsoleHistoryPush 0x00402d60 */
static void Debug_ConsoleHistoryPush(const char* cmd)
{
    int i;
    for (i = DC_HIST_MAX - 2; i > 0; i--)
        memcpy(g_dcHist[i], g_dcHist[i - 1], DC_HIST_LEN);
    g_dcHistCount++;
    if (g_dcHistCount > 8) g_dcHistCount = 8;   /* teto do original */
    g_dcHistIdx = -1;
    strncpy(g_dcHist[0], cmd, DC_OUT_LEN);
    g_dcHist[0][DC_OUT_LEN] = '\0';
}

/* ------------------------------------- Debug_ConsoleHistoryPrev 0x00402e30
 * Avança para o comando mais antigo; passando do fim, volta ao índice 0.
 */
static void Debug_ConsoleHistoryPrev(void)
{
    g_dcHistIdx++;
    g_dcCursor = 0;
    if (g_dcHistIdx > g_dcHistCount || g_dcHistIdx > 8)
        g_dcHistIdx = 0;
    strncpy(g_dcInput, g_dcHist[g_dcHistIdx], DC_OUT_LEN);
    g_dcInput[DC_OUT_LEN] = '\0';
}

/* ------------------------------------- Debug_ConsoleHistoryNext 0x00402dc0
 * Recua para o comando mais recente; chegando em -1, limpa a linha.
 */
static void Debug_ConsoleHistoryNext(void)
{
    g_dcHistIdx--;
    g_dcCursor = 0;
    if (g_dcHistIdx < -1)
        g_dcHistIdx = (g_dcHistCount < 8) ? (g_dcHistCount - 1) : 7;
    if (g_dcHistIdx == -1) {
        memset(g_dcInput, 0, sizeof(g_dcInput));
        return;
    }
    strncpy(g_dcInput, g_dcHist[g_dcHistIdx], DC_OUT_LEN);
    g_dcInput[DC_OUT_LEN] = '\0';
}

/* ================================================== comandos =============== */

/* Help — 0x004034c0. Os textos são os do binário, na mesma ordem de impressão.
 * Como a linha 0 é a mais recente, o original imprime de cima para baixo e o
 * resultado na tela fica invertido; preservado como está. */
static void dcCmdHelp(void)
{
    Debug_PrintString("");
    Debug_PrintString("Command List -");
    Debug_PrintString(" /play step mode(-n, -h, -d, -c, -hd, -dv) [mp3name] - for step test");
    Debug_PrintString(" /autoplay - Automatic step push");
    Debug_PrintString(" /history - to get history");
    Debug_PrintString(" /credit - Freevolt Team");
    Debug_PrintString(" /help - to get help");
    Debug_PrintString(" /drawfps - toggle fps draw. (including uptime print)");
    Debug_PrintString(" /testmode - toggle testmode. [track] [mode] It will test all song. (loop)");
    Debug_PrintString("           - mode = -n, -h, -d, -hd, -dv, -c -> 0~5");
    Debug_PrintString(" /highscore (add (n, dv, hd, d) score name) - highscore management");
}

/* /history — 0x00403540. Formato "N: %s" do original (0x00433920 em diante). */
static void dcCmdHistory(void)
{
    int i;
    for (i = g_dcHistCount - 1; i >= 0; i--)
        Debug_PrintString("%d: %s", i, g_dcHist[i]);
}

/* /credit — 0x00403490 */
static void dcCmdCredit(void)
{
    Debug_PrintString(" /credit - Freevolt Team");
}

/* /drawfps — 0x00402d40 */
static void dcCmdDrawFps(void)
{
    g_game.showDebug = !g_game.showDebug;
    Debug_PrintString("drawfps: %s", g_game.showDebug ? "ON" : "OFF");
}

/* /set — Debug_ConsoleCmdSet 0x004035d0
 * Ajusta a HighSpeed de cada player. No original grava em DAT_00442720 (P1) e
 * DAT_00442724 (P2), que são os mesmos globais de velocidade usados no GameInit. */
static void dcCmdSet(void)
{
    if (g_dcArgc < 2) {
        Debug_PrintString("");
        Debug_PrintString("- Setting System Variable");
        Debug_PrintString("Usage : /set variable value");
        Debug_PrintString("        Variable - s1 -> 1p's HighSpeed");
        Debug_PrintString("                   s2 -> 2p's HighSpeed");
        Debug_PrintString("CurState : s1 = %d, s2 = %d",
                          g_game.cmdSpeedMult[0], g_game.cmdSpeedMult[1]);
        return;
    }
    if (g_dcArgc > 2) {
        long v = atol(g_dcArgv[2]);
        if (v > 0) {
            if (_stricmp("s1", g_dcArgv[1]) == 0) {
                g_game.cmdSpeedMult[0] = (int)v;
                Debug_PrintString("s1 = %ld", v);
            } else if (_stricmp("s2", g_dcArgv[1]) == 0) {
                g_game.cmdSpeedMult[1] = (int)v;
                Debug_PrintString("s2 = %ld", v);
            }
        }
    }
}

/* /autoplay — 0x00402d50 */
static void dcCmdAutoplay(void)
{
    Debug_PrintString("autoplay: use F8 durante a musica");
}

/* /credit do gabinete: acrescenta crédito de serviço (reaproveita coin.c) */
static void dcCmdAddCredit(void)
{
    Arcade_ProcessCoin(3);
    Debug_PrintString("creditos: %d", Coin_GetCredits());
}

/* /highscore — lista a tabela de recordes (ranking.c).
 * O help do original descreve "add (n, dv, hd, d) score name"; aqui só o
 * modo de listagem está implementado, que é o que dá para verificar. */
static void dcCmdHighscore(void)
{
    int i, n;
    if (g_dcArgc > 2 && _stricmp("add", g_dcArgv[1]) == 0 && g_dcArgc > 3) {
        Var_SetSystemVariable(atoi(g_dcArgv[2]), g_dcArgv[3]);
        Debug_PrintString("highscore: %s %s", g_dcArgv[3], g_dcArgv[2]);
        return;
    }
    n = Ranking_GetCount();
    if (n > DC_OUT_LINES - 1) n = DC_OUT_LINES - 1;  /* só cabem 10 linhas */
    for (i = n - 1; i >= 0; i--)
        Debug_PrintString("%2d. %-4s %7d", i + 1, Ranking_GetName(i), Ranking_GetScore(i));
}

/* ------------------------------------------------ Demo_ToggleMode 0x00407cb0
 * O original alterna entre dois modos montando "%d -n -demo" / "%d -h -demo" e
 * despachando "run %s %d" pelo próprio console — o comando `run` aponta para
 * GameInit, ou seja, ele reinicia o jogo com as flags de demo.
 *
 * Aqui só o alternar é reproduzido: o relançamento por flags de linha de
 * comando não tem equivalente neste motor, que não tem camada de argumentos.
 */
static int g_demoMode;

void Demo_ToggleMode(void)
{
    g_demoMode = !g_demoMode;
    Debug_PrintString("demo mode: %s", g_demoMode ? "-h -demo" : "-n -demo");
    Log_Print("Demo_ToggleMode: %s\n", g_demoMode ? "-h -demo" : "-n -demo");
}

static void dcCmdTestmode(void)
{
    Demo_ToggleMode();
}

/* run / /play — 0x00410cf0. "run <id> <modo>"; a confirmação do Song Select usa
 * este mesmo comando (0x4091a0..0x4092a2). Modos do original: -n -h -d -c -hd -dv -nm.
 * -dv (Division) escolhe a seção 7 do .STX — a interface do original não expõe
 * esse modo; o console é o único caminho no PUMPY.EXE. */
static void dcCmdRun(void)
{
    static const struct { const char* flag; const char* mode; } kModes[] = {
        { "-n", "EASY" }, { "-h", "HARD" }, { "-d", "DOUBLE" }, { "-c", "CRAZY" },
        { "-hd", "HALFDOUBLE" }, { "-dv", "DIVISION" }, { "-nm", "NIGHTMARE" },
    };
    if (g_dcArgc < 3) {
        Debug_PrintString("%s step mode(-n, -h, -d, -c, -hd, -dv, -nm)", g_dcArgv[0]);
        return;
    }
    int songId = atoi(g_dcArgv[1]);
    const char* modeName = NULL;
    for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++)
        if (_stricmp(g_dcArgv[2], kModes[i].flag) == 0) modeName = kModes[i].mode;
    if (!modeName || songId <= 0) {
        Debug_PrintString("run: modo ou musica invalidos");
        return;
    }
    int mi = Song_FindMode(&g_game.songDB, modeName);
    if (mi < 0) {
        Debug_PrintString("run: modo %s nao existe no Stage.cfg", modeName);
        return;
    }
    Debug_PrintString("run %d %s", songId, modeName);
    BGM_Stop();
    Menu_ResetState();
    g_game.selectedModeIndex = mi;
    if (Debug_ConsoleIsActive()) Debug_ConsoleToggle();
    Loading_Enter(songId);
}

/* Tabela de comandos — espelha 0x0043d080, terminada por nome NULL.
 * Os nomes e a ordem são os do binário. */
typedef struct { const char* name; void (*fn)(void); } DCCommand;

static const DCCommand g_dcCommands[] = {
    { "run",       dcCmdRun      },
    { "/play",     dcCmdRun      },
    { "/help",     dcCmdHelp     },
    { "/h",        dcCmdHelp     },
    { "-h",        dcCmdHelp     },
    { "--help",    dcCmdHelp     },
    { "/set",      dcCmdSet      },
    { "/history",  dcCmdHistory  },
    { "/autoplay", dcCmdAutoplay },
    { "/credit",   dcCmdCredit   },
    { "/drawfps",   dcCmdDrawFps   },
    { "/highscore", dcCmdHighscore },
    { "/testmode",  dcCmdTestmode  },
    { "/testmode2", dcCmdTestmode  },
    { "/coin",      dcCmdAddCredit }, /* extra deste projeto, não existe no original */
    { NULL, NULL }
};

/* ============================================ console do exceed.exe ========
 * Despachante em 0x41D0CC..0x41D5F8: cadeia de stricmp com os comandos abaixo,
 * nesta ordem, e "Unknown Command: %s" no fim. Não existe /help, /history etc.
 */
bool g_drawJudgeRange;              /* CPlayEngine +0x48D4A, começa FALSE (0x4016FB) */

/* Procs registrados em 0x40F880.. (0x410244), na ordem do registro */
/* era (exceed.exe):
static const char* const k_exProcs[] = {
    "IDLE", "RWARN", "LOGO", "TITLE", "PLAY", "RUN", "NEXTSTAGE", "GAMEOVER",
    "STAGEBREAK", "GRADE", "SELECT", "NAMEINPUT", "IR", "HIGHSCORE", "SETUP",
    "INTRO", "COKE", NULL
};
 * Arcade (piu, registro 0x8061600..0x8061B28): */
static const char* const k_exProcs[] = {
    "IDLE", "RWARN", "HELP", "LOGO", "TITLE", "PLAY", "RUN", "NEXTSTAGE", "GAMEOVER",
    "STAGEBREAK", "GRADE", "SELECT", "NAMEINPUT", "IR", "HIGHSCORE", "SETUP",
    "INTRO", "PLAYMOVIE", "STATION", "LUAPROC", NULL
};

/* Encerra o que estiver tocando antes de trocar de proc (o End() do proc atual) */
static void exLeaveCurrent(void)
{
    if (g_game.state == STATE_GAMEPLAY || g_game.state == STATE_GAME_INIT) Gameplay_Exit();
    g_exDemo = false;
    Movie_Close();
    BGM_Stop();
    Resource_ClearBGA();
}

/* PLAY / RUN (Begin em 0x401A0A): "<step> <modo> [-demo|-demo2]" */
static void exProcRun(const char* name, int argc, char argv[][DC_ARG_LEN])
{
    static const struct { const char* flag; int m; } kModes[] = {
        { "-n", 0 }, { "-h", 1 }, { "-c", 2 }, { "-d", 3 }, { "-nm", 4 }, { "-bt", 5 },
    };
    if (argc < 3) {
        Debug_PrintString("/%s: insufficient parameters!", name);
        Debug_PrintString("Usage: /play step mode (-n, -h, -d, -c, -hd, -dv, -nm) [mp3name]");
        Attract_Idle();
        return;
    }
    if (_stricmp(argv[2], "-hd") == 0) {
        Debug_PrintString("/%s: Half-double mode is not implemented.", name);
        Attract_Idle();
        return;
    }
    int m = -1;
    for (size_t i = 0; i < sizeof(kModes) / sizeof(kModes[0]); i++)
        if (_stricmp(argv[2], kModes[i].flag) == 0) m = kModes[i].m;
    int demo = 0;
    if (argc > 3) {
        if (_stricmp(argv[3], "-demo") == 0) demo = 1;
        else if (_stricmp(argv[3], "-demo2") == 0) demo = 2;
    }
    int id = (int)strtol(argv[1], NULL, 16);   /* nomes de arquivo por "%X" */
    /* -dv (Division) não existe no banco de músicas do Exceed */
    if (m < 0 || !ExSelect_Run(id, m, demo)) {
        Debug_PrintString("/%s: Incorrect parameters!", name);
        Attract_Idle();
        return;
    }
    if (Debug_ConsoleIsActive()) Debug_ConsoleToggle();
}

/* SetCurProc(nome, parâmetros) do original, mapeado para os estados do projeto */
static void exSetCurProc(int argc, char argv[][DC_ARG_LEN])
{
    const char* p = argv[0];
    if (*p == '/') p++;             /* "/play ..." chega como "play ..." (0x41D2FA) */
    if (_stricmp(p, "PLAY") == 0 || _stricmp(p, "RUN") == 0) {
        exLeaveCurrent();
        exProcRun(p, argc, argv);
        return;
    }
    exLeaveCurrent();
    if      (_stricmp(p, "IDLE") == 0)       Attract_Idle();
    else if (_stricmp(p, "RWARN") == 0)      Game_ChangeState(STATE_WARNING_INIT);
    else if (_stricmp(p, "LOGO") == 0)       Game_ChangeState(STATE_LOGO_ENTER);
    else if (_stricmp(p, "TITLE") == 0)      Game_ChangeState(STATE_CREDIT);
    else if (_stricmp(p, "INTRO") == 0)      Attract_Idle_Intro();
    else if (_stricmp(p, "PLAYMOVIE") == 0) {
        /* 0x8050844: "PLAYMOVIE <nome>"; sem nome -> "insufficient parameters!" e IDLE */
        if (argc < 2) { Debug_PrintString("/%s: insufficient parameters!", p); Attract_Idle(); }
        else if (!Attract_PlayMovie(argv[1])) { Debug_PrintString("/%s: incorrect parameters!", p); Attract_Idle(); }
    }
    else if (_stricmp(p, "STATION") == 0)    Game_ChangeState(STATE_STATION);
    else if (_stricmp(p, "HELP") == 0)       Help_Enter(argc > 1);   /* "HELP 1" = BATTLE */
    else if (_stricmp(p, "HIGHSCORE") == 0)  Game_ChangeState(STATE_HIGHSCORE);
    else if (_stricmp(p, "SELECT") == 0)     Game_ChangeState(STATE_EXSELECT);
    else if (_stricmp(p, "NAMEINPUT") == 0)  Game_ChangeState(STATE_NAMEINPUT);
    else if (_stricmp(p, "IR") == 0)         Game_ChangeState(STATE_IR);
    else if (_stricmp(p, "GAMEOVER") == 0)   Game_ChangeState(STATE_GAMEOVER_ENTER);
    else if (_stricmp(p, "STAGEBREAK") == 0) Game_ChangeState(STATE_STAGE_BREAK);
    else if (_stricmp(p, "GRADE") == 0)      Game_ChangeState(STATE_DANCE_GRADE_ENTER);
    else if (_stricmp(p, "SETUP") == 0)      ServiceMenu_Enter();
    else if (_stricmp(p, "NEXTSTAGE") == 0) {
        /* "/runproc nextstage stagenum": NST<n>.MOV, n = 1..3 (main.c: n = 3 - stageCount) */
        int n = (argc > 1) ? atoi(argv[1]) : 1;
        if (n < 1) n = 1;
        if (n > 3) n = 3;
        g_game.stageCount = 3 - n;
        Game_ChangeState(STATE_STAGE_TRANSITION);
    }
    else if (_stricmp(p, "COKE") == 0) {
        /* CPlayAd (0x4133E0): "COKE 1"/"COKE 2" toca BGA\COKE<n>.MOV + AUDIO\COKE<n>.AUD
         * e volta para IDLE. Os arquivos não existem nesta instalação: vai direto. */
        if (argc < 2) Debug_PrintString("/%s: insufficient parameters!", p);
        Log_Print("COKE: propaganda sem arquivos, indo para IDLE\n");
        Attract_Idle();
    }
}

static void exCmdCredits(void)      /* 0x41D67C */
{
    Debug_PrintString("%s %s", "PUMP IT UP - THE EXCEED", "V1");
    Debug_PrintString("     Team Manager: kann");
}

static void exCmdDrawFps(void)      /* 0x41D0F0: [0xA69044] */
{
    g_game.showDebug = !g_game.showDebug;
    Debug_PrintString("DrawFPS Settings Changed to: %s", g_game.showDebug ? "TRUE" : "FALSE");
}

static void exCmdDrawJudgeRange(void)   /* 0x41D13C */
{
    g_drawJudgeRange = !g_drawJudgeRange;
    Debug_PrintString("DrawJudgeRange Settings Changed to: %s", g_drawJudgeRange ? "TRUE" : "FALSE");
}

static void exCmdProcList(void)     /* 0x41D1B9 */
{
    char buf[256] = "";
    for (int i = 0; k_exProcs[i]; i++) {
        strcat(buf, k_exProcs[i]);
        strcat(buf, " ");
    }
    Debug_PrintString("Proc List: %s", buf);
}

static void exCmdRunProc(void)      /* 0x41D26A */
{
    if (g_dcArgc < 2) {
        Debug_PrintString("%s: insufficient parameters", g_dcArgv[0]);
        return;
    }
    int found = 0;
    for (int i = 0; k_exProcs[i]; i++)
        if (_stricmp(g_dcArgv[1], k_exProcs[i]) == 0) found = 1;
    if (!found) {
        Debug_PrintString("%s: proc not found.", g_dcArgv[0]);
        return;
    }
    exSetCurProc(g_dcArgc - 1, &g_dcArgv[1]);
}

static void exCmdPlay(void)         /* 0x41D2E2: SetCurProc("play ...") */
{
    exSetCurProc(g_dcArgc, g_dcArgv);   /* argv[0] = "/play" -> "play" */
}

static void exCmdSet(void)          /* 0x41D309: câmera da roda de banners da Select */
{
    static const struct { const char* name; float* v; } kVars[] = {
        { "x1", &g_exCamX1 }, { "x2", &g_exCamX2 }, { "y1", &g_exCamY1 }, { "y2", &g_exCamY2 },
        { "z1", &g_exCamZ1 }, { "r", &g_exCamR }, { "r2", &g_exCamR2 }, { "r3", &g_exCamR3 },
        { "tx", &g_exCamTx }, { "ty", &g_exCamTy }, { "p", &g_exCamP }, { "cy", &g_exCamCy },
        { "cy2", &g_exCamCy2 },
    };
    for (size_t i = 0; i < sizeof(kVars) / sizeof(kVars[0]); i++) {
        if (_stricmp(g_dcArgv[1], kVars[i].name) == 0) {
            *kVars[i].v = (float)atof(g_dcArgv[2]);
            break;
        }
    }
    Debug_PrintString("%g %g %g %g z: %g rot: %g %g %g tx: %g ty: %g p: %g cy: %g %g",
                      g_exCamX1, g_exCamX2, g_exCamY1, g_exCamY2, g_exCamZ1, g_exCamR,
                      g_exCamR2, g_exCamR3, g_exCamTx, g_exCamTy, g_exCamP, g_exCamCy, g_exCamCy2);
}

static const DCCommand g_exCommands[] = {
    { "/credits",        exCmdCredits        },
    { "/drawfps",        exCmdDrawFps        },
    { "/drawjudgerange", exCmdDrawJudgeRange },
    { "/proclist",       exCmdProcList       },
    { "/runproc",        exCmdRunProc        },
    { "/play",           exCmdPlay           },
    { "/set",            exCmdSet            },
    { NULL, NULL }
};

/* ----------------------------------------- Debug_ConsoleExecute 0x00402e80
 * Tokeniza por espaço e '\n' (delimitadores em 0x0043d078), guarda até 8
 * argumentos e procura argv[0] na tabela com stricmp.
 */
void Debug_ConsoleExecute(const char* cmd)
{
    char work[DC_HIST_LEN];
    char* tok;
    int i;

    if (!cmd || !*cmd) return;

    g_dcArgc = 0;
    memset(g_dcArgv, 0, sizeof(g_dcArgv));

    strncpy(g_dcInput, cmd, DC_OUT_LEN);
    g_dcInput[DC_OUT_LEN] = '\0';
    Debug_ConsoleHistoryPush(g_dcInput);

    strncpy(work, g_dcInput, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    tok = strtok(work, " \n");
    while (tok && g_dcArgc < DC_ARGV_MAX) {
        strncpy(g_dcArgv[g_dcArgc], tok, DC_ARG_LEN - 1);
        g_dcArgv[g_dcArgc][DC_ARG_LEN - 1] = '\0';
        g_dcArgc++;
        tok = strtok(NULL, " \n");
    }

    if (g_dcArgc > 0) {
        const DCCommand* cmds = g_exceedSongIds ? g_exCommands : g_dcCommands;
        for (i = 0; cmds[i].name; i++) {
            if (_stricmp(g_dcArgv[0], cmds[i].name) == 0) {
                cmds[i].fn();
                break;
            }
        }
        if (!cmds[i].name)
            Debug_PrintString(g_exceedSongIds ? "Unknown Command: %s" : "comando desconhecido: %s",
                              g_dcArgv[0]);
    }

    memset(g_dcInput, 0, sizeof(g_dcInput));
    g_dcCursor = 0;
}

/* --------------------------------------- Debug_ConsoleKeyHandler 0x00403190
 * Recebe VK em vez do scancode do original. Correspondência:
 *   original 0x0e Backspace | 0x1c/0x60 Enter | 0x66 Home | 0x67 PgUp
 *            0x69 Left      | 0x6a Right      | 0x6b End  | 0x6c PgDn
 *            0x6e Insert    | 0x6f Delete     | 0x29 tecla que abre/fecha
 */
void Debug_ConsoleKeyHandler(int ch, int vk)
{
    int len = (int)strlen(g_dcInput);

    /* Caractere imprimível: insere ou sobrescreve na posição do cursor */
    if (ch >= 0x20 && ch < 0x7f && vk != VK_OEM_3 && len < DC_INPUT_MAX) {
        if (g_dcInsert) {
            int i;
            for (i = len; i > g_dcCursor; i--)
                g_dcInput[i] = g_dcInput[i - 1];
            g_dcInput[len + 1] = '\0';
        } else if (g_dcCursor >= len) {
            g_dcInput[g_dcCursor + 1] = '\0';
        }
        g_dcInput[g_dcCursor] = (char)ch;
        g_dcCursor++;
        return;
    }

    switch (vk) {
    case VK_BACK:
        if (g_dcCursor > 0) {
            memmove(&g_dcInput[g_dcCursor - 1], &g_dcInput[g_dcCursor],
                    (size_t)(len - g_dcCursor) + 1);
            g_dcCursor--;
        }
        break;
    case VK_RETURN:
        Debug_ConsoleExecute(g_dcInput);
        break;
    case VK_HOME:
        g_dcCursor = 0;
        break;
    case VK_END:
        g_dcCursor = len;
        break;
    case VK_LEFT:
        if (g_dcCursor > 0) g_dcCursor--;
        break;
    case VK_RIGHT:
        if (g_dcCursor < len && g_dcCursor < DC_INPUT_MAX) g_dcCursor++;
        break;
    case VK_PRIOR:
        Debug_ConsoleHistoryPrev();
        break;
    case VK_NEXT:
        Debug_ConsoleHistoryNext();
        break;
    case VK_INSERT:
        g_dcInsert = !g_dcInsert;
        break;
    case VK_DELETE:
        if (g_dcCursor < len)
            memmove(&g_dcInput[g_dcCursor], &g_dcInput[g_dcCursor + 1],
                    (size_t)(len - g_dcCursor));
        break;
    default:
        break;
    }
}

/* ------------------------------------------ Debug_ConsoleDrawLine 0x00402fe0
 * Cada linha sai duas vezes: sombra preta 1px deslocada e o texto em branco.
 */
static void Debug_ConsoleDrawLine(int line)
{
    if (line < 0) line = 0;
    if (line > DC_OUT_LINES - 1) line = DC_OUT_LINES - 1;
    Font_DrawString(1, line * 16 + 319, g_dcOut[line], 0.0f, 0.0f, 0.0f, 1.0f);
    Font_DrawString(0, line * 16 + 320, g_dcOut[line], 1.0f, 1.0f, 1.0f, 1.0f);
}

/* --------------------------------------------- Debug_ConsoleRender 0x00403050
 * Overlay preto a 40% cobrindo y=304..480, 10 linhas de histórico acima da
 * linha de entrada, e cursor piscando a cada 30 frames.
 */
void Debug_ConsoleRender(void)
{
    int i;
    if (!g_dcActive) return;

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(0.0f, 0.0f, 0.0f, 0.4f);
    glBegin(GL_QUADS);
    glVertex2f(0.0f,   304.0f);
    glVertex2f(640.0f, 304.0f);
    glVertex2f(640.0f, 480.0f);
    glVertex2f(0.0f,   480.0f);
    glEnd();
    glEnable(GL_TEXTURE_2D);

    for (i = 0; i < DC_OUT_LINES; i++)
        Debug_ConsoleDrawLine(i);

    Font_DrawString(1, 303, ">", 0.0f, 0.0f, 0.0f, 1.0f);
    Font_DrawString(0, 304, ">", 1.0f, 1.0f, 1.0f, 1.0f);
    Font_DrawString(16, 304, g_dcInput, 1.0f, 1.0f, 1.0f, 1.0f);

    /* Pisca: visível na primeira metade de cada ciclo de 30 frames.
     * Glifo '_' no modo inserção, bloco 0x7f no modo sobrescrita. */
    if ((g_dcBlink % 30) / 15 == 0) {
        int cx = g_dcCursor * 8 + 16;
        if (g_dcInsert) {
            Font_DrawChar(cx, 304, '_', 1.0f, 1.0f, 1.0f, 1.0f);
            Font_DrawChar(cx, 305, '_', 1.0f, 1.0f, 1.0f, 1.0f);
        } else {
            Font_DrawChar(cx, 304, 0x7f, 1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    g_dcBlink++;
}

/* --------------------------------------------- Debug_GetConsoleMode 0x00403180 */
int Debug_GetConsoleMode(void)
{
    return g_dcActive;
}

/* ------------------------------------------------------------------ extras -*/

void Debug_ConsoleToggle(void)
{
    g_dcActive = !g_dcActive;
    if (g_dcActive) {
        /* Font_DrawString depende dos display lists GDI, que Resource_ClearBGA
         * derruba junto com as texturas. Font_Init() retorna cedo se já existirem. */
        Font_Init();
        g_dcCursor = 0;
        g_dcHistIdx = -1;
        memset(g_dcInput, 0, sizeof(g_dcInput));
    }
    Log_Print("DebugConsole: %s\n", g_dcActive ? "aberto" : "fechado");
}

bool Debug_ConsoleIsActive(void)
{
    return g_dcActive != 0;
}
