#ifndef BGA_H
#define BGA_H

#include "pumpy.h"

bool isMenuOverlayLayer(BGALayer* layer);
bool isMenuArrowLayer(BGALayer* layer);
bool isMenuTextLayer(BGALayer* layer);
bool isMenuCenterLayer(BGALayer* layer);
bool layerMatchesDirection(BGALayer* layer, int sel);
int findBGALoopStart(void);
int findBGALoopEnd(void);

/* Exceed: camada por slot do arquivo (0x41F55C) e escala global (0x41F754) */
int BGA_DrawSlot(int bgaIndex, int frame, int slot);
void BGA_SetScale(int bgaIndex, float sx, float sy);
void BGA_SetColor(int bgaIndex, float rgb, float a);   /* 0x41F754 */

/* Exceed2 BGA3: cenas nomeadas (PIU32.EXE 0x41f0f0..0x41f390) */
void BGA_ScenePlay(int bgaIndex, const char* name, bool draw);   /* 0x41f0f0 */
bool BGA_SceneDone(int bgaIndex, const char* name);              /* 0x41f2e0 */
void BGA_SceneReset(int bgaIndex, const char* name);             /* 0x41f390 */
int  BGA_SceneFrame(int bgaIndex, const char* name);             /* [+0x13a30], -1 sem cena */
void BGA_DrawFrame(int bgaIndex, int frame);                     /* 0x41ece0 */
void BGA_ScenePlayAt(int bgaIndex, const char* name, int offset); /* 0x41f200 */
void BGA_SetColor4(int bgaIndex, float r, float g, float b, float a); /* 0x41edb0 */

/* 0x41ee30/0x41ee40: o objeto de textura/SPR (L[0]) do slot, sem os keyframes */
typedef struct {
    char filename[64];
    int isSPR, sprTileStart, sprTileCount, texId, aniFrameCount, patCols, patRows, patFlags;
} BGALayerSrc;
bool BGA_GetLayerSrc(int bgaIndex, int slot, BGALayerSrc* out);
void BGA_SetLayerSrc(int bgaIndex, int slot, const BGALayerSrc* src);
/* posição (pivô) interpolada do slot no quadro; false se invisível */
bool BGA_GetSlotPos(int bgaIndex, int frame, int slot, float* x, float* y);

#endif
