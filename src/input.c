#include "pumpy.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* O Windows SDK nao define VK_A/VK_0 (usa os literais ASCII). */
#ifndef VK_A
#define VK_A 0x41
#endif
#ifndef VK_0
#define VK_0 0x30
#endif

/* Linux native input: the keyboard is read straight from SDL_GetKeyboardState
 * every frame and mapped onto Windows virtual-key codes so the game logic
 * (which indexes g_game.input.keys[256] by VK_*) stays unchanged.
 *
 * The pad uses the original keyboard mapping. The PUMPPAD.DLL cabinet-interface
 * is not available on Linux; a real dance pad / joystick is not integrated yet.
 */

#define KEY_PAD_MAP_SDL P2_JUST_PLACEHOLDER /* unused on this backend */

static bool g_focused = true;

/* Teclas do pad, no esquema do original (KEYInitialize, PUMPY.EXE 0x4018b0).
 *
 * O original carrega "piukey.cfg" com fopen("rt") + fscanf: 2 linhas de 9
 * scancodes hex ("%02x %02x ... %02x\n"), uma grade 3x3 por jogador:
 *
 *      P1: Z X C / A S D / Q W E          (2c 2d 2e 1e 1f 20 10 11 12)
 *      P2: Num1 2 3 / 4 5 6 / 7 8 9       (4f 50 51 4b 4c 4d 47 48 49)
 *
 * Arquivo ausente = padrão acima ("...FALSE" no log). Só os cantos e o centro
 * (índices 0,2,4,6,8 = botões 1,3,5,7,9) viram botões do pad; as outras 4 teclas
 * da grade não têm consumidor no original (a tabela só é escrita, nunca lida).
 *
 * No original as teclas de jogo chegam pela WndProc como VK_*: VK_HOME/END/
 * PRIOR/NEXT/CLEAR são o numpad com NumLock desligado OU as teclas de navegação.
 * Por isso 0x47/0x49/0x4f/0x51 (Num7/9/1/3) aceitam também Home/PgUp/End/PgDn,
 * e 0x4c (Num5) aceita o Keypad 5. Os códigos estendidos (0x80|código, estilo
 * DirectInput: C7 Home, C9 PgUp, CF End, D1 PgDn...) aceitam só a tecla de
 * navegação. Guardamos o SCANCODE físico do SDL: o NumLock não interfere.
 *
 * Tabela em escopo de arquivo: o polling do Input_Update e a captura por
 * evento abaixo precisam dela. */
#define PAD_MAX_KEYS 2

typedef struct {
    int n;
    SDL_Scancode sc[PAD_MAX_KEYS];
} PadKeys;

#define MAX_JOYSTICKS 4
static SDL_Joystick* g_joysticks[MAX_JOYSTICKS];

typedef enum {
    JOY_BIND_NONE = 0,
    JOY_BIND_BUTTON = 1,
    JOY_BIND_HAT = 2,
} JoyBindType;

typedef struct {
    JoyBindType type;
    int         device;
    int         id;
} JoyBinding;

static JoyBinding g_padJoy[2][PAD_BUTTONS_PER_PLAYER];

static struct {
    bool active;
    int  player;
    int  button;
} g_listen;

static PadKeys g_padKeys[2][PAD_BUTTONS_PER_PLAYER];
static bool g_keysLoaded = false;

static const uint8_t kDefaultCfg[2][9] = {
    { 0x2c, 0x2d, 0x2e, 0x1e, 0x1f, 0x20, 0x10, 0x11, 0x12 },
    { 0x4f, 0x50, 0x51, 0x4b, 0x4c, 0x4d, 0x47, 0x48, 0x49 },
};

/* índice da grade 3x3 -> bit do botão (0=7 1=9 2=5 3=1 4=3), -1 = sem função */
static const signed char kGridBtn[9] = { 3, -1, 4, -1, 2, -1, 0, -1, 1 };

