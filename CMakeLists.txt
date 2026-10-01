cmake_minimum_required(VERSION 3.10)

# CMAKE_MSVC_RUNTIME_LIBRARY (runtime estático no build de release) precisa
# da política CMP0091 antes do project().
if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(Pumpy C)

# Triplet estático do vcpkg (ex.: x86-windows-static): SDL2, zlib e o runtime
# do C entram no Pumpy.exe (/MT) -> o release não precisa de nenhuma DLL.
if(MSVC AND VCPKG_TARGET_TRIPLET MATCHES "static")
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
endif()

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Copia o executável para a pasta do jogo depois de compilar (opcional):
#   cmake -DPUMPY_GAME_DIR="/caminho/Pump It Up" ..
set(PUMPY_GAME_DIR "" CACHE PATH "Pasta do jogo: o Pumpy é copiado para lá após o build")

# ---------------------------------------------------------------------------
# Dependências: SDL2 (janela/entrada/áudio), OpenGL + GLU, zlib.
#   Linux  : apt install libsdl2-dev libgl-dev libglu1-mesa-dev zlib1g-dev
#   Windows: vcpkg install sdl2 zlib   (opengl32/glu32 já vêm com o Windows)
#            cmake -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake ..
# ---------------------------------------------------------------------------
add_library(pumpy_deps INTERFACE)

find_package(SDL2 CONFIG QUIET)
find_package(OpenGL QUIET)
find_package(ZLIB QUIET)

set(_have_sdl2 FALSE)
if(TARGET SDL2::SDL2)
    target_link_libraries(pumpy_deps INTERFACE SDL2::SDL2)
    set(_have_sdl2 TRUE)
elseif(TARGET SDL2::SDL2-static)
    target_link_libraries(pumpy_deps INTERFACE SDL2::SDL2-static)
    set(_have_sdl2 TRUE)
