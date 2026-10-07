#include "pumpy.h"
#include "testbga.h"
#include "movie.h"
#include "vsl.h"
#include <SDL.h>

extern bool Window_Create(HINSTANCE hInstance, int width, int height, bool fullscreen);
extern void Window_Destroy(void);
extern void Window_SwapBuffers(void);
extern bool Window_ProcessMessages(void);

static void InitSystems(void) {
    Log_Print("=== PUMP IT UP v%s (%s %s) ===\n",
              GAME_VERSION, GAME_BUILD_DATE, GAME_BUILD_TIME);

    timeBeginPeriod(1);

    /* Tamanho da JANELA. O backbuffer lógico continua 640x480 (é a resolução
     * em que toda a arte foi feita e a base de todas as coordenadas do jogo);
     * o Window_UpdateViewport estica o viewport e mantém o glOrtho em 640x480,
     * então subir aqui só amplia a imagem, sem mexer em nenhuma coordenada.
     * 1024x768 é 4:3, o mesmo aspecto, então não entra tarja preta. */
    /* if (!Window_Create(NULL, WINDOW_DEFAULT_W, WINDOW_DEFAULT_H, false)) { */
    int winW, winH;
    Window_GetResolution(g_game.gfxResIdx, &winW, &winH);   /* GRAPHICS SETTINGS */
    if (!Window_Create(NULL, winW, winH, g_game.isFullscreen)) {
        Log_Print("FATAL: could not create SDL2/OpenGL window: %s\n", SDL_GetError());
        exit(1);
    }

    Render_SetOrtho(LOGICAL_SCREEN_W, LOGICAL_SCREEN_H);

    Log_Print("Systems initialized\n");
}

static void ShutdownSystems(void) {
    Log_Print("Shutting down...\n");
    Window_Destroy();
    timeEndPeriod(1);
    Log_Flush();
}

#include "bga.h"

static void LoadBGAForState(GameState state) {
    const char* bgaName = NULL;
    switch (state) {
    case STATE_WARNING_INIT:
    case STATE_WARNING_ANIM: bgaName = "R_WARN"; break;   /* Exceed2: BGA\R_WARN.DAT (PIU32.EXE); Exceed usava R_WARN_A */
    case STATE_INTRO:        bgaName = ""; break;       /* vídeo, sem BGA */
    case STATE_CREDIT:       bgaName = "82"; break;
    case STATE_NAMEINPUT:    bgaName = "085"; break;    /* CNameInput 0x414012: BGA\085.DAT */
    case STATE_IR:           bgaName = "IR"; break;     /* CInternetRanking 0x41371B: BGA\IR.DAT */
    case STATE_STATION:      bgaName = ""; break;       /* STATION.DAT carregado em Station_Enter */
    case STATE_HIGHSCORE:    bgaName = "HS"; break;     /* CHighscore 0x4126E7: BGA\HS.DAT */     /* CTitle::Begin 0x41C04A: BGA\82.DAT sobre o CREDIT.MOV */
    case STATE_LOGO_ENTER:   bgaName = "81"; break;
    case STATE_MENU_ENTER:
    case STATE_MENU_INPUT:
    case STATE_LOGO_SKIP:    bgaName = "82w"; break;
    case STATE_GAME_INIT:
    case STATE_GAMEPLAY:
        if (g_game.selectedSongIndex >= 0 && g_game.selectedSongIndex < g_game.songDB.songCount) {
            static char songBGAName[16];
            snprintf(songBGAName, sizeof(songBGAName), "%s", Song_DataIdStr(g_game.songDB.songs[g_game.selectedSongIndex].id));
            bgaName = songBGAName;
        } else {
            bgaName = "00";
        }
        break;
    case STATE_GAMEOVER:         bgaName = "84"; break;
    case STATE_SONG_SELECT:
    case STATE_SONG_SELECT_B: bgaName = "099"; break;
    case STATE_SONG_TITLE:
    case STATE_SONG_TITLE_OUT: bgaName = ""; break;
    case STATE_STAFF_ENTER:
    case STATE_STAFF:
    case STATE_STAFF_END:        bgaName = ""; break;
    case STATE_STAGE_TRANSITION:
        if (g_exceedSongIds)            /* Exceed: BGA\NST%d.MOV (sem .DAT) */
            bgaName = "";
        else if (g_game.isBonusSong)
            bgaName = "lt03";
        else
            bgaName = "lt01";
        break;
    case STATE_GAMEOVER_ENTER:   bgaName = g_exceedSongIds ? "" : "84"; break; /* Exceed: BGA\GAMEOVER.MOV */
    case STATE_STAGE_BREAK:           /* Prex3: 083.DAT + 7-1.WAV; Exceed: BGA\SB.MOV (sem .DAT) */
        bgaName = g_exceedSongIds ? "" : "083"; break;
    case STATE_DANCE_GRADE_ENTER:
    case STATE_DANCE_GRADE_DISPLAY: bgaName = g_exceedSongIds ? "GRADE" : "83"; break; /* Exceed 0x40CD2A: BGA\GRADE.DAT */
    case STATE_HOWTOPLAY: bgaName = ""; break; /* limpa BGA do menu imediatamente; 03.DAT carrega no stateFrame==1 */
    case STATE_SERVICE_MENU: bgaName = ""; break; /* SETUP MENU desenha sobre fundo preto */
    default: break;
    }

    if (bgaName) {
        if (bgaName[0] == '\0') {
            Resource_ClearBGA();
        } else if (g_game.bgaPicCount == 0 || _stricmp(g_game.bgaPics[0].name, bgaName) != 0) {
            int idx = Resource_SwitchBGA(bgaName);
            if (idx < 0 && (state == STATE_SONG_SELECT || state == STATE_SONG_SELECT_B ||
                            state == STATE_STAFF_ENTER)) {
                char directPath[MAX_PATH];
                snprintf(directPath, sizeof(directPath), "%s/BGA/%s.DAT",
                         g_game.currentDirectory, bgaName);
                Log_Print("BGA: fallback loading '%s'\n", directPath);
                Resource_LoadBGADirect(directPath);
            }
        }
    }

    /* Exceed CSelect: SELECT.DAT + SELECT2.DAT juntos + banners do 90.DAT */
    if (state == STATE_EXSELECT) {
        BGM_Stop();
        ExSelect_Enter();
    }

    if (state == STATE_MENU_ENTER) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s/AUDIO/082.AUD", g_game.currentDirectory);
        BGM_Stop();
        if (BGM_LoadAUDDirect(path)) BGM_Play(true);
    } else if (state == STATE_SONG_SELECT || state == STATE_SONG_SELECT_B) {
        BGM_Stop();
        /* DESATIVADO (Select do Prex3):
        if (g_game.stageCount == 3)
            SongSelect_Reset();
        else
            SongSelect_ResetIntro(); */
    }
    
    g_game.bgaLoop = (state == STATE_MENU_ENTER || state == STATE_MENU_INPUT ||
                      state == STATE_EXIT);
    if (g_game.bgaLoop) {
        g_game.bgaLoopStart = findBGALoopStart();
        g_game.bgaLoopEnd = findBGALoopEnd();
        g_game.bgaFrame = g_game.bgaLoopStart;
        Log_Print("BGA: loop range %d -> %d (maxFrame=%d)\n",
                  g_game.bgaLoopStart, g_game.bgaLoopEnd,
                  g_game.bgaMaxFrame);
    } else {
        g_game.bgaLoop = false;
        if (state == STATE_SONG_SELECT || state == STATE_SONG_SELECT_B) {
            g_game.bgaFrame = 0;
            if (g_game.bgaMaxFrame > 0) {
                g_game.bgaLoopStart = 0;
                g_game.bgaLoopEnd = 600;
                g_game.bgaLoop = true;
            }
        }
    }
}

