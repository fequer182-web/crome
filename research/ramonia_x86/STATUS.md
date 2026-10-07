# Current baseline — Ramonia x86

## Probe

Version: **V37**

Canonical source:

`src/RamoniaResearchProbe.cpp`

The source was previously stored under the misleading historical name:

`OTCLab_BuildMigrator_V4_9_DIRECT_BIND_x86.cpp`

No functional code change was made during the repository consolidation.

## Canonical profile

`profiles/ramonia_x86/6AB03F94_E281E9CA5BA0AF2.json`

Confirmed core values:

- Game owner RVA: `0xA2D4D0`
- Map owner RVA: `0xA2D600`
- autoWalk: `0x76860`
- attack: `0x79190`
- follow: `0x79300`
- look: `0x77640`
- move: `0x77920`
- use: `0x780D0`
- useWith: `0x784F0`
- talk: `0x796F0`
- talkChannel: `0x79750`

Tile::things:

- begin `+0x24`
- end `+0x28`
- cap `+0x2C`
- stride `4`
- Tile vtable RVA `0x774208`

## V33.1 runtime capture safety + V37 ABI final

- target page protection is changed before arming;
- VEH does not call `VirtualProtect`;
- original byte is restored immediately on breakpoint hit;
- EIP is reset to the restored first instruction;
- diagnostic memory snapshots use `ReadProcessMemory`;
- protection restoration happens outside the VEH;
- breakpoint is one-shot and is not re-armed automatically.

## Rule

The profile is the source of truth for already-confirmed offsets/RVAs.
V34/V37 promote only classified runtime evidence into the portable `actions` profile block. The research probe remains non-calling; active calls now live in the separate Crome pipe bridge/runtime.


## V37 final contract

Already confirmed from V34 profile: `attack`, `follow`, `talkChannel`.

V37 finalizes the remaining move ABI: `autoWalk`, `look`, `move`, `use`, `useWith`. It validates direction-vector contents, stack-local shared-object holders and OTC Position encoding before promoting an action to `runtime_confirmed`.

The profile schema emitted by V35 is `otclab-ramonia-x86-research-v37` / `profileVersion: 37`.


V36 narrows the remaining ABI gap to `move.arg2`, using LocalPlayer-relative Position evidence and nested snapshots before promotion.


## V37 final contract

All primary actions used by the planned Crome runtime are now represented as `runtime_confirmed` in the canonical Ramonia profile.

The last gap, `move.arg2`, was closed from V36 runtime bytes as a direct Position with `u32 x + u32 y + u8 z`, validated against the live LocalPlayer position.

`useWith.arg2` is exposed semantically as generic `Thing` because runtime captures have shown different concrete RTTI targets while preserving the same shared-object holder ABI.


## Runtime bridge validation

The Ramonia pipe bridge is now beyond read-only proof-of-concept.

Confirmed end-to-end path:

- external x64 client -> named pipe -> injected x86 bridge;
- build/profile fingerprint match before enabling actions;
- WndProc dispatcher executes calls on the game/window thread;
- `Game::attack` at RVA `0x79190` confirmed with:
  - `ECX = Game*`
  - arg1 = `Creature*`
  - arg2 = `bool Cancel`
  - `Cancel=false` starts real combat and produces damage.

The canonical profile records the second bool argument. The V37 ABI capture now inspects two attack arguments and persists the observed bool value for future builds.

Bridge V5 adds:

- `GET_VISIBLE_MONSTERS`
- `ATTACK_ID`

`ATTACK_ID` keeps raw process pointers inside the x86 bridge. The external Crome side sends only a semantic `creatureId`.


## ATTACK_ID runtime-confirmed

Bridge V5 was validated end-to-end on the live Ramonia client:

- `GET_VISIBLE_MONSTERS` returned Monster ID + position without Follow/manual target;
- `ATTACK_ID <creatureId>` resolved the Monster pointer inside the x86 bridge;
- before the action both AttackTarget and FollowTarget were empty;
- after the action AttackTarget pointed to the requested Monster ID;
- the call executed through the game-thread dispatcher.

This closes the first semantic Crome action path without raw pointers crossing IPC:

`Crome x64 -> creatureId -> pipe -> x86 resolver -> Game::attack(Creature*, false)`.


## Automatic target loop

