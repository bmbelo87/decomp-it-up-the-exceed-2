#include "pumpy.h"

/* Persistência no formato do PUMPY.EXE (EEPROM do gabinete).
 *
 * O original guarda uma imagem binária de 2048 bytes em "c:\pumpprex3.ini"
 * (fread(...,1,0x800,...) em 0x4067a0; defaults em 0x405150; gravação em 0x405190).
 * Aqui o arquivo é "pumpprex3.ini" na pasta do jogo. Layout (offset = endereço - 0xd38858):
 *
 *   +0x68C  ident  0x312E3358 ("X3.1")           0xd38ee4
 *   +0x690  checksum Adler-32 dos 8 bytes de +0x7E8   0xd38ee8   (0x4195d0 -> 0x419570)
 *   +0x7E8  GAME MODE   (0=NORMAL, 1=EVENT)       0xd39040
 *   +0x7E9  LEVEL       (0=EASY 1=NORMAL 2=HARD)  0xd39041
 *   +0x7EA  STAGE BREAK (0=OFF, 1..4 stages)      0xd39042  default 2
 *   +0x7EB  LANGUAGE                              0xd39043  default 1
 *   +0x7EC  DEMO SOUND  (0=ON)                    0xd39044
 *   +0x7ED  SHOW HELP                             0xd39045
 *   +0x7EE  COIN 1 (moedas por crédito)           0xd39046  default 5
 *   +0x7EF  COIN 2                                0xd39047  default 1
 *   +0x7F0  bookkeeping COIN 1  (u32)             0xd39048  (tela BOOKKEEPING 0x406360)
 *   +0x7F4  bookkeeping COIN 2  (u32)             0xd3904c
 *   +0x7F8  total de moedas     (u32)             0xd39050
 *   +0x7FC  bookkeeping SERVICE (u32)             0xd39054
 *
 * O restante da imagem (ranking etc.) fica como está: é preservado ao regravar.
 *
 * Diferença deliberada: o default do COIN 1 é 0 (FREE PLAY) em vez de 5, senão
 * um arquivo novo deixaria o jogo sem crédito num PC sem moedeiro. */

#define EEP_SIZE       0x800
#define EEP_IDENT      0x68C
#define EEP_CHKSUM     0x690
#define EEP_SETTINGS   0x7E8
#define EEP_SETLEN     8
#define EEP_IDENT_VAL  0x312E3358u   /* "X3.1" */

#define O_MODE      0x7E8
#define O_LEVEL     0x7E9
#define O_STAGEBRK  0x7EA
#define O_LANG      0x7EB
#define O_DEMO      0x7EC
#define O_HELP      0x7ED
#define O_COIN1     0x7EE
#define O_COIN2     0x7EF
#define O_COIN1TOT  0x7F0
#define O_COIN2TOT  0x7F4
#define O_COINTOT   0x7F8
#define O_SVCTOT    0x7FC

#define EEP_DEFAULT_COIN1  0   /* FREE PLAY (o original vem com 5) */

static uint8_t g_eep[EEP_SIZE];

static uint32_t eepGet32(int off) {
    return (uint32_t)g_eep[off] | ((uint32_t)g_eep[off + 1] << 8) |
           ((uint32_t)g_eep[off + 2] << 16) | ((uint32_t)g_eep[off + 3] << 24);
}

static void eepPut32(int off, uint32_t v) {
    g_eep[off]     = (uint8_t)(v);
    g_eep[off + 1] = (uint8_t)(v >> 8);
    g_eep[off + 2] = (uint8_t)(v >> 16);
    g_eep[off + 3] = (uint8_t)(v >> 24);
}