/* Reseta todos os cheats de todos os players (ESC e Game Over). */
void Game_ResetAllCheats(void) {
    for (int _p = 0; _p < 2; _p++) {
        g_game.cmdSpeedMult[_p]      = 1;
        g_game.cmdMirror[_p]         = false;
        g_game.cmdRandomStep[_p]     = false;
        g_game.cmdRandomVelocity[_p] = false;
        g_game.cmdEarthworm[_p]      = false;
        g_game.cmdFreedom[_p]        = false;
        g_game.cmdVanish[_p]         = false;
        g_game.cmdNonStep[_p]        = false;
        g_game.cmdTestBGA[_p]        = false;
    }
    Log_Print("CHEATS: reset global (ESC/GameOver)\n");
}

void Game_ChangeState(GameState newState) {
    Log_Print("State: %s -> %s\n",
              State_ToString(g_game.state), State_ToString(newState));
    g_game.state = newState;
    g_game.nextState = newState;
    g_game.stateFrame = 0;
    g_game.confirmActive = false;
    g_game.confirmTimer = 0;
    Render_SetGlobalColor(0, 0, 0, 0);
    LoadBGAForState(newState);
}

void Game_Init(HINSTANCE hInstance) {
    memset(&g_game, 0, sizeof(g_game));
    g_game.hInstance = hInstance;
    /* screenWidth/Height são as dimensões LÓGICAS, usadas como coordenada de
     * desenho em vários lugares (Font_DrawStringCentered(screenWidth/2, ...)).
     * Não são o tamanho da janela — esse fica em Window_GetSize(). */
    g_game.screenWidth = LOGICAL_SCREEN_W;
    g_game.screenHeight = LOGICAL_SCREEN_H;
    /* Vsync ligado: o SDL_GL_SwapWindow passa a ser quem dá o ritmo do loop.
     * Precisa estar definido ANTES do InitSystems, que é quem cria a janela e
     * lê este campo no SDL_GL_SetSwapInterval. */
    g_game.vsync = true;
    g_game.confirmActive = false;
    g_game.confirmTimer = 0;
    g_game.globalScaleX = 1.0f;
    g_game.globalScaleY = 1.0f;
    g_game.globalAlpha = 1.0f;
    /* g_game.showDebug = true; */
    g_game.showDebug = false;  /* debug oculto ao abrir; F11 alterna (Game_Update) */
    /* g_game.stageBreak = 1; */ /* DISABLED: controlado por optionToggle1 no GameOption */
    g_game.showHelp = 0;
    g_game.cmdSpeedMult[0] = 1;    /* Command P1: velocidade padrão x1 */
    g_game.cmdSpeedMult[1] = 1;    /* Command P2: velocidade padrão x1 */
    g_game.activePlayerMask = 0x1; /* P1 ativo por padrão */
    g_game.isBattleMode = false;   /* BATTLE só ativo quando selecionado no song_select */
    Render_SetGlobalColor(0, 0, 0, 0);
    GetCurrentDirectoryA(MAX_PATH, g_game.currentDirectory);
    GameOption_Load(); /* lê PUMPY.INI — antes de qualquer sistema, para que o restante já veja os valores corretos */
    InitSystems();
    Audio_Init();
    Font_Init();
    Texture_Init();
    Audio_LoadAllWaves();
    Ranking_RegisterDefaults();  /* Game_InitState faz isso em 0x0040517c */

    Log_Print("Loading song database...\n");
    char cfgPath[MAX_PATH];
    char stepDir[MAX_PATH];
    snprintf(cfgPath, sizeof(cfgPath), "%s/Stage.cfg", g_game.currentDirectory);
    snprintf(stepDir, sizeof(stepDir), "%s/STEP", g_game.currentDirectory);
    bool dbOk = Song_LoadDatabase(cfgPath, &g_game.songDB);
    if (!dbOk) {
        /* Exceed: sem Stage.cfg, as músicas vêm das tabelas do exceed.exe */
        ExSelect_BuildSongDB(&g_game.songDB);
        dbOk = g_game.songDB.songCount > 0;
    }
    if (dbOk)
    {
        Log_Print("Song database: %d songs, %d modes\n",
            g_game.songDB.songCount, g_game.songDB.modeCount);
        for (int i = 0; i < g_game.songDB.songCount; i++)
        {
            SongEntry* e = &g_game.songDB.songs[i];
            char stxPath[MAX_PATH];
            snprintf(stxPath, sizeof(stxPath), "%s/%s.STX", stepDir, Song_DataIdStr(e->id));
            FILE* test = fopen(stxPath, "rb");
            if (test) { fclose(test); e->hasChart = true; }
            else {
                /* Exceed2: .STX dentro de STEP.DAT (PIU32.EXE 0x40d0d7) */
                char datPath[MAX_PATH], stxName[64];
                uint32_t sz = 0;
                snprintf(datPath, sizeof(datPath), "%s/STEP.DAT", g_game.currentDirectory);
                snprintf(stxName, sizeof(stxName), "%s.STX", Song_DataIdStr(e->id));
                uint8_t* stx = Resource_ExtractFromPack(datPath, stxName, &sz);
                if (stx) { free(stx); e->hasChart = true; }
            }
        }
        g_game.selectedSongIndex = 0;
        g_game.selectedModeIndex = Song_FindMode(&g_game.songDB, "EASY");
        if (g_game.selectedModeIndex < 0 && g_game.songDB.modeCount > 0)
            g_game.selectedModeIndex = 0;
        g_game.songSelectScroll = 0;
        g_game.songSelectHighlighted = 0;
        g_game.selectedDifficulty = 0;
        g_game.previewSongId = -1;
    }
    else
    {
        Log_Print("WARNING: Failed to load song database from '%s'\n", cfgPath);
    }

    Game_ChangeState(STATE_WARNING_INIT); /* Exceed: R_WARN_A -> 81 -> INTRO -> CREDIT */
    g_game.lastTime = timeGetTime();
}

