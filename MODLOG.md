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

## v2.4.0 (2026-10-04) — escolta policial, etapa 1 (spawn + seguir por velocidade). EXPERIMENTAL
Pedido: 500 m depois de iniciar o serviço, carro de polícia do país com giroflex ligado seguindo
atrás, na mesma velocidade.
- **Comando `spawn` do console** (mesma tabela de g_set_time/goto/warp): handler 0x3f9df0, mensagens
  "[spawn] Usage: spawn (object) [params]", "Supported objects: 'vehicle', 'event'". Monta um placement
  a partir da câmera e chama **0x566960(traffic_mgr=[exe+0x36ae728], args*, placement*)**.
  Sintaxe (0x566960): `spawn vehicle` (aleatório), `spawn vehicle <N>` (N veículos),
  `spawn vehicle <nome> [cadeia de reboques]` → busca 0x938ed0, spawn 0x568120 (2ª tentativa
  "Forced spawn!"), log "Successfully spawned '%s' near [...]" / "Unable to spawn '%s' ...".
  Placement = f32 x,y,z locais + i16 setor x (+0xc) e z (+0xe) (setor = floor(mundo/512)) + quaternion.
- **Polícia por país:** `def/country/<c>/traffic*.sii`, bloco sob o banner "police" (às vezes
  "Police / State - Municipal, Border"), `object: traffic.<modelo>.pol_<c>`. Gerador grava na 4ª coluna
  da linha N (35 de 36 países; Islândia sem mapa). Tipo `traffic.vehicle_type.police`
  (traffic_storage_police.sii), `validation_groups: flares_emergency_min2max12`, `flares_beacon_none`.
- `plugin/escort.h`: `Spawn` chama 0x566960 direto com placement 30 m atrás do caminhão (telemetria:
  posição + heading 0..1) e args falsos {"spawn","vehicle",modelo}; `Find` acha o carro na lista de
  tráfego pelo nome do modelo (+0x518) mais perto do ponto; `Follow` escreve o limite (+0x430) =
  velocidade do caminhão + 0,4·(dist − 30 m) limitado a [−6, +10] m/s; perdido se sumir da lista,
  > 300 m ou > 10 m À FRENTE → novo spawn após ~3 s. `Release` devolve o limite original.
- País = o da cidade de ORIGEM (ainda não acompanha a troca de país no caminho).
- **Pendente:** giroflex (campo desconhecido; "emergency lights"/"Beacons" no exe), seguir em
  cruzamentos (a IA escolhe a própria rota → hoje só respawn), país atual.
- Outras pistas: DLC Special Transport tem escolta traseira (`back_escort_speed`,
  `back_escort_ws_position`, traffic_storage_escort_back.sii, flag "escort" = bit 8 de +0x4b8), mas
  segue trajetórias pré-definidas por rota.
- NÃO testado no jogo.

## v2.4.1 (2026-10-04) — escolta: destravar, excluir, painel F9
- 1º teste: bug meu (telemetria `on_job` atrasa alguns quadros → escolta desarmava na hora; corrigido
  com `g_escort_seen_job`). 2º teste: spawn ok ("carro encontrado no tráfego"), mas o carro NÃO anda e
  a distância só cresce.
- **Causa:** `spawn vehicle` cria o carro com **bit 63 (debug_pause)** em +0x4b8 (game.log:
  "[traffic] Removing AI crashed into debug-paused vehicle"). `Follow` agora limpa o bit a cada quadro.
- Às vezes o jogo recusa: "[traffic] Spawn error: access not allowed" + "Unable to spawn ... (spawning
  failed at closest position ...)"; e o carro pode nascer À FRENTE (o spawn gruda na faixa mais próxima).
  SPAWN_BEHIND 30 → 45 m; carro à frente continua contando como "perdido".