The x64 pipe test now contains the first external target-policy loop. It polls semantic monster snapshots, keeps the current matching target while alive, selects the nearest matching monster when needed, and invokes `ATTACK_ID`. This keeps target policy in the external Crome layer while the x86 bridge remains an ABI/runtime adapter.


## Automatic Target loop runtime-confirmed

The external x64 target loop was validated live against multiple consecutive Rats:

- selected Rat by semantic name without Follow/manual Attack;
- tracked HP transitions (for example 100 -> 85 -> 65 -> 30 -> 15);
- when the current Rat disappeared/died, automatically selected a new Rat ID;
- repeated across multiple targets until the user stopped the loop with CTRL+C.

A transient pipe reconnect race was observed between one-request pipe instances. The x64 client now retries short ERROR_FILE_NOT_FOUND / ERROR_PIPE_BUSY windows locally instead of surfacing them as target failures.


## Bridge V7 game-thread target resolution

After a live UI test produced an access violation in the Ramonia process, the ATTACK_ID handoff was hardened. The pipe side no longer stores a resolved Monster pointer. It stores only creatureId; the game-thread dispatcher resolves the current Monster pointer and validates it immediately before the native attack call. This specifically reduces stale-pointer risk when a creature dies/despawns during retargeting.


## Bridge V8 targeting performance

Live testing confirmed FPS drops only while Target was active. The cause was the provisional full MEM_PRIVATE/vtable scan being executed multiple times per second.

V8 adds a 1000 ms monster snapshot cache. GET_VISIBLE_MONSTERS reuses the cache, and ATTACK_ID uses the cached address only after game-thread vtable + creature-ID revalidation. This materially reduces scan frequency without changing semantic target selection.


## Bridge V9 dispatcher isolation fix

A/B testing established that Ramonia stayed stable when no bridge was injected, but could crash at creature death with the bridge loaded even while Target was disabled. This isolated the permanent WndProc subclass as the leading bridge-level suspect rather than Target selection or ATTACK_ID itself.

V9 removes SetWindowLongPtr/GWLP_WNDPROC entirely. Game-thread execution now uses a thread-specific WH_GETMESSAGE hook + PostThreadMessage + completion event. Dispatches are tokenized to invalidate late messages after timeout. The V8 scan cache is retained.


## Crome v0.8.1 empty-target idle guard

Live testing found a distinct UI/worker bug: with Target enabled, Attack Any disabled, and an empty target-name list, the external worker still polled GET_VISIBLE_MONSTERS and GET_COMBAT_TARGETS even though no monster could ever match.

v0.8.1 treats this configuration as a true idle state. It performs no monster scan/combat-target polling until either at least one target name is configured or Attack Any is enabled. Empty target rules are also ignored during name matching.


## Bridge V10 Tile::things read-only proof

The next crash-isolation step does not modify Target behavior. V10 adds GET_TILE_MONSTERS as a one-shot read-only diagnostic.

It scans exact Tile-vtable objects, validates the runtime-confirmed Tile::things vector (+0x24/+0x28/+0x2C, elem 4), requires a stable vector header during the snapshot, and reports only same-floor nearby Monster objects that are actual members of a Tile::things collection.

No attack/follow/internal call is performed by this diagnostic. The existing GET_VISIBLE_MONSTERS heap scanner and ATTACK_ID path remain unchanged until this Tile-membership proof is validated live.


## Bridge V11 cached Tile Target resolver

V10 live proof succeeded with moving/damaged creatures and multiple same-name Spiders. V11 promotes Tile::things from diagnostic-only to the Target resolver.

GET_VISIBLE_MONSTERS now reads Monsters only as members of cached Tile::things. ATTACK_ID prevalidates the semantic ID against Tile membership on the pipe thread and then resolves the ID again from current Tile::things on the game thread before Game::attack.

The previous broad Monster heap scanner is no longer on the Target/ATTACK_ID execution path. Broad memory scanning remains only as an occasional Tile-address discovery step and is not executed on the game thread.


## Bridge V12 on-demand dispatcher isolation

A crash was reproduced in town with no active target/monster interaction, with the same 0x01037419 access-violation signature. Since the V11 Tile target path was idle at the time, the always-installed WH_GETMESSAGE hook became the principal remaining bridge-level suspect.

