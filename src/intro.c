/*
 * intro.c — telas de vídeo do Exceed.
 *   STATE_INTRO  (CIntro::Begin): BGA\INTRO.MOV + AUDIO\001.AUD, depois do logo 81.
 *   STATE_CREDIT (CTitle::Begin): CREDIT.MOV (raiz do jogo), em loop.
 *
 * Ordem das telas informada pelo usuário: R_WARN_A -> 81 -> INTRO -> CREDIT.
 * Hipótese ainda não conferida no exceed.exe: CENTER pula o INTRO.
 */
#include "pumpy.h"
#include "movie.h"
#include "bga.h"

static unsigned g_titleJoined;   /* [0x568FF4]: bit0 = P1 entrou, bit1 = P2 */
static int      g_titleFade;     /* [CTitle+0x1C] */

/* exceed.exe CIdle (vtable 0x1282BC8, "Idle 루프입니다. 크레딧이 없는 상태."):
 * Begin 0x4130F4 para todos os WAVE; 0x41311C escolhe a próxima tela pelo
 * contador [CIdle+4] (começa em 0, +1 a cada entrada):
 *   % 4 == 0 -> LOGO, 1 -> INTRO, 2 -> HIGHSCORE, 3 -> demo
 *   ("RUN A%02d -h -demo", 0x41926C).
 * Toda tela da atração vai para IDLE ao terminar (0x413394) e para TITLE
 * quando há crédito (0x41FE94). */
static int g_idleCount;         /* [CIdle+4] */
static int g_demoCount = -1;    /* [0x455100] */
static bool g_demoSound = true; /* EEPROM +0x7F0 (0x40265B) */
bool g_exDemo;

bool Demo_SoundOn(void) { return g_demoSound; }

/* 0x41315A + 0x41926C: n = contador % 25 + 1, pulando a 3 (A03 é oculta);
 * "RUN A%02d -h -demo". O -demo (0x401CE0) entra com os dois jogadores em
 * autoplay e [0x568FF4] = 0x100533. */
static bool Demo_Start(void) {
    g_demoCount++;
    int n = g_demoCount % 25 + 1;
    if (n == 3) {
        g_demoCount++;
        n = g_demoCount % 25 + 1;
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "A%02d", n);
    int id = (int)strtol(buf, NULL, 16);

    Menu_ResetState();
    g_game.selectedSongIndex = Song_FindByID(&g_game.songDB, id);
    g_game.selectedModeIndex = Song_FindMode(&g_game.songDB, "HARD");
    if (g_game.selectedSongIndex < 0 || g_game.selectedModeIndex < 0) {
        Log_Print("DEMO: música %s/HARD não encontrada\n", buf);
        return false;
    }
    g_game.activePlayerMask = 3;
    g_game.isBattleMode = false;
    for (int p = 0; p < 2; p++) {
        g_game.cmdSpeedMult[p] = 1;
        g_game.cmdRandomVelocity[p] = false;
        g_exPlayerMods[p] = 0u;
    }
    /* Opção da EEPROM: som na demo */
    g_demoSound = (Eeprom2_Data()[0x7F0] != 0);   /* EEPROM +0x7F0 (PIUEXCEED2.INI, eeprom_x2.c) */
    Log_Print("DEMO: RUN %s -h -demo (som %d)\n", buf, g_demoSound);
    g_exDemo = true;
    Loading_Enter(id);
    return true;
}

/* 0x402B82: com crédito ou 35 s de música ([+0x17C38] >= 35000) -> IDLE */
void Demo_End(void) {
    g_exDemo = false;
    Gameplay_Exit();
    Movie_Close();
    Resource_ClearBGA();
    Attract_Idle();
}