/* botão do pad (0=7 1=9 2=5 3=1 4=3) -> índice na grade 3x3 do piukey.cfg */
static const int kBtnGridIdx[PAD_BUTTONS_PER_PLAYER] = { 6, 8, 4, 0, 2 };

static uint8_t g_currCfg[2][9];

/* scancode PS/2 set 1 (0x80|x = estendido, estilo DirectInput) -> teclas SDL */
#define K1(c, a)    { c, SDL_SCANCODE_##a, SDL_SCANCODE_UNKNOWN }
#define K2(c, a, b) { c, SDL_SCANCODE_##a, SDL_SCANCODE_##b }
static const struct { uint8_t code; SDL_Scancode a, b; } kScan[] = {
    K1(0x01, ESCAPE), K1(0x02, 1), K1(0x03, 2), K1(0x04, 3), K1(0x05, 4), K1(0x06, 5),
    K1(0x07, 6), K1(0x08, 7), K1(0x09, 8), K1(0x0a, 9), K1(0x0b, 0), K1(0x0c, MINUS),
    K1(0x0d, EQUALS), K1(0x0e, BACKSPACE), K1(0x0f, TAB), K1(0x10, Q), K1(0x11, W),
    K1(0x12, E), K1(0x13, R), K1(0x14, T), K1(0x15, Y), K1(0x16, U), K1(0x17, I),
    K1(0x18, O), K1(0x19, P), K1(0x1a, LEFTBRACKET), K1(0x1b, RIGHTBRACKET),
    K1(0x1c, RETURN), K1(0x1d, LCTRL), K1(0x1e, A), K1(0x1f, S), K1(0x20, D),
    K1(0x21, F), K1(0x22, G), K1(0x23, H), K1(0x24, J), K1(0x25, K), K1(0x26, L),
    K1(0x27, SEMICOLON), K1(0x28, APOSTROPHE), K1(0x29, GRAVE), K1(0x2a, LSHIFT),
    K1(0x2b, BACKSLASH), K1(0x2c, Z), K1(0x2d, X), K1(0x2e, C), K1(0x2f, V),
    K1(0x30, B), K1(0x31, N), K1(0x32, M), K1(0x33, COMMA), K1(0x34, PERIOD),
    K1(0x35, SLASH), K1(0x36, RSHIFT), K1(0x37, KP_MULTIPLY), K1(0x38, LALT),
    K1(0x39, SPACE), K1(0x3a, CAPSLOCK), K1(0x3b, F1), K1(0x3c, F2), K1(0x3d, F3),
    K1(0x3e, F4), K1(0x3f, F5), K1(0x40, F6), K1(0x41, F7), K1(0x42, F8), K1(0x43, F9),
    K1(0x44, F10), K1(0x45, NUMLOCKCLEAR), K1(0x46, SCROLLLOCK),
    K2(0x47, KP_7, HOME),     K2(0x48, KP_8, UP),       K2(0x49, KP_9, PAGEUP),
    K1(0x4a, KP_MINUS),
    K2(0x4b, KP_4, LEFT),     K1(0x4c, KP_5),           K2(0x4d, KP_6, RIGHT),
    K1(0x4e, KP_PLUS),
    K2(0x4f, KP_1, END),      K2(0x50, KP_2, DOWN),     K2(0x51, KP_3, PAGEDOWN),
    K2(0x52, KP_0, INSERT),   K2(0x53, KP_PERIOD, DELETE),
    K1(0x57, F11), K1(0x58, F12),
    K1(0x9c, KP_ENTER), K1(0x9d, RCTRL), K1(0xb5, KP_DIVIDE), K1(0xb8, RALT),
    K1(0xc7, HOME), K1(0xc8, UP), K1(0xc9, PAGEUP), K1(0xcb, LEFT), K1(0xcd, RIGHT),
    K1(0xcf, END), K1(0xd0, DOWN), K1(0xd1, PAGEDOWN), K1(0xd2, INSERT), K1(0xd3, DELETE),
};

