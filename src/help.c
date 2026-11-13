/*
 * help.c — proc HELP do arcade (piu, vtable 0x807A618).
 *
 *   Begin 0x8051D56: sem argumento -> BGA\03.DAT + AUDIO\03.AUD;
 *                    "HELP 1" (BATTLE) -> dois BGAs pelo idioma (+0x7EF):
 *                    1 03BE, 2 03BS, 3 03BC, senão 03BH (-1 e -2) + AUDIO\03B.AUD.
 *   Run   0x8051EAE: CENTER de qualquer jogador (0x404) -> efeito 10 e sai.
 *                    Normal: quadro t até 0x6B6. BATTLE: 1º BGA até 0x851,
 *                    depois o 2º em t - 0x852, até 0xB04. t +1 por quadro.
 *   End   0x8051F6A: normal -> "SELECT", BATTLE -> "SELECT 4".
 */
#include "pumpy.h"
#include "bga.h"

static int  g_helpBattle;
static int  g_helpT;          /* [+0x14] */
static int  g_helpBga[2] = { -1, -1 };

void Help_Enter(int battle)
{
    static const char* const k_lang[4] = { "03BH", "03BE", "03BS", "03BC" };
    char name[16], aud[MAX_PATH];
    int lang = Eeprom2_Language();

    g_helpBattle = battle;
    g_helpT = 0;
    g_helpBga[0] = g_helpBga[1] = -1;
    BGM_Stop();
    Game_ChangeState(STATE_HELP);   /* limpa o BGA anterior (LoadBGAForState) */

    if (!battle) {
        if (Resource_LoadBGAByName("03")) g_helpBga[0] = g_game.bgaPicCount - 1;
        snprintf(aud, sizeof(aud), "%s/AUDIO/03.AUD", g_game.currentDirectory);
    } else {
        const char* base = k_lang[(lang >= 1 && lang <= 3) ? lang : 0];
        snprintf(name, sizeof(name), "%s-1", base);
        if (Resource_LoadBGAByName(name)) g_helpBga[0] = g_game.bgaPicCount - 1;
        snprintf(name, sizeof(name), "%s-2", base);
        if (Resource_LoadBGAByName(name)) g_helpBga[1] = g_game.bgaPicCount - 1;
        snprintf(aud, sizeof(aud), "%s/AUDIO/03B.AUD", g_game.currentDirectory);
    }
    /* 0x8051E91: 0x8075122 + 0x8075160, o som toca sempre */
    if (BGM_LoadAUDDirect(aud)) BGM_Play(false);
    Log_Print("HELP: %s (BGA %d %d)\n", battle ? "1" : "-", g_helpBga[0], g_helpBga[1]);
}

static void helpEnd(void)
{
    BGM_Stop();
    Resource_ClearBGA();
    g_helpBga[0] = g_helpBga[1] = -1;
    /* "SELECT 4" no BATTLE; o canal já foi definido pela Station */
    Game_ChangeState(STATE_EXSELECT);
}

void Help_Update(float dt)
{
    (void)dt;
    if (Input_IsPadHit(0, PAD_C) || Input_IsPadHit(1, PAD_C)) {
        BLua_PlayEffect(10);    /* 0x8074CA8(10, 0) */
        helpEnd();
        return;
    }
    if (!g_helpBattle ? (g_helpT > 0x6B6) : (g_helpT > 0xB04)) {
        helpEnd();
        return;
    }
    g_helpT++;
}

void Help_Render(void)
{
    if (!g_helpBattle || g_helpT <= 0x851) {
        if (g_helpBga[0] >= 0) BGA_Render(g_helpBga[0], g_helpT);
    } else if (g_helpBga[1] >= 0) {
        BGA_Render(g_helpBga[1], g_helpT - 0x852);
    }
}
