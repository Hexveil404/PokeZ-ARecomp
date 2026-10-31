# Ferramentas de desenvolvimento

Ferramentas para trabalhar no próprio ZA Recomp: **ZA DevTools**, gravação e replay, captura completa de frame e os
scripts que leem essas capturas. Quem só joga não precisa de nada disto. Tudo fica desligado até você ligar, então uma
execução normal se comporta do mesmo jeito com ou sem estas ferramentas. Por enquanto é só para Windows, exceto a parte
de replay do runtime.

> **Nunca faça commit nem anexe arquivos da Nintendo:** chaves, arquivos do jogo, `extracted/`, saves, capturas e prints
> do jogo. Uma gravação contém uma cópia do save, e uma captura completa contém dados do jogo. Mantenha as duas fora do
> git e das issues.

English: [dev-tools.md](dev-tools.md).

## Preparação

Você precisa dos seus próprios arquivos do jogo já preparados (`exefs/` + `romfs.bin`) e de um `lasgame.dll` compilado.

1. Compile o jogo do jeito normal (veja "Building" no `CLAUDE.md`).
2. Coloque o `lasrecomp.exe` numa pasta só dele, com as DLLs. O arquivo precisa manter esse nome: o código do jogo importa
   o runtime como `lasrecomp.exe`, então um exe renomeado carrega um segundo runtime e crasha ao iniciar.
3. Compile a ferramenta:

   ```bash
   bash tools/devtools/build.sh
   ```

   Isso cria `build/devtools/zadevtools.exe` e um `devtools.ini` ao lado dele.
4. Preencha o `devtools.ini`. Ele fica fora do git, porque guarda os seus caminhos:

   | Chave | O que é |
   |---|---|
   | `work` | Pasta das sessões, repros e capturas. O jogo roda com ela como pasta de trabalho. |
   | `exe` | O `lasrecomp.exe` do passo 2. |
   | `game` | Os seus arquivos do jogo. |
   | `lib` | O `lasgame.dll` a usar (a opção "DLL: dev"). |
   | `release_lib` | Opcional: um compilado pela tela inicial (a opção "DLL: release"). |
   | `profile` | `nome \| pasta do save \| arquivo de configurações`, uma linha por save. |

## ZA DevTools

Abra o `zadevtools.exe`, escolha um perfil e clique em **Start**. Ele inicia o jogo com uma cópia própria do save e com
`LARECOMP_DEVTOOLS=1`, e guarda o que recebeu mesmo depois de o jogo fechar ou crashar. Ele reconecta sozinho.

Para ligar a ferramenta a um jogo que você mesmo iniciou, defina `LARECOMP_DEVTOOLS=1` e aperte **F10** no jogo. O jogo
procura o `zadevtools.exe` ao lado do próprio exe, ou no caminho em `ZADEVTOOLS`. Sem essa variável, F10 continua sendo a
tecla de captura de tela.

| Aba | Para que serve |
|---|---|
| Log | Log ao vivo, com filtro por tag, busca e opção de salvar. |
| Performance | Tempos de frame (média, 1%, pior), fps, espera do render e número de draws. Um gatilho de queda salva `captures/<hora>/`. |
| Frame | Navegar numa captura: passes, draws, shaders e quais draws tocaram o ponto clicado. |

Duas opções mudam como o jogo roda:

- **Race test** define `LARECOMP_SYNC_SUBMIT=1`, e cada lista de comandos espera a GPU. Se um bug some com isso, é uma
  corrida entre CPU e GPU.
- **DLL: dev / release** escolhe a biblioteca do jogo, para comparar um build de desenvolvimento com o que os jogadores
  recebem.

### Reproduzir um bug

Toda execução iniciada pela ferramenta fica gravada em `testes/sessions/<hora>/`, dentro de `work`.

1. Jogue até o bug aparecer na tela e clique em **Mark bug here**. A sessão vira `repros/<hora>/`.
2. Clique em **Replay to the bug**. Uma cópia nova daquele save roda a gravação até a marca, e dali em diante o controle é
   seu.
3. Para abrir uma repro direto: `zadevtools.exe --replay <nome da repro>`. `--start <número do perfil>` inicia um perfil.

## Gravar e reproduzir por script

```bash
export EXE=/c/za-dev/bin/lasrecomp.exe GAME=/c/za-dev/game LIB=/c/za-dev/game/lasgame.dll
SAVE=/c/za-dev/saves/test/save bash tools/za/replay-record.sh cidade      # jogue e feche o jogo
bash tools/za/replay-play.sh cidade 120                                   # reproduz por até 120 s
```

