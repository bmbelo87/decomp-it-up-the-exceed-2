#include "pumpy.h"
#include "bga.h"
#include "movie.h"

static int g_resultFrame;
/* Este static escondia o g_fontTexId global (font.c:103, declarado extern em
 * pumpy.h:376). Funcionava só por efeito colateral: Font_LoadTexture() além de
 * devolver o id também grava no global, então as duas cópias acabavam iguais.
 * Bastava alguém zerar o global — como Font_Shutdown() faz em todo clear de
 * BGA — para as duas divergirem e este arquivo passar a usar uma textura já
 * destruída. Removido; agora usa o global diretamente.
 * static int g_fontTexId = -1; */
static int g_gradeP1 = 5; // 0=S..5=F
static int g_gradeP2 = 5;
static int g_lastDigitSoundCount = 0;
static int g_lastSoundFrame = 0;
static bool g_gradeSoundPlayed = false;

// Y original Ghidra -> Y-DOWN (topo do digito)
// 480 - yUp - 39 = valor
static const int g_statY[7] = {
    131, 177, 223, 269, 315, 361, 407
};
static const int g_statDelay[7] = { 60, 70, 80, 90, 100, 110, 120 };
static const int g_statDigits[7] = { 3, 3, 3, 3, 3, 3, 7 };
#define SPIN 10  // frames por digito girando (0->1->...->9->final)

static int getDig(int v, int rp) {
    int p = 1;
    for (int j = 0; j < rp; j++) p *= 10;
    return (v / p) % 10;
}

static void drawDig(int x, int y, int d) {
    if (g_fontTexId < 0 || d < 0 || d > 9) return;
    int col = d % 8, row = d / 8;
    Texture_Bind(g_fontTexId);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1,1,1,1);
    float yUp = 480.0f - (float)y - 39.0f;
    float u0 = (float)col * 0.125f;
    float u1 = u0 + 0.125f;
    /* V=0 é o topo da FONT.PNG, então o V vai direto: vTop (menor) no vértice
     * de cima. Mesmo ajuste feito no drawTimeDigit do song_select.c. */
    float vTop = (float)row * 0.12109375f + 0.28515625f; // topo do glifo
    float vBot = vTop + 0.12109375f;                     // base do glifo
    glBegin(GL_QUADS);
    glTexCoord2f(u0, vBot); glVertex2f((float)x, yUp);
    glTexCoord2f(u1, vBot); glVertex2f((float)x+36, yUp);
    glTexCoord2f(u1, vTop); glVertex2f((float)x+36, yUp+39);
    glTexCoord2f(u0, vTop); glVertex2f((float)x, yUp+39);
    glEnd();
}

// P1: left-aligned, MSB primeiro
static void drawNumP1(int x, int y, int v, int nd, int elap, int offX) {
    x += offX;
    for (int i = 0; i < nd; i++) {
        int le = elap - i * SPIN;
        if (le < 0) break;
        int d = (le < SPIN) ? (le % 10) : getDig(v, nd-1-i);
        drawDig(x + i*22, y, d);
    }
}

// P2: right-aligned, LSB primeiro
static void drawNumP2(int rx, int y, int v, int nd, int elap, int offX) {
    rx += offX;
    for (int i = 0; i < nd; i++) {
        int le = elap - i * SPIN;
        if (le < 0) break;
        int d = (le < SPIN) ? (le % 10) : getDig(v, i);
        drawDig(rx - i*22, y, d);
    }
}

/* Fórmula e escada extraídas do PUMPY.EXE.
 *
 * A razão é calculada no fim de Gameplay_ProcessJudgment (0x0041042c) e
 * gravada como float em [0x00da22d0] — a decompilação do Ghidra mostra um
 * cast para (int) por erro de tipagem, mas DanceGradeDisplay lê o endereço
 * com "FLD float ptr".
 *
 *   razao = (perfect + great*0.9 + good*0.6 - bad*0.5 - miss + maxCombo*0.03)
 *           / total
 *
 * O termo maxCombo*0.03 só entra fora do modo EVENT (g_nGameMode != 1).
 *
 * A escada de notas vem de DanceGradeDisplay (0x00415330..0x004153ac), onde
 * cada FCOMP compara a razão com 1.0, 0.9, 0.8, 0.7 e 0.6. A nota máxima
 * exige adicionalmente missCount == 0 (teste de [0x00da2314] em 0x00415343).
 */