void Game_Shutdown(void) {
    Audio_Shutdown();
    BGM_Shutdown();
    Font_Shutdown();
    ShutdownSystems();
}



void Game_Update(float dt) {
    Input_Update();
    BGM_Update();   /* reinicia a faixa quando ela está em loop (ver audio.c) */
    if (Input_IsKeyHit(VK_F11)) g_game.showDebug = !g_game.showDebug;

    /* Crase abre/fecha o console de debug (0x29 no original) */
    if (Input_IsKeyHit(VK_OEM_3)) Debug_ConsoleToggle();

    /* Com o console aberto o jogo não enxerga teclado nem pad — as teclas são
     * entregues ao console pelo WndProc. O resto do update segue rodando para
     * as animações não congelarem. */
    if (Debug_ConsoleIsActive()) {
        memset(g_game.input.keys, 0, sizeof(g_game.input.keys));
        memset(g_game.input.padState, 0, sizeof(g_game.input.padState));
    }

    /* Alt+F4: encerra o jogo imediatamente de qualquer tela */
    if (Input_IsKeyHit(VK_F4) && Input_IsKeyDown(VK_MENU))
        Window_RequestQuit();

    /* F1 abre o SETUP MENU de qualquer tela. Dentro do menu o F1 passa a ser o
     * TEST BUTTON (percorre a lista) e o F2 o SERVICE BUTTON (confirma) —
     * tratados em service_menu.c. */
    /* Botoeira do gabinete, na ordem que o I/O TEST do original lista:
     *   F1 TEST | F2 SERVICE | F3 CLEAR | F4 COIN1 | F5 COIN2
     * Fora do SETUP MENU o SERVICE dá crédito de cortesia (type 3, o mesmo que
     * incrementa g_nServiceTotal no original); dentro dele vira SELECT e o
     * I/O TEST lê as teclas por conta própria. */
    if (g_game.state != STATE_SERVICE_MENU) {
        if (Input_IsKeyHit(VK_F4)) Arcade_ProcessCoin(1);  /* COIN1   */
        if (Input_IsKeyHit(VK_F5)) Arcade_ProcessCoin(2);  /* COIN2   */
        if (Input_IsKeyHit(VK_F2)) Arcade_ProcessCoin(3);  /* SERVICE */
    }

    if (Input_IsKeyHit(VK_F1) && g_game.state != STATE_SERVICE_MENU) {
        ServiceMenu_Enter();
        /* Consome a borda do F1 antes de sair: este return pula o
         * memcpy(prevKeys, keys) do fim de Game_Update, e sem isso o mesmo
         * press seria lido de novo no frame seguinte, já como MOVE. */
        memcpy(g_game.input.prevKeys, g_game.input.keys, sizeof(g_game.input.keys));
        return;
    }

    if (Input_IsKeyHit(VK_ESCAPE)) {
        GameState s = g_game.state;
        /* Consome a borda do ESC (mesmo motivo do F1 acima): os returns abaixo
         * pulam o memcpy do fim de Game_Update e o ESC seria lido de novo no
         * frame seguinte, pulando duas cenas (WARN -> LOGO -> MENU). */
        memcpy(g_game.input.prevKeys, g_game.input.keys, sizeof(g_game.input.keys));

        /* Exceed: ESC no CREDIT não faz nada (já é o destino do ESC) */
        if (s == STATE_CREDIT) return;

        BGM_Stop();
        Menu_ResetState();
        memset(g_game.input.padPrevState, 0, sizeof(g_game.input.padPrevState));

        /* Exceed: WARN -> LOGO -> CREDIT; o resto vai para o CREDIT */
        if (s == STATE_WARNING_INIT || s == STATE_WARNING_ANIM || s == STATE_WARNING_END) {
            Game_ChangeState(STATE_LOGO_ENTER);
            return;
        }

        if (s == STATE_STAFF || s == STATE_STAFF_ENTER) {
            Game_ChangeState(STATE_CREDIT);
            return;
        }

        /* ESC no SETUP MENU sai como a opção EXIT (página 9 → estado 4) */
        if (s == STATE_SERVICE_MENU) {
            ServiceMenu_Exit();
            return;
        }

        /* ESC de qualquer tela de jogo reseta os ponteiros de música por modo */
        /* SongSelect_ResetCreditIndices(); */   /* DESATIVADO (Select do Prex3) */

        if (s == STATE_GAMEPLAY || s == STATE_GAME_INIT) {
            Game_ResetAllCheats(); /* ESC durante gameplay: zera todos os cheats */
            Resource_ClearBGA();
            Game_ChangeState(STATE_CREDIT);
            return;
        }

        Resource_ClearBGA();
        Game_ChangeState(STATE_CREDIT);
    }

    g_game.stateFrame++;


    if (g_game.bgaPicCount > 0 && g_game.state != STATE_WARNING_END) {
        bool manualBGA = (g_game.state == STATE_GAMEPLAY ||
                          g_game.state == STATE_DANCE_GRADE_DISPLAY ||
                          g_game.state == STATE_GAMEOPTION_ENTER ||
                          g_game.state == STATE_GAMEOPTION_ANIM ||
                          g_game.state == STATE_GAMEOPTION ||
                          g_game.state == STATE_GAMEOPTION_EXIT ||
                          g_game.state == STATE_STAFF);
        if (!manualBGA) {
            g_game.bgaTimer += dt;
            if (g_game.bgaTimer >= 1.0f / 60.0f) {
                g_game.bgaTimer -= 1.0f / 60.0f;
                if (g_game.bgaLoop && g_game.bgaLoopEnd > g_game.bgaLoopStart) {
                    g_game.bgaFrame++;
                    if (g_game.bgaFrame > g_game.bgaLoopEnd) {
                        Log_Print("BGA: wrap %d -> %d\n", g_game.bgaFrame, g_game.bgaLoopStart);
                        g_game.bgaFrame = g_game.bgaLoopStart;
                    }
                } else {
                    if (g_game.bgaFrame < g_game.bgaMaxFrame) {
                        g_game.bgaFrame++;
                    }
                }
            }
        }
    } else if (g_game.isVSL && g_vsl.active && g_game.bgaFrame < g_vsl.frameCount - 1) {
        g_game.bgaTimer += dt;
        if (g_game.bgaTimer >= 1.0f / 60.0f) {
            g_game.bgaTimer -= 1.0f / 60.0f;
            g_game.bgaFrame++;
            static int lastFrameAdvLog = -1;
            if (abs(g_game.bgaFrame - lastFrameAdvLog) >= 30) {
                Log_Print("VSL: bgaFrame advanced to %d (timer=%.4f, dt=%.4f)\n", g_game.bgaFrame, g_game.bgaTimer, dt);
                lastFrameAdvLog = g_game.bgaFrame;
            }
        }
    }

    switch (g_game.state) {
    case STATE_WARNING_INIT:
    case STATE_WARNING_ANIM:
    case STATE_WARNING_END:
        Gamestate_UpdateWarning(dt);
        break;
    case STATE_LOGO_ENTER:
    case STATE_LOGO_UPDATE:
    case STATE_LOGO_SKIP:
        Gamestate_UpdateLogo(dt);
        break;
    case STATE_INTRO:
    case STATE_CREDIT:
        Gamestate_UpdateIntro(dt);
        break;
    case STATE_HIGHSCORE:
        Highscore_Update(dt);
        break;
    case STATE_IR:
        IR_Update(dt);
        break;
    case STATE_NAMEINPUT:
        NameInput_Update(dt);
        break;
    case STATE_EXSELECT:
        ExSelect_Update(dt);
        break;
    case STATE_STATION:
        Station_Update(dt);
        break;
    case STATE_MENU_ENTER:
    case STATE_MENU_INPUT:
        /* Gamestate_UpdateMenu(dt); */   /* DESATIVADO (menu do Prex3) */
        break;
    case STATE_SONG_SELECT:
    case STATE_SONG_SELECT_B:
        /* Gamestate_UpdateSongSelect(dt); */   /* DESATIVADO (Select do Prex3) */
        break;
    case STATE_SONG_TITLE:
    case STATE_SONG_TITLE_OUT:
        Loading_Update(dt);
        break;
    case STATE_GAMEPLAY:
        if (Movie_IsOpen()) Movie_Update(dt);   /* BGA\%s.MOV da música */
        Gameplay_Update(dt);
        break;
    case STATE_DANCE_GRADE_ENTER:
        if (g_exDemo) { Demo_End(); break; }    /* a demo não tem Grade */
        if (g_game.stateFrame == 1) {
            Movie_Close();
            Result_Enter();
        }
        if (Movie_IsOpen()) Movie_Update(dt);   /* Exceed2: GRADE.MOV (0x40A942) */
        Result_Update(dt);
        break;
    case STATE_DANCE_GRADE_DISPLAY:
        if (Movie_IsOpen()) Movie_Update(dt);   /* Exceed2: GRADE.MOV (0x40A984) */
        Result_Update(dt);
        break;
    case STATE_STAGE_TRANSITION:
        if (g_game.stateFrame == 1) {
            Log_Print("STAGE: transition count=%d bonus=%d\n", g_game.stageCount, g_game.bonusStage);
        }
        {
            /* Exceed "NEXTSTAGE n" (0x415078): n = [0x568FF8]+1 (0x40D131) —
             * 1 depois do stage 1, 2 depois do stage 2, 3 para o extra.
             * Abre BGA\NST<n>.MOV sem loop e toca NEXTSTAGE.WAV do início.
             * 0x4151F4: com decoded >= 57 (0x39) vai para SELECT. */
            bool exDone = false;
            if (g_exceedSongIds) {
                if (g_game.stateFrame == 1) {
                    int n = (g_game.stageCount > 0) ? 3 - g_game.stageCount : 3;
                    if (n < 1) n = 1;
                    char path[MAX_PATH];
                    snprintf(path, sizeof(path), "%s/BGA/NST%d.MOV", g_game.currentDirectory, n);
                    Movie_Close();
                    if (!Movie_Open(path, false))
                        Log_Print("NST: '%s' não abriu\n", path);
                    if (g_waveSoundIds[SND_NEXTSTAGE] >= 0) {
                        Audio_Stop(g_waveSoundIds[SND_NEXTSTAGE]);
                        Audio_Play(g_waveSoundIds[SND_NEXTSTAGE], false);
                    }
                }
                Movie_Update(dt);
                exDone = !Movie_IsOpen() || Movie_GetDecoded() >= 57;
                if (exDone) Movie_Close();
            }
        if (g_exceedSongIds ? exDone : (g_game.stateFrame >= 60)) {
            if (g_game.bonusStage && g_game.stageCount == 0 && !g_game.isBonusSong) {
                // Vai pro bonus stage
                /* Exceed: volta para a CSelect */
                Game_ChangeState(g_exceedSongIds ? STATE_EXSELECT : STATE_SONG_SELECT);
            } else if (g_game.isBonusSong) {
                // Bonus terminou, game over
                Game_ChangeState(STATE_GAMEOVER_ENTER);
            } else if (g_game.stageCount == 0 && !g_game.bonusStage) {
                // Sem bonus, game over
                Game_ChangeState(STATE_GAMEOVER_ENTER);
            } else {
                // Proximo stage
                Game_ChangeState(g_exceedSongIds ? STATE_EXSELECT : STATE_SONG_SELECT);
            }
        }
        }
        break;
    case STATE_STAGE_BREAK:
        if (g_exDemo) { Demo_End(); break; }
        if (g_exceedSongIds) {
            /* exceed.exe 0x4152B0 (Begin): 0x4229C8("BGA\SB.MOV", 0) sem loop;
             * se falhar vai para IDLE (aqui: GameOver). Toca GAMESTOP.WAV
             * do início ([0xA6D2F0]: Stop, SetCurrentPosition(0), Play).
             * 0x41532C (Update): avança o vídeo e, com o cronômetro
             * >= 4500 ms (0x1194), vai para GAMEOVER. */
            static uint32_t sbStart;
            if (g_game.stateFrame == 1) {
                Game_ResetAllCheats();
                Movie_Close();
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s/BGA/SB.MOV", g_game.currentDirectory);
                if (!Movie_Open(path, false)) {
                    Log_Print("SB: '%s' não abriu\n", path);
                    /* Game_ChangeState(STATE_GAMEOVER_ENTER); */
                    Attract_Idle();     /* 0x4152E0: "IDLE" */
                    break;
                }
                if (g_waveSoundIds[SND_GAMESTOP] >= 0) {
                    Audio_Stop(g_waveSoundIds[SND_GAMESTOP]);
                    Audio_Play(g_waveSoundIds[SND_GAMESTOP], false);
                }
                sbStart = timeGetTime();
            }
            Movie_Update(dt);
            if (timeGetTime() - sbStart >= 4500) {
                Movie_Close();
                Game_ChangeState(STATE_GAMEOVER_ENTER);
            }
            break;
        }
        /* 083.DAT aparece enquanto 7-1.WAV toca; quando termina → GameOver */
        if (g_game.stateFrame == 1) {
            Game_ResetAllCheats();
            Audio_Play(g_waveSoundIds[SND_7_1], false);
        }
        if (g_game.stateFrame > 2 && !Audio_IsPlaying(g_waveSoundIds[SND_7_1])) {
            Game_ChangeState(STATE_GAMEOVER_ENTER);
        }
        break;
    case STATE_GAMEOVER_ENTER:
        if (g_exceedSongIds) {
            /* exceed.exe 0x415224: BGA\GAMEOVER.MOV sem loop (falha -> IDLE);
             * 0x41527C: cronômetro >= 4000 ms (0xFA0) -> IDLE. Sem som. */
            static uint32_t goStart;
            if (g_game.stateFrame == 1) {
                BGM_Stop();
                Game_ResetAllCheats();
                Render_SetGlobalColor(0, 0, 0, 0);
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s/BGA/GAMEOVER.MOV", g_game.currentDirectory);
                Movie_Close();
                if (!Movie_Open(path, false))
                    Log_Print("GAMEOVER: '%s' não abriu\n", path);  /* 0x41525D: -> IDLE (abaixo) */
                goStart = timeGetTime();
            }
            Movie_Update(dt);
            if (!Movie_IsOpen() || timeGetTime() - goStart >= 4000) {
                Movie_Close();
                Resource_ClearBGA();
                Menu_ResetState();
                /* Game_ChangeState(STATE_MENU_ENTER); */
                Attract_Idle();     /* 0x41527C: "IDLE" */
            }
            break;
        }
        if (g_game.stateFrame == 1) {
            BGM_Stop();
            Game_ResetAllCheats(); /* Game Over: zera todos os cheats (centralizado) */
            Render_SetGlobalColor(0, 0, 0, 0);
        }
        if (g_game.stateFrame >= 120) {
            Render_SetGlobalColor(0, 0, 0, 1);
        }
        if (g_game.stateFrame >= 150) {
            Resource_ClearBGA();
            Menu_ResetState();
            Game_ChangeState(STATE_MENU_ENTER);
        }
        break;
    case STATE_STAFF_ENTER:
        /* Staff_Enter(); */   /* DESATIVADO (Staff do Prex3) */
        break;
    case STATE_STAFF:
        /* Staff_Update(dt); */   /* DESATIVADO (Staff do Prex3) */
        break;
    case STATE_STAFF_END:
        BGM_Stop();
        Menu_ResetState();
        Game_ChangeState(STATE_MENU_ENTER);
        break;
    case STATE_GAMEOPTION_ENTER:
    case STATE_GAMEOPTION_ANIM:
    case STATE_GAMEOPTION:
    case STATE_GAMEOPTION_EXIT:
        /* Gamestate_UpdateGameOption(dt); */   /* DESATIVADO (Game Option do Prex3) */
        break;
    case STATE_SERVICE_MENU:
        /* Só captura o input aqui; o desenho e a aplicação ficam em
         * ServiceMenu_UpdateRender, no Game_Render. A captura precisa ser
         * nesta fase porque Game_Update termina zerando as bordas do teclado
         * com memcpy(prevKeys, keys). */
        ServiceMenu_Update();
        break;
    case STATE_HOWTOPLAY:
        /* Frame 1: carrega 03.DAT e inicia 003.AUD (igual ao padrão GAMEOPTION_ENTER) */
        if (g_game.stateFrame == 1) {
            Resource_SwitchBGA("03");
            g_game.bgaFrame = 0;
            g_game.bgaLoop  = false;
            {
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s/AUDIO/003.AUD", g_game.currentDirectory);
                BGM_Stop();
                if (BGM_LoadAUDDirect(path)) BGM_Play(true); /* loop=true: igual demais AUDs do jogo */
            }
        }
        /* Sai para SongSelect: CN do player ativo OU BGA terminou */
        if (g_game.stateFrame > 30) {
            bool skipCN = false;
            if (g_game.activePlayerMask & 0x1) skipCN |= Input_IsPadHit(0, PAD_C);
            if (g_game.activePlayerMask & 0x2) skipCN |= Input_IsPadHit(1, PAD_C);
            bool bgaEnded = (g_game.bgaMaxFrame > 0 && g_game.bgaFrame >= g_game.bgaMaxFrame);
            if (skipCN || bgaEnded) {
                BGM_Stop();
                Game_ChangeState(STATE_SONG_SELECT);
            }
        }
        break;
    case STATE_RESET_WARNING:
    Game_ChangeState(STATE_LOGO_ENTER);
        break;
    case STATE_EXIT:
        if (g_game.stateFrame < 30) {
            float a = g_game.globalColorA + dt * 2.0f;
            if (a > 1.0f) a = 1.0f;
            Render_SetGlobalColor(0, 0, 0, a);
        }
        if (g_game.stateFrame >= 30)
            Window_RequestQuit();
        break;
    default:
        break;
    }
    memcpy(g_game.input.prevKeys, g_game.input.keys, sizeof(g_game.input.keys));
}

