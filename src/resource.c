#include "pumpy.h"
#include "vsl.h"
#include "df_resource.h"
#include "zlibinflate.h"

#if !defined(_WIN32)
#include <dirent.h>
#endif

#define RES_MAGIC "RES"
#define RES_KEY_INIT 0xEF
#define RES_KEY_STEP 0x4F

typedef struct {
    char name[16];
    uint32_t size;
    uint32_t offset;
    uint32_t checksum;
} RESEntry; // 28 bytes total

typedef struct {
    char path[MAX_PATH];
    uint8_t* data;
    uint32_t fileSize;
    int fileCount;
    RESEntry* entries;
    int isPack;          /* RESPACK (Exceed): data já decifrado e descomprimido */
} RESArchive;

static RESArchive* g_resArchive = NULL;

static void res_xor_decrypt(uint8_t* data, uint32_t size) {
    uint8_t key = RES_KEY_INIT;
    for (uint32_t i = 0; i < size; i++) {
        data[i] ^= key;
        key = (uint8_t)(key + RES_KEY_STEP);
    }
}

/* ---------------------------------------------------------------------------
 * RESPACK — BGA\*.DAT do Exceed (exceed.exe).
 *   0x00 "RESPACK\x1A", 0x0C u32 entradas; índice em 0x18 (0x28 se o DWORD em
 *   0x18 for zero), entradas de 0x12C bytes, XOR 0x5C + 0xC1 por byte.
 *   entrada: +0x000 nome, +0x10C tamanho real, +0x110 tamanho comprimido,
 *            +0x118 chave (16 bytes), +0x128 offset.
 *   dados em header + n*0x12C + offset + 0x118 -> chave derivada (0x411DBC),
 *   XOR (0x423234) e zlib.
 * ------------------------------------------------------------------------- */
#define RESPACK_MAGIC   "RESPACK\x1A"
#define RESPAC2_MAGIC   "RESPAC2\x1A"   /* Exceed2: PIU32.EXE 0x421c40 */
#define RESPACK_ENTRY   0x12C

static const uint8_t g_packT0[8] = { 0xF0,0x78,0xF9,0xFD,0x1C,0x20,0xC2,0x02 }; /* 0x411dc4 */
static const uint8_t g_packT1[8] = { 0xFF,0xFE,0xFC,0xF8,0xF0,0xE0,0xC0,0x7F }; /* 0x411f94 */
static uint8_t g_packState;                                                     /* 0x1284ab0 */

/* 0x411df3 */
static uint8_t pack_f(uint8_t c, uint8_t s) {
    if (c == 0) return g_packT0[s & 7];
    uint8_t a = pack_f((uint8_t)((c - 1) & 7), (uint8_t)((s - 1) & 7));
    uint8_t r = (uint8_t)((a << 1) | (((a >> 7) ^ (a >> 6)) & 1));
    if (s == 7) r ^= pack_f(c, 0);
    return r;
}

/* 0x411dcc */
static void pack_step0(void) {
    uint8_t dl = 0;
    for (int b = 0; b < 8; b++)
        if ((g_packState >> b) & 1) dl ^= g_packT1[b];
    g_packState = dl;
}

/* 0x411e67 */
static void pack_stepk(uint8_t k) {
    uint8_t r = 0;
    for (int b = 0; b < 8; b++)
        if ((g_packState >> b) & 1) r ^= pack_f(k, (uint8_t)b);
    g_packState = r;
}

/* 0x411ead */
static uint8_t pack_h(uint8_t x) {
    uint8_t r = 0;
    for (int c = 0; c < 8; c++) {
        if (c == 0) pack_step0();
        r ^= (uint8_t)(((g_packState >> c) & 1) << c);
        if (!((x >> c) & 1)) pack_stepk((uint8_t)c);
    }
    return r;
}

/* 0x411dbc -> 0x411f06 */
static void pack_derive_key(int n, const uint8_t* in, uint8_t* out) {
    g_packState = 0xFC;
    uint8_t prev = pack_h((uint8_t)~in[0]);
    for (int i = 1; i < n; i++) {
        uint8_t a = pack_h((uint8_t)~in[i]);
        out[i - 1] = (uint8_t)((prev >> 3) | (a << 5));
        prev = a;
    }
    uint8_t a = pack_h(0);
    out[n - 1] = (uint8_t)((a << 5) | (prev >> 3));
}

/* 0x411dbc com 16 bytes — usado também pelo ENC2 dos .AUD (audio.c) */
void RESPACK_DeriveKey16(const uint8_t* in, uint8_t* out) {
    pack_derive_key(16, in, out);
}

/* 0x423234 */
static void pack_decrypt(uint8_t* data, uint32_t len, const uint8_t* key16) {
    uint8_t k[16];
    pack_derive_key(16, key16, k);
    for (uint32_t i = 0; i < len; i++) {
        data[i] ^= k[i & 15];
        k[i & 15] = (uint8_t)(k[i & 15] + 0x54);
    }
}

/* PIU32.EXE 0x420610: out[i] = in[i] ^ k[i&3], k[j] = (k[j] + i) ^ 0x1C.
 * Os 4 bytes iniciais de k vêm de 0x43f220 (stub de dongle, senha 0xA5A5);
 * os .AUD/.PNZ do jogo foram cifrados com resposta 0 (recuperado por texto
 * conhecido e conferido com o Adler-32 de 0x423b40). */
static void x2_derive_key(int n, const uint8_t* in, uint8_t* out) {
    uint8_t k[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        out[i] = (uint8_t)(in[i] ^ k[i & 3]);
        k[i & 3] = (uint8_t)((k[i & 3] + i) ^ 0x1C);
    }
}

/* zlib adler32(1, ...) — PIU32.EXE 0x423b40 */
static uint32_t enc2_adler32(const uint8_t* d, uint32_t n) {
    uint32_t a = 1, b = 0;
    for (uint32_t i = 0; i < n; i++) {
        a = (a + d[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static uint8_t enc2_bitrev(uint8_t b) {   /* 0x421280 */
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) { r = (uint8_t)((r << 1) | (b & 1)); b >>= 1; }
    return r;
}

/* ENC2 (.AUD / .PNZ).
 *   Exceed2 (PIU32.EXE 0x421390): hdr[0x8C], chave16, hdr[0x88] bytes, u32
 *     semente (= Adler-32 do resultado), T[0x400], dados[hdr[0x84]].
 *     K = 0x420610(chave16); tabela[j] = T[j] ^ K[j&15] (0x4212b0);
 *     saida[i] = bitrev(src[i]) ^ tabela[(semente+i) & 0x3FF] (0x421320).
 *   Exceed (exceed.exe 0x420390): sem chave16; tabela = 0x411DBC sobre T.
 * O cabeçalho não diferencia os dois: tenta o Exceed2 e confere o Adler-32. */
uint8_t* Resource_DecryptENC2(const uint8_t* buf, uint32_t fileSize, uint32_t* outSize) {
    if (fileSize < 0x8C || memcmp(buf, "ENC2", 4) != 0) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        uint32_t size = *(const uint32_t*)(buf + 0x84);
        uint32_t off = 0x8C + (pass == 0 ? 0x10 : 0) + *(const uint32_t*)(buf + 0x88);
        if (off + 4 + 0x400 > fileSize) continue;
        uint32_t avail = fileSize - off - 4 - 0x400;
        if (size > avail) size = avail;
        uint32_t seed = *(const uint32_t*)(buf + off);
        uint8_t table[0x400];
        if (pass == 0) {
            uint8_t K[16];
            x2_derive_key(16, buf + 0x8C, K);
            for (int j = 0; j < 0x400; j++) table[j] = (uint8_t)(buf[off + 4 + j] ^ K[j & 15]);
        } else {
            for (int k = 0; k < 0x400; k += 16)
                RESPACK_DeriveKey16(buf + off + 4 + k, table + k);
        }
        uint8_t* dec = (uint8_t*)malloc(size ? size : 1);
        if (!dec) return NULL;
        const uint8_t* src = buf + off + 4 + 0x400;
        for (uint32_t i = 0; i < size; i++)
            dec[i] = enc2_bitrev(src[i]) ^ table[(seed + i) & 0x3FF];
        if (pass == 1 || enc2_adler32(dec, size) == seed) {
            *outSize = size;
            return dec;
        }
        free(dec);
    }
    return NULL;
}

/* Decifra e descomprime todas as entradas; res->data passa a ser o bloco
 * descomprimido e entries[].offset/size apontam dentro dele. */
static bool respack_load(RESArchive* res) {
    uint8_t* d = res->data;
    int isX2 = memcmp(d, RESPAC2_MAGIC, 8) == 0;
    /* Exceed2: XOR dos dados só com o byte 9 != 0 (0x421cc9, [0xC9B284]);
     * chave = entrada ^ G, G = 0x420610(cabeçalho 0x18..0x27) (0x421b90) */
    int x2Crypt = isX2 && d[9] != 0;
    uint8_t x2G[16];
    if (isX2) x2_derive_key(16, d + 0x18, x2G);
    int n = (int)*(uint32_t*)(d + 0x0C);
    uint32_t hdr = (*(uint32_t*)(d + 0x18) == 0) ? 0x28 : 0x18;
    uint32_t idxSize = (uint32_t)n * RESPACK_ENTRY;
    if (n <= 0 || hdr + idxSize > res->fileSize) return false;

    uint8_t* idx = (uint8_t*)malloc(idxSize);
    if (!idx) return false;
    for (uint32_t i = 0; i < idxSize; i++)
        idx[i] = d[hdr + i] ^ (uint8_t)(0x5C + 0xC1 * i);

    res->fileCount = n;
    res->entries = (RESEntry*)calloc(n, sizeof(RESEntry));
    uint32_t total = 0;
    for (int e = 0; e < n; e++) total += *(uint32_t*)(idx + e * RESPACK_ENTRY + 0x10C);
    uint8_t* out = (uint8_t*)malloc(total ? total : 1);
    if (!res->entries || !out) { free(idx); free(out); return false; }

    uint32_t base = hdr + idxSize + 0x118, pos = 0;
    for (int e = 0; e < n; e++) {
        uint8_t* ent = idx + e * RESPACK_ENTRY;
        uint32_t rawSize = *(uint32_t*)(ent + 0x10C);
        uint32_t cSize   = *(uint32_t*)(ent + 0x110);
        uint32_t off     = *(uint32_t*)(ent + 0x128);
        strncpy(res->entries[e].name, (const char*)ent, sizeof(res->entries[e].name) - 1);
        if (base + off + cSize > res->fileSize) { free(idx); free(out); return false; }

        uint8_t* blob = (uint8_t*)malloc(cSize ? cSize : 1);
        if (!blob) { free(idx); free(out); return false; }
        memcpy(blob, d + base + off, cSize);
        if (!isX2) {
            pack_decrypt(blob, cSize, ent + 0x118);
        } else if (x2Crypt) {
            uint8_t k[16];
            for (int i = 0; i < 16; i++) k[i] = (uint8_t)(ent[0x118 + i] ^ x2G[i]);
            for (uint32_t i = 0; i < cSize; i++) {
                blob[i] ^= k[i & 15];
                k[i & 15] = (uint8_t)(k[i & 15] + 0x54);
            }
        }
        uint32_t got = rawSize;
        int zr = zlib_decompress(blob, cSize, out + pos, &got);
        free(blob);
        if (zr != 0 || got != rawSize) {
            Log_Print("RESPACK: falha ao descomprimir '%s' (zlib=%d)\n", res->entries[e].name, zr);
            free(idx); free(out); return false;
        }
        res->entries[e].offset = pos;
        res->entries[e].size = rawSize;
        pos += rawSize;
    }
    free(idx);
    free(res->data);
    res->data = out;
    res->fileSize = total;
    res->isPack = 1;
    return true;
}

static int res_find_by_name(RESArchive* res, const char* name) {
    for (int i = 0; i < res->fileCount; i++) {
        if (_stricmp(res->entries[i].name, name) == 0)
            return i;
    }
    return -1;
}

#if !defined(_WIN32)
static FILE* res_fopen_case_insensitive(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) return f;

    const char* slash = strrchr(path, '/');
    char dirPath[MAX_PATH];
    const char* fileName = path;
    if (slash) {
        size_t dirLen = (size_t)(slash - path);
        if (dirLen == 0) dirLen = 1;
        if (dirLen >= sizeof(dirPath)) return NULL;
        memcpy(dirPath, path, dirLen);
        dirPath[dirLen] = '\0';
        fileName = slash + 1;
    } else {
        strcpy(dirPath, ".");
    }

    DIR* dir = opendir(dirPath);
    if (!dir) return NULL;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcasecmp(entry->d_name, fileName) == 0) {
            char actualPath[MAX_PATH];
            snprintf(actualPath, sizeof(actualPath), "%s/%s", dirPath, entry->d_name);
            f = fopen(actualPath, "rb");
            break;
        }
    }
    closedir(dir);
    return f;
}
#endif

