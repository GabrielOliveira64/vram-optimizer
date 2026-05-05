cmake_minimum_required(VERSION 3.20)
project(VRAMOptimizer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Opções ───────────────────────────────────────────────────────────────────
option(VRAM_USE_NVAPI    "Habilitar suporte NVAPI (SDK Nvidia)"         OFF)
option(VRAM_USE_MINHOOK  "Habilitar hook DX11 via MinHook (para DLL)"   OFF)
option(VRAM_USE_ISPC     "Habilitar compressão real via ispc_texcomp"   OFF)

# ─── Warnings ────────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /wd4100 /wd4996 /wd4505)
    # Necessário para _CRT_SECURE_NO_WARNINGS (sprintf, fopen etc.)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ─── Fontes por módulo ────────────────────────────────────────────────────────
set(SRC_MONITOR
    src/monitor/VRAMMonitor.h
    src/monitor/VRAMMonitor.cpp
)

set(SRC_ORCHESTRATOR
    src/orchestrator/Orchestrator.h
    src/orchestrator/Orchestrator.cpp
)

set(SRC_IPC
    src/ipc/IPCProtocol.h
    src/ipc/IPCServer.h
    src/ipc/IPCServer.cpp
)

set(SRC_COMPRESSOR
    src/compressor/TextureCompressor.h
    src/compressor/TextureCompressor.cpp
)

set(SRC_HOOK
    src/hook/HookDX11.h
    src/hook/HookDX11.cpp
    src/hook/HookDX11_Textures.cpp
)

# ─── Executável principal (Orchestrator + Monitor + IPC) ─────────────────────
add_executable(vram_optimizer
    src/main.cpp
    ${SRC_MONITOR}
    ${SRC_ORCHESTRATOR}
    ${SRC_IPC}
    ${SRC_COMPRESSOR}
)

target_include_directories(vram_optimizer PRIVATE src)

target_link_libraries(vram_optimizer PRIVATE
    dxgi
    d3d11
    gdi32
)

# ─── DLL do Hook (Módulo 3 — injetada no processo do jogo) ───────────────────
if(VRAM_USE_MINHOOK)
    # Caminho do MinHook SDK — ajuste conforme sua instalação
    # Download: https://github.com/TsudaKageyu/minhook/releases
    set(MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/minhook"
        CACHE PATH "Diretório raiz do MinHook SDK")

    add_library(vram_hook SHARED
        ${SRC_HOOK}
        ${SRC_IPC}
        ${SRC_COMPRESSOR}
    )

    target_include_directories(vram_hook PRIVATE
        src
        "${MINHOOK_DIR}/include"
    )

    target_link_libraries(vram_hook PRIVATE
        dxgi
        d3d11
        gdi32
        "${MINHOOK_DIR}/lib/MinHook.x64.lib"
    )

    target_compile_definitions(vram_hook PRIVATE VRAM_HOOK_BUILD=1)

    # A DLL deve ser copiada para o diretório de saída junto com o executável
    add_custom_command(TARGET vram_hook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vram_hook>
            $<TARGET_FILE_DIR:vram_optimizer>/vram_hook.dll
        COMMENT "Copiando vram_hook.dll para diretório de saída")

    message(STATUS "Hook DLL habilitada (MinHook: ${MINHOOK_DIR})")
endif()

# ─── NVAPI (opcional, GPUs Nvidia) ───────────────────────────────────────────
if(VRAM_USE_NVAPI)
    set(NVAPI_SDK_DIR "$ENV{NVAPI_SDK_DIR}"
        CACHE PATH "Diretório do NVAPI SDK")

    target_compile_definitions(vram_optimizer PRIVATE VRAM_USE_NVAPI=1)
    target_include_directories(vram_optimizer PRIVATE "${NVAPI_SDK_DIR}/include")
    target_link_directories   (vram_optimizer PRIVATE "${NVAPI_SDK_DIR}/amd64")
    target_link_libraries     (vram_optimizer PRIVATE nvapi64)

    if(VRAM_USE_MINHOOK)
        target_compile_definitions(vram_hook PRIVATE VRAM_USE_NVAPI=1)
        target_include_directories(vram_hook PRIVATE "${NVAPI_SDK_DIR}/include")
        target_link_directories   (vram_hook PRIVATE "${NVAPI_SDK_DIR}/amd64")
        target_link_libraries     (vram_hook PRIVATE nvapi64)
    endif()

    message(STATUS "NVAPI habilitado (SDK: ${NVAPI_SDK_DIR})")
endif()

# ─── ispc_texcomp (compressão real de texturas) ───────────────────────────────
if(VRAM_USE_ISPC)
    set(ISPC_DIR "${CMAKE_SOURCE_DIR}/third_party/ispc_texcomp"
        CACHE PATH "Diretório do ispc_texcomp")

    target_compile_definitions(vram_optimizer PRIVATE VRAM_USE_ISPC=1)
    target_include_directories(vram_optimizer PRIVATE "${ISPC_DIR}/include")
    target_link_libraries     (vram_optimizer PRIVATE "${ISPC_DIR}/lib/ispc_texcomp.lib")

    if(VRAM_USE_MINHOOK)
        target_compile_definitions(vram_hook PRIVATE VRAM_USE_ISPC=1)
        target_include_directories(vram_hook PRIVATE "${ISPC_DIR}/include")
        target_link_libraries     (vram_hook PRIVATE "${ISPC_DIR}/lib/ispc_texcomp.lib")
    endif()

    message(STATUS "ispc_texcomp habilitado (dir: ${ISPC_DIR})")
endif()

# ─── Sumário ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "=== VRAM Optimizer Build ===")
message(STATUS "  Compilador  : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  C++ padrão  : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  NVAPI       : ${VRAM_USE_NVAPI}")
message(STATUS "  MinHook DLL : ${VRAM_USE_MINHOOK}")
message(STATUS "  ispc_texcomp: ${VRAM_USE_ISPC}")
message(STATUS "")
