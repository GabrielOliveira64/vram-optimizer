# VRAM Optimizer

Compressor automático de texturas para jogos em tempo real.
Arquitetura modular em C++ — interface C# planejada para fase final.

## Estrutura atual

```
vram_optimizer/
├── CMakeLists.txt
└── src/
    ├── main.cpp                          ← Entrada: modo standalone ou completo
    │
    ├── monitor/
    │   ├── VRAMMonitor.h / .cpp          ← Módulo 1: leitura de VRAM
    │
    ├── orchestrator/
    │   ├── Orchestrator.h / .cpp         ← Módulo 2: ações em cascata
    │
    ├── ipc/
    │   ├── IPCProtocol.h                 ← Protocolo de mensagens (POD structs)
    │   ├── IPCServer.h / .cpp            ← Servidor Named Pipe (lado orquestrador)
    │
    ├── hook/
    │   ├── HookDX11.h                    ← Módulo 3: interface do hook
    │   ├── HookDX11.cpp                  ← Setup, vtable hook, pipes, log
    │   ├── HookDX11_Textures.cpp         ← Interceptação CreateTexture2D
    │   └── HookDX11_Compress.cpp         ← Compressão real + PSSetShaderResources
    │
    └── compressor/
        ├── TextureCompressor.h / .cpp    ← Módulo 4: BC1/BC3/BC7 + downscale
```

## Roadmap

| Fase | Módulo              | Status | Descrição                                     |
|------|---------------------|--------|-----------------------------------------------|
| 1    | VRAMMonitor         | ✅     | NVAPI / D3DKMT / DXGI — detecta automático    |
| 2    | Orchestrator        | ✅     | Ações em cascata, thread-safe, callbacks       |
| 3    | HookDX11 + IPC      | ✅     | Hook vtable, Named Pipe bidirecional           |
| 4    | TextureCompressor   | ✅     | BC1/BC3 software + downscale bilinear          |
| 4b   | ispc_texcomp        | 🔲     | Compressão SIMD (10x mais rápido que software) |
| 5    | Injector            | 🔲     | Injeta vram_hook.dll no processo do jogo       |
| 6    | Interface C#        | 🔲     | WPF/WinUI — gráfico VRAM + lista de texturas  |

---

## Como testar AGORA (modo standalone)

Teste o monitor + orquestrador sem precisar do hook ou de nenhum jogo.
Funciona em qualquer PC com Windows 10+ e placa de vídeo com DXGI.

### 1. Requisitos
- Visual Studio 2022 (ou Build Tools com MSVC)
- Windows SDK 10.0+
- CMake 3.20+ (ou use o comando cl direto abaixo)

### 2. Compilar com CMake
```bat
cd vram_optimizer
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

### 3. Compilar direto (sem CMake — mais rápido para testar)
Abra o "Developer Command Prompt for VS 2022" e execute:
```bat
cd vram_optimizer\src
cl /std:c++17 /EHsc /O2 /W3 ^
   main.cpp ^
   monitor\VRAMMonitor.cpp ^
   orchestrator\Orchestrator.cpp ^
   ipc\IPCServer.cpp ^
   compressor\TextureCompressor.cpp ^
   /I . ^
   /link dxgi.lib d3d11.lib gdi32.lib ^
   /OUT:..\vram_optimizer.exe
```

### 4. Rodar em modo standalone (sem jogo/hook)
```bat
vram_optimizer.exe --standalone
```

Você verá no CMD:
```
==========================================================
  VRAM Optimizer  |  Modo Standalone (sem hook DX11)
==========================================================

Backend detectado : DXGI 1.4
VRAM total        : 2048 MB
Texturas carregadas: 11

Ctrl+C para parar.
----------------------------------------------------------

[====............................................]  42.3% [OK]         433/2048 MB  tex:0/11  livre:1615 MB
```

---

## Como usar o modo completo (com hook no jogo)

### 1. Compile a DLL do hook
```bat
cmake .. -DVRAM_USE_MINHOOK=ON -DMINHOOK_DIR=C:\libs\minhook
cmake --build . --config Release
```
Isso gera `vram_hook.dll` junto com o executável.

### 2. Inicie o orquestrador primeiro
```bat
vram_optimizer.exe
```
Ele abrirá os Named Pipes e ficará aguardando o hook conectar.

### 3. Inicie o jogo e injete a DLL
Use qualquer injector de DLL (ex: Extreme Injector, dll_injector):
- Processo: o jogo que você quer otimizar
- DLL: `vram_hook.dll`

O hook conecta automaticamente ao orquestrador via Named Pipe.

---

## Backends de VRAM

| Backend  | GPU           | Precisão | Requisito               |
|----------|---------------|----------|-------------------------|
| NVAPI    | Nvidia        | Máxima   | NVAPI SDK (opcional)    |
| D3DKMT   | Nvidia/AMD/Intel | Alta  | Windows SDK (incluso)   |
| DXGI 1.4 | Qualquer      | Boa      | Windows 10+ (padrão)    |
| DXGI 1.0 | Qualquer      | Básica   | Fallback automático     |

## Níveis de compressão

| Nível      | Economia  | Qualidade | Velocidade (software) |
|------------|-----------|-----------|-----------------------|
| BC1        | ~75%      | Boa       | ~50ms por 1024×1024   |
| BC3        | ~75%      | Boa+alpha | ~80ms por 1024×1024   |
| BC7        | ~75%      | Ótima     | Requer ispc_texcomp   |
| HalfRes+BC1| ~93%      | Aceitável | ~30ms (menor res)     |
| QuarterRes | ~98%      | Baixa     | Emergência apenas     |

Com ispc_texcomp (SIMD): BC1 fica ~5ms por 1024×1024 (10x mais rápido).
