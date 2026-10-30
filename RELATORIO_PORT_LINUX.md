# Relatório — Port nativo para Linux do ZA Recomp

Branch `feat/linux-native` (a partir de `pokemon-za`, commit `6e8c929`). Data: 30/10/2026.
Nenhum push foi feito. Nenhum arquivo da Nintendo, chave ou código gerado a partir do jogo foi adicionado ao repositório.

## 1. Resumo

| Etapa | Estado | Como foi verificado |
|---|---|---|
| Compilação do runtime e do launcher (ELF x86-64) | **OK** | `build/linux/x86_64/za/lasrecomp` compila e linka com clang 18 |
| Launcher nativo (sem Wine/Proton) | **OK** | aberto de verdade nesta máquina, Vulkan na Intel, capturas de tela de 4 páginas |
| AppImage | **OK** | `build/dist/ZA Recomp-v0.1.12-linux-x86_64.AppImage` (148 MB, com `zaimport`), aberto e capturado |
| Importação dos pacotes (.nsp base + update) | **OK** | `zaimport` linux-x64: base + update 2.0.2 → build ID `b1f12fd919eae86a`, `romfs.bin` 6,0 GB |
| Recompilação ARM64 → C → `lasgame.so` | **OK** | gerador 2 min 13 s (pico 4,4 GB); compilação 20 min com 3 jobs, 0 erros |
| Jogo rodando nativo | **OK (sem interação)** | 2 sessões de ~4 min: seleção de idioma, abertura em Lumiose; 0 travamentos, 0 erros |
| Áudio | **Parcial** | saída aberta (`[audout] opened 48000 Hz, 2 ch`); não foi ouvido |
| Desempenho na Intel UHD | **Abaixo de 30 fps em 3D** | menus a 60 fps; cenas 3D 9–26 fps (§5) |
| Controles, DualShock 4, saves, mods | **Não testado** | as sessões rodaram sem entrada do jogador |

## 2. Auditoria (estado inicial)

- O suporte a Linux já existia, herdado do LAS Recomp: `runtime/platform/linux.c`, seções ELF para as tabelas HLE,
  `lasgame.so`, backend Vulkan compartilhado com o Windows, rastreamento de escrita em páginas (userfaultfd +
  PAGEMAP_SCAN), scripts de AppImage em `tools/linux/` (podman, Ubuntu 22.04).
