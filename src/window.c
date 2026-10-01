/* SDL2-based window / GL context / event pump (Linux native port).
 * The original Win32 implementation is preserved in the repository history. */
#include "pumpy.h"
#include <SDL.h>

/* Since the game was built around a logical 640x480 backbuffer, the window
 * keeps that aspect ratio: when the window is resized the OpenGL viewport is
 * letterboxed so gameplay never stretches. */
#define LOGICAL_W LOGICAL_SCREEN_W
#define LOGICAL_H LOGICAL_SCREEN_H

static SDL_Window*   g_win = NULL;
static SDL_GLContext g_glc = NULL;
static bool          g_quitRequested = false;
static bool          g_winActive = true;
static int           g_winW = WINDOW_DEFAULT_W;
static int           g_winH = WINDOW_DEFAULT_H;

static void Window_UpdateViewport(void);

void Window_RequestQuit(void) {
    g_quitRequested = true;
}

static void Window_UpdateViewport(void) {
    int cw = 0, ch = 0;
    if (!g_win) return;
    SDL_GL_GetDrawableSize(g_win, &cw, &ch);
    if (cw <= 0 || ch <= 0) return;

    int vw = cw;
    int vh = (cw * LOGICAL_H) / LOGICAL_W;
    if (vh > ch) {
        vh = ch;
        vw = (ch * LOGICAL_W) / LOGICAL_H;
    }
    if (g_game.gfxAspect == 1) { vw = cw; vh = ch; }   /* ASPECT: STRETCH */
    glViewport((cw - vw) / 2, (ch - vh) / 2, vw, vh);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    /* All 2D vertices are emitted at z=0. Keep that plane inside the
     * clip volume; near=1/far=0 puts it on the far boundary and can make
     * the compatibility driver discard the entire scene. */
    glOrtho(0, (GLdouble)LOGICAL_W, 0, (GLdouble)LOGICAL_H, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

/* Tela cheia EXCLUSIVA: troca o modo de vídeo do monitor para a RESOLUTION
 * escolhida (640x480 = como o original rodava). O original abria em tela cheia
 * na resolução do jogo; o FULLSCREEN_DESKTOP (janela sem borda na resolução da
 * área de trabalho) fica só como reserva se o monitor não aceitar o modo. */
static void Window_SetFullscreen(bool fs) {
    if (!g_win) return;
    if (!fs) { SDL_SetWindowFullscreen(g_win, 0); return; }

    int w, h;
    Window_GetResolution(g_game.gfxResIdx, &w, &h);
    SDL_DisplayMode want, got;
    memset(&want, 0, sizeof(want));
    want.w = w; want.h = h; want.refresh_rate = 60;
    int disp = SDL_GetWindowDisplayIndex(g_win);
    if (disp < 0) disp = 0;
    if (SDL_GetClosestDisplayMode(disp, &want, &got) &&
        SDL_SetWindowDisplayMode(g_win, &got) == 0 &&
        SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN) == 0) {
        Log_Print("Window: fullscreen exclusivo %dx%d @ %d Hz (pedido %dx%d)\n",
                  got.w, got.h, got.refresh_rate, w, h);
        return;
    }
    Log_Print("Window: modo %dx%d indisponivel (%s), usando fullscreen desktop\n", w, h, SDL_GetError());
    SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP);
}

