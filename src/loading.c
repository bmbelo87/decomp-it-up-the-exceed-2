#include "pumpy.h"
#include "movie.h"

#define LOADING_DURATION_MS 3550
#define LOADING_FADE_MS 48

static int g_pnzTexId = -1;
static int g_loadingSongId = -1;
static int g_loadingTimer = 0;
/* PUMPY.EXE: a confirmação executa o comando de console "run" -> 0x410cf0,
 * que carrega tudo e espera [0xd35eb4] >= 0x3C0 (0x4116b5) antes da música.
 * ATENÇÃO: esse contador conta desde o início do TIME do Song Select
 * (0x4096bd/0x40aadc), então no original normalmente NÃO há espera — o título
 * fica congelado só durante a carga (lenta no original: DirectMusic + .DAT).
 * O piso de 4 s abaixo aproxima esse tempo de carga percebido (validado em jogo);
 * não é uma regra literal do original. */
#define LOADING_MIN_TO_MUSIC_MS 4000
static uint32_t g_loadingStartMs = 0;

void Loading_Enter(int songId) {
    g_loadingSongId = songId;
    g_loadingTimer = LOADING_DURATION_MS;
    g_loadingStartMs = timeGetTime();
    g_pnzTexId = -1;

    // Decrementa stage count (exceto no bonus que nao altera)
    if (g_game.stageCount > 0)
        g_game.stageCount--;
    else
        g_game.isBonusSong = true;

    // Destroi BGA do Song Select (099.DAT) antes de entrar no Title
    Resource_ClearBGA();

    char path[MAX_PATH];
    /* Exceed2 (PIU32.EXE 0x4041a7..0x4041ee): "T%sH" com o byte +0x7EF da EEPROM
     * == 0 (coreano), senão "T%sE"; depois "TITLE\%s.PNZ" (0x420f1a).
     * era: "%s/TITLE/T%s.pnz" — sem a letra, nenhum PNZ do Exceed2 abria. */
    /* Demo Play nunca carrega PNZ: 0x40419b testa [0x484F7C] & 0x100000 (demo)
     * e pula a montagem do nome e a carga (salta para 0x4041f6). */
    if (!g_exDemo) {
        snprintf(path, sizeof(path), "%s/TITLE/T%s%c.PNZ", g_game.currentDirectory, Song_DataIdStr(songId),
                 Eeprom2_Language() == 0 ? 'H' : 'E');
        Log_Print("Loading: loading PNZ '%s'\n", path);

        g_pnzTexId = Resource_LoadPNZ(path);
        if (g_pnzTexId < 0) {
            Log_Print("Loading: PNZ not found for song %d\n", songId);
        }
    }

    g_game.state = STATE_SONG_TITLE;
    g_game.stateFrame = 0;
    Render_SetGlobalColor(0, 0, 0, 0);
}