- As correções do Steam Deck (PRs #7–#12) **estão presentes**: compute em subgrupos de 32 lanes (`d83648c`), índices
  fora do espelho de transform feedback (`ca6c21e`), heap do guest importado como janela em GPUs integradas
  (`c72125e`), SSY/PBK em escalares e rótulos de BRX (`5a9fb4a`), semântica de warp de 32 em subgrupos de 64
  (`95d48f2`) e depth clamp (`15e4d22`, `c79eca0`).
- O que só existia para Windows no lado do Z-A: o empacotamento (`tools/za/dist.sh`, com `zaimport`, `zaprompts`/`ooz`,
  `lasgen`, zig), o `ooz` (escrito para MSVC) e as configurações padrão da release (`larecomp.ini` ao lado do exe).
- O AppImage do upstream (`tools/linux/package.sh`) é do Zelda: nome "LAS Recomp", `rando-gen`, sem `zaimport`/`zaprompts`.
- O auto-update do launcher é só do Windows; no Linux ele mostra o link da release (comportamento já existente, mantido).

## 3. Máquina de teste

Linux Mint 22.3 (base Ubuntu 24.04), kernel 7.0.0-42, glibc 2.39, sessão X11/Cinnamon. Intel Core i5-13420H
(8 núcleos/12 threads, AVX2/FMA/BMI2 → código do jogo em x86-64-v3). **7,4 GB de RAM** + 7 GB de swap. GPU Intel
UHD (Raptor Lake-P), Mesa 25.2.8 ANV, Vulkan 1.4.318; o runtime reporta 5,6 GB de memória de vídeo (compartilhada) e um
orçamento de ~2,3 GB no heap 0, por isso o heap do guest é importado em blocos.

## 4. Mudanças feitas (commits locais)

| Commit | O quê |
|---|---|
| `bde7837` | FSR 2 compila no Linux: `-DFFX_GCC` (sem ele toda função é `__declspec(dllexport)`), `stddef.h` para `size_t`, `wcscpy_s`. `rpath` para `DEPS` (build de desenvolvimento roda sem `LD_LIBRARY_PATH`). |
| `aee89ca` | Pasta de dados no Linux: `~/.local/share/zarecomp` em vez de `lasrecomp` (com os dois instalados, saves, jogo e `larecomp.ini` seriam compartilhados com o LAS Recomp). |
| `e92f089` | `ooz` compila como `libooz.so` por headers substitutos (`tools/za/ooz_posix/`), sem alterar os arquivos do Powzix em `third_party/ooz`. |
| `7299781` | `tools/za/build-linux.sh`: build nativo reproduzível (verifica pacotes e imprime o `apt install` do que falta). |
| `9282558` | Primeira execução fora do Windows: copia o `za-default.ini` do app para a pasta de dados (60 fps, upscaler, resolução da release). |
| `52aefc8` | `tools/za/dist-linux.sh`: AppImage do Z-A (zig, lasgen, zaprompts + ooz, zaimport linux-x64, fontes, `.desktop` próprio). |
| `c7d2b68` | `docs/linux.md` (seção Z-A), READMEs, este relatório. |
| `830dc3f` | `build-linux.sh GAME=1`: funções gigantes em `-O0`, como no build do launcher (160 arquivos na 2.0.2). |

Nenhuma das mudanças altera o `GAMELIB_ID`: jogadores do Windows não precisam recompilar o jogo.

## 5. Testes realizados

**Compilação**
- Runtime, backend Vulkan, UI, ImGui e FSR 2: compilam e linkam (clang 18, lld). Dependências fixadas por sha256 via
  `tools/linux/build_deps.sh`: SDL3 3.4.16, FFmpeg 9.0.2 (LGPL, só os codecs dos vídeos), shaderc 2026.3, headers Vulkan.
- `nm` confirma que não há código do jogo (`f_70…`) no executável.

**Launcher (janela real nesta máquina)**
- Páginas Jogar, Configurações, Sobre: renderização correta, fonte Baloo 2, acentos do PT-BR. Log:
  `[vk] using Intel(R) Graphics (RPL-P)`, swapchain mailbox, recorder thread ativa.
- AppImage: abre direto (FUSE), encontra os recursos ao lado do executável, grava o log em `<dados>/lasrecomp.log`, e na
  primeira execução cria o `larecomp.ini` a partir do `za-default.ini`.
- Observação: em uma das execuções, `vkWaitForPresentKHR` retornou `-4` uma vez na abertura da janela; o vigia de
  apresentação recriou o swapchain e tudo seguiu normal. Não se repetiu em execuções mais longas.

**Ferramentas de recompilação (sem os arquivos do jogo)**
- `test_flagfuse.py`: 208 casos, 0 divergências. `test_regpromote.py`: 0 divergências.
- `test_simd.py` (lowering de FP/SIMD contra o Unicorn): 159.968 casos; **0 falhas** com `-DLARECOMP_NO_HOST_FTZ`,
  107 falhas com o caminho rápido de FTZ do host (ver §7).
- `test_translate.sh` (Maxwell → GLSL → SPIR-V): **2.179 shaders compilados e validados, 0 falhas**.
- Helpers do AppImage: `lasgen` (0 erros ao abrir), `zaprompts --xbox-attack-layout-version` (schema 1), `zig 0.17.0`.
- Caminho de compilação do jogo no computador do jogador: `zig cc -target x86_64-linux-gnu` com as flags reais
  (`-O1 -DGUEST_VOLATILE … -mcpu=x86_64_v3 -fPIC`) compila o `gamelib_entry.c` e linka um `lasgame.so`.

**O jogo (Pokémon Legends: Z-A 2.0.2, dump do próprio usuário)**
- Importação: `zaimport` (compilado para linux-x64) leu o `.nsp` base e o update v2.0.2 com as chaves do usuário,
  sem modificá-los, e aplicou o update. Saída em `extracted/` (fora do git).
- `make gen`: 1.231 blocos (`main`) + 214 (`sdk`), 980 MB de C, 2 min 13 s, pico de 4,4 GB de RAM.
- Compilação: 1.448 objetos, 160 funções gigantes em `-O0`, 20 min 10 s com 3 jobs, pico de 1,1 GB por compilador.
  `lasgame.so` de 364 MB, id `594b2d2146060967-B1F12FD919EAE86A`; carregado com 230.332 + 40.069 funções.
- Execução (`lasrecomp extracted --play`, sem entrada do jogador, encerrada pelo timeout): 60 fps ativado, tela de
  seleção de idioma correta (todos os alfabetos), cena de abertura com a Torre Prisma renderizada completa. Nenhum
  erro, travamento ou perda de dispositivo Vulkan no log.
- Desempenho (Intel UHD Raptor Lake, janela 1280×720, FSR1 + SMAA):

  | Resolução interna | Menus/transições | Cenas 3D (abertura) | Mediana geral |
  |---|---|---|---|
  | `1.5x` (padrão da release, 2880×1620) | 28–30 fps | 10–16 fps | 11,9 fps |
  | `match` (padrão do runtime para iGPU) | 60 fps | 15–26 fps, ~10 na cena mais pesada | 18,1 fps |

  O `za-default.ini` da release força `resolution = 1.5x`, escolhido para GPUs dedicadas. Numa integrada, o padrão
  do próprio runtime (`match`) rende ~50% mais. Ainda não chega a 30 fps estáveis nesta GPU.

## 6. Pendências

1. **Jogar de verdade**: teclado, mouse, DualShock 4 (rumble/LED/giroscópio pedem acesso a `/dev/hidraw*`, regras
   udev do Steam), salvar e carregar, mods, áudio ouvido. As sessões de teste rodaram sem entrada.
2. **Desempenho na Intel UHD**: medir com `LARECOMP_VK_PROF=1` se o gargalo é a GPU ou a CPU (thread de renderização),
   e decidir se o `za-default.ini` deve deixar a resolução em `auto` para GPUs integradas.
3. **Memória**: 7,4 GB nesta máquina (o README pede 8). Funcionou nas sessões curtas; sessões longas não testadas.
4. **AppImage portável**: o `dist-linux.sh` usa a glibc desta máquina (2.39: roda no Ubuntu 24.04/Mint 22 ou mais
   novos). Para distribuições mais antigas, adaptar o caminho do upstream com podman (`tools/linux/appimage.sh`).
5. **Fluxo completo pelo launcher**: a importação e a compilação foram feitas pelo caminho de desenvolvimento
   (`zaimport` + `build-linux.sh GAME=1`). O botão "Preparar o jogo" do AppImage (zaimport + lasgen + zig dentro do
   app) ainda não foi exercitado de ponta a ponta. Ele escolhe `min(núcleos, RAM_GB − 2)` = 5 jobs nesta máquina.
6. **`libooz.so` / ícones dos botões**: o `zaprompts` só roda pelo fluxo do launcher (item 5).

## 7. Problemas encontrados fora do escopo Linux (não corrigidos)

- **FTZ do host "vaza" para operações com FPCR=0** (`runtime/simd_fp.h:150`, commit `cd0ce96`): com `fpcr=0` o
  `&&` faz curto-circuito e `simd_fz_host` não roda, então o FTZ|DAZ do MXCSR ligado por uma operação anterior com FZ
  continua ativo e denormais viram zero. O comentário em `runtime/simd.h` diz que "qualquer outro FPCR" limpa os bits,
  mas isso não vale para FPCR=0. Afeta Windows e Linux (x86). Impacto provável pequeno (o jogo roda com FZ). Corrigir
  muda `simd.h`/`simd_fp.h` e, portanto, o `GAMELIB_ID`, o que obriga todos os jogadores a recompilar. Por isso a
  decisão fica com os mantenedores.
- `tools/test_translate.sh`: o commit de referência `652e234` não existe neste fork, e a lista de renomeações está
  desatualizada (símbolos duplicados no link). Testado aqui com `REF=HEAD` e `--allow-multiple-definition` no modo GLSL.
- `tools/test_vk_pipelines.sh`: não linka mais (símbolos que mudaram de arquivo). Não executado.
- Mensagens do runtime ainda citam "LAS Recomp" em alguns diálogos (ex.: pasta de dados indisponível).

## 8. Como reproduzir

    git switch feat/linux-native
    sudo apt install …                  # lista em docs/linux.md / §6
    tools/za/build-linux.sh             # launcher: build/linux/x86_64/za/lasrecomp
    ./build/linux/x86_64/za/lasrecomp   # da raiz do repositório
    tools/za/dist-linux.sh              # AppImage em build/dist/
    # com o próprio dump em extracted/ (exefs/ + romfs.bin, ex.: zaimport prod.keys extracted base.nsp update.nsp):
    GAME=1 JOBS=3 tools/za/build-linux.sh
    build/linux/x86_64/za/lasrecomp extracted --play

## 9. Licenças e créditos

ZA Recomp e LAS Recomp: MIT (`LICENSE`), créditos ao Peiikai e aos colaboradores (README, página "Sobre" do launcher,
preservada). `ooz`: GPLv3, mantido sem alterações. `tools/za/ooz_posix` existe só para compilá-lo e vai com o
`zaprompts` junto do `COPYING`. FFmpeg compilado como LGPL (o script de dependências recusa GPL). SDL3 (zlib), shaderc
(Apache-2.0), zig (MIT), FSR 2 (MIT), fontes Baloo 2 / BIZ UDPGothic (OFL) e DejaVu: licenças copiadas para o AppImage.
