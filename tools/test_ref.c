#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    FILE* f = fopen("F:/14_px3/gamePC/STEP/101.decompressed", "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* ref = (uint8_t*)malloc(sz);
    fread(ref, 1, sz, f);
    fclose(f);
    printf("Reference: %ld bytes\n", sz);
    printf("First 32 ref: ");
    for (int i = 0; i < 32; i++) printf("%02x ", ref[i]);
    printf("\n");

    // What does Python decompress to from 0x1F0?
    // Stream: 51 bytes, decompresses to 2212
    // Let me check the ref matches
    free(ref);
    return 0;
}
