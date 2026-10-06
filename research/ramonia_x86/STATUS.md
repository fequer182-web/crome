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