static int calcGrade(int perfect, int great, int good, int bad, int miss, int maxCombo) {
    int total = perfect + great + good + bad + miss;
    float ratio;
    if (total == 0) return 5;

    ratio = (float)perfect
          + (float)great * 0.9f
          + (float)good  * 0.6f
          - (float)bad   * 0.5f
          - (float)miss;
    if (g_game.svcGameMode != 1)          /* fora do modo EVENT */
        ratio += (float)maxCombo * 0.03f;
    ratio /= (float)total;

    if (ratio >= 1.0f && miss == 0) return 0;  /* S */
    if (ratio >= 0.9f)              return 1;  /* A */
    if (ratio >= 0.8f)              return 2;  /* B */
    if (ratio >= 0.7f)              return 3;  /* C */
    if (ratio >= 0.6f)              return 4;  /* D */
    return 5;                                  /* F */
}

/* ── Exceed: nota por pontuação (CGrade Begin 0x40CDAC / 0x40CE5D) ───────
 *   razão = score / (1500*N - 3000 - 250*K)
 *   N = [+0x8]: +1 por linha julgada (0x408996 / 0x408E35)
 *   K = [+0xC]: +1 quando a linha fecha com todas as setas pisadas
 *       (0x408954 / 0x408E04) — tratado aqui como N - MISS (PROVÁVEL).
 *   Escada (0x40D95C): S >= 1.0 e MISS == 0, A >= 0.95, B >= 0.90,
 *   C >= 0.85, D >= 0.75, senão F.
 * A razão de cada estágio fica em [+0x174 + estágio*4] (0x40CD72). */
static float g_exStageRatio[2][4];

static float exRatio(int p) {
    unsigned miss = g_game.stats.missCount[p];
    unsigned n = g_game.stats.perfectCount[p] + g_game.stats.greatCount[p] +
                 g_game.stats.goodCount[p] + g_game.stats.badCount[p] + miss;
    unsigned k = n - miss;
    long mx = 1500L * (long)n - 3000L - 250L * (long)k;
    if (mx <= 0) return 0.0f;
    return (float)g_game.stats.score[p] / (float)mx;
}

static int exGradeOf(float r, unsigned miss) {
    if (r >= 1.0f && miss == 0) return 0;
    if (r >= 0.95f) return 1;
    if (r >= 0.90f) return 2;
    if (r >= 0.85f) return 3;
    if (r >= 0.75f) return 4;
    return 5;
}

/* Estágio no contador do original ([0x568FF8]): 0, 1, 2 e 3 = extra */
static int exStageIdx(void) {
    if (g_game.isBonusSong) return 3;
    int s = 2 - g_game.stageCount;
    return (s < 0) ? 0 : (s > 2 ? 2 : s);
}

/* 0x40D0E7..0x40D2A8 */
static GameState exNextState(void) {
    float r1 = (g_game.activePlayerMask & 1) ? exRatio(0) : 0.0f;
    float r2 = (g_game.activePlayerMask & 2) ? exRatio(1) : 0.0f;
    int st = exStageIdx();
    Log_Print("RESULT(EX): estágio %d razão P1 %.3f P2 %.3f\n", st, r1, r2);
    if (r1 < 0.75f && r2 < 0.75f) return STATE_GAMEOVER_ENTER;   /* 0x40D0F3 */
    if (st == 0 || st == 1) {                                    /* NEXTSTAGE 1 / 2 */
        g_game.bonusStage = true;
        return STATE_STAGE_TRANSITION;
    }
    if (st == 2) {                                               /* 0x40D181 */
        bool ok = false;
        for (int p = 0; p < 2 && !ok; p++) {
            if (!(g_game.activePlayerMask & (1 << p))) continue;
            ok = g_exStageRatio[p][0] >= 0.95f && g_exStageRatio[p][1] >= 0.95f &&
                 g_exStageRatio[p][2] >= 0.95f;
        }
        g_game.bonusStage = ok;
        return ok ? STATE_STAGE_TRANSITION : STATE_GAMEOVER_ENTER;
    }
    /* Depois do extra (0x40D21C): NAMEINPUT se algum jogador entra no
     * highscore (0x41213C != -1), senão IR. O próprio NameInput_Enter faz o
     * teste e segue para o IR quando ninguém entra (0x413FD8).
     * return STATE_GAMEOVER_ENTER; */
    return STATE_NAMEINPUT;
}