- **Excluir veículo de IA** (de 0x923951): `0xace9e0(veh+0x80)` e `flags |= bit 24`; o tráfego o
  descarta depois. `escort::Remove`. O plugin guarda todos os carros que criou (`g_escort_all`) e os
  exclui quando: o carro é perdido, o serviço termina/é cancelado, um novo serviço começa, o plugin
  descarrega, ou pelo painel.
- **Painel "Escolta" (F9, ação `Routes.escort`)**: situação, modelo, nº de carros no mundo; do carro:
  id, distância (à frente/atrás), velocidade real (física [+0x238]+0x70 se ativa, senão +0x434),
  limite, alvo, posição, flags (com [pausado]/[sendo removido]); botões "Criar carro agora / Trocar
  por um carro novo" e "Excluir carro(s) da escolta"; caixa da escolta automática.
- Tecla e janela novas escritas à mão em `spfPlugins\RoutePlanner\config\settings.json` (gotcha do
  SPF: não mescla manifesto novo em settings existente); cópia em `settings.json.bak-f9`.
- NÃO testado no jogo. Giroflex continua pendente.

## v2.5.0 (2026-10-04) — escolta conduzida pelo rastro do caminhão. EXPERIMENTAL
- v2.4.1 confirmado: carro nasce e anda. Mas não segue (a IA escolhe a rota). Usuário quer escolta real.
- **RE ao vivo (probe externo, carro de polícia id 14):**
  - Carro parado: escrever veh+0x28 fica (5/5 leituras). Carro andando (física ativa): o jogo
    reescreve veh+0x28 a partir do corpo físico em < 50 ms.
  - Cópias da posição: `A=[phys+0x20]` em +0x10c e +0x12c; `B=[A+0xf8]` em +0xa0 e +0x150.
    Escrever altura +2 m: **A+0x12c e B+0x150 movem o veículo**; A+0x10c e B+0xa0 não (passo anterior).
    Depois o carro cai sozinho (gravidade) → a física segue viva.
  - Layout de B (corpo rígido): +0x140 quaternion (x,y,z,w), +0x150 posição, +0x15c velocidade linear
    (bate com phys+0x70), +0x168 velocidade angular; cópia anterior em +0x90/+0xa0/+0xd0.
    A: +0x12c posição, +0x13c quaternion (w,x,y,z). Posições na origem da física, não do mundo.
- **Plugin:** `escort::Trail` grava o caminho do caminhão (1 m entre pontos, 250 m, zera em teleporte);
  `escort::Drive` leva o corpo do carro ao ponto do rastro GAP=30 m atrás: delta em x,z (máx. 1,5 m por
  quadro) em B+0x150 e A+0x12c, orientação do rastro (heading+pitch) em B+0x140/A+0x13c, velocidade
  linear = direção do rastro × velocidade do caminhão, angular 0; altura fica com a física. Também
  escreve o limite da IA (+0x430) = velocidade do caminhão e limpa debug_pause. Perdido = sumiu ou
  > 80 m do lugar. Sem corpo físico (longe) = a IA dirige.
  Spawn agora no ponto do rastro 45 m atrás (cai na faixa que percorremos).
- 500 m → 250 m. Painel da escolta: F9 → **Home** (manifesto + settings.json à mão).
- Riscos a observar: a IA "brigar" com o arrasto (tremedeira), o jogo fazer "revive" do carro para a
  faixa dele, altura errada em rampas. NÃO testado no jogo. Giroflex pendente.

## v2.5.1 (2026-10-04) — jogo fechou ao "Recarregar Framework"
- Sintoma: o processo saiu durante o `sdk reinit` (última linha do game.log), sem game.crash.txt novo,
  sem dump e sem evento no Windows; o framework.log para no meio da inicialização dos keybinds
  (provavelmente buffer não gravado).
