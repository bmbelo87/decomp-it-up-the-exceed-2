#ifndef PUMPY_H
#define PUMPY_H

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #include <windows.h>
    #include <mmsystem.h>
#else
    #include "platform_linux.h"
#endif

/* Janela, entrada e áudio usam SDL2 em TODAS as plataformas (o DirectSound do
 * original foi substituído). Estes tipos ficam opacos só para preservar o
 * layout das structs compartilhadas. */
typedef void* LPDIRECTSOUND;
typedef void* LPDIRECTSOUNDBUFFER;
#include <GL/gl.h>
#include <GL/glu.h>

/* O gl.h da Microsoft (opengl32 = GL 1.1) não traz constantes do GL 1.2+. */
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "song.h"
#include "step.h"

#define GAME_VERSION "0.9.5"
#define GAME_BUILD_DATE "Jul 27 2026"
#define GAME_BUILD_TIME "15:44"
#define TARGET_FPS 60
#define FRAME_TIME_MS (1000 / TARGET_FPS)

/* Resolução LÓGICA: a base de todas as coordenadas do jogo e a resolução em
 * que a arte original foi feita (os BGA são PNG de 640x480). Não mexer: é o
 * que o glOrtho usa e o que o código de desenho assume em cada 480.0f - y. */
#define LOGICAL_SCREEN_W 640
#define LOGICAL_SCREEN_H 480

/* Tamanho da JANELA na criação. Independente do acima — o viewport estica o
 * backbuffer lógico até aqui, preservando 4:3 (sem tarja em múltiplos de 4:3
 * como 1024x768). A janela é redimensionável, isto é só o padrão. */
#define WINDOW_DEFAULT_W 1024
#define WINDOW_DEFAULT_H 768

#define MAX_TEXTURES 2048
#define MAX_SOUNDS 64
#define MAX_BGA_PICS 50
#define MAX_BGA_EVENTS 6000
#define MAX_SPR_TILES 4096

#define PAD_BUTTONS_PER_PLAYER 5

typedef enum {
    // Original state values (from reverse engineering)
    STATE_BOOT            = 0x00,
    STATE_WARNING_INIT    = 0x01,
    STATE_WARNING_ANIM    = 0x02,
    STATE_WARNING_END     = 0x03,
    STATE_LOGO_ENTER      = 0x04,
    STATE_LOGO_UPDATE     = 0x05,
    STATE_MENU_ENTER      = 0x0A,
    STATE_MENU_INPUT      = 0x0B,
    STATE_RESET_WARNING   = 0x0C,
    STATE_SONGSEL_ENTER   = 0x0D, /* PUMPY.EXE 0x412750 -> 0x1B: entrada da seleção de música */
    STATE_GAME_INIT       = 0x0E,
    STATE_GAMEPLAY        = 0x0F,
    STATE_GAMEOVER        = 0x10,
    STATE_SERVICE_ENTER   = 0x11, /* PUMPY.EXE 0x404ee0 -> 0x12 (menu de serviço). O resultado é 0x19 no original */
    /* STATE_STAFF_ROLL      = 0x12, */  /* nunca foi usado — valor reaproveitado abaixo */
    STATE_SERVICE_MENU    = 0x12, /* SETUP MENU — g_dwState = 0x12 em ServiceMenu_Enter (0x00404ee0) */
    STATE_AUTO_EXIT       = 0x13,
    STATE_RANKING         = 0x14,
    STATE_RANKING_IN      = 0x15,
    STATE_RANKING_OUT     = 0x16,
    STATE_GAMEOVER_ENTER  = 0x17,
    STATE_DANCE_GRADE_ENTER = 0x18,
    STATE_DANCE_GRADE_DISPLAY = 0x19,
    STATE_STAGE_TRANSITION = 0x1A,
    STATE_SONGSEL_1B      = 0x1B, /* 0x40abe0: carrega BGA\\099.DAT + sprites de dificuldade (não é battle) */
    STATE_SONGSEL_1C      = 0x1C, /* 0x4092e0 */
    STATE_SONGSEL_1D      = 0x1D, /* 0x409720: seleção de música principal */
    STATE_SONGSEL_1E      = 0x1E, /* 0x4091a0 */
    STATE_GAMEOPTION_ENTER = 0x1F,
    STATE_GAMEOPTION_ANIM  = 0x20,
    STATE_GAMEOPTION       = 0x21,
    STATE_GAMEOPTION_EXIT  = 0x22,
    STATE_STAFF_ENTER     = 0x23,
    STATE_STAFF           = 0x24,
    STATE_STAFF_END       = 0x25,
    STATE_STAGE_BREAK     = 0x26, /* 083.dat + 7-1.WAV → GameOver */

    // Our custom additions (high range to avoid conflicts)
    STATE_LOGO_SKIP       = 0x80,
    STATE_HOWTOPLAY       = 0x86, /* Show Help: 03.DAT + 03.AUD antes da SongSelect */
    STATE_SONG_SELECT     = 0x81,
    STATE_SONG_SELECT_B   = 0x82,
    STATE_SONG_TITLE      = 0x83,
    STATE_SONG_TITLE_OUT  = 0x84,
    STATE_GAMEPLAY_BEGIN  = 0x85,
    STATE_INTRO           = 0x90, /* Exceed: CIntro — BGA\INTRO.MOV + AUDIO\001.AUD */
    STATE_CREDIT          = 0x91, /* Exceed: CTitle — CREDIT.MOV */
    STATE_HIGHSCORE       = 0x93, /* Exceed: CHighscore — BGA\HS.DAT + ranking da EEPROM */
    STATE_EXSELECT        = 0x92, /* Exceed: CSelect — BGA\SELECT.DAT + SELECT2.DAT + 90.DAT */
    STATE_IR              = 0x94, /* Exceed: CInternetRanking — BGA\IR.DAT + AUDIO\IR.AUD (src/ir.c) */
    STATE_NAMEINPUT       = 0x95, /* Exceed: CNameInput — BGA\085.DAT (src/nameinput.c) */
    STATE_STATION         = 0x96, /* Exceed2: CStation — BGA\STATION.DAT (src/station.c) */
    STATE_EXIT            = 0xFF,
} GameState;

