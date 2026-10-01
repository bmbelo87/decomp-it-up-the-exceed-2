/* service_menu.c — Menu de serviço (SETUP MENU)
 *
 * Reconstrução das funções ServiceMenu_* do PUMPY.EXE.
 *
 * Mapa de origem (endereços do original):
 *   ServiceMenu_Enter                  0x00404ee0
 *   ServiceMenu_Exit                   0x004066d0
 *   ServiceMenu_UpdateRender           0x004066e0
 *   ServiceMenu_RenderMain             0x004052e0
 *   ServiceMenu_RenderFooter           0x004051e0
 *   ServiceMenu_RenderGradientBar      0x00404d40
 *   ServiceMenu_RenderEEPROMOverlay    0x00404e00
 *   ServiceMenu_RenderIOTest           0x004054c0
 *   ServiceMenu_RenderEEPROMTest       0x004056d0
 *   ServiceMenu_RenderScreenTest       0x00405840
 *   ServiceMenu_RenderGameOption       0x00405910
 *   ServiceMenu_RenderCoinOption       0x00405d80
 *   ServiceMenu_RenderSoundTest        0x00406030
 *   ServiceMenu_RenderClearBookkeeping 0x00406210
 *   ServiceMenu_RenderBookkeeping      0x00406360
 *   ServiceMenu_RenderStatistics       0x004064e0
 *   Render_DrawGrid                    0x00403950
 *
 * Convenção de coordenadas: o original projeta em Y-UP (glOrtho 0..480 de baixo
 * pra cima), igual ao Render_SetOrtho daqui. Portanto todas as coordenadas
 * abaixo são as do binário, sem conversão.
 *
 * Botões: o original lê dois bits de input —
 *   0x10000 = TEST BUTTON    (MOVE   / percorre)
 *   0x20000 = SERVICE BUTTON (SELECT / confirma)
 *   0x40000 = CLEAR BUTTON, 0x100000 = COIN1, 0x200000 = COIN2 (só no I/O TEST)
 * No PC seguimos a mesma ordem da botoeira listada pelo I/O TEST:
 *   F1 = TEST | F2 = SERVICE | F3 = CLEAR | F4 = COIN1 | F5 = COIN2
 * O F1 abre o menu de qualquer tela e, já dentro dele, percorre a lista (MOVE);
 * o F2 confirma a opção selecionada (SELECT). Como o mesmo F1 que abre também
 * seria lido como MOVE no primeiro frame, g_svcSkipInput engole esse press.
 *
 * Desvios deliberados em relação ao original (documentados):
 *   1. O original desenha literalmente as format strings ("%s", "%d STAGE",
 *      "1 CREDITS / %d COIN", "SERVICE : %d", "%02d. %-24s [%04d]") sem passar
 *      argumento nenhum — código inacabado no binário. Aqui elas são formatadas
 *      com o valor real, que é a intenção evidente.
 *   2. I/O TEST e EEPROM TEST são adaptados ao PC: o I/O TEST reflete o estado
 *      real do teclado e o EEPROM TEST grava/lê o arquivo de configuração local,
 *      que faz o papel da EEPROM do gabinete.
 *   3. Textos coreanos foram omitidos. ServiceMenu_Enter força g_nLanguage = 1
 *      (inglês) no original, então os ramos coreanos são inalcançáveis na prática.
 */

#include "pumpy.h"
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- cores ---
 * Extraídas de PUMPY.EXE. A paleta de 0x442270 a 0x4422b8 tem 7 entradas e é
 * usada tanto pelas barras do SCREEN TEST quanto como cor de destaque.
 */
static const float SVC_HIGHLIGHT[3]  = { 1.0f, 0.0f, 0.0f }; /* 0x442270 */
static const float SVC_EEPROMDATA[3] = { 1.0f, 1.0f, 0.0f }; /* 0x44227c */
static const float SVC_SUCCESS[3]    = { 0.0f, 1.0f, 0.0f }; /* 0x442288 */
static const float SVC_NORMAL[3]     = { 1.0f, 1.0f, 1.0f }; /* 0x4422b8 */
static const float SVC_SCREENTEST[3] = { 0.0f, 0.0f, 0.0f }; /* 0x4422c4 */

static const float SVC_PALETTE[7][3] = {
    { 1.0f, 0.0f, 0.0f },  /* 0x442270 vermelho  */
    { 1.0f, 1.0f, 0.0f },  /* 0x44227c amarelo   */
    { 0.0f, 1.0f, 0.0f },  /* 0x442288 verde     */
    { 0.0f, 1.0f, 1.0f },  /* 0x442294 ciano     */
    { 0.0f, 0.0f, 1.0f },  /* 0x4422a0 azul      */
    { 1.0f, 0.0f, 1.0f },  /* 0x4422ac magenta   */
    { 1.0f, 1.0f, 1.0f },  /* 0x4422b8 branco    */
};

/* ------------------------------------------------------------- input bits -*/
#define SVC_BIT_TEST     0x10000   /* MOVE   */
#define SVC_BIT_SERVICE  0x20000   /* SELECT */
#define SVC_BIT_CLEAR    0x40000
#define SVC_BIT_COIN1    0x100000
#define SVC_BIT_COIN2    0x200000

