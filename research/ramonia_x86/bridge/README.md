# Crome Ramonia Pipe Bridge V7

IPC oficial do Crome para Ramonia x86.

## Estado atual

A bridge valida a build e o profile V37 antes de liberar actions.

Comandos read-only:

- `HELLO`
- `BUILD_INFO`
- `GET_PLAYER`
- `GET_COMBAT_TARGETS`
- `GET_VISIBLE_MONSTERS` — retorna `id`, `name`, `hpPercent`, posição e distância

Actions:

- `ATTACK_SELECTED` - compatibilidade/teste usando Follow ou AttackTarget ja existente.
- `ATTACK_ID` - recebe somente `creatureId`; o ponteiro e resolvido dentro da bridge x86.

O endpoint e criado por PID:

`\\.\pipe\Crome.Ramonia.<PID>`

O processo externo pode ser x64 enquanto a bridge carregada no Ramonia e x86.

## Contrato de attack confirmado

Build Ramonia atual:

- `Game::attack` RVA `0x79190`
- `ECX = Game*`
- arg1 = `Creature*`
- arg2 = `bool Cancel`
- para iniciar ataque real: `Cancel=false`

Esse contrato foi confirmado em runtime: o alvo foi selecionado e recebeu dano.

A chamada e executada pelo dispatcher no thread da janela do jogo. A thread do
named pipe nao chama `Game::attack` diretamente.

## ATTACK_ID

O cliente externo envia somente:

`{"cmd":"ATTACK_ID","creatureId":1073768932}`

A bridge procura um Monster vivo/visivel usando o profile atual, valida vtable,
ID e posicao, e somente entao envia a chamada para o game thread. Nenhum ponteiro
de processo cru atravessa o IPC.

## Teste

Baseline:

`CromePipeTest_x64.exe`

Listar monsters visiveis sem usar Follow:

`CromePipeTest_x64.exe monsters`

Atacar pelo ID retornado:

`CromePipeTest_x64.exe attackid <creatureId>`

O modo antigo continua disponivel:

`CromePipeTest_x64.exe attack`


## Target loop externo

O primeiro loop automatico fica no cliente externo x64, mantendo regra de bot fora da bridge:

`CromePipeTest_x64.exe target Rat`

Wildcard:

`CromePipeTest_x64.exe target *`

O loop preserva o alvo atual enquanto ele continuar vivo e corresponder ao filtro. Quando morrer ou desaparecer, escolhe o mais proximo entre os monsters compatíveis e chama `ATTACK_ID`. Use `CTRL+C` para encerrar o teste.


## V7 - safer ATTACK_ID handoff

`ATTACK_ID` now carries only the semantic `creatureId` from the pipe thread to the game thread. The live `Monster*` is resolved and revalidated on the game/window thread immediately before `Game::attack`.

This removes the previous cross-thread raw-pointer lifetime window where a creature could die/despawn after the pipe-thread scan but before the native attack call.