// Decide proximo estado baseado nas grades e contagem de stages
GameState Result_GetNextState(void) {
    if (g_exceedSongIds) return exNextState();
    int g1 = calcGrade(g_game.stats.perfectCount[0], g_game.stats.greatCount[0],
                       g_game.stats.goodCount[0], g_game.stats.badCount[0],
                       g_game.stats.missCount[0], (int)g_game.stats.maxCombo[0]);
    int g2 = calcGrade(g_game.stats.perfectCount[1], g_game.stats.greatCount[1],
                       g_game.stats.goodCount[1], g_game.stats.badCount[1],
                       g_game.stats.missCount[1], (int)g_game.stats.maxCombo[1]);

    /* Grade efetivo: P2 sozinho usa g2; caso contrário P1 decide progressão */
    int grade = (g_game.activePlayerMask == 0x2) ? g2 : g1;

    Log_Print("RESULT NEXT: grade=%d g1=%d g2=%d stageCount=%d bonusStage=%d isBonus=%d\n", grade, g1, g2, g_game.stageCount, g_game.bonusStage, g_game.isBonusSong);

    // F = game over
    if (grade == 5) {
        Log_Print("RESULT DECISION: grade=F -> GAMEOVER\n");
        return STATE_GAMEOVER_ENTER;
    }

    // Stage bonus: se S ou A mantem, caso contrario perde o bonus
    if (grade >= 2) // B, C, D
        g_game.bonusStage = false;

    // Bonus stage ja foi: game over direto
    if (g_game.isBonusSong)
        return STATE_GAMEOVER_ENTER;

    if (g_game.stageCount > 0)
        return STATE_STAGE_TRANSITION;

    if (g_game.bonusStage)
        return STATE_STAGE_TRANSITION;  // vai pro bonus

    return STATE_GAMEOVER_ENTER;
}

/* ── Exceed: CGrade (vtable 0x1282474) ──────────────────────────────────
 * Begin 0x40CC94: BGA\GRADE.DAT + AUDIO\GRADE.AUD.
 * Intro 0x40CF34: BGA inteiro (0x41F520) nos quadros 0..60.
 * Update 0x40CF6C: BGA inteiro no quadro (t % 360) + 60; P1 (bit 1 de
 * [0x568FF4], 0x40D860) e depois P2 (bit 2, 0x40D51C) desenham números e
 * letra; sons em t; com t > 360 decide o próximo estado. */
static int g_exT;

/* 0x40C55C(x, y, 32, 31, d): dígito do font.tga, grade de 8 colunas.
 * Coordenadas em Y-UP com y na base, quad de (w+4) x (h+8). */
static void exDigit(int x, int y, int d)
{
    if (g_fontTexId < 0 || !g_game.textures[g_fontTexId].inUse) return;
    float u0 = (float)(d % 8) * 0.125f;
    float u1 = u0 + 0.125f;
    float v0 = (float)(d / 8) * 0.12109375f + 0.28515625f;
    float v1 = v0 + 0.12109375f;
    int w = 32, h = 31;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_game.textures[g_fontTexId].id);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(u0, v0); glVertex2i(x, y + h + 8);
    glTexCoord2f(u0, v1); glVertex2i(x, y);
    glTexCoord2f(u1, v1); glVertex2i(x + w + 4, y);
    glTexCoord2f(u1, v0); glVertex2i(x + w + 4, y + h + 8);
    glEnd();
}

/* 0x40C83C (P1): o dígito mais à direita fica em x; revela da esquerda,
 * um a cada 10 quadros; o dígito da vez gira (elap - 10*(i+1) + 10) % 10. */
