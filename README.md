# SotC-Android

Port do [Shadow of the Colossus — PC](https://github.com/AjncSystem/ShadowOfTheColossus-PC)
(PS4 CUSA08809/CUSA08034) para Android ARM64. Aparelho de referência: AYN Odin 3.
Arquitetura e etapas em [docs/PLANO.md](docs/PLANO.md).

**Estado: M4 em andamento** — o APK abre, carrega o driver Turnip Gen8 V38 pelo libadrenotools, cria a
superfície Vulkan e lê o controle (M0). O FEXCore roda código x86-64 dentro do app e o liga a
funções nativas (M1). O carregador mapeia a imagem do jogo preparada pelo SotC-PC e roda o
inicializador da libc.prx até o primeiro import sem implementação (M2). Os relatórios ficam em
`docs/`. O jogo ainda não roda.

O runtime e a biblioteca de vídeo do SotC-PC rodam no Odin 3: o jogo inicializa, carrega o mundo e
**desenha a abertura da tela de título** a 20–30 FPS, mas o processo termina depois de 8 a 50
segundos (M4, em andamento; detalhes em `docs/PLANO.md`).

## Testar no aparelho

1. O jogo (o **seu** dump, `CUSA08809` ou `CUSA08034`, com `sce_sys/param.sfo`) fica numa pasta
   `PlayStation 4` de qualquer volume, até dois níveis abaixo, por exemplo
   `SD/PlayStation 4/Shadow Of The Colossus/CUSA08809`. Uma atualização em `CUSA08809-patch`, ao
   lado, é aplicada por cima. Outra pasta pode ser indicada em `files/game/game_dir.txt`.
2. Na primeira abertura, o app pede "Acesso a todos os arquivos" para ler o jogo fora da pasta
   dele.
3. A imagem preparada pelo SotC-PC vai para a pasta do app:

```powershell
adb push "E:\GAMES\Shadow Of The Colossus\SotC-PC\out\boot-linked.bin" /sdcard/Android/data/io.github.ajncsystem.sotc/files/game/boot-linked.bin
```

```powershell
adb push "E:\GAMES\Shadow Of The Colossus\SotC-PC\out\content.bin" /sdcard/Android/data/io.github.ajncsystem.sotc/files/game/content.bin
```

O resultado vai para o logcat (tags `SotC` e `SotC-RT`) e para `files/m3-report.txt`.

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

O APK embute o pacote adrenotools que estiver em `drivers/Turnip_Gen8_V38.zip` (Turnip Gen8 V38,
Mesa 27.0.0-1, de StevenMXZ). Ele não está no git: copie o seu para `drivers/` antes de compilar;
sem ele o app usa o driver Qualcomm do sistema. Para testar outro, sem recompilar:

```powershell
adb push Outro_Driver.zip /sdcard/Android/data/io.github.ajncsystem.sotc/files/drivers/
adb shell "echo Outro_Driver.zip > /sdcard/Android/data/io.github.ajncsystem.sotc/files/drivers/selected.txt"
```

`system` no `selected.txt` usa o driver Qualcomm do sistema. O relatório da última execução fica
em `/sdcard/Android/data/io.github.ajncsystem.sotc/files/m0-report.txt`.

## Licença

GPL-2.0-or-later (como o SotC-PC). SDL3: zlib. libadrenotools: BSD-2-Clause. Turnip/Mesa: MIT.
