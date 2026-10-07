#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define RES_KEY_INIT 0xEF
#define RES_KEY_STEP 0x4F

typedef struct { char name[16]; uint32_t size; uint32_t offset; uint32_t checksum; } RESEntry;

static void res_xor_decrypt(uint8_t* data, uint32_t size) {
    uint8_t key = RES_KEY_INIT;
    for (uint32_t i = 0; i < size; i++) { data[i] ^= key; key = (uint8_t)(key + RES_KEY_STEP); }
}

int main() {
    const char* path = "F:\\14_px3\\gamePC\\BGA\\R_WARN.DAT";
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL: cannot open %s\n", path); return 1; }
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
    for (uint32_t i = 0; i < fileCount; i++) {
        entries[i].offset += dataStart;
    }

    int bgaIdx = -1;
    for (uint32_t i = 0; i < fileCount; i++) {
        const char* n = entries[i].name;
        int nl = (int)strlen(n);
        if (nl >= 4 && (stricmp(n + nl - 4, ".bga") == 0)) { bgaIdx = i; break; }
    }
    if (bgaIdx < 0) { printf("No .bga\n"); free(data); free(entries); return 1; }

    uint8_t* bgaData = (uint8_t*)malloc(entries[bgaIdx].size);
    memcpy(bgaData, data + entries[bgaIdx].offset, entries[bgaIdx].size);
    res_xor_decrypt(bgaData, entries[bgaIdx].size);
    uint32_t bgaSize = entries[bgaIdx].size;

    printf("=== R_WARN.BGA hex dump (header) ===\n");
    printf("BGA size: %u bytes\n\n", bgaSize);
    printf("Offset 0x00-0x17 (BGA2 header):\n");
    for (int i = 0; i < 0x18; i++) {
        if (i % 16 == 0) printf("  %04X: ", i);
        printf("%02X ", bgaData[i]);
        if (i % 16 == 15) printf("\n");
    }

    printf("\nOffset 0x18-0xD7 (between header and events):\n");
    for (int i = 0x18; i < 0xD8; i++) {
        if (i % 16 == 0) printf("  %04X: ", i);
        printf("%02X ", bgaData[i]);
        if (i % 16 == 15) { printf(" |"); for (int j = i - 15; j <= i; j++) { char c = bgaData[j]; printf("%c", (c >= 32 && c < 127) ? c : '.'); } printf("|"); printf("\n"); }
    }
    if (0xD8 % 16 != 0) printf("\n");

    printf("\n=== Event analysis with hdr=0xD8, 64-byte events ===\n");
    uint32_t hdr = 0xD8;
    uint32_t evtCount = (bgaSize - hdr) / 64;
    printf("Total events: %u\n\n", evtCount);

    printf("=== REAL events (frame=0, picIdx=0, non-zero data) ===\n");
    for (uint32_t i = 0; i < evtCount; i++) {
        uint8_t* ep = bgaData + hdr + i * 64;
        int32_t frame = *(int32_t*)(ep + 0);
        int32_t picIdx = *(int32_t*)(ep + 4);
        float unk8 = *(float*)(ep + 8);

        if (frame != 0) continue;
        if (picIdx != 0) continue;

        printf("evt[%3u]: ", i);
        for (int b = 0; b < 64; b++) {
            if (b > 0 && b % 4 == 0) printf(" ");
            printf("%02X", ep[b]);
        }
        printf("\n");
    }

    printf("\n=== Events with interesting data (frame=0, xywh non-zero) ===\n");
    for (uint32_t i = 0; i < evtCount; i++) {
        uint8_t* ep = bgaData + hdr + i * 64;
        int32_t frame = *(int32_t*)(ep + 0);
        float x = *(float*)(ep + 12);
        float y = *(float*)(ep + 16);
        float w = *(float*)(ep + 20);
        float h = *(float*)(ep + 24);

        if (frame != 0) continue;
        if (x == 0 && y == 0 && w <= 1 && h <= 1) continue;

        printf("evt[%3u]: frame=%d picIdx=%d xywh=(%.0f,%.0f,%.0f,%.0f)",
               i, frame, *(int32_t*)(ep + 4), x, y, w, h);
        printf(" sx=%.1f sy=%.1f", *(float*)(ep + 28), *(float*)(ep + 32));
        printf(" uv=(%.0f,%.0f)-(%.0f,%.0f)\n",
               *(float*)(ep + 36), *(float*)(ep + 40), *(float*)(ep + 44), *(float*)(ep + 48));
    }

    printf("\n=== Events with frame > 0 and frame < 1000 ===\n");
    for (uint32_t i = 0; i < evtCount; i++) {
        uint8_t* ep = bgaData + hdr + i * 64;
        int32_t frame = *(int32_t*)(ep + 0);
        if (frame <= 0 || frame > 1000) continue;
        printf("evt[%3u]: frame=%d picIdx=%d xywh=(%.0f,%.0f,%.0f,%.0f)",
               i, frame, *(int32_t*)(ep + 4),
               *(float*)(ep + 12), *(float*)(ep + 16), *(float*)(ep + 20), *(float*)(ep + 24));
        printf(" sx=%.1f sy=%.1f uv=(%.0f,%.0f)-(%.0f,%.0f)\n",
               *(float*)(ep + 28), *(float*)(ep + 32),
               *(float*)(ep + 36), *(float*)(ep + 40), *(float*)(ep + 44), *(float*)(ep + 48));
    }

    printf("\n=== Looking for interleaved texture/ref tables ===\n");
    printf("Events that have frame != 0 but contain recognizable patterns:\n");
    for (uint32_t i = 0; i < evtCount; i++) {
        uint8_t* ep = bgaData + hdr + i * 64;
        int32_t frame = *(int32_t*)(ep + 0);
        if (frame == 0) continue;

        if (frame == 1065353216) {
            printf("evt[%3u]: FLOAT1.0 marker - ", i);
        } else if (frame > 1000 && frame < 100000) {
            printf("evt[%3u]: mid-range frame=%d - ", i, frame);
        } else if (frame >= 100000) {
            printf("evt[%3u]: HIGH frame=%d (0x%X) - ", i, frame, frame);
        } else continue;

        int32_t picIdx = *(int32_t*)(ep + 4);
        printf("picIdx=%d (0x%X), bytes: ", picIdx, picIdx);
        for (int b = 0; b < 16; b++) printf("%02X ", ep[b]);
        printf("\n");
    }

    printf("\n=== SPR files in DAT ===\n");
    for (uint32_t i = 0; i < fileCount; i++) {
        const char* name = entries[i].name;
        printf("  [%d] %s (size=%u, offset=0x%X)\n", i, name, entries[i].size, entries[i].offset);
    }

    printf("\n=== Header fields analysis ===\n");
    printf("Offset 0x00: '%.4s' (magic)\n", bgaData);
    printf("Offset 0x04: %u (0x%X)\n", *(uint32_t*)(bgaData + 4), *(uint32_t*)(bgaData + 4));
    printf("Offset 0x08: %u (0x%X)\n", *(uint32_t*)(bgaData + 8), *(uint32_t*)(bgaData + 8));
    printf("Offset 0x0C: %u (0x%X)\n", *(uint32_t*)(bgaData + 0xC), *(uint32_t*)(bgaData + 0xC));
    printf("Offset 0x10: '%.16s'\n", bgaData + 0x10);

    free(bgaData);
    free(data);
    free(entries);
    return 0;
}