static void exNumP1(int x, int y, int v, int nd, int elap)
{
    int shown = 0;
    int px = x + 22 - 22 * nd;
    for (int k = nd - 1, i = 0; k >= 0; k--, i++, px += 22) {
        int p10 = 1;
        for (int j = 0; j < k; j++) p10 *= 10;
        int dig = (v / p10) % 10;
        if (shown > i) {
            exDigit(px, y, dig);
        } else if (shown == i) {
            if (elap >= 10 * (i + 1)) { exDigit(px, y, dig); shown++; }
            else exDigit(px, y, (elap - 10 * (i + 1) + 10) % 10);
        } else {
            break;
        }
    }
}

/* 0x40C710 (P2): começa em x e anda -22; revela da direita */
static void exNumP2(int x, int y, int v, int nd, int elap)
{
    int shown = 0;
    for (int k = 0; k < nd; k++, x -= 22, v /= 10) {
        if (k < shown) {
            exDigit(x, y, v % 10);
        } else if (k == shown) {
            if (elap >= 10 * (k + 1)) { exDigit(x, y, v % 10); shown++; }
            else exDigit(x, y, (elap - 10 * (k + 1) + 10) % 10);
        } else {
            break;
        }
    }
}

/* Letra: pares de slots do BGA2 por nota (0x40D95C / 0x40D621).
 * S 0x24/0x25 · A 0x1A/0x1B · B 0x1C/0x1D · C 0x1E/0x1F · D 0x20/0x21 · F 0x22/0x23 */
static void exLetter(int grade, int frame)
{
    static const int k_slot[6] = { 0x24, 0x1A, 0x1C, 0x1E, 0x20, 0x22 };
    if (grade < 0 || grade > 5) grade = 5;
    BGA_DrawSlot(0, frame, k_slot[grade]);
    BGA_DrawSlot(0, frame, k_slot[grade] + 1);
}

static void exPlayer(int p, int t)
{
    /* perfect, great, good, bad, miss, max combo (3 dígitos) e score (7) */
    static const int k_y[7] = { 0x143, 0x10F, 0xDC, 0xA8, 0x75, 0x41, 0x0F };
    int v[7] = {
        g_game.stats.perfectCount[p], g_game.stats.greatCount[p],
        g_game.stats.goodCount[p],    g_game.stats.badCount[p],
        g_game.stats.missCount[p],    (int)g_game.stats.maxCombo[p],
        (int)g_game.stats.score[p]
    };
    for (int i = 0; i < 7; i++) {
        if (t < 10 * i) break;
        int nd = (i == 6) ? 7 : 3;
        if (p == 0) exNumP1(i == 6 ? 0x96 : 0x3E, k_y[i], v[i], nd, t - 10 * i);
        else        exNumP2(0x251, k_y[i], v[i], nd, t - 10 * i);
    }
    /* t >= 150: letra no quadro t+300 (P1) / t+600 (P2). */
    if (t < 0x96) return;
    if (g_game.isBattleMode) {
        /* BATTLE ([0x568FF4] & 0x40), 0x40DAE9 (P1) / 0x40D78D (P2): no lugar da
         * letra, WIN (0x29/0x2A) ou LOSE (0x26/0x27). Vence o maior max combo;
         * empatado, a maior pontuação, e o P1 leva o empate de pontuação. */
        unsigned c0 = g_game.stats.maxCombo[0], c1 = g_game.stats.maxCombo[1];
        unsigned s0 = g_game.stats.score[0],    s1 = g_game.stats.score[1];
        bool p1Wins = (c0 != c1) ? (c0 > c1) : (s0 >= s1);
        bool win = (p == 0) ? p1Wins : !p1Wins;
        int slot = win ? 0x29 : 0x26;
        int frame = t + (p == 0 ? 300 : 600);
        BGA_DrawSlot(0, frame, slot);
        BGA_DrawSlot(0, frame, slot + 1);
        return;
    }
    exLetter(p == 0 ? g_gradeP1 : g_gradeP2, t + (p == 0 ? 300 : 600));
}

