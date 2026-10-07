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

## v0.5.0 (2026-10-02) — "qualquer carga" confirmado; teleporte para a empresa
- Usuário: "perfeito! funcionou" (qualquer carga/qualquer par). Seleção marcada com "> ".
- Teleporte: o exe tem `cheat company_portal <empresa> <cidade>` (handler 0x5c9e00/0x5c9ea5):
  procura `company.volatile.<empresa>.<cidade>` ("Unknown company instance"), o ponto de teleporte
  (0x6d9980, "No teleport point found for the company"), teleporta (0x7b47b0) e depois tenta trocar
  o GPS ("Unable to override gps while on job" — só aviso, vem depois do teleporte).
  Plugin roda o comando pelo `SPF_GameConsole_API` logo após iniciar (checkbox, ligado por padrão);
  manifesto exige o hook "GameConsole". NÃO testado no jogo.

## v0.6.0 (2026-10-02) — teleporte direto, peso e ordenação
- **Gotcha 3:** o console da versão de varejo NÃO tem o comando `cheat` (game.log:
  `'cheat company_portal renar honningsvag' - unknown command`). Os handlers existem no exe.
  Agora `game::TeleportToCompany` chama o handler 0x5c9e00 direto com um `array_t<string_dyn_t>`
  falso: dados em +0x18, contagem em +0x20, strings de 32 bytes com `char*` em +8 (acessor 0x113030).
  rcx não é lido pelo handler. Hook GameConsole não é mais pedido. NÃO testado no jogo.
- Peso: `gen_routes.py` estima o maior carregamento num reboque simples padrão compatível
  (`vehicle/trailer_defs/*.sii`: body_type, volume, gross_trailer_weight_limit − chassis_mass −
  body_mass; carga: body_types[], mass, volume por unidade). É estimativa: o jogo pode sortear
  outro reboque. Coluna 4 da linha G (kg). Ex.: Maçãs ~23 t, Cimento ~29 t, Guindaste móvel ~36 t.
- Ordenação: Carga (A–Z), Mais pesada, Mais leve, Empresa de origem, Empresa de destino
  (`SortOptions`, estável; a seleção acompanha a opção).
- Alguns nomes PT-BR trazem `\n` literal (ex. "Paletes vazios\n") → trocado por espaço.

## v0.7.0 (2026-10-02) — carga engatada na hora (modo Quick Job), erro 18 contornado
- Teleporte via handler 0x5c9e00 rodou mas o jogo logou "Teleported from X to X" (mesma posição).
  Removido. Usuário descobriu que, chegando à empresa (noclip `0` + Ctrl+F9 = ação `teleport` do
  modo dev, leva o caminhão até a câmera livre), o reboque aparece.
- **3º argumento de gerar/assumir = mercado de fretes (true) vs Quick Job (false).** Em TAKE,
  `[rbp+0x2b0] = !r8b`; com false entra no bloco 0x82f738: sem reboque próprio calcula o spawn da
  empresa (`0x8838f0(&pos, company+0x58, truck)`) e chama `0x6057a0` / `0x5f8560(..., &pos, 1)`,
  que põe caminhão+reboque lá; com reboque próprio usa o reboque atual. Em GEN, r8b=true só
  acrescenta a checagem 0x495e50 (erro 9). O `cheat get_job a b true` usa false (dil = arg4 != "true").
  Plugin agora passa false nos dois. NÃO testado: pode vir um caminhão de Quick Job (alugado).
- **Erro 18 (mp_job_country_cargo_allowance_issue):** GEN chama a calculadora de unidades
  0x84f0e0 (unidades = min(volume do reboque / volume da carga, (limite de peso do reboque
  ajustado pelos países da rota) / massa da carga)); 0 → erro 18 em 0x830bc2. Patch de 14 bytes em
  0x830bb3 (`test eax,eax; jnz +5; mov eax,1; mov r12d,eax; jmp 0x830bcc`) aplicado só durante o
  GEN do plugin (`__finally` restaura). `Supported()` também confere os bytes originais.
- "Qualquer carga" vem marcado.

## v0.8.0 (2026-10-02) — sem caminhão alugado; GPS
- Usuário: o modo Quick Job trocou o caminhão dele (não quer) e o GPS parou de recalcular a rota.
- **De onde vem o caminhão alugado:** GEN com r8b=false (quick) grava o nome de um caminhão em
  oferta+0x68 (0x830cb9: só quando não é mercado de fretes); TAKE monta o veículo do serviço com
  `0x79b630(job, job+0x68, job+0x88, job+0xa8, ...)`. No TAKE, o caminho quick (0x82f738) só cuida do
  reboque: sem reboque próprio cria um (0x6fe330), calcula o spawn da empresa (0x8838f0) e posiciona
  (0x6057a0, 0x5f8560).
- Agora: **GEN em modo mercado de fretes (true)** = sem caminhão na oferta, unidades calculadas
  com o caminhão do jogador; **TAKE em modo quick (false)** = spawn na empresa com reboque engatado.
  NÃO testado: pode ser que o TAKE quick dependa do caminhão da oferta.
- Depois de assumir, a tela de fretes do jogo faz `[exe+0x36ae748]+0x254 = 1` (flag genérica de
  "estado mudou", 65 lugares no exe a ligam, inclusive o cancelamento). O plugin agora faz igual.
  Palpite para o GPS; não confirmado.

## v0.9.0 (2026-10-02) — volta ao que funcionava (v0.6) + flag do GPS
- v0.8 (GEN freight + TAKE quick) quebrou: "não consigo criar carga nenhuma". Mistura de modos não serve.
- Pedido do usuário: voltar ao estado antes de "spawnar a carga no caminhão" e aplicar só o GPS.
  game.h e RoutePlanner.cpp restaurados da v0.6.0 (6f57c23): GEN(true) + TAKE(true), sem o patch do
  erro 18 (fica para depois, se ele pedir). Mantido: teleporte removido (ele pediu), "Qualquer carga"
  marcado por padrão, flag `[exe+0x36ae748]+0x254 = 1` após assumir (palpite para o GPS).

## v0.6 restaurada (2026-10-02)
- Pedido do usuário: voltar exatamente à v0.6.0. `plugin/`, `tools/` e `deploy.ps1` = commit 6f57c23
  (com o teleporte via handler, que não funciona; "Qualquer carga" desmarcado; sem flag de GPS; sem
  patch do erro 18). v0.7–v0.9 continuam no histórico do git.

## Restaurado o estado de 611e09f (2026-10-02)
- Pedido do usuário: voltar para antes do pedido de teleporte. `plugin/`, `tools/`, `deploy.ps1` =
  commit 611e09f (v0.4.0 + "> " na carga selecionada): sem teleporte, sem peso/ordenação, "Qualquer
  carga" desmarcado. Iniciar/cancelar e qualquer par/carga confirmados funcionando nesse estado.
  v0.5–v0.9 seguem no histórico.

## v0.4.1 (2026-10-02) — só a correção do erro 18 sobre 611e09f
- Patch de 14 bytes em 0x830bb3 ("0 unidades vira 1") aplicado só durante o GEN do plugin, com
  `__finally` restaurando; `Supported()` confere os bytes originais. Resto igual a 611e09f.
  NÃO testado no jogo.