bool RES_Open(const char* path) {
    if (g_resArchive) RES_Close();

    RESArchive* res = (RESArchive*)calloc(1, sizeof(RESArchive));
    if (!res) return false;

    strncpy(res->path, path, sizeof(res->path) - 1);

    FILE* f =
#if !defined(_WIN32)
        res_fopen_case_insensitive(path);
#else
        fopen(path, "rb");
#endif
    if (!f) { free(res); return false; }

    fseek(f, 0, SEEK_END);
    res->fileSize = (uint32_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    res->data = (uint8_t*)malloc(res->fileSize);
    if (!res->data) { fclose(f); free(res); return false; }

    if (fread(res->data, 1, res->fileSize, f) != res->fileSize) {
        fclose(f); free(res->data); free(res); return false;
    }
    fclose(f);

    if (res->fileSize >= 0x28 && (memcmp(res->data, RESPACK_MAGIC, 8) == 0 ||
                                    memcmp(res->data, RESPAC2_MAGIC, 8) == 0)) {
        if (!respack_load(res)) {
            Log_Print("RESPACK: falha ao abrir '%s'\n", path);
            free(res->data); free(res->entries); free(res); return false;
        }
        g_resArchive = res;
        Log_Print("RESPACK: opened '%s' (%d files, %u bytes)\n", path, res->fileCount, res->fileSize);
        return true;
    }

    if (memcmp(res->data, RES_MAGIC, 4) != 0) {
        Log_Print("RES: invalid magic in '%s'\n", path);
        free(res->data); free(res); return false;
    }

    res->fileCount = *(uint32_t*)(res->data + 8);
    uint32_t dirSize = res->fileCount * sizeof(RESEntry);

    uint8_t* dirEnc = res->data + 0x18;
    res->entries = (RESEntry*)calloc(res->fileCount, sizeof(RESEntry));
    if (!res->entries) { free(res->data); free(res); return false; }

    memcpy(res->entries, dirEnc, dirSize);
    res_xor_decrypt((uint8_t*)res->entries, dirSize);

    uint32_t h2 = 0x18 + dirSize;
    uint32_t seekRel = *(uint32_t*)(res->data + h2 + 0x40);
    uint32_t dataStart = h2 + 0x44 + seekRel;

    for (int i = 0; i < res->fileCount; i++) {
        res->entries[i].offset += dataStart;
    }

    g_resArchive = res;
    Log_Print("RES: opened '%s' (%d files, %u bytes)\n", path, res->fileCount, res->fileSize);
    return true;
}

int RES_GetCount(void) {
    return g_resArchive ? g_resArchive->fileCount : 0;
}

int RES_Find(const char* name) {
    if (!g_resArchive) return -1;
    return res_find_by_name(g_resArchive, name);
}

const char* RES_GetName(int index) {
    if (!g_resArchive || index < 0 || index >= g_resArchive->fileCount) return NULL;
    return g_resArchive->entries[index].name;
}

uint32_t RES_GetSize(int index) {
    if (!g_resArchive || index < 0 || index >= g_resArchive->fileCount) return 0;
    return g_resArchive->entries[index].size;
}

bool RES_Read(int index, uint8_t* buffer) {
    if (!g_resArchive || index < 0 || index >= g_resArchive->fileCount) return false;
    RESEntry* e = &g_resArchive->entries[index];
    if (e->offset + e->size > g_resArchive->fileSize) return false;
    memcpy(buffer, g_resArchive->data + e->offset, e->size);
    if (!g_resArchive->isPack) res_xor_decrypt(buffer, e->size);
    return true;
}

uint8_t* RES_ReadAlloc(int index) {
    if (!g_resArchive || index < 0 || index >= g_resArchive->fileCount) return NULL;
    RESEntry* e = &g_resArchive->entries[index];
    uint8_t* buf = (uint8_t*)malloc(e->size);
    if (!buf) return NULL;
    if (!RES_Read(index, buf)) { free(buf); return NULL; }
    return buf;
}

void RES_Close(void) {
    if (g_resArchive) {
        free(g_resArchive->data);
        free(g_resArchive->entries);
        free(g_resArchive);
        g_resArchive = NULL;
    }
}

bool RES_IsOpen(void) {
    return g_resArchive != NULL;
}

void* Resource_Load(int resId, const char* type, DWORD* outSize) {
    /* Windows .rc embedded resources are not available on the Linux port.
     * No caller uses this path (it is kept to preserve the API). */
    (void)resId;
    (void)type;
    if (outSize) *outSize = 0;
    return NULL;
}

int Resource_LoadWave(int resId, const char* name) {
    DWORD size;
    void* data = Resource_Load(resId, "WAVE", &size);
    if (!data) return -1;
    return Audio_LoadWAV(name, (const uint8_t*)data, size);
}

int Resource_LoadTexture(int resId, const char* name) {
    DWORD size;
    void* data = Resource_Load(resId, "TEXTURE", &size);
    if (!data) return -1;

    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    char tmpPath[MAX_PATH];
    snprintf(tmpPath, sizeof(tmpPath), "%s_tmp_%s", tmpDir, name);

    FILE* f = fopen(tmpPath, "wb");
    if (!f) { free(data); return -1; }
    fwrite(data, 1, size, f);
    fclose(f);

    int texId = Texture_Load(tmpPath);
    remove(tmpPath);
    return texId;
}

static bool hasExt(const char* name, const char* ext) {
    int nl = (int)strlen(name);
    int el = (int)strlen(ext);
    if (nl < el) return false;
    return _stricmp(name + nl - el, ext) == 0;
}

int loadTextureFromRES(const char* resName) {
    int idx = RES_Find(resName);
    Log_Print("RES: tex find '%s' -> %d\n", resName, idx);
    if (idx < 0) {
        const char* ext = strrchr(resName, '.');
        char base[64];
        if (ext) {
            int baseLen = (int)(ext - resName);
            int cpLen = baseLen < 63 ? baseLen : 63;
            memcpy(base, resName, cpLen);
            base[cpLen] = '\0';
        } else {
            strncpy(base, resName, sizeof(base) - 1);
        }

        const char* exts[] = { "png", "tga", "jpg", "bmp", "dds" }; /* dds: Exceed */
        for (int ei = 0; ei < 5; ei++) {
            char tryName[64];
            snprintf(tryName, sizeof(tryName), "%s.%s", base, exts[ei]);
            idx = RES_Find(tryName);
            Log_Print("RES: tex try '%s' -> %d\n", tryName, idx);
            if (idx >= 0) {
                Log_Print("RES: tex matched entry name='%s'\n", RES_GetName(idx));
                break;
            }
        }
        if (idx < 0) return -1;
    }

    uint32_t sz = RES_GetSize(idx);
    if (sz == 0) return -1;

    uint8_t* buf = RES_ReadAlloc(idx);
    if (!buf) return -1;

    char tmpDir[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpDir);
    char tmpPath[MAX_PATH];
    snprintf(tmpPath, sizeof(tmpPath), "%s_tmp_%s", tmpDir, RES_GetName(idx));

    FILE* f = fopen(tmpPath, "wb");
    if (f) {
        fwrite(buf, 1, sz, f);
        fclose(f);
        Log_Print("RES: tex loading '%s' (%u bytes)\n", tmpPath, sz);
        int texId = Texture_Load(tmpPath);
        Log_Print("RES: tex loaded -> %d\n", texId);
        remove(tmpPath);
        free(buf);
        return texId;
    }
    Log_Print("RES: tex write failed for '%s'\n", tmpPath);
    free(buf);
    return -1;
}

#define SPR_MAX_TILES 512

typedef struct {
    char name[16];
    char texture[64];
    int srcX, srcY, srcW, srcH;  // Posição e tamanho na tela
    int u1, v1, u2, v2;          // Coordenadas de recorte na textura
    bool flipH, flipV;           // Espelhamento
} SPRTile;

typedef struct {
    bool isAni;
    int tileCount;
    int patCols;
    int patRows;
    int patFlags;
    SPRTile tiles[SPR_MAX_TILES];
} SPRFile;

static void ParseSPR_TileDefinition(SPRFile* spr, char tokens[10][64], int tokCount) {
    SPRTile* t = &spr->tiles[spr->tileCount];
    memset(t, 0, sizeof(SPRTile));
    int ti = 0;
    strncpy(t->texture, tokens[ti], sizeof(t->texture) - 1); ti++;
    t->srcX = atoi(tokens[ti+0]);
    t->srcY = atoi(tokens[ti+1]);
    t->srcW = atoi(tokens[ti+2]);
    t->srcH = atoi(tokens[ti+3]);
    int u1 = atoi(tokens[ti+4]);
    int v1 = atoi(tokens[ti+5]);
    int u2 = atoi(tokens[ti+6]);
    int v2 = atoi(tokens[ti+7]);
    t->u1 = u1; t->v1 = v1;
    t->u2 = u2; t->v2 = v2;
    t->flipH = (u1 > u2);
    t->flipV = (v1 > v2);
    for (int ci = 0; t->texture[ci]; ci++)
        if (t->texture[ci] >= 'A' && t->texture[ci] <= 'Z')
            t->texture[ci] = (char)(t->texture[ci] + 32);
    spr->tileCount++;
}

static void ParseSP2_TileDefinition(SPRFile* spr, char tokens[10][64], int tokCount) {
    SPRTile* t = &spr->tiles[spr->tileCount];
    memset(t, 0, sizeof(SPRTile));
    int ti = 0;
    // .sp2: primeiro token é nome do tile
    if (tokCount >= 10 && !strchr(tokens[0], '.')) {
        ti = 1;  // pular nome
    }
    strncpy(t->texture, tokens[ti], sizeof(t->texture) - 1); ti++;
    t->srcX = atoi(tokens[ti+0]);
    t->srcY = atoi(tokens[ti+1]);
    t->srcW = atoi(tokens[ti+2]);
    t->srcH = atoi(tokens[ti+3]);
    int u1 = atoi(tokens[ti+4]);
    int v1 = atoi(tokens[ti+5]);
    int u2 = atoi(tokens[ti+6]);
    int v2 = atoi(tokens[ti+7]);
    t->u1 = u1; t->v1 = v1;
    t->u2 = u2; t->v2 = v2;
    t->flipH = (u2 < 0);
    t->flipV = (v2 < 0);
    for (int ci = 0; t->texture[ci]; ci++)
        if (t->texture[ci] >= 'A' && t->texture[ci] <= 'Z')
            t->texture[ci] = (char)(t->texture[ci] + 32);
    spr->tileCount++;
}

static bool parseSPR(const uint8_t* data, uint32_t size, SPRFile* spr) {
    memset(spr, 0, sizeof(SPRFile));
    const char* text = (const char*)data;

    // Parse header lines: TYPE ani/tile, NUM <count>
    int expectedTiles = 0;
    for (uint32_t i = 0; i + 4 < size; i++) {
        if (text[i] == '\n' || text[i] == '\r') { i++; continue; }
        if (i > 0 && text[i-1] != '\n' && text[i-1] != '\r') continue;
        // At start of a line
        if ((text[i+0] == 'T' || text[i+0] == 't') &&
            (text[i+1] == 'Y' || text[i+1] == 'y') &&
            (text[i+2] == 'P' || text[i+2] == 'p') &&
            (text[i+3] == 'E' || text[i+3] == 'e')) {
            const char* rest = text + i + 4;
            while (*rest == ' ' || *rest == '\t') rest++;
            if ((rest[0] == 'a' || rest[0] == 'A') &&
                (rest[1] == 'n' || rest[1] == 'N') &&
                (rest[2] == 'i' || rest[2] == 'I')) {
                spr->isAni = true;
            } else if ((rest[0] == 'p' || rest[0] == 'P') &&
                       (rest[1] == 'a' || rest[1] == 'A') &&
                       (rest[2] == 't' || rest[2] == 'T')) {
                // TYPE PATTERN <flag> <cols> <rows>
                const char* p = rest + 7;
                while (*p == ' ' || *p == '\t') p++;
                spr->patFlags = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                while (*p == ' ' || *p == '\t') p++;
                spr->patCols = atoi(p);
                while (*p >= '0' && *p <= '9') p++;
                while (*p == ' ' || *p == '\t') p++;
                spr->patRows = atoi(p);
            }
        } else if ((text[i+0] == 'N' || text[i+0] == 'n') &&
                   (text[i+1] == 'U' || text[i+1] == 'u') &&
                   (text[i+2] == 'M' || text[i+2] == 'm')) {
            const char* rest = text + i + 3;
            while (*rest == ' ' || *rest == '\t') rest++;
            expectedTiles = atoi(rest);
        }
    }

    // Formato SPR:
    // TYPE tile/ani
    // NUM <count>
    // T <texture> <srcX> <srcY> <srcW> <srcH> <u1> <v1> <u2> <v2>
    
    for (uint32_t i = 0; i < size; i++) {
        if (text[i] != 'T' && text[i] != 't') continue;
        if (i > 0 && text[i-1] != '\n' && text[i-1] != '\r') continue;
        if (i + 2 >= size) continue;
        if (text[i+1] != ' ' && text[i+1] != '\t') continue;
        if (i > 0 && text[i-1] != '\n' && text[i-1] != '\r') continue;
        if (i + 2 >= size) continue;
        if (text[i+1] != ' ' && text[i+1] != '\t') continue;

        const char* line = text + i + 2;
        uint32_t lineLen = 0;
        while (i + 2 + lineLen < size && line[lineLen] >= ' ') lineLen++;
        if (lineLen == 0) continue;

        char buf[256];
        uint32_t cp = lineLen < sizeof(buf) - 1 ? lineLen : sizeof(buf) - 1;
        memcpy(buf, line, cp);
        buf[cp] = '\0';

        char tokens[10][64];
        int tokCount = 0;
        memset(tokens, 0, sizeof(tokens));
        char* p = buf;
        while (*p && tokCount < 10) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            int ti = 0;
            while (*p && *p != ' ' && *p != '\t' && ti < 63) {
                tokens[tokCount][ti++] = *p++;
            }
            tokens[tokCount][ti] = '\0';
            tokCount++;
        }

        if (tokCount < 9) continue;

        // Detecta formato: .sp2 tem nome do tile (token sem '.') antes da textura
        if (tokCount >= 10 && !strchr(tokens[0], '.')) {
            ParseSP2_TileDefinition(spr, tokens, tokCount);
        } else {
            ParseSPR_TileDefinition(spr, tokens, tokCount);
        }

        if (spr->tileCount >= SPR_MAX_TILES) break;
    }

    return spr->tileCount > 0;
}