/* ---------------------------------------------------------------- estado --
 * Equivalentes dos globais do original:
 *   g_svcPage      <- g_nServiceMenuPage   @0x00d387f8
 *   g_svcOption    <- g_nServiceMenuOption
 *   g_svcCursor    <- g_nServiceMenuCursor
 *   g_svcSubCursor <- g_nSubMenuCursor
 *   g_svcAudioIdx  <- DAT_004422d4 (init -1)
 *   g_svcSeIdx     <- DAT_004422d0 (init -1)
 */
/* GRAPHICS SETTINGS (extra do port): 10 itens no SETUP MENU; página 11. */
#define SVC_MAIN_COUNT 10
#define SVC_PAGE_GRAPHICS 11
static int  g_svcPage;
static int  g_svcOption;
static int  g_svcCursor;
static int  g_svcSubCursor;
static int  g_svcEepromDone;
static int  g_svcEepromResult;
static int  g_svcAudioIdx  = -1;
static int  g_svcSeIdx     = -1;
static int  g_svcSkipInput;          /* engole o F2 que abriu o menu */
static char g_svcEepromBuf[64];

/* Padrão gravado/lido pelo EEPROM TEST — PUMPY.EXE 0x4422e4 */
static const char SVC_EEPROM_PATTERN[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123";

/* --------------------------------------------------------------- helpers -*/

/* Snapshot do input do frame, capturado em ServiceMenu_Update (fase de update)
 * e consumido pelas páginas durante o render. Ver o comentário em
 * ServiceMenu_Update para o motivo. */
static uint32_t g_svcHitBits;
static uint32_t g_svcHeldBits;

static uint32_t svcBitsHit(void)  { return g_svcHitBits;  }
static uint32_t svcBitsHeld(void) { return g_svcHeldBits; }

static void svcText(float x, float y, const char* s)
{
    Font_DrawText(x, y, s);
}

static void svcColor(const float* c)
{
    glColor3fv(c);
}

/* Aplica highlight quando o índice bate com o cursor (padrão do original) */
static void svcColorFor(int idx, int cursor)
{
    glColor3fv(idx == cursor ? SVC_HIGHLIGHT : SVC_NORMAL);
}

/* ------------------------------------------------- Render_DrawGrid 0x403950
 * Malha de 16px + moldura de 2px. Usado pelo SCREEN TEST para conferir
 * alinhamento e overscan do monitor.
 */
static void svcDrawGrid(void)
{
    int i;
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    glBegin(GL_LINES);
    for (i = 0; i < 640; i += 16) {          /* verticais   */
        glVertex2f((float)i, 0.0f);
        glVertex2f((float)i, 480.0f);
    }
    for (i = 0; i < 480; i += 16) {          /* horizontais */
        glVertex2f(0.0f,   (float)i);
        glVertex2f(640.0f, (float)i);
    }
    /* moldura — linhas duplas nas quatro bordas */
    glVertex2f(0.0f, 0.0f);     glVertex2f(639.0f, 0.0f);
    glVertex2f(0.0f, 1.0f);     glVertex2f(639.0f, 1.0f);
    glVertex2f(0.0f, 478.0f);   glVertex2f(640.0f, 478.0f);
    glVertex2f(0.0f, 479.0f);   glVertex2f(640.0f, 479.0f);
    glVertex2f(0.0f, 0.0f);     glVertex2f(0.0f,   479.0f);
    glVertex2f(1.0f, 0.0f);     glVertex2f(1.0f,   479.0f);
    glVertex2f(638.0f, 0.0f);   glVertex2f(638.0f, 479.0f);
    glVertex2f(639.0f, 0.0f);   glVertex2f(639.0f, 479.0f);
    glEnd();

    glEnable(GL_TEXTURE_2D);
}

/* ------------------------------------- ServiceMenu_RenderGradientBar 0x404d40
 * 7 barras de 256x32 a partir de (64, 384), descendo 32 a cada uma. Cada barra
 * é um degradê do preto (esquerda) até a cor da paleta (direita).
 */
static void svcRenderGradientBar(void)
{
    int i;
    glShadeModel(GL_SMOOTH);
    glDisable(GL_TEXTURE_2D);
    glPushMatrix();
    glTranslatef(64.0f, 384.0f, 0.0f);
    for (i = 0; i < 7; i++) {
        glBegin(GL_QUADS);
        glColor3fv(SVC_SCREENTEST);
        glVertex2i(0, 0);
        glVertex2i(0, 32);
        glColor3fv(SVC_PALETTE[i]);
        glVertex2i(256, 32);
        glVertex2i(256, 0);
        glEnd();
        glTranslatef(0.0f, -32.0f, 0.0f);
    }
    glPopMatrix();
    glEnable(GL_TEXTURE_2D);
    glShadeModel(GL_FLAT);
}

/* ---------------------------------- ServiceMenu_RenderEEPROMOverlay 0x404e00
 * Hexágono de cores (TRIANGLE_FAN) centrado em (480, 304), raio ~112.
 * Apesar do nome no original, é parte do teste visual de cor.
 */
static void svcRenderEEPROMOverlay(void)
{
    glShadeModel(GL_SMOOTH);
    glDisable(GL_TEXTURE_2D);
    glPushMatrix();
    glTranslatef(480.0f, 304.0f, 0.0f);
    glBegin(GL_TRIANGLE_FAN);
    glColor3fv(SVC_NORMAL);       glVertex2i(  0,    0);
    glColor3fv(SVC_PALETTE[0]);   glVertex2i(  0,  112);
    glColor3fv(SVC_PALETTE[1]);   glVertex2i( 96,   48);
    glColor3fv(SVC_PALETTE[2]);   glVertex2i( 96,  -48);
    glColor3fv(SVC_PALETTE[3]);   glVertex2i(  0, -112);
    glColor3fv(SVC_PALETTE[4]);   glVertex2i(-96,  -48);
    glColor3fv(SVC_PALETTE[5]);   glVertex2i(-96,   48);
    glColor3fv(SVC_PALETTE[0]);   glVertex2i(  0,  112);
    glEnd();
    glPopMatrix();
    glEnable(GL_TEXTURE_2D);
    glShadeModel(GL_FLAT);
}

/* -------------------------------------- ServiceMenu_RenderFooter 0x004051e0 */
static void svcRenderFooter(void)
{
    char buf[64];
    svcColor(SVC_NORMAL);

    sprintf(buf, "PUMP IT UP (PREX 3 / %d)", 3);
    svcText(0.0f, 16.0f, buf);
    svcText(0.0f,  0.0f, "1999-2003 ANDAMIRO CO., LTD.");

    switch (g_svcPage) {
    case 0: case 4: case 5: case 6: case 7: case 10: case SVC_PAGE_GRAPHICS:
        svcText(236.0f, 36.0f, "MOVE   - TEST    BUTTON");
        svcText(236.0f, 16.0f, "SELECT - SERVICE BUTTON");
        break;
    case 1: case 3:
        svcText(236.0f, 16.0f, "EXIT   - SERVICE BUTTON");
        break;
    case 8:
        svcText(236.0f, 36.0f, "MOVE PAGE - TEST    BUTTON");
        svcText(236.0f, 16.0f, "EXIT      - SERVICE BUTTON");
        break;
    default:
        break;   /* página 2 (EEPROM TEST) não desenha rodapé de botões */
    }
}

/* ---------------------------------------- ServiceMenu_RenderMain 0x004052e0 */
/* static const char* SVC_MAIN_ITEMS[9] = {
    "I/O TEST", "EEPROM TEST", "SCREEN TEST", "GAME OPTION", "COIN OPTION",
    "SOUND TEST", "BOOKEEPING", "STATISTICS", "EXIT"
}; */
/* "BOOKEEPING" com um K só — typo presente no binário original, preservado.
 * "GRAPHICS SETTINGS" é extra deste port (não existe no original): página 11,
 * porque a página 9 é o EXIT. */
static const char* SVC_MAIN_ITEMS[SVC_MAIN_COUNT] = {
    "I/O TEST", "EEPROM TEST", "SCREEN TEST", "GAME OPTION", "COIN OPTION",
    "SOUND TEST", "BOOKEEPING", "STATISTICS", "GRAPHICS SETTINGS", "EXIT"
};

static void svcRenderMain(void)
{
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "SETUP MENU");

    /* itens a partir de Y=352 descendo 20 */
    for (i = 0; i < SVC_MAIN_COUNT; i++) {
        svcColorFor(i, g_svcOption);
        svcText(276.0f, (float)(352 - i * 20), SVC_MAIN_ITEMS[i]);
    }

    if (hit & SVC_BIT_TEST) {
        g_svcOption++;
        if (g_svcOption > SVC_MAIN_COUNT - 1) g_svcOption = 0;
    }
    if (hit & SVC_BIT_SERVICE) {
        /* 0..7 -> páginas 1..8 (original); 8 -> GRAPHICS (11); 9 -> EXIT (9) */
        if (g_svcOption == 8)      g_svcPage = SVC_PAGE_GRAPHICS;
        else if (g_svcOption == 9) g_svcPage = 9;
        else                       g_svcPage = g_svcOption + 1;
        g_svcCursor     = 0;
        g_svcEepromDone = 0;
    }

    svcColor(SVC_NORMAL);
    svcText(276.0f, 152.0f, "Lock OK = 0 Err = 0");
}

