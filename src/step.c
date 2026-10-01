#include "step.h"
#include "zlibinflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CStep::LoadStep_STX: uncompress() num buffer de 2 MB (BlockSizeUnCompressed = 2097152). */
#define STX_DECOMP_MAX 2097152

void Log_Print(const char* fmt, ...);
uint8_t* Resource_ExtractFromPack(const char* datPath, const char* name, uint32_t* outSize);

/* Exceed2: sem a pasta STEP, os .STX vêm de STEP.DAT (PIU32.EXE 0x40d0d7).
 * "<raiz>/STEP/X.STX" -> entrada "X.STX" de "<raiz>/STEP.DAT", extraída para
 * um arquivo temporário (o parser abaixo lê por FILE*). */
static FILE* stepOpen(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (f) return f;

    char dat[512];
    strncpy(dat, path, sizeof(dat) - 1);
    dat[sizeof(dat) - 1] = '\0';
    char* name = NULL;
    for (char* p = dat; *p; p++) if (*p == '/' || *p == '\\') name = p;
    if (!name) return NULL;
    *name++ = '\0';
    char stxName[64];
    strncpy(stxName, name, sizeof(stxName) - 1);
    stxName[sizeof(stxName) - 1] = '\0';
    char* dir = NULL;
    for (char* p = dat; *p; p++) if (*p == '/' || *p == '\\') dir = p;
    if (dir) strcpy(dir, "/STEP.DAT");
    else strcpy(dat, "STEP.DAT");

    uint32_t size = 0;
    uint8_t* buf = Resource_ExtractFromPack(dat, stxName, &size);
    if (!buf) return NULL;

    char tmp[512];
    const char* t = getenv("TEMP");
    snprintf(tmp, sizeof(tmp), "%s/pumpy_step.stx", t ? t : ".");
    f = fopen(tmp, "wb");
    if (f) { fwrite(buf, 1, size, f); fclose(f); }
    free(buf);
    Log_Print("STX: '%s' lido de '%s' (%u bytes)\n", stxName, dat, size);
    return fopen(tmp, "rb");
}

/* Linhas cruas de um bloco -> StepRow (mesma regra de espelho do bloco principal). */
static StepRow* stepParseRows(const uint8_t* dec, uint32_t n, bool mirror)
{
    StepRow* r = (StepRow*)malloc((n ? n : 1) * sizeof(StepRow));
    if (!r) return NULL;
    for (uint32_t ri = 0; ri < n; ri++) {
        const uint8_t* src = dec + STX_GRID_OFFSET + ri * STX_ROW_SIZE;
        r[ri].half1.dl = src[0]; r[ri].half1.ul = src[1]; r[ri].half1.cn = src[2];
        r[ri].half1.ur = src[3]; r[ri].half1.dr = src[4];
        if (mirror) r[ri].half2 = r[ri].half1;
        else { r[ri].half2.dl = src[5]; r[ri].half2.ul = src[6]; r[ri].half2.cn = src[7];
               r[ri].half2.ur = src[8]; r[ri].half2.dr = src[9]; }
    }
    /* O original lê só k < NumLines - 1: a última linha fica zerada (memset). */
    if (n) memset(&r[n - 1], 0, sizeof(StepRow));
    return r;
}

/* Preenche NT_HOLD_B entre cabeça e cauda (single, half1) — mesma regra do
 * preenchimento do chart, aplicada aos ramos guardados do Division. */
static void stepFillHolds(StepRow* rows, uint32_t n)
{
    for (int panel = 0; panel < 5; panel++) {
        for (uint32_t ri = 0; ri < n; ri++) {
            uint8_t* v = &((uint8_t*)&rows[ri].half1)[panel];
            if (*v != NT_HOLD_H) continue;
            uint32_t t = ri + 1;
            while (t < n) {
                uint8_t tv = ((uint8_t*)&rows[t].half1)[panel];
                if (tv == NT_HOLD_T) break;
                if (tv == NT_HOLD_H) { t = n; break; }
                t++;
            }
            if (t >= n) continue;
            for (uint32_t k = ri + 1; k < t; k++) {
                uint8_t* bv = &((uint8_t*)&rows[k].half1)[panel];
                if (*bv == 0) *bv = NT_HOLD_B;
            }
            ri = t;
        }
    }
}