static const char* const kBtnName[PAD_BUTTONS_PER_PLAYER] = { "7", "9", "5", "1", "3" };

static bool Input_ScanLookup(uint8_t code, PadKeys* out) {
    for (size_t i = 0; i < sizeof(kScan) / sizeof(kScan[0]); i++) {
        if (kScan[i].code != code) continue;
        out->n = 0;
        out->sc[out->n++] = kScan[i].a;
        if (kScan[i].b != SDL_SCANCODE_UNKNOWN) out->sc[out->n++] = kScan[i].b;
        return true;
    }
    return false;
}

static uint8_t Input_CodeFromScancode(SDL_Scancode sc) {
    for (size_t i = 0; i < sizeof(kScan) / sizeof(kScan[0]); i++) {
        if (kScan[i].a == sc || (kScan[i].b != SDL_SCANCODE_UNKNOWN && kScan[i].b == sc))
            return kScan[i].code;
    }
    return 0;
}

static void Input_InitJoysticks(void) {
    static bool inited = false;
    if (inited) return;
    inited = true;
    int n = SDL_NumJoysticks();
    for (int i = 0; i < n && i < MAX_JOYSTICKS; i++) {
        g_joysticks[i] = SDL_JoystickOpen(i);
        if (g_joysticks[i]) {
            Log_Print("Input: opened joystick %d: '%s'\n", i, SDL_JoystickName(g_joysticks[i]));
        }
    }
}

/* Carrega piukey.cfg (pasta do jogo) como o KEYInitialize do original. */
void Input_LoadKeyConfig(void) {
    uint8_t cfg[2][9];
    memcpy(cfg, kDefaultCfg, sizeof(cfg));
    g_keysLoaded = true;

    Input_InitJoysticks();

    Log_Print("KEYInitialize : LOADING PIUKEY.CFG\n");
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/piukey.cfg", g_game.currentDirectory);
    FILE* f = fopen(path, "rt");
    if (!f) {
        Log_Print("...FALSE\n");
    } else {
        bool ok = true;
        for (int p = 0; p < 2 && ok; p++)
            for (int i = 0; i < 9; i++) {
                unsigned v;
                if (fscanf(f, "%x", &v) != 1) { ok = false; break; }
                cfg[p][i] = (uint8_t)v;
            }
        fclose(f);
        Log_Print(ok ? "...OK\n" : "...FALSE\n");
    }

    memcpy(g_currCfg, cfg, sizeof(g_currCfg));

    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < 9; i++) {
            int b = kGridBtn[i];
            if (b < 0) continue;
            PadKeys* k = &g_padKeys[p][b];
            if (!Input_ScanLookup(cfg[p][i], k)) {
                Log_Print("Input: piukey.cfg P%d key %02x unknown, using default %02x\n",
                          p + 1, cfg[p][i], kDefaultCfg[p][i]);
                cfg[p][i] = kDefaultCfg[p][i];
                g_currCfg[p][i] = kDefaultCfg[p][i];
                Input_ScanLookup(cfg[p][i], k);
            }
            char names[64] = "";
            for (int j = 0; j < k->n; j++) {
                size_t len = strlen(names);
                snprintf(names + len, sizeof(names) - len, "%s%s", j ? " | " : "",
                         SDL_GetScancodeName(k->sc[j]));
            }
            Log_Print("Input: P%d %s = %02x (%s)\n", p + 1, kBtnName[b], cfg[p][i], names);
        }
    }
}

uint8_t Input_GetButtonKey(int player, PadButton b) {
    if (player < 0 || player > 1 || b < 0 || b >= PAD_BUTTONS_PER_PLAYER) return 0;
    return g_currCfg[player][kBtnGridIdx[b]];
}