/* -------------------------------------- ServiceMenu_RenderIOTest 0x004054c0 */
static void svcRenderIOTest(void)
{
    static const struct { uint32_t bit; float y; const char* label; } lines[5] = {
        { SVC_BIT_TEST,    400.0f, "1. TEST BUTTON"    },
        { SVC_BIT_SERVICE, 384.0f, "2. SERVICE BUTTON" },
        { SVC_BIT_CLEAR,   368.0f, "3. CLEAR BUTTON"   },
        { SVC_BIT_COIN1,   352.0f, "4. COIN1 "         },
        { SVC_BIT_COIN2,   336.0f, "5. COIN2 "         },
    };
    char buf[64];
    int i;
    uint32_t held = svcBitsHeld();
    uint32_t hit  = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "I/O TEST MENU");

    for (i = 0; i < 5; i++) {
        int on = ((held | hit) & lines[i].bit) != 0;
        svcColor(on ? SVC_HIGHLIGHT : SVC_NORMAL);
        sprintf(buf, "%s : %s", lines[i].label, on ? "ON" : "OFF");
        svcText(260.0f, lines[i].y, buf);
    }

    if (hit & SVC_BIT_SERVICE) g_svcPage = 0;
}

/* ---------------------------------- ServiceMenu_RenderEEPROMTest 0x004056d0
 * Adaptação: a EEPROM do gabinete é substituída pelo arquivo de configuração
 * local. Grava o padrão, lê de volta e compara.
 */