bool Step_LoadSong(const char* path, StepSong* song)
{
    memset(song, 0, sizeof(StepSong));

    FILE* f = stepOpen(path);
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    if (fileSize < STX_HEADER_SIZE)
    {
        fclose(f);
        return false;
    }

    uint8_t* header = (uint8_t*)malloc(STX_HEADER_SIZE);
    if (!header) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    fread(header, 1, STX_HEADER_SIZE, f);

    if (memcmp(header, STX_MAGIC, 4) != 0)
    {
        free(header); fclose(f);
        return false;
    }

    const char* titlePtr = (const char*)header + STX_TITLE_OFFSET;
    while (*titlePtr == 0 && titlePtr < (const char*)header + STX_OFFSET_TABLE)
        titlePtr++;
    if (*titlePtr)
    {
        strncpy(song->title, titlePtr, sizeof(song->title) - 1);
        song->title[sizeof(song->title) - 1] = '\0';
    }

    uint32_t sectionOffsets[STX_SECTION_COUNT];
    memcpy(sectionOffsets, header + STX_OFFSET_TABLE, STX_SECTION_COUNT * 4);
    free(header);

    for (int si = 0; si < STX_SECTION_COUNT; si++)
    {
        uint32_t secOff = sectionOffsets[si];
        if (secOff == 0 || secOff >= (uint32_t)fileSize) continue;

        fseek(f, secOff, SEEK_SET);

        uint8_t secHeader[STX_SECTION_HEADER];
        if (fread(secHeader, 1, STX_SECTION_HEADER, f) != STX_SECTION_HEADER)
            continue;

        uint32_t compSize;
        memcpy(&compSize, secHeader + STX_SECTION_HEADER - 4, 4);
        if (compSize == 0 || compSize > (uint32_t)(fileSize - secOff - STX_SECTION_HEADER))
            continue;

        /* Layout real da seção, conforme Step_ParseFile (0x004068b0, PUMPY.EXE; confirmado no CStep::LoadStep_STX do fonte do Exceed):
         *   [0]   int     — nível de dificuldade
         *   [4]   50 ints — quantos blocos cada grupo tem
         *   [204] os blocos, cada um [4 bytes tamanho][dados zlib]
         * O original soma essas contagens e lê exatamente esse número de blocos
         * em sequência; o primeiro é o que já lemos acima (o tamanho dele é o
         * último int do header). */
        uint32_t blockCounts[50];
        memcpy(blockCounts, secHeader + 4, sizeof(blockCounts));
        int totalBlocks = 0;
        for (int bi = 0; bi < 50; bi++) {
            if (blockCounts[bi] > STEP_MAX_BLOCK_X) { totalBlocks = 0; break; }  /* header suspeito */
            totalBlocks += (int)blockCounts[bi];
        }
        if (totalBlocks < 1) totalBlocks = 1;

        /* Division: cada contagem não-nula do header é uma página e os blocos
         * dela são ramos. Só vira "páginas" se alguma página tiver > 1 bloco;
         * senão os blocos continuam sendo mudanças de BPM em sequência. */
        int blkPage[STEP_MAX_BLOCK_Y * STEP_MAX_BLOCK_X], blkBranch[STEP_MAX_BLOCK_Y * STEP_MAX_BLOCK_X], divPages = 0;
        bool isDiv = false;
        {
            int bi = 0;
            for (int g = 0; g < 50 && bi < STEP_MAX_BLOCK_Y * STEP_MAX_BLOCK_X; g++) {
                if (!blockCounts[g]) continue;
                if (blockCounts[g] > 1) isDiv = true;
                for (uint32_t k = 0; k < blockCounts[g] && bi < STEP_MAX_BLOCK_Y * STEP_MAX_BLOCK_X; k++) {
                    blkPage[bi] = divPages; blkBranch[bi] = (int)k; bi++;
                }
                divPages++;
            }
            if (divPages > STEP_DIV_MAX_PAGES) isDiv = false;
        }

        /* Só era usado pela antiga varredura de gap, que saiu:
         * uint32_t secEnd = secOff + STX_SECTION_HEADER + compSize; */

        // Read main section compressed data
        fseek(f, secOff + STX_SECTION_HEADER, SEEK_SET);
        uint8_t* compData = (uint8_t*)malloc(compSize);
        if (!compData || fread(compData, 1, compSize, f) != compSize)
        {
            free(compData);
            continue;
        }

        uint8_t* decompBuf = (uint8_t*)malloc(STX_DECOMP_MAX);
        if (!decompBuf) { free(compData); continue; }

        uint32_t decompLen = STX_DECOMP_MAX;
        uint32_t inConsumed = 0;
        int ret = zlib_decompress_ex(compData, compSize, decompBuf, &decompLen, &inConsumed);
        free(compData);

        if (ret != 0 || decompLen < STX_GRID_OFFSET + STX_ROW_SIZE)
        {
            free(decompBuf);
            continue;
        }

        float bpm;
        uint32_t beatPerMeasure, beatSplit;
        int32_t delay;
        memcpy(&bpm, decompBuf, 4);
        memcpy(&beatPerMeasure, decompBuf + 4, 4);
        memcpy(&beatSplit, decompBuf + 8, 4);
        memcpy(&delay, decompBuf + 12, 4);

        uint32_t rowCount;
        memcpy(&rowCount, decompBuf + STX_DECOMP_HEADER, 4);

        uint32_t dataBytes = (uint32_t)(decompLen - STX_GRID_OFFSET);
        uint32_t expectedRows = dataBytes / STX_ROW_SIZE;
        if (rowCount > expectedRows)
            rowCount = expectedRows;
        if (rowCount == 0)
        {
            free(decompBuf);
            continue;
        }

        int chartIdx = song->chartCount;
        StepChart* chart = &song->charts[chartIdx];
        chart->bpm = bpm;
        chart->beatPerMeasure = beatPerMeasure;
        chart->beatSplit = beatSplit;
        chart->delay = delay;
        chart->rowCount = rowCount;
        chart->hasSplit = false;
        chart->segmentCount = 1;
        chart->segments[0].bpm = bpm;
        chart->segments[0].beatPerMeasure = beatPerMeasure;
        chart->segments[0].beatSplit = beatSplit;
        chart->segments[0].delay = delay;
        chart->segments[0].rowStart = 0;
        chart->segments[0].rowCount = rowCount;
        /* Velocidade do bloco x1000 (bloco+96 = chart+0x60 a partir do BPM; o
         * PUMPY.EXE lê em 0x4118d0 e multiplica pela velocidade do jogador). */
        memcpy(&chart->segments[0].speed, decompBuf + 96, 4);

        bool mirror = true;
        chart->panelCount = STEP_PANELS_SINGLE;
        if (si == 3 || si == 5 || si == 6)
        {
            chart->panelCount = STEP_PANELS_DOUBLE;
            mirror = false;
        }

        chart->rows = (StepRow*)malloc(rowCount * sizeof(StepRow));
        if (!chart->rows) { free(decompBuf); continue; }

        for (uint32_t ri = 0; ri < rowCount; ri++)
        {
            const uint8_t* src = decompBuf + STX_GRID_OFFSET + ri * STX_ROW_SIZE;
            chart->rows[ri].half1.dl = src[0];
            chart->rows[ri].half1.ul = src[1];
            chart->rows[ri].half1.cn = src[2];
            chart->rows[ri].half1.ur = src[3];
            chart->rows[ri].half1.dr = src[4];

            if (mirror)
            {
                chart->rows[ri].half2.dl = src[0];
                chart->rows[ri].half2.ul = src[1];
                chart->rows[ri].half2.cn = src[2];
                chart->rows[ri].half2.ur = src[3];
                chart->rows[ri].half2.dr = src[4];
            }
            else
            {
                chart->rows[ri].half2.dl = src[5];
                chart->rows[ri].half2.ul = src[6];
                chart->rows[ri].half2.cn = src[7];
                chart->rows[ri].half2.ur = src[8];
                chart->rows[ri].half2.dr = src[9];
            }
        }

        /* O original lê só k < NumLines - 1: a última linha do bloco fica zerada. */
        memset(&chart->rows[rowCount - 1], 0, sizeof(StepRow));

        if (isDiv) {
            chart->divPageCount = divPages;
            chart->divPages[0].rowStart = 0;
            chart->divPages[0].rowCount = rowCount;
            chart->divPages[0].branchRows[0] = stepParseRows(decompBuf, rowCount, mirror);
            memcpy(chart->divPages[0].cond[0], decompBuf + 16, sizeof(chart->divPages[0].cond[0]));
            memcpy(&chart->divPages[0].speed[0], decompBuf + 96, 4);
            chart->divPages[0].branchCount = 1;
            Log_Print("STX: secao %d DIVISION: %d paginas\n", si, divPages);
        }

        free(decompBuf);

        /* Blocos seguintes (block splits com mudança de BPM).
         *
         * Antes isto varria o espaço até a próxima seção procurando o magic
         * zlib 78 9C. Era heurística: o formato real, visto em Step_ParseFile
         * (0x004068b0), tem a contagem de blocos no header e os blocos vêm em
         * sequência logo após o primeiro, cada um prefixado por 4 bytes com o
         * próprio tamanho comprimido. Nada de procurar magic. */
        {
            uint32_t blockPos = secOff + STX_SECTION_HEADER + compSize;

            for (int blk = 1; blk < totalBlocks; blk++)
            {
                if (blockPos + 4 > (uint32_t)fileSize) break;

                uint32_t bSize = 0;
                fseek(f, (long)blockPos, SEEK_SET);
                if (fread(&bSize, 4, 1, f) != 1) break;
                blockPos += 4;
                if (bSize == 0 || blockPos + bSize > (uint32_t)fileSize) break;

                uint8_t* bComp = (uint8_t*)malloc(bSize);
                if (!bComp) break;
                if (fread(bComp, 1, bSize, f) != bSize) { free(bComp); break; }
                blockPos += bSize;

                uint8_t* bDec = (uint8_t*)malloc(STX_DECOMP_MAX);
                if (!bDec) { free(bComp); break; }
                uint32_t bdl = STX_DECOMP_MAX, bic = 0;
                int bret = zlib_decompress_ex(bComp, bSize, bDec, &bdl, &bic);
                free(bComp);
                if (bret != 0 || bdl < STX_GRID_OFFSET + STX_ROW_SIZE) { free(bDec); break; }

                float sBpm;
                uint32_t sBpmM, sBpmS;
                int32_t sDelay;
                memcpy(&sBpm, bDec, 4);
                memcpy(&sBpmM, bDec + 4, 4);
                memcpy(&sBpmS, bDec + 8, 4);
                memcpy(&sDelay, bDec + 12, 4);

                uint32_t sRowCount;
                memcpy(&sRowCount, bDec + STX_DECOMP_HEADER, 4);
                uint32_t expRows2 = (bdl - STX_GRID_OFFSET) / STX_ROW_SIZE;
                if (sRowCount > expRows2) sRowCount = expRows2;
                if (sRowCount == 0) { free(bDec); continue; }

                /* Sanidade mínima. O delay pode ser negativo — na 826 os blocos
                 * seguintes têm delay entre -3 e -5, e a regra antiga (>= 0) os
                 * reprovava, encerrando a música no fim do primeiro bloco. */
                if (!(sBpm > 0.0f && sBpm < 2000.0f) || sBpmS == 0 || sBpmS > 256 || sBpmM == 0) {
                    Log_Print("STX: bloco %d da secao %d invalido (BPM=%.1f m=%u s=%u)\n",
                              blk, si, sBpm, sBpmM, sBpmS);
                    free(bDec);
                    continue;
                }

                if (isDiv && blk < STEP_MAX_BLOCK_Y * STEP_MAX_BLOCK_X) {
                    int pg = blkPage[blk], br = blkBranch[blk];
                    if (br > 0) {
                        /* Ramo alternativo: guardado, não entra no chart tocável. */
                        if (br < STEP_MAX_BLOCK_X && pg < STEP_DIV_MAX_PAGES) {
                            chart->divPages[pg].branchRows[br] = stepParseRows(bDec, sRowCount, mirror);
                            memcpy(chart->divPages[pg].cond[br], bDec + 16, sizeof(chart->divPages[pg].cond[br]));
                            memcpy(&chart->divPages[pg].speed[br], bDec + 96, 4);
                            if (chart->divPages[pg].branchCount < br + 1) chart->divPages[pg].branchCount = br + 1;
                            if (chart->divPages[pg].branchRows[br]) stepFillHolds(chart->divPages[pg].branchRows[br], sRowCount);
                            Log_Print("STX: DIVISION pagina %d ramo %d rows=%u cond G[%d,%d] W[%d,%d]\n", pg, br, sRowCount,
                                      chart->divPages[pg].cond[br][10], chart->divPages[pg].cond[br][11],
                                      chart->divPages[pg].cond[br][12], chart->divPages[pg].cond[br][13]);
                        }
                        free(bDec);
                        continue;
                    }
                    if (pg < STEP_DIV_MAX_PAGES) {
                        chart->divPages[pg].rowStart = rowCount;
                        chart->divPages[pg].rowCount = sRowCount;
                        chart->divPages[pg].branchRows[0] = stepParseRows(bDec, sRowCount, mirror);
                        memcpy(chart->divPages[pg].cond[0], bDec + 16, sizeof(chart->divPages[pg].cond[0]));
                        memcpy(&chart->divPages[pg].speed[0], bDec + 96, 4);
                        if (chart->divPages[pg].branchCount < 1) chart->divPages[pg].branchCount = 1;
                        if (chart->divPages[pg].branchRows[0]) stepFillHolds(chart->divPages[pg].branchRows[0], sRowCount);
                    }
                }

                chart->hasSplit = true;

                int segIdx = chart->segmentCount;
                if (segIdx < STEP_MAX_BLOCK_Y) {
                    chart->segments[segIdx].bpm = sBpm;
                    chart->segments[segIdx].beatPerMeasure = sBpmM;
                    chart->segments[segIdx].beatSplit = sBpmS;
                    chart->segments[segIdx].delay = sDelay;
                    chart->segments[segIdx].rowStart = rowCount;
                    chart->segments[segIdx].rowCount = sRowCount;
                    memcpy(&chart->segments[segIdx].speed, bDec + 96, 4);
                    chart->segmentCount++;
                }

                Log_Print("STX: secao %d bloco %d: BPM=%.1f m=%u s=%u delay=%d rows=%u (rowStart=%u)\n",
                          si, blk, sBpm, sBpmM, sBpmS, sDelay, sRowCount, rowCount);

                uint32_t totalRows = rowCount + sRowCount;
                StepRow* merged = (StepRow*)realloc(chart->rows, totalRows * sizeof(StepRow));
                if (!merged) { free(bDec); break; }
                chart->rows = merged;

                for (uint32_t ri = 0; ri < sRowCount; ri++)
                {
                    const uint8_t* src = bDec + STX_GRID_OFFSET + ri * STX_ROW_SIZE;
                    StepRow* dst = &chart->rows[rowCount + ri];
                    dst->half1.dl = src[0]; dst->half1.ul = src[1];
                    dst->half1.cn = src[2]; dst->half1.ur = src[3];
                    dst->half1.dr = src[4];
                    if (mirror) {
                        dst->half2.dl = src[0]; dst->half2.ul = src[1];
                        dst->half2.cn = src[2]; dst->half2.ur = src[3];
                        dst->half2.dr = src[4];
                    } else {
                        dst->half2.dl = src[5]; dst->half2.ul = src[6];
                        dst->half2.cn = src[7]; dst->half2.ur = src[8];
                        dst->half2.dr = src[9];
                    }
                }
                /* Última linha do bloco zerada, como no original (k < NumLines - 1). */
                memset(&chart->rows[rowCount + sRowCount - 1], 0, sizeof(StepRow));
                rowCount += sRowCount;
                chart->rowCount = rowCount;
                free(bDec);
            }
        }

        song->chartCount++;
    }

    for (int c = 0; c < song->chartCount; c++)
        if (song->charts[c].divPageCount > 0 && song->charts[c].divPages[0].branchRows[0])
            stepFillHolds(song->charts[c].divPages[0].branchRows[0], song->charts[c].divPages[0].rowCount);

    // Preenche NT_HOLD_B entre HEAD e TAIL
    for (int c = 0; c < song->chartCount; c++)
    {
        StepChart* ch = &song->charts[c];
        for (int panel = 0; panel < 5; panel++)
        {
            for (uint32_t ri = 0; ri < ch->rowCount; ri++)
            {
                uint8_t* v = NULL;
                switch (panel) {
                    case 0: v = &ch->rows[ri].half1.dl; break;
                    case 1: v = &ch->rows[ri].half1.ul; break;
                    case 2: v = &ch->rows[ri].half1.cn; break;
                    case 3: v = &ch->rows[ri].half1.ur; break;
                    case 4: v = &ch->rows[ri].half1.dr; break;
                }
                if (!v || *v != NT_HOLD_H) continue;

                uint32_t tailRi = ri + 1;
                while (tailRi < ch->rowCount) {
                    uint8_t* tv = NULL;
                    switch (panel) {
                        case 0: tv = &ch->rows[tailRi].half1.dl; break;
                        case 1: tv = &ch->rows[tailRi].half1.ul; break;
                        case 2: tv = &ch->rows[tailRi].half1.cn; break;
                        case 3: tv = &ch->rows[tailRi].half1.ur; break;
                        case 4: tv = &ch->rows[tailRi].half1.dr; break;
                    }
                    if (tv && *tv == NT_HOLD_T) break;
                    /* Outro HEAD antes do TAIL significa dado malformado: sem
                     * este corte o primeiro head adotava o tail do segundo e o
                     * hold virava um trecho inteiro que não existe no chart —
                     * era o "hold que surge do nada" segurando a seta. */
                    if (tv && *tv == NT_HOLD_H) { tailRi = ch->rowCount; break; }
                    tailRi++;
                }
                if (tailRi >= ch->rowCount) continue;

                // Preenche body entre head+1 e tail-1
                for (uint32_t bri = ri + 1; bri < tailRi; bri++)
                {
                    uint8_t* bv = NULL;
                    switch (panel) {
                        case 0: bv = &ch->rows[bri].half1.dl; break;
                        case 1: bv = &ch->rows[bri].half1.ul; break;
                        case 2: bv = &ch->rows[bri].half1.cn; break;
                        case 3: bv = &ch->rows[bri].half1.ur; break;
                        case 4: bv = &ch->rows[bri].half1.dr; break;
                    }
                    if (bv && *bv == 0) *bv = NT_HOLD_B;
                }

                ri = tailRi;
            }
            // P2 (half2)
            for (uint32_t ri = 0; ri < ch->rowCount; ri++)
            {
                uint8_t* v = NULL;
                switch (panel) {
                    case 0: v = &ch->rows[ri].half2.dl; break;
                    case 1: v = &ch->rows[ri].half2.ul; break;
                    case 2: v = &ch->rows[ri].half2.cn; break;
                    case 3: v = &ch->rows[ri].half2.ur; break;
                    case 4: v = &ch->rows[ri].half2.dr; break;
                }
                if (!v || *v != NT_HOLD_H) continue;

                uint32_t tailRi = ri + 1;
                while (tailRi < ch->rowCount) {
                    uint8_t* tv = NULL;
                    switch (panel) {
                        case 0: tv = &ch->rows[tailRi].half2.dl; break;
                        case 1: tv = &ch->rows[tailRi].half2.ul; break;
                        case 2: tv = &ch->rows[tailRi].half2.cn; break;
                        case 3: tv = &ch->rows[tailRi].half2.ur; break;
                        case 4: tv = &ch->rows[tailRi].half2.dr; break;
                    }
                    if (tv && *tv == NT_HOLD_T) break;
                    /* Outro HEAD antes do TAIL significa dado malformado: sem
                     * este corte o primeiro head adotava o tail do segundo e o
                     * hold virava um trecho inteiro que não existe no chart —
                     * era o "hold que surge do nada" segurando a seta. */
                    if (tv && *tv == NT_HOLD_H) { tailRi = ch->rowCount; break; }
                    tailRi++;
                }
                if (tailRi >= ch->rowCount) continue;

                for (uint32_t bri = ri + 1; bri < tailRi; bri++)
                {
                    uint8_t* bv = NULL;
                    switch (panel) {
                        case 0: bv = &ch->rows[bri].half2.dl; break;
                        case 1: bv = &ch->rows[bri].half2.ul; break;
                        case 2: bv = &ch->rows[bri].half2.cn; break;
                        case 3: bv = &ch->rows[bri].half2.ur; break;
                        case 4: bv = &ch->rows[bri].half2.dr; break;
                    }
                    if (bv && *bv == 0) *bv = NT_HOLD_B;
                }

                ri = tailRi;
            }
        }
    }

    fclose(f);
    return song->chartCount > 0;
}

