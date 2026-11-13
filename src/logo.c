#include "pumpy.h"

static bool padHit(int player, PadButton btn) {
    return Input_IsPadHit(player, btn);
}

static void subEnter(GameState state) {
    g_game.stateFrame = 0;
    g_game.state = state;
}

void Gamestate_UpdateLogo(float dt) {
    (void)dt;
    if (Input_IsKeyHit(VK_ESCAPE)) {
        Game_ChangeState(STATE_LOGO_ENTER);
        return;
    }
    switch (g_game.state) {
    case STATE_LOGO_ENTER:
        /* exceed.exe CLogo 0x413DB0: com crédito (0x41FE94) vai para TITLE */
        if (Attract_HasCredit()) {   /* era: Coin_HasCredit() */
            Game_ChangeState(STATE_CREDIT);
            return;
        }
        /* O CENTER não pula o logo no original:
        if (padHit(0, PAD_C) || padHit(1, PAD_C)) {
            Audio_Play(g_waveSoundIds[SND_3_2], false);
            Game_ChangeState(STATE_INTRO);
            return;
        } */
        if (g_game.bgaFrame >= g_game.bgaMaxFrame && g_game.stateFrame > 60)
            Attract_Idle();     /* fim -> IDLE (0x413394); era: Game_ChangeState(STATE_INTRO) */
        break;
    case STATE_LOGO_SKIP:
        Game_ChangeState(STATE_MENU_ENTER);
        break;
    default:
        break;
    }
}