static void svcRenderEEPROMTest(void)
{
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "EEPROM TEST");

    if (!g_svcEepromDone) {
        FILE* f = fopen("eeprom.dat", "wb");
        memset(g_svcEepromBuf, 0, sizeof(g_svcEepromBuf));
        if (f) {
            fwrite(SVC_EEPROM_PATTERN, 1, strlen(SVC_EEPROM_PATTERN), f);
            fclose(f);
            f = fopen("eeprom.dat", "rb");
            if (f) {
                size_t n = fread(g_svcEepromBuf, 1, sizeof(g_svcEepromBuf) - 1, f);
                g_svcEepromBuf[n] = '\0';
                fclose(f);
            }
        }
        g_svcEepromResult = (strcmp(g_svcEepromBuf, SVC_EEPROM_PATTERN) == 0);
        g_svcEepromDone   = 1;
        Log_Print("ServiceMenu: EEPROM test result=%d\n", g_svcEepromResult);
    }

    svcText(120.0f, 340.0f, "WRITE  ...");
    svcText(120.0f, 300.0f, "READ   ...");
    svcText(120.0f, 260.0f, "RESULT ...");

    svcColor(SVC_EEPROMDATA);
    svcText(220.0f, 340.0f, SVC_EEPROM_PATTERN);
    svcText(220.0f, 300.0f, g_svcEepromBuf);

    if (g_svcEepromResult) {
        svcColor(SVC_SUCCESS);
        svcText(220.0f, 260.0f, "SUCCESS");
    } else {
        svcColor(SVC_HIGHLIGHT);
        svcText(220.0f, 260.0f, "FAIL");
    }

    if (hit & SVC_BIT_SERVICE) g_svcPage = 0;
}

/* ---------------------------------- ServiceMenu_RenderScreenTest 0x00405840 */
static void svcRenderScreenTest(void)
{
    uint32_t hit = svcBitsHit();

    svcDrawGrid();
    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "SCREEN TEST");
    svcRenderGradientBar();
    svcRenderEEPROMOverlay();

    if (hit & SVC_BIT_SERVICE) g_svcPage = 0;
}

/* ---------------------------------- ServiceMenu_RenderGameOption 0x00405910 */
static const char* SVC_GAMEOPT_ITEMS[9] = {
    "GAME MODE", "LEVEL", "STAGE BREAK", "LANGUAGE", "DEMO SOUND",
    "SHOW HELP", "DEFAULT SETTING", "SAVE AND EXIT", "EXIT"
};
static const char* SVC_LEVEL_NAMES[3]  = { "1. EASY", "2. NORMAL", "3. HARD" };
static const char* SVC_LANG_NAMES[4]   = { "KOREAN", "ENGLISH", "PORTUGUESE", "SPANISH" };

/* Espelha os defaults de GameOption_Load (game_option.c) */
static void svcGameOptionReset(void)
{
    g_game.optionDifficulty = 1;    /* NORMAL          */
    g_game.optionToggle1    = 1;    /* STAGE BREAK on  */
    g_game.optionToggle2    = 0;    /* SHOW HELP off   */
    g_game.svcGameMode      = 0;    /* NORMAL          */
    g_game.svcDemoSound     = 0;
    g_game.svcLangOption    = 1;    /* ENGLISH         */
}

