# Turnip com VK_KHR_fragment_shader_barycentric

Patch local do SotC-Android para o Turnip (Mesa), sem envio ao Mesa. O jogo interpola parte das
entradas à mão; sem a extensão, o port gera as baricêntricas num geometry shader (~20% do tempo de
GPU). Com ela, o Odin 3 foi de 16–17 para 24–25 FPS na mesma cena (720p, modo médio), mas o mato
e as sombras saem errados (ver `docs/PLANO.md`), então o port só usa a extensão com
`SOTC_TU_KHR_BARYCENTRIC=1`.

- `ir3_nir_lower_fs_barycentric.c`: passe NIR do ir3. Baricêntricas = `(1-i-j, i, j)` a partir dos
  `ij` do rasterizador; valor de uma entrada em cada vértice = `bary.f` em `ij` (0,0), (1,0) e
  (0,1). Entradas inteiras por vértice leem o vértice provocante.
- `sotc_barycentric.py`: aplica tudo na árvore do Mesa (extensão, recurso, propriedade, o passe e
  uma guarda em `ir3_compiler_nir.c`). Cada edição se ancora em texto exato e falha se o Mesa mudou
  ali; rodar de novo não duplica nada.

## Gerar o pacote

Base: [StevenMXZ/Adreno-Tools-Drivers](https://github.com/StevenMXZ/Adreno-Tools-Drivers) `b5725b5`
(`build_turnip.sh`: Mesa `main` + patches do 8xx, NDK r29 Linux, meson), com o Mesa em `fdae82f`.
Num Linux (aqui, WSL Debian 13) com git, meson, ninja, flex, bison, zip, unzip, glslang e
python3-mako/yaml:

```sh
git clone https://github.com/StevenMXZ/Adreno-Tools-Drivers turnip && cd turnip
git checkout b5725b5
MESA_COMMIT=fdae82f0e23f21a649e27f296b623be6304a87ef ./build_turnip.sh  # baixa NDK e Mesa, aplica os patches deles e compila
cd turnip_workdir/mesa
python3 /caminho/SotC-Android/patches/turnip/sotc_barycentric.py
ninja -C build-android-aarch64 install
```

Depois empacote `libvulkan_freedreno.so` (de `turnip_workdir/install/lib`) com um `meta.json` como
o de `build_turnip.sh` (`"name": "Turnip R3 + baricentricas (SotC)"`) em
`drivers/Turnip_R3_barycentric_SotC.zip`.

Licença: MIT, como o Mesa.