/* INTRO e PLAYMOVIE usam o mesmo estado (STATE_INTRO) */
static int   g_introKind;           /* 0 = CIntro, 1 = PLAYMOVIE */
static char  g_pmName[32];          /* PLAYMOVIE <nome> */
static DWORD g_introStartMs;        /* relógio sem som (0x80756DC) */
static bool  g_introSound;          /* EEPROM +0x7F0 == 1 */
static bool  g_titleSound;          /* CTitle [+0x6D] */
static int   g_titleCount;          /* CTitle [+0x54] */
static int   g_titleOut;            /* CTitle [+0x5C]: fade de saída (0 = rodando) */

/* Extra do port (GAME STYLE = MODDED), igual ao NXReconstructed: atração em
 * FREE PLAY / EVENT. No original (piu 0x804EE5A) o crédito vale 99 nesses modos,
 * então LOGO/vídeos/HIGHSCORE/demo vão direto para o TITLE e o TITLE nunca sai:
 * a atração não roda. No MODDED com FREE PLAY ela roda; CENTER de qualquer
 * jogador leva ao TITLE, e o TITLE volta à atração pelo tempo do original
 * (0xC4D quadros + fade de 60). No VANILLA vale o teste do original. */
static bool s_attractWantTitle;
static bool attractFree(void) { return g_game.gameStyleModded && Coin_IsFreePlay(); }

bool Attract_HasCredit(void) {
    if (!attractFree()) return Coin_HasCredit();
    if (Input_IsPadHit(0, PAD_C) || Input_IsPadHit(1, PAD_C)) s_attractWantTitle = true;
    return s_attractWantTitle;
}

/* era (exceed.exe, % 4):
void Attract_Idle(void) {
    Audio_StopAll();
    for (int guard = 0; guard < 4; guard++) {
        switch (g_idleCount++ & 3) {
        case 0: Game_ChangeState(STATE_LOGO_ENTER); return;
        case 1: Game_ChangeState(STATE_INTRO);      return;
        case 2: Game_ChangeState(STATE_HIGHSCORE);  return;
        case 3: if (Demo_Start()) return; break;
        }
    }
}
 * Arcade (piu 0x80504D8, tabela 0x807A1B8): [+4] % 6 ->
 *   0 LOGO, 1 PLAYMOVIE FREEVOLT, 2 INTRO, 3 TITLE, 4 HIGHSCORE, 5 demo. */
void Attract_Idle(void) {
    Audio_StopAll();            /* 0x4130F9..0x41310F */
    if (s_attractWantTitle) {   /* MODDED: CENTER durante a demo (Demo_End cai aqui) */
        Game_ChangeState(STATE_CREDIT);
        return;
    }
    /* Port (como no NXReconstructed): o passo seguinte vem da tela em que se está.
     * O boot entra no LOGO e o crédito pula para o TITLE sem passar pelo IDLE, e
     * o contador ficava atrasado: do TITLE voltava ao LOGO em vez do HIGHSCORE. */
    if (g_game.state == STATE_LOGO_ENTER)      g_idleCount = 1;   /* -> PLAYMOVIE FREEVOLT */
    else if (g_game.state == STATE_CREDIT)     g_idleCount = 4;   /* -> HIGHSCORE */
    else if (g_game.state == STATE_HIGHSCORE)  g_idleCount = 5;   /* -> demo */
    for (int guard = 0; guard < 6; guard++) {
        switch (g_idleCount++ % 6) {
        case 0: Game_ChangeState(STATE_LOGO_ENTER); return;
        case 1: if (Attract_PlayMovie("FREEVOLT")) return; break;
        case 2: g_introKind = 0; Game_ChangeState(STATE_INTRO); return;
        case 3: Game_ChangeState(STATE_CREDIT);     return;
        case 4: Game_ChangeState(STATE_HIGHSCORE);  return;
        case 5: if (Demo_Start()) return; break;
        }
    }
}

void Attract_Idle_Intro(void) { g_introKind = 0; Game_ChangeState(STATE_INTRO); }

/* PLAYMOVIE <nome> (arcade 0x8050844): exige BGA\<nome>.MOV e AUDIO\<nome>.AUD;
 * senão "/%s: incorrect parameters!" e IDLE. (O caso STAFF, com BGA\STAFF.DAT
 * por cima, só é chamado pelo console e não foi reproduzido.) */