elseif(SDL2_FOUND AND SDL2_LIBRARIES)
    target_include_directories(pumpy_deps INTERFACE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(pumpy_deps INTERFACE ${SDL2_LIBRARIES})
    set(_have_sdl2 TRUE)
endif()

set(_have_gl FALSE)
if(OpenGL_FOUND AND TARGET OpenGL::GL AND TARGET OpenGL::GLU)
    target_link_libraries(pumpy_deps INTERFACE OpenGL::GL OpenGL::GLU)
    set(_have_gl TRUE)
endif()

set(_have_zlib FALSE)
if(TARGET ZLIB::ZLIB)
    target_link_libraries(pumpy_deps INTERFACE ZLIB::ZLIB)
    set(_have_zlib TRUE)
endif()

# Fallback só para Linux sem pacotes -dev: usa os headers vendorizados em
# third_party/linux e linka as bibliotecas de runtime (.so.N) direto.
if(NOT (_have_sdl2 AND _have_gl AND _have_zlib))
    if(UNIX AND NOT APPLE)
        message(WARNING "Pacotes -dev de SDL2/GL/GLU/zlib não encontrados: usando headers "
                        "vendorizados (third_party/linux) e as bibliotecas de runtime.")
        set(_libdirs /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu /lib64 /usr/lib64 /usr/lib)
        target_include_directories(pumpy_deps INTERFACE
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/linux/include
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/linux/include/SDL2)
        foreach(_spec "SDL2;SDL2-2.0;libSDL2-2.0.so.0" "GL;GL;libGL.so.1" "GLU;GLU;libGLU.so.1" "ZLIB;z;libz.so.1")
            list(GET _spec 0 _id)
            list(GET _spec 1 _n1)
            list(GET _spec 2 _n2)
            find_library(PUMPY_LIB_${_id} NAMES ${_n1} ${_n2} PATHS ${_libdirs})
            if(NOT PUMPY_LIB_${_id})
                message(FATAL_ERROR "Biblioteca ${_n1} não encontrada. Instale os pacotes -dev "
                                    "(libsdl2-dev libgl-dev libglu1-mesa-dev zlib1g-dev).")
            endif()
            target_link_libraries(pumpy_deps INTERFACE ${PUMPY_LIB_${_id}})
        endforeach()
    else()
        message(FATAL_ERROR "Faltam dependências (SDL2 / OpenGL+GLU / zlib). No Windows use vcpkg: "
                            "vcpkg install sdl2 zlib, e passe o toolchain file do vcpkg ao CMake.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Fontes. O código é o mesmo em todas as plataformas (SDL2 + OpenGL); só os
# poucos helpers Win32 usados pelo jogo são reimplementados fora do Windows.
# ---------------------------------------------------------------------------
set(PLATFORM_SOURCES "")
if(NOT WIN32)
    list(APPEND PLATFORM_SOURCES src/platform_posix.c)
endif()

# DESATIVADO no Exceed: o exceed.exe le WAVE/ do disco, nada e embutido.
# WAVs embutidos no executável (o PUMPY.EXE original os guarda como recursos).
# tools/embed_waves.py converte WAVE/*.wav num .c com arrays de bytes.
# find_package(Python3 REQUIRED COMPONENTS Interpreter)
# file(GLOB WAVE_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/WAVE/*.wav")
# set(WAVE_EMBEDDED_C "${CMAKE_CURRENT_BINARY_DIR}/generated/wave_embedded.c")
# add_custom_command(
#     OUTPUT "${WAVE_EMBEDDED_C}"
#     COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_waves.py"
#             "${CMAKE_CURRENT_SOURCE_DIR}/WAVE" "${WAVE_EMBEDDED_C}"
#     DEPENDS ${WAVE_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_waves.py"
#     COMMENT "Embutindo WAVE/*.wav"
#     VERBATIM)

add_executable(Pumpy
    src/main.c
    src/window.c
    src/warning.c
    src/logo.c
    src/menu.c
    src/song_select.c
    src/staff.c
    src/movie.c
    src/intro.c
    src/highscore.c
    src/ir.c
    src/ir_password.c
    src/ir_mixtable.c
    src/nameinput.c
    src/exceed_select.c
    src/station.c
    src/eeprom_x2.c
    # src/exceed_songs.c   (Exceed: tabelas do exceed.exe)
    src/exceed2_songs.c
    src/service_menu.c
    src/coin.c
    src/debug_console.c
    src/ranking.c
    src/result.c
    src/loading.c
    src/gameplay.c
    src/game_option.c
    src/eeprom.c
    src/render.c
    src/texture.c
    src/audio.c
    src/input.c
    src/font.c
    src/bga.c
    src/vsl.c
    src/df_resource.c
    src/util.c
    src/resource.c
    src/step.c
    src/song.c
    src/zlibinflate.c
    # src/xbrz.c        # UPSCALE (xBRZ) das texturas — DESATIVADO (30/09/2026)
    ${PLATFORM_SOURCES}
    # ${WAVE_EMBEDDED_C}   (desativado no Exceed)
)

target_include_directories(Pumpy PRIVATE include)

target_compile_definitions(Pumpy PRIVATE
    _CRT_SECURE_NO_WARNINGS
    SDL_MAIN_HANDLED          # main() próprio, sem SDL2main (ver window.c: SDL_SetMainReady)
)

target_link_libraries(Pumpy PRIVATE pumpy_deps)

if(MSVC)
    target_compile_options(Pumpy PRIVATE /W3)
    target_link_options(Pumpy PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
    # 4 GB de endereço no Win32 rodando em Windows 64-bit (UPSCALE xBRZ ocupa muita textura)
    target_link_options(Pumpy PRIVATE /LARGEADDRESSAWARE)
elseif(WIN32)
    target_link_options(Pumpy PRIVATE -mwindows)
endif()

if(WIN32)
    target_link_libraries(Pumpy PRIVATE winmm)   # timeGetTime / timeBeginPeriod
    target_sources(Pumpy PRIVATE src/icon.rc)    # ícone do .exe (src/icon.ico, gerado de icon.png)
else()
    target_link_libraries(Pumpy PRIVATE m pthread dl)
endif()

# Windows: leva a SDL2.dll junto (vcpkg/SDL2 dev com DLL).
if(WIN32 AND TARGET SDL2::SDL2)
    add_custom_command(TARGET Pumpy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:Pumpy>
        VERBATIM)
endif()

if(PUMPY_GAME_DIR)
    add_custom_command(TARGET Pumpy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:Pumpy> "${PUMPY_GAME_DIR}"
        COMMENT "Copiando Pumpy para ${PUMPY_GAME_DIR}"
        VERBATIM)
endif()

# Assets (BGA, AUDIO, STEP, Stage.cfg, WAVE) são fornecidos pelo usuário com
# os arquivos originais do jogo; o executável espera o diretório de trabalho
# apontando para a pasta do jogo.
