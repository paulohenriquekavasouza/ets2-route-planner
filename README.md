# ETS2 Route Planner

Plugin para o **Euro Truck Simulator 2** (1.61.1.1, Steam, single-player) feito sobre o
[SPF-Framework](https://github.com/TrackAndTruckDevs/SPF-Framework) 1.2.4: escolha origem, destino e carga
dentro do jogo e comece o serviço na hora.

- **Home**: planejador na interface do próprio jogo (pausa o jogo, cursor do jogo, bandeiras, ícones de
  carga, logotipos das empresas). Páginas Planejar, Favoritas, Local e Carga; Esc ou Home fecham.
- **F8**: o mesmo planejador numa janela ImGui do SPF.
- Qualquer cidade para qualquer cidade, com qualquer carga; rotas favoritas; maior rota possível; cidade atual.
- Ao iniciar (opções): teleporte até a empresa de origem, soltar o freio de mão, 7h e tempo limpo, tanque cheio.
- Cancelar o serviço atual ou teleportar até a carga.

Funciona chamando funções do executável do jogo por endereço, conferidas por assinatura: **só vale para a
versão 1.61.1.1**. Em outra versão o plugin se desliga sozinho ("jogo não reconhecido").

## Estrutura

| Caminho | O que é |
|---|---|
| `plugin/Host.cpp` | DLL que o SPF carrega (manifesto, teclas, janela); recarrega o núcleo quando o arquivo muda |
| `plugin/Core.cpp` | Toda a lógica e as duas interfaces |
| `plugin/game.h` | Chamadas ao jogo (serviço, teleporte, combustível, avisos, janelas da UI do jogo) |
| `plugin/routes.h`, `tools/gen_routes.py` | Dados de países, cidades, empresas e cargas, gerados dos arquivos do seu próprio jogo |
| `plugin/SPF_API/` | Cabeçalhos do SPF-Framework (de terceiros) |
| `MODLOG.md` | Diário do projeto: o que foi descoberto no jogo, versão por versão |

## Compilar e instalar

Requisitos: Visual Studio com CMake, Python 3, o SPF-Framework instalado no jogo e os `def` e `locale` do jogo
extraídos (por exemplo com o [Extractor](https://github.com/sk-zk/Extractor)).

```powershell
pwsh deploy.ps1
```

O script tem os caminhos da minha máquina no topo (jogo, extração, CMake): ajuste antes de usar. Ele compila,
roda os testes, gera `routes.tsv` e instala em `bin\win_x64\plugins\spfPlugins\RoutePlanner\`. Nenhum arquivo
do jogo é versionado aqui.

## Branches

- `master`: versão atual (tags `v1.0` … `v3.3.1` são as versões aprovadas).
- `escolta`: experimento parado de uma viatura de polícia que escolta o caminhão.
