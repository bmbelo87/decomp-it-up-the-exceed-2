#include "pumpy.h"
#include <stdarg.h>
#include <inttypes.h>

GameContext g_game = {0};

static FILE* g_logFile = NULL;

/* A versão anterior formatava direto em g_game.logBuffer no offset logPos e
 * depois fazia OutputDebugStringA(logBuffer + logPos - written). Dois problemas:
 *
 *   1. vsnprintf devolve o tamanho que a string TERIA, não o que coube. Com o
 *      buffer cheio, `written` continuava grande enquanto logPos ficava preso
 *      em sizeof-1 — e `logPos - written` apontava para ANTES do buffer.
 *   2. Chegando em sizeof-1, o terceiro argumento do vsnprintf virava 1: nada
 *      mais era escrito e o OutputDebugString passava a imprimir lixo antigo.
 *
 * Como logBuffer não é lido em lugar nenhum (só servia de scratch), agora a
 * formatação acontece num buffer local e o ponteiro nunca sai do lugar.
 */
void Log_Print(const char* fmt, ...) {
    char line[1024];
    va_list args;
    int written;

    va_start(args, fmt);
    written = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (written < 0) return;                       /* erro de formatação */
    if (written >= (int)sizeof(line))              /* truncado: usa o que coube */
        written = (int)sizeof(line) - 1;
    line[written] = '\0';

    OutputDebugStringA(line);

    if (!g_logFile) {
        g_logFile = fopen("pumpy.log", "w");
    }
    if (g_logFile) {
        fputs(line, g_logFile);
        fflush(g_logFile);   /* mantido: foi o que permitiu localizar o crash */
    }
}

void Log_Flush(void) {
    g_game.logPos = 0;
    g_game.logBuffer[0] = '\0';
}

float Math_Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

float Math_Clamp(float v, float min, float max) {
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

uint32_t Timer_GetTime(void) {
    return timeGetTime();
}

float Timer_GetDelta(void) {
    uint32_t now = Timer_GetTime();
    float dt = (now - g_game.lastTime) * 0.001f;
    g_game.lastTime = now;
    return dt;
}

void Timer_Wait(uint32_t ms) {
    Sleep(ms);
}

const char* State_ToString(GameState state) {
    switch (state) {
        case STATE_BOOT: return "BOOT";
        case STATE_WARNING_INIT: return "WARNING_INIT";
        case STATE_WARNING_ANIM: return "WARNING_ANIM";
        case STATE_WARNING_END: return "WARNING_END";
        case STATE_LOGO_ENTER: return "LOGO_ENTER";
        case STATE_LOGO_UPDATE: return "LOGO_UPDATE";
        case STATE_INTRO: return "INTRO";
        case STATE_CREDIT: return "CREDIT";
        case STATE_EXSELECT: return "EXSELECT";
        case STATE_STATION: return "STATION";

        case STATE_MENU_ENTER: return "MENU_ENTER";
        case STATE_MENU_INPUT: return "MENU_INPUT";
        case STATE_RESET_WARNING: return "RESET_WARNING";
        case STATE_GAME_INIT: return "GAME_INIT";
        case STATE_GAMEPLAY: return "GAMEPLAY";
        case STATE_GAMEOVER:
        case STATE_GAMEOVER_ENTER: return "GAMEOVER_ENTER";
        case STATE_DANCE_GRADE_ENTER: return "DANCE_GRADE_ENTER";
        case STATE_DANCE_GRADE_DISPLAY: return "DANCE_GRADE_DISPLAY";
        case STATE_STAGE_TRANSITION: return "STAGE_TRANSITION";
        case STATE_GAMEOPTION_ENTER: return "GAMEOPTION_ENTER";
        case STATE_GAMEOPTION_ANIM: return "GAMEOPTION_ANIM";
        case STATE_GAMEOPTION: return "GAMEOPTION";
        case STATE_GAMEOPTION_EXIT: return "GAMEOPTION_EXIT";
        case STATE_SONG_SELECT: return "SONG_SELECT";
        case STATE_SONG_SELECT_B: return "SONG_SELECT_B";
        case STATE_SONG_TITLE: return "SONG_TITLE";
        case STATE_SONG_TITLE_OUT: return "SONG_TITLE_OUT";
        case STATE_LOGO_SKIP: return "LOGO_SKIP";
        case STATE_EXIT: return "EXIT";
        default: return "UNKNOWN";
    }
}

int Sprite_FindTile(const char* name)
{
    for (int i = 0; i < g_game.sprTileCount; i++)
    {
        if (_stricmp(g_game.sprTiles[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int Sprite_GetTexture(int tileIdx)
{
    if (tileIdx < 0 || tileIdx >= g_game.sprTileCount) return -1;
    return g_game.sprTiles[tileIdx].texId;
}

void Sprite_DrawTile(int tileIdx, float x, float y, float scaleX, float scaleY, float alpha)
{
    if (tileIdx < 0 || tileIdx >= g_game.sprTileCount) return;
    SPRTileDef* t = &g_game.sprTiles[tileIdx];
    if (t->texId < 0) return;

    int tw = Texture_GetWidth(t->texId);
    int th = Texture_GetHeight(t->texId);
    if (tw <= 0) tw = 256;
    if (th <= 0) th = 256;

    // SPRTileDef stores UV as NORMALIZED [0..1]
    // Texture_DrawUV expects PIXEL coords, so multiply back
    float u1 = (float)t->u1 * (float)tw;
    float v1 = (float)t->v1 * (float)th;
    float u2 = (float)t->u2 * (float)tw;
    float v2 = (float)t->v2 * (float)th;

    if (t->u1 == 0.0f && t->v1 == 0.0f && t->u2 == 1.0f && t->v2 == 1.0f && !t->flipH && !t->flipV)
    {
        float w = (float)t->srcW * scaleX;
        float h = (float)t->srcH * scaleY;
        Texture_Draw(t->texId, x - w/2, y - h/2, w / (float)tw,
                    h / (float)th, alpha);
    }
    else
    {
        float w = (float)t->srcW * scaleX;
        float h = (float)t->srcH * scaleY;
        Texture_DrawUV(t->texId, x - w/2, y - h/2, w, h,
                      u1, v1, u2, v2, 1.0f, 1.0f, 1.0f, alpha);
    }
}

void Sprite_DrawTileUV(int tileIdx, float x, float y, float w, float h, float alpha)
{
    if (tileIdx < 0 || tileIdx >= g_game.sprTileCount) return;
    SPRTileDef* t = &g_game.sprTiles[tileIdx];
    if (t->texId < 0) return;

    int tw = Texture_GetWidth(t->texId);
    int th = Texture_GetHeight(t->texId);
    if (tw <= 0) tw = 256;
    if (th <= 0) th = 256;

    float u1 = (float)t->u1 * (float)tw;
    float v1 = (float)t->v1 * (float)th;
    float u2 = (float)t->u2 * (float)tw;
    float v2 = (float)t->v2 * (float)th;

    Texture_DrawUV(t->texId, x - w/2, y - h/2, w, h,
                  u1, v1, u2, v2, 1.0f, 1.0f, 1.0f, alpha);
}

int Sprite_GetTileW(int tileIdx)
{
    if (tileIdx < 0 || tileIdx >= g_game.sprTileCount) return 0;
    return g_game.sprTiles[tileIdx].srcW;
}

int Sprite_GetTileH(int tileIdx)
{
    if (tileIdx < 0 || tileIdx >= g_game.sprTileCount) return 0;
    return g_game.sprTiles[tileIdx].srcH;
}