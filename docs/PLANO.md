# SotC-Android — plano

Port do [SotC-PC](https://github.com/AjncSystem/ShadowOfTheColossus-PC) para Android ARM64, com o
AYN Odin 3 (Snapdragon 8 Elite, Adreno 830) como aparelho de referência. É um APK próprio, não um
fork do Bachata-S4.

## Por que não é só recompilar

No PC, o código x86-64 do jogo roda direto na CPU. No Odin 3 a CPU é ARM64, então o código do
jogo passa por um tradutor x86-64 → ARM64: o **FEXCore**, usado como biblioteca. Todo o resto é
compilado nativo em ARM64 com o NDK: o carregador, o runtime (`runtime_*.c`), o núcleo de vídeo
(shadPS4 + shim) e o som. Quando o jogo chama uma função do sistema, a chamada sai do código
traduzido e vai para a função nativa por uma ponte (HLE thunk).

```
 código do jogo (x86-64)  ──FEXCore JIT──▶ ARM64
        │ import (NID)
        ▼
 thunk HLE ──▶ runtime nativo ARM64 (kernel, memória, threads, arquivos, pad, áudio...)
                    │
                    ▼
            núcleo de vídeo shadPS4 (GCN → SPIR-V) ──▶ Vulkan ──▶ Turnip (adrenotools)
```

## Restrições do aparelho (medidas no Odin 3)

- Android 15, kernel 6.6, páginas de 4 KiB, 12 GB de RAM.
- **Espaço de endereços de 39 bits (512 GiB).** O SotC-PC reserva a faixa de usuário do PS4 até
  `0xfc00000000` (~1 TiB). No Android a reserva precisa diminuir para o que o jogo usa de fato.
- O FEX traduz endereços 1:1 (a memória do jogo é memória do processo), então a imagem do jogo
  continua abaixo de 2 GiB (ponteiros de 31 bits) e a faixa do PS4 tem que caber livre no processo.
- Driver de vídeo: **Turnip Gen8 V38** (Mesa 27.0.0-1, Vulkan 1.4), carregado pelo
  libadrenotools a partir da pasta interna do app.

## Etapas

| # | Etapa | Pronto quando |
|---|---|---|
| M0 ✅ | APK base: Gradle + NDK + SDL3, Turnip pelo adrenotools, janela Vulkan, controles | O Odin 3 abre o app, o logcat mostra o Turnip V38 e os botões do controle |
| M1 ✅ | FEXCore no APK: roda um bloco x86-64 de teste e chama uma função nativa pela ponte HLE | Bloco de teste retorna o valor certo pelo thunk |
| M2 ✅ | Carregador: imagem preparada (`boot-linked.bin`, `content.bin`, `patches.bin`) mapeada na faixa reduzida, TLS (`fs:`) no estado do FEX, salto para o entry point | O jogo executa até a primeira chamada de sistema não implementada |
| M3 ✅ | Runtime: `runtime_*.c` em ARM64 (caminho Linux do SotC-PC), threads com contexto FEX próprio, fibras trocando o estado do convidado, sinais da GPU repassados pelo FEX | Logos e menus rodam sem vídeo (na prática exigiu o M4: a inicialização chama a GPU) |
| M4 | Vídeo: `gpu/` (shadPS4 + shim) com NDK, `vk_platform` usando o `vkGetInstanceProcAddr` do adrenotools, rastreio de páginas da GPU por `mprotect` | Tela de título aparece |
| M5 | Som (SDL3/AAudio), controles do Odin 3, saves e DLC em armazenamento do app | Jogo jogável do início ao 1º colosso |
| M6 | Desempenho: configuração do FEX (TSO, multiblock), patches de CPU do SotC, 60 FPS | Meta inicial: superar os 9,2 FPS do Bachata-S4 0.2.6 |

### Resultado do M0 (23/11/2026, Odin 3 + Turnip Gen8 V38)

Relatório completo em `docs/m0-report-odin3-turnip-v38.txt`. O driver carrega pelo
adrenotools (`turnip Mesa driver`, Mesa 27.0.0-devel, Vulkan 1.4.363), o controle aparece como
"Odin Controller" e a tela limpa roda a 60 FPS. O que muda o plano do M4 (vídeo):

- **Falta `VK_EXT_external_memory_host`.** O shadPS4 usa essa extensão para expor a memória do
  jogo à GPU. Caminhos: o `ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT` do adrenotools
  (`adrenotools_import_user_mem`) ou o fallback com cópias do shadPS4.
- **Falta `shaderFloat64`.** Instruções F64 do GCN precisam ser emuladas no recompilador.
- **Subgroup de 128.** O GCN é wave64; o Turnip tem `subgroupSizeControl`, então os shaders
  pedem 64 explicitamente.
- **Superfície com `currentTransform` ROTATE_90.** O painel é retrato; a apresentação final tem
  que girar a imagem (ou aceitar o custo da composição do Android).
- Também faltam `VK_KHR_fragment_shader_barycentric`, `VK_EXT_depth_range_unrestricted`,
  `VK_EXT_shader_atomic_float2`, `VK_KHR_maintenance8` e as extensões AMD/NV (o shadPS4 já tem
  caminho sem elas).

### Resultado do M1 (23/11/2026, Odin 3)

Relatório em `docs/m1-report-odin3.txt`. O FEXCore (revisão `f2b679f`, a mesma do Bachata-S4)
compila com o NDK r28 para o bionic e roda dentro do `libmain.so`. Os 8 testes passam:

| Teste | Resultado |
|---|---|
| Chamada HLE com 8 argumentos (6 em registradores, 2 na pilha) | ok |
| Callback do nativo para o convidado de dentro de uma chamada HLE | ok, RSP e registradores preservados |
| Argumento e retorno `double` por XMM0/XMM1 | ok |
| 2 threads convidadas em 2 threads do host, cada uma com seu `fs:[0]` | ok |
| Invalidação de código depois de um patch | ok |
| Loop de inteiros (200M iterações) | convidado a **65% da velocidade nativa** |
| Ida e volta convidado → nativo → convidado | **42 ns** por chamada |
| Cópia de memória do convidado, 64 MiB, com TSO ligado | 12 GB/s (memcpy nativo: 20 GB/s) |

Como a ponte funciona (`app/src/main/cpp/guest/`):

- Cada função nativa ganha um stub x86 de 16 bytes:
  `mov r10, rcx; mov rax, 0x5C0CD00D_<índice>; syscall; ret`. O FEX entrega o `syscall` ao
  nosso `SyscallHandler`. No ABI `OS_GENERIC` ele guarda todos os registradores no estado da
  thread e **não** passa argumentos nem grava o retorno: o handler lê RAX do estado e escreve
  o resultado de volta.
- `GuestCpu::Call()` chama código do convidado a partir do nativo (callbacks, entrada de
  threads). Ele guarda e restaura os registradores do convidado, porque o `CPUState` do FEX não
  é copiável.
- O FEX não tem verificação de código automodificável ligada aqui (o handler não marca páginas);
  patches no código do jogo precisam chamar `InvalidateCode()`.

**Como o FEX é compilado.** O FEX não aceita `CMAKE_SYSTEM_NAME=Android`, então
`cmake/fex-android-toolchain.cmake` apresenta o clang do NDK como um cross-compiler Linux aarch64
com o sysroot do bionic. A libc++ do NDK r28 (LLVM 19) não tem `std::atomic_ref`:
`cmake/fex-bionic-compat/atomic_ref_polyfill.h` é injetado com `-include` só nesse build. Um
`static_assert` do `fextl::move_only_function` nunca passa com a libc++; o patch local
`patches/fex/0001-libcxx-move-only-function.patch` desliga esse assert só com a libc++ (o SBO
de 3 ponteiros da libc++ guarda o lambda de 1 ponteiro sem alocar).

**Atenção:** o FEX pede no próprio `CLAUDE.md` que IA não gere código para contribuições ao
projeto. Os arquivos acima são remendos locais deste port e **não devem ser enviados ao FEX**.

Pendências que ficam para o M2/M3:

- Medir com TSO desligado (`SOTC_FEX_TSO=0`, ainda sem forma de passar a variável pelo app).
- O `QueryGuestExecutableRange` responde "tudo é executável"; o carregador vai responder pelo
  mapa da imagem do jogo.
- Sinais (falhas de página da GPU, SIGSEGV do jogo) ainda não passam pelo FEX.

### Resultado do M2 (23/11/2026, Odin 3)

Relatório em `docs/m2-report-odin3.txt`. O carregador (`app/src/main/cpp/loader/`) lê o
`boot-linked.bin` que o SotC-PC já prepara (BBPROBE5, com as mesmas validações do `probe.c`) e:

- reserva 256 MiB abaixo de 2 GiB de cima para baixo com `MAP_FIXED_NOREPLACE`; no Odin 3 a
  imagem caiu em `0x60000000`;
- aplica as 714.353 realocações. Dos 687 imports, 180 funções e 17 dados vão para o código dos
  PRX ligados (libc.prx, libSceFios2.prx), 2 dados para o host e 487 funções para **stubs de
  parada** (`... syscall; hlt`), que registram qual import foi chamado e param o convidado;
- monta o TCB como o `runtime_thread.c` (TLS variante II, `tcb[0]=self`, `tcb[1]=dtv`,
  `tcb[2]=thread`) e põe em `gs` (o `link_modules.py` trocou os `fs:[0]` do eboot por
  `gs:[0]`);
- roda os inicializadores dos módulos e depois o entry point na thread `guest-main`.

**Resultado:** o inicializador da libc.prx roda sob o FEX e para no primeiro import sem
implementação: `sceKernelGetProcParam`, chamado de `libc.prx +0x5e373`.

Lições que valem para o M3:

- Chamadas ao convidado de nível superior (inicializadores, entry point, início de threads)
  não podem usar o `HandleCallback` do FEX: ele volta para uma pilha de host salva por um
  `ExecuteThread` anterior e, numa thread nova, cai com `sp=0`. O `GuestCpu::Call()` agora
  empurra um endereço de retorno que aponta para um `hlt` e usa `ExecuteThread`; o
  `HandleCallback` fica só para chamadas aninhadas, de dentro de uma chamada HLE.
- No Linux, o `syscall` não termina o bloco do JIT nesta revisão do FEX, então o handler não pode
  mudar o RIP. Funções que não voltam (stop, `exit`, `pthread_exit`) usam stubs que terminam em
  `hlt`.
- O heap do Android (scudo) põe uma tag no byte alto dos ponteiros (`0xb4…`). Tudo que o jogo
  enxerga como ponteiro tem que vir de `mmap` ou de memória estática, nunca de `malloc`/`new`.
- `__progname` (dado) ainda não tem origem e virou uma página sem acesso.

**Como o M3 vai ligar o runtime do SotC-PC.** Os `runtime_*.c` devolvem ponteiros de funções C
nativas, que no PC o código do jogo chama direto. Aqui cada uma vira um stub HLE com uma ponte
genérica: os argumentos inteiros (RDI, RSI, RDX, RCX→R10, R8, R9 e mais dois da pilha) vão para
x0–x7, XMM0–7 vão para d0–d7, e na volta x0/x1 vão para RAX/RDX e d0/d1 para XMM0/XMM1. Isso
cobre funções não variádicas com até 8 argumentos inteiros e 8 de ponto flutuante. Os casos
especiais vão precisar de adaptadores próprios: structs por valor, retornos grandes, `va_list` do
convidado e os pontos em que o runtime chama ponteiros de função do jogo (callbacks, entrada de
threads, `atexit`), que precisam passar por `GuestCpu::Call()`.

### M3 em andamento (24/11/2026, Odin 3): runtime ligado, para na primeira chamada de GPU

Relatório em `docs/m3-report-odin3.txt`. O runtime do SotC-PC (`app/src/main/cpp/runtime/`,
commit `aaf95c0`, mudanças marcadas `sotc-android:` e listadas em `runtime/VENDOR.txt`)
compila para ARM64 e atende os imports pela ponte genérica (`guest/host_bridge.S`). O jogo é
lido de onde já está no Odin 3, o cartão SD (`/storage/4A21-0000/PlayStation 4/Shadow Of The
Colossus/CUSA08809`), com a atualização `CUSA08809-patch` aplicada por cima. Isso precisa da
permissão "Acesso a todos os arquivos"; o `GameLocator.java` procura em todos os volumes.

**Resultado:** 341 imports vão para o runtime, 164 para o código dos PRX e 162 continuam como
paradas. Os inicializadores da libc.prx e da libSceFios2.prx retornam 0. O entry point do jogo
roda e o runtime atende `_init_env`, `atexit`, `sceKernelIsNeoMode`, a primeira alocação de
memória direta e 78 locks de mutex. O jogo para no primeiro import da GPU:
`sceGnmGetTheTessellationFactorRingBufferBaseAddress` (eboot `+0x18b77`).

O que foi preciso:

- **Ponte genérica:** argumentos System V → x0–x7/d0–d7 e retorno x0/x1/d0/d1 → RAX/RDX/XMM0/XMM1.
  Os XMM são lidos direto do `CPUState`; o layout (SSE neste aparelho) é detectado uma vez.
- **Threads:** cada thread do jogo ganha um contexto FEX com pilha própria
  (`sotc_guest_attach`). A entrada passa pelo FEX, e o `pthread_exit` usa um stub que termina em
  `hlt`.
- **SIGBUS:** com TSO, até loads e stores comuns viram instruções atômicas, que falham quando
  desalinhadas. O handler chama o `HandleUnalignedAccess` do FEXCore, que emula o acesso e
  corrige o código gerado (modo half-barrier), como faz o frontend Linux do FEX.
- **Heap do Android:** `android:allowNativeHeapPointerTagging="false"`, porque o runtime entrega
  ponteiros de `malloc` ao jogo, que os compacta em 40 bits.

**Por que o M3 para aqui.** A meta "logos e menus sem vídeo" não dá para separar do M4: o jogo
chama a GnmDriver logo no começo da inicialização e depois espera os eventos de fim de comando
e de flip da GPU. Um "GPU falso" que só completasse esses eventos seria jogado fora depois. O
próximo passo é portar a biblioteca `gpu/` do SotC-PC (shadPS4 + shim, com GnmDriver, VideoOut
e as filas de eventos do kernel) para Android.

Ainda faltam, independentemente da GPU: as fibras (`runtime_fiber.c`), o dado `__progname` e o
repasse de SIGSEGV do jogo.

### M4: recorte

`app/src/main/cpp/gpu/` é o `gpu/` do SotC-PC (shadPS4 `b4e7ae7` + correções SotC, mesmo
commit `aaf95c0`), com as mudanças marcadas `sotc-android:`.

- **Ficam de fora na primeira passada:** FSR3 com geração de quadros (`vk_fsrfg`), FSR4, DLSS
  (`vk_dlssg`, Streamline), AvPlayer (precisa de FFmpeg), RenderDoc e os patches de CPU x86 do
  eboot (`sotc/cpu_patches*`); o FEX já emula SSE4a e BMI. FSR1, nitidez e o menu ImGui
  continuam.
- **Precisam de versão ARM64:** o SRT walker do recompilador
  (`flatten_extended_userdata_pass.cpp`), que gera código x86 com xbyak e o executa no host; o
  rastreio de páginas (`page_manager.cpp`), que decodifica a instrução x86 que faltou com Zydis
  (aqui leitura e escrita vêm do ESR do sinal ARM64); intrínsecos `rdtsc`/`_mm_*`; a superfície
  X11, que aqui é Android. O núcleo do Bachata-S4 (shadps4-arm64, GPL-2.0-or-later) resolveu os
  mesmos pontos e serve de referência.
- **Driver:** o `vk_platform` usa o `vkGetInstanceProcAddr` do adrenotools (Turnip). Faltas
  conhecidas no Turnip: `VK_EXT_external_memory_host`, `shaderFloat64`, subgroup de 128.
- **Dependências:** Boost (`externals/ext-boost`, o subconjunto do shadPS4), Zydis 4.1.1
  (`externals/zydis`), fmt e xxhash do FEX, e os headers Vulkan 1.4.363, VMA e robin-map copiados
  do MSYS2 (`externals/sysdeps/include`). Os shaders do host são compilados no PC pelo
  `glslangValidator` do MSYS2 (`I:\msys64\clang64\bin`).

### M4 em andamento (24/11/2026, Odin 3): a abertura da tela de título aparece

A biblioteca de vídeo inteira (shadPS4 + shim + SotC) compila para ARM64 e roda no Turnip Gen8
V38. Depois da inicialização (M3), o jogo:

- carrega o mundo (`LoadInitialWorld`, 16.681 recursos), abre o som pelo AAudio (threads `sndx`)
  e reconhece o "Odin Controller";
- entra no loop principal (`Shadow Game Update`) e **desenha a abertura da tela de título**: a lua
  entre as nuvens com o pássaro e a silhueta do Wander a cavalo, de **20 a 30 FPS** (o medidor do
  SotC chega a 54);
- depois de 8 a 50 segundos, o processo termina. Numa corrida: `Vulkan device lost` (Turnip) e
  `exit(23)`; noutra, o Android só registra "Process has died", sem sinal.

O que mudou para chegar aqui:

- **SRT walker em bytecode** (`flatten_extended_userdata_pass_arm64.cpp`): mesma análise do IR do
  walker x86 (Xbyak), emitida como bytecode de pilha e executada por `RunSrtWalker()`. Leituras de
  ponteiro nulo retornam zero, como o handler x86 do SotC faz. Alguns offsets com `Phi` não são
  computáveis e caem na janela dinâmica, como no x86.
- **Rastreio de páginas:** `signal_context.cpp` lê PC e ESR do sinal ARM64; o `page_manager`
  desliga os caminhos que decodificam instruções x86 do host; o handler de SIGSEGV do app chama
  `bbgpu_handle_fault`.
- **Integração:** o `WindowSDL` reaproveita a janela da activity e não tem thread própria de
  eventos; o `vk_platform` cria a superfície Android com o `vkGetInstanceProcAddr` do adrenotools;
  `SUBOPTIMAL` (painel retrato, transform identidade) não recria o swapchain; o `bbgpu_init` roda
  entre ler a imagem e ligar os imports, como no `probe.c`; o Java define `BB_GPU_USER_DIR` e
  `HOME` antes de carregar o `libmain.so` (o `path_util.cpp` monta caminhos num construtor
  estático).
- **Fibras** (`loader/fibers.cpp`): a troca acontece no handler HLE, salvando e carregando RSP e
  os registradores callee-saved no estado do FEX; o `ret` do stub entra na outra fibra.
- **Callbacks do NGS2** (alocador, voz, efeito) passam pelo FEX.
- **libc++ do NDK r28:** `-fexperimental-library` para `std::jthread`/`stop_token`.

Próximos passos do M4:

1. Achar a causa das saídas: o `device lost` do Turnip (shader ou comando específico) e a morte
   sem sinal (exit do runtime? memória?). Ligar `BB_FAULT_RING`/logs do shadPS4 e capturar com
   `VK_EXT_device_fault` indisponível.
2. Medir memória: 5,3 GB de memória direta + 0,4 GB flexível + o orçamento Vulkan de 7 GB num
   aparelho de 12 GB.
3. Conferir a orientação da imagem e o FSR1 na saída.

A preparação da imagem (`scripts/*.py` do SotC-PC) roda no PC no começo; o APK recebe a pasta
`out/` pronta. Portar a preparação para o aparelho fica para depois de M5.

### Launcher (24/11/2026)

O app abre no `LauncherActivity`, com as páginas do launcher do SotC-PC (Jogar, Jogo e saves,
Imagem e FPS, Controles e som, Driver de vídeo, Avançado). O ícone é o do SotC-PC (o rosto do
colosso, `launcher/make_icon.py`) e o espadachim do `window_icon.png` fica no topo do menu.

- `Settings.java` guarda as escolhas e monta o ambiente do jogo como o `game_environment()` do
  `sotc_launcher_win.py` (modos de jogo, FPS, texturas, efeitos, VRAM, FSR1/RCAS), mais as opções
  do Android (driver, `TU_DEBUG`, TSO do FEX, autoteste).
- O `SotcActivity` roda no processo `:game`: aplica esse ambiente com `Os.setenv` antes de carregar
  o `libmain.so` e termina o processo ao fechar, para a próxima partida começar limpa.
- `loader/sotc_modes.cpp` aplica na memória, depois das relocações, o que o `sotc_modes.py` gera
  no PC (`patches.bin` e `cmdline.bin`): fps60, fps60game, grassbuf, af16, hdr, a correção do
  alocador de páginas, a reserva de texturas no modo Pro e as cvars na linha de comando do motor
  (o `lea` de `+VSyncWindow=10%` aponta para o texto no fim da área de trampolins).
- `bbgpu_set_fps_target_slot` (fps60game) e `runtime_memory_set_release_site` (T38, dono das
  páginas do pool) ligados como no `probe.c`. Sob o FEX o `__builtin_return_address(0)` do
  runtime é o da ponte HLE; `sotc_guest_return_address()` dá o endereço de retorno do jogo.
- O autoteste do M1 só roda quando ligado no launcher; sem ele, o FEX inicia direto.
- O SRT walker lê a memória do jogo por `sotc_safe_load32/64` (`guest/host_bridge.S`): um ponteiro
  velho (visto: `0x9bc00`) lê zero, como no handler x86 do PC, em vez de derrubar a thread de shaders.
- A janela de endereços do PS4 (`USER_MIN`..) fica reservada (`PROT_NONE`) no início do `main`,
  antes do driver: no Android (39 bits, 512 GiB) a janela do PC (até `0xfc00000000`) cobria o
  espaço inteiro do processo e o `MAP_FIXED` do runtime podia cair sobre memória do sistema.
  Agora são 64 GiB, e liberar devolve à reserva em vez de `munmap`.

### Queda da GPU (24/11/2026): o que já foi descartado

O jogo chega ao loop principal e desenha a 25–30 FPS, mas o kgsl derruba o contexto entre 15 s e
4,5 min: `CP Protected mode error | WRITE | addr=0x08847`, `CP DDE BR opcode error | opcode=0`,
`CP BV opcode error | opcode=0x70970001` ou `RBBM: GPC error` → `Vulkan device lost`. Testado,
sem mudar o resultado (um jogo de 1,5 a 3 min cada):

| Teste | Resultado |
|---|---|
| Turnip Gen8 V36 | cai (30 s) |
| Driver Qualcomm do sistema | sai com código 23 no primeiro shader |
| `TU_DEBUG=sysmem` | cai (66 s) |
| `TU_DEBUG=flushall,syncdraw` | cai (35 s) |
| `SOTC_RECORD_THREAD=0` | cai (15 s) |
| `BB_DIRECT_MEMORY_ACCESS=0` | cai (152 s) |
| `SOTC_RB_HOT=0` | cai (30 s) |
| janela de memória reservada | cai (56 s) |
| Turnip do uncharted-android 0.3.8 (Mesa 26.2.5 + patches KGSL/a8xx, sysmem e recuperação de contexto) | `RBBM: GPC error` aos 46 s; a recuperação troca o contexto, o trabalho enviado se perde, o jogo espera a GPU 5 s e sai (código 23) |

**Resolvido (24/11/2026): arenas com memória própria.** O cache de buffers do SotC usa arenas
esparsas de 4 GB; no Adreno 830 o suporte esparso do Turnip corrompe o fluxo de comandos. Portado o
caminho sem memória esparsa do uncharted-android (issue #36) e do Bloodborne-Android
(`MemoryType::Backed`): cada arena é um buffer comum com memória própria, em páginas de 16 MiB
(`BB_NONSPARSE_ARENA_MB`), fundidas com cópia quando um pedido atravessa arenas; residência e
liberação só mexem na tabela BDA; o despejo de blocos frios fica desligado. Padrão em GPUs Qualcomm
(`BB_NONSPARSE_ARENAS=0` volta ao esparso). Resultado: 4 min sem queda (antes 15 s a 2,5 min), a
abertura do jogo com texturas, 15–30 FPS.

**Velocidade (24/11/2026):** o FEX escala o `rdtsc` do jogo para 1 GHz+ quando o contador do
aparelho é lento (`SmallTSCScale`, 19,2 MHz << 6), mas o runtime informa 19,2 MHz ao jogo: o tempo
corria 64 vezes mais rápido ("Starting main loop: 00:02:57" em ~10 s). A escala fica desligada.

**Cores (24/11/2026): subgrupos de 64 no Adreno.** O Turnip informa subgrupos de 128 (mínimo 64) e
só deixa fixar o tamanho no fragment e no compute: o port não entrava no caminho de 64 (exige os
três estágios) nem no de 32 (mínimo 64), e compilava o código wave64 do jogo para 128 lanes sem
fixar o tamanho. Wander amarelo chapado, Agro verde, pontos vermelhos e verdes e brilho vermelho,
a mesma família de falha das AMD no PC (0.57, issue #7). Agora o Adreno usa o caminho de 64
(`IsAdrenoWave64`: 64 fixado no fragment e no compute; `SOTC_ADRENO_WAVE64=0` desliga) com as
barreiras de LDS ligadas, e esses shaders têm pasta de cache própria (`_w64`). Os pontos coloridos
e o brilho vermelho sumiram na abertura.

**Memória (24/11/2026):** arenas com memória própria ocupam páginas inteiras: com 16 MiB eram
2,2 GB aos 100 s e o Android matou o jogo (lowmemorykiller, ~500 MB livres). No Adreno as páginas
são de 2 MiB (`BB_NONSPARSE_ARENA_MB`): 1,3 GB e ~1,4 GB livres no mesmo ponto, mesmo FPS. 4,4 min
sem queda.

**Terreno preto (24/11/2026): baricêntricas por geometry shader.** O SotC interpola à mão as
entradas dos pixel shaders a partir das baricêntricas; o Turnip não tem
`VK_KHR_fragment_shader_barycentric` nem a extensão da AMD, e o geometry shader que as fornece (PC,
GTX 10xx, issues #57/#68/#102) só ligava no driver da NVIDIA. Sem ele: terreno e rochas pretos e
personagens chapados. Agora liga também no Turnip (cache `_w64gs`). Com isso o jogo aparece
completo: campo, rochas, Wander, Agro, Santuário.

**Jogável (24/11/2026):** com "pular logos" e "pular filme" o jogo vai direto ao Santuário; o
Wander anda, a câmera gira, R1 ergue a espada. Para testes automáticos, `BB_PAD_FILE` (variáveis
extras) aponta para um arquivo em `Download` com os botões apertados (`cross`, `ly=0`, ...).

Ainda a ver: o FPS (8–17 no campo perto do Santuário), a memória (~700 MB livres) e partidas
longas.

### Desempenho (24/11/2026): ~10 FPS no campo do Santuário, meta 30 com a qualidade do PS4

Medido parado no mesmo ponto (`bench.ps1`), 1080p, Turnip V38:

| Mudança | FPS |
|---|---|
| base | 9–10 |
| resolução interna 1280x720 (`SOTC_RENDER`, FSR 1 até 1080p) | 10,7 |
| 720p + `TU_DEBUG=sysmem` | 11,4 |
| sem o geometry shader das baricêntricas (terreno preto) | 12,4 |
| TSO do FEX desligado / efeitos desligados / VRAM econômica / texturas desligadas | 10–11 |
| grama reduzida / `SOTC_MUTEX_SPIN_US` 0 ou 200 / FPS ilimitado | 10–10,7 |

- GPU: kgsl 75–80% ocupada, mas a 734–832 MHz de 1100 (o regulador não sobe). O custo não cai com
  a resolução: é por draw/barreira (~3.900 draws e ~900 dispatches por quadro).
- CPU: o JIT do jogo ~19%; ~30% em CAS de mutex e ~20% no kernel. `SOTC_HLE_STATS=1`: 2 milhões de
  chamadas HLE/s, quase todas `scePthreadMutexLock/Unlock` (~770 mil/s cada) e `scePthreadYield`
  (~200 mil/s): as threads de trabalho do jogo esperando tarefa em laço (sintoma, não causa).
- `SOTC_GPU_TIMES` dá ~23 ms de GPU por quadro; num tiler as marcações dentro de uma passada não
  medem o trabalho dos tiles, então o número real é provavelmente maior.

Novidades: barra de desempenho no topo (`SOTC_PERF_BAR`, L3 + R3 mostra/esconde), resolução interna
(`SOTC_RENDER=LxA`, sites do `sotc_modes.py --render`) e `SOTC_HLE_STATS`.

Modo de desempenho máximo do Odin 3: a GPU vai a 1050 MHz (antes 734–832), 89 °C, e o FPS só passa
de 9–10 para 10,9, com a GPU ainda ~80% ocupada. Nem clock nem resolução movem o FPS: o limite está
em como o trabalho chega à GPU (barreiras, esperas e submissões), não na velocidade dela.

**O gargalo (`SOTC_DRAW_SPLIT=1`, ~100 ms por quadro):** a thread de desenho fica ~45 ms ocupada
(binds 26, draws e dispatches ~42) e ~58 ms parada; a interpretação dos comandos leva ~25 ms; e o
jogo espera leituras da GPU ~30 ms por quadro, em ~30 idas e voltas CPU↔GPU (`finish` ~1.450 em
5 s). Cada leitura precisa (o jogo lendo memória que a GPU escreveu) manda a fila, espera a GPU e só
então libera o jogo: no PC ~0,1–0,3 ms, aqui milissegundos. Por isso nem clock nem resolução mudam o
FPS. `BB_READBACKS=1` (relaxadas) fecha o jogo, como no PC.

**Driver da Qualcomm (sistema):** passa da inicialização (subgrupos de 64 nativos, Vulkan 1.3,
15 GB), mas o compilador recusa o primeiro shader de compute (`0x50784801`, "Shader compilation
failed for shaderType: 5"). Não tem `extended_dynamic_state3`, `attachment_feedback_loop`,
`memory_budget`, `shader_atomic_float2` nem `shaderFloat64`. Para ver o motivo falta despejar o
SPIR-V (`BB_DUMP_SHADERS=1`; o adb não lê `Android/data`, e apontar `BB_GPU_USER_DIR` para
`Download` não gravou).

**Sincronizações encurtadas (24/11/2026): 10 → 13 FPS.** Com o Turnip no modo sem tiles,
`SOTC_GPU_TIMES` mostra ~31 ms de GPU por quadro: a GPU daria conta de 30 FPS, mas ficava parada
esperando trabalho. Dois ajustes do SotC-PC que no Adreno estavam desligados:
- envio periódico (`SOTC_FLUSH_US`): só ligava em NVIDIA/AMD; a cada 250 µs (1000 µs: sem ganho)
  a GPU roda o quadro enquanto o jogo o grava;
- marcador de espera (`SOTC_WAIT_MARKER=1`): a GPU escreve um marcador que a CPU lê, em vez de
  acordar pelo kernel a cada leitura.
Juntos: 13 FPS em três partidas seguidas, kgsl 88% ocupada. Padrão no Android (Settings.java).
`SOTC_WAIT_SPIN_US=1000` sozinho fechou o jogo uma vez.

Testado depois, sem ganho (12–13 FPS, kgsl ~88%): Turnip V36 (8,4), Turnip do uncharted-android
(12,3), Turnip faux123 1.2.10 (10,7, feito e testado num Odin 3), `TU_DEBUG=noubwc` (12,3), `sysmem,noubwc,nolrz` (11,6). Falhas de proteção de página: ~1.500/s
(no Bloodborne-PC eram 115 mil/s e a janela de 64 KiB deu +22%; aqui não pesa). Jogando, com a
câmera e o Wander em movimento, o FPS fica em 10–11.

**Configuração do ShadowOfTheColossus-Android-Aot (24/11/2026): 13 → 19–20 FPS.** O port de
erickmendes22 (Daedalus, fork do SotC-PC, GPL-2.0-or-later) roda o jogo no Android a 30 FPS com AOT
na CPU (bbrecomp) e ajustes de renderização. Trazidos para cá (padrão no Android, opções no
launcher):

| Mudança | FPS |
|---|---|
| nosso padrão, 1080p | 13 |
| as opções dele que já existiam aqui (`SOTC_TAA_OFF`, `SOTC_RB_STREAM`, `SOTC_RB_HOT=0`, `SOTC_UPLOAD_BUDGET_MB=0`, `SOTC_ASYNC_TRANSLATE/PRELOAD=0`, `TU_DEBUG=nomultipos`) + 720p interno | 16,1 |
| + patches do Bachata S4+ bloom e blur (`SOTC_BACHATA_PERF`, portados para `sotc_modes.cpp`) | 18,6 |
| idem em 540p (`960x540`, altura múltipla de 4) | 20–22 |
| padrão novo do launcher (720p, bloom/blur/TAA desligados) | 19,3 |

Os patches dele não encaixam direto: ele parte do SotC-PC 0.74 e nós do 0.77 (os sete scripts
falharam por trechos que mudaram); precisam ser portados à mão. Perfil (`simpleperf --app`): na
thread de desenho `memcpy` 8%, `BindTextures` 3,8%, `tlsdesc_resolver_dynamic` 2,9%, `@plt` 2,2%;
na de comandos `BackAlloc` 7,4%, `ProcessCommands` 4,6%, `RunSrtWalker` 4,6%,
`StageSpecialization ==`/`ForEachSharp` ~3%. Primeiro passo: `-O2` em todo o `libmain.so`, ThinLTO
e `-Bsymbolic-functions` (`SOTC_LTO`). A medição dele (16,5 FPS) não vale: o Odin estava em
estado térmico 5, CPUs a ~90 °C e limitadas a 2,2 GHz depois de mais de uma hora de testes.
Medir com o aparelho frio, e com pausas entre testes longos.

Com o Odin frio: o modo médio limita as CPUs a ~2,2 GHz (hardware: 3,53 e 4,32 GHz). LTO no modo
médio: 18,7 FPS (sem LTO: 19,3; o LTO não muda o FPS). Modo máximo: 20,6 FPS (+10%), mas a
carcaça foi de 46 °C a 61–63 °C em ~1,5 min e o Android voltou ao estado térmico 5: não se sustenta
numa partida. O FPS tampouco escala com o clock da CPU: as threads de comandos ainda esperam.
`--ez autostart true` no LauncherActivity abre o jogo sem tocar em JOGAR (testes por adb).

Resultados (aparelho a ~49 °C pelos sensores do kernel; o "skin" do `thermalservice` fica parado
na última leitura): base 16,9 (fim 18); `SOTC_RB_AHEAD=1` 19,2, mas a leitura antecipada só existe
no caminho síncrono, e o que domina é o assíncrono (`BeginAsyncReadback`: copia no envio atual,
envia e espera a GPU terminar tudo o que veio antes); o ganho é ruído, não fica ligada. SSR e
névoa desligados (patches do Bachata) 19,5: efeitos leves. O peso está no tempo de GPU por quadro
(~83% ocupada, ~40 ms por quadro; 30 FPS pede 33 ms). O geometry shader das baricêntricas custa
~20%: o Turnip tem `VK_KHR_fragment_shader_barycentric` no código (V38, faux123, uncharted), mas não
a habilita no Adreno 830; ligá-la pede um Turnip compilado por nós.

**Turnip próprio (24/11/2026), investigação:** no Mesa `main`, o Turnip não implementa
`VK_KHR_fragment_shader_barycentric` (o nome no `.so` vem da tabela comum de extensões). Seria
implementar: as baricêntricas a partir dos `ij` do ir3 e os valores por vértice com `bary.f` em
`ij` fixos, mais a declaração do recurso. Build: `build_turnip.sh` de StevenMXZ/Adreno-Tools-Drivers
(Mesa `main` + patches do 8xx, NDK r29 Linux, meson), no WSL Debian 13 deste PC (faltam meson,
bison, flex). Ganho esperado: o custo do geometry shader, ~20%. Patch só local, sem envio ao Mesa.

**Turnip próprio, resultado (24/11/2026):** `patches/turnip/` (passe `ir3_nir_lower_fs_barycentric.c`
e `sotc_barycentric.py`, aplicado depois dos patches do `build_turnip.sh`; Mesa `main` fdae82f).
Baricêntricas = `(1-i-j, i, j)` a partir dos `ij` do ir3; valor por vértice = `bary.f` em `ij`
(0,0), (1,0), (0,1); entradas inteiras por vértice leem o vértice provocante. O driver lista a
extensão e o perfil de shader fica "barycentrics 1, manual interpolation 0" (sem geometry shader).
720p, modo médio, cache de shaders quente, mesma cena:

| Driver | FPS médio (45 s) | fim da medição |
|---|---|---|
| V38 + geometry shader | 15,5 | 16–17 |
| Turnip próprio (KHR) | 18,8 | 24–25 |

Trocar de driver muda o perfil e refaz o cache de shaders: a primeira partida após a troca tem
quedas (12–23 FPS) até compilar tudo.

**Mas a imagem sai errada** (visto pelo usuário): mato com riscos horizontais esticados e sombras
estranhas. Com o mesmo driver e o geometry shader (`SOTC_NO_BARYCENTRIC=1`) a imagem fica certa,
então o erro está no passe. Tentativas que não resolveram (todas pioraram o terreno para triângulos
de cor chapada):
- ler como entrada flat os parâmetros flat do PS4 (eles também são lidos como P10/P20: o SotC
  interpola à mão justamente os parâmetros flat);
- ler como flat só os parâmetros flat lidos apenas como P0, com e sem provoking vertex = primeiro;
- trocar a ordem dos vértices do passe (permutações 0 e 1 iguais; a 2 não chegou a carregar).
Hipótese ainda aberta: `bary.f` em `ij` fixos não devolve os bits do vértice (zera denormais,
propaga NaN), e o jogo guarda dados empacotados nesses parâmetros. Próximo passo seria extrair o
shader do mato e comparar os valores por vértice com os do geometry shader.

Por isso o port usa o geometry shader no Turnip mesmo com a extensão; `SOTC_TU_KHR_BARYCENTRIC=1`
liga a extensão para investigar. O Turnip próprio continua o padrão: com o geometry shader ele deu
20–21 FPS na mesma cena (V38: 16–17).

**Resolvido (25/11/2026): bits exatos por vértice → 26–28 FPS.** O shader mais pesado do G-buffer
(`0x972ea312`, extraído com `BB_DUMP_SHADERS=1 BB_DUMP_ONLY=972ea312` e `BB_GPU_USER_DIR` numa pasta
interna do app, que o `run-as` lê) lê os valores por vértice como bits: `BitCastU32F32`, depois
`>> 16` e conversão de `half`. O jogo guarda dois `half` em cada 32 bits. O `bary.f` em `ij`
fixos é aritmética de float: zera denormais (com o `half` alto zero, o valor inteiro some) e
arredonda os bits baixos da mantissa (o `half` baixo). Somar `1 << 23` aos bits tirou os
denormais, mas as pedras ainda saíram facetadas (o arredondamento). A correção:
`ir3_nir_encode_per_vertex_inputs`, chamada em `tu_link_shaders`, faz o estágio antes do pixel
shader escrever as duas metades de 16 bits como floats inteiros (0–65535, exatos em qualquer conta
de interpolação), a baixa no componente original e a alta num slot genérico livre, e o pixel
shader junta os bits de volta. Imagem igual à do geometry shader, 26–28 FPS na mesma cena (com o
geometry shader: 20–21). Pacote `meta.json` versão 2; a extensão é o padrão
(`SOTC_TU_KHR_BARYCENTRIC=0` volta ao geometry shader).

Para testes: o teste da extensão e o despejo de shaders têm pasta de cache própria (tag `_t…`),
então trocar de modo não apaga o cache normal; e o `bench.ps1` regrava o launcher no padrão ao fim
de cada partida (as variáveis de teste ficavam no campo de variáveis extras, e o jogo aberto pelo
launcher saía no modo de teste). Não listar `Android/data` com o adb: com milhares de arquivos de
dump o adb travou duas vezes.

**Turnip DD do port AOT (24/11/2026):** o ShadowOfTheColossus-Android-Aot embute só o Turnip DD do
Droid-Deck (v0.9.1, Mesa `0c418c5054`). Não tem baricêntricas (o port usa o mesmo caminho do
shadPS4 sem extensão); traz ajustes do Adreno 8xx copiados do driver da Qualcomm (histerese dos SP,
`RB_RESOLVE_CR_CNTL`, LRZ por máscara de cor, autotuner de GMEM). Com o nosso geometry shader, mesma
cena, Odin frio: DD 18,9 e 18,5 (fim 19); nosso Turnip 19,5 (fim 20). Os ajustes não ajudam aqui: o
FPS do port AOT não vem do driver. O geometry shader já só entra nos pipelines cujo pixel shader lê
valores por vértice (`NeedsBarycentricGs`).

**Onde vai o tempo de GPU (24/11/2026).** `SOTC_GPU_TIMES=1` (com `SOTC_FPS=1`, que imprime o
relatório; `TU_DEBUG=sysmem,nomultipos` para os carimbos valerem dentro das passadas) agora soma por
passada (tamanho, destinos de cor, profundidade) e marca os draws com o geometry shader das
baricêntricas (`0x80000000xxxxxxxx`). Os carimbos em volta de cada draw inflam o total (~54 ms de
operações por quadro com o jogo a 20 FPS), mas as proporções valem:

| Passada | ms por quadro | draws |
|---|---|---|
| G-buffer 1280x720, 7 cores + profundidade | 23,7 | ~1.560 |
| compute fora de passada | 11,8 | ~1.170 |
| só profundidade 1280x720 (pré-passada) | 10,4 | ~2.150 |
| sombra 1024x1024 | 5,6 | ~860 |
| resto (pós-processamento) | ~2 | ~60 |
| **draws com o geometry shader** (em várias passadas) | **31,4** | ~2.480 |

Um só pixel shader, `0x972ea312` (com geometry shader, ~1.000 draws indiretos no G-buffer), leva
~11 ms. Testado sem ganho: geometry shader passando só os componentes lidos (19,9 FPS; o Turnip
já tira os não lidos ao ligar os estágios). Grama com metade da distância (`SOTC_GRASS=0.5,1,0.5,1,1`,
aceita vírgulas agora): 20,7 (fim 21–22), +5–8% com perda visível.

O SotC-PC tinha outro limite: GPU folgada (~13 ms) e o jogo esperando leituras e a thread de
desenho; o que ganhou lá (envio periódico, marcador de espera, `SOTC_RB_STREAM`, caminhos rápidos
da thread de desenho, cache de shaders) já está aqui. As opções do launcher PC que não vieram
(`SOTC_PC_MEMORY`, `SOTC_TEX_DIRECT`/`GATHER`) pedem 24 GB e mexem na troca de área, não no FPS
parado. Aqui o limite é o próprio trabalho da GPU, e mais da metade dele está nos draws que passam
pelo geometry shader.

Próximo teste (com o aparelho frio, no modo médio, fechando o jogo ao fim de cada medição: a
carcaça chegou a ~100 °C segundo o usuário depois de vários testes seguidos):
- `SOTC_RB_AHEAD=1`: leitura antecipada (Pink-shadPS4): quando o dado foi escrito por trabalho já
  enviado, copia num envio próprio em vez de mandar e esperar tudo o que foi gravado. Só liga em
  NVIDIA e AMD; no Adreno está desligada, como estavam o envio periódico e o marcador.
- `SOTC_RB_PUSH=1` (cópia logo após o dispatch que escreveu) e `SOTC_RB_STATS=1` (quais leituras
  custam espera, por instrução e região) para o mapa das leituras seletivas, no estilo do port de
  TLOU (leitura relaxada no geral e precisa só onde o jogo precisa).

Ainda não trazidos: os atalhos de CPU dele na montagem de comandos (`stage_perf_fastpaths`,
`stage_sotc_stage_spec_cpu`, `stage_sotc_flatbuf_reuse_057`, `stage_sotc_rt_memo`,
`stage_sotc_reg_snapshot`), `SOTC_DEPTH_FAST`/`SOTC_GBUFFER_FAST`, o driver Turnip "GameHub DD
26.3" e o AOT.

Ainda: o kgsl conta ~68 ms de GPU por quadro e as marcações ~31 ms; a diferença está entre as
operações (barreiras, passagens de render, cópias). E o jogo fechou no carregamento em ~1 de cada
3 partidas em várias configurações; `bench.ps1` agora guarda o log quando isso acontece.

## Referências

- SotC-PC: carregador (`src/probe.c`), runtime (`src/runtime_*.c`), vídeo (`gpu/`).
- [shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64) (GPL-2.0-or-later): como o
  Bachata-S4 integra o FEXCore (`src/core/guest_cpu/fex_*`, `runtime/patches/fex-fexcore-only.patch`).
- [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause), [SDL3](https://github.com/libsdl-org/SDL) (zlib).

## Licença

GPL-2.0, como o SotC-PC. Nenhum arquivo do jogo, firmware ou chave da Sony entra no repositório.