void Input_SetButtonKey(int player, PadButton b, uint8_t code) {
    if (player < 0 || player > 1 || b < 0 || b >= PAD_BUTTONS_PER_PLAYER) return;
    g_currCfg[player][kBtnGridIdx[b]] = code;
    Input_ScanLookup(code, &g_padKeys[player][b]);
}

void Input_GetButtonKeyName(int player, PadButton b, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (player < 0 || player > 1 || b < 0 || b >= PAD_BUTTONS_PER_PLAYER) {
        snprintf(out, outSize, "--");
        return;
    }
    uint8_t code = g_currCfg[player][kBtnGridIdx[b]];
    for (size_t i = 0; i < sizeof(kScan) / sizeof(kScan[0]); i++) {
        if (kScan[i].code == code) {
            snprintf(out, outSize, "%s", SDL_GetScancodeName(kScan[i].a));
            return;
        }
    }
    snprintf(out, outSize, "%02X", code);
}

void Input_GetButtonJoyName(int player, PadButton b, char* out, size_t outSize) {
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (player < 0 || player > 1 || b < 0 || b >= PAD_BUTTONS_PER_PLAYER) {
        snprintf(out, outSize, "--");
        return;
    }
    JoyBinding* j = &g_padJoy[player][b];
    if (j->type == JOY_BIND_BUTTON) {
        snprintf(out, outSize, "J%d Btn %d", j->device + 1, j->id);
    } else if (j->type == JOY_BIND_HAT) {
        const char* dir = (j->id & SDL_HAT_UP) ? "UP" :
                          (j->id & SDL_HAT_DOWN) ? "DOWN" :
                          (j->id & SDL_HAT_LEFT) ? "LEFT" :
                          (j->id & SDL_HAT_RIGHT) ? "RIGHT" : "?";
        snprintf(out, outSize, "J%d Hat %s", j->device + 1, dir);
    } else {
        snprintf(out, outSize, "--");
    }
}

void Input_ClearJoyBindings(void) {
    memset(g_padJoy, 0, sizeof(g_padJoy));
}

void Input_RestoreDefaultConfig(void) {
    memcpy(g_currCfg, kDefaultCfg, sizeof(g_currCfg));
    memset(g_padJoy, 0, sizeof(g_padJoy));
    for (int p = 0; p < 2; p++) {
        for (int i = 0; i < 9; i++) {
            int b = kGridBtn[i];
            if (b >= 0) Input_ScanLookup(g_currCfg[p][i], &g_padKeys[p][b]);
        }
    }
}

void Input_SaveKeyConfig(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/piukey.cfg", g_game.currentDirectory);
    FILE* f = fopen(path, "wt");
    if (!f) {
        Log_Print("Input: falha ao salvar '%s'\n", path);
        return;
    }
    for (int p = 0; p < 2; p++) {
        fprintf(f, "%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
                g_currCfg[p][0], g_currCfg[p][1], g_currCfg[p][2],
                g_currCfg[p][3], g_currCfg[p][4], g_currCfg[p][5],
                g_currCfg[p][6], g_currCfg[p][7], g_currCfg[p][8]);
    }
    fclose(f);
    Log_Print("Input: piukey.cfg salvo com sucesso\n");
}

void Input_SaveJoyConfig(void) {
    GameOption_Save();
}

void Input_WriteJoyConfigToIni(FILE* f) {
    if (!f) return;
    fprintf(f, "[Joystick]\n");
    static const char* const bNames[5] = { "UL", "UR", "C", "DL", "DR" };
    for (int p = 0; p < 2; p++) {
        for (int b = 0; b < 5; b++) {
            fprintf(f, "Joy_P%d_%s=%d,%d,%d\n", p + 1, bNames[b],
                    (int)g_padJoy[p][b].type, g_padJoy[p][b].device, g_padJoy[p][b].id);
        }
    }
}