static void Render_StateInfo(void) {
    static uint32_t lastFpsTime = 0;
    static int fpsCount = 0;
    static float currentFps = 0;
    char buf[256];
    int y = -4;

    fpsCount++;
    uint32_t now = timeGetTime();
    if (now - lastFpsTime >= 1000) {
        currentFps = fpsCount / ((now - lastFpsTime) / 1000.0f);
        fpsCount = 0;
        lastFpsTime = now;
    }

    // FPS no canto inferior direito
    snprintf(buf, sizeof(buf), "FPS: %.1f", currentFps);
    int fw = (int)strlen(buf) * 8;
    Font_DrawString(g_game.screenWidth - fw - 8, 20,
                    buf, 1.0f, 1.0f, 0.0f, 1.0f);

    snprintf(buf, sizeof(buf), "State: %s  Frame: %u/%u",
             State_ToString(g_game.state), g_game.stateFrame, g_game.frameCounter);
    Font_DrawString(8, y, buf, 1.0f, 1.0f, 0.0f, 1.0f);
    y += 16;

    if (g_game.isVSL && g_vsl.active) {
        int songId = 0;
        if (g_game.selectedModeIndex >= 0 && g_game.selectedModeIndex < g_game.songDB.modeCount &&
            g_game.selectedSongIndex >= 0) {
            SongMode* mode = &g_game.songDB.modes[g_game.selectedModeIndex];
            if (g_game.selectedSongIndex < mode->songCount)
                songId = mode->songIds[g_game.selectedSongIndex];
        }
        snprintf(buf, sizeof(buf), "BGA: %d.DAT - VSL | frame=%d/%d | meshs=%d",
                 songId, g_game.bgaFrame, g_vsl.frameCount, g_vsl.meshTableCount);
        Font_DrawString(8, y, buf, 1.0f, 1.0f, 0.0f, 1.0f);
        y += 16;
    } else if (g_game.bgaPicCount > 0) {
        BGAPicture* pic = &g_game.bgaPics[0];
        snprintf(buf, sizeof(buf), "BGA: %s | layers=%d | frame=%d/%d | tiles=%d",
                 pic->name, pic->layerCount, g_game.bgaFrame, g_game.bgaMaxFrame, g_game.sprTileCount);
        Font_DrawString(8, y, buf, 1.0f, 1.0f, 0.0f, 1.0f);
        y += 16;
    } else {
        snprintf(buf, sizeof(buf), "BGA: (none loaded)");
        Font_DrawString(8, y, buf, 1.0f, 1.0f, 0.0f, 1.0f);
        y += 16;
    }
    
    if (g_game.bgm.buffer || g_game.bgm.useMCI) {
        snprintf(buf, sizeof(buf), "BGM: %s | mode=%s | playing=%d",
                 g_game.bgm.name[0] ? g_game.bgm.name : "(unnamed)",
                 g_game.bgm.useMCI ? "MCI" : "DSound",
                 g_game.bgm.playing);
        Font_DrawString(8, y, buf, 1.0f, 1.0f, 0.0f, 1.0f);
        y += 16;
    }
    
    if (g_game.state == STATE_SONG_SELECT || g_game.state == STATE_SONG_SELECT_B) {
        if (g_game.selectedModeIndex >= 0 && g_game.selectedModeIndex < g_game.songDB.modeCount) {
            SongMode* mode = &g_game.songDB.modes[g_game.selectedModeIndex];
            snprintf(buf, sizeof(buf), "Mode: %s | Song %d/%d | Preview ID=%d",
                     mode->name, g_game.songSelectHighlighted + 1, mode->songCount,
                     g_game.previewSongId);
            Font_DrawString(8, y, buf, 1.0f, 1.0f, 0.0f, 1.0f);
            y += 16;
        }
    }
}