void Step_FreeSong(StepSong* song)
{
    for (int i = 0; i < song->chartCount; i++)
    {
        free(song->charts[i].rows);
        song->charts[i].rows = NULL;
        for (int pg = 0; pg < STEP_DIV_MAX_PAGES; pg++)
            for (int br = 0; br < 10; br++) {
                free(song->charts[i].divPages[pg].branchRows[br]);
                song->charts[i].divPages[pg].branchRows[br] = NULL;
            }
        song->charts[i].divPageCount = 0;
    }
    song->chartCount = 0;
}

int Step_SelectChart(const char* modeName, int fallbackSection)
{
    if (modeName == NULL) return fallbackSection;

    if (_stricmp(modeName, "PRACTICE") == 0) return 0;
    if (_stricmp(modeName, "NORMAL") == 0) return 1;
    if (_stricmp(modeName, "HARD") == 0) return 2;
    if (_stricmp(modeName, "NIGHTMARE") == 0) return 3;
    if (_stricmp(modeName, "CRAZY") == 0) return 4;
    if (_stricmp(modeName, "DOUBLE") == 0) return 5;
    if (_stricmp(modeName, "FULLDOUBLE") == 0) return 5;
    if (_stricmp(modeName, "HALFDOUBLE") == 0) return 6;
    if (_stricmp(modeName, "DIVISION") == 0) return 7;
    if (_stricmp(modeName, "LIGHTMAP") == 0) return 8;

    return fallbackSection;
}

