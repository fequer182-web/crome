# Crome Ramonia Pipe Bridge V11

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


## V8 - monster snapshot cache

The temporary heap/vtable monster discovery is now throttled with a 1000 ms snapshot cache instead of rescanning process memory on every Target poll.

`GET_VISIBLE_MONSTERS` reuses the fresh snapshot. `ATTACK_ID` no longer triggers a full heap scan on the game thread; it resolves the requested ID from the cached snapshot and revalidates the cached address (Monster vtable + creature ID) immediately before `Game::attack`.

This is an interim performance fix until the confirmed Map/Tile/creature registry path replaces heap scanning entirely.


## V9 - no WndProc subclass

Live isolation showed Ramonia remained stable without the bridge, while it could crash on creature death with the bridge loaded even when Target was disabled. The previous bridge permanently replaced the game window WndProc with `SetWindowLongPtr(GWLP_WNDPROC)`.

V9 removes the WndProc subclass completely.

Game-thread actions now use a thread-scoped `WH_GETMESSAGE` hook on the existing game window thread. The pipe thread posts a private thread message, waits on an event, and the hook executes the action on the game thread. Each dispatch uses a token so a timed-out/late message cannot execute a stale pending action.

The V8 1000 ms monster snapshot cache remains in place.


## V10 - read-only Tile::things monster proof

V10 adds a diagnostic command only:

`GET_TILE_MONSTERS`

It does not attack, follow, write game memory, or call an internal game function.

The diagnostic reuses the V37 runtime-confirmed Tile contract:

- Tile vtable RVA `0x774208`;
- `Tile::things` vector at `+0x24`;
- vector end/cap at `+0x28/+0x2C`;
- element size `4`;
- Monster vtable and Creature ID/name/HP/position fields already confirmed by the profile.

For this proof, the bridge scans for exact live-looking Tile objects, snapshots each Tile::things vector only when the vector header remains stable for the whole read, and reports nearby same-floor Monsters found as members of Tile::things. The command is intentionally one-shot/manual and is NOT wired into the Crome Target loop yet.

External test:

`CromePipeTest_x64.exe tilemon [PID]`

A successful response returns `source:"Tile::things"`, `readOnly:true`, and Monster ID/name/HP/position/distance plus Tile membership diagnostics.


## V11 - cached Tile::things Target resolver

Live V10 validation proved that the same Spider creature ID remained stable while HP, position and distance changed, and that two simultaneous Spiders were independently visible through Tile::things.

V11 moves the real Target path onto that validated source.

- GET_VISIBLE_MONSTERS no longer uses the broad Monster heap scan.
- The bridge performs a broad scan only to discover exact Tile objects and caches their addresses.
- Normal Target polls traverse only cached Tile::things vectors.
- Tile vector pointer arrays are copied as one snapshot and the vector header is rechecked before any Thing is trusted.
- ATTACK_ID first verifies the requested creature ID is currently present in Tile::things on the pipe thread.
- The game-thread dispatcher resolves the same creature ID again from current cached Tile::things immediately before Game::attack.
- No Monster pointer from the old broad heap scan is used by ATTACK_ID.
- Broad Tile discovery is refreshed on cache initialization, large movement/floor change, or a long safety interval; it is never run from the game thread.

GET_TILE_MONSTERS now reports source `Tile::things-cache`, cachedTiles and cacheAgeMs for diagnostics.