# ===== v1.0 (2026-10-02) =====
- Usuário aprovou ("PERFEITO!"): estado 611e09f + correção do erro 18 = **v1.0.0** (tag git `v1.0`).
  Daqui pra frente tudo é 1.x. Backup completo em `D:\Projetos\_backupsts2-routes1.0\`
  (fonte, `RoutePlanner.dll` e `routes.tsv` instalados) + `v1.0.zip`; restaurar = ver LEIA-ME.txt lá.

## v1.1.0 (2026-10-02) — experimento: carga engatada ao iniciar, sem caminhão alugado
- Teleporte via company_portal descartado de novo (usuário: "não precisa de teleporte"). A v1.0 foi
  reinstalada byte a byte do backup antes deste experimento.
- TAKE 0x82ef8e: `movzx eax,r8b; lea rdx,[rbp+0xb0]; xor al,1; mov [rbp+0x2b0],al`. [rbp+0x2b0] liga o
  bloco de posicionamento do Quick Job (cria o reboque 0x6fe330, spawn da empresa 0x8838f0,
  0x6057a0/0x5f8560 engatam) e, com r8b=true, 0x82fa8f ainda chama 0x5ddf20(actor, &pos) = move o
  caminhão do jogador para lá. O caminhão alugado vem do GEN em modo quick (oferta+0x68), que NÃO usamos.
- Patch de 2 bytes em 0x82ef99 (`34 01` → `b0 01`, `mov al,1`) só durante o TAKE do plugin, com
  `__finally`. Caixa "Já sair com a carga engatada (experimental)", ligada por padrão; desligada = v1.0.
  NÃO testado no jogo.

## v1.2.0 (2026-10-02) — v1.1 falhou; v1.0 + teleporte para a empresa
- v1.1 (patch 0x82ef99): 1ª tentativa = exceção 0xC0000005 dentro do TAKE (pega pelo SEH; estado do
  jogo pode ter ficado inconsistente → recomendado recarregar o save); 2ª = `mp_job_trailer_not_created`
  (14) com "Trailer position is occupied by player or trailer!" no game.log. Abandonado.
- **Reinterpretação do teleporte "que não funcionava":** o caminhão estava a ~73 m do pátio da Renar
  em Honningsvåg (posição [37707;2;-119822] vs. pátio [37634.9;2;-119831]) — os testes eram feitos
  já na cidade/empresa de origem. "Teleported from A to A" também aparece em eventos normais do jogo.
- v1.2.0 = v1.0 + teleporte via handler do `company_portal` (0x5c9e00) 10 quadros depois de iniciar
  (caixa "Ir até a empresa de origem ao iniciar", ligada) + botão "Ir até a carga (teleporte)" no
  serviço atual (usa source_company_id/source_city_id da telemetria). Log: posição do caminhão
  (telemetria) antes, logo depois e 1 s depois. Testar com origem em OUTRA cidade.

## v1.3.0 (2026-10-02) — teleporte para o pátio da empresa; retry no erro 14
- v1.2 (handler company_portal): "falhou" em todas, caminhão parado (log com posição da telemetria).
  `re`-probe (`scratchpad/portalcheck.py`): as checagens iniciais do handler passam → ele cai em
  "No teleport point found for the company" (0x6d9980; portais são raros). Abandonado.
- **RE ao vivo do serviço atual (Norrsken, Alta):** job = [[ctrl+0x18]+0x28]; **[job+0x28] = empresa**
  (company.volatile); **[empresa+0x10] = item do mapa da empresa** (tipo 6; bbox +0x0c/+0x20;
  +0x58 aponta de volta para a empresa). **item+0x70 (dados) / +0x78 (qtd) = vagas de reboque**:
  ponteiros para nós do mapa (pos s32×3 em 1/256 m, quaternion w,x,y,z em +0x10, UID em +0x30).
  Vaga [0] = [34128.9; 7.2; -113710.4] = exatamente a posição do erro 14 no game.log. O serviço não
  guarda qual vaga usará; o reboque só nasce com o jogador perto.
- Teleporte = `0x5ddf20(actor=[[exe+0x36ae6d8]+0x31b0], placement*, 0, 0, 0)` (o final do
  company_portal). Placement 32 bytes: f32 x,y,z locais + i16 setor x,z (mundo = local + setor·512)
  + quaternion (w,x,y,z; identidade em rva 0x251d990 = (1,0,0,0)).
- Plugin: alvo = 20 m à frente de uma vaga (frente = quaternion aplicado a −Z), mesma direção, +0,5 m
  de altura; escolhe a vaga cujo alvo fica mais longe de todas as vagas. Conferido offline com as
  vagas de Alta: alvos caem dentro da bbox da empresa. 10 quadros após iniciar + botão "Ir até a carga".
- Erro 14 (`mp_job_trailer_not_created`, "Trailer position is occupied"): até 5 novas tentativas
  (na Norrsken a 2ª tentativa funcionou).
- NÃO testado no jogo.

# ===== v2.0 (2026-10-02) =====
- Usuário aprovou a v1.3 ("perfeito, funcionou!"): teleporte para o pátio + retry do erro 14 +
  correção do erro 18 = **v2.0.0** (tag git `v2.0`). Daqui pra frente tudo é 2.x. Backup em
  `D:\Projetos\_backupsts2-routes2.0\` + `v2.0.zip` (LEIA-ME.txt lá). A v1.0 continua em `v1.0`.

## v2.1.0 (2026-10-02) — pagamento: distância real em vez de 0 km
- Usuário: "está pagando muito pouco". Causa: o params+0x64 que eu zerava desde a v0.3 É a distância.
  GEN 0x830ae1: `cvttss2si eax, [params+0x64]` → `mov word [oferta+0x50], ax` = **shortest_distance_km**.
  Tabela de serialização do save (descritores {nome*, classe*, offset, tipo} em .data; atenção: o
  offset certo de cada nome está na entrada ANTERIOR ao listá-las em sequência): job_offer_data
  +0x50 shortest_distance_km (u16), +0x52 ferry_time, +0x54 ferry_price, +0xcc units_count,
  +0xd0 fill_ratio; job_info: planned_distance +0x40, planned_distance_km +0x42, ferry +0x44/+0x46.
- def/economy_data.sii: fixed_revenue 600 €, revenue_per_km_base 15, revenue_coef_per_km 0.9
  (freight market) → com 0 km o serviço pagava só ~600 €.
- Plugin: `game::FreightKm` acha as duas empresas com 0x7d0df0(&empresa, &cidade) (a busca do
  gerador), centro da bbox do item de mapa ([empresa+0x10] +0x0c/+0x20), linha reta × escala do mapa
  19 × fator de estrada 1,2, em km. Log "distância estimada: N km". NÃO testado no jogo; calibrar
  comparando com serviços do mercado de fretes entre as mesmas cidades.

# ===== v2.1 (2026-10-02) =====
- Usuário aprovou o pagamento ("funcionou, o pagamento ficou bom"). Peso estimado da carga de volta
  na lista (gerador da v0.6: maior carga num reboque simples compatível; `CargoMass`; linha "~N t").
  **v2.1.0** = v2.0 + distância estimada no params+0x64 + peso. Tag `v2.1`, backup em
  `D:\Projetos\_backupsts2-routes2.1\` + `v2.1.zip`.

## v2.2.0 (2026-10-02) — soltar o freio de mão após o teleporte
- Teleporte 0x5ddf20, em 0x5de9f8: `actor+0x1c4 |= (cvar g_park_brake_init != 0)` (objeto da cvar em
  rva 0x2d387e0, lido por 0x1cbd70); se engatou, grava 1.0 em actor+0x3cc e +0x3d0. **actor+0x1c4 =
  freio de mão** (byte). Leitura ao vivo com o caminhão parado: +0x1c4 = 1, +0x3cc/+0x3d0 = 1.0.
- Plugin: caixa "Soltar o freio de mão após teleportar" (padrão ligada); zera os três campos logo após
  o teleporte e de novo 1 s depois; log com `SPF_TruckData.parking_brake` antes de cada escrita.
  NÃO testado no jogo.

# ===== v2.2 (2026-10-02) =====
- Usuário confirmou ("funcionou"): freio de mão solto após o teleporte. **v2.2.0**, tag `v2.2`, backup em
  `D:\Projetos\_backupsts2-routes2.2\` + `v2.2.zip`.

## v2.3.0 (2026-10-02) — 7h e tempo limpo antes de criar o serviço
- Comandos de console reais no exe: `g_set_time <horas> [minutos] ['0'|'1' tráfego]` (uso na string
  "Usage: %s <hours> [minutes] ...", junto com "Economy not present.") e `g_set_weather 0/1` (está
  nos atalhos de dev padrão do jogo). (`cheat ...` NÃO existe no console de varejo.)
- Iniciar agora: se a caixa "Antes de iniciar: 7h da manhã e tempo limpo" (padrão ligada) estiver
  marcada, roda `g_set_time 7 0` e `g_set_weather 0` via SPF_GameConsole_API (manifesto exige o hook
  "GameConsole"), espera 5 quadros e só então cria o serviço (o prazo conta da hora nova) e teleporta.
  Botão Iniciar travado durante a espera. NÃO testado no jogo.

# ===== v2.3 (2026-10-02) =====
- Usuário confirmou ("funcionou"): 7h + tempo limpo antes de criar o serviço. **v2.3.0**, tag `v2.3`,
  backup em `D:\Projetos\_backupsts2-routes2.3\` + `v2.3.zip`.

## Escolta policial movida para a branch `escolta` (2026-10-04)
- Pedido do usuário: deixar a escolta em outra branch e voltar para a v2.3. `master` = tag `v2.3`
  (DLL e routes.tsv do backup v2.3 reinstalados, hashes conferidos).
- A branch `escolta` (commit c70e2c0, v2.5.1) guarda todo o trabalho e o diário dele (ler o MODLOG.md
  de lá): comando `spawn vehicle` e a função 0x566960, modelos de polícia por país, bit 63
  debug_pause, exclusão de veículo de IA (0xace9e0 + bit 24), painel, layout do corpo físico
  (posição/orientação/velocidade) e a condução pelo rastro do caminhão (não testada no jogo).
  Pendências lá: confirmar a condução pelo rastro, giroflex, país atual, e a causa do fechamento do
  jogo ao recarregar o framework com um carro vivo (suspeita: chamada ao jogo dentro do OnUnload).
- Restos no `spfPlugins\RoutePlanner\config\settings.json`: tecla `escort` e janela `Escolta` (o SPF
  regrava o arquivo com o jogo aberto; sem efeito na v2.3).

## v2.4.0 (2026-10-04) — favoritas e maior rota (sobre a v2.3; a escolta segue na branch `escolta`)
- Barra no topo da janela: **Planejar**, **Favoritas (n)**, **Maior rota** (sem tecla nova).
- Favoritas: `favorites.tsv` ao lado da DLL (cidade origem, cidade destino, carga, empresa origem,
  empresa destino; `LoadFavorites`/`SaveFavorites` em routes.h). Tela com Iniciar / Editar / Remover.
  "Iniciar" carrega a rota no planejador (`ApplyRoute`) e usa o mesmo `Pending::Start` do botão
  normal (7h + tempo limpo, criação, teleporte, freio). "Editar" abre o planejador com a rota
  carregada e o botão vira "Salvar alterações na favorita". No planejador: "Adicionar esta rota às
  favoritas" (rota = origem, destino e a carga selecionada; não duplica).
- `FilterUnknownCargo` agora preserva a carga selecionada ao remover cargas que o jogo não conhece.
- Maior rota: `game::CompanyCenter` (centro do item de mapa da 1ª empresa conhecida de cada cidade,
  via 0x7d0df0) para as 384 cidades, `FarthestPair` (O(n²)), preenche países e cidades, liga
  "Qualquer carga" e mostra a distância estimada (mesma conta do pagamento: linha reta × 19 × 1,2).
  Roda no OnUpdate porque consulta o jogo.
- Testes novos em routes_test: ida e volta das favoritas, `FindOption`, `FarthestPair`.
- NÃO testado no jogo.

# ===== v2.4 (2026-10-04) =====
- Usuário confirmou favoritas e maior rota ("funcionou"). Pedido extra antes de salvar: na aba Favoritas,
  "Salvar o serviço atual" (ids da telemetria: source/destination city, cargo, companies) e "Salvar a rota
  escolhida em Planejar" (`AddFavorite`). Esses dois botões NÃO foram testados no jogo antes da tag.
- **v2.4.0**, tag `v2.4`, backup em `D:\Projetos\_backupsts2-routes2.4\` + `v2.4.zip`.
- v2.4 refeita (2026-10-04) a pedido: em "Planejar", o botão "Salvar esta rota como favorita" fica sob a lista de
  cargas (só aparece com origem e destino escolhidos) e só habilita com a carga selecionada; mostra o que falta.
  Tag `v2.4` movida para este commit e backup `v2.4` regravado. Não testado no jogo antes da tag.

## Medição pelo GPS movida para a branch `gps-distancia` (2026-10-04)
- Pedido do usuário: esquecer a v2.5 (distância medida pelo GPS do jogo) e seguir com a v2.4. `master` = tag
  `v2.4` (DLL e routes.tsv do backup v2.4 reinstalados, hashes conferidos; favorites.tsv preservado).
- A branch `gps-distancia` (commit ecbb08d) guarda o código e as notas: `SetGpsToCompany` (0x7b47b0 + 0x4fad00,
  estado [game+0x42f0]), teleporte para o pátio por tokens da empresa e o fluxo teleporte → GPS → leitura da
  telemetria → criação do serviço. Nunca foi testado no jogo.

## v2.5.0 (2026-10-04) — botão "Cidade atual" (master, sobre a v2.4)
- Em "Planejar", logo abaixo de "Origem": botão **Cidade atual** → país e cidade de origem = a cidade mais
  próxima do caminhão. `CityPoints()` (extraído do "Maior rota": centro do item de mapa da 1ª empresa
  conhecida de cada cidade, via 0x7d0df0) + `NearestPoint` (routes.h, testado) com a posição da telemetria.
  Roda no OnUpdate (`Pending::CurrentCity`) porque consulta o jogo. Mostra a cidade e a distância no mapa.
- Limite conhecido: "cidade" = onde ficam as empresas dela; numa estrada longe de tudo, vale a mais próxima.
- NÃO testado no jogo.

# ===== v2.5 (2026-10-04) =====
- Usuário confirmou ("funcionou"): botão "Cidade atual". **v2.5.0**, tag `v2.5`, backup em
  `D:\Projetos\_backupsts2-routes2.5\` + `v2.5.zip`.

## Estudo (sem implementar): escolta do DLC Transporte Especial (2026-10-04)
Pergunta do usuário: como funciona a escolta do DLC, para fazer igual com qualquer carga/destino.
- **Dados** (`dlc_oversize/def`): `oversize_data.sii` (escort_max_speed 80 km/h; escort_dangerous_objects:
  rail_train, slow, rail_tram); `route*.sii` = 65 `route_data` (59 no arquivo base; só from_city/to_city); `oversize_offer_data*.sii`
  = 272 ofertas (rota + carga + limites de tempo + dimensões + cutscenes). Veículos: `traffic.transport_t6.escort_N.back`
  / `.front` (tipos `escort_back`/`escort_front`, tag `escort_all`, back com `flares_beacon_min0max28`).
- **O caminho da escolta NÃO é calculado: é desenhado no mapa.** O jogo procura *itens de trajetória* com tags
  da rota: "Front/Back escort trajectory", "Escort meet/leave trajectory", "Offer point", "Trailer start/end"
  (mensagens `[oversize_offer] ... trajectory for route '%s' not found. (tag '%s')`). Só as rotas do DLC têm.
- **Regras nos nós da trajetória** (`def/world/trajectory_rules.sii`, 35 regras): ligar/desligar "escort special"
  (luzes), bloquear nós de IA em cruzamentos (`bl`, `back_bl`, `circ_emp_bl`) e cancelar, parar o veículo,
  liberar a parada quando o jogador chega (`canc_fr_st`, `canc_bc_st`), seções normal/perigosa/troca de faixa,
  piscas, "escort near", mensagens (devagar, trecho apertado, faixa errada, abrir caminho).
- **Criação do veículo** (ferramenta de debug do tráfego, 0x553e80–0x554450): tipos via 0x54e170 (frente) /
  0x54e1e0 (trás) no gerente [exe+0x36ae7e8]; trajetória achada por 0x551e90 perto de um item; veículo criado
  por 0x564b20(traffic, 0, params{tipo}, caminho, ...); 0x924520(veh, 8) + 0x939500 instalam um **controlador
  de escolta** em veh+0x4f0 (objeto 0x158 bytes, vtable rva 0x2320518; update = 0x940580, todo em chamadas
  virtuais — a lógica de distância ao jogador NÃO foi lida); 0x920cc0(veh, trajetória, vel, índice, ...) prende
  o veículo à trajetória; 0x76e190/0x76e1f0 registram como escolta dianteira/traseira.
- **Estado salvo** (oversize_job_save, offsets): posição no mundo (+0x18/+0x24), uid da trajetória (+0x30/+0x38),
  posição AO LONGO dela (+0x40/+0x44), rotação (+0x48/+0x58), velocidade (+0x68/+0x6c), tipo/estado/semente,
  spawn_escort_active (+0x88), trajectory_orders (+0x90), estado do gerente (+0xb8), kdop atual (+0xbc),
  última posição válida do jogador (+0xc0), bloqueios ativos (+0xd0), hash da rota (+0xf8).
- **Conclusão:** para "qualquer carga, qualquer destino" o sistema do DLC não serve direto (sem trajetórias fora
  das 65 rotas; criar trajetórias em tempo de execução exigiria RE do traffic_trajectory_t). O caminho viável é
  o da branch `escolta` (carro conduzido pelo rastro do caminhão), usando os MODELOS de escolta do DLC e
  procurando o que a regra `on_special` liga no veículo (pista para o giroflex).

## v2.5.1 (master, 2026-10-04) — recarga automática (hospedeira + núcleo), sem escolta
- Trazida da branch `escolta` só a recarga automática; o resto é a v2.5.
  - `RoutePlanner.dll` = hospedeira (`Host.cpp`): manifesto, tecla F8, janela, fonte; observa `core\RoutePlannerCore.dll`
    a cada 30 quadros e recarrega a cópia em `core\live\` quando o arquivo muda.
  - `core\RoutePlannerCore.dll` = núcleo (`Core.cpp`, antes `RoutePlanner.cpp`): toda a UI e a lógica; contrato em `core_api.h`.
  - `deploy.ps1`: troca o núcleo de forma atômica (aparece "núcleo #N" no framework.log em ~1 s); a hospedeira só é
    substituída se mudou, e aí precisa de "Recarregar Framework" (a versão do CMake mora nela: não subir a versão a cada ajuste).
- O bloqueio do mouse passa pela hospedeira (o SPF identifica o pedido pelo endereço de retorno).
- Compila e os testes passam; recarga em si já testada em jogo na branch `escolta`. Esta montagem ainda não foi testada em jogo.
- **Abastecer ao iniciar o serviço** (opção "Abastecer o caminhão ao iniciar", ligada por padrão): `game::Refuel()`.
  Canal de telemetria truck.fuel.amount (getter 0x64b630) = [veh+0x190] (capacidade, L) × ([veh+0x1b8] (nível 0..1) +
  [truck+0x1158] (variação pendente)); truck = [actor+0x18] (vtable rva 0x22f0260), veh = [truck+0x1f8]. Encher = nível 1,
  pendente 0. Conferido ao vivo só por leitura (800 L, nível 0,073). Escrita ainda não confirmada em jogo.

## Estudo (2026-10-04, sem código): mostrar mensagens na caixa de aviso do jogo ("Freio de mão acionado!")
- Textos: chaves `aca_parking_brake_on1` (título) e `aca_parking_brake_on2` (texto) do localization.sui; código em 0xa04660..0xa049c9.
- **Função que mostra: `0xa63920(hud, char** titulo, char** texto)`** (prólogo `40 53 48 81 ec 40 04 00 00 48`).
  - `hud = [[[exe+0x36ae6d8]+0x2b30]+0xb0]`; o painel é `adv = [[hud+0x50]+0x340]`.
  - Monta "titulo|texto" (`%s|%s`, até 0x400 bytes), copia para a string em `adv+0xf18` (0xf4920) e põe `adv+0xf38 = 1`.
  - Título ou texto vazio → grava string vazia = esconde a caixa (é assim que o jogo a fecha ao soltar o freio).
  - O jogo só chama com `[[owner+0x2b30]+0x210]` entre 3 e 5 (dirigindo) e `adv+0xf39 == 0`.
- O texto aceita a marcação do jogo: `<br>`, `<color value=@@clr_sel@@>…`, chaves `@@…@@`; a tecla vem de `$KEY$` substituído antes (0xf3f20).
- Não verificado: de onde vem o ícone (P), acentos (o jogo usa UTF-8), se a caixa some sozinha, e a disputa com os avisos
  do próprio jogo (cada aviso dele sobrescreve a mesma string). Chamar só na thread do jogo (Update do plugin), com SEH e conferência do prólogo.
- **Teste (após a v2.5.1, sem tag):** `game::ShowHint/HideHint`; ao iniciar um serviço, 2 s depois vai "Serviço iniciado | carga<br>origem → destino<br>Tanque cheio" para a caixa e é retirada após ~10 s (só se ainda for a nossa: compara com `adv+0xf20`). Reabastecimento da v2.5.1 confirmado em jogo.
- **Correção do estudo da caixa de aviso (2026-10-04):** 0xa63920 é da Academia de Direção (chaves `aca_*`; exige estado 3..5 em
  `[[owner+0x2b30]+0x210]`, que fora da academia é 0 → log "motivo 100"). O aviso normal vem de uma tabela de formatos
  (rva 0x2d6ba30: índice 6 = parking_brake, 11 = car_delivery_ready…) e vai para a **fila de mensagens do conselheiro**:
  `fila = [actor+0x30] + 0xd8` (entradas de 0x70 bytes: id, ativo +4, chave +8, prioridade +0xc, texto +0x18, tipo +0x40, expiração +0x60/+0x68).
  - `0x623a80(fila, char** texto, sub (exe+0x2732198 → ""), tipo, ícone*, prioridade, a, b, u16 c, objeto, {u8 tem_chave; i32 chave}*)`;
    tipo indexa a tabela 0x2d69370 (0xb = ícone próprio em `ícone*`). Freio de mão (0x68a5eb): tipo 6, prio 2, a 1, b 0, c 7, chave {1,0}.
    "Carro pronto" (0x68d4d0): tipo 2, prio 2, a 0, b 2, c 0xffff, sem chave.
  - `0x623970(fila, chave)` retira a mensagem com aquela chave (marca a expiração).
  - Plugin (teste): `game::ShowHint(texto)` com os valores do "carro pronto" e chave própria 0x52504c; `HideHint()` após ~10 s. Não testado em jogo.

## v2.6.0 (master, 2026-10-04) — mensagem na caixa de aviso do jogo ao iniciar o serviço
- Confirmado em jogo: a mensagem aparece na caixa do conselheiro (título laranja, 3 linhas, acentos ok, ícone de sino do tipo 2)
  e é retirada após ~10 s. A seta "→" saía como "?" (a fonte do jogo não tem o glifo) → trocada por "->".
- Sobre a 2.5.1: `game::ShowHint/HideHint` (fila do conselheiro, 0x623a80 / 0x623970) e a mensagem "Serviço iniciado".

## Interface nova (após a v2.6, 2026-10-05, ainda sem versão)
- Pedido: UI refinada como a do jogo (1.61) e organizada. Referência: painéis da UI nova do jogo (1.50+/1.58: Quick Info, mercado de cargas):
  fundo grafite, cartões um pouco mais claros com legenda cinza pequena, âmbar em títulos/aba ativa/ação principal, cinza no resto.
- Núcleo (`Core.cpp`, seção Drawing reescrita): `Theme` (cores/métricas empilhadas só dentro da janela), `Card` (bloco arredondado
  pintado com a altura medida no quadro anterior), `Primary`/`Tab`, legendas (`Caption`). Largura fixa de conteúdo 620 px.
  Organização: cabeçalho próprio → abas PLANEJAR / FAVORITAS → cartão SERVIÇO ATUAL → atalhos (cidade atual, maior rota) →
  ORIGEM e DESTINO lado a lado → CARGA em tabela (carga, peso, empresa de origem, empresa de destino) → AO INICIAR O SERVIÇO (2×2) →
  [Salvar como favorita] [INICIAR SERVIÇO]. Favoritas: cartão de salvar + lista rolável com Iniciar/Editar/Remover por linha.
- Hospedeira: janela sem barra de título, tamanho automático, sem barra de rolagem; fontes novas `rp_small` (14) e `rp_title` (24).
  → precisa "Recarregar Framework". `SameLine(offset)` conta da borda da janela (ou do início do grupo), não do padding.
- Compila; aparência NÃO vista em jogo ainda.

## v2.7.0 (master, 2026-10-05) — interface nova
- Visto em jogo (captura do usuário, ainda com a hospedeira antiga): tema, cartões, abas e tabela ok; sobrava um espaço grande à
  direita (janela antiga de 1030 px, redimensionável) e as caixas de marcação ficavam azuis (o checkbox do SPF não segue FRAME_BG).
- Correções: o núcleo ajusta a janela ao conteúdo a cada quadro (`UI_SetWindowSize(kW + 2·margem, fim do conteúdo, ALWAYS)`),
  valendo também com hospedeira antiga; caixa de marcação própria `Check` (âmbar com tique escuro / cinza), desenhada com o draw list.
- Com a hospedeira nova: sem barra de título, fontes `rp_small` e `rp_title`. Essas duas correções não foram vistas em jogo antes de salvar.

## EXPERIMENTO (após a v2.7, 2026-10-05): janela com a interface do próprio jogo, no Home
- Pedido: abrir o planejador usando a UI do jogo (como a tela de pausa do F1), experimental, no Home; F8 fica como está.
- **Como o jogo faz as telas:** scripts SiiNunit em /ui/*.sii (base_share.scs; extraídos em `C:\Users\Paulo\ets2-ui\ui`, ~290 arquivos).
  Raiz `ui::window` (1440×900 virtuais, origem embaixo à esquerda) com `window_handler` (classe C++ do jogo, ex. `pause_hdl`, `msgbox_hdl`)
  ou `null` (72 scripts); filhos `ui::group`, `ui::text_common` (value + look_template: txt.title.center, txt.normal.center,
  txt.window.bcg_rect4…), `ui::text` (marcação crua), `ui::button_common` (look_template btn.normal, id), `ui::linear_layout`, `ui::button_row`.
  A tela do F1 é `/ui/pause.sii` (83 KB, handler próprio).
- **Funções (0x68cda0, opções do conselheiro):** fs = 0x1536e0(4), fs->vt[4](fs, char** caminho) = existe;
  0x374f20(void** tmp, char** nome, char** camada "hud", 0x100, char** caminho, u8 0x80) cria; 0x33cb90(slot, tmp) assume a posse;
  0x38c080([exe+0x36ae6f8], janela, 0) mostra; 0x38bbc0(gerente, janela) + 0x108650(slot) fecha e solta.
- A pasta Documentos do jogo é montada como `/home` → o script vai em `Documents\Euro Truck Simulator 2\routeplanner\planner.sii`
  (copiado pelo deploy.ps1 de `ui/planner.sii`), sem precisar de mod.
- **Plugin (etapa 1):** `game::OpenGameWindow/CloseGameWindow`; Home (lido direto do Windows, sem mexer na hospedeira) abre/fecha uma
  janela estática: fundo, título, texto e um botão sem ação. NÃO testado em jogo.
- Próximas etapas se a 1 funcionar: achar widgets por id, trocar textos, detectar clique dos botões (sem handler C++), listas.
- **Etapa 1 confirmada em jogo (2026-10-05):** Home abre e fecha a janela nativa (log "aberta"/"fechada"); o cursor NÃO aparece.
  A tecla precisa vir do atalho do SPF (ele engole a Home antes do GetAsyncKeyState).
- RE: janela +0xd8 = flags passadas na criação (0x100 opções, 0x400 = janelas passivas do HUD) | byte de ordem (0x7d/0x80);
  +0xa0 = nome; lista de janelas do gerente em mgr+0xd8 (nó+0x10 = janela); mgr+0x308 = janela `ui_gamepad_focus`, mgr+0x388 = contador
  de janelas sem 0x400; pilha de ponteiros em mgr+0x238. `0x37dab0(janela, char** "tipo_hdl", 0)` acha o handler pelo tipo.
  **Widgets:** filhos em +0x70 (contagem +0x78), id em +0x14, contêiner = bit 7 de +0x60; busca por id = 0x385e20(contêiner, id)
  (reimplementada em `game::FindWidget`). Handlers recebem o widget clicado num virtual (ex. 0x1133440 compara [widget+0x14]).
- Ideia para conteúdo dinâmico sem RE do "set text": reescrever o .sii e recriar a janela.
- Diagnóstico instalado: com a janela aberta, o log lista os bytes do botão (id 200) que mudam (para achar "sobre"/"pressionado").
- Em aberto: como ligar o ponteiro do mouse (no jogo, o botão direito alterna o ponteiro do conselheiro de rota).
- **Cursor nativo (2026-10-05):** dirigindo, o mouse é da câmera; o cursor do jogo só existe com o jogo pausado para uma tela.
  Par usado pelo jogo nas telas de mensagem: `0x68ca20(conselheiro = [actor+0x30])` esconde os painéis do conselheiro, pausa a simulação
  (contadores em [exe+0x36ae718]+0xac0/0xac4/0xac8/0xacc; byte [exe+0x36ae740]+0x28 = 0) e entrega o mouse à UI; `0x68bb60()` desfaz.
  A função 0x688e30 (ao mudar a pausa) grava [gerente UI+0x348] = !pausado. Plugin: `game::PauseForUi(bool)`; Home pausa + abre, Home fecha + retoma.
  Botão de teste: +0x1d7 alterna 80/89 e os floats em +0x208/+0x218 variam a cada quadro (animação de foco), sem mouse. NÃO testado em jogo.
- **Etapa 2 (2026-10-05):** com 0x68ca20 o cursor nativo apareceu e o botão reagiu, mas o mundo ficou PRETO (essa função também desliga
  a câmera e muda o modo do HUD; é para telas cheias). `game::PauseForUi` passou a repetir a sequência à mão SEM a parte da câmera
  ([exe+0x36ae740]+0x28 → 0x5015a0 → vt[20]/vt[18]) e sem o `hud->vt[13]`: entrada de UI (uimgr+0x3b0: vt[33](&0,&2) / vt[21](&{0,-1}),
  0x38a770), contadores G+0xac0..0xacc, 0x428b20/0x428240, temporizador 0x10aeb0/0x58c1e0, 0x441f60(modo 2/1).
  Botão sob o ponteiro: flags +0x60 ganharam os bits 24 e 25 (5C→5F no byte +0x63), +0xc0 0→1; +0x3c/+0x40 = posição do ponteiro no widget.
- Conteúdo dinâmico: `WriteNativeScript` gera o .sii a cada abertura (favoritas: texto + botão "Iniciar" por linha, até 8; "Fechar").
  Clique = botão esquerdo solto (GetAsyncKeyState) com um botão nosso com o bit 24 ligado → fecha, retoma e inicia a favorita pelo mesmo
  caminho do F8 (ou avisa pela caixa do conselheiro se já há serviço). Mudanças de flags vão para o log. NÃO testado em jogo.
- **Etapa 2, 2ª rodada:** (a) o jogo guarda o script já carregado: reabrir o mesmo caminho mostrou o conteúdo ANTIGO (janela de teste)
  mesmo com o arquivo novo em disco → cada abertura grava `planner_N.sii` com nomes de unidade `_nameless.rplN.*` e apaga o anterior.
  (b) o fundo continuou preto sem a parte da câmera → o culpado é o contador `G+0xac4`: só as telas cheias do jogo o sobem
  (0x68ca62, 0x839a70, 0x88b610, 0xa059cb, 0xa63a8c…); a pausa comum (0xa05aa0) sobe só 0xacc, 0xac8, 0xac0. `PauseForUi` não mexe mais nele.
  (c) flags do botão: 5CFF0101 → 5FFF0101 ao entrar o ponteiro e de volta a 5CFF0101 ao sair → o bit 24 serve para o clique.
- **Etapa 3 (2026-10-05): planejador inteiro em tela nativa.** Confirmado em jogo antes: pausa comum (sem `G+0xac4`) mantém o mundo visível,
  cursor nativo, favoritas com "Iniciar" funcionando; o "·" não existe na fonte do jogo (some).
  Agora (`Core.cpp`, bloco "EXPERIMENT (Home)"): `NativeUi` monta o script no formato do F1 (painel 40..1400 × 40..860 com
  `txt.window.bcg_rect4`, linha de título `txt.big.left`/`txt.big.center`, abas, cartões = `txt.background.flat` 18FFFFFF + título
  `txt.big.bold.white.center`, botões `btn.normal`; destaque = botão com pml próprio: imagens de `btn_tab.mat` tingidas com `@@clr_sel@@`).
  Páginas: Planejar (serviço atual com teleporte/cancelar; rota com país/cidade de origem e destino, cidade atual, maior rota; carga com
  "Qualquer carga"; opções ao iniciar como botões liga/desliga; salvar favorita; INICIAR), Favoritas (salvar, iniciar, editar, remover,
  paginada), e seletores em grade paginada de botões: País (5×12), Cidade (5×12), Carga (2×12). Cada clique roda a ação e regrava/reabre
  o script (jogo continua pausado); ações que precisam do jogo rodando (iniciar, cancelar, teleporte, Retomar) fecham e retomam.
  Sem caixa de texto nem lista com rolagem (precisariam de RE dos widgets ou de um handler). NÃO testado em jogo.
- **Etapa 4 (2026-10-05): tela nativa redesenhada** (pedido: mais bonita, selecionado dourado em vez de menor, listas menos custosas,
  bandeiras, Esc fecha). Planejado antes de codar:
  - Botões agora são `ui::button` puros com faces próprias (n/s/p_pml = `<img white.mat color=… stretch>` + conteúdo) → mesmo tamanho,
    dourado quando selecionado (FF0D7FB2). O pml da instância NÃO vale em `ui::button_common` (por isso o destaque anterior não aparecia).
    Cores explícitas do jogo são **AABBGGRR** (FF4050FF = vermelho de aviso, FF30BCFE = âmbar).
  - Marcação usada: camadas separadas por `<ret>`, `<offset hshift= vshift=>`, `<align hstyle= vstyle=>`, `<font face=/font/{normal,small,big_bold}.font>`,
    `<color value=@@clr_white@@|@@clr_txt_d@@|@@clr_sel@@>`.
  - Imagens do jogo: bandeiras `/material/ui/flags/<iso3>.mat` (textura 64×64, recorte `left=p2 right=p62 top=p12 bottom=p52`; iso3 =
    `iso_country_code` do def do país), ícone de carga `/material/ui/cargo_logo/<tipo de carroceria>.mat` (tinta `@@clr_cargo_logo@@`),
    logotipo `/material/ui/company/small/<empresa>.mat` (116×29; 1801 de 1831 filiais têm). Materiais extraídos em `C:\Users\Paulo\ets2-ui\mat`.
  - `routes.tsv`: N ganhou iso3, P ganhou "tem logo" (3º argumento opcional do gen_routes.py), G ganhou o ícone.
  - Páginas: Planejar (ROTA com botões grandes bandeira+cidade+país; CARGA com ícone, empresas com logo, distância e pagamento estimados,
    "Outras empresas"; SERVIÇO ATUAL; faixa de opções; INICIAR), Local (países com bandeira à esquerda, cidades do país à direita com nº de
    empresas, numa página só), Carga (índice de iniciais + cada carga uma vez, com ícone e peso), Favoritas (linhas com bandeiras e ícone).
  - Esc (GetAsyncKeyState) fecha e retoma. NÃO testado em jogo.
- **Etapa 4 confirmada em jogo ("ficou muito maneiro")**: faces próprias, bandeiras, ícones, logos e páginas funcionam. Ajustes pedidos:
  dourado só para o que está selecionado (INICIAR, "Escolher a carga", "Iniciar" das favoritas e "Sim, cancelar" voltaram ao cinza);
  opções viraram a caixa de marcação do jogo (`/material/ui/button/checkbox_1..4.mat`: desligada, desligada+ponteiro, ligada, ligada+ponteiro)
  com o rótulo ao lado; roda do mouse vira a página das listas (`UI_GetMouseWheel` do SPF, lido no Update: não confirmado que chega com a UI
  do SPF fechada); `SiiString` troca →, …, · e € (não existem nas fontes do jogo → "?") e remove < > (são marcação). NÃO testado em jogo.
- Roda do mouse: `UI_GetMouseWheel` do SPF NÃO chega com a UI dele fechada (teste do usuário). Trocado por um gancho `WH_MOUSE_LL` numa thread própria com fila de mensagens, ligado só enquanto a tela nativa está aberta (`WheelStart/WheelStop`; o Shutdown sempre encerra a thread). NÃO testado em jogo.

## v3.0.0 (master, 2026-10-05) — planejador na interface do próprio jogo (Home)
- Aprovada pelo usuário ("funcionou, salve essa como v3.0"). Sobre a 2.7: a tela nativa do Home (ver "EXPERIMENTO" e Etapas 1–4 acima):
  pausa o jogo, cursor nativo, páginas Planejar / Favoritas / Local / Carga feitas com widgets, bandeiras, ícones de carga e logotipos do
  jogo; checkbox do jogo nas opções; roda do mouse vira páginas; Esc ou Home fecham. O F8 (ImGui) continua igual.
- Confirmado em jogo pelo usuário: abrir/fechar, cliques, iniciar serviço, visual, "Qualquer carga" só na página de cargas.
  Não confirmado explicitamente: roda do mouse com o gancho novo.

## v3.0.1 (master, 2026-10-05) — aprovada pelo usuário
- Tela nativa: sem o texto "Esc fecha" (o Esc continua fechando), sem o rótulo "trocar" nos botões de origem/destino, "Outras empresas" virou "Mudar empresas (N)".
- "Limpar planejamento" à direita da linha de ações (espelho de "Salvar como favorita"): aparece quando há país/cidade/carga escolhidos ou uma favorita em edição; zera origem, destino, carga, "Qualquer carga", índice e a mensagem de status.

## v3.1.0 (master, 2026-10-05) — aprovada pelo usuário (trabalhada como 3.0.2)
- **Piscada a cada clique:** a tela era fechada e só então a nova era criada. Agora a nova sobe primeiro e a antiga é fechada 2 quadros
  depois (`g_native_old`). Cada janela tem nome próprio (`rpl<N>`): `0x38c080` derruba qualquer outra janela com o MESMO nome ao mostrar.
- **Filtro digitável na página de cargas:** caixa desenhada com widgets do jogo (fundo escuro + texto + "_"), teclas vindas de um gancho
  `WH_KEYBOARD_LL` na mesma thread do gancho da roda (letras, dígitos, espaço, backspace; engolidas do jogo só nessa página e com o jogo em
  primeiro plano). Comparação sem acento e sem maiúsculas (`Fold`). Um input nativo de verdade (`ui::inputline`) precisaria de handler.
- **Ordenar por:** Nome / Peso (dourado o ativo); clicar no ativo inverte (A–Z ↔ Z–A, mais pesada ↔ mais leve primeiro).
- NÃO testado em jogo.
- A janela antiga passou a ser fechada 3 quadros depois (pedido do usuário após testar com 2). Salva como v3.1.
- Depois da v3.1 (sem tag): a janela antiga passou a ser fechada 5 e depois 4 quadros depois (pedidos do usuário); fica em 4.

## EXPERIMENTO: pré-visualização da rota no mapa do jogo (2026-10-06, após a v3.1, sem tag)
- Pedido: ver a rota escolhida no mapa do jogo, como o GPS a traçaria, numa janela navegável. Em etapas, como a tela nativa.
- **O que o jogo tem (scripts em /ui):** widgets de mapa prontos, usados em janelas SEM handler embutidas por `child_path`:
  `ui_world_map` (/ui/world_map_map.sii; `show_country_names`, `zoom_allowed: true`, `map_config_path`), `ui_job_map`
  (/ui/map_view_detail.sii e mais 11 telas; é o mapa ao lado de uma oferta de carga, que mostra a rota do serviço) e `ui_map`.
  Todos com `id: 100000`.
- **RE do handler de oferta (`job_detail_hdl`, funções 0x105e880..0x1063a60):** acha o mapa com
  `0x385bf0(janela, 100000)` → `[widget+0x98]` (janela filha) → `0x385bf0(filha, 100000)`. No mapa: `+0x1e8` = nível de zoom,
  `+0x888` = estado, `+0x88c` = máscara do que é desenhado, `+0x894` = byte de modo; `0x1015140(mapa, widget|0)`,
  `0x1012d10(mapa, 1)`, `0x1000a60(mapa)`; `0x1063a60(handler)` (996 bytes) prepara o mapa para a oferta. AINDA NÃO achado: a chamada
  que entrega origem/destino (a rota) ao mapa.
- **Etapa 1 (plugin):** página "Mapa" na tela nativa (botão "Ver no mapa (experimento)" no cartão ROTA) com um dos três widgets, trocável
  por botões, para descobrir se desenham e aceitam arrastar/zoom sem handler. NÃO testado em jogo.
- **Etapa 1, crash (2026-10-06):** abrir a página com `ui_world_map` derrubou o jogo: "array_t<vec_t<float,2>>: Index outside array
  boundaries: 8 >= 8", pilha 0x100e405 ← 0x100d706 ← 0x116d3ce ← 0x101100c ← desenho de janelas (0x37a313). A função 0x100d7d0 (nomes de
  países, só com o bit 22 da máscara) indexa a tabela de níveis `[[exe+0x36ae6d8]+0x98]+0x38` com `[mapa+0x1e8]` (nível de zoom), que
  nasce 8 no construtor (0xffc705), um além do último. As telas do jogo chamam antes `0x10005c0(mapa, nível)` (virtual; grava +0x1e8, pega
  a escala em +0x1e0 da tabela de floats `…+0x10` e chama 0x1000750). Plugin: `game::SetMapZoom` logo depois de abrir a janela (mesmo
  quadro, antes do desenho), nível inicial 4, e botões "Zoom −/+" que mudam o mapa aberto sem recriar a janela. NÃO testado em jogo.
- **Etapa 1 confirmada em jogo (2026-10-06):** com o zoom definido, `ui_world_map` e `ui_job_map` aparecem dentro da nossa janela e a
  RODA DO MOUSE dá zoom neles nativamente; só desenham depois que o zoom muda uma vez. `ui_map` (o do conselheiro, /ui/gps.sii) tem outros
  atributos ("has no attribute named 'show_country_names'") → o script não carrega; o jogo só registra o erro, não cai.
- **Etapa 2 (plugin):** os mapas desenham a rota em que o GPS do jogo está → ao abrir a página do mapa o plugin manda a rota do planejador
  para o GPS (`game::SetGpsRoute`: alvos de 24 bytes de 0x7b47b0 para a empresa de origem e a de destino, trocados com 0x4fad00; código vindo
  da branch `gps-distancia`, nunca testado em jogo) e limpa ao sair da página/fechar. Recusado pelo jogo durante um serviço
  ([game+0x42f0] = 1, 6, 7). A rota começa no caminhão. Zoom inicial 1 e um "empurrão" (muda um nível e volta) 2–3 quadros após abrir,
  para o mapa desenhar sozinho. NÃO testado em jogo; não sei se o GPS calcula com o jogo pausado.
- **Etapa 2, teste (2026-10-06):** o GPS aceitou 2 pontos (log "GPS itcc.luxembourg -> bcp.kosice: 2 ponto(s)"), mas a rota não aparece em
  nenhum dos mapas; arrastar com o mouse funciona nativamente. O mapa não abria desenhado porque os níveis de zoom são os `zoom_uplift[]`
  de /def/map_data.sii: 0–1 mapa 3D, 2–3 minimapa, **4–7 mapa mundial (4 cidade … 7 mundo inteiro; maior = mais longe)**; eu abria no 1.
  Agora abre no 7 e os botões ficam entre 4 e 7. O mapa com `show_country_names: true` tentava abrir `/font/map.font` a cada quadro
  (17 mil erros no game.log, que estourou o limite) → `false`. Cores da rota em map_data.sii: `navigation_color` 0xFF0C0CCF.
  Diagnóstico novo no log, 1,5 s após abrir: escala, zoom, estado (+0x888), máscara (+0x88c), modo (+0x894) do mapa e distância/tempo do
  GPS pela telemetria (para saber se o jogo calcula a rota pausado). NÃO testado em jogo.
- **Etapa 2, causa achada (2026-10-06):** o GPS do jogo só CALCULA a rota com o jogo rodando. Log: 0,0 km com a tela aberta (pausado) e
  1053,4 km / 922 min cerca de 1,1 s depois de fechar (Luxemburgo → Odense); o usuário viu a rota no conselheiro e depois a limpeza.
  `SetGpsRoute` funciona (e dá a distância e o tempo REAIS da rota, coisa que o planejador só estimava). O estado do mapa não mostrou
  nada errado (máscara +0x88c = 0, que é o valor normal no handler de ofertas).
- Plugin: ao mandar a rota, despausa (`PauseForUi(false)`), espera a telemetria dar distância > 0 (mínimo 1,5 s, máximo 4 s), pausa de novo
  e refaz a página com "Origem - Destino: N km, HhMM pelo GPS do jogo". Sem cliques durante a espera (sem cursor). Mapa abre no zoom 6.
  NÃO testado em jogo; ainda não se sabe se o widget desenha a rota uma vez calculada.
- **Etapa 2, resultado (2026-10-06, capturas do usuário):** com a espera, o GPS calcula (título "Luxemburgo - Roterdã: 400 km, 5h42" e o
  conselheiro mostrando a rota), mas o widget de mapa solto NÃO desenha a rota do GPS (mostra estradas, cidades e o caminhão; flags de
  desenho = [mapa+0x88c] | [mapa+0x888], esta última 0xFFFFFFFF). Além disso abre com a escala errada até o primeiro giro da roda
  (`0x10005c0` muda o nível, mas a vista só se ajusta depois de um evento de zoom).
- **Etapa 3: usar a tela de mapa do próprio jogo.** As telas do "desktop" do jogo são `screen_desc` em /ui/desc/*.sui
  (`screen.map` → "world_map|/ui/world_map.sii", com `world_map_hdl`). O botão de mapa do conselheiro (0x68c3d9) abre com
  `0x506410(desktop = [exe+0x36ae748], char** nome, char** "", ptr* out, ptr* zero)`; nome = "screen.map" se byte [desktop+0x360], senão
  o nome em exe+0x2dc1018; `out` é solto depois (0x108650). Plugin: `game::OpenMapScreen()`, `game::GameWindowOpen("world_map")`
  (lista do gerente de UI) e o fluxo `StartMapPreview/RunMapPreview`: rota → GPS, fecha a nossa tela, espera o GPS (1,5–4 s), abre o mapa
  do jogo; quando o jogador fecha o mapa, limpa o GPS e reabre o planejador. Em serviço: abre o mapa sem mexer no GPS.
  A página com o widget embutido ficou inalcançável (código ainda lá, a limpar se a etapa 3 vingar). NÃO testado em jogo.
- **Etapa 3, teste (2026-10-06):** `OpenMapScreen` funciona: a tela de mapa do jogo abre com a rota em vermelho (Luxemburgo → Roterdã,
  400 km / 5 h 42) e, ao fechar com Esc, o plugin limpa o GPS e reabre o planejador. MAS o planejador volta com o mundo PRETO (mesmo
  sintoma do contador `G+0xac4` das telas cheias: a tela do desktop ainda está saindo quando pausamos de novo) e o usuário achou pior;
  precisou reiniciar o jogo. Pedido: voltar ao mapa DENTRO da nossa janela. O botão voltou a abrir a página do widget; o fluxo
  `StartMapPreview` ficou no código, sem botão.
- **Diagnóstico para o widget embutido:** a cada segundo o plugin grava os bytes (0xA00) do widget de mapa em arquivo ao lado do plugin:
  `map_real.bin` quando a tela de mapa do jogo (tecla M, janela "world_map" → `ui::portal` id 100000 → janela filha +0x98 → mapa id
  100000) está aberta, `map_ours.bin` quando a nossa página de mapa está. Comparar os dois deve mostrar o que o `world_map_hdl` configura
  (rota do GPS, escala inicial).
- **Comparação dos dois widgets (2026-10-06, `map_real.bin` × `map_ours.bin`, mesma classe/vtable):** o que a tela de mapa do jogo tem e
  o nosso não: flags `+0x888` = 0x00685407 (o nosso 0x00481402: faltam os bits 0, 2, 14 e 21), `+0x898` = 0x05840800 (nosso 0),
  `+0x158`/`+0x168` ponteiros e `+0x170`/`+0x178` = 0x1a4 (o widget de dica, posto por 0x1015140), um array em `+0x408` com 1183 itens
  (os ícones de empresas/pontos, que o nosso mapa não mostra), um array de 1 item em `+0x538` (+0x540/+0x548 = 1; candidato à rota),
  `+0x740` ponteiro, `+0x8d0` = 1, `+0x8e8` ponteiro com `+0x8f0` = 2. Ou seja: quem alimenta o mapa (ícones e rota) é o `world_map_hdl`;
  reproduzir isso no widget solto é um RE grande e incerto. O `+0x888` do nosso nasce 0xFFFFFFFF e vira 0x00481402 depois do 1º zoom.
- **Decisão técnica:** consertar o caminho da tela de mapa do jogo (que já mostra tudo) em vez de alimentar o widget solto. O mundo preto
  vinha de reabrir o planejador (e pausar) com a tela do desktop ainda saindo: novo estágio `Closing` espera os 4 contadores de pausa
  (`game::PauseCounters`) voltarem ao valor de antes do mapa por 250 ms (máx. 5 s) antes de reabrir. Os dois botões ficaram no cartão ROTA:
  "Ver a rota no mapa do jogo" e "Mapa na janela (experimento)". NÃO testado em jogo.
- **Etapa 3, 2º teste:** mesmo esperando os contadores de pausa, o planejador voltou com o mundo preto depois da tela de mapa do jogo
  (o usuário reiniciou o jogo). Caminho abandonado de vez (botão ainda existe); segue-se o widget dentro da nossa janela.
- **Widget embutido, o que faltava (RE, 2026-10-06):**
  - As flags `+0x888` vêm de uma tabela por NÍVEL DE ZOOM em `[[exe+0x36ae6d8]+0x98]+0x178` (lida ao vivo: 685fff, 685fff, 685f7f, 685f7f,
    685faf, 685f27, **685407** (nível 6), **481402** (nível 7)); a diferença vista entre os dois dumps era só o nível. Setter:
    `0x10017d0(mapa, flags)`. `0xffee60(mapa, bits)` reconstrói o conteúdo (bit 0 estradas, bit 1 zera as listas +0x3c0/+0x410…).
    Cor da rota em `[mapa+0x21c]` (0xff0c0ccf).
  - **O mapa precisa receber o objeto de navegação** `nav = game+0x4128` (o mesmo dos pontos do GPS). Montagem dos mapas do jogo
    (0x54a2a9; a da oferta de carga em 0x105f426): `[mapa+0xb0] = nav; 0x100e9b0(mapa, 0.0f); 0x10012e0(mapa, true);
    0x10009d0(mapa, nav); 0x1000a60(mapa)`. `0x10009d0` grava nav em `+0xb8` (nulo no nosso widget, preenchido no do jogo) e zera a rota
    guardada (+0x540, +0x568, +0x6d8). Plugin: `game::AttachMapNavigation`, chamado logo depois do zoom ao abrir a página. NÃO testado em jogo.
- **Rota no mapa embutido: FUNCIONOU (2026-10-06, captura do usuário).** Com `AttachMapNavigation` o `ui_world_map` dentro da nossa janela
  desenha a rota do GPS em vermelho com os marcadores 1 e 2 (Luxemburgo → Roterdã, 400 km / 5 h 42 no título). Home fecha e o GPS é limpo.
  Defeitos vistos: (a) seta do jogador no lugar errado (perto de Frankfurt = origem do mundo: `+0x258` zerado); (b) a vista abre ampliada
  demais, com ícones soltos, até girar a roda; (c) Esc abre o menu do jogo em vez de fechar a nossa tela.
- **Correções:** `0x1000b00(mapa, placement*, bool, foco*, null)` (o HUD chama a cada quadro com o placement do caminhão, 0x689e3e):
  grava o marcador do jogador (`+0x258` posição, `+0x268` rotação), o centro da vista (`+0x1b4`, = foco se dado) e põe a escala do nível
  em vigor (`+0x1ac = +0x1e0`). Placement = o mesmo formato do teleporte. Plugin: `game::SetMapPlacement` com a posição da telemetria e o
  foco no meio entre as duas empresas da rota; zoom inicial escolhido pelo tamanho da rota (5/6/7, limites estimados).
  Esc: capturado pelo gancho de teclado enquanto a tela está aberta (`g_esc`), sem chegar ao jogo. NÃO testado em jogo.
- **Posição do jogador certa (confirmado em jogo).** Novo defeito: o mapa abre com TODOS os ícones da Europa (flags `+0x888` nascem
  0xFFFFFFFF; só o zoom pela roda aplica o conjunto do nível). `game::SetMapZoom` agora também aplica o conjunto do nível
  (`0x10017d0(mapa, tabela[nível])`, tabela em `[[exe+0x36ae6d8]+0x98]+0x178`) e, nos botões de zoom, reconstrói o conteúdo
  (`0xffee60(mapa, 3)`). Assim os ícones seguem a regra do próprio jogo: aparecem conforme o nível de zoom. NÃO testado em jogo.
- **v3.2 (2026-10-06): a pré-visualização da rota deixa de ser experimento.** A pedido do usuário ("está ficando bem legal"):
  - Um só botão no cartão ROTA, "Ver a rota no mapa"; a página mostra só o `ui_world_map` (sem "Mapa mundial"/"Mapa de serviço").
  - Saiu do código o caminho da tela de mapa do próprio jogo (`StartMapPreview`/`RunMapPreview`, `game::OpenMapScreen`, `GameWindowOpen`,
    `PauseCounters`) e os diagnósticos (`MapState`, `CopyMapBytes`, `FindGameWindow`, map_real.bin/map_ours.bin). O que se aprendeu com
    eles está nas entradas acima e no histórico do git (até `92a665c`).
  - Ícones "com um pouco mais de zoom": o mapa usa o conjunto de flags do nível seguinte, mais afastado (`tabela[nível + 1]`), então cada
    grupo de ícones só aparece um nível de zoom depois do que no mapa do jogo. Como a roda do mouse muda o nível sem mexer nas flags
    (quem faz isso no jogo é o handler da tela), `game::KeepMapIcons` confere a cada quadro e reaplica quando o nível muda
    (`0x10017d0` + `0xffee60(mapa, 3)`); o nível lido também acerta os botões Zoom -/+ depois de usar a roda. NÃO testado em jogo.
- **Rumo à v3.3 (2026-10-06): crash ao girar a roda no mapa, rota personalizada.** NADA disto foi testado em jogo ainda.
  - **Crash:** sem registro em game.crash.txt; causa mais provável achada por leitura do executável: o plugin chamava `0xffee60(mapa, 3)`
    (refazer o conteúdo) direto. A atualização do próprio widget (`0x100f240`) só chama isso quando `[mapa+0x81c] != 0` e `[mapa+0xdd0] == 0`
    (nenhuma coleta ainda em andamento), com os bits pendentes de `+0x7c8` (o jogo escreve 3 ali para "tudo"). Agora o plugin só liga os
    bits (`[mapa+0x7c8] |= 3`) e o widget refaz quando pode. `MAP_REBUILD` saiu do código.
  - **O widget de mapa já sabe pontos de passagem** (RE): lista própria `array<route_task_node_t>` em `+0x8c0` (dados `+0x8c8`, quantidade
    `+0x8d0`, máximo 10; lugares a evitar em `+0x9d8`, histórico em `+0xaf0`), aplicada à navegação por `vt[0x1d8]` = `0x1013b00`:
    com modo de navegação 1-4/6-7 (serviço) os pontos vão ANTES do último alvo e o modo fica; senão modo 5 só com os pontos (0 se vazio).
    Quem aciona é a classe da tela (`0x10ab418`, eventos `shortcut_1` adicionar, `shortcut_1h` evitar, `shortcut_2` desfazer, `select*`):
    `0x10112a0(mapa, &índice, &nó, &tipo)` acha o que está sob o cursor (true = já é um ponto da lista), `0x1012c30` + `0x1012eb0` pegam e
    tiram, `0x10130e0(mapa, índice, &nó)` insere na ordem da rota (`0x1016b40` = inserir no array). O cursor o widget acompanha sozinho
    (`0x10127e0`: `+0xd44/+0xd48`). Nó = 24 bytes, primeiro dword 2 = nenhum. `0x4a9510` cria um alvo a partir de uma posição (não usado).
  - **Navegação:** `[game+0x42f0]` É o modo da navegação (`nav+0x1c8`): 0 nada, 2 destino escolhido numa tela, 5 pontos livres, 1/6/7 serviço.
    Alvos em `nav+0x1f8` (dados `+0x200`, quantidade `+0x208`); `0x4faee0(nav, modo, a1d0, alvos, a220, a248)` é o setter completo e
    `0x4fad00` o que zera os outros três; ambos somam 1 em `nav+0x270`.
  - **Plugin:** clique direito na página do mapa = `game::MapClickWaypoint` (adiciona, ou tira o que está sob o cursor); a lista do widget
    vira `g_via`, o GPS recebe caminhão → origem → pontos → destino (`SetGpsRoute` com `via`), a página é refeita olhando para o mesmo
    lugar (`MapViewCenter`, `+0x1b4`) e o widget novo recebe os pontos de volta (`FillMapWaypoints`). Botão "Rota padrão" limpa.
    Mudou cidade, carga ou empresa (`ViaKey`): pontos descartados. Ao iniciar o serviço os pontos vão para `g_job_via` e `RunJobVia`
    os põe no GPS (`ApplyJobWaypoints`: antes do alvo do serviço, modo mantido) quando há serviço, o reboque está engatado e o GPS tem
    um alvo só; uma vez só (o jogo tira cada ponto ao passar por ele). Os pontos guardam ponteiros do mapa carregado: valem só na sessão.
  - **Fonte dos nomes das cidades:** não feito. Não há configuração para isso (`/def/map_data.sii` só tem cores e escalas; o script do
    widget só tem `show_country_names`); tamanho e cor saem do código de desenho (`0x100d3xx`, cor 0xff00aeff, escala por `vt[0x1b0]`).
    Mexer nisso seria alterar código do jogo em memória.
- **Rumo à v3.3, 2ª rodada (2026-10-06), depois do 1º teste do usuário.** NÃO testado em jogo.
  - **O que o log mostrou:** 3 cliques "nada ali" e 1 adicionado, e a lista do widget passou a ter 3 nós: ao receber a navegação o widget
    IMPORTA os alvos do GPS para a lista dele (`+0x8c0` = origem, destino), e o ponto novo entra entre eles. Por isso `FillMapWaypoints`
    falhava ("NÃO devolvidos": lista não vazia) e era desnecessário; saiu. `g_via` agora é a lista inteira do widget (origem, pontos,
    destino) e vai ao GPS como está; `OwnVia()` tira as duas empresas (`game::CompanyNode`) para contar e para o serviço.
  - **Clique:** agora o esquerdo, como no mapa do jogo: soltar o botão sem ter arrastado (≤ 4 px), com o cursor sobre o mapa (bit 24 de
    `+0x60`) e fora de botão. Antes de procurar o que há sob o cursor chama `0x10127e0(mapa, x, y)` com a posição que o widget já tinha
    (`+0xd44/+0xd48`, válida se `+0xd4e`), como a tela do jogo faz: é o que atualiza o item sob o cursor. Clicar na origem/destino é desfeito.
  - **Serviço:** a rota não foi para o GPS e o log não dizia por quê. `RunJobVia` agora aplica quando há serviço, reboque engatado, o
    caminhão a menos de 2 km de onde o serviço começou e o GPS com um alvo só em modo de destino (1-4, 6, 7), quantas vezes for preciso
    (o jogo refaz o GPS ao engatar), e registra o estado a cada ~5 s ("rota personalizada: esperando (… modo M com N alvo(s), reboques …)").
  - **Nomes das cidades:** são markup que o widget escreve (`0x100ca87`: `<font face=/font/%s.font xscale=%g yscale=%g>`), com o nome da
    fonte na string em `+0x230` e a escala em `+0x250` (1.0). Widget novo tem "map" (fonte inexistente → letra de reserva, sem contorno);
    as telas do jogo põem "normal_o" (a contornada: efeito `ui.font.msdf.outline`) com `0xf4a40(string, 0, {texto, 0, tamanho}*)`.
    `game::SetMapNames` faz o mesmo e põe a escala em 1,3 (`kMapNameScale`). Fontes do jogo extraídas em `C:\Users\Paulo\ets2-font`.
- **Rumo à v3.3, 3ª rodada (2026-10-06).** Log do teste: todo clique dava "ponto removido". O `ui_world_map` trata o clique sozinho
  (o "select" do ponteiro) e adiciona o ponto à lista dele e ao GPS; o plugin, um quadro depois, achava esse ponto sob o cursor e o
  tirava, voltando à rota padrão. `MapClickWaypoint` e as funções `MAP_HIT/PICK/DROP/ADD/MOVE` saíram: o plugin só observa a lista do
  widget a cada quadro (`NativeMapSync`) e, quando ela muda, adota como rota, manda ao GPS e refaz a página no mesmo lugar.
  Nomes das cidades: escala 1,3 ficou grande demais com a `normal_o`; agora 0,85 (`kMapNameScale`). NÃO testado em jogo.
- **Rumo à v3.3, 4ª rodada (2026-10-06).** Mapa aprovado pelo usuário ("está mto bom"). A rota não ia para o serviço por dois motivos
  vistos no log: (1) escolher a carga depois de personalizar mudava a chave do serviço (empresas) e descartava os pontos: a chave agora
  é só origem|destino, e `NativeGpsPreview` recompõe `g_via` com as empresas novas em volta dos pontos do jogador; (2) `RunJobVia`
  esperava o reboque engatado ("GPS modo 1 com 1 alvo(s), reboques 2 solto") e o usuário olha o GPS logo ao iniciar: agora aplica de
  imediato (3 s depois do início, perto do ponto de partida, GPS com um alvo só) e de novo se o jogo refizer o GPS. NÃO testado em jogo.
- **v3.3 (2026-10-06): aprovada pelo usuário ("deu boa!").** Rota personalizada no mapa (clique numa estrada), levada ao GPS do
  serviço ao iniciar; nomes das cidades com a fonte contornada do jogo; sem o crash da roda. Último ajuste antes de salvar: os pontos
  entram no GPS ~1 s depois de iniciar (era 3 s; verificação a cada 10 quadros), não testado em jogo.
- **Rumo à v3.3.1 (2026-10-06): menos piscadas no mapa.** Cada mudança refazia a página duas vezes (ao mandar a rota e quando o GPS
  respondia), e cada vez o widget de mapa é criado de novo e recarrega. Agora uma vez só, quando o GPS tem a rota: ao abrir o mapa pelo
  planejador, ao clicar numa estrada e em "Rota padrão" (`g_native_keep` enquanto `g_native_route_until`). NÃO testado em jogo.
  Ideia para zerar as piscadas, não feita: o mapa numa janela do jogo à parte, que não é refeita (incerto: ordem das janelas e cliques).
- **v3.3.1 (2026-10-06): aprovada pelo usuário ("ficou bem melhor").** A página do mapa é refeita uma vez por mudança.
- **Depois da v3.3.1 (2026-10-06):** "Limpar planejamento" também limpa a rota personalizada na hora (o botão do mapa ainda mostrava
  "(personalizada: N pontos)" porque a limpeza por mudança de serviço só rodava no quadro seguinte, depois de a página ser refeita).
  Botões de zoom do mapa com os ícones dos botões de zoom da tela de mapa do jogo (`/material/ui/button/minus.mat` e `plus.mat`,
  30×30, como em `/ui/world_map.sii`). Materiais de UI do jogo extraídos em `C:\Users\Paulo\ets2-uimat`. NÃO testado em jogo.
- **v4.0 (2026-10-06), salva a pedido do usuário.** Sobre a 3.3.1: "Limpar planejamento" limpa a rota personalizada, botões de zoom
  com os ícones do jogo, e o mapa não volta a centralizar na rota ao adicionar um ponto: a posição para onde o mapa olha fica em
  `+0x1a8` (x, altura, z do mundo, em float); `+0x1b4` é só o foco pedido em `0x1000b00`, e era ele que o plugin lia. O arrasto soma em
  `+0x8b0`/`+0x8b8` e a atualização do widget (`0x100f90e`) passa isso para `+0x1a8` e zera. `MapViewCenter` lê `+0x1a8` + o arrasto
  pendente. Esta última correção NÃO foi testada em jogo antes de salvar.
  Sobre "trocar a carga reinicia a rota": não há caminho no código que faça isso desde `9e30f9e` (a chave é só origem|destino) e o
  log não mostra nenhum descarte por troca de carga depois dele; o mais provável é a recarga a quente do núcleo às 21:07 (um deploy
  meu durante o teste), que zera a rota guardada em memória. Nada mudado no código para isso.
- **v4.0.1 (2026-10-06), publicada a pedido do usuário.** Botões de zoom do mapa: `0x10005c0` só troca o nível e a altura do nível
  (`+0x1e0`), o que muda o tamanho de ícones e nomes mas não a vista; a vista usa a altura em `+0x1ac` (o y da posição em `+0x1a8`),
  que `0x1000b00` copia de `+0x1e0`. `SetMapZoom` agora faz essa cópia. (Os botões nunca tinham funcionado de verdade; a roda sempre
  foi do próprio widget.) "Rota padrão" encostado nos botões de zoom; texto de ajuda centralizado sob o mapa. NÃO testado em jogo.
- **Rumo à v4.0.2 (2026-10-06).** NÃO testado em jogo.
  - **Laço infinito ao abrir o mapa** (o usuário associou a escolher a carga antes; o log mostra outra coisa): "a lista do mapa mudou
    (1 nós) … desfeito" em repetição. O caminhão estava parado NA empresa de origem (lkwlog, Luxemburgo): o jogo tira do GPS o ponto
    em que o caminhão já está, a lista do widget volta só com o destino, e o plugin tomava isso por uma mudança a desfazer, refazendo a
    página sem parar. Com a carga escolhida a origem é a empresa da carga, justamente onde o caminhão estava. `NativeMapSync` agora
    compara só os pontos do jogador (sem as duas empresas) e não "desfaz" mais nada.
  - Botão "Maior rota possível" fora da tela nativa (a função `PickLongestRoute` fica; o F8 ainda tem o dele); a linha "N empresas na
    origem, M no destino" subiu para o lugar.
- **v4.0.2 (2026-10-06): confirmada em jogo pelo usuário ("corrigido") e publicada.**
- **Rumo à v4.0.3 (2026-10-06): peso das cargas.** O usuário viu 20 t na lista e 16 t no serviço, e 0 t na lista e 6 t no serviço.
  - O gerador só lia os reboques genéricos (`vehicle/trailer_defs`). Várias cargas trazem o próprio reboque em `cargo/<carga>/*.sii`
    (cegonhas, reboques que são a própria entrega…); sem eles a conta caía na massa de UMA unidade (carros: "0 t").
  - Reboque que é a entrega (massa da unidade 0,0001; Feldbinder, Krone…): o serviço mostra o peso do próprio reboque
    (`chassis_mass + body_mass`, 6.160 kg nos Feldbinder = os "6 t" vistos). Agora é isso que a lista mostra.
  - O jogo escolhe um dos reboques do tipo de carroceria, e o peso muda com ele (maçãs: 11,6 t no curto, 23,2 t no de 3 eixos). A lista
    mostrava só o maior. Agora `routes.tsv` leva o menor numa 6ª coluna e a tela mostra a faixa ("12-23 t", `CargoTonnes`); um número só
    quando não varia. A ordenação por peso usa o maior. 321 das 413 cargas têm faixa de mais de 2 t.
  - Fora da conta (ponytail): reboques duplos e limites de peso por país. NÃO conferido em jogo.
