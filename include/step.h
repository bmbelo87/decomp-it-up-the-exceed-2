#ifndef STEP_H
#define STEP_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#if !defined(_WIN32)
#include <strings.h>
#ifndef _stricmp
#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#endif
#endif

#define STX_MAGIC "STF4"
#define STX_HEADER_SIZE 288
#define STX_TITLE_OFFSET 0x3C
#define STX_OFFSET_TABLE 0xFC
#define STX_SECTION_COUNT 9
#define STX_SECTION_HEADER 0xD0
#define STX_ROW_SIZE 13

#define STX_DECOMP_HEADER 128
#define STX_GRID_OFFSET 132

#define STEP_PANELS_SINGLE 5
#define STEP_PANELS_DOUBLE 10
#define STEP_PANELS_HALF 6

typedef struct {
    uint8_t dl;
    uint8_t ul;
    uint8_t cn;
    uint8_t ur;
    uint8_t dr;
} StepHalf;

// Note type values:
#define NT_TAP     1  // normal tap (0x01)
#define NT_HOLD_H  10 // hold head (0x0A)
#define NT_HOLD_B  11 // hold body (0x0B)
#define NT_HOLD_T  12 // hold tail (0x0C)
// Notas especiais do Division (PUMPY.EXE 0x412b91, 0x40f16a):
#define NT_DIV_G   2  // G: pisar soma no contador de G ([jog+0x2C]) e escolhe ramo
#define NT_DIV_W   3  // W: idem, contador de W ([jog+0x28])
#define NT_DIV_A   4  // A: marcador antes do bloco de decisão (não desenha)
/* CStep (step.hpp do fonte): STXDATA m_STXData[NUM_BLOCK_Y = 50][10] —
 * até 50 páginas/segmentos, 10 ramos cada. */
#define STEP_MAX_BLOCK_Y   50
#define STEP_MAX_BLOCK_X   10
#define STEP_DIV_MAX_PAGES STEP_MAX_BLOCK_Y
/* #define STEP_DIV_MAX_PAGES 8 */

typedef struct {
    StepHalf half1;
    StepHalf half2;
} StepRow;

typedef struct {
    float bpm;
    uint32_t beatPerMeasure;
    uint32_t beatSplit;
    int32_t delay;
    uint32_t rowCount;
    StepRow* rows;
    int panelCount;
    int totalNotes;
    // Split section (BPM changes)
    bool hasSplit;
    // Per-segment timing (segments = main + each split block)
    uint32_t segmentCount; // main + number of splits
    struct {
        float bpm;
        uint32_t beatPerMeasure;
        uint32_t beatSplit;
        int32_t delay;
        uint32_t rowStart; // first row of this segment
        uint32_t rowCount; // rows in this segment
        int32_t speed;     // velocidade do bloco x1000 (bloco+96; 0 = sem multiplicador)
    } segments[STEP_MAX_BLOCK_Y]; // era [8] (7 splits); o original aceita 50 blocos Y

    /* Division (seção 7): o header da seção tem 50 contagens de blocos; cada
     * contagem não-nula é uma PÁGINA e os blocos dela são RAMOS (PUMPY.EXE:
     * chart = página*10 + ramo, 0x4127a0/0x41413e). O chart tocável usa o ramo 0
     * de cada página; ao pisar W/G (0x40f16a) o ramo da página seguinte é
     * escolhido pelas condições e as linhas dela são trocadas (todos os ramos de
     * uma página têm o mesmo número de linhas nos 9 charts de Division). */
    int divPageCount;          // 0 = chart sem páginas (não é Division)
    struct {
        int branchCount;
        uint32_t rowStart;     // linha inicial da página no chart tocável
        uint32_t rowCount;
        StepRow* branchRows[STEP_MAX_BLOCK_X];
        int32_t cond[10][20];  // 10 pares [mín,máx] por ramo (bloco+16)
        int32_t speed[10];     // velocidade de cada ramo x1000 (bloco+96)
    } divPages[STEP_DIV_MAX_PAGES];
} StepChart;

typedef struct {
    char title[64];
    int chartCount;
    StepChart charts[9];
} StepSong;

bool Step_LoadSong(const char* path, StepSong* song);
void Step_FreeSong(StepSong* song);
int Step_SelectChart(const char* modeName, int fallbackSection);

/* Random Step: embaralha painéis do chart no load time.
 *
 * panelMode:
 *   0 = Single (5 painéis): P1 → half1, P2 → half2, permutações independentes
 *   1 = Double/Nightmare (10 painéis): 1 player, embaralha half1+half2 juntos
 *   2 = Half Double (6 painéis): 1 player, embaralha as 6 posições HD específicas
 *
 * Gera permutação via 4 swaps aleatórios (fiel ao original) e aplica
 * uniformemente a todos os rows — hold continuity garantida. */
void Step_ApplyRandomShuffle(StepChart* chart, int panelMode, bool shuffleP1, bool shuffleP2);

/* Mirror: espelha paineis com permutacao fixa (load time, hold-safe).
 * Single:  Z<->E, Q<->C, S fica       perm5=[3,4,2,0,1]
 * HD:      S<->5, E<->1, C<->7        perm6=[5,3,4,1,2,0]
 * DN:      Z<->9, Q<->3, S<->5, E<->1, C<->7  perm10=[8,9,7,5,6,3,4,2,0,1] */
void Step_ApplyMirror(StepChart* chart, int panelMode, bool mirrorP1, bool mirrorP2);

#endif
