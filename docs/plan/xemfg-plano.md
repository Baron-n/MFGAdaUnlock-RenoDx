# XeMFG — plano de universalização

Fonte: `docs/research/xemfg-universalidade-2026-10-21.md` (auditoria). Branch: `feature/1.5.0-xemfg-universal`. Base: tag `rc5-baseline` (`1c9a0b5`).

**Retomada:** a próxima sessão começa pelo primeiro item com status `pendente` ou `em andamento`. Cada item é um commit. Status possíveis: `pendente`, `em andamento`, `feito (<commit>)`, `bloqueado (<motivo>)`.

## Como verificar cada item

- **Matriz:** `ctest -C Release -LE backlog` em `build/tests` deve ficar 100% verde; os opcionais pulados são os de sempre (`provider_identity`, `engineering_manifest`, `release_1_4_5_manifest`).
- **Backlog:** `ctest -C Release -L backlog` lista os casos ainda vermelhos. Cada item tira o seu caso do backlog (remove o label) e o deixa verde na matriz.
- **Replay:** `ctest -C Release -R xemfg_replay` roda o analisador `tools/xemfg_triage.py` sobre `tests/fixtures/replay/*.jsonl` e compara com `tests/fixtures/replay/expected.json`.
  - Os logs não têm dados para reexecutar o addon; o replay prova que **o diagnóstico continua classificando cada sessão de campo pela causa conhecida**.
  - Quando um item mudar a causa esperada de uma sessão futura, o `expected.json` só muda com nova evidência de campo, nunca para esconder regressão.

## Fase 1 — Infra de verificação

| # | Tarefa | Critério de saída | Teste que fica verde | Status |
|---|---|---|---|---|
| 1.1 | Corpus de replay (RC3/RC4/RC5 de Plague Tale e DS2) + analisador de triagem | O analisador reporta, por sessão, build, primeiro bloqueio persistente e causa: PT-RC4 sem espera; PT-RC5 espera de fronteira falhando no XeLL; DS2 jitter não suportado (Streamline e XeFG nativo) | `xemfg_replay_corpus` | feito (ver git log) |
| 1.2 | Diag: cabeçalho de sessão (horário real, executável, GPU/driver, ReShade, módulos relevantes) | Evento `native_session` único por processo, sem caminhos pessoais | `xemfg_native_diagnostics` (novo cenário) | feito |
| 1.3 | Diag: `sdkVersion` do `slInit` e versões de arquivo de `sl.*` | `native_streamline_version` no log e `streamline.sdk_version` no relatório | idem | feito |
| 1.4 | Diag: códigos de erro do XeLL e callback de log do XeLL | `native_xell_failure` com operação e `xell_result`; mensagens do SDK limitadas | `xemfg_native_simulation-sleep` registra a falha num caso forçado | feito (sem caso forçado; ver registro) |
| 1.5 | Diag: structs recusadas por versão/`next` | `native_struct_rejected` com API, tipo, versão e `next` | `xemfg_native_struct-probe` | feito |
| 1.6 | Diag: consultas de suporte do jogo (`slIsFeatureSupported`, `slGetFeatureRequirements`) em passthrough | `native_feature_query` com feature e resultado; nenhum valor alterado | todos os testes nativos (`feature_query`) | feito |
| 1.7 | Diag: amostra de constantes na primeira admissão e histograma de tipos de tag/viewports | `native_constants_sample`, `native_tag_histogram` | todos os testes nativos | feito |
| 1.8 | Diag: exportar o relatório de compatibilidade em disco ao aposentar a saída e no shutdown | `MFGAdaUnlock-XeMFG-compatibility.json` escrito fora do hot path | todos os testes nativos (`report_export`) | feito |
| 1.9 | Matriz de conformidade no fixture sintético, label `backlog` | Casos registrados; os que falham ficam no `backlog`, pela razão esperada | `ctest -L backlog`: `reflex-v2`, `reflex-next`, `constants-v3`, `backbuffer-subrect`, `menu-viewport-first`, `sl1-interposer`, `constant-sleep-token` | feito |
| 1.10 | Streamline_Sample em 3-4 versões 2.x, incluindo 2.11.1 | Build reproduzível por script; smoke com o addon reporta `ready` | `xemfg_sl_sample_<versão>` (opcional, pulado sem SDK) | feito |