static void exGradeSounds(int t)
{
    if (t <= 120 && t % 5 == 0)                  /* 0x40CFDF: 8-1.WAV */
        Audio_Play(g_waveSoundIds[SND_8_1], false);
    if (t == 0x9E)                               /* 0x40D4DF: 9-5.WAV */
        Audio_Play(g_waveSoundIds[SND_9_5], false);
    if (t == 0xA3) {                             /* 0x40D312: melhor nota */
        int best = 5;
        if ((g_game.activePlayerMask & 1) && g_gradeP1 < best) best = g_gradeP1;
        if ((g_game.activePlayerMask & 2) && g_gradeP2 < best) best = g_gradeP2;
        static const SoundID k_rank[6] = { SND_RANK_A, SND_RANK_A, SND_RANK_B,
                                           SND_RANK_C, SND_RANK_D, SND_RANK_F };
        Audio_Play(g_waveSoundIds[k_rank[best]], false);
    }
}

void Result_Enter(void) {
    g_resultFrame = 0;
    g_exT = 0;
    g_lastDigitSoundCount = 0;
    g_lastSoundFrame = -100;
    g_gradeSoundPlayed = false;
    g_game.bgaLoop = false;
    g_game.bgaFrame = 0;

    Font_LoadTexture();   /* já grava no g_fontTexId global */

    BGM_Stop();
    char ap[MAX_PATH];
    /* Exceed 0x40CD3F: AUDIO\GRADE.AUD (loop: HIPÓTESE, igual ao Prex3) */
    snprintf(ap, sizeof(ap), "%s/AUDIO/%s.AUD", g_game.currentDirectory,
             g_exceedSongIds ? "GRADE" : "83");
    if (BGM_LoadAUDDirect(ap)) BGM_Play(true);
    /* Exceed2 (PIU32.EXE 0x40A674..0x40A68F): BGA\GRADE.MOV por baixo do
     * GRADE.DAT, aberto sem loop (2º argumento 0) se o arquivo existir */
    if (g_exceedSongIds) {
        char mp[MAX_PATH];
        snprintf(mp, sizeof(mp), "%s/BGA/GRADE.MOV", g_game.currentDirectory);
        FILE* mf = fopen(mp, "rb");
        if (mf) { fclose(mf); Movie_Open(mp, false); }
    }

    g_gradeP1 = calcGrade(g_game.stats.perfectCount[0], g_game.stats.greatCount[0],
                          g_game.stats.goodCount[0], g_game.stats.badCount[0],
                          g_game.stats.missCount[0], (int)g_game.stats.maxCombo[0]);
    g_gradeP2 = calcGrade(g_game.stats.perfectCount[1], g_game.stats.greatCount[1],
                          g_game.stats.goodCount[1], g_game.stats.badCount[1],
                          g_game.stats.missCount[1], (int)g_game.stats.maxCombo[1]);
    if (g_exceedSongIds) {
        Gameplay_ExScoreSync();
        int st = exStageIdx();
        if (st == 0) memset(g_exStageRatio, 0, sizeof(g_exStageRatio));
        /* IR: TOTAL_SCORE e m_PlayOrder[estágio] = índice em g_exSongs (src/ir.c) */
        IR_RecordStage(st, g_game.selectedSongIndex, g_game.stats.score);
        for (int p = 0; p < 2; p++) {
            float r = exRatio(p);
            g_exStageRatio[p][st] = r;
            if (p == 0) g_gradeP1 = exGradeOf(r, g_game.stats.missCount[0]);
            else        g_gradeP2 = exGradeOf(r, g_game.stats.missCount[1]);
        }
    }
    Log_Print("Result: grades P1=%d P2=%d\n", g_gradeP1, g_gradeP2);
}