void Game_Render(void) {
    Gameplay_RefreshClock();
    /* Rendering subsystems such as VSL may leave either matrix selected.
     * Re-establish the fixed 640x480 2D transform for every frame. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, 640, 0, 480, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glLoadIdentity();

    /* Keep a missing-assets run visibly diagnosable instead of presenting a
     * featureless black window. Normal game rendering is unchanged once the
     * BGA/song assets have loaded. */
    bool missingAssets = (g_game.songDB.songCount == 0);
    if (missingAssets) {
        glDisable(GL_TEXTURE_2D);
        glColor4f(0.035f, 0.055f, 0.09f, 1.0f);
        glBegin(GL_QUADS);
        glVertex2f(0, 0);
        glVertex2f(640, 0);
        glVertex2f(640, 480);
        glVertex2f(0, 480);
        glEnd();
        glColor4f(1, 1, 1, 1);
        Font_DrawStringCentered(320, 190, "PUMP IT UP", 1.0f, 0.85f, 0.1f, 1.0f);
        Font_DrawStringCentered(320, 225, "ASSETS NOT FOUND", 1.0f, 1.0f, 1.0f, 1.0f);
        Font_DrawStringCentered(320, 250, "Stage.cfg / BGA / AUDIO / WAVE", 0.7f, 0.8f, 0.9f, 1.0f);
        Font_DrawStringCentered(320, 285, g_game.currentDirectory, 0.6f, 0.7f, 0.8f, 1.0f);
    }

    // No background fill for states that have full-screen BGA
    if (!missingAssets && g_game.state != STATE_WARNING_INIT && g_game.state != STATE_WARNING_ANIM && 
        g_game.state != STATE_WARNING_END &&
        g_game.state != STATE_LOGO_ENTER && g_game.state != STATE_LOGO_UPDATE &&
        g_game.state != STATE_LOGO_SKIP) {
        glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
        glBegin(GL_QUADS);
        glVertex2f(0, 0);
        glVertex2f(640, 0);
        glVertex2f(640, 480);
        glVertex2f(0, 480);
        glEnd();
        glColor4f(1, 1, 1, 1);
    }

    /* Exceed2: GRADE.MOV desenhado antes do GRADE.DAT nas duas fases da nota
     * (PIU32.EXE 0x40A942 / 0x40A984: 0x420C80 e só depois o BGA) */
    if (g_exceedSongIds && Movie_IsOpen() &&
        (g_game.state == STATE_DANCE_GRADE_ENTER || g_game.state == STATE_DANCE_GRADE_DISPLAY))
        Movie_Render();

    if ((g_game.cmdTestBGA[0] || g_game.cmdTestBGA[1]) &&
               (g_game.state == STATE_GAMEPLAY || g_game.state == STATE_GAMEPLAY_BEGIN)) {
        DrawStar(); /* Extra do port: BGA Off (TestBGA) substitui o BGA/VSL da música */
    } else if (g_game.isVSL && g_vsl.active) {
        VSL_Render(g_game.bgaFrame);
    } else if (g_game.bgaPicCount > 0 &&
        g_game.state != STATE_LOGO_SKIP &&
        g_game.state != STATE_DANCE_GRADE_DISPLAY &&
        g_game.state != STATE_GAMEOPTION_ENTER &&
        g_game.state != STATE_GAMEOPTION_ANIM &&
        g_game.state != STATE_GAMEOPTION &&
        g_game.state != STATE_GAMEOPTION_EXIT &&
        g_game.state != STATE_SONG_SELECT &&
        g_game.state != STATE_SONG_SELECT_B &&
        g_game.state != STATE_CREDIT &&         /* CREDIT desenha por slot (intro.c) */
        g_game.state != STATE_EXSELECT &&
        g_game.state != STATE_STATION &&        /* STATION desenha por cena (station.c) */
        g_game.state != STATE_HIGHSCORE &&
        g_game.state != STATE_IR &&
        g_game.state != STATE_NAMEINPUT) {      /* NAMEINPUT desenha por slot (nameinput.c) */             /* IR desenha o próprio BGA (ir.c) */      /* HIGHSCORE desenha por slot (highscore.c) */       /* EXSELECT desenha por slot (exceed_select.c) */
        BGA_Render(0, g_game.bgaFrame);
    }

    switch (g_game.state) {
        case STATE_INTRO:
        case STATE_CREDIT:
            Gamestate_RenderIntro();
            break;
        case STATE_HIGHSCORE:
            Highscore_Render();
            break;
        case STATE_IR:
            IR_Render();
            break;
        case STATE_NAMEINPUT:
            NameInput_Render();
            break;
        case STATE_EXSELECT:
            ExSelect_Render();
            break;
        case STATE_STATION:
            Station_Render();
            break;
        case STATE_MENU_ENTER:
        case STATE_MENU_INPUT:
        case STATE_EXIT:
            /* Gamestate_RenderMenu(0, g_game.bgaFrame); */   /* DESATIVADO (menu do Prex3) */
            break;
    case STATE_SONG_SELECT:
    case STATE_SONG_SELECT_B:
        /* Gamestate_RenderSongSelect(); */   /* DESATIVADO (Select do Prex3) */
        break;
    case STATE_SONG_TITLE:
    case STATE_SONG_TITLE_OUT:
        Loading_Render();
        break;
    case STATE_GAMEPLAY:
        if (Movie_IsOpen() && !g_game.cmdTestBGA[0] && !g_game.cmdTestBGA[1])
            Movie_Render();     /* fundo no lugar do .DAT; TestBGA (extra do port) substitui */
        Gameplay_Render();
        break;
    case STATE_STAGE_BREAK:
        if (g_exceedSongIds && Movie_IsOpen()) Movie_Render();   /* SB.MOV */
        break;
    case STATE_DANCE_GRADE_DISPLAY:
        Result_Render();
        break;
    case STATE_STAGE_TRANSITION:
        if (g_exceedSongIds) {
            if (Movie_IsOpen()) Movie_Render();   /* NST<n>.MOV */
        } else if (g_game.isBonusSong) {
            Font_DrawStringCentered(320, 240, "BONUS STAGE", 1, 1, 0, 1);
        } else if (g_game.stageCount > 0) {
            Font_DrawStringCentered(320, 240, "NEXT STAGE", 1, 1, 1, 1);
        } else {
            Font_DrawStringCentered(320, 240, "FINAL STAGE", 1, 0, 0, 1);
        }
        break;
    case STATE_GAMEOVER_ENTER:
        if (g_exceedSongIds) {
            if (Movie_IsOpen()) Movie_Render();   /* GAMEOVER.MOV */
        } else
        Font_DrawStringCentered(320, 240, "GAME OVER", 1, 0, 0, 1);
        break;
    case STATE_GAMEOPTION_ENTER:
    case STATE_GAMEOPTION_ANIM:
    case STATE_GAMEOPTION:
    case STATE_GAMEOPTION_EXIT:
        /* Gamestate_RenderGameOption(); */   /* DESATIVADO (Game Option do Prex3) */
        break;
    case STATE_SERVICE_MENU:
        ServiceMenu_UpdateRender();
        break;
    default:
        break;
    }

    /* Console por último: é overlay, desenha sobre tudo */
    Debug_ConsoleRender();

    if (g_game.showDebug) Render_StateInfo();
    else if (g_game.gfxShowFps) {
        /* SHOW FPS (GRAPHICS SETTINGS): só o contador, no canto inferior direito. */
        static uint32_t t0 = 0; static int n = 0; static float fps = 0;
        char fb[32];
        n++;
        uint32_t now = timeGetTime();
        if (now - t0 >= 1000) { fps = n / ((now - t0) / 1000.0f); n = 0; t0 = now; }
        snprintf(fb, sizeof(fb), "FPS: %.1f", fps);
        Font_Init();   /* Resource_ClearBGA -> Font_Shutdown desliga a fonte */
        Font_DrawString(g_game.screenWidth - (int)strlen(fb) * 8 - 8, 20, fb, 1.0f, 1.0f, 0.0f, 1.0f);
    }
    static int renderLogCount = 0;
    if (g_game.isVSL && renderLogCount < 30) {
        Log_Print("RENDER: state=%d isVSL=%d active=%d globalA=%.2f frame=%d fc=%d\n", g_game.state, g_game.isVSL, g_vsl.active, g_game.globalColorA, g_game.bgaFrame, g_vsl.frameCount);
        renderLogCount++;
    }
    if (renderLogCount < 5) {
        Log_Print("RENDER: state=%d isVSL=%d globalA=%.2f frame=%d\n", g_game.state, g_game.isVSL, g_game.globalColorA, g_game.bgaFrame);
        renderLogCount++;
    }
    Render_EndScene();
}