/* ── Helpers de permutacao (usados pelo Mirror) ─────────────────────────────
 * Aplicam uma permutacao fixa a um StepHalf, StepRow DN ou StepRow HD.
 * nota em panel[i] vai para panel[perm[i]]. */

static void applyPermToHalf(StepHalf* h, const int perm[5])
{
    uint8_t v[5] = { h->dl, h->ul, h->cn, h->ur, h->dr };
    uint8_t o[5] = { 0 };
    for (int i = 0; i < 5; i++) o[perm[i]] = v[i];
    h->dl = o[0]; h->ul = o[1]; h->cn = o[2]; h->ur = o[3]; h->dr = o[4];
}

static void applyPermToDN(StepRow* row, const int perm[10])
{
    uint8_t v[10] = {
        row->half1.dl, row->half1.ul, row->half1.cn, row->half1.ur, row->half1.dr,
        row->half2.dl, row->half2.ul, row->half2.cn, row->half2.ur, row->half2.dr
    };
    uint8_t o[10] = { 0 };
    for (int i = 0; i < 10; i++) o[perm[i]] = v[i];
    row->half1.dl=o[0]; row->half1.ul=o[1]; row->half1.cn=o[2]; row->half1.ur=o[3]; row->half1.dr=o[4];
    row->half2.dl=o[5]; row->half2.ul=o[6]; row->half2.cn=o[7]; row->half2.ur=o[8]; row->half2.dr=o[9];
}