void Result_Update(float dt) {
    (void)dt;
    if (g_exceedSongIds) {
        if (g_game.state == STATE_DANCE_GRADE_ENTER) {
            /* 0x40CF34: quadros 0..60, depois zera o contador */
            if (++g_resultFrame > 0x3C) {
                g_resultFrame = 0;
                g_exT = 0;
                Game_ChangeState(STATE_DANCE_GRADE_DISPLAY);
            }
            return;
        }
        if (g_game.state != STATE_DANCE_GRADE_DISPLAY) return;
        exGradeSounds(g_exT);
        if (g_exT > 0x168) {                     /* 0x40D0E7 */
            GameState ns = Result_GetNextState();
            Movie_Close();                       /* End 0x40B64B */
            BGM_Stop();
            Resource_ClearBGA();
            Game_ChangeState(ns);
            return;
        }
        g_exT++;
        return;
    }
    if (g_game.state == STATE_DANCE_GRADE_ENTER) {
        g_resultFrame++;
        if (g_resultFrame >= 15) {
            g_resultFrame = 0;
            Game_ChangeState(STATE_DANCE_GRADE_DISPLAY);
        }
        return;
    }
    if (g_game.state != STATE_DANCE_GRADE_DISPLAY) return;

    g_resultFrame++;
    if (g_resultFrame >= 0x24E) {
        GameState ns = Result_GetNextState();
        Log_Print("RESULT: auto-transition at f=%d, next=%d=%s\n", g_resultFrame, ns, ns==STATE_GAMEOVER_ENTER?"GAMEOVER":ns==STATE_STAGE_TRANSITION?"STAGE_TRANS":"SONG_SEL");
        BGM_Stop();
        Resource_ClearBGA();
        Game_ChangeState(ns);
        return;
    }
    if (Input_IsKeyHit(VK_ESCAPE) || Input_IsKeyHit(VK_RETURN) ||
        Input_IsKeyHit(VK_SPACE) || Input_IsKeyHit(VK_F1)) {
        Log_Print("RESULT: manual exit at f=%d bgaCount=%d\n", g_resultFrame, g_game.bgaPicCount);
        BGM_Stop();
        Resource_ClearBGA();
        Log_Print("RESULT: manual exit, next=%d\n", Result_GetNextState());
        Game_ChangeState(Result_GetNextState());
        return;
    }
    if (Input_IsKeyHit(VK_ESCAPE) || Input_IsKeyHit(VK_RETURN) ||
        Input_IsKeyHit(VK_SPACE) || Input_IsKeyHit(VK_F1)) {
        Log_Print("RESULT: manual exit (2nd), next=%d\n", Result_GetNextState());
        BGM_Stop();
        glClear(GL_COLOR_BUFFER_BIT);
        Texture_Shutdown();
        Resource_ClearBGA();
        Render_SetGlobalColor(0, 0, 0, 1.0f);
        Game_ChangeState(Result_GetNextState());
        return;
    }
    // Keep BGA playing/looping so background tiles cycle
    g_game.bgaFrame++;
    if (g_game.bgaFrame >= g_game.bgaMaxFrame)
        g_game.bgaFrame = 0;
}