bool Attract_PlayMovie(const char* name) {
    char mov[MAX_PATH], aud[MAX_PATH];
    FILE* f;
    snprintf(mov, sizeof(mov), "%s/BGA/%s.MOV", g_game.currentDirectory, name);
    snprintf(aud, sizeof(aud), "%s/AUDIO/%s.AUD", g_game.currentDirectory, name);
    if (!(f = fopen(mov, "rb"))) { Log_Print("/PLAYMOVIE: incorrect parameters! (%s)\n", mov); return false; }
    fclose(f);
    if (!(f = fopen(aud, "rb"))) { Log_Print("/PLAYMOVIE: incorrect parameters! (%s)\n", aud); return false; }
    fclose(f);
    snprintf(g_pmName, sizeof(g_pmName), "%s", name);
    g_introKind = 1;
    Game_ChangeState(STATE_INTRO);
    return true;
}

/* [0x568FF4] bits 0/1: quem entrou no CREDIT (lido pela Select) */
unsigned Title_GetJoinedMask(void) { return g_titleJoined; }
void Title_SetJoinedMask(unsigned m) { g_titleJoined = m; }   /* CStation: entrada tardia */

static void intro_open(const char* rel, bool loop) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s", g_game.currentDirectory, rel);
    Movie_Open(path, loop);
}