static void applyPermToHD(StepRow* row, const int perm[6])
{
    /* Layout HD: [0]=h1.cn [1]=h1.ur [2]=h1.dr [3]=h2.dl [4]=h2.ul [5]=h2.cn */
    uint8_t v[6] = {
        row->half1.cn, row->half1.ur, row->half1.dr,
        row->half2.dl, row->half2.ul, row->half2.cn
    };
    uint8_t o[6] = { 0 };
    for (int i = 0; i < 6; i++) o[perm[i]] = v[i];
    row->half1.cn=o[0]; row->half1.ur=o[1]; row->half1.dr=o[2];
    row->half2.dl=o[3]; row->half2.ul=o[4]; row->half2.cn=o[5];
}

/* ── Mirror — permutacao fixa, aplicada a todos os rows (hold-safe) ─────────
 *
 * Permutacoes (nota em panel[i] vai para panel[perm[i]]):
 *   Single5: Z<->E, Q<->C, S fica     → [3,4,2,0,1]
 *   HD6:     S<->5, E<->1, C<->7      → [5,3,4,1,2,0]
 *   DN10:    Z<->9, Q<->3, S<->5,
 *            E<->1, C<->7             → [8,9,7,5,6,3,4,2,0,1]
 *
 * Como a permutacao e a mesma em todos os rows, holds ficam consistentes. */
