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
| 1.4 | Diag: códigos de erro do XeLL e callback de log do XeLL | `native_xell_failure` com operação e `xell_result`; mensagens do SDK limitadas | `xemfg_native_simulation-sleep` registra a falha num caso forçado | pendente |
| 1.5 | Diag: structs recusadas por versão/`next` | `native_struct_rejected` com API, tipo, versão e `next` | cenário da matriz `reflex-v2` (ainda backlog no comportamento) | pendente |
| 1.6 | Diag: consultas de suporte do jogo (`slIsFeatureSupported`, `slGetFeatureRequirements`) em passthrough | `native_feature_query` com feature e resultado; nenhum valor alterado | cenário fixture `feature-query` | pendente |
| 1.7 | Diag: amostra de constantes na primeira admissão e histograma de tipos de tag/viewports | `native_constants_sample`, `native_tag_histogram` | `xemfg_native_diagnostics` | pendente |
| 1.8 | Diag: exportar o relatório de compatibilidade em disco ao aposentar a saída e no shutdown | `MFGAdaUnlock-XeMFG-compatibility.json` escrito fora do hot path | idem | pendente |
| 1.9 | Matriz de conformidade no fixture sintético, label `backlog` | Casos registrados e falhando pela razão esperada: `reflex-v2`, `reflex-next`, `constants-v3`, `tag-next`, `backbuffer-subrect`, `menu-viewport-first`, `late-interposer`, `sl1-interposer` | `ctest -L backlog` lista todos | pendente |
| 1.10 | Streamline_Sample em 3-4 versões 2.x, incluindo 2.11.1 | Build reproduzível por script; smoke com o addon reporta `ready` | `xemfg_sl_sample_<versão>` (opcional, pulado sem SDK) | pendente |

## Fase 2 — Falhas duras (Etapa 2 da auditoria)

**Antes de começar:** perguntar ao usuário o hardware-alvo. Se incluir RTX 20/30, acrescentar o item 2.5.

| # | Tarefa | Critério de saída | Teste | Status |
|---|---|---|---|---|
| 2.1 | `ReflexOptions` v2+ ou com `next`: encaminhar sem devolver erro ao jogo, zerando só o nó base | O jogo recebe o resultado original; `reflexOptionsReady` verdadeiro | `reflex-v2`, `reflex-next` saem do backlog | pendente |
| 2.2 | Leitura por prefixo de structs com versão maior (`Constants` v3+, `DLSSGOptions` >5, `DLSSGState` >4, `ResourceTag` com `next`) | Prefixo conhecido usado e fato registrado; versão menor que a mínima continua recusada | `constants-v3`, `tag-next` | pendente |
| 2.3 | Não instalar hooks de entrada fora do SL 2.x | Interposer sem `slGetFeatureFunction` ou com `sdkVersion` < 2 não recebe detours de entrada | `sl1-interposer` | pendente |
| 2.4 | `LdrRegisterDllNotification` para `sl.interposer.dll`, com instalação fora do callback | Interposer carregado após a janela de 20 s ainda é isolado antes do `slInit` | `late-interposer` | pendente |
| 2.5 | (Só se RTX 20/30 for alvo) contorno de `slIsFeatureSupported`/requisitos DLSS-G | A definir com o usuário | a definir | condicional |

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