static void svcRenderGameOption(void)
{
    char buf[64];
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "GAME OPTION");

    for (i = 0; i < 9; i++) {
        float y = (float)(352 - i * 20);
        svcColorFor(i, g_svcCursor);
        svcText(196.0f, y, SVC_GAMEOPT_ITEMS[i]);

        switch (i) {
        case 0:  /* GAME MODE — verde quando NORMAL */
            if (g_game.svcGameMode == 0) {
                svcColor(SVC_SUCCESS);
                svcText(404.0f, y, "NORMAL");
            } else {
                svcText(404.0f, y, "EVENT");
            }
            break;
        case 1:  /* LEVEL — verde quando NORMAL (1) */
            if (g_game.optionDifficulty == 1) svcColor(SVC_SUCCESS);
            svcText(404.0f, y, SVC_LEVEL_NAMES[g_game.optionDifficulty % 3]);
            break;
        case 2:  /* STAGE BREAK — 0=OFF, 1..4 = "%d STAGE"; verde quando 2
                  * Usa optionToggle1, que é o campo realmente lido pelo jogo
                  * (gameplay.c e game_option.c). Os leitores tratam como
                  * booleano, e 1..4 continuam sendo "ligado" para eles. */
            if (g_game.optionToggle1 == 2) svcColor(SVC_SUCCESS);
            if (g_game.optionToggle1 != 0) {
                sprintf(buf, "%d STAGE", g_game.optionToggle1);
                svcText(404.0f, y, buf);
            } else {
                svcText(404.0f, y, "OFF");
            }
            break;
        case 3:  /* LANGUAGE — verde quando ENGLISH (1) */
            if (g_game.svcLangOption == 1) svcColor(SVC_SUCCESS);
            svcText(404.0f, y, SVC_LANG_NAMES[g_game.svcLangOption & 3]);
            break;
        case 4:  /* DEMO SOUND — verde quando ligado (0 no original) */
            if (g_game.svcDemoSound == 0) svcColor(SVC_SUCCESS);
            svcText(404.0f, y, g_game.svcDemoSound == 0 ? "ON" : "OFF");
            break;
        case 5:  /* SHOW HELP — optionToggle2 é o campo lido por menu.c */
            if (g_game.optionToggle2 == 0) svcColor(SVC_SUCCESS);
            svcText(404.0f, y, g_game.optionToggle2 ? "ON" : "OFF");
            break;
        default:
            break;
        }
    }

    if (hit & SVC_BIT_TEST) {
        g_svcCursor++;
        if (g_svcCursor > 8) g_svcCursor = 0;
    }
    if (hit & SVC_BIT_SERVICE) {
        switch (g_svcCursor) {
        case 0:
            g_game.svcGameMode = !g_game.svcGameMode;
            break;
        case 1:
            g_game.optionDifficulty++;
            if (g_game.optionDifficulty > 2) g_game.optionDifficulty = 0;
            break;
        case 2:
            g_game.optionToggle1++;
            if (g_game.optionToggle1 > 4) g_game.optionToggle1 = 0;
            break;
        case 3:
            g_game.svcLangOption++;
            if (g_game.svcLangOption >= 4) g_game.svcLangOption = 1;
            break;
        case 4:
            g_game.svcDemoSound = !g_game.svcDemoSound;
            break;
        case 5:
            g_game.optionToggle2 = !g_game.optionToggle2;
            break;
        case 6:
            svcGameOptionReset();
            break;
        case 7:
            GameOption_Save();
            /* fall-through: SAVE AND EXIT salva e cai no EXIT, como no original */
        case 8:
            g_svcPage = 0;
            break;
        default:
            break;
        }
    }

    /* O original força ENGLISH quando LANGUAGE cai em 0 (coreano) */
    if (g_game.svcLangOption == 0) g_game.svcLangOption = 1;
}

/* ------------------------------------------ GRAPHICS SETTINGS (extra do port)
 * Mesmo padrão da GAME OPTION: TEST move, SERVICE altera. As mudanças valem na
 * hora (Window_ApplyGraphics); SAVE AND EXIT grava no PUMPY.INI. */
static const char* SVC_GFX_ITEMS[8] = {
    "FULLSCREEN", "RESOLUTION", "VSYNC", "TEXTURE FILTER",
    "SHOW FPS", "ASPECT", "SAVE AND EXIT", "EXIT"
};
/* DESATIVADO (30/09/2026): item UPSCALE (XBRZ), entre ASPECT e SAVE AND EXIT.
static const char* SVC_GFX_ITEMS[9] = {
    "FULLSCREEN", "RESOLUTION", "VSYNC", "TEXTURE FILTER",
    "SHOW FPS", "ASPECT", "UPSCALE (XBRZ)", "SAVE AND EXIT", "EXIT"
};
static const char* SVC_GFX_UPS[5] = { "OFF", "OFF", "2X", "3X", "4X" };
*/
static const char* SVC_GFX_RES[5] = { "640x480", "800x600", "1024x768", "1280x960", "1600x1200" };

