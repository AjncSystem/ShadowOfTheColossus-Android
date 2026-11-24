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

## Referências

- SotC-PC: carregador (`src/probe.c`), runtime (`src/runtime_*.c`), vídeo (`gpu/`).
- [shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64) (GPL-2.0-or-later): como o
  Bachata-S4 integra o FEXCore (`src/core/guest_cpu/fex_*`, `runtime/patches/fex-fexcore-only.patch`).
- [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause), [SDL3](https://github.com/libsdl-org/SDL) (zlib).

## Licença

GPL-2.0, como o SotC-PC. Nenhum arquivo do jogo, firmware ou chave da Sony entra no repositório.
