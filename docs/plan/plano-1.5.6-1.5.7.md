# Plano 1.5.6 / 1.5.7: correções, HUD e pacing do XeMFG, startup do DLSS-G

Base: `fork/main` (release 1.5.5, `10b31a7`). Uma branch e um PR por bloco, sempre com base na `fork/main`.

## Como retomar

- A próxima sessão começa pelo **primeiro item `pendente` ou `em andamento`** da tabela.
- Cada item é pequeno (até ~1 h de trabalho) e vira **um commit**. No fim do item, o status muda para `feito (<commit>)` e uma linha entra no [registro](#registro-de-sessões).
- Status possíveis: `pendente`, `em andamento`, `feito (<commit>)`, `bloqueado (<motivo>)`, `descartado (<motivo>)`.
- Itens marcados **[você]** dependem de você (abrir o jogo, clicar no painel). Os outros eu faço sozinho.

## Gate de cada item

1. Build completo (`tools/Build-XeMFG.ps1` / alvo do addon).
2. `ctest -C Release -LE backlog` em `build/tests`, 100% verde. Os opcionais pulados são os de sempre (`provider_identity`, `engineering_manifest`, `release_1_4_5_manifest`).
3. `ctest -C Release -R xemfg_replay`: 8/8.
4. Teste novo do item: falha antes da mudança e passa depois.

Release só sai se todos os testes automáticos e de campo passarem sem regressão.

## Decisões já tomadas (2026-10-24)

- Branches: XeMFG #1 em `fix/xemfg-reserved-resource-state`; depois uma branch por bloco.
- 1.5.6 = blocos 1 a 3. 1.5.7 = blocos 4 e 5. O bloco 6 vem depois.
- DLSS-G: a recusa do runtime à contagem forçada é lembrada até o jogo desligar e religar o FG, o multiplicador mudar ou a swapchain ser recriada.
- XeMFG, rota Reflex: quando a espera do XeLL é recusada, o jogo recebe o resultado do `slReflexSleep` original.
- A recusa do DLSS-G (item 2.4) é validada só por fixture (não há jogo conhecido com plugin limitado).
- UI Composition continua desligado por padrão; as conversões de HUD só rodam com ele ligado.
- Limite pela taxa do monitor (P2): opção **desligada por padrão**, com aviso no painel.
- Pesquisa de qualidade de imagem do DLSS-G: descartada por agora.

## Achados dos logs de campo usados no plano

| Jogo | Achado | Item |
|---|---|---|
| Cyberpunk 2077 | HUD-less recusado por formato: `R8G8B8A8_TYPELESS` (27) | 4.2 |
| Mafia: The Old Country | Textura de UI recusada: `B8G8R8A8_TYPELESS` (90) com backbuffer `R10G10B10A2` (24) | 4.3 |
| Kingdom Come: Deliverance II | Único jogo instalado com saída **scRGB FP16** (`native_scrgb_ready`), mas não manda HUD-less | 4.4 |
| Death Stranding 2 (não instalado) | UI `R8G8B8A8_UNORM_SRGB` (29) contra backbuffer `UNORM` (28) | 4.2 (fixture) |

---

## Release 1.5.6

### Bloco 0: preparação

| # | Tarefa | Teste / critério | Est. | Status |
|---|---|---|---|---|
| 0.1 | Build limpo da 1.5.5 e matriz completa; anotar contagem de testes como referência | matriz verde, contagem no registro | 1 h | feito (sem commit: só build e matriz) |

### Bloco 1: XeMFG, estado reservado do recurso (branch `fix/xemfg-reserved-resource-state`)

| # | Tarefa | Teste / critério | Est. | Status |
|---|---|---|---|---|
| 1.1 | Cenário de conformidade: recurso A tagueado com estado válido, liberado; recurso B no mesmo endereço tagueado com `0x80000000` | caso novo no `backlog`, falhando por herdar o estado de A | 1 h | feito (`8a371fe`) |
| 1.2 | Guardar o último estado válido no próprio recurso (`SetPrivateData` com GUID próprio); remover `taggedStates` | caso 1.1 sai do backlog; a substituição no mesmo recurso (Witcher 3) continua passando | 1 h | feito (`4c06b10`) |
| 1.3 | **[você]** Witcher 3: mudar opções gráficas várias vezes com FG ligado | nenhuma perda de device; `native_tag_reserved_state` no log | 30 min | em andamento |

### Bloco 2: DLSS-G, crash e custo (branch `fix/dlssg-hook-safety`)

| # | Tarefa | Teste / critério | Est. | Status |
|---|---|---|---|---|
| 2.1 | Extrair o patch da vtable do NGX para um header testável | teste unitário com vtable falsa, comportamento idêntico | 1 h | pendente |
| 2.2 | Patch da vtable instalado uma única vez (trava) e nunca guarda o próprio hook como original | teste com 8 threads simultâneas: um patch, sem recursão | 45 min | pendente |
| 2.3 | Testar `session` nulo em `TryPatchFlipMeteringInModule` | teste de política/revisão; matriz verde | 20 min | pendente |
| 2.4 | Política pura de "recusa lembrada" em `force_policy.hpp` | testes unitários: lembra, reseta em off/on, multiplicador, swapchain | 1 h | pendente |
| 2.5 | Ligar a política no `HookedSetOptions` e nos pontos de reset | teste: com recusa, 1 chamada ao Streamline por frame em vez de 3 | 1 h | pendente |

### Bloco 3: XeMFG, Reflex e log (branch `fix/xemfg-reflex-sleep-log`)

| # | Tarefa | Teste / critério | Est. | Status |
|---|---|---|---|---|
| 3.1 | Cenário: rota Reflex com token constante (como o Plague Tale) | caso novo no backlog: `slReflexSleep` original não é chamado | 45 min | pendente |
| 3.2 | Rota Reflex chama o `slReflexSleep` original em toda recusa e devolve o resultado dele | caso 3.1 sai do backlog; rota XeLL inalterada | 45 min | pendente |
| 3.3 | Log do XeFG agrupado por padrão (`PatternBudget`), como o do XeLL | teste: 200 mensagens repetidas + 1 nova; a nova aparece | 45 min | pendente |
| 3.4 | **[você]** Plague Tale com saída Reflex | Reflex ativo no painel, interpolando, sem erro | 30 min | pendente |

### Release 1.5.6

| # | Tarefa | Est. | Status |
|---|---|---|---|
| R6.1 | Gate completo no build final de release | 1 h | pendente |
| R6.2 | **[você]** Regressão rápida: um jogo XeMFG (ex.: CONTROL) e um DLSS-G (ex.: Cyberpunk) | 30 min | pendente |
| R6.3 | Versão, `docs/releases/1.5.6.md`, merge na `fork/main` e `develop`, tag e release no GitHub | 45 min | pendente |

---

## Release 1.5.7

### Bloco 4: XeMFG, HUD e pacing (branch `feature/xemfg-hud-pacing`)

| # | Tarefa | Teste / critério | Est. | Status |
|---|---|---|---|---|
| 4.1 | **[você + eu]** Linha de base de pacing com a 1.5.6: PresentMon 30 s em Plague Tale e KCD2, mesma cena | resumo p50/p99 e frames por Present no registro | 1 h | pendente |
| 4.2 | HUD-less/UI do mesmo grupo de formato (TYPELESS, `_SRGB`) copiados para o formato do backbuffer | cenário de fixture com 27→28 e 29→28: entrada aceita e bits preservados | 1,5 h | pendente |
| 4.3 | UI em outro formato (BGRA8 → RGB10A2) convertida por shader, com modo de refinamento do Intel | teste numérico em WARP; **avaliar em campo, pode ser descartado** se o alpha de 2 bits piscar | 2 h | pendente |
| 4.4 | HUD-less FP16 convertido pelo encoder scRGB existente | só fixture: nenhum jogo instalado manda HUD-less em scRGB | 1,5 h | pendente |
| 4.5 | Painel: o que o jogo fornece (HUD-less/UI, convertido ou recusado) e aviso quando o UI Composition está disponível e desligado | teste de UI/preview | 1 h | pendente |
| 4.6 | Função pura do limite: base máxima = taxa do monitor ÷ multiplicador, com margem de VRR (~3%) | testes unitários (60/120/144/165/240 Hz, 2x–6x, limite do jogo menor que o calculado) | 45 min | pendente |
| 4.7 | Opção `[XeFG] DisplayCap` (desligada), painel com aviso quando base × multiplicador passa da taxa, ligação ao `minimumIntervalUs` do XeLL | cenário de conformidade: intervalo aplicado e removido; rota Reflex intocada | 1,5 h | pendente |
| 4.8 | **[você]** Cyberpunk e Mafia com UI Composition ligado | HUD nítido nos frames gerados, sem piscar; log mostra a conversão | 30 min | pendente |
| 4.9 | **[você + eu]** Plague Tale e KCD2: P2 ligado e desligado, PresentMon | cadência mais regular ou igual; nenhum frame a mais descartado | 1 h | pendente |

### Bloco 5: DLSS-G, startup e detecção (branch `perf/dlssg-startup`)

| # | Tarefa | Teste / critério | Est. | Status |
|---|---|---|---|---|
| 5.1 | Busca de marcador com cache de módulos inspecionados e busca rápida | teste unitário da busca; contagem de varreduras por módulo = 1 | 1 h | pendente |
| 5.2 | `OnInitDevice`/`OnInitCommandQueue` só sinalizam o worker | teste: nenhuma varredura na thread chamadora | 1 h | pendente |
| 5.3 | Sonda: `LdrRegisterDllNotification` dispara antes do DllMain de uma DLL de teste? | resultado registrado; decide o 5.4 | 45 min | pendente |
| 5.4 | Detecção por notificação (patch dentro do callback, sem varredura), mantendo o hook de `LoadLibrary` como reserva | teste com DLL carregada por `LdrLoadDll` direto | 1,5 h | pendente |
| 5.5 | **[você]** DLSS-G em 2 jogos (ex.: Cyberpunk, Alan Wake 2): MFG liberado de primeira, startup sem travadas | log mostra o patch antes do primeiro `SetOptions` | 30 min | pendente |

### Release 1.5.7

| # | Tarefa | Est. | Status |
|---|---|---|---|
| R7.1 | Gate completo no build final | 1 h | pendente |
| R7.2 | **[você]** Regressão rápida XeMFG + DLSS-G | 30 min | pendente |
| R7.3 | Versão, notas, merge, tag e release | 45 min | pendente |

---

## Depois: bloco 6, refatoração e fixture do DLSS-G

Um PR por item.

| # | Tarefa | Est. | Status |
|---|---|---|---|
| 6.1 | Fixture com Streamline falso para o DLSS-G (`SetOptions`, `GetState`, Reflex) | 3 h | pendente |
| 6.2 | Extrair `provider_patches` (gates, midpoint, thin geometry) do `addon.cpp` | 2 h | pendente |
| 6.3 | Extrair `streamline_patches` (teto, flip metering) | 1,5 h | pendente |
| 6.4 | Extrair `discovery` (worker, varreduras) | 1,5 h | pendente |
| 6.5 | Extrair `latency_guard` e publicar um snapshot único para a UI | 3 h | pendente |
| 6.6 | Extrair o overlay (UI) | 3 h | pendente |
| 6.7 | Limpezas: detecção de API duplicada, comentário repetido, atribuição redundante, `IsBadReadPtr` | 1 h | pendente |

## Fora do plano (registrado para não perder)

- XeMFG: associação de `slSetTag` sem token pelo marker `RenderSubmitStart` (pode cair no frame anterior).
- XeMFG: corrida de dados leve em `s.invert`/`s.jitter`; `Snapshot()` e `flush()` por Present.
- DLSS-G: `DLSSGOptions` acima da v5 desliga o override (ler pelo prefixo, como o XeMFG).
- DLSS-G: pesquisa de qualidade de imagem (cercas, retículo do HUD). Exige instrumentação por estágio antes de qualquer heurística nova.

## Registro de sessões

(Uma linha por item concluído: data, item, commit, resultado do gate e observações.)

- **2026-10-24 · 0.1** Build Release incremental da 1.5.5 na árvore `build/tests` já configurada (sem erros nem avisos). Matriz `-LE backlog`: **175/175**, opcionais de sempre pulados (`provider_identity`, `engineering_manifest`, `release_1_4_5_manifest`), replay incluído, ~16 min.
- **2026-10-24 · 1.1** (`8a371fe`) A correção da 1.5.3 não tinha teste automático; o caso `xemfg_conformance_reserved-state` cobre a substituição no mesmo recurso e a herança num recurso recriado no mesmo endereço. Falhou como esperado: o recurso novo herdou o estado 64 do liberado. **O risco era real**, não só teórico.
- **2026-10-24 · 1.2** (`4c06b10`) Estado guardado como private data no próprio recurso; mapa removido. Ajustes no cenário: quadros 10–13 (o quadro 9 tem a verificação de UI composition) e lotes de recursos candidatos para garantir a reutilização do endereço (o heap entrega blocos livres em ordem aleatória; uma tentativa só falhou 1 vez em 5 na matriz). Com a correção: 25/25. Sem ela: 5/5 falham pelo motivo certo. Matriz **176/176**.

