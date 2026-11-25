# Crome — Descoberta: migração de RVAs entre builds Antica Retro

**Data:** 25/11/2026  
**Projeto:** Crome / Antica Retro OpenGL x64  
**Origem:** análise estática das builds e logs de leitura do jogador/alvo neste chat  
**Status:** referência para engenharia reversa; execução nativa **ainda não validada**

## Descoberta principal

As builds do cliente **0x6B06B118** e **0x6B06FF0F** apresentam **rotinas funcionalmente semelhantes e caminhos de código relacionados**. Diversas funções mantêm características da estrutura e sofreram deslocamento de RVA. O padrão não é universal: **não calcular RVAs novos apenas somando 0x80**, pois os deslocamentos variam entre áreas.

## Mapeamento observado

| Elemento | RVA antiga (0x6B06B118) | RVA nova (0x6B06FF0F) | Diferença |
|---|---:|---:|---:|
| Contexto Game | 0xEA5740 | 0xEA9710 | +0x3FD0 |
| Ponteiro LocalPlayer | 0xEA5760 | 0xEA9730 | +0x3FD0 |
| Slot target | 0xEA5770 | 0xEA9740 | +0x3FD0 |
| Slot follow | 0xEA5780 | 0xEA9750 | +0x3FD0 |
| Game::attack (candidato forte) | 0x1435B0 | 0x143630 | +0x80 |
| Game::follow (candidato forte) | 0x143800 | 0x143880 | +0x80 |
| Game::autoWalk (candidato forte) | 0x140C80 | 0x140D00 | +0x80 |
| Montagem/envio de caminho | 0x257230 | 0x257B10 | +0x8E0 |
| Chamada para autoWalk | 0x15F69E | 0x15F71E | +0x80 |

## Evidências e ressalvas

- Na build antiga, os relatórios de desmontagem relacionaram o código do alvo com a função em `0x1435B0`, e apontaram a emissão de mensagens associadas a ataque/follow (`0xA1`/`0xA2`) e caminho (`0x64`).
- Na build nova, a comparação estática identificou equivalentes em RVAs próximas e globais realocados.
- Os testes anteriores com Cave Rat observaram seleção de criatura, movimento e dois incrementos de XP de +10, associados à limpeza do alvo. **Foram ações manuais; não são prova de execução da ABI.**
- Ponteiros de criatura e endereços absolutos mudam entre sessões. Validar base do módulo, hash, tipo de objeto, gestão de referências e thread de jogo antes de qualquer chamada.
- **`nativeActionsReady=false`** até teste de ação nativa independente, em ambiente autorizado e controlado.

## Regra reutilizável para próximas atualizações

1. Comparar PE timestamp, SizeOfImage e hash `.text`; bloquear build desconhecida.
2. Identificar o padrão estrutural das rotinas antigas na build nova; correlacionar xrefs e opcodes.
3. Reencontrar os globais LocalPlayer/target/follow por referências ao código, em vez de assumir deslocamento fixo.
4. Registrar para cada RVA o status `candidate`, `static_evidence` ou `runtime_validated`.
5. Isolar testes antes de habilitar funções no Crome principal.