static void svcRenderGraphics(void)
{
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "GRAPHICS SETTINGS");

    for (i = 0; i < 8; i++) {   /* era 9 com o UPSCALE */
        float y = (float)(352 - i * 20);
        svcColorFor(i, g_svcCursor);
        svcText(196.0f, y, SVC_GFX_ITEMS[i]);
        switch (i) {
        case 0: svcText(404.0f, y, g_game.isFullscreen ? "ON" : "OFF"); break;
        case 1: svcText(404.0f, y, SVC_GFX_RES[(g_game.gfxResIdx >= 0 && g_game.gfxResIdx <= 4) ? g_game.gfxResIdx : 2]); break;
        case 2: svcText(404.0f, y, g_game.vsync ? "ON" : "OFF"); break;
        case 3: svcText(404.0f, y, g_game.gfxTexFilter ? "SHARP" : "SMOOTH"); break;
        case 4: svcText(404.0f, y, g_game.gfxShowFps ? "ON" : "OFF"); break;
        case 5: svcText(404.0f, y, g_game.gfxAspect ? "STRETCH" : "4:3"); break;
        /* case 6: svcText(404.0f, y, SVC_GFX_UPS[(g_game.gfxUpscale >= 2 && g_game.gfxUpscale <= 4) ? g_game.gfxUpscale : 0]); break; */
        default: break;
        }
    }

    if (hit & SVC_BIT_TEST) {
        g_svcCursor++;
        if (g_svcCursor > 7) g_svcCursor = 0;   /* era 8 com o UPSCALE */
    }
    if (hit & SVC_BIT_SERVICE) {
        switch (g_svcCursor) {
        case 0: g_game.isFullscreen = !g_game.isFullscreen; Window_ApplyGraphics(); break;
        case 1: g_game.gfxResIdx = (g_game.gfxResIdx + 1) % 5; Window_ApplyGraphics(); break;
        case 2: g_game.vsync = !g_game.vsync; Window_ApplyGraphics(); break;
        case 3: g_game.gfxTexFilter = !g_game.gfxTexFilter; Window_ApplyGraphics(); break;
        case 4: g_game.gfxShowFps = !g_game.gfxShowFps; break;
        case 5: g_game.gfxAspect = !g_game.gfxAspect; Window_ApplyGraphics(); break;
        /* case 6: g_game.gfxUpscale = (g_game.gfxUpscale < 2) ? 2 : (g_game.gfxUpscale >= 4 ? 0 : g_game.gfxUpscale + 1); break;
         * (UPSCALE desativado: SAVE AND EXIT/EXIT voltam a ser 6/7) */
        case 6:
            GameOption_Save();
            /* fall-through: SAVE AND EXIT salva e sai, como na GAME OPTION */
        case 7:
            g_svcPage = 0;
            break;
        default: break;
        }
    }
}

/* ---------------------------------- ServiceMenu_RenderCoinOption 0x00405d80 */
static const char* SVC_COINOPT_ITEMS[5] = {
    "COIN1 SETTING", "COIN2 SETTING", "DEFAULT SETTING", "SAVE AND EXIT", "EXIT"
};

static void svcRenderCoinOption(void)
{
    char buf[64];
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "COIN OPTION");

    for (i = 0; i < 5; i++) {
        svcColorFor(i, g_svcCursor);
        svcText(196.0f, (float)(352 - i * 20), SVC_COINOPT_ITEMS[i]);
    }

    svcColorFor(0, g_svcCursor);
    if (g_game.svcCoin1 == 0) {
        svcText(404.0f, 352.0f, "FREE PLAY");
    } else {
        sprintf(buf, "1 CREDITS / %d COIN", g_game.svcCoin1);
        svcText(404.0f, 352.0f, buf);
    }

    svcColorFor(1, g_svcCursor);
    if (g_game.svcCoin2 == 0) {
        svcText(404.0f, 332.0f, "FREE PLAY");
    } else {
        sprintf(buf, "%d CREDITS / 1 COIN", g_game.svcCoin2);
        svcText(404.0f, 332.0f, buf);
    }

    if (hit & SVC_BIT_TEST) {
        g_svcCursor++;
        if (g_svcCursor > 4) g_svcCursor = 0;
    }
    if (hit & SVC_BIT_SERVICE) {
        switch (g_svcCursor) {
        case 0:
            g_game.svcCoin1++;
            if (g_game.svcCoin1 > 10) g_game.svcCoin1 = 0;
            break;
        case 1:
            g_game.svcCoin2++;
            if (g_game.svcCoin2 > 9) g_game.svcCoin2 = 1;
            break;
        case 2:
            svcGameOptionReset();
            g_game.svcCoin1 = 1;
            g_game.svcCoin2 = 1;
            break;
        case 3:
            GameOption_Save();
            /* fall-through: SAVE AND EXIT salva e cai no EXIT, como no original */
        case 4:
            g_svcPage = 0;
            break;
        default:
            break;
        }
    }
}

/* ----------------------------------- ServiceMenu_RenderSoundTest 0x00406030
 * Item 0 toca AUDIO/%03d.AUD, item 1 percorre 35 efeitos (0..0x22), item 2 sai.
 * O original compara g_dwInputBits por igualdade exata, não por máscara.
 */
static const char* SVC_SOUND_ITEMS[3] = { "AUDIO", "EFFECT SOUND", "EXIT" };

static void svcRenderSoundTest(void)
{
    char buf[64];
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "SOUND TEST");

    for (i = 0; i < 3; i++) {
        svcColorFor(i, g_svcCursor);
        svcText(180.0f, (float)(352 - i * 20), SVC_SOUND_ITEMS[i]);
    }

    svcColorFor(0, g_svcCursor);
    if (g_svcAudioIdx < 0) {
        svcText(372.0f, 352.0f, "#--");
    } else {
        sprintf(buf, "#%02d", g_svcAudioIdx);
        svcText(372.0f, 352.0f, buf);
    }

    svcColorFor(1, g_svcCursor);
    if (g_svcSeIdx < 0) {
        svcText(372.0f, 332.0f, "#--");
    } else {
        sprintf(buf, "#%02d", g_svcSeIdx);
        svcText(372.0f, 332.0f, buf);
    }

    if (hit == SVC_BIT_TEST) {
        BGM_Stop();
        g_svcCursor++;
        if (g_svcCursor > 2) g_svcCursor = 0;
    } else if (hit == SVC_BIT_SERVICE) {
        if (g_svcCursor == 0) {
            int total = g_game.songDB.songCount;
            g_svcAudioIdx++;
            if (total > 0 && g_svcAudioIdx >= total) g_svcAudioIdx = 0;
            BGM_Stop();
            sprintf(buf, "AUDIO/%03d.AUD", g_svcAudioIdx);
            if (BGM_LoadAUDDirect(buf)) BGM_Play(false);
        } else if (g_svcCursor == 1) {
            g_svcSeIdx++;
            if (g_svcSeIdx > 0x22) g_svcSeIdx = 0;
            Audio_Play(g_svcSeIdx, false);
        } else if (g_svcCursor == 2) {
            BGM_Stop();
            g_svcPage = 0;
        }
    }
}