/* Exceed: registro da tabela de músicas do exceed.exe (0x004551E0, 56 bytes no
 * original). Aqui é só a cópia em C (exceed_songs.c, gerado por
 * tools/gen_exceed_songs.py) — não é lido de arquivo, então os ponteiros não
 * precisam casar com o layout de 32 bits. */
/* Exceed (exceed.exe): 105 músicas, canal[3][50] em 0x004568E0 — DESATIVADO
#define EX_SONG_COUNT     105
#define EX_CHANNEL_COUNT  3
#define EX_CHANNEL_MAX    50
*/
#define EX_SONG_COUNT     138   /* Exceed2: 0x412DD0 retorna 0x8A */
#define EX_CHANNEL_COUNT  5     /* 0 BANYA, 1 K-POP, 2 POP (ARCADE), 3 REMIX, 4 BATTLE */
#define EX_CHANNEL_MAX    53    /* 0x35: linha de 0xD4 bytes em 0x00458AB8 */
typedef struct {
    uint32_t    id;         /* +0x00  hex -> "%X" nos nomes de arquivo */
    const char* artistKr;   /* +0x04 */
    const char* artistEn;   /* +0x08 */
    const char* titleKr;    /* +0x0C */
    const char* titleEn;    /* +0x10 */
    double      bpm;        /* +0x18 */
    int         level[5];   /* +0x20 NORMAL HARD CRAZY FREESTYLE(Double) NIGHTMARE, -1 = não existe */
    uint8_t     visible;    /* +0x34 */
    uint8_t     hidden;     /* +0x35 — init 0x416474: visible = (hidden == 0) */
    uint8_t     lock[5];    /* Exceed2 +0x3C..+0x40 (base 0x4563A4): modo travado por dificuldade */
} ExceedSong;
extern const ExceedSong g_exSongs[EX_SONG_COUNT];
extern const int g_exChannels[EX_CHANNEL_COUNT][EX_CHANNEL_MAX];

typedef enum {
    PAD_UL    = 0,
    PAD_UR    = 1,
    PAD_C     = 2,
    PAD_DL    = 3,
    PAD_DR    = 4,
} PadButton;

typedef struct {
    GLuint id;
    int width;
    int height;
    int format;
    bool inUse;
    uint32_t lastFrame;
    char name[64];
    int hd;          /* 1 = textura substituída por HD\<NOME>_<CRC>.png; width/height continuam lógicos */
} Texture;

typedef struct {
    LPDIRECTSOUNDBUFFER buffer;
    bool inUse;
    char name[64];
} Sound;

typedef struct {
    LPDIRECTSOUNDBUFFER buffer;
    bool playing;
    bool looping;
    bool useMCI;
    char name[MAX_PATH];
    char mciPath[MAX_PATH];
    DWORD dataSize;
    uint32_t durationMs;
} BGM;

#pragma pack(push, 1)
typedef struct {
    int32_t frame;    // 0-3
    int32_t picIndex; // 4-7
    int32_t unk8;     // 8-11
    float x;          // 12-15
    float y;          // 16-19
    float w;          // 20-23
    float h;          // 24-27
    float sx;         // 28-31
    float sy;         // 32-35
    float u1;         // 36-39
    float v1;         // 40-43
    float u2;         // 44-47
    float v2;         // 48-51
    uint32_t pad[3];  // 52-63
} BGA2Event; // 64 bytes
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    int16_t frame;
    int16_t picIndex;
    int16_t flags;
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    int16_t pad0;
    float sx;
    float sy;
    uint8_t pad1[12];
    float alpha;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t pad2;
} BGA1Event; // 0x2C bytes
#pragma pack(pop)

