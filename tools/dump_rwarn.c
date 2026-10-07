#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <windows.h>

#define RES_KEY_INIT 0xEF
#define RES_KEY_STEP 0x4F

typedef struct { char name[16]; uint32_t size; uint32_t offset; uint32_t checksum; } RESEntry;

static void res_xor_decrypt(uint8_t* data, uint32_t size) {
    uint8_t key = RES_KEY_INIT;
    for (uint32_t i = 0; i < size; i++) { data[i] ^= key; key = (uint8_t)(key + RES_KEY_STEP); }
}

#pragma pack(push, 1)
typedef struct { int32_t frame; int32_t picIndex; int32_t unk8; float x; float y; float w; float h; float sx; float sy; float u1; float v1; float u2; float v2; uint32_t pad[3]; } BGA2EventRaw;
#pragma pack(pop)

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

    // Find .bga file
    int bgaIdx = -1;
    for (uint32_t i = 0; i < fileCount; i++) {
        const char* n = entries[i].name;
        int nl = (int)strlen(n);
        if (nl >= 4 && (stricmp(n + nl - 4, ".bga") == 0)) { bgaIdx = i; break; }
    }
    if (bgaIdx < 0) { printf("No .bga\n"); free(data); free(entries); return 1; }

    uint8_t* bgaData = malloc(entries[bgaIdx].size);
    memcpy(bgaData, data + entries[bgaIdx].offset, entries[bgaIdx].size);
    res_xor_decrypt(bgaData, entries[bgaIdx].size);
    uint32_t bgaSize = entries[bgaIdx].size;
    printf("BGA: %s, %u bytes\n\n", entries[bgaIdx].name, bgaSize);

    // Scan for ".spr" to find where the sprite reference is
    int sprRefEnd = -1;
    uint32_t bestHdr = 0x18;
    for (uint32_t i = 4; i + 4 < bgaSize && i < 0x200; i++) {
        if (memcmp(bgaData + i, ".spr", 4) == 0 || memcmp(bgaData + i, ".SPR", 4) == 0) {
            int start = (int)i;
            while (start > 0 && bgaData[start - 1] >= 0x20 && bgaData[start - 1] < 0x7F) start--;
            sprRefEnd = i + 4;
            printf("Found spr '%.*s' at offset 0x%X (name start=0x%X, name end=0x%X)\n",
                (i - start) + 4, bgaData + start, i, start, sprRefEnd);
            bestHdr = ((sprRefEnd + 63) / 64) * 64;
            printf("Calculated header (aligned to 64): 0x%X\n", bestHdr);
            break;
        }
    }

    // Now try to find actual header by looking for event count
    // The event count is at offset 0x04-0x07, but it's 0 for this BGA.
    // Let's try all possible headers and see which gives valid event data
    printf("\n=== Searching for valid header ===\n");
    for (uint32_t hdr = 0x18; hdr + 64 <= bgaSize && hdr <= 0x200; hdr += 8) {
        int remain = (int)(bgaSize - hdr);
        if (remain <= 0) continue;
        if (remain % 64 != 0) continue;
        BGA2EventRaw* e0 = (BGA2EventRaw*)(bgaData + hdr);
        // Check for valid frame numbers and non-zero picIndex or position
        bool allZero = true;
        uint32_t maxFrame = 0;
        uint32_t evtCount = remain / 64;
        for (uint32_t ei = 0; ei < evtCount; ei++) {
            BGA2EventRaw* e = (BGA2EventRaw*)(bgaData + hdr + ei * 64);
            if (e->frame < 0 || e->frame > 100000) { allZero = false; break; }
            if (e->x != 0 || e->y != 0 || e->w != 0 || e->h != 0) allZero = false;
        }
        if (!allZero && remain > 0 && (remain % 64) == 0) {
            printf("hdr=0x%X (%d): %u events, remain=%d\n", hdr, hdr, evtCount, remain);
            break;
        }
    }

    // Now show what proper header should be based on spr ref
    printf("\n=== Events at hdr=0xD8 ===\n");
    uint32_t hdr = 0xD8;
    uint32_t evtCount = (bgaSize - hdr) / 64;
    printf("Event count: %u\n\n", evtCount);

    int lastFrame = -1;
    int maxPics[128], maxPicCount = 0;
    for (uint32_t i = 0; i < evtCount; i++) {
        BGA2EventRaw* e = (BGA2EventRaw*)(bgaData + hdr + i * 64);
        if (e->frame != lastFrame) {
            if (lastFrame >= 0 || i > 0) printf("\n");
            printf("Frame %d:\n", e->frame);
            lastFrame = e->frame;
        }
        printf("  evt[%3u]: picIdx=%-3d unk8=%-4d xywh=(%.0f,%.0f,%.0f,%.0f) sx=%.2f sy=%.2f uv=(%.0f,%.0f)-(%.0f,%.0f)\n",
            i, e->picIndex, e->unk8, e->x, e->y, e->w, e->h, e->sx, e->sy, e->u1, e->v1, e->u2, e->v2);
        
        // Collect unique picIndex
        bool found = false;
        for (int p = 0; p < maxPicCount; p++) { if (maxPics[p] == e->picIndex) { found = true; break; } }
        if (!found && maxPicCount < 128) { maxPics[maxPicCount++] = e->picIndex; }
    }

    printf("\n=== Unique picIndex values ===\n");
    for (int p = 0; p < maxPicCount; p++) {
        printf("  picIndex=%d (0x%X)\n", maxPics[p], maxPics[p]);
    }

    // Dump SPR tiles
    printf("\n=== SPR Files & Tiles ===\n");
    for (uint32_t i = 0; i < fileCount; i++) {
        const char* name = entries[i].name;
        int nl = (int)strlen(name);
        if ((nl >= 4 && (stricmp(name + nl - 4, ".spr") == 0 || stricmp(name + nl - 4, ".sp2") == 0))) {
            uint32_t sprSize = entries[i].size;
            uint8_t* sprData = (uint8_t*)malloc(sprSize);
            memcpy(sprData, data + entries[i].offset, sprSize);
            res_xor_decrypt(sprData, sprSize);
            printf("\n--- %.16s (%u bytes) ---\n", name, sprSize);
            printf("%.*s", sprSize, sprData);
            free(sprData);
        }
    }

    free(bgaData);
    free(data);
    free(entries);
    return 0;
}