/* --------------------------------- ServiceMenu_RenderBookkeeping 0x00406360 */
static const char* SVC_BOOK_ITEMS[2] = { "RESET", "EXIT" };

static void svcRenderBookkeeping(void)
{
    char buf[64];
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "BOOKKEEPING MENU");

    sprintf(buf, "SERVICE : %d", g_game.svcServiceTotal);
    svcText(180.0f, 352.0f, buf);
    sprintf(buf, "COIN 1  : %d", g_game.svcCoin1Total);
    svcText(180.0f, 332.0f, buf);
    sprintf(buf, "COIN 2  : %d", g_game.svcCoin2Total);
    svcText(180.0f, 312.0f, buf);

    /* 2 itens a partir de Y=288 descendo 20 */
    for (i = 0; i < 2; i++) {
        svcColorFor(i, g_svcCursor);
        svcText(180.0f, (float)(288 - i * 20), SVC_BOOK_ITEMS[i]);
    }

    if (hit == SVC_BIT_TEST) {
        g_svcCursor++;
        if (g_svcCursor > 1) g_svcCursor = 0;
    } else if (hit == SVC_BIT_SERVICE) {
        if (g_svcCursor == 0) {
            g_svcPage      = 10;
            g_svcSubCursor = 1;   /* começa em NO, como no original */
        } else {
            g_svcPage = 0;
        }
    }
}

/* ---------------------------- ServiceMenu_RenderClearBookkeeping 0x00406210 */
static const char* SVC_YESNO[2] = { "YES", "NO" };

static void svcRenderClearBookkeeping(void)
{
    int i;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "BOOKKEEPING MENU");
    svcText(196.0f, 352.0f, "CLEAR BOOKKEEPING DATA ?");

    /* YES/NO na horizontal: x = 228 e 324, ambos em Y=312 */
    for (i = 0; i < 2; i++) {
        svcColorFor(i, g_svcSubCursor);
        svcText((float)(228 + i * 96), 312.0f, SVC_YESNO[i]);
    }

    if (hit == SVC_BIT_TEST) {
        g_svcSubCursor++;
        if (g_svcSubCursor > 1) g_svcSubCursor = 0;
    } else if (hit == SVC_BIT_SERVICE) {
        if (g_svcSubCursor == 0) {
            g_game.svcServiceTotal = 0;
            g_game.svcCoin1Total   = 0;
            g_game.svcCoin2Total   = 0;
            Log_Print("ServiceMenu: bookkeeping zerado\n");
        }
        g_svcPage = 7;
    }
}

/* ----------------------------------- ServiceMenu_RenderStatistics 0x004064e0
 * Duas colunas de 15 entradas (30 por página). As 10 primeiras entradas da
 * coluna esquerda recebem um degradê de vermelho para branco — no original
 * glColor3f(1, i*0.1, i*0.1) para i em 0..9, e branco daí em diante.
 */
static void svcRenderStatistics(void)
{
    char buf[96];
    int i, y, total;
    uint32_t hit = svcBitsHit();

    svcColor(SVC_NORMAL);
    svcText(276.0f, 432.0f, "STATISTICS MENU");

    total = g_game.songDB.songCount;

    /* coluna esquerda — entradas [base, base+15) */
    y = 352;
    for (i = g_svcCursor * 30; i < g_svcCursor * 30 + 15; i++) {
        float f = (i >= 0 && i <= 9) ? (float)i * 0.1f : 1.0f;
        if (i >= total) break;
        glColor3f(1.0f, f, f);
        sprintf(buf, "%02d. %-24s [%04d]", i, g_game.songDB.songs[i].title, 0);
        svcText(16.0f, (float)y, buf);
        y -= 20;
    }

    /* coluna direita — entradas [base+15, base+30), sempre em branco */
    y = 352;
    for (i = g_svcCursor * 30 + 15; i < g_svcCursor * 30 + 30; i++) {
        if (i >= total) break;
        sprintf(buf, "%02d. %-24s [%04d]", i, g_game.songDB.songs[i].title, 0);
        svcText(336.0f, (float)y, buf);
        y -= 20;
    }

    if (hit == SVC_BIT_TEST) {
        int pages = total / 30;
        if (total % 30 != 0) pages++;
        if (pages < 1) pages = 1;
        g_svcCursor++;
        if (g_svcCursor >= pages) g_svcCursor = 0;
    } else if (hit == SVC_BIT_SERVICE) {
        g_svcPage = 0;
    }
}