typedef struct {
    float x;
    float y;
    float hotx;
    float hoty;
    float scaleX;
    float scaleY;
    float rotation;
    float r;
    float g;
    float b;
    float a;
    int frame;
    int type;
    int blendMode;
    int z_order;
} BGAKeyframe;

typedef struct {
    char name[16];
    char texture[64];
    int srcX, srcY, srcW, srcH;
    float u1, v1, u2, v2;
    int texId;
    bool flipH, flipV;
} SPRTileDef;

#define MAX_BGA_LAYERS 64
#define MAX_BGA_KEYFRAMES 512

typedef struct {
    char filename[64];
    int isSPR;
    int kfCount;
    BGAKeyframe keyframes[MAX_BGA_KEYFRAMES];
    int sprTileStart;
    int sprTileCount;
    int texId;
    int aniFrameCount;
    int patCols;
    int patRows;
    int patFlags;
} BGALayer;

#define MAX_BGA_SCENES 32

typedef struct {
    char name[64];
    int start, end, loop, mode;   /* +0x44 +0x46 +0x48 +0x4C */
    int cur;                      /* [+0x13a30] */
    int rev;                      /* [+0x13a34] */
} BGAScene;

typedef struct {
    char name[64];
    int version;
    int layerCount;
    BGALayer layers[MAX_BGA_LAYERS];
    int maxFrame;
    /* Exceed: o jogo endereça camadas pelo slot do arquivo (0x41F55C, 0..49),
     * contando slots vazios; o parser compacta, então guarda slot -> camada. */
    int slotLayer[MAX_BGA_LAYERS];   /* -1 = slot vazio */
    int slotCount;
    float scaleX, scaleY;            /* 0x41F754 (+0x11AAAC/+0x11AAB0) */
    /* Exceed2 BGA3: cenas nomeadas após "SCENE1" (PIU32.EXE 0x41e474..0x41e51b).
     * Registro de 0x50 B: +0 id, +4 nome[0x40], +0x44 início, +0x46 fim,
     * +0x48 volta, +0x4C modo. Estado: [+0x13a30] quadro, [+0x13a34] reverso. */
    int sceneCount;
    BGAScene scenes[MAX_BGA_SCENES];
} BGAPicture;

typedef struct {
    bool keys[256];
    bool prevKeys[256];
    bool padConnected[2];
    uint32_t padState[2];
    uint32_t padPrevState[2];
    bool autoplay;
} InputState;

typedef struct {
    uint32_t score[2];
    uint32_t combo[2];
    uint32_t combo_0;  // Combo for player 0 (used in comparison logic)
    uint32_t combo_1;  // Combo for player 1 (used in comparison logic)
    int32_t life[2];
    uint32_t maxCombo[2];
    int perfectCount[2];
    int greatCount[2];
    int goodCount[2];
    int badCount[2];
    int missCount[2];
    int missCombo[2];
    int lifeSpeed[2];  /* multiplicador de ganho de vida (200–1000) */
} GameplayStats;