V12 removes the permanent hook. Startup only resolves the game window/thread. A WH_GETMESSAGE hook and completion event are created only for an ATTACK action and are removed immediately after the action (including timeout/error cleanup). Tile::things caching/Target semantics remain unchanged from V11.


## Bridge V13 corpse transition discovery

After V12 remained stable under extended cave combat, Loot discovery starts with a read-only Tile transition probe.

LOOT_MARK captures cached Tile::things Items plus nearby Monster membership. LOOT_DIFF reports new exact-Item objects and Monsters that disappeared, correlated by the same cached Tile address internally and exposed only through ephemeral tileSlot diagnostics.

No corpse is opened and no item is moved in V13. The goal is to identify the real corpse client item ID(s) produced by creature death before validating the Game::use/container-open ABI.


## Bridge V14 active loot ABI test

V13 correlated Rat/Cave Rat disappearance with new Item 2813 on the exact death Tiles (plus transient Item 2016). V14 promotes this only to an experimental one-shot use path; 2813 is not persisted as a universal corpse ID.

Game::use and Game::move use their V37 runtime-confirmed shared_object_ptr_ref contracts. The bridge passes addresses of the actual 4-byte Tile::things / Container::items holder elements rather than raw Item pointers. The V12 on-demand WH_GETMESSAGE lifetime remains unchanged.

GET_OPEN_CONTAINERS reads the exact Container RTTI graph rooted at the runtime-confirmed Game+0x10 inline registry. MOVE_CONTAINER_ITEM is prepared for gold 2148 -> an already-open Bag. Its special container Position encoding is explicitly experimental pending this live test.


## V15 loot snapshot correction

Live V14 evidence showed Container registry decoding is correct (container id/name/capacity/items and gold 2148 were read successfully). The active use call was not reached because the corpse candidate filter required the new Item to appear on the exact Tile occupied by the monster at LOOT_MARK. In the observed run the Rat moved before death, so corpse candidate 2813 appeared on a different Tile.

V15 stores the exact Tile cache address list at LOOT_MARK and rereads that frozen Tile set at LOOT_DIFF. It no longer rejects the diff merely because the global Target cache generation changed. When at least one tracked monster disappears, newly-added Items in the marked snapshot are retained as death-window candidates; USE_LOOT_ITEM still revalidates the requested Item ID and exact holder on the game thread before calling Game::use.


## V15 live move confirmation

Live validation completed successfully:
- source Container: id=1, name=`dead rat`
- destination Container: id=0, name=`bag`
- moved Item: gold id=2148, count=3
- before: Bag gold=27, dead rat gold=3
- after: Bag gold=30, dead rat no longer contained gold

This confirms the active `Game::move` path end-to-end for this build, including:
- Item `shared_object_ptr_ref` holder ABI;
- runtime-confirmed Position ABI;
- runtime-confirmed Count ABI;
- destination container encoding `x=0xFFFF, y=0x40|containerId, z=slot`.

The bridge now reports `destinationEncoding=otclient_container_position_runtime_confirmed` for this path.


## V16 container use / Auto Food foundation

The live V15 `Game::move` confirmation also validated the 4-byte `Container::items` holder representation. V16 reuses the already runtime-confirmed `Game::use(Item shared_object_ptr_ref)` ABI for items that are already inside open containers.

New bridge command:

`USE_CONTAINER_ITEM`

Arguments:
- `itemId` — required u16 item id;
- `sourceContainerId` — optional. If omitted, the bridge searches every currently open container and uses the first matching item.

The x64 pipe test exposes:
- `useitem <containerId|any> [itemId] [PID]`
- `usefood <containerId|any> [itemId=2696] [PID]`

For the first live validation, cheese id `2696` is the default food candidate. This path is intentionally independent from Loot: any matching food already present in an open bag/container may be used regardless of whether it came from a corpse, the player, or another container.

V16 keeps the existing safety rule: the selected Container, holder address, Item pointer, and item id are revalidated on the game thread immediately before `Game::use`. Continuous Auto Food scheduling is not enabled until this one-shot container-use path is confirmed live.


## V16 live container-use confirmation

Live validation succeeded with cheese id `2696` already inside the player's open Bag:

- `GET_OPEN_CONTAINERS` resolved Bag id=0 and cheese at slot=2;
- `USE_CONTAINER_ITEM` returned `result=success`;
- the live Ramonia client consumed the cheese;
- holder ABI remained `shared_object_ptr_ref`.

