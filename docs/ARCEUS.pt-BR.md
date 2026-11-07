<div align="center">

# Pokémon Legends: Arceus no ZA Recomp

**Experimental.** O mesmo recompilador estático e o mesmo runtime, agora rodando um segundo jogo da Game Freak.

[English](ARCEUS.md) · **Português (Brasil)**

</div>

---

O ZA Recomp foi feito para *Pokémon Legends: Z-A*. *Pokémon Legends: Arceus* usa a mesma família de engine e, com as
mudanças deste branch, roda nativamente no PC: o código ARM64 é recompilado antecipadamente para C, o sistema e a GPU
do Switch são reimplementados sobre Vulkan, e nenhum emulador roda enquanto você joga.

> [!IMPORTANT]
> **Você precisa da sua própria cópia do jogo** (o jogo base e a atualização 1.1.1, extraídos do seu Switch, com as
> suas `prod.keys` / `title.keys`). Nada do jogo vem aqui: nem código, nem assets, nem fontes, nem saves.

> [!WARNING]
> É um trabalho inicial. Foi jogado no **Linux** (GPU integrada Intel) de um jogo novo até a Vila Jubilife. Os builds
> de Windows e macOS ainda não foram testados com o Arceus.

## O que funciona

| | |
|---|---|
| Inicialização, menu de idioma, introdução, escolha do nome | ✅ |
| Praia das Obsidian Fieldlands, Laventon e os três iniciais | ✅ |
| Vila Jubilife, sede da Equipe Galáxia, diálogos da história | ✅ |
| Personagens, texto dos diálogos, exposição / iluminação | ✅ |
| Taxa de quadros (GPU integrada Intel Raptor Lake, resolução 75 %) | ~30 fps |
| Traduções de fãs e outros mods de `romfs` | ✅ (`mods/<nome>/romfs`) |
| Teclado, controle (SDL3) | ✅ |

## Versão suportada

| Jogo | Title ID | Versão | Build ID do `main` |
|---|---|---|---|
| Pokémon Legends: Arceus | `01001F5010DFA000` | **1.1.1** (update v262144) | `AEE8F150DDA1B5A8` |

Outras versões não são reconhecidas: o gerador e a tela inicial conferem o build ID.

## Como configurar

1. Coloque os seus pacotes numa pasta: o `.nsp` / `.xci` do jogo base, o `.nsp` da atualização 1.1.1 e as suas chaves.
2. Abra o ZA Recomp e escolha essa pasta na tela inicial. O `zaimport` lê os pacotes do Arceus como faz com os do
   Z-A e prepara `exefs/` e `romfs.bin`.
3. Deixe a tela inicial compilar o código do jogo. Ela gera `main`, `sdk` e os dois módulos extras do Arceus
   (`subsdk0`, `subsdk1`) e depois os compila (de 10 a 60 minutos, uma vez só).
4. Jogue.

### Traduções e mods

Mods que substituem arquivos do jogo vão em `mods/<nome>/romfs/` (ou pela página **Mods**). Por exemplo, uma tradução
de fã cujo arquivo traz `01001F5010DFA000/romfs` vai em `mods/<qualquer nome>/romfs`. Mods nunca vêm junto: baixe dos
autores.

## O que mudou para ele rodar

O Arceus difere do Z-A em alguns pontos que o runtime tratava como fixos:

- **Dois módulos de código extras.** O Arceus traz `subsdk0` e `subsdk1`. O loader os carrega em `0x700a000000` /
  `0x700b000000`, o gerador os recompila, a biblioteca do jogo os lista (`GAMELIB_ABI 2`) e a tela inicial os compila.
- **Uma reserva de 32 GB de endereços.** O alocador principal reserva 32 GB de uma vez, então a região alias cresceu
  para 48 GB, até o topo do espaço de 39 bits.
- **Heap e pool físico ao mesmo tempo.** O Z-A não tem heap; o Link's Awakening só tem heap. O Arceus define um heap
  pequeno e guarda a memória gráfica num pool físico de 3,1 GB. A janela de memória da GPU agora pega o maior dos dois.
- **Storage buffers mais abaixo nesse pool.** Em GPUs integradas só o topo do pool é importado para a memória global
  dos shaders; para um jogo com heap agora é o 1 GB do topo (onde ficam as letras e os buffers da exposição do Arceus).
  Importar o pool inteiro também corrigia o texto, mas prendia 3,2 GB e travava uma máquina de 7,4 GB no swap.
- **Uma jump table que o gerador não resolvia.** O índice é lido da memória antes da checagem de limites; a emulação
  do gerador agora pula uma leitura que falha ali em vez de desistir.
- **Serviços**: `friend:u/a/v` (um Switch offline, sem amigos).
- **Depuração**: o despejo de frame do F9 agora captura o frame inteiro com a thread de render ligada.

## Problemas conhecidos

- Um crash raro nas threads de trabalho do jogo (um objeto procurado com um id inválido). Visto uma vez na sede da
  Equipe Galáxia, não se repetiu.
- Um fragment shader com atômico de 64 bits não é traduzido (`atom size 2`); nada visível depende dele até agora.
- O build completo do Arceus pela tela inicial (do zero) ainda não foi feito de ponta a ponta; a biblioteca usada nos
  testes foi compilada com `tools/za/build-linux.sh`.

## Relatar um problema

Abra uma issue com o `logs/play.log` (ou o log impresso no terminal), o que você estava fazendo e uma captura (F10).
Num travamento, o log tem um despejo `[hang]`; num crash, um relatório `[crash]`.