## Fase 2 — Falhas duras (Etapa 2 da auditoria)

**Antes de começar:** perguntar ao usuário o hardware-alvo. Se incluir RTX 20/30, acrescentar o item 2.5.

| # | Tarefa | Critério de saída | Teste | Status |
|---|---|---|---|---|
| 2.0 | **Sequência contígua de frames para o SDK Intel** (achado de campo do Plague Tale, item 1.4): o XeLL ligado ao XeFG exige IDs de sleep consecutivos; o jogo dorme no frame 1 no carregamento e volta no 562. A ponte atribui um ID próprio e contíguo a cada frame que dorme e o usa em `xellSleep`, nos markers, nos tags, nas constantes e no `SetPresentId` | Plague Tale passa a interpolar; nenhum `but expected frame` no log | `constant-sleep-token` sai do backlog; nova sessão de campo do Plague Tale no corpus | feito |
| 2.1 | `ReflexOptions` v2+ ou com `next`: encaminhar sem devolver erro ao jogo, zerando só o nó base | O jogo recebe o resultado original; `reflexOptionsReady` verdadeiro | `reflex-v2`, `reflex-next` saem do backlog | feito |
| 2.2 | Leitura por prefixo de structs com versão maior (`Constants` v3+, `DLSSGOptions` >5, `DLSSGState` >4, `ResourceTag` com `next`) | Prefixo conhecido usado e fato registrado; versão menor que a mínima continua recusada | `constants-v3`, `tag-next` | feito |
| 2.3 | Não instalar hooks de entrada fora do SL 2.x | Interposer sem `slGetFeatureFunction` ou com `sdkVersion` < 2 não recebe detours de entrada | `sl1-interposer` | feito |
| 2.4 | `LdrRegisterDllNotification` para `sl.interposer.dll`, com instalação fora do callback | Interposer carregado após a janela de 20 s ainda é isolado antes do `slInit` | `late-interposer` | pendente |
| 2.5 | Alvo confirmado pelo usuário: **RTX 20/30/40/50**. Em placas sem DLSS-G nativo, o jogo precisa ver o DLSS-G como suportado para mostrar a opção, e o XeMFG gera os frames | Jogo com Streamline mostra e ativa o FG numa GPU sem DLSS-G; nenhuma mudança em RTX 40/50 | caso `unsupported-dlssg` no fixture (retorno de suporte negado) | pendente |

## Fase 3 — Convenções aprendidas e perfis (Etapas 1 e 5)

| # | Tarefa | Critério de saída | Teste | Status |
|---|---|---|---|---|
| 3.1 | Carregador de `profiles.json` (recurso embutido + `MFGAdaUnlock.profiles.json` na pasta do jogo) e log do perfil aplicado | Perfis só ajustam política; JSON inválido é ignorado com log | `xemfg_profiles` (unit) | pendente |
| 3.2 | Mover para perfis: observador Steam, retenção da saída nativa, REFramework, conflitos de plugin | Comportamento idêntico ao RC5 com o perfil padrão; sem nomes de módulo no código | testes de observador/continuidade/REFramework existentes | pendente |
| 3.3 | Convenção depth/jitter aprendida: divergência ⇒ recriação da saída com as flags corretas; persistência em `[XeFG.Learned]` por hash do executável | Primeira sessão converge após uma recriação; a seguinte já inicia certa; overrides manuais têm prioridade | novo cenário `learned-depth` | pendente |
| 3.4 | Remover `native_game_defaults.hpp` | TLOU2/AW2/DS2 cobertos por aprendizado ou perfil com evidência | `xemfg_native_*` + replay | pendente |

## Fase 4 — Correções de dados (Etapa 4)