void Loading_Update(float dt) {
    if (g_game.state == STATE_SONG_TITLE) {
        int ms = (int)(dt * 1000.0f);
        if (ms < 1) ms = 1;
        g_loadingTimer -= ms;

        if (g_loadingTimer <= 0) {
            g_loadingTimer = LOADING_FADE_MS;
            g_game.state = STATE_SONG_TITLE_OUT;
            g_game.stateFrame = 0;
        }
        return;
    }

    if (g_game.state == STATE_SONG_TITLE_OUT) {
        int ms = (int)(dt * 1000.0f);
        if (ms < 1) ms = 1;
        g_loadingTimer -= ms;

        if (g_loadingTimer <= 0) {
            /* Resource_ClearBGA chama Texture_Shutdown, que destrói TODAS as texturas
               incluindo o pnz. Resetar g_pnzTexId antes para evitar Texture_Unload
               posterior no slot que já foi reutilizado pelo BGA (bug: destruía HALL.PNG). */
            g_pnzTexId = -1;

            Resource_ClearBGA();

            /* PIU32.EXE 0x404346..0x40479B ([+0x19ACC]):
             *   BGA\%s.DAT existe (0x423AB0) -> carrega o DAT (= 1);
             *   senão BGA\%s.MOV -> 0x420C00(path, 0) sem loop (= 2);
             *   senão BGA\000.MOV -> 0x420C00(path, 1) EM LOOP (= 2) — fundo das
             *   músicas sem BGA próprio (os REMIX);
             *   senão nada (0x423270, = 3).
             * era (exceed.exe 0x402243): .MOV antes do .DAT e sem o 000.MOV.
             * O vídeo é aberto junto com a música, abaixo. */
            char bgaPath[MAX_PATH];
            snprintf(bgaPath, sizeof(bgaPath), "%s/BGA/%s.DAT", g_game.currentDirectory, Song_DataIdStr(g_loadingSongId));
            char movPath[MAX_PATH];
            snprintf(movPath, sizeof(movPath), "%s/BGA/%s.MOV", g_game.currentDirectory, Song_DataIdStr(g_loadingSongId));
            Movie_Close();
            bool useMov = false, movLoop = false;
            FILE* tf = fopen(bgaPath, "rb");
            if (tf) {
                fclose(tf);
                Log_Print("Loading: loading BGA '%s'\n", bgaPath);
                Resource_LoadBGADirect(bgaPath);
            } else if ((tf = fopen(movPath, "rb")) != NULL) {
                fclose(tf);
                useMov = true;
            } else {
                snprintf(movPath, sizeof(movPath), "%s/BGA/000.MOV", g_game.currentDirectory);
                if ((tf = fopen(movPath, "rb")) != NULL) {
                    fclose(tf);
                    useMov = true;
                    movLoop = true;
                }
            }
            g_game.bgaLoop = false;
            BGA_Reset();

            char audioPath[MAX_PATH];
            snprintf(audioPath, sizeof(audioPath), "%s/AUDIO/%s.AUD", g_game.currentDirectory, Song_IdStr(g_loadingSongId));
            Log_Print("Loading: loading AUD '%s'\n", audioPath);
            bool audOk = BGM_LoadAUDDirect(audioPath);

            g_game.songSelectHighlighted = g_game.selectedSongIndex;

            /* Steps carregados antes da espera, como na init do original
             * (0x410cf0 carrega o step antes do loop de 0x4116b5). */
            g_game.state = STATE_GAMEPLAY;
            Gameplay_Start(g_loadingSongId);

            /* Extra do port: o vídeo é aberto, lido inteiro para a memória e tem o
             * 1º quadro decodificado AQUI, com o PNZ na tela e antes da espera
             * (que absorve o tempo). Sem isso a leitura de disco (blocos de 4 KB
             * durante a música) e a init do decoder caíam dentro do gameplay.
             * era: Movie_Open depois da espera. */
            if (useMov) {
                Log_Print("Loading: loading MOV '%s'\n", movPath);
                if (Movie_Open(movPath, movLoop)) {
                    Movie_Preload();
                    Movie_Prime();
                }
            }

            /* Espera ativa como no original (0x4116b5): nada é desenhado. */
            while (timeGetTime() - g_loadingStartMs < LOADING_MIN_TO_MUSIC_MS)
                Sleep(1);
            if (audOk && !(g_exDemo && !Demo_SoundOn()))   /* 0x40236A */
                BGM_Play(false);
            g_game.stateFrame = 0;
            g_game.bgaFrame = 0;
            Render_SetGlobalColor(0, 0, 0, 0);
        }
        return;
    }
}

void Loading_Render(void) {
    if (g_game.state != STATE_SONG_TITLE && g_game.state != STATE_SONG_TITLE_OUT)
        return;

    if (g_pnzTexId >= 0) {
        int tw = Texture_GetWidth(g_pnzTexId);
        int th = Texture_GetHeight(g_pnzTexId);

        float scaleX = 640.0f / (float)tw;
        float scaleY = 480.0f / (float)th;
        float scale = scaleX < scaleY ? scaleX : scaleY;
        float dw = tw * scale;
        float dh = th * scale;
        float dx = (640.0f - dw) / 2.0f;
        float dy = (480.0f - dh) / 2.0f;

        Texture_Draw(g_pnzTexId, dx, dy, scale, scale, 1.0f);
    } else {
        char buf[64];
        snprintf(buf, sizeof(buf), "Loading %d...", g_loadingSongId);
        Font_DrawStringCentered(320, 240, buf, 0.5f, 0.5f, 0.5f, 1.0f);
    }
}

bool Loading_IsActive(void) {
    return g_game.state == STATE_SONG_TITLE || g_game.state == STATE_SONG_TITLE_OUT;
}