typedef struct {
    HWND hWnd;
    HDC hDC;
    HGLRC hRC;
    HINSTANCE hInstance;
    
    LPDIRECTSOUND pDS;
    LPDIRECTSOUNDBUFFER pPrimaryBuffer;
    Sound sounds[MAX_SOUNDS];
    int soundCount;
    BGM bgm;
    
    Texture textures[MAX_TEXTURES];
    int textureCount;
    
    BGAPicture bgaPics[MAX_BGA_PICS];
    int bgaPicCount;
    int currentBGA;
    int bgaFrame;
    int bgaMaxFrame;
    float bgaTimer;
    bool bgaLoop;
    int bgaLoopStart;
    int bgaLoopEnd;
    int bgaStateIndices[16];
    
    SPRTileDef sprTiles[MAX_SPR_TILES];
    int sprTileCount;
    
    GameState state;
    GameState nextState;
    GameState fadeTarget;
    uint32_t frameCounter;
    uint32_t stateFrame;
    uint32_t flags;
    
    int screenWidth;
    int screenHeight;
    bool isFullscreen;
    bool vsync;
    /* GRAPHICS SETTINGS (extra do port, Service Menu; salvo no PUMPY.INI) */
    int  gfxResIdx;      /* 0..4 = 640x480, 800x600, 1024x768, 1280x960, 1600x1200 */
    int  gfxTexFilter;   /* 0 = SMOOTH (GL_LINEAR), 1 = SHARP (GL_NEAREST) */
    bool gfxShowFps;
    int  gfxAspect;      /* 0 = 4:3 com bordas, 1 = esticar */
    int  gfxUpscale;     /* 0 = OFF, 2/3/4 = xBRZ ao carregar texturas (xbrz.c) */
    
    InputState input;
    GameplayStats stats;
    
    SongDB songDB;
    StepSong currentSong;
    int selectedSongIndex;
    int selectedModeIndex;
    int selectedDifficulty;
    int songSelectScroll;
    int songSelectHighlighted;
    int previewSongId;
    
    float globalScaleX;
    float globalScaleY;
    float globalAlpha;
    float globalColorR;
    float globalColorG;
    float globalColorB;
    float globalColorA;
    float fadeAlpha;
    
    bool confirmActive;
    uint32_t confirmTimer;

    int optionDifficulty; // 0=Easy, 1=Normal, 2=Hard
    int optionToggle1;    // opcao toggle 1 (Stage Break)
    int optionToggle2;    // opcao toggle 2 (Show Help)
    int optionCurrentItem; // 0-4
    int stageBreak;       // Stage Break global (0=OFF, 1=ON)
    int showHelp;         // Show Help global (0=OFF, 1=ON)
    int audioOffsetMs;    /* Offset de áudio em ms (default=80). Positivo = adianta o receptor/timing.
                             Equivalente a ajuste de latência: bgmPos/1000 - audioOffsetMs/1000.
                             Salvo em PUMPY.INI como AudioOffset=X. */

    /* ── Menu de serviço (SETUP MENU) ────────────────────────────────────────
     * Equivalentes dos globais do original usados por ServiceMenu_RenderGameOption
     * (0x00405910) e ServiceMenu_RenderCoinOption (0x00405d80).
     * Os campos optionDifficulty/stageBreak/showHelp acima já cobrem
     * g_nGameLevel, g_nStageBreak e g_nShowHelp.
     */
    int svcGameMode;      /* g_nGameMode    — 0=NORMAL, 1=EVENT            */
    int svcDemoSound;     /* g_nDemoSound   — 0=ON, 1=OFF (verde quando 0) */
    int svcLangOption;    /* g_nLangOption  — 0=KR,1=EN,2=PT,3=ES          */
    int svcCoin1;         /* g_nCoin1Setting — 0=FREE PLAY, 1..10          */
    int svcCoin2;         /* g_nCoin2Setting — 0=FREE PLAY, 1..9           */
    int svcServiceTotal;  /* contadores do BOOKKEEPING MENU                */
    int svcCoin1Total;    /* ram0x00d39048 no original                     */
    int svcCoin2Total;    /* g_nCoin2Total                                 */
    int svcCoinTotal;     /* g_nCoinTotal — acumulador em MOEDAS, não em
                           * créditos. Créditos = svcCoinTotal / svcCoin1. */

    /* ── Commands (códigos inseridos na SongSelect) ──────────────────────────
     * Sequência confirmada no Ghidra (DAT_00442378, byte pattern 08 10 08 10 04):
     *   UL UR UL UR CN  — 5 botões, buffer circular de 5 posições.
     *   (no original: UL=0x08, UR=0x10, CN=0x04 como nota mask)
     * Cada vez que a sequência é completada, avança para o próximo estado:
     *   x1 → x2 → x3 → x4 → RV → x1
     *
     * RV = Random Velocity (ícone raccel): é o mesmo bit 0x200 que o command de
     * 9 botões UL UR UL UR UL UR UL UR CN alterna (PUMPY.EXE 0x4073dc / 0x40747e).
     *
     * Máscara por jogador no original (DAT_00da22b4 P1 / DAT_00da22b0 P2):
     *   0x002/0x004/0x008 x2/x3/x4   0x010 Vanish     0x020 Mirror
     *   0x040 Random Step            0x080 Non-Step   0x200 Random Velocity
     *   0x400 Freedom                0x800 Earthworm
     *   Mirror   – inverte painéis: UL↔DR, UR↔DL, CN=CN
     *   Vanish   – setas somem ao chegar perto do receptor
     *   Non-Step – todas as setas ficam invisíveis
     *   Freedom  – receptor invisível
     *   Earthworm– velocidade salta entre x2/x3 (ou x1/x2 acima de 180 BPM) a cada compasso
     */
    int  cmdSpeedMult[2];        /* multiplicador de velocidade por jogador: 1..4. Default=1 */
    bool cmdMirror[2];           /* Mirror ativo por jogador */
    bool cmdRandomStep[2];       /* Random Step ativo por jogador */
    bool cmdRandomVelocity[2];   /* Random Velocity ativo por jogador */
    bool cmdEarthworm[2];        /* Earthworm ativo por jogador */
    bool cmdFreedom[2];          /* Freedom ativo por jogador (oculta receptor) */
    bool cmdVanish[2];           /* Vanish ativo por jogador */
    bool cmdNonStep[2];          /* Non-Step ativo por jogador */
    int  activePlayerMask; /* 0x1=P1 ativo, 0x2=P2 ativo (ambos=0x3). Default=0x1 */
    bool isBattleMode;    /* true quando BATTLE selecionado (P1+P2, HARD steps em half1 duplicado em half2) */
    bool isVSL;           // true when current song uses 3D VSL instead of BGA
    
    bool showDebug;
    int stageCount;      // 3 = Stage 1, 2 = Stage 2, 1 = Stage 3, 0 = bonus/gameover
    bool bonusStage;     // true se S/A em todos os stages anteriores
    bool isBonusSong;    // true quando estiver jogando o bonus stage
    uint32_t timerId;
    uint32_t lastTime;
    float deltaTime;
    
    char currentDirectory[MAX_PATH];
    char logBuffer[4096];
    int logPos;
} GameContext;

