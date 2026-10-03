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