void Step_ApplyMirror(StepChart* chart, int panelMode, bool mirrorP1, bool mirrorP2)
{
    if (!chart || !chart->rows || chart->rowCount == 0) return;

    if (panelMode == 1) {
        /* DN: Z<->9, Q<->3, S<->5, E<->1, C<->7
         * pan [0 1 2 3 4 5 6 7 8 9] -> [8 9 7 5 6 3 4 2 0 1] */
        static const int dnPerm[10] = { 8, 9, 7, 5, 6, 3, 4, 2, 0, 1 };
        for (uint32_t ri = 0; ri < chart->rowCount; ri++)
            applyPermToDN(&chart->rows[ri], dnPerm);
        Log_Print("MIRROR DN rows=%u\n", chart->rowCount);
    }
    else if (panelMode == 2) {
        /* HD: S<->5, E<->1, C<->7
         * pos [0 1 2 3 4 5] -> [5 3 4 1 2 0] */
        static const int hdPerm[6] = { 5, 3, 4, 1, 2, 0 };
        for (uint32_t ri = 0; ri < chart->rowCount; ri++)
            applyPermToHD(&chart->rows[ri], hdPerm);
        Log_Print("MIRROR HD rows=%u\n", chart->rowCount);
    }
    else {
        /* Single: Z<->E(DL<->UR), Q<->C(UL<->DR), S fica
         * [0 1 2 3 4] -> [3 4 2 0 1] */
        static const int singlePerm[5] = { 3, 4, 2, 0, 1 };
        for (uint32_t ri = 0; ri < chart->rowCount; ri++) {
            if (mirrorP1) applyPermToHalf(&chart->rows[ri].half1, singlePerm);
            if (mirrorP2) applyPermToHalf(&chart->rows[ri].half2, singlePerm);
        }
        Log_Print("MIRROR Single P1=%d P2=%d rows=%u\n", mirrorP1, mirrorP2, chart->rowCount);
    }
}

