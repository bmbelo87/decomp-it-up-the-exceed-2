#include "pumpy.h"
#include <stdio.h>

// BGA2 Event structure (64 bytes) - based on hex analysis
typedef struct {
    int32_t frame;      // 0-3
    int32_t picIndex;   // 4-7
    int32_t flags;      // 8-11
    float x;            // 12-15
    float y;            // 16-19
    float w;            // 20-23
    float h;            // 24-27
    float sx;           // 28-31
    float sy;           // 32-35
    float u1;           // 36-39
    float v1;           // 40-43
    float u2;           // 44-47
    float v2;           // 48-51
    int16_t alpha;      // 52-53
    uint8_t r;          // 54
    uint8_t g;          // 55
    uint8_t b;          // 56
    uint8_t a;          // 57
    int32_t pad1;       // 58-61
    int32_t pad2;       // 62-65 -> actually 60-63
} BGA2EventParsed; // 64 bytes

void BGA2_DumpEvents(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) return;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t* data = (uint8_t*)malloc(size);
    fread(data, 1, size, f);
    fclose(f);
    
    // BGA2 header is 0xD8 bytes
    int headerSize = 0xD8;
    int eventSize = 64;
    int eventCount = (size - headerSize) / eventSize;
    
    printf("BGA2: %s\n", filename);
    printf("  Size: %ld bytes\n", size);
    printf("  Header: %d bytes\n", headerSize);
    printf("  Events: %d\n", eventCount);
    printf("\n");
    
    for (int i = 0; i < eventCount && i < 10; i++) {
        BGA2EventParsed* evt = (BGA2EventParsed*)(data + headerSize + i * eventSize);
        printf("Event %d:\n", i);
        printf("  frame=%d, picIndex=%d, flags=%d\n", evt->frame, evt->picIndex, evt->flags);
        printf("  x=%.2f, y=%.2f, w=%.2f, h=%.2f\n", evt->x, evt->y, evt->w, evt->h);
        printf("  sx=%.2f, sy=%.2f\n", evt->sx, evt->sy);
        printf("  u1=%.2f, v1=%.2f, u2=%.2f, v2=%.2f\n", evt->u1, evt->v1, evt->u2, evt->v2);
        printf("  alpha=%d, r=%d, g=%d, b=%d, a=%d\n", evt->alpha, evt->r, evt->g, evt->b, evt->a);
        printf("\n");
    }
    
    free(data);
}

int main() {
    BGA2_DumpEvents("F:\\14_px3\\gamePC\\BGA\\R_WARN.DAT");
    return 0;
}