/* Adler-32 com valor inicial 1 (PUMPY.EXE 0x419570: módulo 0xFFF1). */
static uint32_t eepAdler32(const uint8_t* d, int n) {
    uint32_t a = 1, b = 0;
    for (int i = 0; i < n; i++) {
        a = (a + d[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void eepStamp(void) {
    eepPut32(EEP_IDENT, EEP_IDENT_VAL);
    eepPut32(EEP_CHKSUM, eepAdler32(&g_eep[EEP_SETTINGS], EEP_SETLEN));
}

/* PUMPY.EXE 0x405150: tudo 0xFF, depois os defaults de 0x404f40/0x404fa0 e os totais zerados. */
static void eepDefaults(void) {
    memset(g_eep, 0xFF, sizeof(g_eep));
    g_eep[O_MODE]     = 0;
    g_eep[O_LEVEL]    = 1;
    g_eep[O_STAGEBRK] = 2;
    g_eep[O_LANG]     = 1;
    g_eep[O_DEMO]     = 0;
    g_eep[O_HELP]     = 0;
    g_eep[O_COIN1]    = EEP_DEFAULT_COIN1;
    g_eep[O_COIN2]    = 1;
    eepPut32(O_COIN1TOT, 0);
    eepPut32(O_COIN2TOT, 0);
    eepPut32(O_COINTOT, 0);
    eepPut32(O_SVCTOT, 0);
    eepStamp();
}

static void eepPath(char* out, size_t n) {
    snprintf(out, n, "%s/pumpprex3.ini", g_game.currentDirectory);
}

static void eepToGame(void) {
    g_game.svcGameMode     = g_eep[O_MODE];
    g_game.optionDifficulty = g_eep[O_LEVEL];
    g_game.optionToggle1   = g_eep[O_STAGEBRK];
    g_game.svcLangOption   = g_eep[O_LANG];
    g_game.svcDemoSound    = g_eep[O_DEMO];
    g_game.optionToggle2   = g_eep[O_HELP];
    g_game.svcCoin1        = g_eep[O_COIN1];
    g_game.svcCoin2        = g_eep[O_COIN2];
    g_game.svcCoin1Total   = (int)eepGet32(O_COIN1TOT);
    g_game.svcCoin2Total   = (int)eepGet32(O_COIN2TOT);
    g_game.svcCoinTotal    = (int)eepGet32(O_COINTOT);
    g_game.svcServiceTotal = (int)eepGet32(O_SVCTOT);
}

static void eepFromGame(void) {
    g_eep[O_MODE]     = (uint8_t)g_game.svcGameMode;
    g_eep[O_LEVEL]    = (uint8_t)g_game.optionDifficulty;
    g_eep[O_STAGEBRK] = (uint8_t)g_game.optionToggle1;
    g_eep[O_LANG]     = (uint8_t)g_game.svcLangOption;
    g_eep[O_DEMO]     = (uint8_t)g_game.svcDemoSound;
    g_eep[O_HELP]     = (uint8_t)g_game.optionToggle2;
    g_eep[O_COIN1]    = (uint8_t)g_game.svcCoin1;
    g_eep[O_COIN2]    = (uint8_t)g_game.svcCoin2;
    eepPut32(O_COIN1TOT, (uint32_t)g_game.svcCoin1Total);
    eepPut32(O_COIN2TOT, (uint32_t)g_game.svcCoin2Total);
    eepPut32(O_COINTOT,  (uint32_t)g_game.svcCoinTotal);
    eepPut32(O_SVCTOT,   (uint32_t)g_game.svcServiceTotal);
    eepStamp();
}

/* Exceed2: as opções e o bookkeeping ficam no PIUEXCEED2.INI (eeprom_x2.c).
 * Bytes conferidos no PIU32.EXE (GAME/COIN OPTION 0x4185B8.., moeda 0x41F8F0):
 *   7EC modo (1 = EVENT)  7ED LEVEL  7EE STAGE BREAK  7EF idioma  7F0 DEMO SOUND
 *   7F1 ajuda  7F2 COIN1 (0 = FREE PLAY)  7F3 COIN2
 *   7F4 / 7F8 / 7FC  totais COIN1 / COIN2 / SERVICE (u32)
 * O acumulador de créditos (svcCoinTotal) fica só em memória, como no original. */
static uint32_t x2Get32(const uint8_t* x, int o) {
    return (uint32_t)x[o] | ((uint32_t)x[o + 1] << 8) | ((uint32_t)x[o + 2] << 16) | ((uint32_t)x[o + 3] << 24);
}
static void x2Put32(uint8_t* x, int o, uint32_t v) {
    x[o] = (uint8_t)v; x[o + 1] = (uint8_t)(v >> 8); x[o + 2] = (uint8_t)(v >> 16); x[o + 3] = (uint8_t)(v >> 24);
}

/* Grava a imagem (PUMPY.EXE 0x405190). */
void Eeprom_Save(void) {
    uint8_t* x = Eeprom2_Data();
    x[0x7EC] = (uint8_t)g_game.svcGameMode;
    x[0x7ED] = (uint8_t)g_game.optionDifficulty;
    x[0x7EE] = (uint8_t)g_game.optionToggle1;
    x[0x7EF] = (uint8_t)g_game.svcLangOption;
    x[0x7F0] = (uint8_t)g_game.svcDemoSound;
    x[0x7F1] = (uint8_t)g_game.optionToggle2;
    x[0x7F2] = (uint8_t)g_game.svcCoin1;
    x[0x7F3] = (uint8_t)g_game.svcCoin2;
    x2Put32(x, 0x7F4, (uint32_t)g_game.svcCoin1Total);
    x2Put32(x, 0x7F8, (uint32_t)g_game.svcCoin2Total);
    x2Put32(x, 0x7FC, (uint32_t)g_game.svcServiceTotal);
    Eeprom2_Save();
#if 0  /* DESATIVADO (08/10/2026): imagem do Prex3 (pumpprex3.ini) */
    eepFromGame();
    char path[MAX_PATH];
    eepPath(path, sizeof(path));
    FILE* f = fopen(path, "wb");
    if (!f) {
        Log_Print("EEPROM: cannot write '%s'\n", path);
        return;
    }
    fwrite(g_eep, 1, sizeof(g_eep), f);
    fclose(f);
#endif
}

/* Lê e valida a imagem (PUMPY.EXE 0x4067a0) e aplica em g_game.
 * Retorno: 1 = arquivo válido; 0 = arquivo inválido (resetado, com o aviso do original);
 *          -1 = arquivo ausente (defaults em g_game; quem chamou decide migrar/gravar). */
int Eeprom_Load(void) {
    /* Exceed2: lê do PIUEXCEED2.INI (ver Eeprom_Save); Eeprom2_Load já valida e
     * aplica os padrões do PIU32.EXE (0x421DE0) */
    const uint8_t* x = Eeprom2_Data();
    g_game.svcGameMode      = x[0x7EC];
    g_game.optionDifficulty = x[0x7ED];
    g_game.optionToggle1    = x[0x7EE];
    g_game.svcLangOption    = x[0x7EF];
    g_game.svcDemoSound     = x[0x7F0];
    g_game.optionToggle2    = x[0x7F1];
    g_game.svcCoin1         = x[0x7F2];
    g_game.svcCoin2         = x[0x7F3];
    g_game.svcCoin1Total    = (int)x2Get32(x, 0x7F4);
    g_game.svcCoin2Total    = (int)x2Get32(x, 0x7F8);
    g_game.svcServiceTotal  = (int)x2Get32(x, 0x7FC);
    g_game.svcCoinTotal     = 0;
    return 1;
#if 0  /* DESATIVADO (08/10/2026) */
    char path[MAX_PATH];
    eepPath(path, sizeof(path));
    Log_Print("EEPROM: loading %s\n", path);

    FILE* f = fopen(path, "rb");
    if (!f) {
        eepDefaults();
        eepToGame();
        return -1;
    }
    size_t got = fread(g_eep, 1, sizeof(g_eep), f);
    fclose(f);

    if (got != sizeof(g_eep) || eepGet32(EEP_IDENT) != EEP_IDENT_VAL) {
        Log_Print("Warning - ident mismatch\n");
        eepDefaults();
        eepToGame();
        Eeprom_Save();               /* o original reseta e regrava neste caso */
        return 0;
    }
    if (eepGet32(EEP_CHKSUM) != eepAdler32(&g_eep[EEP_SETTINGS], EEP_SETLEN)) {
        Log_Print("Warning - EEPROM chksum err\n");
        eepDefaults();               /* e não regrava, como o original */
        eepToGame();
        return 0;
    }
    eepToGame();
    return 1;
#endif
}