This confirms `Game::use` end-to-end for a Container::items holder, not only a Tile::things holder.

The x64 pipe client now also exposes a continuous policy loop:

`autofood [itemId=2696] [successDelayMs=60000] [retryDelayMs=2000] [PID]`

The loop lives outside the x86 bridge. It repeatedly asks the bridge to use the configured food in any open container. After a successful use it waits `successDelayMs`; while food is absent it polls using `retryDelayMs`. This preserves the architecture rule that the x64 Crome layer owns bot policy/scheduling while the x86 bridge remains a validated ABI adapter.


## V16 UI integration and multi-container live confirmation

Extended live Auto Food validation confirmed that `USE_CONTAINER_ITEM` searches across multiple open containers rather than only Bag id=0. Cheese id `2696` was consumed successfully from container ids 0, 1, 2 and 3 during one continuous `autofood` session. When no matching food was present, the loop waited; when a new cheese appeared later, it was detected and consumed without restarting the client.

The Crome UI now wires the Tools -> Food Eater control to the Ramonia V16 bridge. Food policy remains in the external x64 Crome layer, with configurable Food ID and interval, while the injected x86 bridge remains the validated ABI adapter. Other Tools remain disabled on Ramonia until their own backends are confirmed.


## V16 Crome UI live confirmation

Final live validation through the actual Crome UI succeeded. With Tools -> Food Eater enabled, Food ID 2696 and the Ramonia V16 bridge injected, Crome consumed food correctly without using CromePipeTest. This confirms the production path:

Crome x64 UI/policy -> named pipe -> Ramonia x86 bridge V16 -> validated Container::items holder -> Game::use.

Auto Food is therefore considered runtime-confirmed for this Ramonia build. CromePipeTest remains only as a diagnostic/manual validation tool for this feature.


## V17 one-cycle Auto Loot diagnostic

A one-cycle external Auto Loot diagnostic was added to CromePipeTest without changing the validated x86 bridge contracts.

Command:

`autoloot [corpseId=2813] [targetBagId=0] [lootItemId=2148] [waitMs=30000] [PID]`

The diagnostic performs this sequence:

1. reads currently open containers and verifies that the target bag is open;
2. calls `LOOT_MARK`;
3. polls `LOOT_DIFF` until a death-window corpse candidate with the requested corpse item id appears;
4. calls `USE_LOOT_ITEM` to open the corpse;
5. detects the newly opened corpse container;
6. calls `MOVE_CONTAINER_ITEM` to move the requested loot item into the target bag;
7. rereads the target bag and reports the resulting total.

Initial defaults are intentionally limited to the already observed Ramonia Rat/Cave Rat evidence:
- corpse item id: `2813`;
- target bag id: `0`;
- loot item id: gold `2148`.

This stage is diagnostic-only. Continuous Loot in the Crome UI remains disabled until the complete one-cycle chain is confirmed live.


## V17 live Auto Loot confirmation and Crome UI integration

The one-cycle Auto Loot diagnostic was live-confirmed twice against Rat corpses.

Confirmed sequence:
- corpse item id `2813` detected after Rat death;
- corpse opened through `USE_LOOT_ITEM`;
- newly opened container identified as `dead rat`;
- gold item id `2148` moved through `MOVE_CONTAINER_ITEM`;
- destination bag id `0` updated successfully.

Observed successful move counts included 3 gold and 1 gold in separate runs. The diagnostic also reported the destination Bag total after each move.

Two earlier diagnostic runs timed out before a corpse candidate was detected. To remove that manual timing weakness from normal use, Crome v0.8.3 arms `LOOT_MARK` only when an actual AttackTarget is present. It keeps one baseline per target, waits for that target ID to disappear, then runs `LOOT_DIFF`, opens the configured corpse and moves the configured loot items. After a Crome `ATTACK_ID`, Loot also arms its baseline immediately in the same worker cycle so very weak targets cannot die before the next 300 ms poll.

Ramonia Loot defaults:
- corpse item id: `2813`;
- destination container id: `0`;
- loot-only-list: enabled;
- initial loot item list: `2148` (gold).

The Ramonia Loot policy is external x64 Crome logic. The x86 bridge remains unchanged at V16 and continues to expose the already runtime-confirmed semantic actions.