static bool isValidBGA(const uint8_t* data, uint32_t size) {
    if (size < 8) return false;
    return (memcmp(data, "BGA2", 4) == 0) || (memcmp(data, "BGA", 3) == 0);
}

/* ---------------------------------------------------------------------------
 * BGA3 — Exceed2 (PIU32.EXE 0x41e190, chamado por 0x41dce0 com o byte 3 == '3').
 *   0x00 "BGA3", 0x04 3 x u32 (não lidos pelo carregador além do bloco de 0xC)
 *   100 x { nome[0x40], s32 n (< 0 -> 0), n x keyframe de 0x64 bytes }
 *     keyframe (layout interno, igual ao que 0x41dea0 monta a partir do BGA2):
 *       +0 s16 quadro, +2 s16 grupo, +4 s16 interp., +6 s16 blend,
 *       +8 x, +C y, +10 hotx, +14 hoty, +18 sx, +1C sy, +20 rot, +24/+28 rot
 *       (outros eixos), +2C r g b a, +3C.. não usados aqui.
 *   "SCENE1", u32 n, n x cena de 0x50 bytes (ver BGAScene).
 * 0x41eee0 expande os grupos: +2 == 1 continua o grupo; o keyframe que fecha
 * repete [início..ele] max(+2,1) vezes (0 -> 1, último com 1 -> 1), deslocando
 * o quadro pelo vão do grupo; no resultado +2 vira 0 quando != 1.
 * ------------------------------------------------------------------------- */
static void bga3_emit(BGALayer* layer, const uint8_t* r, int frameAdd) {
    if (layer->kfCount >= MAX_BGA_KEYFRAMES) return;
    BGAKeyframe* kf = &layer->keyframes[layer->kfCount++];
    int16_t grp = *(const int16_t*)(r + 2);
    kf->frame = (int)*(const int16_t*)(r + 0) + frameAdd;
    kf->type = (grp == 1) ? 1 : 0;
    kf->blendMode = (int)*(const int16_t*)(r + 6);
    kf->x = *(const float*)(r + 0x08);
    kf->y = *(const float*)(r + 0x0C);
    kf->hotx = *(const float*)(r + 0x10);
    kf->hoty = *(const float*)(r + 0x14);
    kf->scaleX = *(const float*)(r + 0x18);
    kf->scaleY = *(const float*)(r + 0x1C);
    kf->rotation = *(const float*)(r + 0x20);   /* eixo Z: hipótese (BGA2 grava a rotação aqui) */
    kf->r = *(const float*)(r + 0x2C);
    kf->g = *(const float*)(r + 0x30);
    kf->b = *(const float*)(r + 0x34);
    kf->a = *(const float*)(r + 0x38);
    kf->z_order = 0;
}

static bool parseBGA3(const uint8_t* d, uint32_t size, BGAPicture* bga) {
    bga->version = 3;
    bga->scaleX = bga->scaleY = 1.0f;
    uint32_t pos = 16;
    for (int slot = 0; slot < 100; slot++) {
        if (pos + 0x44 > size) return false;
        char name[64];
        memcpy(name, d + pos, 64);
        name[63] = '\0';
        int n = *(const int32_t*)(d + pos + 0x40);
        if (n < 0) n = 0;
        pos += 0x44;
        if (pos + (uint32_t)n * 0x64 > size) return false;
        const uint8_t* recs = d + pos;
        pos += (uint32_t)n * 0x64;

        if (slot >= MAX_BGA_LAYERS) {
            if (n > 0) Log_Print("BGA3: slot %d '%s' ignorado (MAX_BGA_LAYERS)\n", slot, name);
            continue;
        }
        /* slots vazios contam (índice de camada == slot, como no BGA2 do RES) */
        BGALayer* layer = &bga->layers[slot];
        bga->layerCount = slot + 1;
        if (name[0] == '\0' || n == 0) continue;

        for (int ci = 0; name[ci]; ci++)
            if (name[ci] >= 'A' && name[ci] <= 'Z') name[ci] = (char)(name[ci] + 32);
        strncpy(layer->filename, name, sizeof(layer->filename) - 1);
        const char* dot = strrchr(name, '.');
        layer->isSPR = (dot && (_stricmp(dot, ".spr") == 0 || _stricmp(dot, ".sp2") == 0));

        /* 0x41eee0 */
        for (int i = 0; i < n; i++) {
            int16_t g = *(const int16_t*)(recs + i * 0x64 + 2);
            int rep = (g == 1) ? 0 : (g == 0 ? 1 : g);
            if (i == n - 1 && rep == 0) rep = 1;
            if (rep == 0) continue;
            int j = i;
            while (j > 0 && *(const int16_t*)(recs + (j - 1) * 0x64 + 2) == 1) j--;
            if (j >= i) continue;
            int span = *(const int16_t*)(recs + i * 0x64) - *(const int16_t*)(recs + j * 0x64);
            for (int k = 0; k < rep; k++)
                for (int m = j; m <= i; m++)
                    bga3_emit(layer, recs + m * 0x64, k * span);
        }
        if (layer->kfCount > 0) {
            int maxF = layer->keyframes[layer->kfCount - 1].frame;
            if (maxF > bga->maxFrame) bga->maxFrame = maxF;
        }
        Log_Print("BGA3: slot %d '%s' %s kf=%d->%d\n", slot, name, layer->isSPR ? "SPR" : "TGA", n, layer->kfCount);
    }

    /* 0x41e48a: "SCENE1" + cenas */
    if (pos + 10 <= size && memcmp(d + pos, "SCENE1", 6) == 0) {
        pos += 6;
        uint32_t n = *(const uint32_t*)(d + pos);
        pos += 4;
        for (uint32_t i = 0; i < n && pos + 0x50 <= size; i++, pos += 0x50) {
            if (bga->sceneCount >= MAX_BGA_SCENES) break;
            BGAScene* sc = &bga->scenes[bga->sceneCount++];
            memcpy(sc->name, d + pos + 4, 63);
            sc->name[63] = '\0';
            sc->start = *(const int16_t*)(d + pos + 0x44);
            sc->end   = *(const int16_t*)(d + pos + 0x46);
            sc->loop  = *(const int16_t*)(d + pos + 0x48);
            sc->mode  = *(const int32_t*)(d + pos + 0x4C);
            sc->cur = sc->start;        /* 0x41e504 */
            sc->rev = 0;
            Log_Print("BGA3: cena '%s' %d..%d volta=%d modo=%d\n", sc->name, sc->start, sc->end, sc->loop, sc->mode);
        }
    }
    return true;
}