void Result_Render(void) {
    if (g_exceedSongIds) {
        if (g_game.bgaPicCount <= 0) return;
        if (g_game.state == STATE_DANCE_GRADE_ENTER) {
            BGA_SetEventFrame(0, g_resultFrame);
            return;
        }
        if (g_game.state != STATE_DANCE_GRADE_DISPLAY) return;
        BGA_SetEventFrame(0, g_exT % 360 + 60);
        if (g_game.activePlayerMask & 1) exPlayer(0, g_exT);
        if (g_game.activePlayerMask & 2) exPlayer(1, g_exT);
        return;
    }
    if (g_game.state == STATE_DANCE_GRADE_ENTER) {
        if (g_game.bgaPicCount > 0) BGA_Render(0, g_game.bgaFrame);
        return;
    }
    if (g_game.state != STATE_DANCE_GRADE_DISPLAY) return;

    int f = g_resultFrame;

    // Toca 8-1.wav a cada 5 frames (independente do digito, total 25)
    if (f >= 60 && (f - g_lastSoundFrame) >= 5 && g_lastDigitSoundCount < 25) {
        Audio_Play(g_waveSoundIds[SND_8_1], false);
        g_lastDigitSoundCount++;
        g_lastSoundFrame = f;
    }

    /* Grade sound (5-1 + rank) quando a nota aparece no frame 0xd2.
     * Para P2 solo usa g_gradeP2; para 2P toca apenas o som do player efetivo
     * (P1 decide, pois ambas as grades já estão visíveis na tela). */
    if (f >= 0xd2 && !g_gradeSoundPlayed) {
        g_gradeSoundPlayed = true;
        int effectiveGrade = (g_game.activePlayerMask == 0x2) ? g_gradeP2 : g_gradeP1;
        Audio_Play(g_waveSoundIds[SND_5_1], false);
        int rankSnd;
        if (effectiveGrade <= 1) rankSnd = SND_RANK_A;        // S ou A
        else if (effectiveGrade == 2) rankSnd = SND_RANK_B;
        else if (effectiveGrade == 3) rankSnd = SND_RANK_C;
        else if (effectiveGrade == 4) rankSnd = SND_RANK_D;
        else rankSnd = SND_RANK_F;
        Audio_Play(g_waveSoundIds[rankSnd], false);
    }

    // Last frames: only show "Press ENTER" text, no BGA/CLEAR/FAIL
    if (f >= 0x24E) {
        Font_DrawStringCentered(g_game.screenWidth/2, 16,
            "Press ENTER or ESC to continue", 0.7f, 0.7f, 0.7f, 1.0f);
        return;
    }

    if (g_game.bgaPicCount > 0) BGA_Render(0, g_game.bgaFrame);

    int offP1 = 0, offP2 = 0;
    bool drawNums = true;
    if (f > 0x194 && f < 0x1a0) {
        int d = f - 0x195;
        offP1 = (int)((float)d * -22.72f);
        offP2 = (int)((float)d * 23.11f);
    } else if (f >= 0x1a0) {
        drawNums = false;
    }

    if (drawNums) {
        /* Itera apenas os players ativos.
         * P1 (p=0): drawNumP1 no lado esquerdo (x=8).
         * P2 (p=1): drawNumP2 no lado direito (rx=603). */
        int pStart = (g_game.activePlayerMask == 0x2) ? 1 : 0;
        int pEnd   = (g_game.activePlayerMask & 0x2)  ? 2 : 1;
        for (int p = pStart; p < pEnd; p++) {
            int stats[7] = {
                g_game.stats.perfectCount[p],
                g_game.stats.greatCount[p],
                g_game.stats.goodCount[p],
                g_game.stats.badCount[p],
                g_game.stats.missCount[p],
                (int)g_game.stats.maxCombo[p],
                (int)g_game.stats.score[p]
            };
            for (int i = 0; i < 7; i++) {
                if (f < g_statDelay[i]) continue;
                int elap = f - g_statDelay[i];
                if (p == 0)
                    drawNumP1(8,   g_statY[i], stats[i], g_statDigits[i], elap, offP1);
                else
                    drawNumP2(603, g_statY[i], stats[i], g_statDigits[i], elap, offP2);
            }
        }
    }

    // Grade letter via BGA event layer: frames 211-416
    if (f > 0xd2 && f < 0x1a0) {
        if (g_game.activePlayerMask & 0x1)
            BGA_SetEventLayer(0, f + 0x348, 44 + g_gradeP1);
        if (g_game.activePlayerMask & 0x2)
            BGA_SetEventLayer(0, f + 0x438, 44 + g_gradeP2);
    }

    /* CLEAR/FAIL — P2 solo usa g_gradeP2; em 2P mostra resultado de cada player
     * no mesmo layer (0x1d=CLEAR, 0x1b=FAIL). Para 2P, o CLEAR/FAIL visual
     * do binário original usava apenas a tela de P1; mantemos a mesma layer. */
    if (f > 0x1a3) {
        int activeGrade;
        if (g_game.activePlayerMask == 0x2)
            activeGrade = g_gradeP2;          // P2 solo
        else if (g_game.activePlayerMask == 0x3)
            activeGrade = (g_gradeP1 < g_gradeP2) ? g_gradeP1 : g_gradeP2; // 2P: melhor nota decide
        else
            activeGrade = g_gradeP1;          // P1 solo (padrão)

        int clearFail = (activeGrade < 5) ? 0x1d : 0x1b;
        int cfOff     = (clearFail == 0x1d) ? 0xc1 : 0x175;
        BGA_SetEventLayer(0, f + cfOff, clearFail);
    }

    Font_DrawStringCentered(g_game.screenWidth/2, 16,
        "Press ENTER or ESC to continue", 0.7f, 0.7f, 0.7f, 1.0f);
}
