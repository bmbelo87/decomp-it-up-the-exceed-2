#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#define RES_KEY_INIT 0xEF
#define RES_KEY_STEP 0x4F

typedef struct { char name[16]; uint32_t size; uint32_t offset; uint32_t checksum; } RESEntry;

static void res_xor_decrypt(uint8_t* data, uint32_t size) {
    uint8_t key = RES_KEY_INIT;
    for (uint32_t i = 0; i < size; i++) { data[i] ^= key; key = (uint8_t)(key + RES_KEY_STEP); }
}

static uint8_t* load_bga_from_dat(const char* datPath, uint32_t* outSize) {
    FILE* f = fopen(datPath, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", datPath); return NULL; }
    fseek(f, 0, SEEK_END);
    uint32_t fileSize = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc(fileSize);
    fread(data, 1, fileSize, f);
    fclose(f);

    uint32_t fileCount = *(uint32_t*)(data + 8);
    uint32_t dirSize = fileCount * sizeof(RESEntry);
    uint32_t h2 = 0x18 + dirSize;
    uint32_t seekRel = *(uint32_t*)(data + h2 + 0x40);
    uint32_t dataStart = h2 + 0x44 + seekRel;

    uint8_t* dirEnc = data + 0x18;
    RESEntry* entries = (RESEntry*)calloc(fileCount, sizeof(RESEntry));
    memcpy(entries, dirEnc, fileCount * sizeof(RESEntry));
    res_xor_decrypt((uint8_t*)entries, fileCount * sizeof(RESEntry));
    for (uint32_t i = 0; i < fileCount; i++) entries[i].offset += dataStart;

    int bgaIdx = -1;
    for (uint32_t i = 0; i < fileCount; i++) {
        const char* n = entries[i].name;
        int nl = (int)strlen(n);
        if (nl >= 4 && (stricmp(n + nl - 4, ".bga") == 0)) { bgaIdx = (int)i; break; }
    }
    if (bgaIdx < 0) { printf("No .bga found\n"); free(entries); free(data); return NULL; }

    uint8_t* bgaData = (uint8_t*)malloc(entries[bgaIdx].size);
    memcpy(bgaData, data + entries[bgaIdx].offset, entries[bgaIdx].size);
    res_xor_decrypt(bgaData, entries[bgaIdx].size);
    *outSize = entries[bgaIdx].size;
    free(entries);
    free(data);
    return bgaData;
}

int main(int argc, char** argv) {
    const char* path = (argc > 1) ? argv[1] : "F:\\14_px3\\gamePC\\BGA\\81.DAT";
    uint32_t bgaSize = 0;
    uint8_t* bga = load_bga_from_dat(path, &bgaSize);
    if (!bga) return 1;

    printf("=== %s ===\n", path);
    printf("BGA size: %u bytes\n", bgaSize);
    printf("Magic: %.4s\n", bga);

    bool isBGA2 = (memcmp(bga, "BGA2", 4) == 0);
    bool isBGA0 = (memcmp(bga, "BGA", 3) == 0);
    printf("Format: %s\n", isBGA2 ? "BGA2" : (isBGA0 ? "BGA v0" : "UNKNOWN"));

    int kfSize = isBGA2 ? 64 : 44;
    int layerStart = 0;

    // Scan for layers
    int layerNum = 0;
    int maxFrame = 0;
    int scanStart = isBGA2 ? 4 : 4;

    while (scanStart < (int)bgaSize - 4) {
        // Find filename
        char filename[64] = {0};
        int nameStart = -1, nameEnd = -1;
        for (int i = scanStart; i + 4 < (int)bgaSize; i++) {
            if (bga[i] == '.') {
                char ext[5];
                memcpy(ext, bga + i, 4); ext[4] = 0;
                if (stricmp(ext, ".spr") == 0 || stricmp(ext, ".sp2") == 0 ||
                    stricmp(ext, ".tga") == 0 || stricmp(ext, ".TGA") == 0) {
                    int s = i;
                    while (s > 0 && bga[s - 1] >= 0x20 && bga[s - 1] < 0x7F) s--;
                    int len = (i + 4 - s);
                    if (len < 64) { memcpy(filename, bga + s, len); filename[len] = 0; nameStart = s; nameEnd = i + 4; break; }
                }
            }
        }
        if (nameStart < 0) break;

        for (int ci = 0; filename[ci]; ci++)
            if (filename[ci] >= 'A' && filename[ci] <= 'Z') filename[ci] = (char)(filename[ci] + 32);

        int ptr = nameEnd;
        while (ptr < (int)bgaSize && bga[ptr] == 0) ptr++;
        int aligned = ((ptr + 3) / 4) * 4;

        int numKF = 0, dataOff = 0;
        for (int scan = aligned; scan < (int)bgaSize - 4; scan += 4) {
            uint32_t val = *(uint32_t*)(bga + scan);
            if (val >= 1 && val <= 5000) { numKF = (int)val; dataOff = scan + 4; break; }
        }

        if (numKF == 0 || dataOff == 0) { scanStart = nameEnd; continue; }

        printf("\n--- Layer %d: %s (%d/%d keyframes) ---\n", layerNum, filename, numKF, numKF);
        for (int i = 0; i < numKF; i++) {
            uint32_t ofs = dataOff + i * kfSize;
            if (ofs + kfSize > bgaSize) break;

            if (isBGA2) {
                uint32_t ts;
                float r, g, b, a;
                float x, y, hotx, hoty, sx, sy;
                // BGA2: 64-byte keyframe, timestamp at offset 44
                ts = *(uint32_t*)(bga + ofs + 44);
                float* floats = (float*)(bga + ofs);
                x = floats[0]; y = floats[1]; hotx = floats[2]; hoty = floats[3];
                sx = floats[4]; sy = floats[5];
                r  = floats[7]; g = floats[8]; b = floats[9]; a = floats[10];

                int frame = (int)(ts & 0xFFFF);
                int type = (int)((ts >> 16) & 0xFFFF);

                printf("  kf[%3d]: frame=%4d type=%d (x=%.0f y=%.0f hx=%.0f hy=%.0f sx=%.2f sy=%.2f) a=%.2f\n",
                       i, frame, type, x, y, hotx, hoty, sx, sy, a);
            } else {
                // BGA v0: 44-byte keyframe
                int16_t frame = *(int16_t*)(bga + ofs);
                int16_t spriteID = *(int16_t*)(bga + ofs + 2);
                float* floats = (float*)(bga + ofs + 4);
                float x = floats[0], y = floats[1], hotx = floats[2], hoty = floats[3];
                float scale = floats[4], rot = floats[5];
                float r = floats[6], g = floats[7], b = floats[8], a = floats[9];

                printf("  kf[%3d]: frame=%4d spriteID=%d (x=%.0f y=%.0f hx=%.0f hy=%.0f scale=%.2f rot=%.2f) r=%.2f g=%.2f b=%.2f a=%.2f\n",
                       i, frame, spriteID, x, y, hotx, hoty, scale, rot, r, g, b, a);
            }
        }

        if (numKF > 0) {
            uint32_t lastOfs = dataOff + (numKF - 1) * kfSize;
            uint32_t ts = *(uint32_t*)(bga + lastOfs);
            int lastFrame = (int)(ts & 0xFFFF);
            if (lastFrame > maxFrame) maxFrame = lastFrame;
        }

        scanStart = dataOff + numKF * kfSize;
        layerNum++;
    }

    printf("\n=== maxFrame across all layers: %d ===\n", maxFrame);
    free(bga);
    return 0;
}