| # | Tarefa | Critério de saída | Teste | Status |
|---|---|---|---|---|
| 4.1 | Teste numérico de câmera parada com jitter Halton (backlog) | Teste falha: MV com jitter não vira ≈0 | `xemfg_dejitter_numeric` | pendente |
| 4.2 | Compute pass de remoção de jitter do MV; XeFG inicializado sem `JITTERED_MV` | MV convertido com erro máximo < 1e-3 px no teste; DS2 deixa de ser recusado | `xemfg_dejitter_numeric`, `xemfg_native_jittered-motion` atualizado | pendente |
| 4.3 | `kBufferTypeBackbuffer` com subretângulo | `resourceBase/Size` do backbuffer seguem o tag | `backbuffer-subrect` | pendente |
| 4.4 | FP16/scRGB | Swapchain FP16 aceita, com conversão ou caminho do SDK | novo cenário `scrgb-output` | pendente |
| 4.5 | Projeto UE5 de teste com o plugin Streamline | Projeto e script de build/empacotamento | manual | pendente |

## Fase 5 — Adaptadores (Etapa 3)

| # | Tarefa | Critério de saída | Teste | Status |
|---|---|---|---|---|
| 5.1 | Extrair o adaptador Streamline para `FramePacket` sem mudar comportamento | Matriz e replay idênticos | matriz completa | pendente |
| 5.2 | XeFG nativo do jogo com o multiplicador do usuário | Contexto do jogo usa o multiplicador do INI | novo cenário `native-game-xefg` | pendente |
| 5.3 | Adaptador FidelityFX FG (`ffx_api`) | Fixture FFX chega a `ready` | `xemfg_ffx_adapter` | pendente |
| 5.4 | Adaptador DLSS-SR experimental, atrás de `[Adapters] DlssSr=1` | Desligado por padrão; fixture NGX chega a `ready` quando ligado | `xemfg_dlss_sr_adapter` | pendente |

## Final

| # | Tarefa | Status |
|---|---|---|
| F.1 | Instalar o addon final nos jogos (com backup do addon anterior) e testar o que for possível sem interação manual | pendente |
| F.2 | Registro final: o que foi feito e o que não foi, com motivos, em `docs/plan/xemfg-registro.md` | pendente |

## Registro de execução

