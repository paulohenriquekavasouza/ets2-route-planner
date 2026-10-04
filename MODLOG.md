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

## v2.5.0 (2026-10-04) — distância do serviço medida pelo GPS do jogo (master; não confundir com a v2.5.x da branch `escolta`)
- Pedido: calcular a rota real em vez da estimativa em linha reta ("caminho 2").
- **GPS do jogo** (final do handler `company_portal`, 0x5c9fc6): estado `[game+0x42f0]` (game =
  [exe+0x36ae6d8]) — 0, 2, 3, 4, 5 permitem; 1, 6, 7 = "Unable to override gps while on job".
  `0x7b47b0(game, alvo*, item_de_mapa_da_empresa, 0, 0)` monta um alvo de 24 bytes (1º dword = 2 →
  sem alvo); `0x4fad00(game+0x4128, 5, array{vtbl rva 0x21fafa8, data, size, capacity}*)` troca os
  pontos do GPS (copia o array com 0x4fe9d0). A distância da rota sai na telemetria
  (`SPF_NavigationData.navigation_distance`, metros).
- **Novo fluxo do Iniciar** (caixa "Distância (pagamento) medida pelo GPS do jogo", padrão ligada; exige
  o teleporte): 7h/tempo limpo → teleporte para o pátio da ORIGEM antes de criar o serviço
  (`TeleportToTrailerSpot` agora aceita a empresa por tokens) → 45 quadros → `SetGpsToCompany(destino)` →
  lê a distância até ficar 45 quadros sem mudar (mín. 30 quadros, > 1 km, limite ~8 s) → cria o serviço com
  esses km em params+0x64 → não teleporta de novo. Falhou em qualquer passo = estimativa antiga.
  Log: "distância: N km (GPS do jogo|estimativa; estimativa em linha reta M km)" → serve para calibrar
  o fator 19 × 1,2.
- "Maior rota" continua pela estimativa (o GPS só mede uma rota por vez).
- NÃO testado no jogo.