- Diferença para as recargas que funcionaram: havia um carro de escolta vivo e a v2.4.1 chamava
  `escort::Remove` (0xace9e0 + bit 24) dentro do `OnUnload`. Hipótese (não confirmada): mexer no
  tráfego no meio do reinit do SDK derruba o jogo. Agora o `OnUnload` não chama nada do jogo; os
  carros ficam com a IA. Outra possibilidade em aberto: o corpo físico que alterei nos testes externos.
- **Regra:** nunca chamar funções do jogo em OnUnload. Para trocar de versão com escolta ativa:
  excluir os carros pelo painel (Home) ou cancelar o serviço antes de recarregar.
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

## v2.6.0 (branch `escolta`, 2026-10-04) — retomada: master v2.5 trazida, escolta em lista, Home chama a escolta
- Merge da master (v2.5: favoritas, maior rota, cidade atual + estudo do DLC) nesta branch; a master, a tag
  v2.5 e o backup v2.5 não foram tocados. Conflitos: versão (→ 2.6.0), routes_test (os dois lados), MODLOG.
- **Escolta como lista** (`g_escort_slots`, `EscortSlot`): rótulo, distância atrás pelo rastro, modelo, carro,
  estado próprio. Hoje 1 entrada ("Polícia", 30 m). Outro veículo = mais uma entrada com outra distância;
  `CallEscort` é onde se decide o modelo de cada uma.
- **País atual:** `PoliceModelHere` = polícia do país da cidade mais próxima do caminhão (CityPoints +
  NearestPoint da master), decidido na hora de chamar a escolta (não mais o país da origem do serviço).
- **Home = chamar/trocar a escolta agora**, com ou sem serviço (ação `Routes.escort`; settings.json já tinha
  KEY_HOME e a janela Escolta). Painel da escolta: botão "Escolta" na barra do topo do F8; nele "Chamar/Trocar
  a escolta agora" e "Dispensar a escolta". A regra automática (250 m após iniciar serviço) continua.
- Spawn de cada carro no ponto do rastro a (distância + 15 m); rastro curto → em linha reta atrás.
- A testar no jogo: condução pelo rastro (`escort::Drive`, corpo físico), nunca exercitada.

## v2.6.1 (branch `escolta`, 2026-10-04) — IA da escolta por velocidade + conferência do spawn
- Teste da v2.6.0: carro nasce, mas NÃO é conduzido. Log: desvio 10–20 m andando; com o caminhão parado o
  carro foge (5 → 80 m). Leitura ao vivo: limite = -0.0, velocidade 4,6 m/s.
- **Achado 1: escrever o corpo físico só "pega" com o carro parado.** Nos testes externos (v2.5 desta branch)
  o carro estava com limite 0 (corpo adormecido) e as escritas em B+0x150/A+0x12c ficaram. Com o carro andando,
  o motor de física sobrescreve a cada passo → `escort::Drive`/`Orientation` removidos. Conduzir o carro pelo
  rastro via corpo físico NÃO funciona.
- **Achado 2 (bug meu):** eu escrevia limite = velocidade do caminhão; parado, a telemetria dá -0 e limite
  negativo = "sem limite" para a IA → o carro saía andando. Agora limite mínimo 0,001 ("fique parado").
- **Nova lógica (`escort::Steer`)** — o carro continua sendo IA normal (dirige, rodas giram); o plugin faz o papel
  do controlador de escolta do DLC:
  - `Trail::Project`: ponto do rastro mais próximo do carro → distância atrás (pelo rastro) e desvio lateral;
    `Locate` cai para o referencial do caminhão quando o carro está longe do rastro ou na ponta dele.
  - `WantSpeed`: velocidade do caminhão + 0,35·(distância − alvo), limitado a −8/+12 m/s, 0..42 m/s; caminhão
    parado e carro quase no lugar → 0.
  - escreve o limite (+0x430), limpa debug_pause (bit 63) e allow_overtake (bit 20), e empurra a velocidade
    real da física (phys+0x70 e +0xe8, como no ets2-police): +3 m/s² para alcançar, −6 m/s² para frear.
  - "fora do caminho" (não está no rastro e > 12 m de lado, ou à frente do caminhão) ou > 350 m atrás por
    2,5 s → carro excluído e outro nasce no rastro.