/* ============================================================== interface ==*/

/* ------------------------------------------ ServiceMenu_Enter 0x00404ee0 */
void ServiceMenu_Enter(void)
{
    BGM_Stop();

    g_svcPage       = 0;
    g_svcOption     = 0;
    g_svcCursor     = 0;
    g_svcSubCursor  = 0;
    g_svcEepromDone = 0;
    g_svcSkipInput  = 1;    /* engole o F2 que abriu o menu */

    /* A ordem aqui importa. Game_ChangeState chama LoadBGAForState, que para
     * STATE_SERVICE_MENU cai em Resource_ClearBGA() — e esse chama
     * Texture_Shutdown() + Font_Shutdown(), zerando g_fontTexId e os display
     * lists GDI. Carregar a fonte antes desta linha seria desfeito na hora,
     * e como a página principal do menu é só texto, o resultado é tela preta. */
    Game_ChangeState(STATE_SERVICE_MENU);

    /* Só agora recarrega a fonte que o clear acabou de derrubar. */
    Font_Init();   /* display lists GDI (Font_DrawString / overlay de debug) */
    {
        int fid = Font_LoadTexture();   /* textura usada por Font_DrawText */
        Log_Print("ServiceMenu: entrou (fontTexId=%d)%s\n", fid,
                  fid < 0 ? "  <<< AVISO: sem fonte, o menu fica invisivel" : "");
    }
}

/* ------------------------------------------- ServiceMenu_Exit 0x004066d0
 * O original volta para o estado 4, que aqui é STATE_LOGO_ENTER.
 */
void ServiceMenu_Exit(void)
{
    Log_Print("ServiceMenu: saiu\n");
    Game_ChangeState(STATE_LOGO_ENTER);
}

/* --------------------------------------------------- ServiceMenu_Update ---
 * Captura o input do frame. Precisa rodar na fase de update, não no render.
 *
 * Motivo: Game_Update termina com memcpy(prevKeys, keys) (main.c). Como
 * Input_IsKeyHit() é "keys[k] && !prevKeys[k]", depois desse memcpy nenhuma
 * borda é mais detectável — qualquer IsKeyHit() chamado durante Game_Render
 * retorna false. O original lê o input dentro da própria função de render, mas
 * aqui isso deixaria o menu completamente inerte.
 */
void ServiceMenu_Update(void)
{
    uint32_t hit = 0, held = 0;

    if (Input_IsKeyHit(VK_F1)) hit |= SVC_BIT_TEST;     /* TEST    = MOVE   */
    if (Input_IsKeyHit(VK_F2)) hit |= SVC_BIT_SERVICE;  /* SERVICE = SELECT */
    if (Input_IsKeyHit(VK_F3)) hit |= SVC_BIT_CLEAR;
    if (Input_IsKeyHit(VK_F4)) hit |= SVC_BIT_COIN1;
    if (Input_IsKeyHit(VK_F5)) hit |= SVC_BIT_COIN2;

    if (Input_IsKeyDown(VK_F1)) held |= SVC_BIT_TEST;
    if (Input_IsKeyDown(VK_F2)) held |= SVC_BIT_SERVICE;
    if (Input_IsKeyDown(VK_F3)) held |= SVC_BIT_CLEAR;
    if (Input_IsKeyDown(VK_F4)) held |= SVC_BIT_COIN1;
    if (Input_IsKeyDown(VK_F5)) held |= SVC_BIT_COIN2;

    g_svcHitBits   = g_svcSkipInput ? 0u : hit;
    g_svcHeldBits  = held;
    g_svcSkipInput = 0;
}

/* ---------------------------------- ServiceMenu_UpdateRender 0x004066e0
 * Desenha a página atual e aplica o input já capturado por ServiceMenu_Update.
 */
void ServiceMenu_UpdateRender(void)
{
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    svcColor(SVC_NORMAL);

    switch (g_svcPage) {
    case 0:  svcRenderMain();             svcRenderFooter(); break;
    case 1:  svcRenderIOTest();           svcRenderFooter(); break;
    case 2:  svcRenderEEPROMTest();       svcRenderFooter(); break;
    case 3:  svcRenderScreenTest();       svcRenderFooter(); break;
    case 4:  svcRenderGameOption();       svcRenderFooter(); break;
    case 5:  svcRenderCoinOption();       svcRenderFooter(); break;
    case 6:  svcRenderSoundTest();        svcRenderFooter(); break;
    case 7:  svcRenderBookkeeping();      svcRenderFooter(); break;
    case 8:  svcRenderStatistics();       svcRenderFooter(); break;
    case 9:  ServiceMenu_Exit();          svcRenderFooter(); break;
    case 10: svcRenderClearBookkeeping(); svcRenderFooter(); break;
    case SVC_PAGE_GRAPHICS: svcRenderGraphics(); svcRenderFooter(); break;
    default: svcRenderFooter(); break;
    }
}