extern GameContext g_game;
extern StepChart* g_chart;

// Font/arrow globals (loaded from 00.DAT via Resource_LoadFontAndArrows)
extern int g_fontTexId;      // font.tga
extern int g_fontDec00Id;    // dec00.tga
extern int g_fontSpr01;      // 01.spr start index
extern int g_fontSpr02;      // 02.spr
extern int g_fontSpr03;      // 03.spr
extern int g_fontSpr04;      // 04.spr
extern int g_fontSpr05;      // 05.spr
extern int g_fontSprW01;     // w01.spr
extern int g_fontSprW02;     // w02.spr
extern int g_fontSprW03;     // w03.spr
extern int g_fontSprW04;     // w04.spr
extern int g_fontSprW05;     // w05.spr
extern int g_fontSprM01;     // m01.spr
extern int g_fontSprM02;     // m02.spr
extern int g_fontSprM03;     // m03.spr
extern int g_fontSprM04;     // m04.spr
extern int g_fontSprM05;     // m05.spr
extern int g_fontSprHD01;    // HD01.SPR
extern int g_fontSprHD02;    // HD02.SPR
extern int g_fontSprHD03;    // HD03.SPR
extern int g_fontSprHD05;    // HD05.SPR
extern int g_fontSprBT01;    // BT_MC01.SPR
extern int g_fontSprBT02;    // BT_MC02.SPR
extern int g_fontSprGGS;     // gg_s.spr (Exceed, lifebar single)
extern int g_fontSprGGD;     // gg_d.spr (Exceed, lifebar double)
extern int g_fontArrow541;
extern int g_fontArrow542;
extern int g_fontArrow543;
extern int g_fontArrow544;
extern int g_fontArrow545;
extern int g_fontArrowETC;
extern int g_fontArrowF;
extern int g_fontSpark;

void Game_Init(HINSTANCE hInstance);
void Game_Shutdown(void);
void Game_MainLoop(void);
void Game_ChangeState(GameState newState);
void Game_Update(float dt);
void Game_Render(void);

void Log_Print(const char* fmt, ...);
void Log_Flush(void);

float Math_Lerp(float a, float b, float t);
float Math_Clamp(float v, float min, float max);

uint32_t Timer_GetTime(void);
float Timer_GetDelta(void);
void Timer_Wait(uint32_t ms);
const char* State_ToString(GameState state);

bool Window_Create(HINSTANCE hInstance, int width, int height, bool fullscreen);
void Window_Destroy(void);
void Window_SwapBuffers(void);
bool Window_ProcessMessages(void);
void Window_ToggleFullscreen(void);
void Window_ApplyGraphics(void);
void Window_GetResolution(int idx, int* w, int* h);
void Window_RequestQuit(void);   /* SDL port: asks the message pump to exit   */

bool Font_Init(void);
void Font_DrawChar(int x, int y, unsigned char c, float r, float g, float b, float a);
void Font_DrawString(int x, int y, const char* str, float r, float g, float b, float a);
void Font_DrawStringCentered(int x, int y, const char* str, float r, float g, float b, float a);
void Font_DrawStringScaled(int x, int y, const char* str, float r, float g, float b, float a, float scale);
void Font_DrawStringCenteredScaled(int x, int y, const char* str, float r, float g, float b, float a, float scale);
int Font_LoadTexture(void);
void Font_LoadFontOnly(void);
const char* GetVersionString(void);
void Font_DrawText(float x, float y, const char* text);
void Font_DrawDigit(int texId, int digit, int x, int y, float scale);
void Font_DrawNumber(int texId, int x, int y, int number, int digits, float scale);
void Font_DrawDecDigit(int texId, float x, float y, int digit, float alpha, float scaleX, float scaleY, float r, float g, float b);
void Font_DrawNumber(int texId, int x, int y, int number, int digits, float scale);
void Font_Shutdown(void);

void Texture_Init(void);
void Texture_ApplyFilterAll(void);
int Texture_Load(const char* name);
int Texture_LoadFromMemory(const uint8_t* buf, uint32_t bufSize, const char* debugName);
int Texture_LoadFromMemoryColorKey(const uint8_t* buf, uint32_t bufSize, const char* debugName);
void Texture_Unload(int id);
void Texture_Bind(int id);
int Texture_GetWidth(int id);
int Texture_GetHeight(int id);
void Texture_Draw(int id, float x, float y, float scaleX, float scaleY, float alpha);
void Texture_DrawUV(int id, float x, float y, float w, float h, float u1, float v1, float u2, float v2, float r, float g, float b, float alpha);
void Texture_InsetUV(int texW, int texH, float* u1, float* v1, float* u2, float* v2); /* SMOOTH: meio texel para dentro */
void Texture_Shutdown(void);