- **Conferência do spawn:** depois de achar o carro recém-criado, `Place::Good`: ≥ 18 m atrás, ≤ 2,5 m de lado
  (mesma faixa) e sentido ≥ 0,7 (mesma direção). Senão exclui e tenta 10 m mais atrás (ciclo de 6 distâncias:
  alvo + 10 … alvo + 60 m). O rastro agora é gravado sempre (mesmo sem escolta ativa), para o primeiro carro já
  nascer na faixa por onde passamos; rastro de 400 m.
- Testes: projeção, lugar bom/ruim (faixa ao lado, contramão, à frente), velocidades.
- Limite conhecido: em cruzamentos a IA pode ir por outro caminho → troca de carro (aparece outro atrás).
  NÃO testado no jogo.

## v2.6.2 (branch `escolta`, 2026-10-04) — seguir pelos cruzamentos (curva forçada) e faixa do spawn
- v2.6.1 confirmado: o carro segue (IA por velocidade). Faltava: virar junto nos cruzamentos; e em rodovia
  de várias faixas às vezes nasce na faixa ao lado.
- **"Force navigation" do editor de tráfego** (0xcd1b00; strings "Traffic tool sub mode: Force navigation"):
  - posição → item do mapa: `0x6c6510(pos16*, false, raio 8.0|20.0, flag)` (pos16 = f32 x,y,z + i16 setores);
  - item do mapa → objeto de tráfego: `0x6d60f0(traffic_mgr, item)`;
  - objeto → faixa/curva mais próxima: virtual +0x80 `(obj, &{item*, dist=-1}, &placement32, 0x8000)`;
  - curva: virtual +0x08 = tipo (0x500000 = faixa de estrada comum, não pode); virtual +0x40 → flags de acesso
    (máscara 0xffffffffff = IA pode usar);
  - **`0x94d090(curva, on)`**: liga bit 6 de curva+0x74 ("forçada"), registra numa lista global (rva 0x2d895e0)
    e liga bit 7 nas curvas irmãs (mesma entrada, outras saídas) → toda IA que chega por aquela entrada segue a
    curva forçada. `0x94cfb0` mexe só no bit 7. `0x9452a0(objeto)` atualiza.
- **Plugin:** com a escolta ativa, a cada quadro `escort::CurveAt(posição do caminhão)`; se for curva de
  cruzamento nova → `ForceCurve(true)` e guarda com o hodômetro. Libera (`ForceCurve(false)`) quando o caminhão
  andou (maior distância da escolta + 60 m) além dela, ou ao dispensar/terminar/trocar. Máx. 24 de uma vez.
  Efeito colateral: nesse intervalo TODO o tráfego que entra por ali faz a mesma curva.
  No OnUnload nada é chamado → curvas ainda forçadas ficam assim até reiniciar o jogo (dispensar antes).
- Faixa do spawn: `Place::Good` agora exige ≤ 1,8 m de lado (antes 2,5; faixas têm 3,5–4,5 m).
- NÃO testado no jogo.

## v2.6.3 (branch `escolta`, 2026-10-04) — retorno: projeção com memória; diagnóstico da curva
- Teste da v2.6.2: "segue bem", mas num retorno o carro não seguiu. Log: NENHUMA linha "curva forçada" (a busca
  `CurveAt` nunca devolveu curva; quando virou junto foi escolha da IA) e a distância atrás pulando
  (32 → 146 → 192 → 24 → 287 → 376 m) com o carro a < 3 m do rastro → "saiu do nosso caminho" e troca.