void Gamestate_UpdateIntro(float dt) {
    switch (g_game.state) {
    case STATE_INTRO:
        if (g_game.stateFrame == 1) {
            /* arcade CIntro 0x80505C8 / PLAYMOVIE 0x8050844: o som só toca com
             * DEMO SOUND (+0x7F0 == 1). era: sempre tocava. */
            char aud[MAX_PATH], mov[MAX_PATH];
            if (g_introKind == 1) {
                snprintf(aud, sizeof(aud), "%s/AUDIO/%s.AUD", g_game.currentDirectory, g_pmName);
                snprintf(mov, sizeof(mov), "BGA/%s.MOV", g_pmName);
            } else {
                snprintf(aud, sizeof(aud), "%s/AUDIO/001.AUD", g_game.currentDirectory);
                snprintf(mov, sizeof(mov), "BGA/INTRO.MOV");
            }
            intro_open(mov, false);
            BGM_Stop();
            g_introSound = (Eeprom2_Data()[0x7F0] == 1);
            if (g_introSound && BGM_LoadAUDDirect(aud)) BGM_Play(false);
            g_introStartMs = timeGetTime();
        }
        Movie_Update(dt);
        /* 0x8050716 / 0x8050A70: com crédito vai para TITLE (era: Coin_HasCredit()) */
        if (Attract_HasCredit()) {
            Movie_Close();
            BGM_Stop();
            Game_ChangeState(STATE_CREDIT);
            break;
        }
        if (g_game.stateFrame > 1) {
            /* tempo = posição do som com DEMO SOUND, senão relógio (0x807554C / 0x80756DC) */
            uint32_t ms = g_introSound ? BGM_GetPositionMs() : (uint32_t)(timeGetTime() - g_introStartMs);
            bool sndEnd = g_introSound && BGM_HasEnded();
            bool done;
            if (g_introKind == 1)   /* PLAYMOVIE: fim do vídeo ou do som */
                done = Movie_HasEnded() || !Movie_IsOpen() || sndEnd;
            else                    /* INTRO: 43000 ms (0xA7F7) ou fim do som */
                done = ms > 0xA7F7 || sndEnd;
            if (done) {
                Log_Print("INTRO: fim (tipo %d, %u ms)\n", g_introKind, ms);
                Movie_Close();
                BGM_Stop();
                Attract_Idle();
            }
        }
        break;
#if 0   /* fim do INTRO pelo vídeo (antes do arcade) */
        {
        /* Hipótese antiga (CENTER pulava o INTRO), não existe no original:
        if (g_game.stateFrame > 1 &&
            (Movie_HasEnded() || !Movie_IsOpen() ||
             Input_IsPadHit(0, PAD_C) || Input_IsPadHit(1, PAD_C))) { */
        if (g_game.stateFrame > 1 && (Movie_HasEnded() || !Movie_IsOpen())) {
            Log_Print("INTRO: fim (frame %d, ended=%d open=%d)\n", g_game.stateFrame,
                      Movie_HasEnded(), Movie_IsOpen());
            Movie_Close();
            /* Game_ChangeState(STATE_CREDIT); */
            Attract_Idle();     /* fim do vídeo -> IDLE (0x413394) */
        }
        }
#endif
    case STATE_CREDIT:
        if (g_game.stateFrame == 1) {
            /* CTitle::Begin (0x41C05A): 0x426570("AUDIO\TITLE.AUD") e depois
             * 0x4229C8("CREDIT.MOV", 1) — vídeo em loop. */
            char aud[MAX_PATH];
            snprintf(aud, sizeof(aud), "%s/AUDIO/TITLE.AUD", g_game.currentDirectory);
            BGM_Stop();
            /* arcade 0x807201F: o TITLE.AUD só toca com DEMO SOUND ou com crédito
             * ([+0x6D]); no fim do som ele recomeça (0x8072269).
             * era: if (BGM_LoadAUDDirect(aud)) BGM_Play(true); */
            if (BGM_LoadAUDDirect(aud)) {
                g_titleSound = (Eeprom2_Data()[0x7F0] != 0) || Coin_HasCredit();
                if (g_titleSound) BGM_Play(true);
            }
            intro_open("CREDIT.MOV", true);
            g_titleJoined = 0;
            g_titleFade = 0;
            g_titleCount = 0;
            g_titleOut = 0;
            s_attractWantTitle = false;   /* MODDED: chegou ao TITLE */
        }
        Movie_Update(dt);
        if (!g_titleSound && Coin_HasCredit()) {   /* 0x807227C */
            g_titleSound = true;
            BGM_Play(true);
        }

        /* arcade 0x80722A2..0x80722D7: sem ninguém dentro e sem crédito, depois
         * de 3149 quadros (0xC4D) sai; 0x8072394: fade preto de 60 quadros
         * (alpha = n/60) e IDLE. Crédito durante o fade volta a rodar. */
        /* MODDED + FREE PLAY: o crédito "eterno" não segura o TITLE */
        bool titleNoCredit = attractFree() || !Coin_HasCredit();
        if (g_titleOut > 0) {
            if (!titleNoCredit) {   /* era: if (Coin_HasCredit()) */
                g_titleOut = 0;
                g_titleFade = 0;
            } else if (g_titleOut > 0x3B) {
                Movie_Close();
                BGM_Stop();
                Attract_Idle();
                return;
            } else {
                g_titleOut++;
                g_titleFade = g_titleOut;
                break;
            }
        }
        g_titleCount++;   /* [+0x54], +1 por quadro */
        if (!(g_titleJoined & 3) && titleNoCredit && g_titleCount > 0xC4D) {   /* era: !Coin_HasCredit() */
            g_titleOut = 1;
            g_titleFade = 1;
            break;
        }

        /* 0x41C192..0x41C1C8 / 0x41C594 / 0x41C5EC: com crédito, CENTER do
         * P1 (bit 0x4 de [0x568FE8]) ou do P2 (bit 0x400) entra o jogador:
         * consome crédito (0x41FEE0), liga o bit em [0x568FF4] e toca
         * WAVE/2-1.wav ([0xA6D304], carregado em 0x4258A6). */
        if (Coin_HasCredit()) {
            if (!(g_titleJoined & 1) && Input_IsPadHit(0, PAD_C)) {
                Coin_ConsumeCredit();
                g_titleJoined |= 1;
                Audio_Play(g_waveSoundIds[SND_2_1], false);
            } else if (!(g_titleJoined & 2) && Input_IsPadHit(1, PAD_C)) {
                Coin_ConsumeCredit();
                g_titleJoined |= 2;
                Audio_Play(g_waveSoundIds[SND_2_1], false);
            }
        }

        /* 0x41C4F3 / 0x41C56B: com alguém dentro, [this+0x1C] conta +1 por
         * frame (fade preto = contador/60) e passando de 60 vai para "SELECT"
         * (0x4102D4). */
        /* Exceed2 (PIU32.EXE 0x41acfe / 0x41aef7): com alguém dentro a fase 3
         * vai direto para "STATION" (0x41af1f), sem fade; o CREDIT.MOV e o
         * TITLE.AUD continuam (o CStation desenha o vídeo, 0x419d1f). */
        if (g_titleJoined & 3) {
            Game_ChangeState(STATE_STATION);
            return;
        }
#if 0   /* Exceed (exceed.exe): fade de 60 quadros e SELECT — DESATIVADO */
        if (g_titleJoined & 3) {
            if (g_titleFade > 60) {
                Movie_Close();
                /* Game_ChangeState(STATE_SONG_SELECT); */  /* select do Prex3 (099.DAT) */
                /* Contagem de estágios do projeto (3 + bônus, regra do Prex3).
                 * HIPÓTESE para o Exceed: ainda não conferido no exceed.exe. */
                Menu_ResetState();
                Game_ChangeState(STATE_EXSELECT);
                return;
            }
            g_titleFade++;
        }
#endif
        break;
    default:
        break;
    }
}