static bool loadBGAFromRES(const char* bgaName, int bgaIdx) {
    uint32_t bgaSize = RES_GetSize(bgaIdx);
    uint8_t* bgaData = RES_ReadAlloc(bgaIdx);
    if (!bgaData) return false;

    bool isBGA2 = (memcmp(bgaData, "BGA2", 4) == 0);

    if (!isBGA2 && memcmp(bgaData, "BGA", 3) != 0) {
        Log_Print("BGA: unknown BGA format\n");
        free(bgaData);
        return false;
    }

    BGAPicture* bga = &g_game.bgaPics[g_game.bgaPicCount];
    memset(bga, 0, sizeof(BGAPicture));
    snprintf(bga->name, sizeof(bga->name), "%s", bgaName);

    if (memcmp(bgaData, "BGA3", 4) == 0) {
        bool ok = parseBGA3(bgaData, bgaSize, bga);
        free(bgaData);
        if (!ok) return false;
        g_game.bgaPicCount++;
        Log_Print("BGA: loaded '%s' (v3, %d slots, %d cenas)\n", bgaName, bga->layerCount, bga->sceneCount);
        return true;
    }

    if (isBGA2) {
        bga->version = 2;
        int evPos = 16;
        int maxEvents = 50;
        for (int evt = 0; evt < maxEvents && evPos + 68 <= (int)bgaSize && bga->layerCount < MAX_BGA_LAYERS; evt++)
        {
            char filename[64];
            memset(filename, 0, sizeof(filename));
            int nameLen = 0;
            for (int i = 0; i < 64 && evPos + i < (int)bgaSize; i++)
            {
                char c = bgaData[evPos + i];
                if (c >= 0x20 && c <= 0x7E)
                {
                    if (nameLen < 63) filename[nameLen++] = c;
                }
                else break;
            }
            filename[nameLen] = '\0';

            int count = 0;
            if (evPos + 68 <= (int)bgaSize)
                count = *(int*)(bgaData + evPos + 64);

            BGALayer* layer = &bga->layers[bga->layerCount];

            // Increment layerCount for ALL entries (empty entries keep indices in sync)
            if (count > 0 && nameLen > 0)
            {
                for (int ci = 0; filename[ci]; ci++)
                    if (filename[ci] >= 'A' && filename[ci] <= 'Z')
                        filename[ci] = (char)(filename[ci] + 32);

                strncpy(layer->filename, filename, sizeof(layer->filename) - 1);
                char* dot = strrchr(filename, '.');
                layer->isSPR = (dot && (_stricmp(dot, ".spr") == 0 || _stricmp(dot, ".sp2") == 0));

                int kfDataOff = evPos + 68;
                layer->kfCount = 0;
                for (int i = 0; i < count && layer->kfCount < MAX_BGA_KEYFRAMES; i++)
                {
                    uint32_t ofs = kfDataOff + i * 64;
                    if (ofs + 64 > bgaSize) break;
                    uint8_t* raw = bgaData + ofs;

                    float* floats = (float*)raw;
                    uint32_t ts = *(uint32_t*)(raw + 44);
                    int32_t blendMode = *(int16_t*)(raw + 48);
                    uint32_t z_order = *(uint32_t*)(raw + 52);

                    BGAKeyframe* kf = &layer->keyframes[layer->kfCount];
                    /* BGA2: frame é int16_t signed (valores negativos = "ativo desde antes do frame 0") */
                    kf->frame = (int)(int16_t)(ts & 0xFFFF);
                    kf->type = (int)((ts >> 16) & 0xFFFF);
                    kf->x = floats[0];
                    kf->y = floats[1];
                    kf->hotx = floats[2];
                    kf->hoty = floats[3];
                    kf->scaleX = floats[4];
                    kf->scaleY = floats[5];
                    kf->rotation = floats[6];
                    kf->r = floats[7];
                    kf->g = floats[8];
                    kf->b = floats[9];
                    kf->a = floats[10];
                    kf->blendMode = (int)blendMode;
                    kf->z_order = (int)z_order;
                    layer->kfCount++;
                }

                if (layer->kfCount > 0)
                {
                    int maxF = layer->keyframes[layer->kfCount - 1].frame;
                    if (maxF > bga->maxFrame) bga->maxFrame = maxF;
                }

                Log_Print("BGA2: evt[%d] '%s' %s kf=%d maxFrame=%d\n",
                    evt, filename, layer->isSPR ? "SPR" : "TGA", layer->kfCount, bga->maxFrame);
            }
            else
            {
                Log_Print("BGA2: evt[%d] (empty)\n", evt);
            }
            bga->layerCount++;

            evPos += 64 + 4 + (count > 0 ? count * 64 : 0);
        }
    } else {
        bga->version = 0;
        int kfSize = 44;

        // Fixed-record format: [64 bytes name][4 bytes count][count * 44 bytes keyframes]
        // Matches original EXE's sequential file read (FUN_00401020)
        int entryPos = 16;  // skip 16-byte header
        while (entryPos + 64 + 4 <= (int)bgaSize && bga->layerCount < MAX_BGA_LAYERS) {
            char filename[64];
            memcpy(filename, bgaData + entryPos, 64);
            filename[63] = '\0';

            int nameLen = (int)strlen(filename);
            if (nameLen == 0) {
                // Empty slot, no keyframe data
                int count = *(int*)(bgaData + entryPos + 64);
                entryPos += 68 + count * kfSize;
                continue;
            }

            int count = *(int*)(bgaData + entryPos + 64);
            if (count == 0) {
                entryPos += 68;
                continue;
            }

            for (int ci = 0; filename[ci]; ci++) {
                if (filename[ci] >= 'A' && filename[ci] <= 'Z')
                    filename[ci] = (char)(filename[ci] + 32);
            }

            BGALayer* layer = &bga->layers[bga->layerCount];
            strncpy(layer->filename, filename, sizeof(layer->filename) - 1);

            char* dot = strrchr(filename, '.');
            layer->isSPR = (dot && (_stricmp(dot, ".spr") == 0 || _stricmp(dot, ".sp2") == 0));

            if (bga->layerCount == 0) {
                strncpy(bga->name, filename, sizeof(bga->name) - 1);
            }

            int dataOff = entryPos + 68;
            layer->kfCount = 0;
            for (int i = 0; i < count && layer->kfCount < MAX_BGA_KEYFRAMES; i++) {
                uint32_t ofs = dataOff + i * kfSize;
                if (ofs + kfSize > bgaSize) break;

                int16_t frame = *(int16_t*)(bgaData + ofs);
                int16_t spriteID = *(int16_t*)(bgaData + ofs + 2);
                float* floats = (float*)(bgaData + ofs + 4);
                BGAKeyframe* kf = &layer->keyframes[layer->kfCount];

                kf->frame = frame;
                kf->type = (spriteID != 0) ? 1 : 0;
                kf->x = floats[0];
                kf->y = floats[1];
                kf->hotx = floats[2];
                kf->hoty = floats[3];
                kf->scaleX = floats[4];
                kf->scaleY = floats[4];
                kf->rotation = floats[5];
                kf->r = floats[6];
                kf->g = floats[7];
                kf->b = floats[8];
                kf->a = floats[9];
                kf->blendMode = 0;

                Log_Print("  BGA0 kf[%d]: frame=%d spriteID=%d x=%.1f y=%.1f sc=%.2f rot=%.2f r=%.2f g=%.2f b=%.2f a=%.2f\n",
                          i, frame, spriteID,
                          floats[0], floats[1], floats[4], floats[5],
                          floats[6], floats[7], floats[8], floats[9]);

                layer->kfCount++;
            }

            if (layer->kfCount > 0) {
                int maxF = layer->keyframes[layer->kfCount - 1].frame;
                if (maxF > bga->maxFrame) bga->maxFrame = maxF;
            }

            Log_Print("BGA0: '%s' %s kf=%d maxFrame=%d\n",
                filename, layer->isSPR ? "SPR" : "TGA", layer->kfCount, bga->maxFrame);

            bga->layerCount++;
            entryPos += 68 + count * kfSize;
        }
    }

    free(bgaData);
    g_game.bgaPicCount++;
    Log_Print("BGA: loaded '%s' (v%d, %d layers)\n",
        bgaName, bga->version, bga->layerCount);

    return true;
}

int loadSPRFromRES(const char* sprName, int* outPatCols, int* outPatRows, int* outPatFlags) {
    char sprFileName[64];
    const char* ext = strrchr(sprName, '.');
    if (ext && (_stricmp(ext, ".spr") == 0 || _stricmp(ext, ".sp2") == 0)) {
        strncpy(sprFileName, sprName, sizeof(sprFileName) - 1);
    } else {
        snprintf(sprFileName, sizeof(sprFileName), "%s.spr", sprName);
    }

    int sprIdx = RES_Find(sprFileName);
    if (sprIdx < 0) {
        char altName[64];
        if (ext && _stricmp(ext, ".sp2") == 0) {
            snprintf(altName, sizeof(altName), "%.*s.spr", (int)(ext - sprName), sprName);
        } else if (ext) {
            snprintf(altName, sizeof(altName), "%.*s.sp2", (int)(ext - sprName), sprName);
        } else {
            snprintf(altName, sizeof(altName), "%s.sp2", sprName);
        }
        sprIdx = RES_Find(altName);
        if (sprIdx < 0) {
            for (int i = 0; i < RES_GetCount(); i++) {
                const char* n = RES_GetName(i);
                if (n && (_stricmp(n, sprFileName) == 0 || _stricmp(n, altName) == 0)) { sprIdx = i; break; }
            }
        }
    }

    if (sprIdx < 0) return 0;

    uint32_t sprSize = RES_GetSize(sprIdx);
    uint8_t* sprData = RES_ReadAlloc(sprIdx);
    if (!sprData) return 0;

    SPRFile spr;
    if (!parseSPR(sprData, sprSize, &spr)) {
        free(sprData);
        Log_Print("RES: spr '%s' parse failed (%d bytes)\n", sprFileName, sprSize);
        return 0;
    }

    Log_Print("RES: spr '%s' (%d tiles, isAni=%d)\n", sprFileName, spr.tileCount, spr.isAni);

    for (int i = 0; i < spr.tileCount && g_game.sprTileCount < MAX_SPR_TILES; i++) {
        SPRTileDef* t = &g_game.sprTiles[g_game.sprTileCount];
        // Nome do tile = nome do arquivo SPR + "_" + tileIndex
        // Para que possamos encontrar tiles por nome do arquivo SPR
        snprintf(t->name, sizeof(t->name), "%s_%d", sprFileName, i);
        // Remove extensão do nome
        for (int ci = 0; t->name[ci]; ci++) {
            if (t->name[ci] >= 'A' && t->name[ci] <= 'Z')
                t->name[ci] = (char)(t->name[ci] + 32);
        }
        strncpy(t->texture, spr.tiles[i].texture, sizeof(t->texture) - 1);
        t->srcX = spr.tiles[i].srcX;
        t->srcY = spr.tiles[i].srcY;
        t->srcW = spr.tiles[i].srcW;
        t->srcH = spr.tiles[i].srcH;
        t->flipH = spr.tiles[i].flipH;
        t->flipV = spr.tiles[i].flipV;
        t->texId = -1;

        char texName[64];
        strncpy(texName, spr.tiles[i].texture, sizeof(texName) - 1);
        for (int ci = 0; texName[ci]; ci++) {
            if (texName[ci] >= 'A' && texName[ci] <= 'Z')
                texName[ci] = (char)(texName[ci] + 32);
        }
        t->texId = loadTextureFromRES(texName);

        if (t->texId >= 0 && t->texId < MAX_TEXTURES && g_game.textures[t->texId].inUse) {
            float texW = (float)g_game.textures[t->texId].width;
            float texH = (float)g_game.textures[t->texId].height;
            if (texW <= 0) texW = 256.0f;
            if (texH <= 0) texH = 256.0f;

            bool isSp2 = (ext && _stricmp(ext, ".sp2") == 0);
            if (isSp2) {
                // .sp2: u2/v2 são largura/altura a partir de u1/v1 (negativo = flip)
                // NÃO inverter start/end — a ordem original codifica o flip
                float uStart = (float)spr.tiles[i].u1;
                float uEnd = uStart + (float)spr.tiles[i].u2;
                float vStart = (float)spr.tiles[i].v1;
                float vEnd = vStart + (float)spr.tiles[i].v2;
                t->u1 = uStart / texW;
                t->u2 = uEnd / texW;
            t->v1 = vStart / texH;
            t->v2 = vEnd / texH;
        } else {
            t->u1 = (float)spr.tiles[i].u1 / texW;
            t->u2 = (float)spr.tiles[i].u2 / texW;
            t->v1 = (float)spr.tiles[i].v1 / texH;
            t->v2 = (float)spr.tiles[i].v2 / texH;
        }
        } else {
            t->u1 = 0.0f; t->v1 = 0.0f;
            t->u2 = 1.0f; t->v2 = 1.0f;
        }

        g_game.sprTileCount++;
    }

    free(sprData);
    if (spr.patCols > 0 && spr.patRows > 0) {
        if (outPatCols) *outPatCols = spr.patCols;
        if (outPatRows) *outPatRows = spr.patRows;
        if (outPatFlags) *outPatFlags = spr.patFlags;
        return spr.tileCount;
    }
    return spr.isAni ? spr.tileCount : 0;
}