typedef enum {
    SND_3_2 = 0,
    SND_2_1,
    SND_4_2,
    SND_8_1,
    SND_5_1,
    SND_RANK_A,
    SND_RANK_B,
    SND_RANK_C,
    SND_RANK_D,
    SND_RANK_F,
    SND_10_2,   /* 10-2.WAV: som do auto-scroll DL/DR hold */
    SND_7_1,    /* 7-1.WAV:  som do Stage Break (antes do Game Over) */
    SND_COIN_PARTIAL, /* 01-1.WAV:  moeda inserida sem fechar crédito */
    SND_COIN_CREDIT,  /* COIN2.WAV: crédito completo */
    SND_10_1,   /* 10-1.WAV: bip do TIME no SongSelect, a cada segundo a partir de 10 */
    SND_CHGMOD,     /* Exceed CHGMOD.WAV    [0xA6D300]: troca de canal / cancela o painel */
    SND_START,      /* Exceed START.WAV     [0xA6D2EC]: confirmação da música */
    SND_TIME_LIMIT, /* Exceed TIME_LIMIT.WAV [0xA6D30C]: contador da Select <= 5 */
    SND_PUSHPANEL,  /* Exceed PUSHPANEL.WAV [0xA6D2E8]: jogador entra na Select */
    SND_GAMESTOP,   /* Exceed GAMESTOP.WAV  [0xA6D2F0]: Stage Break (0x4152ED) */
    SND_9_5,        /* Exceed 9-5.WAV       [0xA6D2F8]: CGrade t=158 (0x40D4DF) */
    SND_NEXTSTAGE,  /* Exceed NEXTSTAGE.WAV [0xA6D2FC]: NEXTSTAGE (0x41517F) */
    SND_COUNT
} SoundID;

bool Audio_Init(void);
int Audio_LoadWAV(const char* name, const uint8_t* data, DWORD size);
int Audio_LoadFromResource(const char* name, int resId);
int Audio_LoadWaveFile(const char* filename);
void Audio_LoadAllWaves(void);
extern int g_waveSoundIds[SND_COUNT];
void Audio_Play(int id, bool loop);
void Audio_Stop(int id);
void Audio_StopAll(void);
bool Audio_IsPlaying(int id);
void Audio_SetVolume(int id, long volume);
void Audio_Shutdown(void);

bool BGM_Load(const char* path);
bool BGM_LoadWAV(const char* path);
bool BGM_LoadMP3(const char* path);
bool BGM_LoadAUD(int songId, bool preview);
bool BGM_LoadAUDDirect(const char* path);
void BGM_Play(bool loop);
void BGM_Update(void);   /* por frame: refaz o loop no caminho DirectShow */
void BGM_Stop(void);
bool BGM_IsPlaying(void);
uint32_t BGM_GetPositionMs(void);
uint32_t BGM_GetDurationMs(void);
bool BGM_HasEnded(void);
bool BGM_IsDSActive(void);
void BGM_SetVolume(long volume);
void BGM_Shutdown(void);

bool Input_LoadPumpPad(void);
void Input_LoadKeyConfig(void); /* lê piukey.cfg (teclas do pad, formato do original); chamado sozinho no 1º uso */
void Input_Update(void);
bool Input_IsPadHit(int player, PadButton button);
bool Input_IsPadDown(int player, PadButton button);
bool Input_IsKeyHit(int key);
bool Input_IsKeyDown(int key);
void Input_Shutdown(void);
bool Input_ProcessEvent(void* evt); /* SDL port: feeds SDL events into keyboard state */

bool BGA_Load(const char* path);
bool BGA_LoadFromMemory(const uint8_t* data, uint32_t size, bool isBGA2);
void BGA_Render(int bgaIndex, int frame);
void BGA_SetEventFrame(int bgaIndex, int frame);
void BGA_SetEventLayer(int bgaIndex, int frame, int layerIdx);
void BGA_Reset(void);
void BGA_Shutdown(void);

extern int g_menuSelection;
void Menu_ResetState(void);

void Staff_Enter(void);
void Staff_Update(float dt);

/* ranking.c — tabela de recordes (Var_ e Ranking_ do PUMPY.EXE) */
void Var_RegisterName(int index, int score, const char* name);  /* 0x00402ca0 */
void Var_SetSystemVariable(int score, const char* name);        /* 0x00402be0 */
void Ranking_RegisterDefaults(void);                            /* 0x00404fe0 */
int  Ranking_GetCount(void);
int  Ranking_GetScore(int i);
const char* Ranking_GetName(int i);