void Input_LoadJoyConfig(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/PUMPY.INI", g_game.currentDirectory);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128];
    static const char* const bNames[5] = { "UL", "UR", "C", "DL", "DR" };
    while (fgets(line, sizeof(line), f)) {
        for (int p = 0; p < 2; p++) {
            for (int b = 0; b < 5; b++) {
                char prefix[32];
                snprintf(prefix, sizeof(prefix), "Joy_P%d_%s=", p + 1, bNames[b]);
                char* pos = strstr(line, prefix);
                if (pos) {
                    int t = 0, dev = 0, id = 0;
                    if (sscanf(pos + strlen(prefix), "%d,%d,%d", &t, &dev, &id) == 3) {
                        g_padJoy[p][b].type = (JoyBindType)t;
                        g_padJoy[p][b].device = dev;
                        g_padJoy[p][b].id = id;
                    }
                }
            }
        }
    }
    fclose(f);
}

bool Input_IsListening(void) {
    return g_listen.active;
}

void Input_StartListen(int player, PadButton b) {
    g_listen.active = true;
    g_listen.player = player;
    g_listen.button = (int)b;
}

void Input_CancelListen(void) {
    g_listen.active = false;
}

static uint32_t g_padEdge[2];

static int Input_VKFromEvent(const SDL_Event* ev);