bool Resource_LoadBGA(int resId) {
    (void)resId;
    return false;
}

static void loadAllSPRsFromRES(void) {
    int count = RES_GetCount();
    for (int i = 0; i < count; i++) {
        const char* name = RES_GetName(i);
        if (name && (hasExt(name, ".spr") || hasExt(name, ".sp2"))) {
            loadSPRFromRES(name, NULL, NULL, NULL);
        }
    }
}

int Resource_LoadAllSPRs(const char* datPath) {
    if (!RES_Open(datPath)) {
        Log_Print("RES: failed to open '%s' for SPR loading\n", datPath);
        return 0;
    }
    int startCount = g_game.sprTileCount;
    loadAllSPRsFromRES();
    RES_Close();
    int loaded = g_game.sprTileCount - startCount;
    Log_Print("RES: loaded %d SPR tiles from '%s'\n", loaded, datPath);
    return loaded;
}

bool Resource_LoadBGAByName(const char* datName) {
    Log_Print("RES: loading BGA '%s'\n", datName);

    char datPath[MAX_PATH];
    const char* paths[] = {
        "%s/BGA/%s.DAT",
        "%s/assets/BGA/%s.DAT",
        "%s/BGA/%s.dat",
        "%s/assets/BGA/%s.dat"
    };

    bool opened = false;
    for (int i = 0; i < 4; i++) {
        snprintf(datPath, sizeof(datPath), paths[i], g_game.currentDirectory, datName);
        if (RES_Open(datPath)) { opened = true; break; }
    }
    if (!opened) return false;

    int bgaIdx = -1;
    for (int i = 0; i < RES_GetCount(); i++) {
        const char* n = RES_GetName(i);
        if (n && hasExt(n, ".bga")) { bgaIdx = i; break; }
    }

    if (bgaIdx >= 0) {
        if (!loadBGAFromRES(datName, bgaIdx)) {
            RES_Close();
            return false;
        }

        BGAPicture* bga = &g_game.bgaPics[g_game.bgaPicCount - 1];

        for (int i = 0; i < bga->layerCount; i++) {
            BGALayer* layer = &bga->layers[i];
            const char* srcName = layer->filename;
            if (srcName[0] == '\0') continue;
            if (*srcName == '?') srcName++;
            if (layer->isSPR && layer->sprTileCount == 0) {
                layer->sprTileStart = g_game.sprTileCount;
                int patC = 0, patR = 0, patF = 0;
                int aniFrames = loadSPRFromRES(srcName, &patC, &patR, &patF);
                layer->sprTileCount = g_game.sprTileCount - layer->sprTileStart;
                layer->aniFrameCount = aniFrames;
                layer->patCols = patC;
                layer->patRows = patR;
                layer->patFlags = patF;
            } else if (!layer->isSPR) {
                layer->texId = loadTextureFromRES(srcName);
            }
        }
    } else {
        Log_Print("RES: no .bga in '%s', loading SPRs only\n", datName);
        loadAllSPRsFromRES();
    }

    RES_Close();
    return true;
}

bool Resource_LoadBGADirect(const char* datPath) {
    g_game.isVSL = false;

    if (VSL_IsVSL(datPath)) {
        g_game.isVSL = true;
        bool result = VSL_Load(datPath);
        return result;
    }

    if (!RES_Open(datPath)) return false;

    int bgaIdx = -1;
    for (int i = 0; i < RES_GetCount(); i++) {
        const char* n = RES_GetName(i);
        if (n && hasExt(n, ".bga")) { bgaIdx = i; break; }
    }
    if (bgaIdx < 0) { RES_Close(); return false; }

    uint32_t bgaSize = RES_GetSize(bgaIdx);
    uint8_t* bgaData = RES_ReadAlloc(bgaIdx);
    if (!bgaData) { RES_Close(); return false; }

    bool isBGA2 = (memcmp(bgaData, "BGA2", 4) == 0);

    BGAPicture* bga = &g_game.bgaPics[g_game.bgaPicCount];
    memset(bga, 0, sizeof(BGAPicture));
    snprintf(bga->name, sizeof(bga->name), "%s", RES_GetName(bgaIdx));
    if (hasExt(bga->name, ".bga")) bga->name[strlen(bga->name) - 4] = '\0';

    int kfSize;
    if (isBGA2) {
        bga->version = 2;
        kfSize = 64;
    } else if (memcmp(bgaData, "BGA", 3) == 0) {
        bga->version = 0;
        kfSize = 44;
    } else {
        Log_Print("BGA: unknown BGA format\n");
        free(bgaData);
        RES_Close();
        return false;
    }

    int entryPos = 16;
    while (entryPos + 64 + 4 <= (int)bgaSize && bga->layerCount < MAX_BGA_LAYERS) {
        char filename[64];
        memcpy(filename, bgaData + entryPos, 64);
        filename[63] = '\0';

        int nameLen = (int)strlen(filename);
        int count = *(int*)(bgaData + entryPos + 64);

        if (nameLen == 0 && count == 0) {
            entryPos += 68 + count * kfSize;
            bga->layerCount++;
            continue;
        }

        for (int ci = 0; filename[ci]; ci++) {
            if (filename[ci] >= 'A' && filename[ci] <= 'Z')
                filename[ci] = (char)(filename[ci] + 32);
        }

        BGALayer* layer = &bga->layers[bga->layerCount];
        strncpy(layer->filename, filename, sizeof(layer->filename) - 1);

        char* dot = strrchr(filename, '.');
        layer->isSPR = (dot && (_stricmp(dot, ".spr") == 0 || _stricmp(dot, ".sp2") == 0));

        int dataOff = entryPos + 68;
        int readCount = count;
        if (readCount > MAX_BGA_KEYFRAMES)
            readCount = MAX_BGA_KEYFRAMES;
        layer->kfCount = 0;
        for (int i = 0; i < readCount && layer->kfCount < MAX_BGA_KEYFRAMES; i++) {
            uint32_t ofs = dataOff + i * kfSize;
            if (ofs + (uint32_t)kfSize > bgaSize) break;

            BGAKeyframe* kf = &layer->keyframes[layer->kfCount];

            if (kfSize == 64) {
                float* kfData = (float*)(bgaData + ofs);
                uint32_t ts = *(uint32_t*)(bgaData + ofs + 44);
                /* BGA2: frame é int16_t signed (valores negativos = "ativo desde antes do frame 0") */
                kf->frame = (int)(int16_t)(ts & 0xFFFF);
                kf->type = (int)((ts >> 16) & 0xFFFF);
                kf->x = kfData[0];
                kf->y = kfData[1];
                kf->hotx = kfData[2];
                kf->hoty = kfData[3];
                kf->scaleX = kfData[4];
                kf->scaleY = kfData[5];
                kf->rotation = kfData[6];
                kf->r = kfData[7];
                kf->g = kfData[8];
                kf->b = kfData[9];
                kf->a = kfData[10];
                kf->blendMode = (int)*(int16_t*)(bgaData + ofs + 48);
                kf->z_order = (int)*(int32_t*)(bgaData + ofs + 52);
            } else {
                int16_t frame = *(int16_t*)(bgaData + ofs);
                int16_t spriteID = *(int16_t*)(bgaData + ofs + 2);
                float* floats = (float*)(bgaData + ofs + 4);
                kf->frame = frame;
                kf->type = (spriteID != 0) ? 1 : 0;
                kf->x = floats[0];
                kf->y = floats[1];
                kf->hotx = floats[2];
                kf->hoty = floats[3];
                kf->scaleX = floats[4];
                kf->scaleY = floats[4];
                kf->rotation = floats[5];
                kf->r = floats[6];
                kf->g = floats[7];
                kf->b = floats[8];
                kf->a = floats[9];
                kf->blendMode = 0;
            }
            layer->kfCount++;
        }

        if (layer->kfCount > 0) {
            int maxF = layer->keyframes[layer->kfCount - 1].frame;
            if (maxF > bga->maxFrame) bga->maxFrame = maxF;
        }

        Log_Print("BGA: '%s' %s kf=%d maxFrame=%d\n",
            filename, layer->isSPR ? "SPR" : "TGA", layer->kfCount, bga->maxFrame);

        bga->layerCount++;
        entryPos += 68 + count * kfSize;
    }

    free(bgaData);
    g_game.bgaPicCount++;

    for (int i = 0; i < bga->layerCount; i++) {
        BGALayer* layer = &bga->layers[i];
        const char* srcName = layer->filename;
        if (srcName[0] == '\0') continue;
        if (*srcName == '?') srcName++;
        if (layer->isSPR) {
            layer->sprTileStart = g_game.sprTileCount;
            int patC = 0, patR = 0, patF = 0;
            int aniFrames = loadSPRFromRES(srcName, &patC, &patR, &patF);
            layer->sprTileCount = g_game.sprTileCount - layer->sprTileStart;
            layer->aniFrameCount = aniFrames;
            layer->patCols = patC;
            layer->patRows = patR;
            layer->patFlags = patF;
            Log_Print("BGA SPR layer[%d] '%s': sprTileStart=%d sprTileCount=%d aniFrames=%d patCols=%d patRows=%d\n",
                i, srcName, layer->sprTileStart, layer->sprTileCount, aniFrames, patC, patR);
            /* DBG: logar texId dos tiles carregados */
            for (int ti = layer->sprTileStart; ti < layer->sprTileStart + layer->sprTileCount && ti < g_game.sprTileCount; ti++) {
                Log_Print("  tile[%d] name='%s' texId=%d\n", ti, g_game.sprTiles[ti].name, g_game.sprTiles[ti].texId);
            }
        } else {
            layer->texId = loadTextureFromRES(srcName);
            Log_Print("BGA TGA layer[%d] '%s': texId=%d\n", i, srcName, layer->texId);
        }
    }

    RES_Close();
    return true;
}

bool Resource_LoadStage(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';

        if (strlen(line) == 0 || line[0] == ';' || line[0] == '#') continue;

        char key[64], val[192];
        if (sscanf(line, "%63[^=]=%191s", key, val) == 2) {
            if (_stricmp(key, "BGA") == 0) {
                char bgaPath[MAX_PATH];
                snprintf(bgaPath, sizeof(bgaPath), "%s/BGA/%s.DAT", g_game.currentDirectory, val);
                Resource_LoadBGADirect(bgaPath);
            } else if (_stricmp(key, "MUSIC") == 0) {
                Log_Print("Resource: music '%s' (not yet implemented)\n", val);
            }
        }
    }

    fclose(f);
    Log_Print("Resource: loaded stage '%s'\n", path);
    return true;
}

