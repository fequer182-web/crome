# Current baseline — Ramonia x86

## Probe

Version: **V34**

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

## V33.1 runtime capture safety + V34 ABI classifier

- target page protection is changed before arming;
- VEH does not call `VirtualProtect`;
- original byte is restored immediately on breakpoint hit;
- EIP is reset to the restored first instruction;
- diagnostic memory snapshots use `ReadProcessMemory`;
- protection restoration happens outside the VEH;
- breakpoint is one-shot and is not re-armed automatically.

## Rule

The profile is the source of truth for already-confirmed offsets/RVAs.
V34 promotes only classified runtime evidence into the portable `actions` profile block; active calls remain disabled.