/* ── Random Step shuffle — por row ──────────────────────────────────────────
 * Cada row recebe permutacao independente → resultado verdadeiramente caótico.
 * Hold continuity: bodies/tails seguem o head (rastreado via holdDest[]).
 *
 * Algoritmo por row:
 *  1. Panels com HOLD_B/T → colocados no destino do head correspondente
 *  2. Panels livres (TAP/HOLD_H) → shuffled aleatoriamente nos slots livres
 *  3. HOLD_H atualiza holdDest para as rows seguintes
 *
 * panelMode: 0=Single(5p) 1=Double/NM(10p) 2=HalfDouble(6p)
 */

/* Shuffle in-place de um array de inteiros (Fisher-Yates) */
static void shuffleIntArr(int* arr, int n)
{
    for (int i = n - 1; i > 0; i--) {
        int j = rand() % (i + 1);
        int t = arr[i]; arr[i] = arr[j]; arr[j] = t;
    }
}

/* Single: embaralha half1 (playerHalf=0) ou half2 (playerHalf=1) por row */
static void rsShuffleSingle(StepChart* chart, int playerHalf)
{
    int holdDest[5];
    for (int i = 0; i < 5; i++) holdDest[i] = -1;

    for (uint32_t ri = 0; ri < chart->rowCount; ri++) {
        StepHalf* h = (playerHalf == 0) ? &chart->rows[ri].half1 : &chart->rows[ri].half2;
        uint8_t v[5] = { h->dl, h->ul, h->cn, h->ur, h->dr };
        uint8_t o[5] = { 0 };
        bool occ[5] = { false, false, false, false, false };

        /* Passo 1: bodies/tails vao para holdDest */
        for (int i = 0; i < 5; i++) {
            if (v[i] == NT_HOLD_B || v[i] == NT_HOLD_T) {
                int d = holdDest[i];
                if (d >= 0 && d < 5) { o[d] = v[i]; occ[d] = true; }
                if (v[i] == NT_HOLD_T) holdDest[i] = -1;
            }
        }

        /* Passo 2: coleta paineis livres (src) e slots livres (dst) */
        int src[5]; int sc = 0;
        int dst[5]; int dc = 0;
        for (int i = 0; i < 5; i++) {
            if (v[i] != 0 && v[i] != NT_HOLD_B && v[i] != NT_HOLD_T) src[sc++] = i;
            if (!occ[i]) dst[dc++] = i;
        }

        /* Passo 3: shuffle dos paineis fonte → slots livres */
        shuffleIntArr(dst, dc);  /* embaralha DESTINOS — garante slot aleatorio mesmo com 1 nota */
        for (int i = 0; i < sc; i++) {
            int s = src[i], d = dst[i];
            o[d] = v[s];
            if (v[s] == NT_HOLD_H) holdDest[s] = d;
        }

        h->dl = o[0]; h->ul = o[1]; h->cn = o[2]; h->ur = o[3]; h->dr = o[4];
    }
}