bool Window_Create(HINSTANCE hInstance, int width, int height, bool fullscreen) {
    (void)hInstance;

#ifdef SDL_MAIN_HANDLED
    SDL_SetMainReady(); /* main() próprio (sem SDL2main): necessário no Windows */
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        Log_Print("SDL: SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /* Legacy GL 2.x context -> glBegin/glEnd immediate mode works everywhere. */
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);

    /* Compatibility profile is only valid for GL >= 3.2 on some drivers; retry
     * without it if creation fails. */
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    g_win = SDL_CreateWindow("Pump it Up: Exceed 2",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              width, height, flags);
    if (!g_win) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, 0);
        g_win = SDL_CreateWindow("Pump it Up: Exceed 2",
                                 SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 width, height, flags);
    }
    if (!g_win) {
        Log_Print("SDL: SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    g_glc = SDL_GL_CreateContext(g_win);
    if (!g_glc) {
        Log_Print("SDL: SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_win);
        g_win = NULL;
        SDL_Quit();
        return false;
    }
    SDL_GL_MakeCurrent(g_win, g_glc);
    SDL_GL_SetSwapInterval(g_game.vsync ? 1 : 0);

    if (fullscreen)
        Window_SetFullscreen(true);
        /* SDL_SetWindowFullscreen(g_win, SDL_WINDOW_FULLSCREEN_DESKTOP); */

    /* screenWidth/Height são a resolução LÓGICA, não o tamanho da janela:
     * várias telas desenham em cima delas (Font_DrawStringCentered com
     * screenWidth/2, o FPS em screenWidth - fw - 8, o centro do gameplay em
     * screenHeight/2). Gravar aqui o tamanho da janela jogava tudo isso para
     * fora do backbuffer de 640x480. O tamanho real da janela fica nos
     * estáticos abaixo, para quem precisar dele. */
    g_winW = width;
    g_winH = height;
    g_game.screenWidth  = LOGICAL_W;
    g_game.screenHeight = LOGICAL_H;
    g_game.isFullscreen = fullscreen;
    g_quitRequested = false;

    Window_UpdateViewport();

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0f);

    Log_Print("Window: created (janela %dx%d, backbuffer logico %dx%d, %s, GL %s)\n",
              g_winW, g_winH, LOGICAL_W, LOGICAL_H,
              fullscreen ? "fullscreen" : "windowed",
              glGetString(GL_VERSION) ? (const char*)glGetString(GL_VERSION) : "?");
    return true;
}

void Window_Destroy(void) {
    if (g_glc) {
        SDL_GL_DeleteContext(g_glc);
        g_glc = NULL;
    }
    if (g_win) {
        SDL_DestroyWindow(g_win);
        g_win = NULL;
    }
    SDL_Quit();
    Log_Print("Window: destroyed\n");
}

void Window_SwapBuffers(void) {
    if (g_win) SDL_GL_SwapWindow(g_win);
}

/* GRAPHICS SETTINGS — resoluções 4:3 da página do Service Menu. */
void Window_GetResolution(int idx, int* w, int* h) {
    static const int kRes[5][2] = { {640,480}, {800,600}, {1024,768}, {1280,960}, {1600,1200} };
    if (idx < 0 || idx > 4) idx = 2;
    *w = kRes[idx][0]; *h = kRes[idx][1];
}

/* Aplica tela cheia, tamanho da janela, vsync, proporção e filtro na hora. */
void Window_ApplyGraphics(void) {
    if (!g_win) return;
    /* SDL_SetWindowFullscreen(g_win, g_game.isFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0); */
    Window_SetFullscreen(g_game.isFullscreen);   /* exclusivo: RESOLUTION vira o modo de vídeo */
    if (!g_game.isFullscreen) {
        int w, h;
        Window_GetResolution(g_game.gfxResIdx, &w, &h);
        SDL_SetWindowSize(g_win, w, h);
        SDL_SetWindowPosition(g_win, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        g_winW = w; g_winH = h;
    }
    SDL_GL_SetSwapInterval(g_game.vsync ? 1 : 0);
    Window_UpdateViewport();
    Texture_ApplyFilterAll();
    Log_Print("Window: graficos fs=%d res=%d vsync=%d filtro=%d aspect=%d\n",
              (int)g_game.isFullscreen, g_game.gfxResIdx, (int)g_game.vsync,
              g_game.gfxTexFilter, g_game.gfxAspect);
}

void Window_ToggleFullscreen(void) {
    if (!g_win) return;
    g_game.isFullscreen = !g_game.isFullscreen;
    /* Uint32 fsflag = g_game.isFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
    SDL_SetWindowFullscreen(g_win, fsflag); */
    Window_SetFullscreen(g_game.isFullscreen);
    Window_UpdateViewport();
    Log_Print("Window: %s\n", g_game.isFullscreen ? "fullscreen" : "windowed");
}

/* Event dispatcher -> input.c keeps keyboard/focus state. Returns false when
 * the game should exit (window close, SDL_QUIT, or Window_RequestQuit). */
bool Window_ProcessMessages(void) {
    if (g_quitRequested) return false;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT) {
            g_quitRequested = true;
            return false;
        }
        if (ev.window.type == SDL_WINDOWEVENT) {
            switch (ev.window.event) {
            case SDL_WINDOWEVENT_SIZE_CHANGED:
            case SDL_WINDOWEVENT_RESIZED:
                Window_UpdateViewport();
                break;
            case SDL_WINDOWEVENT_FOCUS_GAINED:
                g_winActive = true;
                break;
            case SDL_WINDOWEVENT_FOCUS_LOST:
                g_winActive = false;
                break;
            case SDL_WINDOWEVENT_CLOSE:
                g_quitRequested = true;
                break;
            default:
                break;
            }
        }
        Input_ProcessEvent(&ev);
    }
    return true;
}