A gravação fica em `replays/<nome>/`, dentro de `WORK` (padrão: a pasta atual), e o log do replay em
`replays/<nome>/run/replay.log`. `SHOT=<frames>` salva prints, e `SAMPLE=<atraso,duração>` faz o perfil da execução.

Tenha em mente estes limites:

- O replay segue as leituras de controle do próprio jogo, então não é idêntico pixel a pixel. Dois replays do mesmo build
  podem diferir bastante numa comparação de imagens.
- Uma gravação pode sair de sincronia depois de uma mudança no código do jogo ou de tempo, e o personagem acaba andando
  contra uma parede. Grave de novo.
- O replay usa as configurações gravadas. Sem `fps60 = 1` nelas, o jogo roda a 30 fps.
- Não deixe um terminal dentro de `replays/<nome>/run`: isso bloqueia a limpeza no replay seguinte.

## Captura de frame

| Como | Resultado |
|---|---|
| **F9** com `LARECOMP_DEBUG_FRAMES=1` | Uma imagem por passe: `build/pass_<frame>_NNN.png`. |
| **F9** também com `LARECOMP_FULLCAP=1` | `build/fullcap_<frame>.bin`, cerca de 170 MB: grades de cor e profundidade em volta de cada draw, constant buffers, storage, vertex streams, texturas e dispatches de compute. |
| `LARECOMP_DEBUG_FRAME=<frame>` | O mesmo num frame escolhido, sem apertar tecla. |
| `LARECOMP_DUMP_GLSL=1` | Shaders traduzidos em `build/glsl/`. |

As grades vêm em pares, antes e depois de cada draw, então dá para atribuir uma mudança ao draw exato.

### Scripts de análise

`tools/devtools/analysis/` lê as capturas completas. O `fcread.py` é o carregador, e os outros se apoiam nele:

```bash
python -I tools/devtools/analysis/cells.py tools/devtools/analysis build/fullcap_3510.bin <draw>
```

| Script | Responde |
|---|---|
| `cells.py`, `cells2.py` | Qual draw pintou cada célula da grade num passe? |
| `who.py` | Quais draws tocaram uma faixa de endereços? |
| `mesh.py`, `strides.py`, `brute.py` | Como são os vértices de um draw, e qual layout serve para eles? |
| `inst.py`, `argscheck.py` | Os argumentos indiretos da GPU e as instâncias fazem sentido? |
| `clip.py`, `raster.py`, `raster_inst.py` | Onde os triângulos de um draw caem na tela? |
| `cb9.py`, `sb0.py`, `d90.py` | Dumps de buffers específicos, de investigações antigas: pontos de partida para adaptar. |

## Opções de depuração

| Variável | Efeito |
|---|---|
| `LARECOMP_HIDDEN=1` | Roda sem janela na tela, para testes por script. |
| `LARECOMP_INPUT=<frame:BOTÃO:frames,...>` | Entrada de controle por script. |
| `LARECOMP_SHOT_AT=<frames>` | Salva a imagem do jogo nesses frames em `build/frames/`. |
| `LARECOMP_SHADER_FLAT=0/1` | Forma de shader em linha reta desligada / ligada. |
| `LARECOMP_SHADER_QUAD=0` | Derivadas por IDs de subgroup em vez de operações de quad. |
| `LARECOMP_VK_INTEL_RENDER_PASSES=0/1` | Render passes em vez de dynamic rendering. |
| `LARECOMP_POOL_WATCH=<end>:<tam>` | Registra o que muda uma faixa da memória do jogo. |
| `LARECOMP_ARGS_TRACE=<frame>:<ini>:<tam>` | Registra mudanças nos argumentos de draws indiretos. É lento. |
| `LARECOMP_FS_MBPS=<n>` | Limita as leituras da RomFS à velocidade de um armazenamento lento. |

O `CLAUDE.md` tem a lista de desligadores e opções de perfil.

## Logs

| Arquivo | O que guarda |
|---|---|
| `logs/launcher.log` | Histórico da tela inicial, instalação e compilação, mantido entre execuções. É o arquivo a pedir em falhas de instalação e compilação. |
| `logs/play.log` | A última execução. A anterior fica em `logs/play-anterior.log`. |
| `lasrecomp-backup-*.log` | Execuções de antes de o jogo estar instalado, ao lado do exe. |