/* Loop de passo fixo a 60 Hz com acumulador, renderizando com vsync.
 *
 * O loop antigo comparava ticks de 240 Hz e fazia `lastTick = tick` a cada
 * frame, jogando fora o resto acumulado. Como a condição era >= 4 ticks
 * (16,67 ms) mas o relógio só tem resolução de 1 ms, o intervalo real entre
 * frames alternava entre ~16,7 e ~20,8 ms: a rolagem das setas ficava com um
 * tremor periódico mesmo com o jogo "a 60 fps". Além disso o loop girava em
 * busy-wait, sem dormir, o que esquenta um núcleo e piora o jitter do
 * escalonador.
 *
 * Agora o resto fica no acumulador (nada é descartado), o passo de lógica é
 * sempre exatamente 1/60 e o SwapBuffers com vsync é quem alinha o ritmo ao
 * refresh do monitor. Sem vsync disponível, o Sleep(1) segura o loop em ~60 Hz
 * sem queimar CPU. */
void Game_MainLoop(void) {
    bool running = true;
    const double STEP_MS = 1000.0 / 60.0;
    const int    MAX_CATCHUP = 5;   /* teto de passos por iteração */

    double accumulator = 0.0;
    uint32_t prevTime = timeGetTime();

    while (running) {
        if (!Window_ProcessMessages()) {
            running = false;
            break;
        }

        uint32_t now = timeGetTime();
        double elapsed = (double)(uint32_t)(now - prevTime);
        prevTime = now;

        /* Uma pausa longa (janela minimizada, breakpoint, troca de faixa que
         * travou) não pode virar uma avalanche de catch-up. */
        if (elapsed > 250.0) elapsed = 250.0;
        accumulator += elapsed;

        /* Extra do port: detector de travadas. Mede update/render deste giro e,
         * se passar de 50 ms, grava no log onde o tempo foi gasto. */
        extern double g_movieMs, g_logMs;
        double hzF = (double)SDL_GetPerformanceFrequency();
        uint64_t hz0 = SDL_GetPerformanceCounter();
        g_movieMs = 0.0; g_logMs = 0.0;

        int steps = 0;
        while (accumulator >= STEP_MS && steps < MAX_CATCHUP) {
            accumulator -= STEP_MS;
            g_game.frameCounter++;
            Game_Update(1.0f / 60.0f);
            steps++;
        }
        if (steps == MAX_CATCHUP)
            accumulator = 0.0;   /* desistiu de alcançar: não acumula dívida */

        /* era: só desenhava com steps > 0. Igual à NX: com vsync desenha em
         * TODO refresh no gameplay (o swap bloqueia e dá o ritmo) e as setas
         * pegam o relógio da música no momento do desenho
         * (Gameplay_RefreshClock). Nos desenhos extras g_renderTick = false e
         * as cenas do BGA não avançam: animações, judge e spark seguem a 60 Hz.
         * Fora do gameplay continua no ritmo de 60 Hz. */
        bool extra = g_game.vsync && g_game.state == STATE_GAMEPLAY;
        if (steps > 0 || extra) {
            g_renderTick = (steps > 0);
            uint64_t hz1 = SDL_GetPerformanceCounter();
            double movUpd = g_movieMs, logUpd = g_logMs;
            Game_Render();       /* o swap com vsync bloqueia até o refresh */
            g_renderTick = true;
            uint64_t hz2 = SDL_GetPerformanceCounter();
            double updMs = (double)(hz1 - hz0) * 1000.0 / hzF;
            double rndMs = (double)(hz2 - hz1) * 1000.0 / hzF;
            if (updMs + rndMs > 50.0 || elapsed > 50.0) {
                double logTot = g_logMs;
                Log_Print("HITCH: gap=%.0f ms update=%.1f ms (%d passos, video=%.1f, log=%.1f) render=%.1f ms (log=%.1f) estado=%d\n",
                          elapsed, updMs, steps, movUpd, logUpd, rndMs, logTot - logUpd, (int)g_game.state);
            }
        } else {
            Sleep(1);            /* nada a fazer neste giro */
        }
    }
}

/* On Linux this is a plain main(); the original WinMain/SEH path only exists
 * on Windows. */
int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    Game_Init(NULL);
    Game_MainLoop();
    Game_Shutdown();
    return 0;
}