bool Input_ProcessEvent(void* evp) {
    SDL_Event* ev = (SDL_Event*)evp;

    if (g_listen.active) {
        if (ev->type == SDL_KEYDOWN) {
            if (ev->key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                g_listen.active = false;
                return true;
            }
            uint8_t code = Input_CodeFromScancode(ev->key.keysym.scancode);
            if (code != 0) {
                Input_SetButtonKey(g_listen.player, (PadButton)g_listen.button, code);
                g_listen.active = false;
                return true;
            }
        } else if (ev->type == SDL_JOYBUTTONDOWN) {
            int dev = 0;
            for (int i = 0; i < MAX_JOYSTICKS; i++) {
                if (g_joysticks[i] && SDL_JoystickInstanceID(g_joysticks[i]) == ev->jbutton.which) {
                    dev = i; break;
                }
            }
            g_padJoy[g_listen.player][g_listen.button].type = JOY_BIND_BUTTON;
            g_padJoy[g_listen.player][g_listen.button].device = dev;
            g_padJoy[g_listen.player][g_listen.button].id = ev->jbutton.button;
            g_listen.active = false;
            return true;
        } else if (ev->type == SDL_JOYHATMOTION) {
            if (ev->jhat.value != SDL_HAT_CENTERED) {
                int dev = 0;
                for (int i = 0; i < MAX_JOYSTICKS; i++) {
                    if (g_joysticks[i] && SDL_JoystickInstanceID(g_joysticks[i]) == ev->jhat.which) {
                        dev = i; break;
                    }
                }
                g_padJoy[g_listen.player][g_listen.button].type = JOY_BIND_HAT;
                g_padJoy[g_listen.player][g_listen.button].device = dev;
                g_padJoy[g_listen.player][g_listen.button].id = ev->jhat.value;
                g_listen.active = false;
                return true;
            }
        }
        return true;
    }

    switch (ev->type) {
    case SDL_KEYDOWN:
        if (!ev->key.repeat) {
            if (!g_keysLoaded) Input_LoadKeyConfig();
            SDL_Scancode sc = ev->key.keysym.scancode;
            for (int p = 0; p < 2; p++)
                for (int b = 0; b < PAD_BUTTONS_PER_PLAYER; b++)
                    for (int i = 0; i < g_padKeys[p][b].n; i++)
                        if (g_padKeys[p][b].sc[i] == sc)
                            g_padEdge[p] |= (1u << b);
        }
        if (Debug_ConsoleIsActive()) {
            int vk = Input_VKFromEvent(ev);
            Debug_ConsoleKeyHandler(0, vk);
        }
        break;
    case SDL_JOYBUTTONDOWN: {
        int dev = 0;
        for (int i = 0; i < MAX_JOYSTICKS; i++) {
            if (g_joysticks[i] && SDL_JoystickInstanceID(g_joysticks[i]) == ev->jbutton.which) {
                dev = i; break;
            }
        }
        for (int p = 0; p < 2; p++) {
            for (int b = 0; b < PAD_BUTTONS_PER_PLAYER; b++) {
                if (g_padJoy[p][b].type == JOY_BIND_BUTTON &&
                    g_padJoy[p][b].device == dev &&
                    g_padJoy[p][b].id == ev->jbutton.button) {
                    g_padEdge[p] |= (1u << b);
                }
            }
        }
        break;
    }
    case SDL_JOYHATMOTION: {
        int dev = 0;
        for (int i = 0; i < MAX_JOYSTICKS; i++) {
            if (g_joysticks[i] && SDL_JoystickInstanceID(g_joysticks[i]) == ev->jhat.which) {
                dev = i; break;
            }
        }
        for (int p = 0; p < 2; p++) {
            for (int b = 0; b < PAD_BUTTONS_PER_PLAYER; b++) {
                if (g_padJoy[p][b].type == JOY_BIND_HAT &&
                    g_padJoy[p][b].device == dev &&
                    (ev->jhat.value & g_padJoy[p][b].id)) {
                    g_padEdge[p] |= (1u << b);
                }
            }
        }
        break;
    }
    case SDL_JOYDEVICEADDED: {
        int id = ev->jdevice.which;
        for (int i = 0; i < MAX_JOYSTICKS; i++) {
            if (!g_joysticks[i]) {
                g_joysticks[i] = SDL_JoystickOpen(id);
                if (g_joysticks[i])
                    Log_Print("Input: joystick connected in slot %d: '%s'\n", i, SDL_JoystickName(g_joysticks[i]));
                break;
            }
        }
        break;
    }
    case SDL_JOYDEVICEREMOVED: {
        SDL_JoystickID jid = ev->jdevice.which;
        for (int i = 0; i < MAX_JOYSTICKS; i++) {
            if (g_joysticks[i] && SDL_JoystickInstanceID(g_joysticks[i]) == jid) {
                Log_Print("Input: joystick disconnected from slot %d\n", i);
                SDL_JoystickClose(g_joysticks[i]);
                g_joysticks[i] = NULL;
                break;
            }
        }
        break;
    }
    case SDL_TEXTINPUT:
        if (Debug_ConsoleIsActive() && ev->text.text[0]) {
            Debug_ConsoleKeyHandler((unsigned char)ev->text.text[0], 0);
        }
        break;
    default:
        break;
    }
    return true;
}

/* Map an SDL_Scancode onto a Windows VK_* code. Covers the keys the engine
 * actually listens to. */