- **Causa do salto:** depois de um retorno (ou 2ª volta na mesma rua) há dois trechos do rastro lado a lado e o
  "ponto mais próximo" caía no trecho errado. `Trail::Project(p, out, hint)`: com a distância atrás da última
  vez (`slot.hint`), só considera o trecho a ±40 m dela (se o carro estiver a ≤ 12 m dele); senão, o mais próximo.
  O hint nasce com a distância pedida no spawn e é atualizado enquanto o carro está no rastro. Teste novo (retorno).
- **Diagnóstico de `CurveAt`:** agora devolve a etapa em que parou (1 sem gerente, 2 sem item do mapa, 3 item sem
  objeto de tráfego, 4 nenhuma faixa, 5 faixa de estrada comum, 6 sem acesso de IA, 7 curva forçável, 9 exceção) e
  o tipo da faixa; o plugin loga "sob o caminhão: etapa N, tipo …" a cada mudança. Serve para descobrir por que
  nada foi forçado (hipóteses: tipo diferente de 0x500000 também nas ruas, raio/altura da busca, índice virtual).
- Outras coisas vistas no log: muitos spawns seguidos na contramão ("sentido -1.0") em pista dupla — rejeitados
  certo, mas ruidoso; um carro "6 m atrás" ao pedir 50 m (mesma ambiguidade do rastro).

## v2.6.4 (branch `escolta`, 2026-10-04) — curva forçada de verdade; puxar o carro para a nossa faixa
- Diagnóstico da v2.6.3 no jogo: na rua "etapa 5, tipo 0x500000"; no cruzamento e na rotatória
  "etapa 7, tipo 0x600000" → a busca `CurveAt` funciona (0x600000 = curva de prefab).
- **Por que nada era "forçado" no log:** `ForceCurve` chamava 0x94d090 (ok) e depois 0x9452a0(objeto), que lê o
  objeto do editor de mapas ([exe+0x36ae738], nulo no jogo) → exceção pega pelo SEH → devolvia false → o plugin
  não registrava a curva e repetia a cada quadro. Efeito real: as curvas FORAM forçadas (o carro ficou "no lugar"
  nas duas travessias) e nunca liberadas (ficam até reiniciar o jogo). A lista global de curvas forçadas existe
  no jogo (objeto em rva 0x2d895e0 com vtable, contagem 8). Corrigido: 0x9452a0 removida; bits 6/7 bastam.
- **Faixa:** sugestão do usuário (o carro ir para a pista em que estou). `Place::side` = quanto o nosso rastro está
  à direita (+) ou à esquerda (−) do carro (vetor direita = (−fz, fx)); `Steer` leva o deslocamento lateral da
  IA (+0x460; positivo = direita, o "encostar" do ets2-police) para `atual + side` (±7 m), a 1,2 m/s, quando o
  carro está no rastro, no mesmo sentido e entre 0,6 e 8 m de lado. A IA continua "achando" que está na faixa dela.
- NÃO testado no jogo. Reiniciar o jogo antes (curvas da sessão anterior ficaram forçadas).

## v2.7.0 (branch `escolta`, 2026-10-04) — recarga automática (hospedeira + núcleo); faixa só quando muda de verdade
- v2.6.4 confirmado: "ficou legal, ele troca de faixa" (o deslocamento lateral +0x460 funciona com sinal
  positivo = direita) e vira junto nos cruzamentos (curva forçada sem a 0x9452a0).