// ─── ENC1 decryption ────────────────────────────────────────────────
static const uint8_t ENC1_TABLE[1024] = {
    0x38, 0x1e, 0xb7, 0x49, 0x69, 0x0c, 0x01, 0x88, 0xa5, 0xc8, 0x60, 0x00, 0x1a, 0x3b, 0x03, 0x91,
    0x4c, 0x29, 0xc7, 0xcb, 0xbb, 0x77, 0xd8, 0x14, 0xa8, 0x11, 0x9b, 0xe9, 0xed, 0x97, 0xff, 0xab,
    0xb9, 0xc5, 0xa4, 0x0c, 0x57, 0x32, 0xa0, 0xce, 0x90, 0x35, 0x6c, 0x37, 0x0a, 0xe0, 0x66, 0xa6,
    0x8d, 0xce, 0x1a, 0xf3, 0xec, 0x9d, 0x2e, 0xea, 0xdc, 0x5d, 0x9e, 0x4a, 0x85, 0xa3, 0x17, 0x08,
    0xc3, 0x56, 0x24, 0x18, 0xe8, 0xbe, 0x27, 0x94, 0x4d, 0xec, 0x71, 0x6e, 0x6f, 0xc8, 0x52, 0x12,
    0x76, 0x17, 0xa2, 0xf2, 0x8c, 0x4a, 0x39, 0xcd, 0x50, 0x7e, 0x45, 0x3a, 0xd4, 0x71, 0x2a, 0x97,
    0x89, 0x9b, 0x25, 0xfd, 0x30, 0x88, 0xab, 0xe8, 0x47, 0xae, 0x63, 0x0d, 0x8c, 0xf5, 0x79, 0x6d,
    0xfb, 0x78, 0x56, 0x52, 0x12, 0xc1, 0xb0, 0xef, 0x72, 0x07, 0xcf, 0xc0, 0xcf, 0x4f, 0xbf, 0xaa,
    0x98, 0x8b, 0xd5, 0x9e, 0x40, 0xe8, 0xaf, 0x36, 0x12, 0x10, 0x4c, 0xba, 0x43, 0x72, 0x7b, 0xda,
    0xf3, 0x2d, 0x2d, 0x2d, 0x3c, 0x7d, 0xa4, 0x71, 0xcd, 0x59, 0xdb, 0x87, 0x08, 0x19, 0x95, 0x98,
    0x5c, 0x5e, 0x5e, 0xaf, 0x90, 0xc7, 0x06, 0xcc, 0x2e, 0x7c, 0x6d, 0xf0, 0x47, 0x68, 0x36, 0xb2,
    0x2d, 0x25, 0x2a, 0x53, 0xfe, 0x53, 0x5c, 0x39, 0xd0, 0x5d, 0xbd, 0xa9, 0xfa, 0x30, 0x86, 0x9d,
    0xf2, 0xe1, 0xb5, 0xdf, 0x92, 0x60, 0xec, 0x24, 0x1c, 0x62, 0xab, 0x27, 0x0e, 0x02, 0x0d, 0x4d,
    0xd7, 0x8b, 0x80, 0x82, 0x04, 0xd1, 0x99, 0x1c, 0xcb, 0xd9, 0xbd, 0xc7, 0x28, 0x21, 0x2b, 0xc5,
    0x50, 0xde, 0xbd, 0x5a, 0xc5, 0xe2, 0x34, 0x3a, 0xbe, 0xff, 0xbb, 0xf2, 0x39, 0x22, 0x7c, 0x6c,
    0xee, 0x83, 0x1a, 0xbf, 0xe9, 0x34, 0x81, 0x54, 0xd9, 0xd2, 0x41, 0x9d, 0x31, 0x68, 0xf9, 0x1e,
    0xa5, 0x67, 0xa5, 0x82, 0xf5, 0x56, 0x9c, 0xda, 0xe4, 0x6f, 0xad, 0xa9, 0xf0, 0xc9, 0x71, 0x5c,
    0x69, 0xce, 0x73, 0x23, 0x44, 0x9d, 0x64, 0xa7, 0x9c, 0x43, 0x20, 0x40, 0x73, 0x49, 0x0a, 0x34,
    0x0a, 0x24, 0x48, 0xeb, 0x7c, 0x5d, 0x68, 0xf4, 0xfe, 0x29, 0xbc, 0x96, 0xf0, 0xe5, 0xc4, 0xc1,
    0x44, 0x87, 0xff, 0x0d, 0x75, 0xa3, 0xa7, 0xc8, 0xd5, 0x6b, 0xd5, 0xd8, 0x16, 0x6b, 0xa6, 0xff,
    0x6a, 0x6a, 0xb5, 0xf7, 0x38, 0x4e, 0xba, 0xa4, 0x3d, 0x41, 0x75, 0x5a, 0x54, 0xc6, 0x7d, 0x67,
    0x09, 0x7d, 0xf7, 0xd7, 0x17, 0xc4, 0x94, 0x84, 0xe3, 0x0b, 0x0f, 0x01, 0x20, 0xab, 0xdb, 0x4d,
    0x4f, 0x26, 0x93, 0xac, 0x15, 0x50, 0xf5, 0xc1, 0x95, 0x93, 0x01, 0xe7, 0x65, 0x83, 0xb6, 0xa0,
    0xb1, 0x04, 0x64, 0xa8, 0xf4, 0x86, 0xa8, 0xc6, 0x21, 0xe1, 0x77, 0x4a, 0xa1, 0xf0, 0xdd, 0x6e,
    0x63, 0x49, 0x07, 0x69, 0x83, 0xc6, 0xb0, 0xe0, 0x0d, 0xe4, 0x90, 0x9a, 0xa6, 0x8f, 0xd4, 0xb8,
    0x04, 0x5b, 0x74, 0x48, 0xf9, 0xb6, 0x3f, 0x5e, 0xd6, 0x08, 0xfc, 0x1f, 0x3b, 0x2e, 0x2f, 0x05,
    0xe6, 0xcf, 0xec, 0xae, 0xc2, 0xae, 0x66, 0xf3, 0x23, 0x06, 0xb2, 0xdb, 0x58, 0xaf, 0x38, 0x12,
    0x98, 0x95, 0x10, 0x6c, 0xd6, 0xe7, 0x66, 0x7f, 0xe9, 0xc3, 0x51, 0x9f, 0xac, 0x7b, 0xae, 0x15,
    0x65, 0x76, 0x11, 0x33, 0xf6, 0xfc, 0x93, 0x1f, 0x1e, 0x5c, 0xd2, 0xe1, 0xe7, 0x1b, 0x05, 0x9b,
    0x3c, 0xde, 0x04, 0xac, 0x8c, 0x03, 0xf3, 0xfd, 0x2c, 0xfa, 0x51, 0x80, 0x57, 0x53, 0x43, 0xb8,
    0xa3, 0x4a, 0x7e, 0x39, 0x61, 0xaf, 0x67, 0xbc, 0x92, 0x74, 0x9f, 0xa6, 0x59, 0xdf, 0x05, 0xde,
    0x7b, 0x24, 0xa4, 0x19, 0x89, 0xaa, 0xdc, 0x99, 0x14, 0x23, 0x03, 0x4c, 0x48, 0x45, 0x32, 0x3f,
    0x81, 0x64, 0x6d, 0x03, 0x22, 0x85, 0x09, 0xc0, 0x3d, 0x6c, 0x37, 0x62, 0xd8, 0xcf, 0xe6, 0xa8,
    0xb8, 0x50, 0xc5, 0x35, 0xfa, 0x81, 0xca, 0x52, 0xe1, 0x77, 0x3e, 0xa7, 0xa7, 0x28, 0xbc, 0x5a,
    0x31, 0x2f, 0x1d, 0xba, 0xb7, 0xfd, 0x8f, 0xca, 0x92, 0x48, 0x3f, 0xb9, 0x89, 0xe3, 0x41, 0x92,
    0x37, 0xaa, 0x09, 0x06, 0x46, 0x7e, 0x40, 0xf7, 0x0f, 0xe5, 0x2b, 0x73, 0x6d, 0xca, 0x82, 0xf1,
    0x53, 0x74, 0xc9, 0x58, 0x1c, 0x8a, 0xf4, 0x77, 0x76, 0xd3, 0xaa, 0x1b, 0x2a, 0x70, 0x3e, 0x9a,
    0x96, 0x44, 0x78, 0xea, 0xb3, 0x34, 0x8d, 0x27, 0x42, 0xb7, 0x85, 0x7a, 0x28, 0xe8, 0xe4, 0xb8,
    0xad, 0x16, 0x3d, 0x5f, 0xcc, 0x14, 0xf9, 0x91, 0x1c, 0x13, 0x49, 0xb3, 0xc2, 0x08, 0xdd, 0xa3,
    0x7f, 0xac, 0x0e, 0xcc, 0xdc, 0x07, 0x10, 0xf8, 0x2f, 0xea, 0x83, 0xa5, 0xb3, 0xee, 0x74, 0x3b,
    0x6f, 0x13, 0x2b, 0xd0, 0xf1, 0xb3, 0x6a, 0x35, 0xc0, 0xcb, 0xed, 0x46, 0x57, 0xdd, 0x20, 0x6b,
    0x9f, 0xda, 0x79, 0x43, 0x38, 0x27, 0x3d, 0x32, 0x59, 0x18, 0xb0, 0x98, 0xbc, 0xef, 0x13, 0x5f,
    0x2c, 0x2c, 0x41, 0x1b, 0x5f, 0xed, 0x11, 0xed, 0xc9, 0x75, 0x5d, 0x7a, 0x37, 0x73, 0x1a, 0xb6,
    0xc8, 0xc2, 0xd4, 0x72, 0x8f, 0xf1, 0xa2, 0x88, 0x63, 0x72, 0x45, 0xe2, 0x26, 0x19, 0xfa, 0x5b,
    0x0c, 0x54, 0x02, 0x02, 0x57, 0x62, 0xc4, 0xbf, 0xd6, 0xee, 0x82, 0x05, 0xa1, 0xa2, 0x8e, 0xef,
    0x25, 0x76, 0xe0, 0x15, 0x7d, 0xe7, 0x58, 0x10, 0xeb, 0xfe, 0x3f, 0x87, 0x5f, 0xb4, 0x47, 0xee,
    0x00, 0x29, 0x3c, 0xb9, 0xd9, 0x66, 0x01, 0x30, 0x8e, 0xcc, 0x84, 0xa9, 0x4e, 0xca, 0xbe, 0x13,
    0xd1, 0xa9, 0x4d, 0xdd, 0xf8, 0x79, 0x94, 0xde, 0xd3, 0xf4, 0xf8, 0x59, 0x28, 0xd3, 0xd2, 0x80,
    0xba, 0x11, 0x60, 0xd7, 0x80, 0x99, 0x87, 0x0f, 0x23, 0x97, 0xd7, 0x0b, 0x29, 0x84, 0xa1, 0xb1,
    0xb0, 0x2c, 0x7b, 0xea, 0xc1, 0x8b, 0xa2, 0x2b, 0x78, 0x42, 0xbd, 0x26, 0x55, 0x70, 0xda, 0xf8,
    0x0b, 0x6a, 0x68, 0x61, 0xe5, 0xd1, 0x40, 0xa1, 0x42, 0x36, 0xbf, 0xcd, 0x3e, 0x60, 0x52, 0x18,
    0x02, 0x42, 0xad, 0x0c, 0x4e, 0x9a, 0xb9, 0x9e, 0xc3, 0x21, 0xcb, 0x51, 0x64, 0x1e, 0x45, 0x54,
    0xeb, 0x94, 0xf6, 0x5a, 0x16, 0x06, 0x44, 0xd4, 0xf1, 0x62, 0xfc, 0x00, 0x65, 0x20, 0xc3, 0x0e,
    0x3e, 0xe2, 0xef, 0xb1, 0x6b, 0x90, 0x85, 0x07, 0x70, 0x1b, 0xd5, 0x4b, 0xbe, 0x95, 0x25, 0x75,
    0x26, 0x18, 0x0b, 0xe2, 0x8f, 0x4b, 0x7f, 0xf6, 0x22, 0xad, 0x3a, 0x96, 0x81, 0x15, 0x99, 0x0f,
    0xcd, 0x47, 0x55, 0xeb, 0x09, 0xa0, 0x0a, 0xfe, 0xd9, 0x4f, 0x2a, 0x93, 0xdf, 0xdb, 0x7c, 0x31,
    0xe3, 0xc7, 0xfb, 0x8d, 0x79, 0x1d, 0x35, 0x8b, 0x61, 0xd6, 0x46, 0x4f, 0x5b, 0xc9, 0x86, 0xc6,
    0x2e, 0x86, 0x8d, 0xe0, 0x58, 0x30, 0xf7, 0x63, 0xc4, 0xb2, 0x1d, 0xe4, 0x51, 0xe6, 0x33, 0xb4,
    0x6f, 0xd1, 0x9e, 0x69, 0x8e, 0x7f, 0xbb, 0x9f, 0xd2, 0x8c, 0x6e, 0x91, 0x9c, 0xb4, 0xe6, 0x7e,
    0x9b, 0x00, 0x2f, 0x4b, 0xd3, 0x55, 0xb4, 0x46, 0x61, 0x4b, 0x4e, 0xf6, 0xfb, 0x17, 0x1f, 0x8a,
    0x4c, 0xb5, 0x1d, 0x19, 0x14, 0xfc, 0xc2, 0xb5, 0xd0, 0x8e, 0x67, 0x0e, 0x5b, 0x36, 0x22, 0xdf,
    0x3c, 0xf2, 0x7a, 0x56, 0x32, 0x6e, 0x70, 0xfb, 0x3b, 0x16, 0xe3, 0xd8, 0xbb, 0x89, 0x31, 0x21,
    0xfd, 0x9a, 0xb7, 0x8a, 0x97, 0x78, 0x33, 0xd0, 0xb2, 0x33, 0xb6, 0xf5, 0x91, 0x9c, 0x7a, 0xce,
    0xa0, 0x96, 0xb1, 0xdc, 0x8a, 0x3a, 0x1f, 0xf9, 0xc0, 0xe5, 0x65, 0x88, 0x55, 0x84, 0x5e, 0xe9,
};