(Atualizado a cada item.)
- **1.1** Corpus de 5 sessões de campo e `tools/xemfg_triage.py`. Achado: Plague Tale **não chama `slReflexSleep`** (zero recusas na RC5). A espera de fronteira da RC5 dispara, mas o XeLL a recusa (`eErrorReflexAPI`, 14). A hipótese de sleep dentro da simulação não se confirmou; o próximo bloqueio é o erro do XeLL (item 1.4).
- **1.2** `native_session` (UTC, executável, Windows, GPU/driver, build) no worker de descoberta e `native_modules` (nomes e versões de módulos relevantes) na criação da saída, só quando o inventário muda. Todos os testes nativos verificam os eventos e a ausência de caminhos. Matriz: 141 aprovados, 3 opcionais pulados.
- **1.3** `native_streamline_version` (sdkVersion do `slInit`, versões do interposer e plugins) e `streamline` no relatório. Achado: **em todas as sessões de campo o `slInit` acontece antes dos hooks**; a posse vem sempre do fallback `slSetD3DDevice`. Por isso o evento também sai nessa fronteira, com `sdk_version: "unobserved"`. Matriz: 141 aprovados.
- **1.4** `native_xell_context` (versão do XeLL, callback de log registrado), `native_xell_log` (avisos e erros do SDK, no máximo 64) e `native_xell_failure` (operação, `xell_result`, intervalo, modo). Recusas de marker pelo SDK passam a ser sempre registradas. Uma sonda com o `libxell.dll` real mostrou que `xellSetSleepMode` aceita qualquer intervalo, mas **`xellSleep` devolve -4 ("Sleep() called after markers.") quando aquele frame já recebeu markers**. É a explicação mais provável do erro 14 no Plague Tale; a próxima sessão de campo confirma. Não há caminho público para forçar essa falha no fixture sem código só de teste no addon, então o teste cobre o registro do contexto e do callback e exige zero falhas nos cenários válidos. Também corrigida a regressão de link do 1.3 no alvo de prévia (`native_session.cpp`): a matriz do 1.3 tinha rodado com um binário de prévia antigo. A partir daqui, build completo antes de toda matriz. Matriz: 141 aprovados.
- **1.4 (complemento)** Agrupamento das mensagens do XeLL por padrão (`log_budget.hpp`), para que o carregamento não esgote o orçamento antes do gameplay. Com isso, uma sessão de campo (o jogo pede o FG sozinho, sem interação) mostrou a **causa raiz do Plague Tale**: `Sleep() called for frame: 2339 but expected frame: 2`. A mensagem está no `libxell.dll` e só aparece com o XeLL ligado ao XeFG; o XeLL isolado aceita saltos (sonda com 7 sequências). O jogo pede espera repetidamente para o frame 1 durante o carregamento e salta para 562 no gameplay. Correção planejada como item 2.0; sessão adicionada ao corpus (`plague-tale-1.4-xell.jsonl`). Matriz: 141 aprovados; replay: 6 sessões.
- **1.5** `native_struct_rejected` (API, versão, `next`, motivo; agrupado por padrão) em `slReflexSetOptions`, `slDLSSGSetOptions`, `slDLSSGGetState` e `slSetConstants`. O cenário novo `xemfg_native_struct-probe` envia ReflexOptions v2 e com `next`, DLSSGOptions v6 e Constants v3, e exige os quatro registros. O comportamento (recusa) não mudou; ele muda nos itens 2.1 e 2.2. Matriz: 142 aprovados; replay: 6.
- **1.6** `native_feature_query` em passthrough para `slIsFeatureSupported` e `slGetFeatureRequirements`, com o resultado que o jogo recebeu. Com o Streamline real (do PRAGMATA), `slIsFeatureSupported(DLSS_G)` devolve `eOk` mesmo com o plugin DLSS-G desligado pela ponte: o isolamento não faz o jogo esconder o FG nessa versão, o que responde à dúvida do §4 da auditoria. Uma rodada da matriz mostrou que o worker de descoberta corre com as primeiras consultas do jogo; o teste agora repete a consulta depois da criação da saída. Essa corrida também vale em jogo (consultas muito precoces podem não aparecer) e será tratada no 2.4. Matriz: 142 aprovados; replay: 6.
- **1.7** `native_constants_sample`: as primeiras constantes de cada saída, literais (mvecScale, jitter, near/far/FOV/aspecto, as sete flags; `INVALID_FLOAT` sai como `null`). `native_tag_histogram`: todos os tags por tipo de buffer (inclusive os não mapeados) e por viewport, em 240 chamadas e na aposentadoria. No smoke, o histograma já mostra o tipo 23 (o HUD inválido de teste), que a ponte não mapeia. Matriz: 142 aprovados; replay: 6.
- **1.8** O relatório de compatibilidade é gravado sozinho em `MFGAdaUnlock-XeMFG-compatibility.json`, com `export_reason`: `steady` (240 chamadas de tag), `retired` (aposentadoria da saída) e `shutdown` (síncrono no `slShutdown`). As duas primeiras usam uma thread coalescida; a escrita é atômica (temporário + `MoveFileEx`). Matriz: 142 aprovados; replay: 6.
- **1.9** Matriz de conformidade `xemfg_conformance_<caso>`; cada caso afirma o comportamento desejado.
  - Já passam: `tag-next`, `late-interposer` (os eventos de device cobrem o interposer tardio) e `sleep-gap`.
  - No backlog, cada um falhando pelo motivo esperado:
    - `reflex-v2`/`reflex-next`: "Reflex disable".
    - `constants-v3`: entradas recusadas.
    - `backbuffer-subrect`: região não registrada.
    - `menu-viewport-first`: "native FG disable".
    - `sl1-interposer`: **o processo cai com 0xC0000005**. Os detours 2.x leem a ABI 1.x; isso confirma o risco que a auditoria marcou como não verificado.
    - `constant-sleep-token`: reproduz exatamente o Plague Tale.
  - **Causa raiz do Plague Tale, completa:**
    - O jogo chama `slReflexSleep` sempre com o token 0, em todo frame (8.514 chamadas após ligar o FG, no trace de campo), e manda os markers com tokens reais.
    - A disassembly do `xellSleep` (libxell 1.3.0.5 e 1.3.2.10) mostra que, depois de 16 sleeps aceitos, o XeLL entra em modo estrito e exige `último + 1` (campos `+0x6614`, `+0x6630`, `+0x6638`, contador `+0x6634`).
    - A ponte intercala `xellSleep(1)` com a espera de fronteira `xellSleep(N)`, e o modo estrito recusa uma e outra alternadamente.
  - **Correção do 2.0, agora definida:** uma espera explícita com token anterior à simulação mais recente é velha e nunca vai ao XeLL. A espera de fronteira cobre o frame real.
  - O log assíncrono perdia registros sob contenção de trava (duas intermitências na matriz); corrigido em commit próprio, com teste.
  - Gate: 145 aprovados; backlog: 7; replay: 6.