- **Recarga automática** (pedido do usuário), mesmo esquema do ets2-police:
  - `RoutePlanner.dll` = hospedeira (`plugin/Host.cpp`): manifesto, teclas, janelas, fonte; vigia
    `core\RoutePlannerCore.dll` (a cada 30 quadros) e recarrega de uma cópia em `core\live\core-N.dll`.
  - `core\RoutePlannerCore.dll` = núcleo (`plugin/Core.cpp`, ex-RoutePlanner.cpp): toda a lógica e o desenho.
    Contrato em `plugin/core_api.h` (`Core_Init`, `CoreApi`, `CoreExports`).
  - `Shutdown(game_calls_ok)`: na recarga a quente (quadro normal) o núcleo antigo exclui os carros da escolta e
    libera as curvas forçadas; no descarregamento do framework (sdk reinit) NÃO chama o jogo.
  - Bloqueio do mouse feito pela hospedeira (`CoreApi::SetMouseBlocked`): o SPF indexa o pedido pelo endereço de
    retorno, e um pedido feito por um núcleo já descarregado nunca seria retirado.
  - A pasta do plugin (routes.tsv, favorites.tsv) vem da hospedeira (`CoreApi::plugin_dir`); o núcleo roda de core\live.
  - `deploy.ps1`: compila as duas, testa, gera routes.tsv, troca a hospedeira só se o hash mudou (avisa "HOST
    CHANGED") e troca o núcleo por rename atômico → entra no jogo em ~1 s. O log mostra "núcleo #N (data hora)".
  - Estado que se perde numa recarga do núcleo: seleção na tela, serviço "armado" para a escolta, carros da escolta.
- **Faixa:** `Steer` só desliza o carro (+0x460) quando ele está em OUTRA faixa (> 2,2 m do nosso rastro,
  `LANE_APART`), até chegar a < 0,4 m; depois não mexe mais. Dentro da faixa a IA segue normalmente (centro da
  faixa, reage ao tráfego). Estado "trocando para a sua faixa" no painel.
- Limite conhecido: deslocado de faixa, a IA "pensa" que ainda está na faixa original (reage ao tráfego DELA).
  Troca de faixa lógica da IA (bits change_lane/lane_target de +0x4b8) não foi investigada.
- NÃO testado no jogo. Primeira vez: "Recarregar Framework" (ou reiniciar) para a hospedeira nova entrar.

## v2.7.1 (branch `escolta`, 2026-10-04) — faixa de volta ao contínuo; giroflex
- Usuário: a regra de faixa da v2.7.0 (só quando a mais de 2,2 m) seguia pior → voltou a do v2.6.4 (puxar
  continuamente entre 0,6 e 8 m). Recarga automática confirmada ("núcleo #2" entrou sozinho).
  Lição: não mexer na versão do CMake a cada ajuste (ela está na hospedeira → força "Recarregar Framework").
- **Giroflex — RE estático:**
  - Regra `on_special` do DLC (tokens das regras aparecem como constantes em 0x6b4c50/0x6b5bb0): só vale para
    veículo em modo escolta (bit 8); liga o bit 2 de [controlador+0x24] (controlador = 0x9245c0(veh, 8)); quem lê
    é o gerente oversize (0x76d994, avisos de distância). NÃO é luz.
  - Máscaras de luz (validação de veículos, 0x16bf898): 0x20 freio, 0x40 pisca esq., 0x80 pisca dir.,
    0x200 beacon, 0x1000 e 0x2000 emergency (a barra de teto da polícia: tipo police tem
    flares_emergency_min2max12 e flares_beacon_none).
  - **Rotina de luzes do tráfego 0x92b420** (por veículo, por quadro): objeto de luzes em veh+0x210, virtual
    +0x80 = "acender máscara", +0x88(0xffffffff) = apagar tudo (quando bit 37 engine_off). Acende 0x4000 e
    0x200 SEMPRE, 0x20 (freio, conforme veh+0x450), 0x100 (veh+0xe8 & 1), 3 (faixa [veh+0x428] com +0x98 ≠ 0:
    túnel/noite?), 0x40/0x80 pelos bits 50/51. Nunca 0x1000 → polícia do tráfego anda de teto apagado.
- **Plugin:** `escort::LightsOn(car, 0x1000|0x2000)` chama o virtual +0x80 do objeto de luzes a cada quadro
  (caixa "Giroflex ligado" no painel; para de tentar após 5 falhas). NÃO testado no jogo: pode piscar se o
  jogo limpar a máscara depois do nosso quadro, ou não ter efeito se emergency depender de outra coisa.