/* debug_console.c — console de debug (Debug_Console* do PUMPY.EXE) */
void Demo_ToggleMode(void);                        /* 0x00407cb0 */
void Debug_PrintString(const char* fmt, ...);      /* 0x00402fa0 */
void Debug_ConsoleExecute(const char* cmd);        /* 0x00402e80 */
void Debug_ConsoleKeyHandler(int ch, int vk);      /* 0x00403190 */
void Debug_ConsoleRender(void);                    /* 0x00403050 */
int  Debug_GetConsoleMode(void);                   /* 0x00403180 */
void Debug_ConsoleToggle(void);
bool Debug_ConsoleIsActive(void);

/* coin.c — crédito e moeda (Arcade_/Coin_ do PUMPY.EXE) */
void Arcade_ProcessCoin(int type);   /* 0x00402340 — 1=COIN1, 2=COIN2, 3=SERVICE */
int  Coin_GetCredits(void);          /* 0x00402430 */
int  Coin_GetMaxCredits(void);       /* 0x00402460 — devolve moedas, não créditos */
void Coin_ConsumeCredit(void);       /* 0x00402480 */
bool Coin_IsFreePlay(void);
bool Coin_HasCredit(void);

/* service_menu.c — SETUP MENU (ServiceMenu_* do PUMPY.EXE) */
void ServiceMenu_Enter(void);        /* 0x00404ee0 */
void ServiceMenu_Exit(void);         /* 0x004066d0 */
void ServiceMenu_Update(void);       /* captura o input — chamar em Game_Update */
void ServiceMenu_UpdateRender(void); /* 0x004066e0 — desenha e aplica o input */
void Result_Enter(void);
void Result_Update(float dt);
void Result_Render(void);
GameState Result_GetNextState(void);

void Loading_Enter(int songId);
void Loading_Update(float dt);
void Loading_Render(void);
bool Loading_IsActive(void);

void Gamestate_UpdateWarning(float dt);
void Gamestate_UpdateLogo(float dt);
void Gamestate_UpdateIntro(float dt);
void Gamestate_RenderIntro(void);
unsigned Title_GetJoinedMask(void); /* intro.c — [0x568FF4] bits 0/1 */
void Title_SetJoinedMask(unsigned m);
void Station_Update(float dt);      /* station.c — CStation do Exceed2 */
void Station_Render(void);
/* Exceed: songDB sintético montado das tabelas do exceed.exe quando não há
 * Stage.cfg. Com ele, os IDs são hex e os arquivos saem em "%X"
 * (STEP\%X.STX, AUDIO\%X.AUD, BGA\%X.DAT, TITLE\T%X.PNZ). */
extern bool g_exceedSongIds;
const char* Song_IdStr(int id);
int Song_DataId(int id);            /* A26 -> 401, A27 -> 402 (TITLE/BGA/STEP); o .AUD fica o da própria música */
const char* Song_DataIdStr(int id);
bool ExSelect_IsXMode(void);
unsigned ExSelect_GetFlags(void);
void Attract_Idle(void);            /* Exceed CIdle: próxima tela da atração */
extern bool g_exDemo;               /* [0x568FF4] & 0x100000: demo da atração ("-demo") */
bool Demo_SoundOn(void);            /* EEPROM +0x7F0 */
void Demo_End(void);
void Gameplay_ExScoreSync(void);    /* Exceed: GOOD +100, BAD -700, MISS -1000, score >= 0 */        /* [0x568FF4] & 0x8000 */     /* "%X" com g_exceedSongIds, senão "%d" */
void ExSelect_BuildSongDB(SongDB* db);
void ExSelect_Enter(void);          /* exceed_select.c — CSelect */
bool ExSelect_Run(int id, int m, int demo); /* RUN/PLAY do console: m = 0..4 (N H C D NM) ou 5 (-bt) */
/* Roda de banners da Select, ajustável pelo /set do console (0x456E9C..0x456EC4, 0x563AC8/CC) */
extern float g_exCamX1, g_exCamX2, g_exCamY1, g_exCamY2, g_exCamZ1, g_exCamR, g_exCamR2,
             g_exCamR3, g_exCamTx, g_exCamTy, g_exCamP, g_exCamCy, g_exCamCy2;