static uint8_t bit_reverse(uint8_t b) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) {
        if (b & (1 << i))
            r |= (1 << (7 - i));
    }
    return r;
}

uint8_t* Resource_DecryptENC1(const uint8_t* data, uint32_t dataSize, uint32_t* outSize) {
    if (dataSize < 0x90 || memcmp(data, "ENC1", 4) != 0)
        return NULL;

    uint32_t payload_size;
    uint32_t extra_skip;
    memcpy(&payload_size, data + 0x84, 4);
    memcpy(&extra_skip, data + 0x88, 4);

    uint32_t seed_off = 0x8C + extra_skip;
    if (seed_off + 4 + payload_size > dataSize)
        return NULL;

    uint32_t seed;
    memcpy(&seed, data + seed_off, 4);

    const uint8_t* payload = data + seed_off + 4;

    uint8_t* out = (uint8_t*)malloc(payload_size);
    if (!out) return NULL;

    for (uint32_t i = 0; i < payload_size; i++) {
        uint8_t step1 = bit_reverse(payload[i]);
        uint32_t idx = (i + seed) & 0x3FF;
        out[i] = ENC1_TABLE[idx] ^ step1;
    }

    *outSize = payload_size;
    return out;
}

int Resource_LoadPNZ(const char* path) {
    /* Os arquivos no disco são T<id>.PNZ em maiúsculas e o caller monta o
     * caminho com ".pnz"; num filesystem case-sensitive o fopen direto falha.
     * Mesma solução já usada pelo RES_Open. */
    FILE* f =
#if !defined(_WIN32)
        res_fopen_case_insensitive(path);
#else
        fopen(path, "rb");
#endif
    if (!f) {
        Log_Print("PNZ: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* buf = (uint8_t*)malloc(fileSize);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, fileSize, f);
    fclose(f);

    uint32_t decSize = 0;
    uint8_t* dec = NULL;
    if (fileSize >= 0x8C && memcmp(buf, "ENC2", 4) == 0) {
        dec = Resource_DecryptENC2(buf, (uint32_t)fileSize, &decSize);
    } else {
        dec = Resource_DecryptENC1(buf, (uint32_t)fileSize, &decSize);
    }
    free(buf);

    if (!dec) {
        Log_Print("PNZ: decryption failed for '%s'\n", path);
        return -1;
    }

    /* O decodificador escolhe pelo nome: PNG no Prex3, TGA/BMP/JPG no Exceed */
    const char* tmpName = "pnz_title.png";
    if (decSize >= 2 && dec[0] == 'B' && dec[1] == 'M') tmpName = "pnz_title.bmp";
    else if (decSize >= 3 && dec[0] == 0xFF && dec[1] == 0xD8) tmpName = "pnz_title.jpg";
    else if (decSize >= 4 && memcmp(dec, "\x89PNG", 4) != 0) tmpName = "pnz_title.tga";
    Log_Print("PNZ: %u bytes decodificados, magic %02X %02X %02X %02X -> %s\n",
              decSize, dec[0], dec[1], dec[2], dec[3], tmpName);
    int texId = Texture_LoadFromMemory(dec, decSize, tmpName);
    free(dec);

    if (texId < 0) {
        Log_Print("PNZ: failed to load as texture: '%s'\n", path);
    }
    return texId;
}

int Resource_LoadTextureFromDAT(const char* datPath, const char* resName)
{
    if (!RES_Open(datPath)) {
        Log_Print("RES: failed to open '%s' for texture '%s'\n", datPath, resName);
        return -1;
    }
    int texId = loadTextureFromRES(resName);
    RES_Close();
    return texId;
}

void Resource_ClearBGA(void)
{
    BGA_Shutdown();
    if (g_game.isVSL) {
        VSL_Shutdown();
        g_game.isVSL = false;
    }
    Texture_Shutdown();
    Font_Shutdown();
    g_cdLoaded = false;
    /* BGA_Shutdown zera sprTileCount mas não reseta os índices de fonte/arrow.
     * Resetar aqui para evitar que indices stale sejam usados após o clear. */
    g_fontArrow541 = -1;
    g_fontArrow542 = -1;
    g_fontArrow543 = -1;
    g_fontArrow544 = -1;
    g_fontArrow545 = -1;
    g_fontArrowETC = -1;
    g_fontArrowF   = -1;
    g_fontSpark    = -1;
}

int Resource_SwitchBGA(const char* datName)
{
    Resource_ClearBGA();
    if (!Resource_LoadBGAByName(datName))
    {
        Log_Print("Resource: failed to load BGA '%s'\n", datName);
        return -1;
    }
    BGA_Reset();
    return g_game.bgaPicCount - 1;
}

int Resource_LoadSPR(const char* datPath, const char* sprName) {
    if (!RES_Open(datPath)) return 0;
    int count = loadSPRFromRES(sprName, NULL, NULL, NULL);
    RES_Close();
    return count;
}

// ─── SPR_LoadSPR / SPR_LoadSP2 (igual Ghidra) ──────────────────────────────
// .spr: u2/v2 são coordenadas absolutas
int SPR_LoadSPR(const char* sprName, int* outPatCols, int* outPatRows, int* outPatFlags) {
    char sprFileName[64];
    const char* ext = strrchr(sprName, '.');
    if (ext && _stricmp(ext, ".spr") == 0) {
        strncpy(sprFileName, sprName, sizeof(sprFileName) - 1);
    } else {
        snprintf(sprFileName, sizeof(sprFileName), "%s.spr", sprName);
    }

    int sprIdx = RES_Find(sprFileName);
    if (sprIdx < 0) {
        char altName[64];
        snprintf(altName, sizeof(altName), "%s.sp2", sprName);
        // Also try without extension
        const char* dot = strrchr(sprName, '.');
        int baseLen = dot ? (int)(dot - sprName) : (int)strlen(sprName);
        if (baseLen > 63) baseLen = 63;
        memcpy(altName, sprName, baseLen);
        altName[baseLen] = '\0';
        sprIdx = RES_Find(altName);
    }
    if (sprIdx < 0) return 0;

    uint32_t sprSize = RES_GetSize(sprIdx);
    uint8_t* sprData = RES_ReadAlloc(sprIdx);
    if (!sprData) return 0;

    SPRFile spr;
    if (!parseSPR(sprData, sprSize, &spr)) {
        free(sprData);
        return 0;
    }
    free(sprData);

    int startIdx = g_game.sprTileCount;
    for (int i = 0; i < spr.tileCount && g_game.sprTileCount < MAX_SPR_TILES; i++) {
        SPRTileDef* t = &g_game.sprTiles[g_game.sprTileCount];
        snprintf(t->name, sizeof(t->name), "%s_%d", sprFileName, i);
        for (int ci = 0; t->name[ci]; ci++) {
            if (t->name[ci] >= 'A' && t->name[ci] <= 'Z')
                t->name[ci] = (char)(t->name[ci] + 32);
        }
        strncpy(t->texture, spr.tiles[i].texture, sizeof(t->texture) - 1);
        t->srcX = spr.tiles[i].srcX;
        t->srcY = spr.tiles[i].srcY;
        t->srcW = spr.tiles[i].srcW;
        t->srcH = spr.tiles[i].srcH;
        t->flipH = spr.tiles[i].flipH;
        t->flipV = spr.tiles[i].flipV;
        t->texId = -1;

        char texName[64];
        strncpy(texName, spr.tiles[i].texture, sizeof(texName) - 1);
        for (int ci = 0; texName[ci]; ci++) {
            if (texName[ci] >= 'A' && texName[ci] <= 'Z')
                texName[ci] = (char)(texName[ci] + 32);
        }
        t->texId = loadTextureFromRES(texName);

        if (t->texId >= 0 && g_game.textures[t->texId].inUse) {
            float texW = (float)g_game.textures[t->texId].width;
            float texH = (float)g_game.textures[t->texId].height;
            if (texW <= 0) texW = 256.0f;
            if (texH <= 0) texH = 256.0f;
            // .spr: u2/v2 são absolutos. flipH vem naturalmente (u1 > u2 = espelhado).
            // flipV precisa trocar v1/v2 pra imagem nao ficar de cabeca pra baixo.
            t->u1 = (float)spr.tiles[i].u1 / texW;
            t->u2 = (float)spr.tiles[i].u2 / texW;
            t->v1 = (float)spr.tiles[i].v1 / texH;
            t->v2 = (float)spr.tiles[i].v2 / texH;
        } else {
            t->u1 = 0.0f; t->v1 = 0.0f;
            t->u2 = 1.0f; t->v2 = 1.0f;
        }
        g_game.sprTileCount++;
    }

    if (spr.patCols > 0 && spr.patRows > 0) {
        if (outPatCols) *outPatCols = spr.patCols;
        if (outPatRows) *outPatRows = spr.patRows;
        if (outPatFlags) *outPatFlags = spr.patFlags;
        return spr.tileCount;
    }
    return spr.isAni ? spr.tileCount : 0;
}

// .sp2: u2/v2 são OFFSETS (largura/altura) a partir de u1/v1, negativo = flip
int SPR_LoadSP2(const char* sprName, int* outPatCols, int* outPatRows, int* outPatFlags) {
    char sprFileName[64];
    const char* ext = strrchr(sprName, '.');
    if (ext && _stricmp(ext, ".sp2") == 0) {
        strncpy(sprFileName, sprName, sizeof(sprFileName) - 1);
    } else {
        snprintf(sprFileName, sizeof(sprFileName), "%s.sp2", sprName);
    }

    int sprIdx = RES_Find(sprFileName);
    if (sprIdx < 0) return 0;

    uint32_t sprSize = RES_GetSize(sprIdx);
    uint8_t* sprData = RES_ReadAlloc(sprIdx);
    if (!sprData) return 0;

    SPRFile spr;
    if (!parseSPR(sprData, sprSize, &spr)) {
        free(sprData);
        return 0;
    }
    free(sprData);

    int startIdx = g_game.sprTileCount;
    for (int i = 0; i < spr.tileCount && g_game.sprTileCount < MAX_SPR_TILES; i++) {
        SPRTileDef* t = &g_game.sprTiles[g_game.sprTileCount];
        snprintf(t->name, sizeof(t->name), "%s_%d", sprFileName, i);
        for (int ci = 0; t->name[ci]; ci++) {
            if (t->name[ci] >= 'A' && t->name[ci] <= 'Z')
                t->name[ci] = (char)(t->name[ci] + 32);
        }
        strncpy(t->texture, spr.tiles[i].texture, sizeof(t->texture) - 1);
        t->srcX = spr.tiles[i].srcX;
        t->srcY = spr.tiles[i].srcY;
        t->srcW = spr.tiles[i].srcW;
        t->srcH = spr.tiles[i].srcH;
        t->flipH = spr.tiles[i].flipH;
        t->flipV = spr.tiles[i].flipV;
        t->texId = -1;

        char texName[64];
        strncpy(texName, spr.tiles[i].texture, sizeof(texName) - 1);
        for (int ci = 0; texName[ci]; ci++) {
            if (texName[ci] >= 'A' && texName[ci] <= 'Z')
                texName[ci] = (char)(texName[ci] + 32);
        }
        t->texId = loadTextureFromRES(texName);

        if (t->texId >= 0 && g_game.textures[t->texId].inUse) {
            float texW = (float)g_game.textures[t->texId].width;
            float texH = (float)g_game.textures[t->texId].height;
            if (texW <= 0) texW = 256.0f;
            if (texH <= 0) texH = 256.0f;
            // .sp2: u2/v2 são OFFSETS de u1/v1 (negativo = flip)
            float uStart = (float)spr.tiles[i].u1;
            float uEnd = uStart + (float)spr.tiles[i].u2;
            float vStart = (float)spr.tiles[i].v1;
            float vEnd = vStart + (float)spr.tiles[i].v2;
            t->u1 = uStart / texW;
            t->u2 = uEnd / texW;
            t->v1 = vStart / texH;
            t->v2 = vEnd / texH;
        } else {
            t->u1 = 0.0f; t->v1 = 0.0f;
            t->u2 = 1.0f; t->v2 = 1.0f;
        }
        g_game.sprTileCount++;
    }

    if (spr.patCols > 0 && spr.patRows > 0) {
        if (outPatCols) *outPatCols = spr.patCols;
        if (outPatRows) *outPatRows = spr.patRows;
        if (outPatFlags) *outPatFlags = spr.patFlags;
        return spr.tileCount;
    }
    return spr.isAni ? spr.tileCount : 0;
}

// g_fontTexId is defined in font.c
int g_fontDec00Id = -1;
int g_fontSpr01 = -1;
int g_fontSpr02 = -1;
int g_fontSpr03 = -1;
int g_fontSpr04 = -1;
int g_fontSpr05 = -1;
int g_fontSprW01 = -1;
int g_fontSprW02 = -1;
int g_fontSprW03 = -1;
int g_fontSprW04 = -1;
int g_fontSprW05 = -1;
int g_fontSprM01 = -1;
int g_fontSprM02 = -1;
int g_fontSprM03 = -1;
int g_fontSprM04 = -1;
int g_fontSprM05 = -1;
int g_fontSprHD01 = -1;
int g_fontSprHD02 = -1;
int g_fontSprHD03 = -1;
int g_fontSprHD05 = -1;
int g_fontSprBT01 = -1;
int g_fontSprBT02 = -1;
int g_fontSprGGS = -1;
int g_fontSprGGD = -1;
int g_fontArrow541 = -1;
int g_fontArrow542 = -1;
int g_fontArrow543 = -1;
int g_fontArrow544 = -1;
int g_fontArrow545 = -1;
int g_fontArrowETC = -1;
int g_fontArrowF = -1;
int g_fontSpark = -1;   /* Exceed2: spark.spr (TYPE ani, stepfx0..4), [obj+0x34194] */

void Resource_LoadFontAndArrows(const char* datPath) {
    if (!RES_Open(datPath)) return;

    int startCount = g_game.sprTileCount;

    g_fontTexId = loadTextureFromRES("font.tga");
    g_fontDec00Id = loadTextureFromRES("dec00.tga");
    Log_Print("DEBUG: g_fontDec00Id after load: %d\n", g_fontDec00Id);

    g_fontArrow541 = g_game.sprTileCount;
    SPR_LoadSP2("arrow541.sp2", NULL, NULL, NULL);
    Log_Print("DEBUG: Before ARROW542 load: sprTileCount=%d\n", g_game.sprTileCount);
    g_fontArrow542 = g_game.sprTileCount;
    SPR_LoadSP2("arrow542.sp2", NULL, NULL, NULL);
    Log_Print("DEBUG: After ARROW542 load: sprTileCount=%d, g_fontArrow542=%d\n", g_game.sprTileCount, g_fontArrow542);
    g_fontArrow543 = g_game.sprTileCount;
    SPR_LoadSP2("arrow543.sp2", NULL, NULL, NULL);
    g_fontArrow544 = g_game.sprTileCount;
    SPR_LoadSP2("arrow544.sp2", NULL, NULL, NULL);
    g_fontArrow545 = g_game.sprTileCount;
    SPR_LoadSP2("arrow545.sp2", NULL, NULL, NULL);
    g_fontArrowETC = g_game.sprTileCount;
    SPR_LoadSP2("arrowETC.sp2", NULL, NULL, NULL);
    g_fontArrowF = g_game.sprTileCount;
    SPR_LoadSPR("arrowf.spr", NULL, NULL, NULL);
    /* Exceed2 0x405A16: spark.spr */
    g_fontSpark = g_game.sprTileCount;
    SPR_LoadSPR("spark.spr", NULL, NULL, NULL);
    if (g_game.sprTileCount == g_fontSpark) g_fontSpark = -1;

    g_fontSpr01 = g_game.sprTileCount;
    SPR_LoadSPR("01.spr", NULL, NULL, NULL);
    g_fontSpr02 = g_game.sprTileCount;
    SPR_LoadSPR("02.spr", NULL, NULL, NULL);
    g_fontSprW01 = g_game.sprTileCount;
    SPR_LoadSPR("w01.spr", NULL, NULL, NULL);
    g_fontSprW02 = g_game.sprTileCount;
    SPR_LoadSPR("w02.spr", NULL, NULL, NULL);
    g_fontSpr03 = g_game.sprTileCount;
    SPR_LoadSPR("03.spr", NULL, NULL, NULL);
    g_fontSpr04 = g_game.sprTileCount;
    SPR_LoadSPR("04.spr", NULL, NULL, NULL);
    g_fontSprW03 = g_game.sprTileCount;
    SPR_LoadSPR("w03.spr", NULL, NULL, NULL);
    g_fontSprW04 = g_game.sprTileCount;
    SPR_LoadSPR("w04.spr", NULL, NULL, NULL);
    g_fontSpr05 = g_game.sprTileCount;
    SPR_LoadSPR("05.spr", NULL, NULL, NULL);
    g_fontSprW05 = g_game.sprTileCount;
    SPR_LoadSPR("w05.spr", NULL, NULL, NULL);
    g_fontSprM01 = g_game.sprTileCount;
    SPR_LoadSPR("m01.spr", NULL, NULL, NULL);
    g_fontSprM02 = g_game.sprTileCount;
    SPR_LoadSPR("m02.spr", NULL, NULL, NULL);
    g_fontSprM03 = g_game.sprTileCount;
    SPR_LoadSPR("m03.spr", NULL, NULL, NULL);
    g_fontSprM04 = g_game.sprTileCount;
    SPR_LoadSPR("m04.spr", NULL, NULL, NULL);
    g_fontSprM05 = g_game.sprTileCount;
    SPR_LoadSPR("m05.spr", NULL, NULL, NULL);
    g_fontSprHD01 = g_game.sprTileCount;
    SPR_LoadSPR("HD01.SPR", NULL, NULL, NULL);
    g_fontSprHD02 = g_game.sprTileCount;
    SPR_LoadSPR("HD02.SPR", NULL, NULL, NULL);
    g_fontSprHD03 = g_game.sprTileCount;
    SPR_LoadSPR("HD03.SPR", NULL, NULL, NULL);
    g_fontSprHD05 = g_game.sprTileCount;
    SPR_LoadSPR("HD05.SPR", NULL, NULL, NULL);
    g_fontSprBT01 = g_game.sprTileCount;
    SPR_LoadSPR("BT_MC01.SPR", NULL, NULL, NULL);
    g_fontSprBT02 = g_game.sprTileCount;
    SPR_LoadSPR("BT_MC02.SPR", NULL, NULL, NULL);
    /* Exceed (exceed.exe 0x404651/0x404661): lifebar em gg_s.spr e gg_d.spr.
     * Não existem no 00.DAT do Prex3: sem tiles, o índice fica -1. */
    g_fontSprGGS = g_game.sprTileCount;
    SPR_LoadSPR("gg_s.spr", NULL, NULL, NULL);
    if (g_game.sprTileCount == g_fontSprGGS) g_fontSprGGS = -1;
    g_fontSprGGD = g_game.sprTileCount;
    SPR_LoadSPR("gg_d.spr", NULL, NULL, NULL);
    if (g_game.sprTileCount == g_fontSprGGD) g_fontSprGGD = -1;

    /* Não existe inversão de V a fazer aqui. Os .spr/.sp2 nomeiam texturas
     * .tga, mas nenhum .DAT do jogo traz TGA: o RES só tem PNG, e o fallback
     * de extensão do loadTextureFromRES resolve arrow01.tga -> ARROW01.PNG,
     * font.tga -> FONT.PNG etc. O decoder de PNG do texture.c escreve as
     * linhas na ordem do arquivo (a única inversão do módulo está dentro do
     * Texture_LoadTGA), então V=0 é o topo da imagem — a mesma convenção que
     * o Texture_DrawUV assume ao mandar v1 para o vértice de cima. Trocar
     * v1/v2 aqui virava todo tile de cabeça para baixo; era o que deixava os
     * ícones de command (arrow541.sp2) invertidos no SongSelect.
     * Tiles com flipV continuam com v1 > v2 vindo do parser, que é o que
     * realmente pede o espelhamento. */
    (void)startCount;

    RES_Close();
}

/* Exceed2: os .STX ficam dentro de STEP.DAT (PIU32.EXE 0x40d0d7).
 * Extrai uma entrada de um RESPACK/RESPAC2 sem mexer no arquivo aberto.
 * O último pacote fica em cache (a Select consulta todas as músicas). */
static RESArchive* g_packCache = NULL;

uint8_t* Resource_ExtractFromPack(const char* datPath, const char* name, uint32_t* outSize)
{
    RESArchive* saved = g_resArchive;
    g_resArchive = NULL;
    if (g_packCache && _stricmp(g_packCache->path, datPath) == 0) {
        g_resArchive = g_packCache;
    } else {
        if (g_packCache) { g_resArchive = g_packCache; RES_Close(); g_packCache = NULL; }
        if (RES_Open(datPath)) g_packCache = g_resArchive;
    }
    uint8_t* out = NULL;
    if (g_resArchive) {
        int idx = RES_Find(name);
        if (idx >= 0) {
            *outSize = RES_GetSize(idx);
            out = RES_ReadAlloc(idx);
        }
    }
    g_resArchive = saved;
    return out;
}