/* Double/Nightmare: 10 paineis combinados, por row */
static void rsShuffleDN(StepChart* chart)
{
    int holdDest[10];
    for (int i = 0; i < 10; i++) holdDest[i] = -1;

    for (uint32_t ri = 0; ri < chart->rowCount; ri++) {
        StepRow* row = &chart->rows[ri];
        uint8_t v[10] = {
            row->half1.dl, row->half1.ul, row->half1.cn, row->half1.ur, row->half1.dr,
            row->half2.dl, row->half2.ul, row->half2.cn, row->half2.ur, row->half2.dr
        };
        uint8_t o[10] = { 0 };
        bool occ[10] = { false, false, false, false, false, false, false, false, false, false };

        for (int i = 0; i < 10; i++) {
            if (v[i] == NT_HOLD_B || v[i] == NT_HOLD_T) {
                int d = holdDest[i];
                if (d >= 0 && d < 10) { o[d] = v[i]; occ[d] = true; }
                if (v[i] == NT_HOLD_T) holdDest[i] = -1;
            }
        }

        int src[10]; int sc = 0;
        int dst[10]; int dc = 0;
        for (int i = 0; i < 10; i++) {
            if (v[i] != 0 && v[i] != NT_HOLD_B && v[i] != NT_HOLD_T) src[sc++] = i;
            if (!occ[i]) dst[dc++] = i;
        }

        shuffleIntArr(src, sc);
        for (int i = 0; i < sc; i++) {
            int s = src[i], d = dst[i];
            o[d] = v[s];
            if (v[s] == NT_HOLD_H) holdDest[s] = d;
        }

        row->half1.dl=o[0]; row->half1.ul=o[1]; row->half1.cn=o[2]; row->half1.ur=o[3]; row->half1.dr=o[4];
        row->half2.dl=o[5]; row->half2.ul=o[6]; row->half2.cn=o[7]; row->half2.ur=o[8]; row->half2.dr=o[9];
    }
}

/* Half Double: 6 posicoes especificas, por row */
static void rsShuffleHD(StepChart* chart)
{
    int holdDest[6];
    for (int i = 0; i < 6; i++) holdDest[i] = -1;

    for (uint32_t ri = 0; ri < chart->rowCount; ri++) {
        StepRow* row = &chart->rows[ri];
        /* Layout HD: [0]=h1.cn [1]=h1.ur [2]=h1.dr [3]=h2.dl [4]=h2.ul [5]=h2.cn */
        uint8_t v[6] = {
            row->half1.cn, row->half1.ur, row->half1.dr,
            row->half2.dl, row->half2.ul, row->half2.cn
        };
        uint8_t o[6] = { 0 };
        bool occ[6] = { false, false, false, false, false, false };

        for (int i = 0; i < 6; i++) {
            if (v[i] == NT_HOLD_B || v[i] == NT_HOLD_T) {
                int d = holdDest[i];
                if (d >= 0 && d < 6) { o[d] = v[i]; occ[d] = true; }
                if (v[i] == NT_HOLD_T) holdDest[i] = -1;
            }
        }

        int src[6]; int sc = 0;
        int dst[6]; int dc = 0;
        for (int i = 0; i < 6; i++) {
            if (v[i] != 0 && v[i] != NT_HOLD_B && v[i] != NT_HOLD_T) src[sc++] = i;
            if (!occ[i]) dst[dc++] = i;
        }

        shuffleIntArr(src, sc);
        for (int i = 0; i < sc; i++) {
            int s = src[i], d = dst[i];
            o[d] = v[s];
            if (v[s] == NT_HOLD_H) holdDest[s] = d;
        }

        row->half1.cn=o[0]; row->half1.ur=o[1]; row->half1.dr=o[2];
        row->half2.dl=o[3]; row->half2.ul=o[4]; row->half2.cn=o[5];
    }
}

void Step_ApplyRandomShuffle(StepChart* chart, int panelMode, bool shuffleP1, bool shuffleP2)
{
    if (!chart || !chart->rows || chart->rowCount == 0) return;

    if (panelMode == 1) {
        rsShuffleDN(chart);
        Log_Print("RS DN: per-row random, rows=%u\n", chart->rowCount);
    } else if (panelMode == 2) {
        rsShuffleHD(chart);
        Log_Print("RS HD: per-row random, rows=%u\n", chart->rowCount);
    } else {
        if (shuffleP1) rsShuffleSingle(chart, 0);
        if (shuffleP2) rsShuffleSingle(chart, 1);
        Log_Print("RS Single: per-row P1=%d P2=%d rows=%u\n", shuffleP1, shuffleP2, chart->rowCount);
    }
}