- **1.10** `tools/sl_sample.py` compila o Streamline_Sample oficial (tags v2.7.32, v2.9.0, v2.11.1 e v2.14.16, cada uma com o SDK da mesma versão baixado do GitHub).
  - Build só DX12, com o `dxc`/`fxc` do Windows SDK; não exige Vulkan SDK.
  - Um patch local (não versionado) faz `MFG_SAMPLE_DLSSG=1` iniciar com DLSS-G e Reflex ligados.
  - `tests/xemfg_sl_sample_tests.py` instala ReShade, addon e par Intel ao lado do sample, roda 30 s e aplica a triagem de campo.
  - **As quatro versões interpolam com runtime NVIDIA real**, inclusive a 2.11.1 do Plague Tale. Isso confirma que o problema do jogo é o token constante, não a versão do SL.
  - Testes `xemfg_sl_sample_*` (label `sl_sample`) só existem com `-DXEMFG_SL_SAMPLES_ROOT=<build/sl-samples>`.
  - Gate: 149 aprovados; replay: 7.
  - **Fase 1 concluída.** Próximo: fase 2, que começa com a pergunta do hardware-alvo.
- **2.0** A ponte só despacha `xellSleep` para um ID maior que o último despachado (`Output::lastXellSleep`). Uma espera com token velho ou repetido é recusada (`native_sleep_rejected`, `not_newer_than_dispatched`) e a espera de fronteira cobre o frame real.
  - `constant-sleep-token` saiu do backlog.
  - **Em campo, Resonance: A Plague Tale Legacy passou a interpolar** (`blocker: ready`, `enabled: 1`, lançamentos Tensor crescendo de 77k para 224k em 40 s); sessão `plague-tale-2.0-interpolating.jsonl` no corpus.
  - O teste unitário `xemfg_latency_translation` reutilizava IDs mais antigos para simular uma corrida com resize; foi renumerado e ganhou o caso de ID monotônico.
  - Gate: 150 aprovados com essa renumeração; replay: 8.
  - Hardware-alvo da fase 2 respondido: RTX 20/30/40/50 (item 2.5 confirmado).
- **2.1 + 2.2** (um commit só: as duas mudanças ficaram no mesmo arquivo durante o gate).
  - `ReflexOptions` v2+ ou com `next` não devolve mais erro ao jogo. A rota Reflex nativa encaminha a struct original; a rota XeLL encaminha o prefixo v1 com a cadeia `next` do jogo e zera só `mode`/`frameLimitUs`.
  - `Constants` v3+, `DLSSGOptions` >5, `DLSSGState` >4, `ResourceTag` v2+ e `sl::Resource` v2+ são lidos pelo prefixo conhecido (o ABI do Streamline só acrescenta campos). Tipo estrangeiro ou versão 0 continuam recusados. Cada adaptação gera `native_struct_adapted`.
  - Testes unitários de constantes atualizados para a nova política.
  - Saíram do backlog: `reflex-v2`, `reflex-next`, `constants-v3`.
  - Gate: 153 aprovados; replay: 8.
- **2.3** `StreamlineAbi2`: um interposer sem `slGetFeatureFunction` ou com versão de arquivo 0.x/1.x não recebe nenhum detour (nem de ciclo de vida nem de entrada) e gera `native_streamline_unsupported_abi` uma vez. O caso `sl1-interposer`, que derrubava o processo com 0xC0000005, saiu do backlog. Gate: 154 aprovados; replay: 8.