static int Input_VKFromEvent(const SDL_Event* ev) {
    SDL_Keycode sym = ev->key.keysym.sym;
    if (ev->key.keysym.scancode == SDL_SCANCODE_GRAVE) return VK_OEM_3; /* ver Input_Update */
    switch (sym) {
    case SDLK_BACKSPACE: return VK_BACK;
    case SDLK_TAB:       return VK_TAB;
    case SDLK_RETURN:    return VK_RETURN;
    case SDLK_ESCAPE:    return VK_ESCAPE;
    case SDLK_SPACE:     return VK_SPACE;
    case SDLK_HOME:      return VK_HOME;
    case SDLK_END:       return VK_END;
    case SDLK_PAGEUP:    return VK_PRIOR;
    case SDLK_PAGEDOWN:  return VK_NEXT;
    case SDLK_INSERT:    return VK_INSERT;
    case SDLK_DELETE:    return VK_DELETE;
    case SDLK_LEFT:      return VK_LEFT;
    case SDLK_RIGHT:     return VK_RIGHT;
    case SDLK_UP:        return VK_UP;
    case SDLK_DOWN:      return VK_DOWN;
    case SDLK_F1:  return VK_F1;
    case SDLK_F2:  return VK_F2;
    case SDLK_F3:  return VK_F3;
    case SDLK_F4:  return VK_F4;
    case SDLK_F5:  return VK_F5;
    case SDLK_F6:  return VK_F6;
    case SDLK_F7:  return VK_F7;
    case SDLK_F8:  return VK_F8;
    case SDLK_F9:  return VK_F9;
    case SDLK_F10: return VK_F10;
    case SDLK_F11: return VK_F11;
    case SDLK_F12: return VK_F12;
    case SDLK_BACKQUOTE: return VK_OEM_3;
    case SDLK_KP_5:      return VK_CLEAR;
    case SDLK_KP_ENTER:
    case SDLK_KP_1:  return VK_END;
    case SDLK_KP_3:  return VK_NEXT;
    case SDLK_KP_7:  return VK_HOME;
    case SDLK_KP_9:  return VK_PRIOR;
    default:
        if (sym >= SDLK_a && sym <= SDLK_z)   return VK_A + (sym - SDLK_a);
        if (sym >= SDLK_0 && sym <= SDLK_9)   return VK_0 + (sym - SDLK_0);
        if (sym >= SDLK_F1 && sym <= SDLK_F12) return VK_F1 + (sym - SDLK_F1);
        return 0;
    }
}

bool Input_LoadPumpPad(void) {
    Log_Print("Input: PUMPPAD.DLL not available on Linux, using keyboard\n");
    return false;
}

