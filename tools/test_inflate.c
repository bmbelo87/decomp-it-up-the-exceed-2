#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "zlibinflate.h"

int main(void) {
    FILE* f = fopen("F:/14_px3/gamePC/STEP/101.STX", "rb");
    if (!f) { printf("FAIL: open\n"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* data = (uint8_t*)malloc(sz);
    fread(data, 1, sz, f);
    fclose(f);

    struct { int pos; int len; int expect; } streams[] = {
        {0x1F0, 51, 2212},
        {0x2F3, 47, 2212},
        {0x3F2, 409, 5683},
        {0x65B, 47, 2212},
        {0x75A, 480, 11572},
        {0xA0A, 446, 5774},
        {0xC98, 47, 2212},
        {0xD97, 47, 2212},
    };
    
    int allOk = 1;
    for (int si = 0; si < 8; si++) {
        uint8_t out[16384];
        uint32_t outLen = sizeof(out);
        int ret = zlib_decompress(data + streams[si].pos, streams[si].len, out, &outLen);
        int ok = (ret == 0 && outLen == (uint32_t)streams[si].expect);
        printf("Stream 0x%X: ret=%d outLen=%u (expected %d) %s\n",
               streams[si].pos, ret, outLen,
               streams[si].expect, ok ? "OK" : "FAIL");
        if (!ok) allOk = 0;
    }
    
    free(data);
    printf("\n%s\n", allOk ? "ALL OK" : "SOME FAILED");
    return allOk ? 0 : 1;
}
