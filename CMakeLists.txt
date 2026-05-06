cmake_minimum_required(VERSION 3.20)
project(VRAMOptimizer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Opções ───────────────────────────────────────────────────────────────────
option(VRAM_USE_NVAPI   "Habilitar suporte NVAPI (SDK Nvidia)"        OFF)
option(VRAM_USE_MINHOOK "Compilar DLL do hook DX11 via MinHook"       OFF)
option(VRAM_USE_ISPC    "Habilitar compressão SIMD via ispc_texcomp"  OFF)

# ─── Flags ────────────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /wd4100 /wd4996 /wd4505)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Estrutura de pastas (raiz do projeto):
#
#   compressor/   TextureCompressor.h / .cpp
#   hook/         HookDX11.h / .cpp / _Textures.cpp / _Compress.cpp
#   injector/     Injector.h / .cpp / injector_main.cpp
#   ipc/          IPCProtocol.h
#                 IPCServer.h / .cpp          ← hook ↔ orchestrator
#                 IPCStatusServer.h / .cpp    ← orchestrator ↔ UI C#
#   monitor/      VRAMMonitor.h / .cpp
#   orchestrator/ Orchestrator.h / .cpp
#   src/          main.cpp
#   ui/           VRAMOptimizer.UI.csproj     ← projeto C# separado
# ─────────────────────────────────────────────────────────────────────────────

set(SRC_MONITOR
    monitor/VRAMMonitor.h
    monitor/VRAMMonitor.cpp
)
set(SRC_ORCHESTRATOR
    orchestrator/Orchestrator.h
    orchestrator/Orchestrator.cpp
)
set(SRC_IPC
    ipc/IPCProtocol.h
    ipc/IPCServer.h
    ipc/IPCServer.cpp
    ipc/IPCStatusServer.h
    ipc/IPCStatusServer.cpp
)
set(SRC_COMPRESSOR
    compressor/TextureCompressor.h
    compressor/TextureCompressor.cpp
)
set(SRC_HOOK
    hook/HookDX11.h
    hook/HookDX11.cpp
    hook/HookDX11_Textures.cpp
    hook/HookDX11_Compress.cpp
)
set(SRC_INJECTOR
    injector/Injector.h
    injector/Injector.cpp
)

# ─────────────────────────────────────────────────────────────────────────────
# [1] vram_optimizer.exe — Orquestrador principal
#     Monitor + Orchestrator + IPCServer (hook) + IPCStatusServer (UI)
# ─────────────────────────────────────────────────────────────────────────────
add_executable(vram_optimizer
    src/main.cpp
    ${SRC_MONITOR}
    ${SRC_ORCHESTRATOR}
    ${SRC_IPC}
    ${SRC_COMPRESSOR}
)
target_include_directories(vram_optimizer PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(vram_optimizer PRIVATE dxgi d3d11 gdi32)

# ─────────────────────────────────────────────────────────────────────────────
# [2] vram_injector.exe — CLI de injeção (sem deps extras)
# ─────────────────────────────────────────────────────────────────────────────
add_executable(vram_injector
    injector/injector_main.cpp
    ${SRC_INJECTOR}
)
target_include_directories(vram_injector PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(vram_injector PRIVATE psapi)

# ─────────────────────────────────────────────────────────────────────────────
# [3] vram_hook.dll — Hook DX11 injetado no jogo
#     Habilitado com: cmake .. -DVRAM_USE_MINHOOK=ON
#
#     Estrutura esperada:
#       third_party/minhook/include/MinHook.h
#       third_party/minhook/lib/MinHook.x64.lib
#     Download: https://github.com/TsudaKageyu/minhook/releases
# ─────────────────────────────────────────────────────────────────────────────
if(VRAM_USE_MINHOOK)
    set(MINHOOK_DIR "${CMAKE_SOURCE_DIR}/third_party/minhook"
        CACHE PATH "Raiz do MinHook SDK")

    add_library(vram_hook SHARED
        ${SRC_HOOK}
        ${SRC_IPC}
        ${SRC_COMPRESSOR}
    )
    target_include_directories(vram_hook PRIVATE
        ${CMAKE_SOURCE_DIR}
        "${MINHOOK_DIR}/include"
    )
    target_link_libraries(vram_hook PRIVATE
        dxgi d3d11 gdi32
        "${MINHOOK_DIR}/lib/MinHook.x64.lib"
    )
    target_compile_definitions(vram_hook PRIVATE VRAM_HOOK_BUILD=1)

    # Copia a DLL para junto dos executáveis
    add_custom_command(TARGET vram_hook POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            $<TARGET_FILE:vram_hook>
            $<TARGET_FILE_DIR:vram_optimizer>/vram_hook.dll
        COMMENT "Copiando vram_hook.dll -> output"
    )
    message(STATUS "Hook DLL habilitada | MinHook: ${MINHOOK_DIR}")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# NVAPI (opcional)
# cmake .. -DVRAM_USE_NVAPI=ON
# set NVAPI_SDK_DIR=C:\path\nvapi-sdk
# Download: https://developer.nvidia.com/nvapi
# ─────────────────────────────────────────────────────────────────────────────
if(VRAM_USE_NVAPI)
    set(NVAPI_SDK_DIR "$ENV{NVAPI_SDK_DIR}" CACHE PATH "Raiz do NVAPI SDK")
    foreach(TGT vram_optimizer vram_hook)
        if(TARGET ${TGT})
            target_compile_definitions(${TGT} PRIVATE VRAM_USE_NVAPI=1)
            target_include_directories(${TGT} PRIVATE "${NVAPI_SDK_DIR}/include")
            target_link_directories   (${TGT} PRIVATE "${NVAPI_SDK_DIR}/amd64")
            target_link_libraries     (${TGT} PRIVATE nvapi64)
        endif()
    endforeach()
    message(STATUS "NVAPI habilitado | SDK: ${NVAPI_SDK_DIR}")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# ispc_texcomp (opcional) — BC1/BC3/BC7 com SIMD
# cmake .. -DVRAM_USE_ISPC=ON
# Estrutura:
#   third_party/ispc_texcomp/include/ispc_texcomp.h
#   third_party/ispc_texcomp/lib/ispc_texcomp.lib
# Download: https://github.com/GameTechDev/ISPCTextureCompressor
# ─────────────────────────────────────────────────────────────────────────────
if(VRAM_USE_ISPC)
    set(ISPC_DIR "${CMAKE_SOURCE_DIR}/third_party/ispc_texcomp"
        CACHE PATH "Raiz do ispc_texcomp")
    foreach(TGT vram_optimizer vram_hook)
        if(TARGET ${TGT})
            target_compile_definitions(${TGT} PRIVATE VRAM_USE_ISPC=1)
            target_include_directories(${TGT} PRIVATE "${ISPC_DIR}/include")
            target_link_libraries     (${TGT} PRIVATE "${ISPC_DIR}/lib/ispc_texcomp.lib")
        endif()
    endforeach()
    message(STATUS "ispc_texcomp habilitado | dir: ${ISPC_DIR}")
endif()

# ─── Copia executáveis para a mesma pasta (facilita testar junto) ─────────────
add_custom_command(TARGET vram_injector POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:vram_injector>
        $<TARGET_FILE_DIR:vram_optimizer>/vram_injector.exe
    COMMENT "Copiando vram_injector.exe -> output"
)

# ─── Sumário ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "========== VRAM Optimizer — Build ==========")
message(STATUS "  Compilador    : ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  C++ padrão    : C++${CMAKE_CXX_STANDARD}")
message(STATUS "  NVAPI         : ${VRAM_USE_NVAPI}")
message(STATUS "  Hook DLL      : ${VRAM_USE_MINHOOK}")
message(STATUS "  ispc_texcomp  : ${VRAM_USE_ISPC}")
message(STATUS "")
message(STATUS "Targets:")
message(STATUS "  vram_optimizer.exe  — Orquestrador + Monitor + IPC")
message(STATUS "  vram_injector.exe   — CLI de injeção da DLL")
if(VRAM_USE_MINHOOK)
message(STATUS "  vram_hook.dll       — Hook DX11 (injetar no jogo)")
endif()
message(STATUS "")
message(STATUS "Compilar modo standalone (sem deps extras):")
message(STATUS "  cmake .. -G \"Visual Studio 17 2022\" -A x64")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")
message(STATUS "Compilar com hook + NVAPI:")
message(STATUS "  cmake .. -DVRAM_USE_MINHOOK=ON -DVRAM_USE_NVAPI=ON")
message(STATUS "  cmake --build . --config Release")
message(STATUS "============================================")
message(STATUS "")