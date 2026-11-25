# SotC-Android

Port do [Shadow of the Colossus — PC](https://github.com/AjncSystem/ShadowOfTheColossus-PC)
(PS4 CUSA08809/CUSA08034) para Android ARM64. Aparelho de referência: AYN Odin 3.
Arquitetura e etapas em [docs/PLANO.md](docs/PLANO.md).

**Estado: M4 em andamento.** O jogo roda no Odin 3 pelo runtime e pela biblioteca de vídeo do
SotC-PC sobre o FEXCore: inicializa, carrega o mundo e **mostra a abertura com texturas**, a 15–30
FPS, sem as quedas do driver de vídeo dos testes anteriores (o cache de buffers usa memória própria
em vez de memória esparsa no Adreno; detalhes em `docs/PLANO.md`). Relatórios das etapas em `docs/`.

## Launcher

O app abre num launcher, como o do SotC-PC, feito para o controle do Odin 3 (direcional, A
confirma, B volta):

- **Jogar**: verifica a permissão, o jogo, a imagem preparada e o driver; escolhe o modo
  (Normal, Pro, Extremo, Personalizado); botão JOGAR.
- **Jogo e saves**: pasta do jogo (seletor de pastas ou busca automática em `PlayStation 4`),
  pasta `out` do SotC-PC, nome do jogador e idioma.
- **Imagem e FPS**: 30/60/ilimitado e FPS dinâmico, PS4 Pro, texturas, filtro 16x, FSR 1, nitidez,
  memória de vídeo, apresentação, mostrar FPS, efeitos (vinheta, névoa, SSAO, motion blur),
  pular logos e filme.
- **Controles e som**: vibração, volume e canais.
- **Driver de vídeo**: os pacotes adrenotools embutidos, em `Download` (até três pastas abaixo) e
  em `files/drivers`; importar um `.zip`; opções `TU_DEBUG` do Turnip.
- **Avançado**: cvars do motor, variáveis de ambiente, leituras da GPU, TSO do FEX, autoteste,
  menu de depuração, limpar o cache de shaders e o relatório da última sessão.

As escolhas viram o ambiente do jogo (`SOTC_MODES`, `SOTC_CVARS`, `SOTC_FPS_*`, `SOTC_LOD_*`,
`BB_FSR1`...), como no PC. Os patches do `scripts/sotc_modes.py` (fps60game, grassbuf, af16,
correção do alocador de páginas) e a linha de comando do motor são aplicados na memória ao
carregar. O jogo roda num processo próprio (`:game`): fechar o jogo libera toda a memória dele.

## Testar no aparelho

1. O jogo (o **seu** dump, `CUSA08809` ou `CUSA08034`, com `sce_sys/param.sfo`) fica onde está,
   por exemplo `SD/PlayStation 4/Shadow Of The Colossus/CUSA08809`. Uma atualização em
   `CUSA08809-patch`, ao lado, é aplicada por cima. Escolha a pasta em **Jogo e saves**.
2. O launcher pede "Acesso a todos os arquivos" para ler o jogo fora da pasta do app.
3. A imagem preparada pelo SotC-PC (a pasta `out` com `boot-linked.bin` e `content.bin`) pode
   ficar em qualquer pasta (escolha em **Jogo e saves**) ou na pasta do app:

```powershell
adb push "E:\GAMES\Shadow Of The Colossus\SotC-PC\out\boot-linked.bin" /sdcard/Android/data/io.github.ajncsystem.sotc/files/game/boot-linked.bin
```

```powershell
adb push "E:\GAMES\Shadow Of The Colossus\SotC-PC\out\content.bin" /sdcard/Android/data/io.github.ajncsystem.sotc/files/game/content.bin
```

O resultado vai para o logcat (tags `SotC` e `SotC-RT`) e para `files/m3-report.txt` (botão
"Ver o relatório da última sessão" no launcher).

Este projeto não contém o jogo, firmware nem chaves da Sony.
## Compilar

Clone com os submódulos de que o build precisa (não use `--recursive`: o FEX traz binários de
teste que o build não usa):

```powershell
git submodule update --init
```

```powershell
git -C externals/FEX submodule update --init --depth 1 External/unordered_dense External/rpmalloc External/xxhash External/fmt External/range-v3 Source/Common/cpp-optparse
```

```powershell
git -C externals/libadrenotools submodule update --init; git -C externals/zydis submodule update --init
```

Requisitos: Android SDK com NDK 28.2.13676358 e CMake 3.31.5, JDK 17+.
O `build.ps1` usa por padrão o SDK e o cache do Gradle em `E:\Hokkaido Fork`
(`ANDROID_HOME`, `GRADLE_USER_HOME` e `JAVA_HOME` mudam isso).

```powershell
.\build.ps1            # app\build\outputs\apk\debug\app-debug.apk
.\build.ps1 -Install   # instala no aparelho, abre e segue o logcat (tag SotC)
```

Dependências em `externals/`: SDL 3.4.20, libadrenotools e FEX (revisão
`f2b679f6028ce1c38875233aecfcf5d3f8ebecec`, com os submódulos `External/unordered_dense`,
`rpmalloc`, `xxhash`, `fmt`, `range-v3` e `Source/Common/cpp-optparse`). O `build.ps1` aplica
os patches de `patches/fex/` e precisa do `gzip` do Git for Windows e de um `python3` no PATH.

A biblioteca de vídeo (`app/src/main/cpp/gpu/`, do SotC-PC) também usa `externals/ext-boost`,
`externals/zydis` (v4.1.1), `externals/Vulkan-Headers` (v1.4.363), `externals/VulkanMemoryAllocator`
(v3.4.0) e `externals/robin-map` (v1.4.1), as versões com que o SotC-PC compila, e o
`glslangValidator` do MSYS2 CLANG64 para os shaders do host (`I:\msys64\clang64\bin`, ou
`SOTC_GLSLANG_DIR`). O runtime usa `externals/third_party/LibAtrac9`.

## Driver de vídeo

O APK embute os pacotes adrenotools que estiverem em `drivers/*.zip`. O padrão é
`drivers/Turnip_R3_barycentric_SotC.zip`: o Turnip de StevenMXZ (Mesa `main` + patches do Adreno
8xx) com `VK_KHR_fragment_shader_barycentric` (`patches/turnip/README.md` diz como gerar). O port
ainda usa o geometry shader das baricêntricas no Turnip: pela extensão o mato e as sombras saem
errados (`SOTC_TU_KHR_BARYCENTRIC=1` liga para testar). O Turnip Gen8 V38 (`drivers/Turnip_Gen8_V38.zip`)
continua escolhível. Os pacotes não estão no git: gere ou copie o seu para `drivers/` antes de
compilar; sem o padrão o app usa o driver Qualcomm do sistema. Outros pacotes (.zip com `meta.json`) aparecem na
página **Driver de vídeo** do launcher quando estão em `Download` ou são importados por ela.

## Licença

GPL-2.0-or-later (como o SotC-PC). SDL3: zlib. libadrenotools: BSD-2-Clause. Turnip/Mesa: MIT.