/* CTitle: desenho do BGA\82.DAT sobre o vídeo (0x41C1CE..0x41C483).
 *   com crédito (0x41FEB8 != 0): slots 16,17,20 no quadro 244 + c%30 (P1)
 *                                 e 365 + c%30 (P2)  — bb, ba, pcs
 *   sem crédito:                  slots 6,7 no quadro c%47 (P1)
 *                                 e 147 + c%47 (P2)   — pic
 *   P1/P2 pulados pelos bits 0/1 de [0x568FF4]; sempre slot 3 (ltd) no quadro 0.
 * c = [CTitle+0x14], +1 por frame (0x41C54E) = stateFrame. */
void Gamestate_RenderIntro(void) {
    Movie_Render();
    if (g_game.state != STATE_CREDIT || g_game.bgaPicCount <= 0) return;

    int c = g_game.stateFrame;
    unsigned joined = g_titleJoined;           /* [0x568FF4] */
    if (Coin_HasCredit()) {
        if (!(joined & 1)) {
            BGA_DrawSlot(0, 244 + c % 30, 16);
            BGA_DrawSlot(0, 244 + c % 30, 17);
            BGA_DrawSlot(0, 244 + c % 30, 20);
        }
        if (!(joined & 2)) {
            BGA_DrawSlot(0, 365 + c % 30, 16);
            BGA_DrawSlot(0, 365 + c % 30, 17);
            BGA_DrawSlot(0, 365 + c % 30, 20);
        }
    } else {
        if (!(joined & 1)) {
            BGA_DrawSlot(0, c % 47, 6);
            BGA_DrawSlot(0, c % 47, 7);
        }
        if (!(joined & 2)) {
            BGA_DrawSlot(0, 147 + c % 47, 6);
            BGA_DrawSlot(0, 147 + c % 47, 7);
        }
    }
    BGA_DrawSlot(0, 0, 3);

    /* 0x41C488: fade preto por cima, alpha = [CTitle+0x1C] / 60.0 */
    if (g_titleFade > 0) {
        float a = (float)g_titleFade / 60.0f;
        if (a > 1.0f) a = 1.0f;
        glDisable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(0.0f, 0.0f, 0.0f, a);
        glBegin(GL_QUADS);
        glVertex2f(0, 0); glVertex2f(640, 0); glVertex2f(640, 480); glVertex2f(0, 480);
        glEnd();
        glColor4f(1, 1, 1, 1);
    }
}