extern bool g_drawJudgeRange;       /* CPlayEngine +0x48D4A: só o /drawjudgerange mexe; o exe nunca lê */
void Highscore_Update(float dt);
void Highscore_Render(void);
/* Exceed: Internet Ranking (src/ir.c) */
void IR_RecordStage(int stage, int songIndex, const uint32_t score[2]);
void IR_Update(float dt);
void IR_Render(void);
/* Exceed: entrada de nome do ranking (src/nameinput.c) */
void NameInput_Update(float dt);
void NameInput_Render(void);
void ExSelect_Update(float dt);
void ExSelect_SetStation(int station, int startChannel);
/* eeprom_x2.c — save do Exceed2 (PIUEXCEED2.INI, layout do PIU32.EXE) */
void     Eeprom2_Load(void);
void     Eeprom2_Save(void);
uint8_t* Eeprom2_Data(void);
uint8_t  Eeprom2_Language(void);
bool     Eeprom2_SongOff(int i);
void     Eeprom2_CountPlay(int i);
bool     Eeprom2_CanonUnlocked(void);
void     Eeprom2_SetCanonUnlocked(void);   /* Exceed2: [0x484FB4] e argumento do "SELECT" */
void ExSelect_Render(void);
void Gamestate_UpdateMenu(float dt);
extern bool g_cdLoaded;
void Gamestate_UpdateSongSelect(float dt);
void Gamestate_RenderMenu(int bgaIndex, int frame);
void Gamestate_RenderSongSelect(void);
void SongSelect_Reset(void);
void SongSelect_ResetIntro(void);
void SongSelect_ResetCreditIndices(void);

void Gameplay_Start(int songId);
void Gameplay_Enter(void);
void Gameplay_Exit(void);
void Gameplay_Update(float dt);
void Gameplay_Render(void);

// Original combo rendering functions from PUMPY.EXE
void Combo_DrawMain(int comboValue);    /* 0x00411b40 — desenho principal do combo */
void Combo_DrawSprite(int spriteType);  /* 0x00411a90 — sprites COMBO / MAX COMBO */
void Combo_DrawDigit(int digit);        /* 0x004119d0 — um dígito do combo       */

void Gamestate_UpdateGameOption(float dt);
void Gamestate_RenderGameOption(void);
void Gamestate_InitGameOption(void);
void GameOption_Load(void);

/* eeprom.c — imagem de 2048 bytes no formato do PUMPY.EXE (pumpprex3.ini) */
int  Eeprom_Load(void);   /* 1=ok, 0=inválido (resetado), -1=ausente */
void Eeprom_Save(void);   /* lê PUMPY.INI; chama-se na inicialização do jogo */
void GameOption_Save(void);   /* escreve PUMPY.INI; chama-se em cada alteração */

void Render_Clear(uint8_t r, uint8_t g, uint8_t b);
void Render_SetOrtho(int width, int height);
void Render_Rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void Render_RectOutline(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void Render_Line(float x1, float y1, float x2, float y2, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void Render_BeginScene(void);
void Render_SetGlobalColor(float r, float g, float b, float a);

int Sprite_FindTile(const char* name);
void Sprite_DrawTile(int tileIdx, float x, float y, float scaleX, float scaleY, float alpha);
void Sprite_DrawTileUV(int tileIdx, float x, float y, float w, float h, float alpha);
int Sprite_GetTileW(int tileIdx);
int Sprite_GetTileH(int tileIdx);
void Render_EndScene(void);
int Math_ROUND(float x);

bool RES_Open(const char* path);
int RES_GetCount(void);
int RES_Find(const char* name);
const char* RES_GetName(int index);
uint32_t RES_GetSize(int index);
bool RES_Read(int index, uint8_t* buffer);
uint8_t* RES_ReadAlloc(int index);
void RES_Close(void);
bool RES_IsOpen(void);
void RESPACK_DeriveKey16(const uint8_t* in, uint8_t* out); /* exceed.exe 0x411DBC */

void* Resource_Load(int resId, const char* type, DWORD* outSize);
int Resource_LoadWave(int resId, const char* name);
int Resource_LoadTexture(int resId, const char* name);
bool Resource_LoadBGA(int resId);
bool Resource_LoadBGAByName(const char* datName);
bool Resource_LoadBGADirect(const char* datPath);
bool Resource_LoadStage(const char* path);
int Resource_LoadAllSPRs(const char* datPath);
int Resource_LoadSPR(const char* datPath, const char* sprName);
void Resource_LoadFontAndArrows(const char* datPath);
void Resource_ClearBGA(void);
int Resource_LoadStateBGA(const char* stateName);
int Resource_GetStateBGAIndex(const char* stateName);
int Resource_SwitchBGA(const char* datName);
int Resource_LoadTextureFromDAT(const char* datPath, const char* resName);
uint8_t* Resource_DecryptENC1(const uint8_t* data, uint32_t dataSize, uint32_t* outSize);
uint8_t* Resource_DecryptENC2(const uint8_t* buf, uint32_t fileSize, uint32_t* outSize); /* PIU32.EXE 0x421390 */
uint8_t* Resource_ExtractFromPack(const char* datPath, const char* name, uint32_t* outSize);
int Resource_LoadPNZ(const char* path);

int loadTextureFromRES(const char* resName);
int loadSPRFromRES(const char* name, int* outCols, int* outRows, int* outFlags);
int SPR_LoadSPR(const char* sprName, int* outPatCols, int* outPatRows, int* outPatFlags);
int SPR_LoadSP2(const char* sprName, int* outPatCols, int* outPatRows, int* outPatFlags);

#endif