void Input_Update(void) {
    int p;

    if (!g_keysLoaded) Input_LoadKeyConfig();

    for (p = 0; p < 2; p++) {
        g_game.input.padPrevState[p] = g_game.input.padState[p];
        g_game.input.padState[p] = 0;
    }

    /* Refresh VK-keyed state from the physical keyboard. */
    SDL_PumpEvents();
    const Uint8* kbd = SDL_GetKeyboardState(NULL);
    if (!kbd) return;

    memset(g_game.input.keys, 0, sizeof(g_game.input.keys));

    /* Alphabet */
    for (int c = SDLK_a; c <= SDLK_z; c++)
        if (kbd[SDL_GetScancodeFromKey(c)]) g_game.input.keys[VK_A + (c - SDLK_a)] = true;
    /* Digits */
    for (int c = SDLK_0; c <= SDLK_9; c++)
        if (kbd[SDL_GetScancodeFromKey(c)]) g_game.input.keys[VK_0 + (c - SDLK_0)] = true;
    /* Function keys + control keys */
    static const struct { SDL_Keycode k; int vk; } ctrl[] = {
        { SDLK_F1, VK_F1 }, { SDLK_F2, VK_F2 }, { SDLK_F3, VK_F3 },
        { SDLK_F4, VK_F4 }, { SDLK_F5, VK_F5 }, { SDLK_F6, VK_F6 },
        { SDLK_F7, VK_F7 }, { SDLK_F8, VK_F8 }, { SDLK_F9, VK_F9 },
        { SDLK_F10, VK_F10 }, { SDLK_F11, VK_F11 }, { SDLK_F12, VK_F12 },
        { SDLK_RETURN, VK_RETURN }, { SDLK_ESCAPE, VK_ESCAPE },
        { SDLK_SPACE, VK_SPACE }, { SDLK_BACKQUOTE, VK_OEM_3 },
        { SDLK_HOME, VK_HOME }, { SDLK_END, VK_END },
        { SDLK_PAGEUP, VK_PRIOR }, { SDLK_PAGEDOWN, VK_NEXT },
        { SDLK_INSERT, VK_INSERT }, { SDLK_DELETE, VK_DELETE },
        { SDLK_LEFT, VK_LEFT }, { SDLK_RIGHT, VK_RIGHT },
        { SDLK_UP, VK_UP }, { SDLK_DOWN, VK_DOWN },
        { SDLK_KP_5, VK_CLEAR },
        /* Keypad = Home/End/PgUp/PgDn (igual ao Input_VKFromEvent). */
        { SDLK_KP_7, VK_HOME }, { SDLK_KP_1, VK_END },
        { SDLK_KP_9, VK_PRIOR }, { SDLK_KP_3, VK_NEXT },
        { SDLK_LALT, VK_MENU }, { SDLK_RALT, VK_MENU },
    };
    for (size_t i = 0; i < sizeof(ctrl) / sizeof(ctrl[0]); i++) {
        if (kbd[SDL_GetScancodeFromKey(ctrl[i].k)])
            g_game.input.keys[ctrl[i].vk] = true;
    }
    /* VK_OEM_3 no Windows é a posição física à esquerda do 1 (scancode 0x29),
     * independente do layout. Pelo keycode (SDLK_BACKQUOTE) ela some em
     * layouts onde a crase é tecla morta (ABNT2), e o console não abre. */
    if (kbd[SDL_SCANCODE_GRAVE]) g_game.input.keys[VK_OEM_3] = true;

    g_focused = false;
    SDL_Window* kb = SDL_GetKeyboardFocus();
    if (kb && (SDL_GetWindowFlags(kb) & SDL_WINDOW_INPUT_FOCUS))
        g_focused = true;

    /* Pad state from keyboard and joysticks, only when the window has focus. */
    if (g_focused) {
        for (p = 0; p < 2; p++) {
            for (int b = 0; b < PAD_BUTTONS_PER_PLAYER; b++) {
                for (int i = 0; i < g_padKeys[p][b].n; i++) {
                    if (kbd[g_padKeys[p][b].sc[i]]) {
                        g_game.input.padState[p] |= (1u << b);
                        break;
                    }
                }
                if (g_padJoy[p][b].type == JOY_BIND_BUTTON) {
                    int dev = g_padJoy[p][b].device;
                    if (dev >= 0 && dev < MAX_JOYSTICKS && g_joysticks[dev]) {
                        if (SDL_JoystickGetButton(g_joysticks[dev], g_padJoy[p][b].id))
                            g_game.input.padState[p] |= (1u << b);
                    }
                } else if (g_padJoy[p][b].type == JOY_BIND_HAT) {
                    int dev = g_padJoy[p][b].device;
                    if (dev >= 0 && dev < MAX_JOYSTICKS && g_joysticks[dev]) {
                        if (SDL_JoystickGetHat(g_joysticks[dev], 0) & g_padJoy[p][b].id)
                            g_game.input.padState[p] |= (1u << b);
                    }
                }
            }
        }
        /* Junta as bordas capturadas na fila de eventos desde o último update:
         * garante o toque curto que já foi solto antes deste polling. */
        for (p = 0; p < 2; p++)
            g_game.input.padState[p] |= g_padEdge[p];
    }
    g_padEdge[0] = 0;
    g_padEdge[1] = 0;
}

bool Input_IsPadHit(int player, PadButton button) {
    uint32_t mask = 1 << button;
    return (g_game.input.padState[player] & mask) &&
           !(g_game.input.padPrevState[player] & mask);
}

bool Input_IsPadDown(int player, PadButton button) {
    return (g_game.input.padState[player] & (1 << button)) != 0;
}

bool Input_IsKeyHit(int key) {
    return g_game.input.keys[key] && !g_game.input.prevKeys[key];
}

bool Input_IsKeyDown(int key) {
    return g_game.input.keys[key];
}

void Input_Shutdown(void) {
    for (int i = 0; i < MAX_JOYSTICKS; i++) {
        if (g_joysticks[i]) {
            SDL_JoystickClose(g_joysticks[i]);
            g_joysticks[i] = NULL;
        }
    }
    Log_Print("Input: shutdown\n");
}