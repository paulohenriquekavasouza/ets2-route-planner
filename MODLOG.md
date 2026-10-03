# MODLOG — ETS2 Planejador de Rotas (RoutePlanner)

## Ideia
F8 no jogo abre uma janela: país + cidade de origem, país + cidade de destino, lista de cargas
possíveis entre as duas, "Iniciar serviço" (começa na hora) e o serviço atual com "Cancelar".
Só single-player. Origem: conversa "Caminhão autônomo ETS2" (mesmo setup do `ets2-police`).

## Ambiente (2026-10-02)
- ETS2 1.61.1.1 (Steam), SPF-Framework v1.2.4 já instalado (ver `D:\Projetos\ets2-police\MODLOG.md`).
- Plugin: `bin\win_x64\plugins\spfPlugins\RoutePlanner\` (`RoutePlanner.dll` + `routes.tsv`).
- PolicePatrol desativado no SPF (usuário desligou às 21:14; `plugin_states.PolicePatrol.enabled=false`).
  RoutePlanner pré-ativado em `spfAssets\config\framework_settings.json` (cópia em `.bak-routes`).
- Saves ficam no Steam Cloud: `C:\Program Files (x86)\Steam\userdata\<id-steam>\227300\remote\profiles\70686B206D656F`.
  Backup: `backup/cloud-profile-2026-10-02.zip` (restaurar = extrair por cima, com o jogo fechado).

## Rota
Mesmo plugin SPF (C++20, DLL única, sem hot reload por enquanto: mudou → deploy.ps1 → SPF
"Recarregar Framework"). Dados do mundo vêm dos defs do próprio jogo (gerados no deploy, nunca
versionados); iniciar/cancelar = chamar funções do exe.

## Fonte da verdade — comandos de console no exe
- `cheat job <carga> <empresa.cidade> <empresa.cidade>` (ex. do exe:
  `cheat job grain_b gld_frm_grg.mccook myr_wd_mkt.scottsbluff`): handler rva 0x5c0910 → 0x4a0230.
  Só **altera uma oferta existente** no Freight Market da empresa de origem ("No job to hack in
  source company"); não inicia o serviço.
- `cheat get_job <cidade> <cidade> [true]`: handler 0x5c7f20. **Gera e assume** o serviço:
  1. params (0x70 bytes, init como no handler: +0x30 string_t {vtbl rva 0x21d18c0, data "" rva
     0x1df110e}, u16 +0x61 = 1, f32 +0x64 = [rva 0x251d65c]).
  2. `0x82e0b0(params, &src_city_tok, &dst_city_tok)` escolhe empresas ligadas ao acaso e preenche
     +0x00/+0x08 (origem: empresa/cidade, ordem a confirmar) e +0x10/+0x18 (destino).
  3. **+0x20 = token da carga** (0 = aleatória; o gerador busca com 0xab7840).
  4. `0x82ffb0(&res{offer*, int status=1}, params, true, own_trailer)`; status != 0 = erro.
  5. `0x82eed0(params, offer, true, own_trailer, false)` → 0 = ok.
  6. solta a referência (vtbl[1], decremento em +8, free 0xfbf00 se zerar), `0x82e270(params)`,
     `0x11a290(params+0x30)`.
  Códigos de erro → nomes em `const char*[]` rva 0x1e1a830.
  `own_trailer`: player = [[exe+0x3045760]+0x18]; caminhões em player+0x70 (data +0x78, count +0x80);
  1º caminhão vivo e com [+0x48] vivo. ("vivo" = bit 31 do dword em +8.)
- Cancelar: `0x7a5c40(ctrl)` com ctrl = [exe+0x3045760]; os 3 chamadores do jogo checam antes
  `[[ctrl+0x18]+0x28]` (serviço atual) vivo. Aplica a multa de cancelamento normal.
- Plugin compara 10 bytes do prólogo de cada função (game.h) e roda tudo sob SEH.

## Dados (tools/gen_routes.py → routes.tsv)
- `def/city/*.sui` (city_name, country), `def/country/*.sui`, `def/company/<co>.sui` (nome),
  `def/company/<co>/editor/<cidade>.sii` (filial na cidade), `.../out|in/*.sii` (cargas), `def/cargo/*.sui`.
- Extração em `C:\Users\Paulo\ets2-extract` com `scs_extractor_1.50.exe`.
- **Gotcha 1:** o scs_extractor 1.50 só lê o `def.scs` base. Nos `dlc_*.scs` falha com
  "fs_hashfs_v2.cpp(861): Unsupported meta type" (cria só pastas vazias); o `scs_extractor.exe`
  antigo diz "Unsupported HashFS version (v2)". `locale.scs` não tem diretório raiz → nem extrai.
  Resultado v0.1: só mapa base (80 cidades, 36 países, 366 filiais, 321 cargas) e nomes de carga
  derivados do token. Precisa de extrator atualizado para DLCs de mapa e tradução PT-BR.

## v0.1.0 (2026-10-02) — compilado, AINDA NÃO testado no jogo
- F8 abre/fecha (ação `RoutePlanner.Routes.toggle`). Janela "Planejador" interativa; enquanto aberta
  o plugin captura o mouse (override + block, uma única chamada — gotcha 5 do ets2-police).
- Ações do jogo rodam no `OnUpdate` (mesma thread do render/jogo).
- Log de cada início: `start <carga> <emp>.<cid> -> ...: ok|erro | params a b c d` (tokens
  decodificados) → confirma a ordem empresa/cidade em params.
- A conferir no jogo: (1) SPF carrega o plugin após "Recarregar Framework"; (2) cursor aparece;
  (3) params batem com as cidades (senão "Formato de parâmetros inesperado"); (4) serviço começa
  com a carga escolhida; (5) cancelar.

## v0.2.0 (2026-10-02) — 1º teste do usuário
Feedback: texto cortado ao lado dos combos; sem cursor com o F8; "Iniciar" deu
`@@mp_job_missing_target_navigation@@ (6)`.
- **Ordem dos params confirmada pelo log:** +0x00 empresa origem, +0x08 cidade origem, +0x10
  empresa destino, +0x18 cidade destino (`params lkwlog calais euroflow antwerp`). Código fixo nisso.
- **Erro 6:** o jogo só tem navegação pré-calculada entre pares de empresas "ligados"; o par escolhido
  (nbfc.calais → cont_port.antwerp) não era. Agora, ao escolher as duas cidades, o plugin chama o
  sorteio do jogo (0x82e0b0) 400 vezes e guarda os pares distintos; a lista de cargas só mostra
  opções desses pares. Log: "ligações <src>|<dst>: N pares, M cargas".
- Combos: rótulo à esquerda (Text + SameLine) e id oculto `##` — o rótulo do ImGui ficava à direita
  e era cortado com largura -1.
- Cursor: SPF só mostra cursor nas janelas dele; o plugin desenha uma seta no foreground (como a
  inspeção do ets2-police).
- **Dados de todos os DLCs + PT-BR:** sk-zk/Extractor 2026-07-29 (MIT) em `C:\Users\Paulo\tools\extractor`.
  `extractor def.scs dlc_*.scs -S -q -p=/def -d C:\Users\Paulo\ets2-x2` e
  `extractor locale.scs -q -D -p=/locale/pt_br -d C:\Users\Paulo\ets2-x2\locale`.
  **Gotcha 2:** no Git Bash, `-p=/def` vira caminho do Windows → "0 extracted". Usar `MSYS_NO_PATHCONV=1`.
  Traduções: `locale/pt_br/localization.sui` (pares `key[]`/`val[]`), chaves `@@cn_<carga>@@`,
  `city_name_localized`, `name_localized` do país. Resultado: 36 países, 384 cidades, 1831 filiais,
  413 cargas.

## v0.3.0 (2026-10-02) — 2º teste: UI ok, cursor ok, mas erros 7 e 6; seleção sem destaque
- Log: steinkjer → thessaloniki, 1 par ligado (sag_tre_pln → lefko), 3 cargas. `bob_d30` →
  `mp_job_missing_cargo (7)`; `ter_forklift` → `mp_job_missing_target_navigation (6)` mesmo no par ligado.
- **Erro 6 de verdade:** gerar (0x830234) e assumir (0x82f1dc) só checam `params+0x64 >= 0` e
  nenhum dos dois lê o valor depois. O `get_job` põe -1.0 (rva 0x251d65c) → nesta build ele
  sempre falharia. O outro chamador do gerador (0x128b160) copia +0x64 de uma struct de params pronta.
  Agora o plugin escreve 0.0. **Verificar no jogo:** km planejados e pagamento do serviço criado.
- **Erro 7:** gerador busca a carga por token via 0xab7840 ("cargo.%s"); morta/ausente = 7. O filtro
  das opções agora também descarta cargas que o jogo não acha (`game::CargoExists`).
- Seleção: o tema do SPF deixa `Header` invisível → cor própria na linha selecionada.
- Nomes de empresa em DLC: arquivo `<co>.<dlc>.sui` → usar o nome antes do 1º ponto.

## v0.4.0 (2026-10-02) — 3º teste: iniciar e cancelar FUNCIONAM
- Confirmado no jogo: `rice_c renar.steinkjer -> cont_port.durres: ok` com a carga certa; "Serviço
  cancelado." funcionou. O fix de +0x64 = 0.0 resolveu o erro 6.
- O "pares ligados" era falso: 0x82e0b0 devolve sempre o MESMO par por par de cidades (400 chamadas
  → 1 par distinto). O 1º erro 6 (nbfc → cont_port) vinha do -1 em +0x64, não do par. Removidos o
  filtro e a chamada a 0x82e0b0: params são preenchidos direto (+0 empresa, +8 cidade, +0x10, +0x18,
  +0x20 carga).
- Pedido: qualquer carga entre quaisquer cidades (ex.: Honningsvåg → Iráklio, onde nenhuma empresa
  envia o que a outra recebe). Checkbox "Qualquer carga": junta todas as cargas conhecidas, entre a
  1ª empresa de cada cidade, marcadas "(fora do mercado)". NÃO testado: o gerador pode recusar por
  reboque (erro 8) ou outro motivo; o log mostra o código.
